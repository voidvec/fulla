#pragma once

// v1.4.0 open platform + org self-service configuration (custom_config
// "open_platform" block; env-overridable via the ConfigTypes.h rows).
//
// v1.3.2 lesson (WebAuthn #142): env overrides only land on config paths
// that EXIST, so every shipped config file carries the block with these
// defaults; operators flip behavior via config or env, never by adding
// missing keys.
//
// Defaults are fail-closed (same手法 as WebAuthn): self-service org/app
// creation is OFF until the operator enables it; quotas are conservative.

#include <drogon/drogon.h>

namespace openplatform
{

struct OpenPlatformConfig
{
    bool enabled = false;        // master switch for self-service app creation
    bool requireOrg = false;     // true = only org owner/admins may create apps
    int maxAppsPerUser = 5;      // personal (org_id IS NULL) apps per creator
    int maxOrgApps = 10;         // apps per organization
    int maxOrgsPerUser = 3;      // orgs a user may own
    int maxRedirectUris = 20;    // per app
    int creationRatePerDay = 5;  // app creations per creator per UTC day

    static OpenPlatformConfig load()
    {
        OpenPlatformConfig cfg;
        const Json::Value &root = ::drogon::app().getCustomConfig();
        if (!root.isMember("open_platform") || !root["open_platform"].isObject())
        {
            return cfg;  // defaults (fail-closed)
        }
        const Json::Value &json = root["open_platform"];
        // get(key, default) is tolerant of missing/other-type members.
        cfg.enabled = json.get("enabled", cfg.enabled).asBool();
        cfg.requireOrg = json.get("require_org", cfg.requireOrg).asBool();
        cfg.maxAppsPerUser = json.get("max_apps_per_user", cfg.maxAppsPerUser).asInt();
        cfg.maxOrgApps = json.get("max_org_apps", cfg.maxOrgApps).asInt();
        cfg.maxOrgsPerUser = json.get("max_orgs_per_user", cfg.maxOrgsPerUser).asInt();
        cfg.maxRedirectUris = json.get("max_redirect_uris", cfg.maxRedirectUris).asInt();
        cfg.creationRatePerDay =
          json.get("creation_rate_limit_per_day", cfg.creationRatePerDay).asInt();
        return cfg;
    }
};

}  // namespace openplatform
