#include <fulla/storage/redis/RedisCachedClientRepository.h>
#include <fulla/oauth2/model/ClientType.h>
#include <fulla/common/utils/ConstantTimeCompare.h>

#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <algorithm>
#include <sstream>
#include <string_view>

namespace fulla::storage::redis
{

// Callback + DTO aliases for the base interface (safe at namespace scope here:
// this .cc does not include IOAuth2Storage.h, so no oauth2::* clash).
using OAuth2Client = ::fulla::oauth2::model::OAuth2Client;
using ClientCallback = RedisCachedClientRepositoryBase::ClientCallback;

using namespace ::drogon;
using namespace ::drogon::nosql;

namespace
{
// JSON helpers — same Json::CharReaderBuilder / StreamWriterBuilder pattern as
// RedisClientRepository.cc (this package's existing serialization idiom).

// Serialize an OAuth2Client to a compact JSON string for Redis SET.
// clientSecretHash and salt ARE included: they are hashes/salts (not plaintext
// secrets), and the wrapped Postgres impl already returns them in getClient().
// The cache key is the clientId (a non-secret identifier); the raw token is
// never stored (design §5.3).
std::string serializeClient(const OAuth2Client &c)
{
    Json::Value json;
    json["clientId"] = c.clientId;
    json["clientType"] = ::fulla::oauth2::model::clientTypeToString(c.clientType);
    json["clientSecretHash"] = c.clientSecretHash;
    json["salt"] = c.salt;
    json["tokenEndpointAuthMethod"] = c.tokenEndpointAuthMethod;

    Json::Value redirectUris(Json::arrayValue);
    for (const auto &uri : c.redirectUris)
        redirectUris.append(uri);
    json["redirectUris"] = redirectUris;

    Json::Value allowedScopes(Json::arrayValue);
    for (const auto &s : c.allowedScopes)
        allowedScopes.append(s);
    json["allowedScopes"] = allowedScopes;

    // #220: registered grant types. Older cache entries lack the member and
    // deserialize to an empty list = unrestricted (legacy semantics).
    Json::Value allowedGrantTypes(Json::arrayValue);
    for (const auto &g : c.allowedGrantTypes)
        allowedGrantTypes.append(g);
    json["allowedGrantTypes"] = allowedGrantTypes;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, json);
}

// Deserialize a JSON string back into an OAuth2Client. Returns false on parse
// error or missing required fields (the caller treats false as a cache miss).
bool deserializeClient(const std::string &jsonStr, OAuth2Client &out)
{
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream s(jsonStr);
    if (!Json::parseFromStream(builder, s, &root, &errs))
    {
        LOG_ERROR << "RedisCachedClientRepository: JSON parse error: " << errs;
        return false;
    }
    if (!root.isMember("clientId") || !root.isMember("clientType"))
        return false;

    out.clientId = root["clientId"].asString();
    // PR #47 review (Owner #1, defense-in-depth): a structurally-valid-but-
    // empty clientId would produce a blank OAuth2Client that masks the real
    // (non-empty) entry in the backing repo. Reject it as a miss.
    if (out.clientId.empty())
        return false;
    try
    {
        out.clientType =
          ::fulla::oauth2::model::stringToClientType(root["clientType"].asString());
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "RedisCachedClientRepository: bad clientType: " << e.what();
        return false;
    }
    out.clientSecretHash = root.get("clientSecretHash", "").asString();
    out.salt = root.get("salt", "").asString();
    out.tokenEndpointAuthMethod = root.get("tokenEndpointAuthMethod", "").asString();

    if (root.isMember("redirectUris") && root["redirectUris"].isArray())
    {
        for (const auto &uri : root["redirectUris"])
            out.redirectUris.push_back(uri.asString());
    }
    if (root.isMember("allowedScopes") && root["allowedScopes"].isArray())
    {
        for (const auto &sc : root["allowedScopes"])
            out.allowedScopes.push_back(sc.asString());
    }
    if (root.isMember("allowedGrantTypes") && root["allowedGrantTypes"].isArray())
    {
        for (const auto &g : root["allowedGrantTypes"])
            out.allowedGrantTypes.push_back(g.asString());
    }
    return true;
}

// Metric label sets are allocated once (small maps); reusing them avoids per-call
// unordered_map construction on the hot path.
const ::fulla::common::ports::MetricLabels kHitLabels{{"repo", "client"}, {"outcome", "hit"}};
const ::fulla::common::ports::MetricLabels kMissLabels{{"repo", "client"}, {"outcome", "miss"}};
const ::fulla::common::ports::MetricLabels kErrorLabels{{"repo", "client"}, {"outcome", "error"}};

// Wave-2 P0 (docs/performance-optimization/optimization-wave-2-plan.md):
// validate the secret against the CACHED row, eliminating one PG round-trip
// per token/introspect request. This must stay semantically identical to the
// Postgres path (PostgresClientRepository.cc:190-251):
//   - PUBLIC clients are accepted without a secret;
//   - CONFIDENTIAL with an empty secret is rejected;
//   - sha256(secret + salt), lowercased on both sides, constant-time
//     comparison with length equality.
// The deserialized DTO already carries clientType (enum), clientSecretHash
// and salt — a row whose type failed to parse fails deserializeClient and
// is treated as a cache miss (the PG path re-validates), so no extra
// type-fallback branch is needed here.
bool validateCachedClientSecret(const OAuth2Client &client, const std::string &clientSecret)
{
    if (client.clientType == ::fulla::oauth2::model::ClientType::PUBLIC)
        return true;
    if (clientSecret.empty())
        return false;
    std::string computedHash = ::drogon::utils::getSha256(clientSecret + client.salt);
    std::transform(
      computedHash.begin(), computedHash.end(), computedHash.begin(), [](unsigned char c) {
          return static_cast<char>(::tolower(c));
      }
    );
    std::string storedLower = client.clientSecretHash;
    std::transform(
      storedLower.begin(), storedLower.end(), storedLower.begin(), [](unsigned char c) {
          return static_cast<char>(::tolower(c));
      }
    );
    const size_t cmpLen =
      (computedHash.length() < storedLower.length()) ? computedHash.length() : storedLower.length();
    return ::fulla::common::utils::constantTimeMemcmp(
             computedHash.c_str(), storedLower.c_str(), cmpLen
           ) == 0 &&
           computedHash.length() == storedLower.length();
}
}  // namespace

RedisCachedClientRepository::RedisCachedClientRepository(
  std::shared_ptr<RedisCachedClientRepositoryBase> impl,
  RedisClientPtr redisClient,
  std::shared_ptr<::fulla::common::ports::IMetrics> metrics,
  int clientTtlSeconds
) : impl_(std::move(impl)),
    redisClient_(std::move(redisClient)),
    metrics_(std::move(metrics)),
    clientTtlSeconds_(clientTtlSeconds > 0 ? clientTtlSeconds : 300)
{
}

void RedisCachedClientRepository::emitMetric(const char *outcome) const
{
    if (!metrics_)
        return;
    // PR #47 review (Owner #3): explicit string compare is clearer than the
    // first-character dispatch and survives a label rename.
    const auto &labels = std::string_view(outcome) == "hit"   ? kHitLabels
                         : std::string_view(outcome) == "miss" ? kMissLabels
                                                               : kErrorLabels;
    metrics_->incrementCounter("fulla_cache_total", labels);
}

void RedisCachedClientRepository::getClient(const std::string &clientId, ClientCallback &&cb)
{
    // Shared callback + once-guard: the user callback MUST fire exactly once
    // (design §5.5/S1). drogon may invoke both the success and error callbacks
    // on a torn connection; the atomic<bool> exchange prevents a double fire.
    auto sharedCb = std::make_shared<ClientCallback>(std::move(cb));
    auto fired = std::make_shared<std::atomic<bool>>(false);
    auto self = shared_from_this();  // keep this decorator alive across async hops

    // Cache-fill continuation: after the wrapped impl returns a populated
    // client (on a miss path), issue a fire-and-forget SET to populate the
    // cache for the next reader. A null/unpopulated result is NOT cached
    // (missing client — caching nullopt would shadow a future registration).
    // The SET is best-effort: a failed SET just means the next read also misses.
    auto delegateAndFill = [self, sharedCb, fired, clientId](bool recordMiss) {
        if (recordMiss)
            self->emitMetric("miss");
        // Wrap the user callback so we can intercept the impl_ result and fill
        // the cache before forwarding. The wrapper respects `fired` so it stays
        // exactly-once even if (hypothetically) installed on a path that already
        // fired (it is not, but defense-in-depth is cheap here).
        auto wrappingCb = std::make_shared<ClientCallback>(
          [self, sharedCb, fired, clientId](const std::optional<OAuth2Client> &client) {
              if (client && self->redisClient_)
              {
                  // Fire-and-forget SET EX. Captures self so the decorator (and
                  // its redisClient_) survive the async hop; ignores both result
                  // and error (best-effort fill).
                  std::string payload = serializeClient(*client);
                  std::string key = "fulla:cache:client:" + clientId;
                  self->redisClient_->execCommandAsync(
                    [](const RedisResult &) {},  // ignore success
                    [key](const RedisException &e) {
                        LOG_DEBUG << "RedisCachedClientRepository: cache-fill SET failed for "
                                  << key << ": " << e.what();
                    },
                    "SET %s %s EX %d",
                    key.c_str(),
                    payload.c_str(),
                    self->clientTtlSeconds_
                  );
              }
              if (!fired->exchange(true))
                  (*sharedCb)(client);
          }
        );
        self->impl_->getClient(clientId, std::move(*wrappingCb));
    };

    // Null Redis client → pure pass-through to the wrapped impl (soft-fail).
    if (!redisClient_)
    {
        delegateAndFill(true);
        return;
    }

    std::string cmd = "GET fulla:cache:client:" + clientId;
    redisClient_->execCommandAsync(
      [self, sharedCb, fired, delegateAndFill, clientId](const RedisResult &result) {
          // Cache HIT: a string value we can deserialize.
          if (result.type() == RedisResultType::kString)
          {
              OAuth2Client cached;
              if (deserializeClient(result.asString(), cached))
              {
                  self->emitMetric("hit");
                  if (!fired->exchange(true))
                      (*sharedCb)(std::move(cached));
                  return;
              }
              // Deserialization failed → treat as a miss (don't trust corrupt cache).
          }
          // Cache MISS (kNil, kArray, or deserialize failure) → delegate + fill.
          delegateAndFill(true);
      },
      [self, sharedCb, fired, clientId](const RedisException &e) {
          LOG_WARN << "RedisCachedClientRepository: GET error for client " << clientId << ": "
                   << e.what();
          // Error → soft-fail to impl (design §5.5). Use the error metric (not
          // miss) so operators can distinguish Redis errors from genuine misses.
          self->emitMetric("error");
          // Delegate WITHOUT fill: the error path signals Redis is unhealthy, so
          // a SET would likely also fail and add noise. The next successful read
          // path (miss → fill) repopulates once Redis recovers.
          if (!fired->exchange(true))
              self->impl_->getClient(clientId, std::move(*sharedCb));
      },
      cmd.c_str()
    );
}

void RedisCachedClientRepository::validateClient(
  const std::string &clientId,
  const std::string &clientSecret,
  BoolCallback &&cb
)
{
    // Wave-2 P0: on a cache hit, validate the secret locally against the
    // cached row (identical semantics to the Postgres path — see
    // validateCachedClientSecret above) instead of one PG round-trip per
    // request. On a miss we pass through to the wrapped impl; the cache is
    // then filled by the getClient call that token/introspect issue in the
    // same request, so subsequent validations hit.
    auto sharedCb = std::make_shared<BoolCallback>(std::move(cb));
    auto self = shared_from_this();

    if (!redisClient_)
    {
        impl_->validateClient(clientId, clientSecret, std::move(*sharedCb));
        return;
    }

    std::string cmd = "GET fulla:cache:client:" + clientId;
    redisClient_->execCommandAsync(
      [self, sharedCb, clientId, clientSecret](const RedisResult &result) {
          if (result.type() == RedisResultType::kString)
          {
              OAuth2Client cached;
              if (deserializeClient(result.asString(), cached))
              {
                  self->emitMetric("hit");
                  (*sharedCb)(validateCachedClientSecret(cached, clientSecret));
                  return;
              }
              // Corrupt cache entry → treat as a miss (don't trust it).
          }
          self->emitMetric("miss");
          self->impl_->validateClient(clientId, clientSecret, std::move(*sharedCb));
      },
      [self, sharedCb, clientId, clientSecret](const RedisException &e) {
          LOG_WARN << "RedisCachedClientRepository: validateClient GET error for " << clientId
                   << ": " << e.what();
          self->emitMetric("error");
          // Error → soft-fail to the Postgres path (same policy as getClient).
          self->impl_->validateClient(clientId, clientSecret, std::move(*sharedCb));
      },
      cmd.c_str()
    );
}

}  // namespace fulla::storage::redis
