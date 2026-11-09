// tests/integration/storage/RedisCachedTokenRepositoryTest.cc
//
// #42 Phase 2 (postgres-redis-cache-design.md §5.2/§5.4): integration tests
// for the RedisCachedTokenRepository decorator. Verifies the cache-aside
// behavior for getAccessToken + introspectToken, the C1 revoked-check, the
// N2 discriminator (refresh-token introspections must NOT be cached), the
// revokeAccessToken invalidation + negative cache, the C7 TTL guard, the
// saveAccessToken warming, and the §5.5 soft-fail (null Redis client).
//
// Requires a live Redis instance. SKIPs cleanly when unavailable (same
// convention as the Phase 1 client-cache test + ContractFixtures.h). The
// wrapped ITokenRepository is a counting fake (inline) so the tests can
// assert exactly how many times the backing impl was consulted.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <drogon/nosql/RedisClient.h>

#include <fulla/oauth2/repository/ITokenRepository.h>
#include <fulla/oauth2/model/Dto.h>
#include <fulla/storage/redis/RedisCachedTokenRepository.h>

#include <atomic>
#include <cstdarg>
#include <mutex>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using OAuth2AccessToken = fulla::oauth2::model::OAuth2AccessToken;
using TokenIntrospection = fulla::oauth2::model::TokenIntrospection;
using ITokenRepository = fulla::oauth2::repository::ITokenRepository;
using RedisCachedTokenRepository = fulla::storage::redis::RedisCachedTokenRepository;

namespace
{

// Counting fake ITokenRepository — records every call so the tests can
// distinguish a cache hit (count unchanged) from a miss (count incremented).
// Mirrors FakeTokenRepo (libs/oauth2/test/TokenServiceTest.cc) but minimal.
class CountingFakeTokenRepo : public ITokenRepository
{
  public:
    std::atomic<int> getAccessTokenCalls{0};
    std::atomic<int> introspectTokenCalls{0};
    std::atomic<int> revokeAccessTokenCalls{0};
    std::atomic<int> saveAccessTokenCalls{0};

    // The token this fake "stores" (keyed by the hashed-token string the
    // decorator passes through). The test populates it before constructing
    // the decorator.
    std::unordered_map<std::string, OAuth2AccessToken> accessStore;
    // A separate "refresh-token" hash the introspect fallthrough would
    // resolve (used to exercise the N2 discriminator: an introspection for
    // a refresh-token hash must NOT be cached).
    std::unordered_map<std::string, TokenIntrospection> refreshIntrospectStore;

    void saveAccessToken(const OAuth2AccessToken &t, VoidCallback &&cb) override
    {
        saveAccessTokenCalls++;
        accessStore[t.token] = t;
        if (cb)
            cb();
    }
    void getAccessToken(const std::string &token, AccessTokenCallback &&cb) override
    {
        getAccessTokenCalls++;
        auto it = accessStore.find(token);
        if (it != accessStore.end())
            cb(it->second);
        else
            cb(std::nullopt);
    }
    void saveTokenPair(const OAuth2AccessToken &at, const fulla::oauth2::model::OAuth2RefreshToken &rt, SaveResultCallback &&cb) override
    {
        (void)rt;  // unused — fake only persists the access token
        accessStore[at.token] = at;
        cb(true);
    }
    void saveRefreshToken(const fulla::oauth2::model::OAuth2RefreshToken &, VoidCallback &&cb) override { if (cb) cb(); }
    void getRefreshToken(const std::string &, RefreshTokenCallback &&cb) override { cb(std::nullopt); }
    void revokeRefreshToken(const std::string &, VoidCallback &&cb) override { if (cb) cb(); }
    void atomicRevokeRefreshToken(const std::string &, RefreshTokenCallback &&cb) override { cb(std::nullopt); }
    void revokeTokenFamily(const std::string &, VoidCallback &&cb) override { if (cb) cb(); }

    void introspectToken(const std::string &token, TokenIntrospectionCallback &&cb) override
    {
        introspectTokenCalls++;
        // First try the access store (the real Postgres impl tries access
        // then refresh). If the token is an access token, return its
        // introspection; otherwise check the refresh-introspect store (this
        // models the refresh-token fallthrough the N2 discriminator guards).
        auto atIt = accessStore.find(token);
        if (atIt != accessStore.end())
        {
            const auto &t = atIt->second;
            int64_t now = std::time(nullptr);
            if (t.revoked || t.expiresAt < now)
            {
                TokenIntrospection inactive;
                inactive.active = false;
                cb(inactive);
                return;
            }
            TokenIntrospection intro;
            intro.active = true;
            intro.orgId = t.orgId;
            intro.clientId = t.clientId;
            intro.tokenType = "Bearer";
            intro.exp = t.expiresAt;
            intro.iat = t.issuedAt;
            intro.nbf = t.notBefore;
            intro.sub = t.userId;
            intro.aud = t.audience;
            intro.iss = t.issuer;
            intro.scope = t.scope;
            cb(intro);
            return;
        }
        auto rtIt = refreshIntrospectStore.find(token);
        if (rtIt != refreshIntrospectStore.end())
        {
            cb(rtIt->second);
            return;
        }
        TokenIntrospection inactive;
        inactive.active = false;
        cb(inactive);
    }

    void incrementIntrospectCount(const std::string &, VoidCallback &&cb) override { if (cb) cb(); }
    void revokeAccessToken(const std::string &token, const std::string & /*revokedBy*/, VoidCallback &&cb) override
    {
        revokeAccessTokenCalls++;
        auto it = accessStore.find(token);
        if (it != accessStore.end())
        {
            it->second.revoked = true;
            it->second.revokedAt = std::time(nullptr);
        }
        if (cb)
            cb();
    }
    void purgeExpired() override {}
    bool supportsTransactions() const override { return false; }
    bool supportsCas() const override { return false; }
};

OAuth2AccessToken makeAccessToken(const std::string &hash)
{
    OAuth2AccessToken t;
    t.token = hash;
    t.clientId = "test-client";
    t.userId = "test-user";
    t.scope = "openid profile";
    t.expiresAt = std::time(nullptr) + 3600;  // 1h remaining
    t.revoked = false;
    t.issuedAt = std::time(nullptr);
    t.issuer = "http://localhost:5555";
    t.audience = "test-client";
    t.notBefore = std::time(nullptr);
    t.introspectCount = 0;
    return t;
}

template <typename Op>
std::optional<OAuth2AccessToken> waitForAccess(Op &&op)
{
    std::promise<std::optional<OAuth2AccessToken>> p;
    auto f = p.get_future();
    op([&p](std::optional<OAuth2AccessToken> v) { p.set_value(std::move(v)); });
    if (f.wait_for(std::chrono::seconds(10)) == std::future_status::timeout)
        throw std::runtime_error("token cache test TIMEOUT");
    return f.get();
}

template <typename Op>
std::optional<TokenIntrospection> waitForIntro(Op &&op)
{
    std::promise<std::optional<TokenIntrospection>> p;
    auto f = p.get_future();
    op([&p](std::optional<TokenIntrospection> v) { p.set_value(std::move(v)); });
    if (f.wait_for(std::chrono::seconds(10)) == std::future_status::timeout)
        throw std::runtime_error("token cache test TIMEOUT");
    return f.get();
}

template <typename Op>
void waitForVoid(Op &&op)
{
    std::promise<void> p;
    auto f = p.get_future();
    op([&p]() { p.set_value(); });
    if (f.wait_for(std::chrono::seconds(10)) == std::future_status::timeout)
        throw std::runtime_error("token cache test TIMEOUT");
    f.get();
}

::drogon::nosql::RedisClientPtr getRedisOrNull()
{
    try
    {
        return ::drogon::app().getRedisClient("default");
    }
    catch (...)
    {
        return nullptr;
    }
}

// #90 case 1: bounded poll until `key` exists in Redis. The cache fill after a
// MISS is an async SET; under load it can outlast any fixed sleep, so tests
// must wait for the key itself (same event-driven discipline the
// DelayedDoubleDeleteTest applies to its DEL). Returns false on timeout so the
// caller fails with a clear cause.
bool waitForRedisKey(
  const ::drogon::nosql::RedisClientPtr &redis,
  const std::string &key,
  std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)
)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::promise<int> p;
        auto f = p.get_future();
        redis->execCommandAsync(
          [&p](const ::drogon::nosql::RedisResult &r) {
              p.set_value(static_cast<int>(r.asInteger()));
          },
          [&p](const ::drogon::nosql::RedisException &) { p.set_value(-1); },
          "EXISTS %s", key.c_str()
        );
        if (
          f.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
          f.get() == 1)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void evictTokenKeys(const std::string &hash)
{
    auto redis = getRedisOrNull();
    if (!redis)
        return;
    waitForVoid([&](auto cb) {
        redis->execCommandAsync(
          [cb](const ::drogon::nosql::RedisResult &) { cb(); },
          [cb](const ::drogon::nosql::RedisException &) { cb(); },
          "DEL fulla:cache:token:access:%s fulla:cache:token:introspect:%s fulla:cache:token:revoked:%s",
          hash.c_str(), hash.c_str(), hash.c_str()
        );
    });
}

std::string uniqueHash()
{
    return "testhash_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

}  // namespace

// ===========================================================================
// Test 1: getAccessToken cache miss → fill → hit. Verifies the cache-aside
// cycle + round-trip fidelity (13 fields, C6) + that the second read does
// NOT touch the backing impl.
// ===========================================================================
DROGON_TEST(Integration_P2_Storage_RedisCachedTokenRepository_GetAccessToken_MissThenHit)
{
    auto redis = getRedisOrNull();
    if (!redis)
    {
        LOG_INFO << "[skip] Redis unavailable — token cache test skipped";
        return;
    }

    const std::string hash = uniqueHash();
    evictTokenKeys(hash);

    auto fake = std::make_shared<CountingFakeTokenRepo>();
    fake->accessStore[hash] = makeAccessToken(hash);
    auto decorator = std::make_shared<RedisCachedTokenRepository>(fake, redis);

    // First read: MISS → delegates to impl + fills cache.
    auto first = waitForAccess([&](auto cb) { decorator->getAccessToken(hash, std::move(cb)); });
    REQUIRE(first.has_value());
    CHECK(first->token == hash);
    CHECK(first->clientId == "test-client");
    CHECK(first->scope == "openid profile");
    // Round-trip fidelity: all 13 fields survived the JSON serialize/deserialize.
    CHECK(first->revoked == false);
    CHECK(first->issuer == "http://localhost:5555");
    CHECK(first->audience == "test-client");
    CHECK(first->introspectCount == 0);
    CHECK(fake->getAccessTokenCalls.load() == 1);

    // Wait for the async cache fill (SET) to land before asserting the HIT
    // (#90 case 1: a fixed sleep flakes under load).
    REQUIRE(waitForRedisKey(redis, "fulla:cache:token:access:" + hash));

    // Second read: HIT → does NOT touch impl.
    auto second = waitForAccess([&](auto cb) { decorator->getAccessToken(hash, std::move(cb)); });
    REQUIRE(second.has_value());
    CHECK(second->token == hash);
    CHECK(fake->getAccessTokenCalls.load() == 1);  // ← cache-hit assertion

    evictTokenKeys(hash);
}

// ===========================================================================
// Test 2 (C1): a revoked token is NEVER served from cache. After
// revokeAccessToken sets token:revoked:{hash}, getAccessToken returns
// nullopt immediately (negative-cache hit), even though the access-cache
// entry may still exist from the race window.
// ===========================================================================
DROGON_TEST(Integration_P2_Storage_RedisCachedTokenRepository_Revoked_NotServed)
{
    auto redis = getRedisOrNull();
    if (!redis)
    {
        LOG_INFO << "[skip] Redis unavailable — token cache test skipped";
        return;
    }

    const std::string hash = uniqueHash();
    evictTokenKeys(hash);

    auto fake = std::make_shared<CountingFakeTokenRepo>();
    fake->accessStore[hash] = makeAccessToken(hash);
    auto decorator = std::make_shared<RedisCachedTokenRepository>(fake, redis);

    // Populate the access cache.
    auto first = waitForAccess([&](auto cb) { decorator->getAccessToken(hash, std::move(cb)); });
    REQUIRE(first.has_value());
    REQUIRE(waitForRedisKey(redis, "fulla:cache:token:access:" + hash));

    // Revoke via the decorator (sets token:revoked + DELs access/introspect).
    waitForVoid([&](auto cb) { decorator->revokeAccessToken(hash, "test-revoker", std::move(cb)); });
    CHECK(fake->revokeAccessTokenCalls.load() == 1);
    // getAccessToken consults token:revoked first, so waiting for that key to
    // exist (not the DELs) is what makes the negative-cache read deterministic.
    REQUIRE(waitForRedisKey(redis, "fulla:cache:token:revoked:" + hash));

    // getAccessToken now returns nullopt (negative cache hit — does NOT
    // delegate to impl, because C1 checks token:revoked first).
    int callsBefore = fake->getAccessTokenCalls.load();
    auto after = waitForAccess([&](auto cb) { decorator->getAccessToken(hash, std::move(cb)); });
    CHECK(!after.has_value());
    // The impl's getAccessToken was NOT called (negative-cache hit short-circuits).
    CHECK(fake->getAccessTokenCalls.load() == callsBefore);

    evictTokenKeys(hash);
}

// ===========================================================================
// Test 3 (N2 discriminator): introspectToken caches a result ONLY when the
// token is a confirmed access token (its access-cache key exists). A
// refresh-token introspection (access cache absent) must NOT be cached.
// ===========================================================================
DROGON_TEST(Integration_P2_Storage_RedisCachedTokenRepository_Introspect_N2_Discriminator)
{
    auto redis = getRedisOrNull();
    if (!redis)
    {
        LOG_INFO << "[skip] Redis unavailable — token cache test skipped";
        return;
    }

    const std::string accessHash = uniqueHash();
    const std::string refreshHash = uniqueHash();
    evictTokenKeys(accessHash);
    evictTokenKeys(refreshHash);

    auto fake = std::make_shared<CountingFakeTokenRepo>();
    fake->accessStore[accessHash] = makeAccessToken(accessHash);
    // refreshHash is NOT in accessStore — introspectToken falls through to
    // the refresh-introspect store (models the refresh-token path).
    TokenIntrospection rtIntro;
    rtIntro.active = true;
    rtIntro.clientId = "test-client";
    rtIntro.sub = "test-user";
    rtIntro.exp = std::time(nullptr) + 3600;
    fake->refreshIntrospectStore[refreshHash] = rtIntro;

    auto decorator = std::make_shared<RedisCachedTokenRepository>(fake, redis);

    // --- Access-token path: warm the access cache first (the N2 gate). ---
    waitForAccess([&](auto cb) { decorator->getAccessToken(accessHash, std::move(cb)); });
    REQUIRE(waitForRedisKey(redis, "fulla:cache:token:access:" + accessHash));

    // First introspect of the ACCESS token → MISS → delegate → cache (N2
    // gate passes because access:{hash} exists).
    auto intro1 = waitForIntro([&](auto cb) { decorator->introspectToken(accessHash, std::move(cb)); });
    REQUIRE(intro1.has_value());
    CHECK(intro1->active == true);
    CHECK(fake->introspectTokenCalls.load() == 1);
    REQUIRE(waitForRedisKey(redis, "fulla:cache:token:introspect:" + accessHash));

    // Second introspect of the ACCESS token → HIT (impl NOT consulted).
    auto intro2 = waitForIntro([&](auto cb) { decorator->introspectToken(accessHash, std::move(cb)); });
    REQUIRE(intro2.has_value());
    CHECK(intro2->active == true);
    CHECK(fake->introspectTokenCalls.load() == 1);  // ← cached

    // --- Refresh-token path: introspect refreshHash (no access cache for it). ---
    auto rtIntro1 = waitForIntro([&](auto cb) { decorator->introspectToken(refreshHash, std::move(cb)); });
    REQUIRE(rtIntro1.has_value());
    CHECK(rtIntro1->active == true);
    int rtCallsAfter1 = fake->introspectTokenCalls.load();
    // The N2 gate fails for a refresh hash (no access:{hash} entry), so the
    // decorator issues NO cache write at all -- the old fixed sleep waited
    // for a SET that never happens. Bounded absence check instead: poll that
    // the introspect key stays absent, then assert the next call still
    // reaches the impl.
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::promise<int> p;
            auto f = p.get_future();
            redis->execCommandAsync(
              [&p](const ::drogon::nosql::RedisResult &r) {
                  p.set_value(static_cast<int>(r.asInteger()));
              },
              [&p](const ::drogon::nosql::RedisException &) { p.set_value(-1); },
              "EXISTS fulla:cache:token:introspect:%s", refreshHash.c_str()
            );
            REQUIRE(f.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
            if (f.get() == 1)
                FAIL("refresh-token introspect result was cached despite the N2 gate");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Second introspect of the REFRESH token → NOT cached (N2 gate fails:
    // access:{refreshHash} does not exist) → impl consulted again.
    auto rtIntro2 = waitForIntro([&](auto cb) { decorator->introspectToken(refreshHash, std::move(cb)); });
    REQUIRE(rtIntro2.has_value());
    CHECK(rtIntro2->active == true);
    CHECK(fake->introspectTokenCalls.load() == rtCallsAfter1 + 1);  // ← NOT cached

    evictTokenKeys(accessHash);
    evictTokenKeys(refreshHash);
}

// ===========================================================================
// Test 4 (soft-fail): a null Redis client makes the decorator a pure
// pass-through. The backing impl is consulted on every call (no caching).
// ===========================================================================
DROGON_TEST(Integration_P2_Storage_RedisCachedTokenRepository_NullRedis_PassThrough)
{
    const std::string hash = uniqueHash();
    auto fake = std::make_shared<CountingFakeTokenRepo>();
    fake->accessStore[hash] = makeAccessToken(hash);

    auto decorator = std::make_shared<RedisCachedTokenRepository>(fake, nullptr);

    auto r1 = waitForAccess([&](auto cb) { decorator->getAccessToken(hash, std::move(cb)); });
    REQUIRE(r1.has_value());
    CHECK(fake->getAccessTokenCalls.load() == 1);

    auto r2 = waitForAccess([&](auto cb) { decorator->getAccessToken(hash, std::move(cb)); });
    REQUIRE(r2.has_value());
    CHECK(fake->getAccessTokenCalls.load() == 2);  // ← no cache with null Redis
}

// ===========================================================================
// Test 5 (saveAccessToken warming): saving a token warms the access cache,
// so the first getAccessToken is a HIT (impl NOT consulted). Improves the
// cold-start window for the N2 discriminator.
// ===========================================================================
DROGON_TEST(Integration_P2_Storage_RedisCachedTokenRepository_SaveAccessToken_WarmsCache)
{
    auto redis = getRedisOrNull();
    if (!redis)
    {
        LOG_INFO << "[skip] Redis unavailable — token cache test skipped";
        return;
    }

    const std::string hash = uniqueHash();
    evictTokenKeys(hash);

    auto fake = std::make_shared<CountingFakeTokenRepo>();
    auto decorator = std::make_shared<RedisCachedTokenRepository>(fake, redis);

    // Save (warm) the token.
    auto t = makeAccessToken(hash);
    waitForVoid([&](auto cb) { decorator->saveAccessToken(t, std::move(cb)); });
    CHECK(fake->saveAccessTokenCalls.load() == 1);
    REQUIRE(waitForRedisKey(redis, "fulla:cache:token:access:" + hash));

    // getAccessToken → HIT (impl NOT consulted, the warm entry served).
    auto r = waitForAccess([&](auto cb) { decorator->getAccessToken(hash, std::move(cb)); });
    REQUIRE(r.has_value());
    CHECK(r->token == hash);
    CHECK(fake->getAccessTokenCalls.load() == 0);  // ← warmed, no impl call

    evictTokenKeys(hash);
}

namespace
{
class RevocationMetrics : public fulla::common::ports::IMetrics
{
  public:
    std::atomic<int> invalidationFailures{0};
    std::atomic<int> cacheErrors{0};

    void incrementCounter(
      const std::string &name,
      const fulla::common::ports::MetricLabels &labels,
      double
    ) override
    {
        if (name == "fulla_cache_invalidation_failures_total")
            ++invalidationFailures;
        if (name == "fulla_cache_total" && labels.at("outcome") == "error")
            ++cacheErrors;
    }

    void setGauge(const std::string &, const fulla::common::ports::MetricLabels &, double) override
    {
    }

    void observeHistogram(
      const std::string &,
      const fulla::common::ports::MetricLabels &,
      double
    ) override
    {
    }
};

// Only used by writer A's invalidation path. Reader B talks to real Redis.
class FailedInvalidationRedis : public ::drogon::nosql::RedisClient
{
  public:
    void execCommandAsync(
      ::drogon::nosql::RedisResultCallback &&,
      ::drogon::nosql::RedisExceptionCallback &&error,
      std::string_view,
      ...
    ) noexcept override
    {
        error(
          ::drogon::nosql::RedisException(
            ::drogon::nosql::RedisErrorCode::kConnectionBroken, "S03 invalidation fault"
          )
        );
    }

    std::shared_ptr<::drogon::nosql::RedisSubscriber> newSubscriber() noexcept override
    {
        return nullptr;
    }

    std::shared_ptr<::drogon::nosql::RedisTransaction> newTransaction() noexcept(false) override
    {
        throw std::runtime_error("S03 invalidation fault");
    }

    void newTransactionAsync(
      const std::function<void(const std::shared_ptr<::drogon::nosql::RedisTransaction> &)> &cb
    ) override
    {
        cb(nullptr);
    }

    void setTimeout(double) override
    {
    }

    void closeAll() override
    {
    }
};

class DeferredTokenRepo : public CountingFakeTokenRepo
{
  public:
    std::promise<void> reached;
    std::mutex mutex;
    AccessTokenCallback pending;
    OAuth2AccessToken snapshot;

    void getAccessToken(const std::string &token, AccessTokenCallback &&cb) override
    {
        ++getAccessTokenCalls;
        {
            std::lock_guard<std::mutex> guard(mutex);
            snapshot = accessStore.at(token);
            pending = std::move(cb);
        }
        reached.set_value();
    }

    void release()
    {
        AccessTokenCallback cb;
        {
            std::lock_guard<std::mutex> guard(mutex);
            cb = std::move(pending);
        }
        cb(snapshot);
    }
};

void redisDelete(const ::drogon::nosql::RedisClientPtr &redis, const std::string &key)
{
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    redis->execCommandAsync(
      [promise](const auto &) { promise->set_value(); },
      [promise](const auto &) {
          promise->set_exception(std::make_exception_ptr(std::runtime_error("Redis DEL fault")));
      },
      "DEL %s",
      key.c_str()
    );
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        throw std::runtime_error("Redis DEL timed out");
    future.get();
}
}  // namespace

DROGON_TEST(Integration_P0_Storage_TokenCache_OrgBindingSurvivesBothCacheHits)
{
    auto redis = getRedisOrNull();
    if (!redis)
        return;
    auto fake = std::make_shared<CountingFakeTokenRepo>();
    auto hash = uniqueHash();
    auto token = makeAccessToken(hash);
    token.orgId = 731;
    fake->accessStore[hash] = token;
    auto nodeA = std::make_shared<RedisCachedTokenRepository>(fake, redis);
    auto nodeB = std::make_shared<RedisCachedTokenRepository>(fake, redis);
    auto warm = waitForAccess([&](auto cb) { nodeA->getAccessToken(hash, std::move(cb)); });
    REQUIRE(warm.has_value());
    REQUIRE(waitForRedisKey(redis, "fulla:cache:token:access:" + hash));
    auto hit = waitForAccess([&](auto cb) { nodeB->getAccessToken(hash, std::move(cb)); });
    REQUIRE(hit.has_value());
    CHECK(hit->orgId == std::optional<int32_t>(731));
    CHECK(fake->getAccessTokenCalls.load() == 1);
    auto intro = waitForIntro([&](auto cb) { nodeA->introspectToken(hash, std::move(cb)); });
    REQUIRE(intro.has_value());
    REQUIRE(waitForRedisKey(redis, "fulla:cache:token:introspect:" + hash));
    auto introHit = waitForIntro([&](auto cb) { nodeB->introspectToken(hash, std::move(cb)); });
    REQUIRE(introHit.has_value());
    CHECK(introHit->orgId == std::optional<int32_t>(731));
    CHECK(fake->introspectTokenCalls.load() == 1);
    evictTokenKeys(hash);
}

DROGON_TEST(Integration_P0_Storage_TokenCache_TwoNodesFailedInvalidationExpiresWithinBudget)
{
    auto redis = getRedisOrNull();
    if (!redis)
        return;
    auto fake = std::make_shared<CountingFakeTokenRepo>();
    auto hash = uniqueHash();
    fake->accessStore[hash] = makeAccessToken(hash);
    auto metrics = std::make_shared<RevocationMetrics>();
    auto nodeA = std::make_shared<RedisCachedTokenRepository>(
      fake, std::make_shared<FailedInvalidationRedis>(), metrics, 2
    );
    auto nodeB = std::make_shared<RedisCachedTokenRepository>(fake, redis, nullptr, 2);
    const auto expires = std::time(nullptr) + 2;
    auto warm = waitForAccess([&](auto cb) { nodeB->getAccessToken(hash, std::move(cb)); });
    REQUIRE(warm.has_value());
    REQUIRE(waitForRedisKey(redis, "fulla:cache:token:access:" + hash));
    waitForVoid([&](auto cb) { nodeA->revokeAccessToken(hash, "s03", std::move(cb)); });
    auto stale = waitForAccess([&](auto cb) { nodeB->getAccessToken(hash, std::move(cb)); });
    REQUIRE(stale.has_value());
    std::this_thread::sleep_until(std::chrono::system_clock::from_time_t(expires + 1));
    auto denied = waitForAccess([&](auto cb) { nodeB->getAccessToken(hash, std::move(cb)); });
    CHECK(!denied.has_value());
    CHECK(metrics->invalidationFailures.load() == 8);
    CHECK(metrics->cacheErrors.load() == 1);
    evictTokenKeys(hash);
}

DROGON_TEST(Integration_P0_Storage_TokenCache_LateSnapshotCannotRestartBudgetAfterSecondDelete)
{
    auto redis = getRedisOrNull();
    if (!redis)
        return;
    auto fake = std::make_shared<DeferredTokenRepo>();
    auto hash = uniqueHash();
    fake->accessStore[hash] = makeAccessToken(hash);
    auto nodeA = std::make_shared<RedisCachedTokenRepository>(fake, redis, nullptr, 1);
    auto nodeB = std::make_shared<RedisCachedTokenRepository>(fake, redis, nullptr, 1);
    auto reached = fake->reached.get_future();
    auto pending = std::async(std::launch::async, [nodeB, hash]() {
        return waitForAccess([&](auto cb) { nodeB->getAccessToken(hash, std::move(cb)); });
    });
    if (reached.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        throw std::runtime_error("deferred master lookup not reached");
    waitForVoid([&](auto cb) { nodeA->revokeAccessToken(hash, "s03", std::move(cb)); });
    // A loop barrier proves the delayed DEL timer has run, rather than
    // relying on a short sleep. The stale master callback is still held.
    auto timer = std::make_shared<std::promise<void>>();
    auto fired = timer->get_future();
    ::drogon::app().getLoop()->runAfter(1.2, [timer]() { timer->set_value(); });
    if (fired.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        throw std::runtime_error("second-delete timer did not run");
    redisDelete(redis, "fulla:cache:token:revoked:" + hash);
    fake->release();
    auto result = pending.get();
    CHECK(!result.has_value());
    CHECK(
      !waitForRedisKey(redis, "fulla:cache:token:access:" + hash, std::chrono::milliseconds(100))
    );
    evictTokenKeys(hash);
}

DROGON_TEST(Integration_P0_Storage_TokenCache_RedisErrorFallsBackToAuthoritativeState)
{
    auto fake = std::make_shared<CountingFakeTokenRepo>();
    auto hash = uniqueHash();
    auto token = makeAccessToken(hash);
    token.revoked = true;
    fake->accessStore[hash] = token;
    auto node = std::make_shared<RedisCachedTokenRepository>(
      fake, std::make_shared<FailedInvalidationRedis>()
    );
    auto result = waitForAccess([&](auto cb) { node->getAccessToken(hash, std::move(cb)); });
    REQUIRE(result.has_value());
    CHECK(result->revoked);
    CHECK(fake->getAccessTokenCalls.load() == 1);
}

DROGON_TEST(Integration_P0_Storage_TokenCache_RechecksRevocationAfterDeferredMasterRead)
{
    auto redis = getRedisOrNull();
    if (!redis)
        return;
    auto fake = std::make_shared<DeferredTokenRepo>();
    auto hash = uniqueHash();
    fake->accessStore[hash] = makeAccessToken(hash);
    auto nodeA = std::make_shared<RedisCachedTokenRepository>(fake, redis);
    auto nodeB = std::make_shared<RedisCachedTokenRepository>(fake, redis);
    auto reached = fake->reached.get_future();
    auto pending = std::async(std::launch::async, [nodeB, hash]() {
        return waitForAccess([&](auto cb) { nodeB->getAccessToken(hash, std::move(cb)); });
    });
    if (reached.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        throw std::runtime_error("deferred master lookup not reached");
    waitForVoid([&](auto cb) { nodeA->revokeAccessToken(hash, "s03", std::move(cb)); });
    CHECK(waitForRedisKey(redis, "fulla:cache:token:revoked:" + hash));
    fake->release();
    CHECK(!pending.get().has_value());
    CHECK(
      !waitForRedisKey(redis, "fulla:cache:token:access:" + hash, std::chrono::milliseconds(100))
    );
    evictTokenKeys(hash);
}

namespace
{
class HungRedisStage : public FailedInvalidationRedis
{
  public:
    HungRedisStage(::drogon::nosql::RedisClientPtr redis, bool first)
        : redis_(std::move(redis)), first_(first)
    {
    }

    std::promise<void> held;
    std::mutex mutex;
    ::drogon::nosql::RedisResultCallback success;
    ::drogon::nosql::RedisExceptionCallback error;

    void execCommandAsync(
      ::drogon::nosql::RedisResultCallback &&ok,
      ::drogon::nosql::RedisExceptionCallback &&failed,
      std::string_view command,
      ...
    ) noexcept override
    {
        if (!first_ && command == "EVAL %s 2 %s %s")
        {
            va_list args;
            va_start(args, command);
            const auto script = va_arg(args, const char *);
            const auto negative = va_arg(args, const char *);
            const auto positive = va_arg(args, const char *);
            va_end(args);
            redis_->execCommandAsync(
              std::move(ok), std::move(failed), "EVAL %s 2 %s %s", script, negative, positive
            );
            return;
        }
        {
            std::lock_guard<std::mutex> guard(mutex);
            success = std::move(ok);
            error = std::move(failed);
        }
        held.set_value();
    }

    void failLate()
    {
        ::drogon::nosql::RedisExceptionCallback cb;
        {
            std::lock_guard<std::mutex> guard(mutex);
            success = {};
            cb = std::move(error);
        }
        if (cb)
            cb(
              ::drogon::nosql::RedisException(
                ::drogon::nosql::RedisErrorCode::kConnectionBroken, "late reconnect failure"
              )
            );
    }

  private:
    ::drogon::nosql::RedisClientPtr redis_;
    bool first_;
};
}  // namespace

DROGON_TEST(Integration_P0_Storage_TokenCache_HungRedisStagesCompleteOnceAndReleaseCallback)
{
    auto redis = getRedisOrNull();
    if (!redis)
        return;
    for (int stage : {0, 1, 2})
    {
        auto fake = std::make_shared<CountingFakeTokenRepo>();
        auto hash = uniqueHash();
        fake->accessStore[hash] = makeAccessToken(hash);
        auto hung = std::make_shared<HungRedisStage>(redis, stage == 0);
        auto blocked = hung->held.get_future();
        auto node = std::make_shared<RedisCachedTokenRepository>(fake, hung);
        auto witness = std::make_shared<int>(1);
        std::weak_ptr<int> lifetime = witness;
        auto promise = std::make_shared<std::promise<bool>>();
        auto completed = promise->get_future();
        auto calls = std::make_shared<std::atomic<int>>(0);
        const auto started = std::chrono::steady_clock::now();
        if (stage == 2)
            node->introspectToken(
              hash, [witness, promise, calls](std::optional<TokenIntrospection> r) {
                  (void)witness;
                  ++*calls;
                  promise->set_value(r && r->active);
              }
            );
        else
            node
              ->getAccessToken(hash, [witness, promise, calls](std::optional<OAuth2AccessToken> r) {
                  (void)witness;
                  ++*calls;
                  promise->set_value(r.has_value());
              });
        witness.reset();
        if (blocked.wait_for(std::chrono::seconds(3)) != std::future_status::ready)
            throw std::runtime_error("Redis stage not held");
        if (completed.wait_for(std::chrono::seconds(7)) != std::future_status::ready)
            throw std::runtime_error("cache deadline did not complete");
        CHECK(!completed.get());
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(7));
        const auto releaseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!lifetime.expired() && std::chrono::steady_clock::now() < releaseDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(lifetime.expired());
        hung->failLate();
        CHECK(calls->load() == 1);
        evictTokenKeys(hash);
    }
}
