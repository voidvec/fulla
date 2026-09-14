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

}  // namespace fulla::drogon::utils
