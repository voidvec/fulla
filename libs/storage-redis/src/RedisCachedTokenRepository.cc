#include <fulla/storage/redis/RedisCachedTokenRepository.h>
#include <fulla/storage/redis/DelayedDoubleDelete.h>

#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <sstream>
#include <string_view>

namespace fulla::storage::redis
{

// DTO + callback aliases (safe at namespace scope: this .cc does not include
// IOAuth2Storage.h, so no oauth2::* clash).
using OAuth2AccessToken = ::fulla::oauth2::model::OAuth2AccessToken;
using TokenIntrospection = ::fulla::oauth2::model::TokenIntrospection;
using AccessTokenCallback = RedisCachedTokenRepositoryBase::AccessTokenCallback;
using TokenIntrospectionCallback = RedisCachedTokenRepositoryBase::TokenIntrospectionCallback;
using VoidCallback = RedisCachedTokenRepositoryBase::VoidCallback;

using namespace ::drogon;
using namespace ::drogon::nosql;

namespace
{
// ---------------------------------------------------------------------------
// D1 (C6): complete (de)serializers for the two cached DTOs. Every field is
// round-tripped — a missing field on read would silently default-construct
// and break introspection/audit consumers. Uses the same Json::StreamWriter/
// CharReader idiom as RedisCachedClientRepository.cc.
// ---------------------------------------------------------------------------

std::string serializeAccessToken(const OAuth2AccessToken &t)
{
    Json::Value j;
    j["cacheSchema"] = 2;
    if (t.orgId)
        j["orgId"] = *t.orgId;
    j["token"] = t.token;
    j["clientId"] = t.clientId;
    j["userId"] = t.userId;
    j["scope"] = t.scope;
    j["expiresAt"] = static_cast<Json::Int64>(t.expiresAt);
    j["revoked"] = t.revoked;
    j["issuedAt"] = static_cast<Json::Int64>(t.issuedAt);
    j["issuer"] = t.issuer;
    j["audience"] = t.audience;
    j["notBefore"] = static_cast<Json::Int64>(t.notBefore);
    j["introspectCount"] = t.introspectCount;
    j["revokedAt"] = static_cast<Json::Int64>(t.revokedAt);
    j["revokedBy"] = t.revokedBy;
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, j);
}

bool deserializeAccessToken(const std::string &s, OAuth2AccessToken &out)
{
    try
    {
        Json::Value root;
        Json::CharReaderBuilder b;
        std::string errs;
        std::istringstream is(s);
        if (!Json::parseFromStream(b, is, &root, &errs))
        {
            LOG_ERROR << "RedisCachedTokenRepository: access-token JSON parse error: " << errs;
            return false;
        }
        // token + expiresAt are the load-bearing fields; the rest default safely.
        if (!root.isMember("token") || !root.isMember("expiresAt"))
            return false;
        if (root.get("cacheSchema", 0).asInt() != 2)
            return false;
        if (root.isMember("orgId"))
        {
            if (!root["orgId"].isInt())
                return false;
            out.orgId = root["orgId"].asInt();
        }
        out.token = root["token"].asString();
        out.clientId = root.get("clientId", "").asString();
        out.userId = root.get("userId", "").asString();
        out.scope = root.get("scope", "").asString();
        out.expiresAt = root.get("expiresAt", Json::Int64(0)).asInt64();
        out.revoked = root.get("revoked", false).asBool();
        out.issuedAt = root.get("issuedAt", Json::Int64(0)).asInt64();
        out.issuer = root.get("issuer", "").asString();
        out.audience = root.get("audience", "").asString();
        out.notBefore = root.get("notBefore", Json::Int64(0)).asInt64();
        out.introspectCount = root.get("introspectCount", 0).asInt();
        out.revokedAt = root.get("revokedAt", Json::Int64(0)).asInt64();
        out.revokedBy = root.get("revokedBy", "").asString();
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

std::string serializeIntrospection(const TokenIntrospection &t)
{
    Json::Value j;
    j["cacheSchema"] = 2;
    if (t.orgId)
        j["orgId"] = *t.orgId;
    j["active"] = t.active;
    j["clientId"] = t.clientId;
    j["tokenType"] = t.tokenType;
    j["exp"] = static_cast<Json::Int64>(t.exp);
    j["iat"] = static_cast<Json::Int64>(t.iat);
    j["nbf"] = static_cast<Json::Int64>(t.nbf);
    j["sub"] = t.sub;
    j["aud"] = t.aud;
    j["iss"] = t.iss;
    j["scope"] = t.scope;
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, j);
}

bool deserializeIntrospection(const std::string &s, TokenIntrospection &out)
{
    try
    {
        Json::Value root;
        Json::CharReaderBuilder b;
        std::string errs;
        std::istringstream is(s);
        if (!Json::parseFromStream(b, is, &root, &errs))
        {
            LOG_ERROR << "RedisCachedTokenRepository: introspection JSON parse error: " << errs;
            return false;
        }
        if (!root.isMember("active"))
            return false;
        if (root.get("cacheSchema", 0).asInt() != 2)
            return false;
        if (root.isMember("orgId"))
        {
            if (!root["orgId"].isInt())
                return false;
            out.orgId = root["orgId"].asInt();
        }
        out.active = root["active"].asBool();
        out.clientId = root.get("clientId", "").asString();
        out.tokenType = root.get("tokenType", "Bearer").asString();
        out.exp = root.get("exp", Json::Int64(0)).asInt64();
        out.iat = root.get("iat", Json::Int64(0)).asInt64();
        out.nbf = root.get("nbf", Json::Int64(0)).asInt64();
        out.sub = root.get("sub", "").asString();
        out.aud = root.get("aud", "").asString();
        out.iss = root.get("iss", "").asString();
        out.scope = root.get("scope", "").asString();
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

const ::fulla::common::ports::MetricLabels kHitLabels{{"repo", "token"}, {"outcome", "hit"}};
const ::fulla::common::ports::MetricLabels kMissLabels{{"repo", "token"}, {"outcome", "miss"}};
const ::fulla::common::ports::MetricLabels kErrorLabels{{"repo", "token"}, {"outcome", "error"}};

// Wave-2 P2 (docs/performance-optimization/optimization-wave-2-plan.md):
// one EVAL round-trip replaces the serial EXISTS(revoked) → GET(value) pair
// (~450µs each per the instrumented report §2) on both token read paths.
// Reply shapes (Lua truncates a trailing nil):
//   {1, ''}          → revoked (negative-cache hit)
//   {0, <string>}    → not revoked, value present
//   {0}              → not revoked, no value (miss)
const char kRevokedThenGetScript[] =
  "local r = redis.call('EXISTS', KEYS[1]) "
  "if r > 0 then return {1, ''} end "
  "return {0, redis.call('GET', KEYS[2])}";

// Tri-state decoder for kRevokedThenGetScript replies.
enum class RevokedThenGet { kRevoked, kValue, kMiss };

RevokedThenGet decodeRevokedThenGet(const RedisResult &result, std::string &valueOut)
{
    if (result.type() != RedisResultType::kArray)
        return RevokedThenGet::kMiss;
    const auto arr = result.asArray();
    if (arr.empty() || arr[0].type() != RedisResultType::kInteger)
        return RevokedThenGet::kMiss;
    if (arr[0].asInteger() > 0)
        return RevokedThenGet::kRevoked;
    if (arr.size() > 1 && arr[1].type() == RedisResultType::kString)
    {
        valueOut = arr[1].asString();
        return RevokedThenGet::kValue;
    }
    return RevokedThenGet::kMiss;
}
// A late database snapshot or queued Redis command must not renew the
// original budget, nor return an active token after a revocation marker.
// The access-key check keeps refresh-token introspections out of L2.
enum class SnapshotCacheResult
{
    Valid,
    Rejected,
    Error
};

void guardedFill(
  const ::drogon::nosql::RedisClientPtr &redis,
  const std::string &token,
  const std::string &payload,
  int64_t expires,
  bool introspection,
  std::function<void(SnapshotCacheResult)> cb
)
{
    static const char script[] =
      "if redis.call('EXISTS', KEYS[1]) > 0 then return 0 end "
      "if tonumber(redis.call('TIME')[1]) >= tonumber(ARGV[2]) then return 0 end "
      "if ARGV[3] == '1' and redis.call('EXISTS', KEYS[3]) == 0 then return 2 end "
      "redis.call('SET', KEYS[2], ARGV[1], 'EXAT', ARGV[2]) return 1";
    auto callback = std::make_shared<decltype(cb)>(std::move(cb));
    const auto revoked = "fulla:cache:token:revoked:" + token;
    const auto key =
      std::string("fulla:cache:token:") + (introspection ? "introspect:" : "access:") + token;
    const auto access = "fulla:cache:token:access:" + token;
    redis->execCommandAsync(
      [callback](const RedisResult &result) {
          (*callback)(
            result.type() != RedisResultType::kInteger ? SnapshotCacheResult::Error
            : result.asInteger() > 0                   ? SnapshotCacheResult::Valid
                                                       : SnapshotCacheResult::Rejected
          );
      },
      [callback](const RedisException &) { (*callback)(SnapshotCacheResult::Error); },
      "EVAL %s 3 %s %s %s %s %lld %d",
      script,
      revoked.c_str(),
      key.c_str(),
      access.c_str(),
      payload.c_str(),
      static_cast<long long>(expires),
      introspection ? 1 : 0
    );
}
}  // namespace

RedisCachedTokenRepository::RedisCachedTokenRepository(
  std::shared_ptr<RedisCachedTokenRepositoryBase> impl,
  RedisClientPtr redisClient,
  std::shared_ptr<::fulla::common::ports::IMetrics> metrics,
  int accessTokenMaxTtlSeconds
) : impl_(std::move(impl)),
    redisClient_(std::move(redisClient)),
    metrics_(std::move(metrics)),
    accessTokenMaxTtlSeconds_(accessTokenMaxTtlSeconds > 0 ? accessTokenMaxTtlSeconds : 60)
{
}

void RedisCachedTokenRepository::emitMetric(const char *outcome) const
{
    if (!metrics_)
        return;
    const auto &labels = std::string_view(outcome) == "hit"   ? kHitLabels
                         : std::string_view(outcome) == "miss" ? kMissLabels
                                                               : kErrorLabels;
    metrics_->incrementCounter("fulla_cache_total", labels);
}

// ---------------------------------------------------------------------------
// getAccessToken (C1: revoked check on every return; C7: TTL guard)
// ---------------------------------------------------------------------------
void RedisCachedTokenRepository::getAccessToken(const std::string &token, AccessTokenCallback &&cb)
{
    auto callback = std::make_shared<AccessTokenCallback>(std::move(cb));
    auto fired = std::make_shared<std::atomic<bool>>(false);
    const auto cacheUntil = std::time(nullptr) + accessTokenMaxTtlSeconds_;
    auto timer = std::make_shared<std::atomic<::trantor::TimerId>>(0);
    auto *loop = ::drogon::app().getLoop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto finish = [callback, fired, timer, loop, deadline, metrics = metrics_](
                    std::optional<OAuth2AccessToken> value
                  ) {
        if (fired->exchange(true))
            return false;
        if (std::chrono::steady_clock::now() >= deadline)
        {
            value = std::nullopt;
            if (metrics)
                metrics->incrementCounter("fulla_cache_total", kErrorLabels);
        }
        const auto id = timer->exchange(0);
        if (id)
            loop->invalidateTimer(id);
        auto deliver = std::move(*callback);
        if (deliver)
            deliver(std::move(value));
        return true;
    };
    auto self = shared_from_this();
    if (!redisClient_)
    {
        emitMetric("miss");
        impl_->getAccessToken(token, std::move(finish));
        return;
    }
    // A reconnecting Redis client can queue forever without an error
    // callback. One deadline spans lookup, master read and guarded fill;
    // completion moves out the business callback even if Redis retains us.
    timer->store(loop->runAfter(5.0, [finish]() { finish(std::nullopt); }));
    redisClient_->execCommandAsync(
      [self, token, cacheUntil, finish, fired](const RedisResult &result) {
          if (fired->load())
              return;
          std::string value;
          const auto state = decodeRevokedThenGet(result, value);
          if (state == RevokedThenGet::kRevoked)
          {
              self->emitMetric("hit");
              finish(std::nullopt);
              return;
          }
          OAuth2AccessToken cached;
          if (
            state == RevokedThenGet::kValue && deserializeAccessToken(value, cached) &&
            cached.token == token && !cached.revoked && cached.expiresAt > std::time(nullptr) &&
            cached.notBefore <= std::time(nullptr)
          )
          {
              self->emitMetric("hit");
              finish(std::move(cached));
              return;
          }
          self->emitMetric("miss");
          self->impl_->getAccessToken(
            token,
            [self, token, cacheUntil, finish, fired](std::optional<OAuth2AccessToken> fresh) {
                if (fired->load())
                    return;
                if (
                  !fresh || fresh->revoked || fresh->notBefore > std::time(nullptr) ||
                  fresh->expiresAt <= std::time(nullptr) || std::time(nullptr) >= cacheUntil
                )
                {
                    finish(std::nullopt);
                    return;
                }
                const auto expires = std::min<int64_t>(fresh->expiresAt, cacheUntil);
                guardedFill(
                  self->redisClient_,
                  token,
                  serializeAccessToken(*fresh),
                  expires,
                  false,
                  [self, fresh, expires, finish](SnapshotCacheResult status) {
                      if (status == SnapshotCacheResult::Error)
                          self->emitMetric("error");
                      finish(
                        status == SnapshotCacheResult::Valid && std::time(nullptr) < expires
                          ? fresh
                          : std::nullopt
                      );
                  }
                );
            }
          );
      },
      [self, token, finish, fired](const RedisException &) mutable {
          if (fired->load())
              return;
          self->emitMetric("error");
          self->impl_->getAccessToken(token, std::move(finish));
      },
      "EVAL %s 2 %s %s",
      kRevokedThenGetScript,
      ("fulla:cache:token:revoked:" + token).c_str(),
      ("fulla:cache:token:access:" + token).c_str()
    );
}

void RedisCachedTokenRepository::introspectToken(
  const std::string &token,
  TokenIntrospectionCallback &&cb
)
{
    auto callback = std::make_shared<TokenIntrospectionCallback>(std::move(cb));
    auto fired = std::make_shared<std::atomic<bool>>(false);
    const auto cacheUntil = std::time(nullptr) + accessTokenMaxTtlSeconds_;
    auto timer = std::make_shared<std::atomic<::trantor::TimerId>>(0);
    auto *loop = ::drogon::app().getLoop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto finish = [callback, fired, timer, loop, deadline, metrics = metrics_](
                    std::optional<TokenIntrospection> value
                  ) {
        if (fired->exchange(true))
            return false;
        if (std::chrono::steady_clock::now() >= deadline)
        {
            value = std::nullopt;
            if (metrics)
                metrics->incrementCounter("fulla_cache_total", kErrorLabels);
        }
        const auto id = timer->exchange(0);
        if (id)
            loop->invalidateTimer(id);
        auto deliver = std::move(*callback);
        if (deliver)
            deliver(std::move(value));
        return true;
    };
    auto self = shared_from_this();
    if (!redisClient_)
    {
        emitMetric("miss");
        impl_->introspectToken(token, std::move(finish));
        return;
    }
    auto inactive = []() {
        TokenIntrospection value;
        value.active = false;
        return value;
    };
    // A reconnecting Redis client can queue forever without an error
    // callback. One deadline spans lookup, master read and guarded fill;
    // completion moves out the business callback even if Redis retains us.
    timer->store(loop->runAfter(5.0, [finish]() { finish(std::nullopt); }));
    redisClient_->execCommandAsync(
      [self, token, cacheUntil, finish, inactive, fired](const RedisResult &result) {
          if (fired->load())
              return;
          std::string value;
          const auto state = decodeRevokedThenGet(result, value);
          if (state == RevokedThenGet::kRevoked)
          {
              self->emitMetric("hit");
              finish(inactive());
              return;
          }
          TokenIntrospection cached;
          if (
            state == RevokedThenGet::kValue && deserializeIntrospection(value, cached) &&
            cached.active && cached.exp > std::time(nullptr) && cached.nbf <= std::time(nullptr)
          )
          {
              self->emitMetric("hit");
              finish(std::move(cached));
              return;
          }
          self->emitMetric("miss");
          self->impl_->introspectToken(
            token,
            [self, token, cacheUntil, finish, inactive, fired](
              std::optional<TokenIntrospection> fresh
            ) {
                if (fired->load())
                    return;
                if (!fresh || !fresh->active)
                {
                    finish(fresh);
                    return;
                }
                const auto expires = std::min<int64_t>(fresh->exp, cacheUntil);
                if (expires <= std::time(nullptr) || fresh->nbf > std::time(nullptr))
                {
                    finish(inactive());
                    return;
                }
                guardedFill(
                  self->redisClient_,
                  token,
                  serializeIntrospection(*fresh),
                  expires,
                  true,
                  [self, fresh, expires, finish, inactive](SnapshotCacheResult status) {
                      if (status == SnapshotCacheResult::Error)
                          self->emitMetric("error");
                      finish(
                        status == SnapshotCacheResult::Valid && std::time(nullptr) < expires
                          ? fresh
                          : inactive()
                      );
                  }
                );
            }
          );
      },
      [self, token, finish, fired](const RedisException &) mutable {
          if (fired->load())
              return;
          self->emitMetric("error");
          self->impl_->introspectToken(token, std::move(finish));
      },
      "EVAL %s 2 %s %s",
      kRevokedThenGetScript,
      ("fulla:cache:token:revoked:" + token).c_str(),
      ("fulla:cache:token:introspect:" + token).c_str()
    );
}

// ---------------------------------------------------------------------------
// revokeAccessToken (negative-cache-before-DEL, §5.4)
// ---------------------------------------------------------------------------
void RedisCachedTokenRepository::revokeAccessToken(
  const std::string &token,
  const std::string &revokedBy,
  VoidCallback &&cb
)
{
    auto sharedCb = std::make_shared<VoidCallback>(std::move(cb));
    auto self = shared_from_this();

    impl_->revokeAccessToken(token, revokedBy, [self, sharedCb, token]() {
        // After the Postgres write succeeds, invalidate the caches. Order
        // matters: SET the negative entry BEFORE DEL-ing the positive entries
        // so a concurrent reader that re-populates the positive cache is
        // corrected on its next read (it sees the revoked marker). The
        // negative entry's 60s TTL is the N3 fixed value (revokeAccessToken
        // has no exp). The SET stays fire-and-forget (best-effort); the
        // positive-entry DELs now go through the shared double-delete helper
        // (#79: the delayed second DEL closes the refill race this comment's
        // marker alone only self-corrects on the NEXT read; #80: failures
        // become WARN/ERROR + counter instead of invisible). The user
        // callback fires regardless.
        if (self->redisClient_)
        {
            std::string revokedKey = "fulla:cache:token:revoked:" + token;
            std::string accessKey = "fulla:cache:token:access:" + token;
            std::string introKey = "fulla:cache:token:introspect:" + token;
            // SET negative entry (60s).
            self->redisClient_->execCommandAsync(
              [](const RedisResult &) {},
              [self](const RedisException &) {
                  self->emitMetric("error");
                  LOG_WARN << "Token revocation cache marker failed";
              },
              "SET %s 1 EX %d",
              revokedKey.c_str(),
              std::max(60, self->accessTokenMaxTtlSeconds_)
            );
            // DEL positive entries (double-delete, failure-observable).
            invalidateWithDoubleDelete(
              self->redisClient_, accessKey, self->metrics_, "token"
            );
            invalidateWithDoubleDelete(
              self->redisClient_, introKey, self->metrics_, "token"
            );
        }
        if (*sharedCb)
            (*sharedCb)();
    });
}

// ---------------------------------------------------------------------------
// saveAccessToken (cache warming — improves cold-start for the N2 discriminator)
// ---------------------------------------------------------------------------
void RedisCachedTokenRepository::saveAccessToken(const OAuth2AccessToken &token, VoidCallback &&cb)
{
    auto callback = std::make_shared<VoidCallback>(std::move(cb));
    auto self = shared_from_this();
    const auto cacheUntil = std::time(nullptr) + accessTokenMaxTtlSeconds_;
    impl_->saveAccessToken(token, [self, callback, token, cacheUntil]() {
        const auto expires = std::min<int64_t>(token.expiresAt, cacheUntil);
        if (self->redisClient_ && !token.revoked && expires > std::time(nullptr))
        {
            guardedFill(
              self->redisClient_,
              token.token,
              serializeAccessToken(token),
              expires,
              false,
              [self](SnapshotCacheResult status) {
                  if (status == SnapshotCacheResult::Error)
                      self->emitMetric("error");
              }
            );
        }
        if (*callback)
            (*callback)();
    });
}

}  // namespace fulla::storage::redis
