#include <fulla/drogon/services/SocialLoginGuard.h>

#ifdef WITH_SOCIAL
#include <drogon/drogon.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/common/utils/SocialAuthConfig.h>
#include "../controllers/SocialAuthorizeUrl.h"
#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace fulla::drogon::services
{
namespace
{
using StateService = identity::SocialLoginStateService;
using Status = identity::SocialLoginStateStatus;

std::pair<std::string, std::string> originAndPath(const std::string &uri)
{
    return common::utils::socialOriginAndPath(uri);
}

bool loopbackOrigin(const std::string &origin)
{
    static const std::regex pattern(R"(^http://(localhost|127\.0\.0\.1|\[::1\])(:[0-9]+)?$)");
    return std::regex_match(origin, pattern);
}

bool decodeForm(const std::string &encoded, std::string &decoded)
{
    for (size_t i = 0; i < encoded.size(); ++i)
    {
        if (encoded[i] != '%')
            continue;
        if (i + 2 >= encoded.size() || !std::isxdigit(static_cast<unsigned char>(encoded[i + 1])) ||
            !std::isxdigit(static_cast<unsigned char>(encoded[i + 2])))
            return false;
        i += 2;
    }
    decoded = ::drogon::utils::urlDecode(encoded);
    return true;
}

std::string parseBody(const ::drogon::HttpRequestPtr &req, std::string &code, std::string &state)
{
    // Never use getParameter(): it merges query and body parameters.
    const auto query = req->query();
    if (!query.empty())
        return "VALIDATION_INVALID_INPUT";
    auto type = req->getHeader("content-type");
    type = type.substr(0, type.find(';'));
    while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back()))) type.pop_back();
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (type == "application/json")
    {
        Json::CharReaderBuilder reader;
        reader["rejectDupKeys"] = true;
        reader["failIfExtra"] = true;
        reader["allowComments"] = false;
        reader["allowTrailingCommas"] = false;
        Json::Value body;
        const auto raw = req->body();
        std::istringstream input(std::string(raw.data(), raw.size()));
        if (!Json::parseFromStream(reader, input, &body, nullptr) || !body.isObject())
            return "VALIDATION_INVALID_INPUT";
        if (!body.isMember("code") || !body.isMember("state"))
            return "VALIDATION_MISSING_REQUIRED_FIELD";
        if (!body["code"].isString() || !body["state"].isString())
            return "VALIDATION_INVALID_INPUT";
        code = body["code"].asString();
        state = body["state"].asString();
    }
    else if (type == "application/x-www-form-urlencoded")
    {
        const auto raw = req->body();
        std::istringstream input(std::string(raw.data(), raw.size()));
        std::string field;
        std::set<std::string> seen;
        while (std::getline(input, field, '&'))
        {
            const auto equals = field.find('=');
            std::string name, value;
            if (!decodeForm(field.substr(0, equals), name) ||
                !decodeForm(equals == std::string::npos ? "" : field.substr(equals + 1), value))
                return "VALIDATION_INVALID_INPUT";
            if (name != "code" && name != "state")
                continue;
            if (!seen.insert(name).second)
                return "VALIDATION_INVALID_INPUT";
            (name == "code" ? code : state) = value;
        }
    }
    else
        return "VALIDATION_INVALID_INPUT";
    if (code.empty() || state.empty())
        return "VALIDATION_MISSING_REQUIRED_FIELD";
    return StateService::validHandle(state) ? "" : "VALIDATION_INVALID_INPUT";
}
}  // namespace

SocialLoginGuard::SocialLoginGuard(std::shared_ptr<identity::SocialLoginStateService> states,
  Json::Value config, const std::vector<std::string> &providers, bool allowInsecureLoopback)
    : states_(std::move(states)), config_(std::move(config))
{
    const auto frontend = config_["frontend"].get("url", "http://localhost:5173").asString();
    const auto frontendOrigin = originAndPath(frontend).first;
    secure_ = !(allowInsecureLoopback && loopbackOrigin(frontendOrigin));
    if (!secure_)
        cookieName_ = "fulla-social-binding-dev";
    for (const auto &origin : config_["cors"]["allow_origins"])
        if (origin.isString() && origin.asString() != "*")
            origins_.insert(origin.asString());
    std::set<std::pair<std::string, std::string>> callbacks;
    for (const auto &provider : providers)
    {
        const auto redirect = controllers::social_detail::socialRedirectUri(config_, provider);
        if (!callbacks.insert(originAndPath(redirect)).second)
            throw std::invalid_argument("Enabled social providers require distinct callback paths");
        const auto &block = config_["external_auth"][provider];
        const auto id = block.get(provider == "wechat" ? "appid" : "client_id", "").asString();
        // P2-1c: the secret participates in authorize-URL building (the
        // not-configured gate) and becomes hot-updatable from the registry.
        const auto secret =
          block.get(provider == "wechat" ? "secret" : "client_secret", "").asString();
        providers_.emplace(provider, StaticEntry{id, secret, redirect});
    }
}

SocialLoginGuard::ResponseCallback SocialLoginGuard::noStore(ResponseCallback &&callback)
{
    return [callback = std::move(callback)](const ::drogon::HttpResponsePtr &response) {
        response->addHeader("Cache-Control", "no-store");
        response->addHeader("Pragma", "no-cache");
        callback(response);
    };
}

void SocialLoginGuard::error(const ::drogon::HttpRequestPtr &req, ResponseCallback &&callback,
                            const std::string &code)
{
    common::error::ErrorResponder::respond(req, noStore(std::move(callback)), code,
                                          "Social login request rejected");
}

std::string SocialLoginGuard::requestError(const ::drogon::HttpRequestPtr &req,
                             const std::string &provider) const
{
    const auto &origin = req->getHeader("origin");
    if (origin.empty() || origins_.count(origin) == 0 ||
        req->getHeader("sec-fetch-site") == "cross-site")
        return "AUTHZ_ACCESS_DENIED";
    {
        std::lock_guard<std::mutex> lock(staticMutex_);
        if (providers_.count(provider) == 0 && !findDynamic(provider))
            return "VALIDATION_RESOURCE_NOT_FOUND";
    }
    if (!states_)
        return "INTERNAL_ERROR";
    return {};
}

identity::SocialLoginContext SocialLoginGuard::context(
  const std::string &provider, const std::string &binding) const
{
    {
        std::lock_guard<std::mutex> lock(staticMutex_);
        auto it = providers_.find(provider);
        if (it != providers_.end())
        {
            return {provider, it->second.clientId, it->second.redirectUri,
                    states_->digest(binding), "login"};
        }
    }
    // P2-1 registry upstream: same context shape (provider, client_id,
    // redirect_uri, binding digest) so the state service and consume-time
    // verification are identical for config-static and registry providers.
    const auto dynamic = findDynamic(provider);
    if (dynamic)
    {
        return {provider, dynamic->clientId, dynamic->redirectUri,
                states_->digest(binding), "login"};
    }
    return {provider, "", "", states_->digest(binding), "login"};
}

void SocialLoginGuard::updateStaticProvider(
  const std::string &provider, const std::string &clientId,
  const std::string &clientSecret, bool enabled)
{
    std::lock_guard<std::mutex> lock(staticMutex_);
    auto it = providers_.find(provider);
    if (!enabled)
    {
        if (it != providers_.end())
            providers_.erase(it);
        return;
    }
    if (it == providers_.end())
    {
        // A registry row may ENABLE a provider config never configured
        // (the P2-1c promise: DB-driven). The redirect follows the shared
        // per-key convention (external_auth.{key}.redirect_uri override,
        // else frontend + /callback/{key}); distinct-callback uniqueness
        // holds by construction of that convention.
        StaticEntry entry;
        entry.clientId = clientId;
        entry.clientSecret = clientSecret;
        entry.redirectUri = controllers::social_detail::socialRedirectUri(config_, provider);
        providers_.emplace(provider, std::move(entry));
        return;
    }
    it->second.clientId = clientId;
    it->second.clientSecret = clientSecret;
}

void SocialLoginGuard::setDynamicProviders(std::vector<DynamicProvider> providers)
{
    std::lock_guard<std::mutex> lock(dynamicMutex_);
    dynamicProviders_ = std::move(providers);
}

std::vector<SocialLoginGuard::DynamicProvider> SocialLoginGuard::dynamicProviders() const
{
    std::lock_guard<std::mutex> lock(dynamicMutex_);
    return dynamicProviders_;
}

std::optional<SocialLoginGuard::DynamicProvider> SocialLoginGuard::findDynamic(
  const std::string &provider) const
{
    std::lock_guard<std::mutex> lock(dynamicMutex_);
    for (const auto &entry : dynamicProviders_)
    {
        if (entry.key == provider)
            return entry;
    }
    return std::nullopt;
}

void SocialLoginGuard::setBinding(const ::drogon::HttpResponsePtr &response,
                                 const std::string &binding, int maxAge) const
{
    ::drogon::Cookie cookie(cookieName_, binding);
    cookie.setPath("/");
    cookie.setSecure(secure_);
    cookie.setHttpOnly(true);
    cookie.setSameSite(::drogon::Cookie::SameSite::kLax);
    cookie.setMaxAge(maxAge);
    response->addCookie(cookie);
}

void SocialLoginGuard::authorize(const ::drogon::HttpRequestPtr &req,
  const std::string &provider, ResponseCallback &&callback)
{
    if (const auto failure = requestError(req, provider); !failure.empty())
    {
        error(req, std::move(callback), failure);
        return;
    }
    auto binding = req->getCookie(cookieName_);
    if (binding.empty() && req->getCookies().count(cookieName_) == 0)
    {
        const auto fresh = states_->newBinding();
        if (!fresh)
        {
            error(req, std::move(callback), "INTERNAL_ERROR");
            return;
        }
        auto response = ::drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(::drogon::k204NoContent);
        setBinding(response, *fresh);
        noStore(std::move(callback))(response);
        return;
    }
    if (!StateService::validHandle(binding))
    {
        auto wrapped = [self = shared_from_this(), callback = std::move(callback)](const auto &response) {
            self->setBinding(response, "", 0);
            callback(response);
        };
        error(req, std::move(wrapped), "VALIDATION_INVALID_INPUT");
        return;
    }
    auto self = shared_from_this();
    states_->issue(context(provider, binding),
      [self, req, provider, binding, callback = std::move(callback)](Status status, const std::string &state) mutable {
          if (status != Status::Ok)
          {
              error(req, std::move(callback), "INTERNAL_ERROR");
              return;
          }
          Json::Value body;
          body["state"] = state;
          std::string authorizeUrl;
          // P2-1c: static entries are runtime-mutable (registry rows drive
          // the three legacy social keys) -- snapshot under the lock and
          // build the URL from the CURRENT credentials, byte-identical to
          // the config-based shapes.
          std::optional<StaticEntry> staticEntry;
          {
              std::lock_guard<std::mutex> lock(self->staticMutex_);
              auto it = self->providers_.find(provider);
              if (it != self->providers_.end())
                  staticEntry = it->second;
          }
          if (staticEntry)
          {
              body["redirect_uri"] = staticEntry->redirectUri;
              authorizeUrl = controllers::social_detail::buildSocialAuthorizeUrlWithCredentials(
                provider, staticEntry->clientId, staticEntry->clientSecret,
                staticEntry->redirectUri, state);
          }
          else
          {
              // P2-1 registry upstream: build from the pre-resolved
              // endpoint snapshot (no I/O on this path).
              const auto dynamic = self->findDynamic(provider);
              if (dynamic)
              {
                  body["redirect_uri"] = dynamic->redirectUri;
                  authorizeUrl = dynamic->authorizeEndpoint +
                                 "?response_type=code&client_id=" +
                                 ::drogon::utils::urlEncodeComponent(dynamic->clientId) +
                                 "&redirect_uri=" +
                                 ::drogon::utils::urlEncodeComponent(dynamic->redirectUri) +
                                 "&scope=" +
                                 ::drogon::utils::urlEncodeComponent(
                                   dynamic->scopes.empty() ? "openid profile email"
                                                           : dynamic->scopes) +
                                 "&state=" + ::drogon::utils::urlEncodeComponent(state);
              }
          }
          if (authorizeUrl.empty())
          {
              error(req, std::move(callback), "INTERNAL_ERROR");
              return;
          }
          body["authorize_url"] = authorizeUrl;
          body["expires_in"] = 600;
          auto response = ::drogon::HttpResponse::newHttpJsonResponse(body);
          self->setBinding(response, binding);
          noStore(std::move(callback))(response);
      });
}

void SocialLoginGuard::validate(const ::drogon::HttpRequestPtr &req,
  const std::string &provider, ValidationCallback &&callback)
{
    if (const auto failure = requestError(req, provider); !failure.empty())
    {
        callback("", failure);
        return;
    }
    std::string code, state;
    if (const auto failure = parseBody(req, code, state); !failure.empty())
    {
        callback("", failure);
        return;
    }
    const auto binding = req->getCookie(cookieName_);
    if (!StateService::validHandle(binding))
    {
        callback("", "VALIDATION_INVALID_INPUT");
        return;
    }
    const auto expected = context(provider, binding);
    if (expected.bindingHash.empty())
    {
        callback("", "INTERNAL_ERROR");
        return;
    }
    states_->consume(state, expected,
      [code, callback = std::move(callback)](Status status) {
          callback(status == Status::Ok ? code : "", status == Status::Ok ? "" :
                   (status == Status::Unavailable ? "INTERNAL_ERROR" : "VALIDATION_INVALID_INPUT"));
      });
}

}  // namespace fulla::drogon::services
#endif
