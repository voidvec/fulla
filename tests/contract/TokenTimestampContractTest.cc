#include <drogon/drogon_test.h>

#include <fulla/storage/memory/MemoryTokenRepository.h>
#include <fulla/storage/postgres/PostgresTokenRepository.h>
#include <fulla/storage/redis/RedisCachedTokenRepository.h>
#include <fulla/storage/redis/RedisTokenRepository.h>

#include "ContractFixtures.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace
{
using AccessToken = ::fulla::oauth2::model::OAuth2AccessToken;
using RefreshToken = ::fulla::oauth2::model::OAuth2RefreshToken;
using Introspection = ::fulla::oauth2::model::TokenIntrospection;
using Repository = ::fulla::oauth2::repository::ITokenRepository;
using Postgres = ::fulla::storage::postgres::PostgresTokenRepository;
using Cached = ::fulla::storage::redis::RedisCachedTokenRepository;
using ::fulla::test::contract::nowSeconds;
using ::fulla::test::contract::uniqueSuffix;
using ::fulla::test::contract::waitForValue;
using ::fulla::test::contract::waitForVoid;

AccessToken accessToken(const std::string &client, int64_t issuedAt, int64_t notBefore)
{
    AccessToken token{};
    token.token = "timestamp-at-" + uniqueSuffix();
    token.clientId = client;
    token.userId = "timestamp-contract-user";
    token.scope = "openid";
    token.expiresAt = nowSeconds() + 7200;
    token.issuedAt = issuedAt;
    token.notBefore = notBefore;
    return token;
}

RefreshToken refreshToken(const AccessToken &at, const std::string &family)
{
    RefreshToken token{};
    token.token = "timestamp-rt-" + uniqueSuffix();
    token.accessToken = at.token;
    token.clientId = at.clientId;
    token.userId = at.userId;
    token.scope = at.scope;
    token.expiresAt = nowSeconds() + 86400;
    token.familyId = family;
    return token;
}

struct WrittenTimes
{
    bool written = false;
    int64_t before = 0;
    int64_t after = 0;
    std::optional<AccessToken> stored;
    std::optional<Introspection> introspected;
};

enum class WritePath
{
    Single,
    Pair,
    Rotation
};

WrittenTimes writeAndRead(
  const std::shared_ptr<Repository> &repo,
  const AccessToken &token,
  WritePath path
)
{
    WrittenTimes result;
    const auto family = "timestamp-family-" + uniqueSuffix();
    RefreshToken expected;
    if (path == WritePath::Rotation)
    {
        auto initial = accessToken(token.clientId, nowSeconds() - 60, nowSeconds() - 60);
        expected = refreshToken(initial, family);
        if (!waitForValue<bool>([&](auto cb) {
                repo->saveTokenPair(initial, expected, std::move(cb));
            }))
            return result;
    }
    const auto next = refreshToken(token, family);
    result.before = nowSeconds();
    if (path == WritePath::Single)
    {
        waitForVoid([&](auto cb) { repo->saveAccessToken(token, std::move(cb)); });
        result.written = true;
    }
    else if (path == WritePath::Pair)
    {
        result.written =
          waitForValue<bool>([&](auto cb) { repo->saveTokenPair(token, next, std::move(cb)); });
    }
    else
    {
        auto rotated = waitForValue<::fulla::oauth2::repository::RefreshTokenResult>([&](auto cb) {
            repo->rotateRefreshToken(expected, token, next, std::move(cb));
        });
        result.written = rotated.status == ::fulla::oauth2::repository::RefreshTokenStatus::Rotated;
    }
    result.after = nowSeconds();
    if (result.written)
    {
        result.stored = waitForValue<std::optional<AccessToken>>([&](auto cb) {
            repo->getAccessToken(token.token, std::move(cb));
        });
        result.introspected = waitForValue<std::optional<Introspection>>([&](auto cb) {
            repo->introspectToken(token.token, std::move(cb));
        });
    }
    return result;
}

std::shared_ptr<Postgres> postgres()
{
    if (!::fulla::test::contract::getPostgresClientOrNull())
        return nullptr;
    auto repo = std::make_shared<Postgres>();
    repo->initFromConfig(Json::Value());
    return repo;
}

void checkWritePath(
  std::shared_ptr<::drogon::test::Case> TEST_CTX,
  const std::shared_ptr<Repository> &repo,
  WritePath path
)
{
    // Explicit old timestamps make an omitted column fail deterministically,
    // regardless of the database's fractional wall clock at execution time.
    constexpr int64_t supplied = 1700000000;
    for (const bool useDefaults : {false, true})
    {
        auto token =
          accessToken("fulla-portal", useDefaults ? 0 : supplied, useDefaults ? 0 : supplied - 10);
        auto result = writeAndRead(repo, token, path);
        REQUIRE(result.written);
        REQUIRE(result.stored.has_value());
        REQUIRE(result.introspected.has_value());
        CHECK(result.introspected->active);
        if (useDefaults)
        {
            CHECK(result.stored->issuedAt >= result.before);
            CHECK(result.stored->issuedAt <= result.after);
            CHECK(result.stored->notBefore == result.stored->issuedAt);
        }
        else
        {
            CHECK(result.stored->issuedAt == supplied);
            CHECK(result.stored->notBefore == supplied - 10);
        }
        CHECK(result.introspected->iat == result.stored->issuedAt);
        CHECK(result.introspected->nbf == result.stored->notBefore);
    }
}

void checkNotBefore(
  std::shared_ptr<::drogon::test::Case> TEST_CTX,
  const std::shared_ptr<Repository> &repo,
  const std::string &client
)
{
    const auto now = nowSeconds();
    for (const int64_t notBefore : {now - 60, now, now + 3600})
    {
        auto token = accessToken(client, now - 60, notBefore);
        waitForVoid([&](auto cb) { repo->saveAccessToken(token, std::move(cb)); });
        auto result = waitForValue<std::optional<Introspection>>([&](auto cb) {
            repo->introspectToken(token.token, std::move(cb));
        });
        REQUIRE(result.has_value());
        CHECK(result->active == (notBefore <= now));
    }
}

bool evictTokenCache(const ::drogon::nosql::RedisClientPtr &redis, const std::string &token)
{
    return waitForValue<bool>([&](auto cb) {
        auto done = std::make_shared<decltype(cb)>(std::move(cb));
        redis->execCommandAsync(
          [done](const ::drogon::nosql::RedisResult &) { (*done)(true); },
          [done](const ::drogon::nosql::RedisException &) { (*done)(false); },
          "DEL %s %s",
          ("fulla:cache:token:access:" + token).c_str(),
          ("fulla:cache:token:introspect:" + token).c_str()
        );
    });
}

bool hasIntrospectionCache(const ::drogon::nosql::RedisClientPtr &redis, const std::string &token)
{
    return waitForValue<bool>([&](auto cb) {
        auto done = std::make_shared<decltype(cb)>(std::move(cb));
        redis->execCommandAsync(
          [done](const ::drogon::nosql::RedisResult &result) { (*done)(result.asInteger() == 1); },
          [done](const ::drogon::nosql::RedisException &) { (*done)(false); },
          "EXISTS %s",
          ("fulla:cache:token:introspect:" + token).c_str()
        );
    });
}
}  // namespace

DROGON_TEST(Integration_P0_Contract_Functional_TokenTimestamps_Postgres_SingleWriteRoundTrip)
{
    auto repo = postgres();
    if (!repo)
        return;
    checkWritePath(TEST_CTX, repo, WritePath::Single);
}

DROGON_TEST(Integration_P0_Contract_Functional_TokenTimestamps_Postgres_PairWriteRoundTrip)
{
    auto repo = postgres();
    if (!repo)
        return;
    checkWritePath(TEST_CTX, repo, WritePath::Pair);
}

DROGON_TEST(Integration_P0_Contract_Functional_TokenTimestamps_Postgres_RotationWriteRoundTrip)
{
    auto repo = postgres();
    if (!repo)
        return;
    checkWritePath(TEST_CTX, repo, WritePath::Rotation);
}

DROGON_TEST(Integration_P0_Contract_Functional_TokenTimestamps_Postgres_RejectsFutureNotBefore)
{
    auto repo = postgres();
    if (!repo)
        return;
    checkNotBefore(TEST_CTX, repo, "fulla-portal");
}

DROGON_TEST(Integration_P0_Contract_Functional_TokenTimestamps_Memory_RejectsFutureNotBefore)
{
    auto repo = std::make_shared<::fulla::storage::memory::MemoryTokenRepository>();
    checkNotBefore(TEST_CTX, repo, "timestamp-memory-client");
}

DROGON_TEST(Integration_P0_Contract_Functional_TokenTimestamps_Redis_RejectsFutureNotBefore)
{
    if (!::fulla::test::contract::getRedisClientOrNull())
        return;
    auto repo = std::make_shared<::fulla::storage::redis::RedisTokenRepository>("default");
    checkNotBefore(TEST_CTX, repo, "timestamp-redis-client");
}

DROGON_TEST(Integration_P0_Contract_Functional_TokenTimestamps_PostgresCache_ColdWarmAndSoftFail)
{
    auto base = postgres();
    auto redis = ::fulla::test::contract::getRedisClientOrNull();
    if (!base)
        return;
    if (!redis)
        return;
    auto cached = std::make_shared<Cached>(base, redis);
    auto fallback = std::make_shared<Cached>(base, nullptr);
    const auto now = nowSeconds();
    for (const bool future : {false, true})
    {
        auto token = accessToken("fulla-portal", now - 60, future ? now + 3600 : now - 60);
        waitForVoid([&](auto cb) { cached->saveAccessToken(token, std::move(cb)); });
        if (!future)
        {
            // saveAccessToken does not await its asynchronous cache fill.
            // Await the guarded access read before testing introspection fill.
            auto warm = waitForValue<std::optional<AccessToken>>([&](auto cb) {
                cached->getAccessToken(token.token, std::move(cb));
            });
            REQUIRE(warm.has_value());
        }
        for (int read = 0; read != 3; ++read)
        {
            auto result = waitForValue<std::optional<Introspection>>([&](auto cb) {
                cached->introspectToken(token.token, std::move(cb));
            });
            REQUIRE(result.has_value());
            CHECK(result->active == !future);
            if (!future)
            {
                // guardedFill completes before the callback, so subsequent
                // reads have a real cache entry without a timing sleep.
                REQUIRE(hasIntrospectionCache(redis, token.token));
                CHECK(result->iat == token.issuedAt);
                CHECK(result->nbf == token.notBefore);
            }
        }
        REQUIRE(evictTokenCache(redis, token.token));
        auto cold = waitForValue<std::optional<AccessToken>>([&](auto cb) {
            cached->getAccessToken(token.token, std::move(cb));
        });
        CHECK(cold.has_value() == !future);
        if (cold)
        {
            CHECK(cold->issuedAt == token.issuedAt);
            CHECK(cold->notBefore == token.notBefore);
        }
        auto softFail = waitForValue<std::optional<Introspection>>([&](auto cb) {
            fallback->introspectToken(token.token, std::move(cb));
        });
        REQUIRE(softFail.has_value());
        CHECK(softFail->active == !future);
    }
}
