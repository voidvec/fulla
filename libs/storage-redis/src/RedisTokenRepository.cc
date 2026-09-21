#include <fulla/storage/redis/RedisTokenRepository.h>
#include <json/json.h>
#include <sstream>
#include <ctime>
#include <chrono>

namespace fulla::storage::redis
{

// Task 27.5: callback + DTO aliases for the new base interface; safe at namespace scope here (this
// .cc does not include IOAuth2Storage.h, so no oauth2::* clash).
using OAuth2AccessToken = ::fulla::oauth2::model::OAuth2AccessToken;
using OAuth2RefreshToken = ::fulla::oauth2::model::OAuth2RefreshToken;
using TokenIntrospection = ::fulla::oauth2::model::TokenIntrospection;
using VoidCallback = ITokenRepositoryBase::VoidCallback;
using AccessTokenCallback = ITokenRepositoryBase::AccessTokenCallback;
using RefreshTokenCallback = ITokenRepositoryBase::RefreshTokenCallback;
using TokenIntrospectionCallback = ITokenRepositoryBase::TokenIntrospectionCallback;

using namespace ::drogon;
using namespace ::drogon::nosql;

namespace
{
// Verbatim copies of the anonymous-namespace JSON helpers from
// RedisOAuth2Storage.cc.
Json::Value parseJson(const std::string &jsonStr)
{
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream s(jsonStr);
    if (!Json::parseFromStream(builder, s, &root, &errs))
    {
        LOG_ERROR << "Redis JSON parse error: " << errs;
        return Json::nullValue;
    }
    return root;
}

std::string jsonToString(const Json::Value &json)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";  // Compact output
    return Json::writeString(builder, json);
}
}  // namespace

void RedisTokenRepository::saveAccessToken(const OAuth2AccessToken &token, VoidCallback &&cb)
{
    if (!redisClient_)
    {
        if (cb)
            cb();
        return;
    }
    Json::Value val;
    val["client_id"] = token.clientId;
    val["user_id"] = token.userId;
    val["scope"] = token.scope;
    val["expires_at"] = (Json::Int64)token.expiresAt;
    val["revoked"] = token.revoked;

    // P1: RFC 7662 fields
    val["issued_at"] = (Json::Int64)token.issuedAt;
    val["issuer"] = token.issuer;
    val["audience"] = token.audience;
    val["not_before"] = (Json::Int64)token.notBefore;
    val["introspect_count"] = token.introspectCount;
    val["revoked_at"] = (Json::Int64)token.revokedAt;
    val["revoked_by"] = token.revokedBy;
    // v1.5.0 M1: org binding (omitted when absent so pre-M1 cached
    // entries stay readable without the field).
    if (token.orgId.has_value())
    {
        val["org_id"] = (Json::Int64)*token.orgId;
    }

    std::string jsonStr = jsonToString(val);

    auto now = std::chrono::system_clock::now();
    size_t nowSec =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    size_t ttl = (token.expiresAt > (int64_t)nowSec) ? (token.expiresAt - nowSec) : 1;

    std::string key = "oauth2:token:" + token.token;
    std::string ttlStr = std::to_string(ttl);

    redisClient_->execCommandAsync(
      [cb](const RedisResult &) {
          if (cb)
              cb();
      },
      [cb](const RedisException &) {
          if (cb)
              cb();
      },
      "SETEX %s %s %s",
      key.c_str(),
      ttlStr.c_str(),
      jsonStr.c_str()
    );
}

void RedisTokenRepository::getAccessToken(const std::string &token, AccessTokenCallback &&cb)
{
    if (!redisClient_)
    {
        cb(std::nullopt);
        return;
    }
    std::string key = "oauth2:token:" + token;
    redisClient_->execCommandAsync(
      [cb, tokenStr = token](const RedisResult &result) {
          if (result.type() == RedisResultType::kNil)
          {
              cb(std::nullopt);
              return;
          }
          std::string jsonStr = result.asString();
          auto json = parseJson(jsonStr);
          if (json.isNull())
          {
              cb(std::nullopt);
              return;
          }
          OAuth2AccessToken accessToken;
          accessToken.token = tokenStr;
          accessToken.clientId = json["client_id"].asString();
          accessToken.userId = json["user_id"].asString();
          accessToken.scope = json["scope"].asString();
          accessToken.expiresAt = json["expires_at"].asInt64();
          accessToken.revoked = json["revoked"].asBool();

          // P1: RFC 7662 fields (with backward compatibility)
          if (json.isMember("issued_at"))
              accessToken.issuedAt = json["issued_at"].asInt64();
          if (json.isMember("issuer"))
              accessToken.issuer = json["issuer"].asString();
          if (json.isMember("audience"))
              accessToken.audience = json["audience"].asString();
          if (json.isMember("not_before"))
              accessToken.notBefore = json["not_before"].asInt64();
          if (json.isMember("introspect_count"))
              accessToken.introspectCount = json["introspect_count"].asInt();
          if (json.isMember("revoked_at"))
              accessToken.revokedAt = json["revoked_at"].asInt64();
          // v1.5.0 M1: org binding (absent in pre-M1 cached entries).
          // Same depth as the sibling ifs above: the pre-existing
          // revoked_by block sat two spaces deeper, which made GCC's
          // -Wmisleading-indentation read the chain as nested
          // (CI linux leg, -Werror).
          if (json.isMember("revoked_by"))
              accessToken.revokedBy = json["revoked_by"].asString();
          if (json.isMember("org_id"))
              accessToken.orgId = json["org_id"].asInt();

          cb(accessToken);
      },
      [cb](const RedisException &) { cb(std::nullopt); },
      "GET %s",
      key.c_str()
    );
}

// Verbatim preservation of RedisOAuth2Storage's no-op: Redis mode never
// actually stored/retrieved refresh tokens.
void RedisTokenRepository::saveRefreshToken(const OAuth2RefreshToken & /*token*/, VoidCallback &&cb)
{
    if (cb)
        cb();
}

void RedisTokenRepository::getRefreshToken(const std::string & /*token*/, RefreshTokenCallback &&cb)
{
    if (cb)
        cb(std::nullopt);
}

void RedisTokenRepository::revokeRefreshToken(const std::string &token, VoidCallback &&cb)
{
    if (!redisClient_)
    {
        if (cb)
            cb();
        return;
    }

    redisClient_->execCommandAsync(
      [cb](const RedisResult &) {
          if (cb)
              cb();
      },
      [cb](const RedisException &e) {
          LOG_ERROR << "Failed to revoke refresh token in Redis: " << e.what();
          // Call callback even on failure to avoid blocking
          if (cb)
              cb();
      },
      "HSET oauth2_refresh_tokens:%s revoked 1",
      token.c_str()
    );
}

void RedisTokenRepository::atomicRevokeRefreshToken(
  const std::string &token,
  RefreshTokenCallback &&cb
)
{
    // Redis doesn't have native CAS, but we can use HSETNX-like logic
    // For simplicity, get then set (acceptable for Redis single-threaded model)
    // NOTE (see supportsCas() == false rationale in the header): this is a
    // genuinely non-atomic two-step even setting aside the fact that
    // getRefreshToken() is currently a no-op below.
    getRefreshToken(
      token, [self = shared_from_this(), this, token, cb = std::move(cb)](auto rt) mutable {
          if (!rt || rt->revoked)
          {
              cb(std::nullopt);
              return;
          }
          auto captured = *rt;
          revokeRefreshToken(token, [cb = std::move(cb), captured]() { cb(captured); });
      }
    );
}

void RedisTokenRepository::revokeTokenFamily(const std::string &familyId, VoidCallback &&cb)
{
    // Redis doesn't support efficient family queries without secondary indexes
    // For production Redis usage, maintain a SET of tokens per family
    // For now, just log and callback (family tracking is primarily for Postgres)
    LOG_WARN << "[SECURITY] Token family revocation requested for: " << familyId
             << " (Redis: limited support)";
    if (cb)
        cb();
}

// ========== P1: Token Introspection (RFC 7662) ==========

void RedisTokenRepository::introspectToken(
  const std::string &token,
  TokenIntrospectionCallback &&cb
)
{
    if (!redisClient_)
    {
        TokenIntrospection introspection;
        introspection.active = false;
        cb(introspection);
        return;
    }

    std::string key = "oauth2:token:" + token;
    redisClient_->execCommandAsync(
      [cb](const RedisResult &result) {
          if (result.type() == RedisResultType::kNil)
          {
              TokenIntrospection introspection;
              introspection.active = false;
              cb(introspection);
              return;
          }

          std::string jsonStr = result.asString();
          auto json = parseJson(jsonStr);

          if (json.isNull())
          {
              TokenIntrospection introspection;
              introspection.active = false;
              cb(introspection);
              return;
          }

          // Check if token is revoked or expired
          bool revoked = json["revoked"].asBool();
          int64_t expiresAt = json["expires_at"].asInt64();
          int64_t now = std::time(nullptr);

          if (revoked || expiresAt < now)
          {
              TokenIntrospection introspection;
              introspection.active = false;
              cb(introspection);
              return;
          }

          // Token is active, populate introspection data
          TokenIntrospection introspection;
          introspection.active = true;
          introspection.clientId = json["client_id"].asString();
          introspection.tokenType = "Bearer";
          introspection.exp = expiresAt;

          // P1 fields (with backward compatibility)
          if (json.isMember("issued_at"))
              introspection.iat = json["issued_at"].asInt64();
          else
              introspection.iat = now;

          // F-016: no hardcoded fallback issuer; an empty iss lets the
          // introspect controller backfill from the configured issuer.
          if (json.isMember("issuer"))
              introspection.iss = json["issuer"].asString();

          if (json.isMember("audience"))
              introspection.aud = json["audience"].asString();

          if (json.isMember("not_before"))
              introspection.nbf = json["not_before"].asInt64();
          else
              introspection.nbf = now;

          introspection.sub = json["user_id"].asString();
          introspection.scope = json["scope"].asString();
          // v1.5.0 M1: org binding (absent in pre-M1 cached entries).
          if (json.isMember("org_id"))
              introspection.orgId = json["org_id"].asInt();

          cb(introspection);
      },
      [cb](const RedisException &) {
          TokenIntrospection introspection;
          introspection.active = false;
          cb(introspection);
      },
      "GET %s",
      key.c_str()
    );
}

void RedisTokenRepository::incrementIntrospectCount(const std::string &token, VoidCallback &&cb)
{
    if (!redisClient_)
    {
        if (cb)
            cb();
        return;
    }

    std::string key = "oauth2:token:" + token;
    redisClient_->execCommandAsync(
      [cb](const RedisResult & /*result*/) {
          // Note: Redis doesn't have atomic increment for JSON fields
          // We need to get the JSON, update the field, and set it back
          // For now, this is a no-op in Redis storage to avoid race conditions
          // The introspect_count is mainly for monitoring in PostgreSQL
          if (cb)
              cb();
      },
      [cb](const RedisException &) {
          if (cb)
              cb();
      },
      "GET %s",
      key.c_str()
    );
}

// ========== P1: Token Revocation (RFC 7009) ==========

void RedisTokenRepository::revokeAccessToken(
  const std::string &token,
  const std::string &revokedBy,
  VoidCallback &&cb
)
{
    if (!redisClient_)
    {
        if (cb)
            cb();
        return;
    }

    std::string key = "oauth2:token:" + token;
    redisClient_->execCommandAsync(
      [self = shared_from_this(), this, cb, key, revokedBy](const RedisResult &result) {
          if (result.type() == RedisResultType::kNil)
          {
              // Token doesn't exist, but return success per RFC 7009
              if (cb)
                  cb();
              return;
          }

          // Token exists, revoke it by updating JSON
          std::string jsonStr = result.asString();
          auto json = parseJson(jsonStr);
          if (!json.isNull())
          {
              json["revoked"] = true;
              json["revoked_at"] = (Json::Int64)std::time(nullptr);
              json["revoked_by"] = revokedBy;

              std::string updatedJsonStr = jsonToString(json);

              // Update the token with revoked status
              // Note: This is not atomic, but acceptable for Redis cache
              redisClient_->execCommandAsync(
                [cb](const RedisResult &) {
                    LOG_INFO << "Token revoked successfully in Redis";
                    if (cb)
                        cb();
                },
                [cb](const RedisException &) {
                    LOG_ERROR << "Failed to update revoked token in Redis";
                    if (cb)
                        cb();
                },
                "SETEX %s %s %s",
                key.c_str(),
                "3600",  // Keep for 1 hour (cleanup will handle it)
                updatedJsonStr.c_str()
              );
          }
          else
          {
              if (cb)
                  cb();
          }
      },
      [cb](const RedisException &) {
          // Token doesn't exist or Redis error
          // Return success per RFC 7009 (prevent token probing)
          if (cb)
              cb();
      },
      "GET %s",
      key.c_str()
    );
}

// ========== Cleanup ==========

void RedisTokenRepository::purgeExpired()
{
    // Verbatim preservation of RedisOAuth2Storage::deleteExpiredData()'s
    // documented no-op: access/refresh tokens are stored with SETEX, so
    // Redis's own key TTL mechanism purges them.
    LOG_DEBUG << "RedisTokenRepository::purgeExpired called (No-op, relying on Redis TTL)";
}

}  // namespace fulla::storage::redis
