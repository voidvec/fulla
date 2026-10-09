#pragma once

// Server-side provider authorize-URL builder, shared by two consumers:
//
// 1. The #71 social LINK flow (UserSelfServiceController's
//    /api/me/social/links/{provider}/authorize): state = the one-time
//    Redis-backed link-state token; the link-back POST must present it.
// 2. The v1.4.0 LOGIN discovery payload (AuthProvidersController,
//    GET /api/auth/providers): state = "" — the login flow is stateless
//    today (the SPA posts the provider code straight back), so no state
//    parameter is emitted.
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
#include <string>

namespace fulla::drogon::controllers::social_detail
{

inline std::string buildSocialAuthorizeUrl(const std::string &provider, const std::string &state)
{
    auto config = ::drogon::app().getCustomConfig();
    if (!config.isMember("external_auth"))
        return "";
    const auto &externalAuth = config["external_auth"];
    if (!externalAuth.isMember(provider) || !externalAuth[provider].isObject())
        return "";

    std::string frontendUrl = "http://localhost:5173";
    if (config.isMember("frontend") && config["frontend"].isMember("url"))
        frontendUrl = config["frontend"]["url"].asString();
    // Trim a trailing slash once so the fallback never doubles it.
    if (!frontendUrl.empty() && frontendUrl.back() == '/')
        frontendUrl.pop_back();

    const auto credentialConfigured = [](const std::string &v) {
        return !v.empty() && v.rfind("YOUR_", 0) != 0;
    };
    const std::string redirectUri = externalAuth[provider].get("redirect_uri", "").asString() !=
                                        ""
                                      ? externalAuth[provider].get("redirect_uri", "").asString()
                                      : frontendUrl + "/callback/" + provider;
    const std::string encodedRedirect = ::drogon::utils::urlEncode(redirectUri);
    const std::string encodedState = ::drogon::utils::urlEncode(state);
    // Link flow passes a one-time state (#71); the login discovery passes ""
    // and gets no state parameter at all (stateless login is the status quo).
    const std::string stateParam =
      state.empty() ? "" : "&state=" + encodedState;

    if (provider == "github")
    {
        const std::string clientId = externalAuth["github"].get("client_id", "").asString();
        const std::string clientSecret = externalAuth["github"].get("client_secret", "").asString();
        if (!credentialConfigured(clientId) || !credentialConfigured(clientSecret))
            return "";
        return "https://github.com/login/oauth/authorize?client_id=" + clientId +
               "&scope=user%3Aemail" + stateParam +
               "&redirect_uri=" + encodedRedirect;
    }
    if (provider == "google")
    {
        const std::string clientId = externalAuth["google"].get("client_id", "").asString();
        const std::string clientSecret =
          externalAuth["google"].get("client_secret", "").asString();
        if (!credentialConfigured(clientId) || !credentialConfigured(clientSecret))
            return "";
        return "https://accounts.google.com/o/oauth2/v2/auth?client_id=" + clientId +
               "&response_type=code&scope=openid%20email%20profile" + stateParam +
               "&redirect_uri=" + encodedRedirect;
    }
    if (provider == "wechat")
    {
        const std::string appId = externalAuth["wechat"].get("appid", "").asString();
        const std::string secret = externalAuth["wechat"].get("secret", "").asString();
        if (!credentialConfigured(appId) || !credentialConfigured(secret))
            return "";
        return "https://open.weixin.qq.com/connect/qrconnect?appid=" + appId +
               "&redirect_uri=" + encodedRedirect +
               "&response_type=code&scope=snsapi_login" + stateParam +
               "#wechat_redirect";
    }
    return "";
}

}  // namespace fulla::drogon::controllers::social_detail
