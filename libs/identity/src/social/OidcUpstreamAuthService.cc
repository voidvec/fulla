// OidcUpstreamAuthService.cc -- see the header's contract block.
//
// P2-1: generic OIDC-in federation for registry-configured upstreams. All
// upstream I/O goes through the injected transport seams; every continuation
// captures `self` (domain-service lifetime rule).

#ifdef WITH_SOCIAL

#include <fulla/identity/OidcUpstreamAuthService.h>

#include "AccountLinkFlow.h"

#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace fulla::identity
{

namespace
{
constexpr auto kDiscoveryTtl = std::chrono::minutes(10);

std::string trimmed(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    return s;
}

bool parseJson(const std::string &body, Json::Value &out)
{
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream stream(body);
    return Json::parseFromStream(builder, stream, &out, &errs);
}

// Minimal application/x-www-form-urlencoded component escaping for URL
// building (authorize URL + nothing else; the token exchange goes through
// the transport seam as pre-encoded form fields).
std::string urlEscape(const std::string &value)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3 / 2);
    for (unsigned char c : value)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out += static_cast<char>(c);
        }
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}
}  // namespace

OidcUpstreamAuthService::OidcUpstreamAuthService(
  std::shared_ptr<ISocialAccountRepository> accountRepo,
  bool autoCreate,
  HttpGet httpGet,
  HttpPostForm httpPostForm)
    : accountRepo_(std::move(accountRepo)),
      autoCreate_(autoCreate),
      httpGet_(std::move(httpGet)),
      httpPostForm_(std::move(httpPostForm))
{
}

void OidcUpstreamAuthService::resolveEndpoints(
  const UpstreamProviderRecord &record,
  std::function<void(bool ok, OidcEndpoints endpoints)> &&cb)
{
    OidcEndpoints endpoints;
    // 1) endpoint_overrides: any of the three keys may be pinned; the rest
    //    still come from discovery.
    Json::Value overrides;
    if (!record.endpointOverrides.empty())
    {
        parseJson(record.endpointOverrides, overrides);
    }
    endpoints.authorize = overrides.get("authorize", "").asString();
    endpoints.token = overrides.get("token", "").asString();
    endpoints.userinfo = overrides.get("userinfo", "").asString();
    if (!endpoints.authorize.empty() && !endpoints.token.empty() && !endpoints.userinfo.empty())
    {
        cb(true, endpoints);
        return;
    }

    // 2) Discovery (cached per issuer).
    const std::string issuer = trimmed(record.issuer);
    {
        std::unique_lock<std::mutex> lock(discoveryMutex_);
        auto it = discoveryCache_.find(issuer);
        if (it != discoveryCache_.end() && it->second.valid &&
            std::chrono::steady_clock::now() - it->second.fetchedAt < kDiscoveryTtl)
        {
            const auto cached = it->second.endpoints;
            lock.unlock();
            if (endpoints.authorize.empty())
                endpoints.authorize = cached.authorize;
            if (endpoints.token.empty())
                endpoints.token = cached.token;
            if (endpoints.userinfo.empty())
                endpoints.userinfo = cached.userinfo;
            cb(!endpoints.authorize.empty() && !endpoints.token.empty() && !endpoints.userinfo.empty(),
               endpoints);
            return;
        }
    }

    auto self = shared_from_this();
    httpGet_(issuer + "/.well-known/openid-configuration", "",
             [self, issuer, endpoints, cb = std::move(cb)](bool ok, int status, std::string body) mutable {
                 OidcEndpoints merged = endpoints;
                 if (ok && status == 200)
                 {
                     Json::Value doc;
                     if (parseJson(body, doc) &&
                         doc.get("issuer", "").asString() == issuer)
                     {
                         OidcEndpoints discovered;
                         discovered.authorize = doc.get("authorization_endpoint", "").asString();
                         discovered.token = doc.get("token_endpoint", "").asString();
                         discovered.userinfo = doc.get("userinfo_endpoint", "").asString();
                         {
                             std::lock_guard<std::mutex> lock(self->discoveryMutex_);
                             CachedDiscovery &entry = self->discoveryCache_[issuer];
                             entry.endpoints = discovered;
                             entry.valid = !discovered.authorize.empty() &&
                                           !discovered.token.empty() &&
                                           !discovered.userinfo.empty();
                             entry.fetchedAt = std::chrono::steady_clock::now();
                         }
                         if (merged.authorize.empty())
                             merged.authorize = discovered.authorize;
                         if (merged.token.empty())
                             merged.token = discovered.token;
                         if (merged.userinfo.empty())
                             merged.userinfo = discovered.userinfo;
                     }
                 }
                 const bool complete = !merged.authorize.empty() && !merged.token.empty() &&
                                       !merged.userinfo.empty();
                 if (!complete)
                 {
                     LOG_WARN << "OidcUpstream: endpoint resolution failed for issuer " << issuer;
                 }
                 cb(complete, merged);
             });
}

std::string OidcUpstreamAuthService::buildAuthorizeUrl(
  const UpstreamProviderRecord &record,
  const OidcEndpoints &endpoints,
  const std::string &redirectUri,
  const std::string &state)
{
    if (endpoints.authorize.empty() || state.empty() || redirectUri.empty())
        return "";
    return endpoints.authorize + "?response_type=code&client_id=" + urlEscape(record.clientId) +
           "&redirect_uri=" + urlEscape(redirectUri) +
           "&scope=" + urlEscape(record.scopes.empty() ? "openid profile email" : record.scopes) +
           "&state=" + urlEscape(state);
}

bool OidcUpstreamAuthService::mapClaims(const std::string &claimsMappingJson,
                                        const std::string &userinfoBody,
                                        UpstreamIdentity &out)
{
    Json::Value userinfo;
    if (!parseJson(userinfoBody, userinfo))
        return false;

    // Spec defaults; an explicit mapping row overrides any of them.
    Json::Value mapping;
    if (!claimsMappingJson.empty())
    {
        parseJson(claimsMappingJson, mapping);  // invalid JSON falls back to defaults
    }
    const auto claim = [&](const char *slot, const char *defaultClaim) -> std::string {
        const std::string source =
          mapping.isMember(slot) ? mapping.get(slot, defaultClaim).asString() : defaultClaim;
        const Json::Value &value = userinfo.get(source, Json::Value());
        return value.isString() ? value.asString() : std::string();
    };

    out.subject = claim("sub", "sub");
    out.email = claim("email", "email");
    out.username = claim("username", "preferred_username");
    out.displayName = claim("name", "name");
    if (out.username.empty())
        out.username = out.subject;  // seed fallback; AccountLinkFlow suffixes collisions
    return !out.subject.empty();
}

void OidcUpstreamAuthService::login(const UpstreamProviderRecord &record,
                                    const OidcEndpoints &endpoints,
                                    const std::string &redirectUri,
                                    const std::string &code,
                                    std::function<void(LoginResult)> &&cb)
{
    auto sharedCb = std::make_shared<std::function<void(LoginResult)>>(std::move(cb));
    if (!accountRepo_ || endpoints.token.empty() || endpoints.userinfo.empty())
    {
        LoginResult result;
        result.errorCode = "INTERNAL_ERROR";
        (*sharedCb)(std::move(result));
        return;
    }

    auto self = shared_from_this();
    httpPostForm_(endpoints.token,
                  {{"grant_type", "authorization_code"},
                   {"code", code},
                   {"redirect_uri", redirectUri},
                   {"client_id", record.clientId},
                   {"client_secret", record.clientSecret}},
                  [self, record, endpoints, sharedCb](bool ok, int status, std::string body) mutable {
                      if (!ok || status != 200)
                      {
                          LOG_WARN << "OidcUpstream: token exchange failed (status="
                                   << (ok ? std::to_string(status) : std::string("transport")) << ")";
                          LoginResult result;
                          result.errorCode = "NET_CONNECTION_FAILED";
                          (*sharedCb)(std::move(result));
                          return;
                      }
                      Json::Value token;
                      if (!parseJson(body, token))
                      {
                          LoginResult result;
                          result.errorCode = "NET_CONNECTION_FAILED";
                          (*sharedCb)(std::move(result));
                          return;
                      }
                      const std::string accessToken = token.get("access_token", "").asString();
                      if (accessToken.empty())
                      {
                          LoginResult result;
                          result.errorCode = "NET_CONNECTION_FAILED";
                          (*sharedCb)(std::move(result));
                          return;
                      }
                      self->fetchUserinfo(record, endpoints, accessToken, sharedCb);
                  });
}

void OidcUpstreamAuthService::fetchUserinfo(
  const UpstreamProviderRecord &record,
  const OidcEndpoints &endpoints,
  const std::string &accessToken,
  std::shared_ptr<std::function<void(LoginResult)>> sharedCb)
{
    auto self = shared_from_this();
    httpGet_(endpoints.userinfo, accessToken,
             [self, record, sharedCb](bool ok, int status, std::string body) mutable {
                 if (!ok || status != 200)
                 {
                     LOG_WARN << "OidcUpstream: userinfo failed (status="
                              << (ok ? std::to_string(status) : std::string("transport")) << ")";
                     LoginResult result;
                     result.errorCode = "NET_CONNECTION_FAILED";
                     (*sharedCb)(std::move(result));
                     return;
                 }
                 UpstreamIdentity identity;
                 if (!mapClaims(record.claimsMapping, body, identity))
                 {
                     LoginResult result;
                     result.errorCode = "VALIDATION_INVALID_INPUT";
                     (*sharedCb)(std::move(result));
                     return;
                 }
                 social_detail::resolveOrCreateAccount(
                   self->accountRepo_,
                   self->autoCreate_,
                   record.providerKey,
                   identity.subject,
                   identity.username,
                   identity.email,
                   [sharedCb](int32_t userId, const std::string &username, bool isNewUser,
                              const std::string &publicSub) {
                       LoginResult result;
                       result.userId = userId;
                       result.username = username;
                       result.isNewUser = isNewUser;
                       result.publicSub = publicSub;
                       (*sharedCb)(std::move(result));
                   },
                   [sharedCb](std::string errorCode) {
                       LoginResult result;
                       result.errorCode = std::move(errorCode);
                       (*sharedCb)(std::move(result));
                   });
             });
}

}  // namespace fulla::identity

#endif  // WITH_SOCIAL
