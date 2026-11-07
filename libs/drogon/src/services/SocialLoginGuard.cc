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
        const auto id = config_["external_auth"][provider]
                          .get(provider == "wechat" ? "appid" : "client_id", "").asString();
        providers_.emplace(provider, std::make_pair(id, redirect));
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
    if (providers_.count(provider) == 0)
        return "VALIDATION_RESOURCE_NOT_FOUND";
    if (!states_)
        return "INTERNAL_ERROR";
    return {};
}

identity::SocialLoginContext SocialLoginGuard::context(
  const std::string &provider, const std::string &binding) const
{
    const auto &config = providers_.at(provider);
    return {provider, config.first, config.second, states_->digest(binding), "login"};
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
          body["redirect_uri"] = self->providers_.at(provider).second;
          body["authorize_url"] = controllers::social_detail::buildSocialAuthorizeUrl(self->config_, provider, state);
          if (body["authorize_url"].asString().empty())
          {
              error(req, std::move(callback), "INTERNAL_ERROR");
              return;
          }
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
