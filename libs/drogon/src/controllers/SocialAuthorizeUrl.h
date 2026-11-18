#pragma once

// Server-side provider authorize-URL builder, shared by two consumers:
//
// 1. The #71 social LINK flow (UserSelfServiceController's
//    /api/me/social/links/{provider}/authorize): state = the one-time
//    Redis-backed link-state token; the link-back POST must present it.
// 2. The browser-bound LOGIN issuance endpoint. Both callers must provide
//    a one-time transaction state; discovery never returns an authorize URL.
//
// Client ids come from custom_config external_auth.{provider}; the redirect
// target is external_auth.{provider}.redirect_uri when set, else the
// frontend URL + /callback/{provider}. Returns "" for an unconfigured
// provider (every caller fails closed -- no stateless URL ever leaves the
// server).
//
// Internal (src-local) header on purpose: not part of the installed SDK
// surface, so the api-diff SemVer baseline is untouched.

#include <drogon/drogon.h>
#include <fulla/common/utils/SocialAuthConfig.h>
#include <string>

namespace fulla::drogon::controllers::social_detail
{

inline Json::Value socialLoginRequestBody()
{
    Json::Value schema;
    schema["type"] = "object";
    schema["required"].append("code");
    schema["required"].append("state");
    schema["properties"]["code"]["type"] = "string";
    schema["properties"]["code"]["minLength"] = 1;
    schema["properties"]["state"]["type"] = "string";
    schema["properties"]["state"]["pattern"] = "^[0-9a-f]{64}$";
    Json::Value body;
    body["required"] = true;
    body["content"]["application/json"]["schema"] = schema;
    body["content"]["application/x-www-form-urlencoded"]["schema"] = schema;
    return body;
}

inline std::string socialRedirectUri(const Json::Value &config, const std::string &provider)
{
    return common::utils::socialRedirectUri(config, provider);
}

// P2-1c: explicit-credential variants. The registry is the runtime source
// of truth for the three legacy providers (config only seeds it on first
// boot); these builders emit the EXACT same URL shapes as the config-based
// versions below — byte-for-byte, tests pin the param order/scope strings.
inline std::string buildSocialAuthorizeUrlWithCredentials(
  const std::string &provider, const std::string &clientId, const std::string &clientSecret,
  const std::string &redirectUri, const std::string &state)
{
    if (state.empty() || redirectUri.empty())
        return "";
    const auto credentialConfigured = [](const std::string &v) {
        return common::utils::socialCredentialConfigured(v);
    };
    if (!credentialConfigured(clientId) || !credentialConfigured(clientSecret))
        return "";
    const std::string encodedRedirect = ::drogon::utils::urlEncode(redirectUri);
    const std::string encodedState = ::drogon::utils::urlEncode(state);
    const std::string stateParam = "&state=" + encodedState;

    if (provider == "github")
    {
        return "https://github.com/login/oauth/authorize?client_id=" + ::drogon::utils::urlEncode(clientId) +
               "&scope=user%3Aemail" + stateParam +
               "&redirect_uri=" + encodedRedirect;
    }
    if (provider == "google")
    {
        return "https://accounts.google.com/o/oauth2/v2/auth?client_id=" + ::drogon::utils::urlEncode(clientId) +
               "&response_type=code&scope=openid%20email%20profile" + stateParam +
               "&redirect_uri=" + encodedRedirect;
    }
    if (provider == "wechat")
    {
        return "https://open.weixin.qq.com/connect/qrconnect?appid=" + ::drogon::utils::urlEncode(clientId) +
               "&redirect_uri=" + encodedRedirect +
               "&response_type=code&scope=snsapi_login" + stateParam +
               "#wechat_redirect";
    }
    return "";
}

inline std::string buildSocialAuthorizeUrl(const Json::Value &config,
                                          const std::string &provider, const std::string &state)
{
    if (state.empty())
        return "";
    const auto &externalAuth = config["external_auth"];
    if (!externalAuth.isMember(provider) || !externalAuth[provider].isObject())
        return "";
    const std::string redirectUri = socialRedirectUri(config, provider);

    if (provider == "wechat")
    {
        return buildSocialAuthorizeUrlWithCredentials(
          "wechat", externalAuth["wechat"].get("appid", "").asString(),
          externalAuth["wechat"].get("secret", "").asString(), redirectUri, state);
    }
    return buildSocialAuthorizeUrlWithCredentials(
      provider, externalAuth[provider].get("client_id", "").asString(),
      externalAuth[provider].get("client_secret", "").asString(), redirectUri, state);
}

inline std::string buildSocialAuthorizeUrl(const std::string &provider, const std::string &state)
{
    return buildSocialAuthorizeUrl(::drogon::app().getCustomConfig(), provider, state);
}

}  // namespace fulla::drogon::controllers::social_detail
