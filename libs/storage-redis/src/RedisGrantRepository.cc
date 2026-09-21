#include <fulla/storage/redis/RedisGrantRepository.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <sstream>
#include <chrono>

namespace fulla::storage::redis
{

// Task 27.5: callback + DTO aliases for the new base interface; safe at namespace scope here (this
// .cc does not include IOAuth2Storage.h, so no oauth2::* clash).
using OAuth2AuthCode = ::fulla::oauth2::model::OAuth2AuthCode;
using AuthorizationTransaction = ::fulla::oauth2::model::AuthorizationTransaction;
using BoolCallback = IGrantRepositoryBase::BoolCallback;
using AuthCodeCallback = IGrantRepositoryBase::AuthCodeCallback;
using VoidCallback = IGrantRepositoryBase::VoidCallback;
using TransactionCallback = IGrantRepositoryBase::TransactionCallback;

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

void RedisGrantRepository::saveAuthCode(const OAuth2AuthCode &code, VoidCallback &&cb)
{
    if (!redisClient_)
    {
        if (cb)
            cb();
        return;
    }
    Json::Value val;
    val["client_id"] = code.clientId;
    val["user_id"] = code.userId;
    val["scope"] = code.scope;
    val["redirect_uri"] = code.redirectUri;
    val["expires_at"] = (Json::Int64)code.expiresAt;
    val["used"] = code.used;
    // P0-5 audit fix: the serialization previously dropped the PKCE pair,
    // nonce, auth_time and amr, so code exchange in this storage mode never
    // verified PKCE and never echoed nonce/auth claims. Persist them
    // (omitted when empty so old rows stay readable).
    if (!code.codeChallenge.empty())
    {
        val["code_challenge"] = code.codeChallenge;
        val["code_challenge_method"] = code.codeChallengeMethod;
    }
    if (!code.nonce.empty())
    {
        val["nonce"] = code.nonce;
    }
    if (code.authTime > 0)
    {
        val["auth_time"] = (Json::Int64)code.authTime;
    }
    if (!code.amr.empty())
    {
        val["amr"] = code.amr;
    }
    // v1.5.0 M1 (design §2.1 item 4): the org binding rides the cached
    // code entry (omitted when absent so pre-M1 entries stay readable).
    if (code.orgId.has_value())
    {
        val["org_id"] = (Json::Int64)*code.orgId;
    }
    std::string jsonStr = jsonToString(val);

    auto now = std::chrono::system_clock::now();
    size_t nowSec =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    size_t ttl = (code.expiresAt > (int64_t)nowSec) ? (code.expiresAt - nowSec) : 1;

    std::string key = "oauth2:code:" + code.code;
    std::string ttlStr = std::to_string(ttl);

    LOG_DEBUG << "saveAuthCode CMD: SETEX " << key << " " << ttlStr << " " << jsonStr;

    redisClient_->execCommandAsync(
      [cb, codeStr = code.code](const RedisResult &result) {
          LOG_DEBUG << "saveAuthCode SUCCESS for: " << codeStr << " Result: " << result.asString();
          if (cb)
              cb();
      },
      [cb, codeStr = code.code](const RedisException &e) {
          LOG_ERROR << "saveAuthCode ERROR for: " << codeStr << " Error: " << e.what();
          if (cb)
              cb();
      },
      "SETEX %s %s %s",
      key.c_str(),
      ttlStr.c_str(),
      jsonStr.c_str()
    );
}

void RedisGrantRepository::getAuthCode(const std::string &code, AuthCodeCallback &&cb)
{
    if (!redisClient_)
    {
        cb(std::nullopt);
        return;
    }
    std::string key = "oauth2:code:" + code;
    LOG_DEBUG << "getAuthCode CMD: GET " << key;

    redisClient_->execCommandAsync(
      [cb, codeStr = code](const RedisResult &result) {
          if (result.type() == RedisResultType::kNil)
          {
              LOG_WARN << "getAuthCode: Key not found for: " << codeStr;
              cb(std::nullopt);
              return;
          }
          std::string jsonStr = result.asString();
          LOG_DEBUG << "getAuthCode Result: " << jsonStr;

          auto json = parseJson(jsonStr);
          if (json.isNull())
          {
              LOG_ERROR << "getAuthCode: Failed to parse JSON";
              cb(std::nullopt);
              return;
          }

          OAuth2AuthCode authCode;
          authCode.code = codeStr;
          authCode.clientId = json["client_id"].asString();
          authCode.userId = json["user_id"].asString();
          authCode.scope = json["scope"].asString();
          authCode.redirectUri = json["redirect_uri"].asString();
          authCode.expiresAt = json["expires_at"].asInt64();
          authCode.used = json["used"].asBool();
          // P0-5: fields absent in pre-fix rows read back as jsoncpp's
          // default values ("" / 0) — same shape the DTO default-initializes.
          authCode.codeChallenge = json["code_challenge"].asString();
          authCode.codeChallengeMethod = json["code_challenge_method"].asString();
          authCode.nonce = json["nonce"].asString();
          authCode.authTime = json["auth_time"].asInt64();
          authCode.amr = json["amr"].asString();
          // v1.5.0 M1: org binding (absent in pre-M1 cached entries).
          if (json.isMember("org_id"))
              authCode.orgId = json["org_id"].asInt();
          cb(authCode);
      },
      [cb, codeStr = code](const RedisException &e) {
          LOG_ERROR << "getAuthCode ERROR for: " << codeStr << " Error: " << e.what();
          cb(std::nullopt);
      },
      "GET %s",
      key.c_str()
    );
}

// Mark used: We update the JSON to set used=true, preserving TTL
void RedisGrantRepository::markAuthCodeUsed(const std::string &code, VoidCallback &&cb)
{
    if (!redisClient_)
    {
        if (cb)
            cb();
        return;
    }
    std::string key = "oauth2:code:" + code;

    // Lua script to Atomic Set Used=true
    std::string script = R"(
        local key = KEYS[1]
        local val = redis.call('GET', key)
        if not val then return nil end
        local json = cjson.decode(val)
        json.used = true
        local newVal = cjson.encode(json)
        local ttl = redis.call('TTL', key)
        if ttl > 0 then
            redis.call('SETEX', key, ttl, newVal)
        else
            redis.call('SET', key, newVal)
        end
        return 1
    )";

    redisClient_->execCommandAsync(
      [cb](const RedisResult &) {
          if (cb)
              cb();
      },
      [cb](const RedisException &) {
          if (cb)
              cb();
      },
      "EVAL %s 1 %s",
      script.c_str(),
      key.c_str()
    );
}

void RedisGrantRepository::consumeAuthCode(
  const std::string &code,
  const std::string &redirectUri,
  AuthCodeCallback &&cb
)
{
    if (!redisClient_)
    {
        cb(std::nullopt);
        return;
    }
    std::string key = "oauth2:code:" + code;

    std::string script = R"(
        local key = KEYS[1]
        local redirect_uri = ARGV[1]
        local val = redis.call('GET', key)
        if not val then return nil end
        local json = cjson.decode(val)
        if json.used then return nil end
        -- CRITICAL: Validate redirect_uri matches authorization
        -- Per OAuth2 RFC 6749 Section 4.1.3. P0-5 audit fix: this previously
        -- skipped the check whenever the REQUEST side was empty, letting a
        -- token-request omit redirect_uri to bypass the binding. The
        -- Postgres repository's semantics are authoritative: when a
        -- redirect_uri was recorded at authorization time it is REQUIRED at
        -- the token endpoint and MUST be identical — an empty request value
        -- fails just like any other mismatch.
        local stored = json.redirect_uri
        if stored == cjson.null then stored = '' end
        if stored ~= '' and redirect_uri ~= stored then
            return nil
        end
        json.used = true
        local newVal = cjson.encode(json)
        local ttl = redis.call('TTL', key)
        if ttl > 0 then
            redis.call('SETEX', key, ttl, newVal)
        else
            redis.call('SET', key, newVal)
        end
        return newVal
    )";

    redisClient_->execCommandAsync(
      [cb, codeStr = code, requestUri = redirectUri](const RedisResult &result) {
          if (result.type() == RedisResultType::kNil)
          {
              // Log if this was a redirect_uri mismatch vs code not found
              // (we can't distinguish in Lua script, but we can log the
              // attempt)
              if (!requestUri.empty())
              {
                  LOG_WARN << "[SECURITY] Auth code consumption failed "
                           << "(code not found, expired, or redirect_uri "
                              "mismatch): "
                           << codeStr;
              }
              cb(std::nullopt);
              return;
          }
          std::string jsonStr = result.asString();

          auto json = parseJson(jsonStr);

          if (json.isNull())
          {
              LOG_ERROR << "consumeAuthCode: Failed to parse JSON result";
              cb(std::nullopt);
              return;
          }

          OAuth2AuthCode authCode;
          authCode.code = codeStr;
          authCode.clientId = json["client_id"].asString();
          authCode.userId = json["user_id"].asString();
          authCode.scope = json["scope"].asString();
          authCode.redirectUri = json["redirect_uri"].asString();
          authCode.expiresAt = json["expires_at"].asInt64();
          authCode.used = true;  // We just marked it
          // P0-5: round-trip the PKCE pair + OIDC context so the token
          // endpoint enforces the verifier and echoes nonce/auth claims in
          // this storage mode too.
          authCode.codeChallenge = json["code_challenge"].asString();
          authCode.codeChallengeMethod = json["code_challenge_method"].asString();
          authCode.nonce = json["nonce"].asString();
          authCode.authTime = json["auth_time"].asInt64();
          authCode.amr = json["amr"].asString();
          // v1.5.0 M1 (review 1.1): the org binding must survive the
          // consume path too -- token exchange reads it here (write/read
          // symmetry with saveAuthCode/getAuthCode; absent in pre-M1
          // cached entries).
          if (json.isMember("org_id"))
          {
              authCode.orgId = json["org_id"].asInt();
          }

          cb(authCode);
      },
      [cb](const RedisException &e) {
          LOG_ERROR << "consumeAuthCode Redis Error: " << e.what();
          cb(std::nullopt);
      },
      "EVAL %s 1 %s %s",
      script.c_str(),
      key.c_str(),
      redirectUri.c_str()
    );
}

// ========== Authorization Transaction Operations ==========

void RedisGrantRepository::saveAuthorizationTransaction(
  const AuthorizationTransaction &transaction,
  BoolCallback &&cb
)
{
    if (!redisClient_)
    {
        cb(false);
        return;
    }

    std::string key = "oauth2:transaction:" + transaction.transactionId;
    Json::Value val;
    val["transaction_id"] = transaction.transactionId;
    val["client_id"] = transaction.clientId;
    val["subject"] = transaction.subject;
    val["redirect_uri"] = transaction.redirectUri;
    val["state"] = transaction.state;
    val["code_challenge"] = transaction.codeChallenge;
    val["code_challenge_method"] = transaction.codeChallengeMethod;
    val["consumed"] = transaction.consumed;
    val["expires_at"] = (Json::Int64)transaction.expiresAt;

    // Serialize requested scopes
    Json::Value scopesJson(Json::arrayValue);
    for (const auto &scope : transaction.requestedScopes)
        scopesJson.append(scope);
    val["requested_scopes"] = scopesJson;

    // Serialize valid scopes
    Json::Value validScopesJson(Json::arrayValue);
    for (const auto &scope : transaction.validScopes)
        validScopesJson.append(scope);
    val["valid_scopes"] = validScopesJson;

    // Serialize consent required scopes
    Json::Value consentScopesJson(Json::arrayValue);
    for (const auto &scope : transaction.consentRequiredScopes)
        consentScopesJson.append(scope);
    val["consent_required_scopes"] = consentScopesJson;

    std::string jsonStr = jsonToString(val);

    auto now = std::chrono::system_clock::now();
    size_t nowSec =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    size_t ttl = (transaction.expiresAt > (int64_t)nowSec) ? (transaction.expiresAt - nowSec) : 600;

    redisClient_->execCommandAsync(
      [cb](const RedisResult &) { cb(true); },
      [cb](const RedisException &e) {
          LOG_ERROR << "Failed to save authorization transaction: " << e.what();
          cb(false);
      },
      "SETEX %s %d %s",
      key.c_str(),
      ttl,
      jsonStr.c_str()
    );
}

void RedisGrantRepository::getAuthorizationTransaction(
  const std::string &transactionId,
  TransactionCallback &&cb
)
{
    if (!redisClient_)
    {
        cb(std::nullopt);
        return;
    }

    std::string key = "oauth2:transaction:" + transactionId;
    redisClient_->execCommandAsync(
      [cb](const RedisResult &result) {
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

          AuthorizationTransaction transaction;
          transaction.transactionId = json["transaction_id"].asString();
          transaction.clientId = json["client_id"].asString();
          transaction.subject = json["subject"].asString();
          transaction.redirectUri = json["redirect_uri"].asString();
          transaction.state = json["state"].asString();
          transaction.codeChallenge = json["code_challenge"].asString();
          transaction.codeChallengeMethod = json["code_challenge_method"].asString();
          transaction.consumed = json["consumed"].asBool();
          transaction.expiresAt = json["expires_at"].asInt64();

          // Parse requested scopes
          if (json.isMember("requested_scopes") && json["requested_scopes"].isArray())
          {
              for (const auto &scope : json["requested_scopes"])
                  transaction.requestedScopes.push_back(scope.asString());
          }

          // Parse valid scopes
          if (json.isMember("valid_scopes") && json["valid_scopes"].isArray())
          {
              for (const auto &scope : json["valid_scopes"])
                  transaction.validScopes.push_back(scope.asString());
          }

          // Parse consent required scopes
          if (json.isMember("consent_required_scopes") && json["consent_required_scopes"].isArray())
          {
              for (const auto &scope : json["consent_required_scopes"])
                  transaction.consentRequiredScopes.push_back(scope.asString());
          }

          cb(transaction);
      },
      [cb](const RedisException &e) {
          LOG_ERROR << "Failed to get authorization transaction: " << e.what();
          cb(std::nullopt);
      },
      "GET %s",
      key.c_str()
    );
}

void RedisGrantRepository::deleteAuthorizationTransaction(
  const std::string &transactionId,
  VoidCallback &&cb
)
{
    if (!redisClient_)
    {
        if (cb)
            cb();
        return;
    }

    std::string key = "oauth2:transaction:" + transactionId;
    redisClient_->execCommandAsync(
      [cb](const RedisResult &) {
          if (cb)
              cb();
      },
      [cb](const RedisException &) {
          if (cb)
              cb();
      },
      "DEL %s",
      key.c_str()
    );
}

void RedisGrantRepository::markTransactionConsumed(
  const std::string &transactionId,
  BoolCallback &&cb
)
{
    if (!redisClient_)
    {
        cb(false);
        return;
    }

    std::string script = R"(
        local key = KEYS[1]
        local val = redis.call('GET', key)
        if not val then return 0 end
        local json = cjson.decode(val)
        if json.consumed then return 0 end
        json.consumed = true
        local newVal = cjson.encode(json)
        redis.call('SETEX', key, redis.call('TTL', key), newVal)
        return 1
    )";

    redisClient_->execCommandAsync(
      [cb](const RedisResult &result) {
          // Script returns 1 if marked successfully, 0 if already consumed or
          // not found
          cb(result.asInteger() == 1);
      },
      [cb](const RedisException &e) {
          LOG_ERROR << "Failed to mark transaction as consumed: " << e.what();
          cb(false);
      },
      "EVAL %s 1 %s",
      script.c_str(),
      transactionId.c_str()
    );
}

// ========== Cleanup ==========

void RedisGrantRepository::purgeExpired()
{
    // Verbatim preservation of RedisOAuth2Storage::deleteExpiredData()'s
    // documented no-op: auth codes and authorization transactions are both
    // stored with SETEX, so Redis's own key TTL mechanism purges them. There
    // is no real cleanup logic to port here.
    LOG_DEBUG << "RedisGrantRepository::purgeExpired called (No-op, relying on Redis TTL)";
}

}  // namespace fulla::storage::redis
