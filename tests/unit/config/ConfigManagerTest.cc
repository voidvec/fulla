#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <filesystem>
#include <fulla/common/config/ConfigManager.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>

#ifdef _WIN32
#define setenv(name, value, overwrite) _putenv_s(name, value)
#define unsetenv(name) _putenv_s(name, "")
#endif

// ============================================================================
// Database-Agnostic Tests (Run in all storage modes)
// ============================================================================

DROGON_TEST(Unit_P0_ConfigManager_Legacy_LoadValidConfig)
{
    std::string configPath = "./config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../../config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../../../config.json";

    Json::Value config;
    CHECK(fulla::common::config::ConfigManager::load(configPath, config) == true);
    CHECK(config.isNull() == false);
    CHECK(config.isMember("db_clients") == true);
}

DROGON_TEST(Unit_P0_ConfigManager_Legacy_TypeSafeAccessWithDefault)
{
    Json::Value config;
    config["port"] = 8080;

    auto port = fulla::common::config::ConfigManager::get<int>(config, "port", 0);
    CHECK(port == 8080);

    auto missing = fulla::common::config::ConfigManager::get<int>(config, "missing", 123);
    CHECK(missing == 123);
}

DROGON_TEST(Unit_P0_ConfigManager_Legacy_ValidateMissingRequiredField)
{
    Json::Value config;
    // Test completely missing db_clients field (not even empty array)
    // Empty arrays are valid for memory storage mode

    std::string errMsg;
    CHECK(fulla::common::config::ConfigManager::validate(config, errMsg) == false);
    CHECK(errMsg.find("db_clients") != std::string::npos);
}

DROGON_TEST(Unit_P0_ConfigManager_Legacy_ValidatePortRange)
{
    Json::Value config;
    config["db_clients"][0]["port"] = 70000;  // Invalid port
    config["redis_clients"][0]["port"] = 65535;

    std::string errMsg;
    CHECK(fulla::common::config::ConfigManager::validate(config, errMsg) == false);
    CHECK(errMsg.find("port") != std::string::npos);
}

// ============================================================================
// Database-Dependent Tests (Skipped in memory storage mode)
// ============================================================================

DROGON_TEST(Unit_P0_ConfigManager_Legacy_Database_EnvOverrideDbHost)
{
    // Skip this test in memory storage mode (no db_clients configured)
    auto plugin = drogon::app().getPlugin<OAuth2Plugin>();
    if (plugin && plugin->getStorageType() == "memory")
    {
        return;
    }

    // Set environment variable
    setenv("FULLA_DB_HOST", "test-host", 1);

    std::string configPath = "./config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../../config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../../../config.json";

    Json::Value config;
    CHECK(fulla::common::config::ConfigManager::load(configPath, config) == true);

    auto dbHost =
      fulla::common::config::ConfigManager::get<std::string>(config, "db_clients.0.host");
    CHECK(dbHost == "test-host");

    unsetenv("FULLA_DB_HOST");
}

// CORS allow_origins is a JSON array consumer (main.cc checks isArray()).
// The env override must split the comma-separated string into a real array,
// not clobber the node into a scalar string.
DROGON_TEST(Unit_P0_ConfigManager_EnvOverride_CorsArray)
{
    setenv("FULLA_CORS_ALLOW_ORIGINS", "https://a.com, https://b.com", 1);

    std::string configPath = "./config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../../config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../../../config.json";

    Json::Value config;
    CHECK(fulla::common::config::ConfigManager::load(configPath, config) == true);

    const Json::Value &origins = config["custom_config"]["cors"]["allow_origins"];
    CHECK(origins.isArray() == true);
    CHECK(origins.size() == 2);
    CHECK(origins[0].asString() == "https://a.com");
    CHECK(origins[1].asString() == "https://b.com");

    unsetenv("FULLA_CORS_ALLOW_ORIGINS");
}

// Google redirect_uri is a scalar string consumer (GoogleController.cc).
DROGON_TEST(Unit_P0_ConfigManager_EnvOverride_GoogleRedirect)
{
    setenv("FULLA_GOOGLE_REDIRECT_URI", "https://prod.example.com/callback", 1);

    std::string configPath = "./config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../../config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../../../config.json";

    Json::Value config;
    CHECK(fulla::common::config::ConfigManager::load(configPath, config) == true);

    auto redirect = fulla::common::config::ConfigManager::get<std::string>(
      config, "custom_config.external_auth.google.redirect_uri"
    );
    CHECK(redirect == "https://prod.example.com/callback");

    unsetenv("FULLA_GOOGLE_REDIRECT_URI");
}

// Verifies the "[name=OAuth2Plugin]" named-element lookup decouples the
// FULLA_VUE_REDIRECT_URI override from plugin array ordering. Each config
// file inserts different plugins (Hodor/AccessLogger) so a numeric index would
// point at the wrong element; the named lookup must resolve regardless.
DROGON_TEST(Unit_P0_ConfigManager_EnvOverride_VueRedirect_ByNameLookup)
{
    setenv("FULLA_VUE_REDIRECT_URI", "https://prod.example.com/callback", 1);

    std::string configPath = "./config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../../config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../../../config.json";

    Json::Value config;
    CHECK(fulla::common::config::ConfigManager::load(configPath, config) == true);

    auto redirect = fulla::common::config::ConfigManager::get<std::string>(
      config, "plugins[name=OAuth2Plugin].config.clients.fulla-portal.redirect_uri"
    );
    CHECK(redirect == "https://prod.example.com/callback");

    unsetenv("FULLA_VUE_REDIRECT_URI");
}

// Companion to the redirect_uri case above: FULLA_VUE_CLIENT_SECRET must land on
// the same plugin client entry. Regression guard for the override having been
// declared against a non-existent top-level "vue_client.secret" path, which made
// ConfigManager::applyEnvOverrides a silent no-op.
DROGON_TEST(Unit_P0_ConfigManager_EnvOverride_VueClientSecret_ByNameLookup)
{
    setenv("FULLA_VUE_CLIENT_SECRET", "prod-strong-vue-secret", 1);

    std::string configPath = "./config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../../config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../../../config.json";

    Json::Value config;
    CHECK(fulla::common::config::ConfigManager::load(configPath, config) == true);

    auto secret = fulla::common::config::ConfigManager::get<std::string>(
      config, "plugins[name=OAuth2Plugin].config.clients.fulla-portal.secret"
    );
    CHECK(secret == "prod-strong-vue-secret");

    unsetenv("FULLA_VUE_CLIENT_SECRET");
}


// ============================================================================
// #102: production-mode signing-key and weak-secret gates
// ============================================================================

namespace
{
// Passes every PRE-EXISTING production check (HTTPS issuer, non-default DB /
// Redis passwords) so the #102 gates are the only thing under test.
Json::Value productionBaseConfig()
{
    Json::Value config;
    Json::Value db;
    db["port"] = 5432;
    db["passwd"] = "strong-db-password";
    config["db_clients"].append(db);
    Json::Value redis;
    redis["port"] = 6379;
    redis["passwd"] = "strong-redis-password";
    config["redis_clients"].append(redis);
    config["custom_config"]["metadata"]["issuer"] = "https://auth.example.test";
    return config;
}

// RAII: force-set FULLA_ENV and clear both signing-key env vars for the test
// body, restoring whatever the host had afterwards.
class ProductionEnvGuard
{
  public:
    ProductionEnvGuard()
    {
        setenv("FULLA_ENV", "production", 1);
        // Empty value un-sets on Windows (_putenv_s removes the variable) and
        // reads as "not set" (len 0) elsewhere in this codebase.
        setenv("FULLA_SIGNING_KEY", "", 1);
        setenv("FULLA_JWT_KEY_PATH", "", 1);
    }
    ~ProductionEnvGuard()
    {
        unsetenv("FULLA_ENV");
        unsetenv("FULLA_SIGNING_KEY");
        unsetenv("FULLA_JWT_KEY_PATH");
    }
};
}  // namespace

DROGON_TEST(Unit_P0_ConfigManager_Production_NoSigningKey_Fails)
{
    ProductionEnvGuard env;
    Json::Value config = productionBaseConfig();
    std::string errMsg;
    CHECK(fulla::common::config::ConfigManager::validate(config, errMsg) == false);
    CHECK(errMsg.find("signing key") != std::string::npos);
}

DROGON_TEST(Unit_P0_ConfigManager_Production_SigningKeyEnv_Passes)
{
    ProductionEnvGuard env;
    setenv("FULLA_SIGNING_KEY", "-----BEGIN RSA PRIVATE KEY-----...", 1);
    Json::Value config = productionBaseConfig();
    std::string errMsg;
    CHECK(fulla::common::config::ConfigManager::validate(config, errMsg) == true);
}

DROGON_TEST(Unit_P0_ConfigManager_Production_ConfigOidcKeyPath_Passes)
{
    ProductionEnvGuard env;
    Json::Value config = productionBaseConfig();
    Json::Value plugin;
    plugin["name"] = "OAuth2Plugin";
    plugin["config"]["oidc"]["signing_key_path"] = "/etc/fulla/keys/signing.pem";
    config["plugins"].append(plugin);
    std::string errMsg;
    CHECK(fulla::common::config::ConfigManager::validate(config, errMsg) == true);
}

// #110-B (PR #176 review MINOR): the keystore directory is a complete key
// source on its own -- rotation deployments use nothing else, so the
// production gate must accept it (a misconfigured keystore then hard-fails
// later in JwkManager::init with the directory-specific error, which is the
// intended diagnostics point).
DROGON_TEST(Unit_P0_ConfigManager_Production_ConfigOidcKeystoreDir_Passes)
{
    ProductionEnvGuard env;
    Json::Value config = productionBaseConfig();
    Json::Value plugin;
    plugin["name"] = "OAuth2Plugin";
    plugin["config"]["oidc"]["signing_keystore_dir"] = "/etc/fulla/keystore";
    config["plugins"].append(plugin);
    std::string errMsg;
    CHECK(fulla::common::config::ConfigManager::validate(config, errMsg) == true);
}

DROGON_TEST(Unit_P0_ConfigManager_Production_WeakConfidentialClientSecret_Fails)
{
    ProductionEnvGuard env;
    setenv("FULLA_SIGNING_KEY", "-----BEGIN RSA PRIVATE KEY-----...", 1);
    Json::Value config = productionBaseConfig();
    Json::Value plugin;
    plugin["name"] = "OAuth2Plugin";
    plugin["config"]["oidc"]["signing_key_path"] = "/etc/fulla/keys/signing.pem";
    plugin["config"]["clients"]["backend-svc"]["client_type"] = "CONFIDENTIAL";
    plugin["config"]["clients"]["backend-svc"]["secret"] = "123456";
    config["plugins"].append(plugin);
    std::string errMsg;
    CHECK(fulla::common::config::ConfigManager::validate(config, errMsg) == false);
    CHECK(errMsg.find("backend-svc") != std::string::npos);
}

DROGON_TEST(Unit_P0_ConfigManager_Production_PublicClientSecretNotEnforced)
{
    ProductionEnvGuard env;
    setenv("FULLA_SIGNING_KEY", "-----BEGIN RSA PRIVATE KEY-----...", 1);
    Json::Value config = productionBaseConfig();
    Json::Value plugin;
    plugin["name"] = "OAuth2Plugin";
    plugin["config"]["oidc"]["signing_key_path"] = "/etc/fulla/keys/signing.pem";
    // PUBLIC clients authenticate via PKCE, not the secret: the placeholder
    // must not brick a production boot (OAuth2Plugin's DB scan skips PUBLIC
    // rows for the same reason).
    plugin["config"]["clients"]["fulla-portal"]["client_type"] = "PUBLIC";
    plugin["config"]["clients"]["fulla-portal"]["secret"] = "123456";
    config["plugins"].append(plugin);
    std::string errMsg;
    CHECK(fulla::common::config::ConfigManager::validate(config, errMsg) == true);
}
