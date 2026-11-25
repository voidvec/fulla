#pragma once

#include <drogon/drogon.h>

#include <string>

// PR #157 review MAJOR 4: the must_change_password redirect target must
// follow the ORIGINATING portal. Hardcoding custom_config.frontend.url sent
// the bootstrap admin (the #145 flagship user) from the admin console to the
// user portal's login page, which is a different origin and (before the
// companion fix) had no query branch.
//
// Resolution rule:
//   * client_id listed in custom_config.admin_console.client_ids (default:
//     the seeded "fulla-admin-console" client) -> {admin_console.url}/admin/login
//   * everything else -> {frontend.url}/login
// Both origins default to the dev vite servers (5174 admin / 5173 user).
namespace fulla::drogon::utils
{

inline std::string mustChangePasswordRedirectUrl(const std::string &clientId)
{
    auto customConfig = ::drogon::app().getCustomConfig();

    bool isAdminConsole = (clientId == "fulla-admin-console");
    if (
      customConfig.isMember("admin_console") &&
      customConfig["admin_console"].isMember("client_ids") &&
      customConfig["admin_console"]["client_ids"].isArray()
    )
    {
        for (const auto &id : customConfig["admin_console"]["client_ids"])
        {
            if (id.asString() == clientId)
            {
                isAdminConsole = true;
                break;
            }
        }
    }

    std::string origin = "http://localhost:5173";
    std::string path = "/login";
    if (isAdminConsole)
    {
        origin = "http://localhost:5174";
        if (
          customConfig.isMember("admin_console") &&
          customConfig["admin_console"].isMember("url") &&
          customConfig["admin_console"]["url"].isString()
        )
        {
            origin = customConfig["admin_console"]["url"].asString();
        }
        path = "/admin/login";
    }
    else if (
      customConfig.isMember("frontend") && customConfig["frontend"].isMember("url") &&
      customConfig["frontend"]["url"].isString()
    )
    {
        origin = customConfig["frontend"]["url"].asString();
    }

    if (!origin.empty() && origin.back() == '/')
        origin.pop_back();
    return origin + path + "?must_change_password=1";
}

// The login and consent pages of an
// authorize flow may live on another origin than the backend -- the SPA
// behind its own port in a stack without a shared gateway, say -- where the
// backend-relative defaults cannot reach them. custom_config.oauth2.login_url
// and consent_url point the authorize endpoint at the right pages; both can
// be set from the environment with FULLA_OAUTH2_LOGIN_URL and
// FULLA_OAUTH2_CONSENT_URL.
//
// The defaults are unchanged, because running code depends on them: the
// backend's own "/login" form (the embedded host example and the browser
// demo drive it) and the relative "/consent" (the capacity harness checks
// the consent redirect against the configured consent_url). The backend
// serves no GET /consent, so a deployment without a same-origin SPA has to
// set consent_url.
//
// Non-string and EMPTY values count as "not set" rather than being thrown
// from asString(): every shipped config carries both keys as "" so that the
// environment overrides land (an override only lands on an existing key).
inline std::string authorizeLoginUrl()
{
    auto customConfig = ::drogon::app().getCustomConfig();
    if (
      customConfig.isMember("oauth2") && customConfig["oauth2"].isMember("login_url") &&
      customConfig["oauth2"]["login_url"].isString() &&
      !customConfig["oauth2"]["login_url"].asString().empty()
    )
    {
        return customConfig["oauth2"]["login_url"].asString();
    }
    return "/login";
}

inline std::string authorizeConsentUrl()
{
    auto customConfig = ::drogon::app().getCustomConfig();
    if (
      customConfig.isMember("oauth2") && customConfig["oauth2"].isMember("consent_url") &&
      customConfig["oauth2"]["consent_url"].isString() &&
      !customConfig["oauth2"]["consent_url"].asString().empty()
    )
    {
        return customConfig["oauth2"]["consent_url"].asString();
    }
    return "/consent";
}

// The resolved target may already carry a query string (an
// explicitly configured absolute oauth2.login_url, say). Callers appending
// the first authorize parameter must use this separator instead of a bare
// '?' so the redirect URL stays well-formed.
inline char authorizeUrlQuerySeparator(const std::string &target)
{
    return target.find('?') == std::string::npos ? '?' : '&';
}

}  // namespace fulla::drogon::utils
