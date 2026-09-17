#pragma once

#include <string>
#include <vector>
#include <json/json.h>

namespace fulla::common::config
{

// Environment variable override configuration
struct EnvOverride
{
    std::string configPath;     // JSON path like "db_clients.0.host"
    const char *envVar;         // Environment variable name
    bool isNumeric;             // Is numeric type
    bool isStringList = false;  // Comma-separated string → JSON array of strings
};

// OAuth2 environment variable override rules
inline const std::vector<EnvOverride> FULLA_ENV_OVERRIDES =
  {{"db_clients.0.host", "FULLA_DB_HOST", false},
   {"db_clients.0.port", "FULLA_DB_PORT", true},
   {"db_clients.0.dbname", "FULLA_DB_NAME", false},
   {"db_clients.0.user", "FULLA_DB_USER", false},
   {"db_clients.0.passwd", "FULLA_DB_PASSWORD", false},
   {"redis_clients.0.host", "FULLA_REDIS_HOST", false},
   {"redis_clients.0.port", "FULLA_REDIS_PORT", true},
   {"redis_clients.0.passwd", "FULLA_REDIS_PASSWORD", false},
   {"custom_config.metadata.issuer", "FULLA_ISSUER", false},
   {"custom_config.frontend.url", "FULLA_FRONTEND_URL", false},
   {"custom_config.external_auth.github.client_id", "FULLA_GITHUB_CLIENT_ID", false},
   {"custom_config.external_auth.github.client_secret", "FULLA_GITHUB_CLIENT_SECRET", false},
   {"custom_config.external_auth.google.client_id", "FULLA_GOOGLE_CLIENT_ID", false},
   {"custom_config.external_auth.google.client_secret", "FULLA_GOOGLE_CLIENT_SECRET", false},
   {"custom_config.external_auth.google.redirect_uri", "FULLA_GOOGLE_REDIRECT_URI", false},
   {"custom_config.external_auth.wechat.appid", "FULLA_WECHAT_APPID", false},
   {"custom_config.external_auth.wechat.secret", "FULLA_WECHAT_SECRET", false},
   {"listeners.0.port", "FULLA_LISTEN_PORT", true},
   // "[name=OAuth2Plugin]" resolves the plugin by its drogon "name" field,
   // independent of array ordering — each config file inserts a different set
   // of plugins (Hodor, AccessLogger) so a numeric index would be fragile.
   // The startup client seeder (bootstrap/ClientSeeder) upserts these config
   // client entries into oauth2_clients.
   {"custom_config.mfa.totp_issuer", "FULLA_MFA_TOTP_ISSUER", false},
   {"custom_config.webauthn.rp_id", "FULLA_WEBAUTHN_RP_ID", false},
   {"custom_config.webauthn.rp_name", "FULLA_WEBAUTHN_RP_NAME", false},
   {"custom_config.webauthn.rp_origins", "FULLA_WEBAUTHN_RP_ORIGINS", false, /*isStringList=*/true},
   // v1.4.0 open platform switches (fail-closed; the block exists in every
   // shipped config so these overrides actually land — v1.3.2 lesson).
   {"custom_config.open_platform.enabled", "FULLA_OPEN_PLATFORM_ENABLED", true},
   {"custom_config.open_platform.require_org", "FULLA_OPEN_PLATFORM_REQUIRE_ORG", true},
   {"custom_config.open_platform.max_apps_per_user", "FULLA_OPEN_PLATFORM_MAX_APPS_PER_USER", true},
   {"custom_config.open_platform.max_org_apps", "FULLA_OPEN_PLATFORM_MAX_ORG_APPS", true},
   {"custom_config.open_platform.max_orgs_per_user", "FULLA_OPEN_PLATFORM_MAX_ORGS_PER_USER", true},
   {"custom_config.open_platform.max_redirect_uris", "FULLA_OPEN_PLATFORM_MAX_REDIRECT_URIS", true},
   {"custom_config.open_platform.creation_rate_limit_per_day",
    "FULLA_OPEN_PLATFORM_CREATION_RATE_PER_DAY",
    true},
   {"plugins[name=OAuth2Plugin].config.clients.fulla-portal.secret",
    "FULLA_PORTAL_CLIENT_SECRET",
    false},
   {"plugins[name=OAuth2Plugin].config.clients.fulla-portal.redirect_uri",
    "FULLA_PORTAL_REDIRECT_URI",
    false},
   {"plugins[name=OAuth2Plugin].config.clients.fulla-admin-console.redirect_uri",
    "FULLA_ADMIN_CONSOLE_REDIRECT_URI",
    false},
   // Deprecated pre-1.3.0 names (vue-client era) — kept as aliases so an
   // upgraded deployment's env file keeps working until it migrates.
   {"plugins[name=OAuth2Plugin].config.clients.fulla-portal.secret",
    "FULLA_VUE_CLIENT_SECRET",
    false},
   {"plugins[name=OAuth2Plugin].config.clients.fulla-portal.redirect_uri",
    "FULLA_VUE_REDIRECT_URI",
    false},
   {"custom_config.cors.allow_origins", "FULLA_CORS_ALLOW_ORIGINS", false, /*isStringList=*/true}};

}  // namespace fulla::common::config
