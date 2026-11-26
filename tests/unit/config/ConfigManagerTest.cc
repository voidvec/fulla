#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <filesystem>
#include <cstdlib>
#include <string>
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

// The authorize endpoint's login/consent redirect targets can be overridden
// from the environment. An override only lands on a key the config file
// already carries (getJsonPointer never creates the path), so every shipped
// config anchors both keys as empty strings.
DROGON_TEST(Unit_P1_ConfigManager_EnvOverride_Oauth2LoginAndConsentUrls)
{
    setenv("FULLA_OAUTH2_LOGIN_URL", "https://sso.example.com/sign-in", 1);
    setenv("FULLA_OAUTH2_CONSENT_URL", "https://sso.example.com/approve", 1);

    std::string configPath = "./config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../../config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../../../config.json";

    Json::Value config;
    CHECK(fulla::common::config::ConfigManager::load(configPath, config) == true);

    CHECK(config["custom_config"]["oauth2"]["login_url"].asString() ==
          "https://sso.example.com/sign-in");
    CHECK(config["custom_config"]["oauth2"]["consent_url"].asString() ==
          "https://sso.example.com/approve");

    unsetenv("FULLA_OAUTH2_LOGIN_URL");
    unsetenv("FULLA_OAUTH2_CONSENT_URL");
}

// A rule whose key the config does not carry must not create that key, and
// must leave the rest of the config untouched. applyEnvOverrides reports the
// miss on stderr (it used to be a silent no-op); its effect on the config is
// what this case pins.
DROGON_TEST(Unit_P1_ConfigManager_EnvOverride_MissingAnchorIsNotCreated)
{
    setenv("FULLA_OAUTH2_LOGIN_URL", "https://sso.example.com/sign-in", 1);

    Json::Value config;
    config["custom_config"]["frontend"]["url"] = "http://localhost:5173";
    const std::vector<fulla::common::config::EnvOverride> rules = {
        {"custom_config.oauth2.login_url", "FULLA_OAUTH2_LOGIN_URL", false},
    };

    fulla::common::config::ConfigManager::applyEnvOverrides(config, rules);

    CHECK(config["custom_config"].isMember("oauth2") == false);
    CHECK(config["custom_config"]["frontend"]["url"].asString() ==
          "http://localhost:5173");

    unsetenv("FULLA_OAUTH2_LOGIN_URL");
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

DROGON_TEST(Unit_P0_ConfigManager_SocialCallbackCollisionFailsBeforeStartup)
{
    auto config = productionBaseConfig();
    auto &custom = config["custom_config"];
    custom["frontend"]["url"] = "https://portal.test";
    for (const auto *provider : {"github", "google"})
    {
        custom["external_auth"][provider]["client_id"] = "configured-client";
        custom["external_auth"][provider]["client_secret"] = "configured-secret";
    }
    custom["external_auth"]["github"]["redirect_uri"] = "https://PORTAL.test:443/callback/shared?provider=github";
    custom["external_auth"]["google"]["redirect_uri"] = "https://portal.test/callback/shared/?provider=google";
    std::string error;
    CHECK(fulla::common::config::ConfigManager::validate(config, error) == false);
    CHECK(error.find("distinct callback") != std::string::npos);
    custom["external_auth"]["google"]["redirect_uri"] = "https://portal.test/callback/google";
    CHECK(fulla::common::config::ConfigManager::validate(config, error) == true);
    custom["external_auth"]["github"]["redirect_uri"] = "https://portal.test/a/../callback/github";
    CHECK(fulla::common::config::ConfigManager::validate(config, error) == false);
    custom["external_auth"]["tiers"]["international"] = false;
    CHECK(fulla::common::config::ConfigManager::validate(config, error) == true);
}

namespace
{
// Saves one process-environment variable and restores it on destruction
// (including "it was not set"): a case that overwrites a key the CI
// environment itself provides (FULLA_ENV, FULLA_SIGNING_KEY) must not break
// the cases that follow.
class ScopedEnvVar
{
  public:
    ScopedEnvVar(const char *name, const std::string &value) : name_(name)
    {
        const char *previous = std::getenv(name);
        hadValue_ = previous != nullptr;
        if (hadValue_)
            previousValue_ = previous;
        setenv(name, value.c_str(), 1);
    }
    ~ScopedEnvVar()
    {
        if (hadValue_)
            setenv(name_.c_str(), previousValue_.c_str(), 1);
        else
            unsetenv(name_.c_str());
    }
    ScopedEnvVar(const ScopedEnvVar &) = delete;
    ScopedEnvVar &operator=(const ScopedEnvVar &) = delete;

  private:
    std::string name_;
    bool hadValue_ = false;
    std::string previousValue_;
};

// Restores the loaded .env table on every exit path (a REQUIRE failure returns
// early), so values injected by one case never leak into the next.
struct DotEnvTableRestore
{
    std::vector<std::pair<std::string, std::string>> entries =
      fulla::common::config::ConfigManager::getDotEnvForTesting();

    ~DotEnvTableRestore()
    {
        fulla::common::config::ConfigManager::setDotEnvForTesting(entries);
    }
};
}  // namespace

// A key that is read from the process environment only must not see a value
// that merely sits in the .env file, while the .env-aware accessor does.
// Such keys used to be read with a raw getenv(), so the .env value was
// ignored without a trace (getProcessEnv reports it now).
DROGON_TEST(Unit_P1_ConfigManager_GetProcessEnv_IgnoresDotEnvValue)
{
    DotEnvTableRestore restore;
    ScopedEnvVar noProcessKey("FULLA_SIGNING_KEY", "");
    fulla::common::config::ConfigManager::setDotEnvForTesting(
      {{"FULLA_SIGNING_KEY", "only-in-dot-env"}});

    const char *viaProcessOnly =
      fulla::common::config::ConfigManager::getProcessEnv("FULLA_SIGNING_KEY");
    CHECK((viaProcessOnly == nullptr || std::string(viaProcessOnly).empty()));

    const char *viaDotEnv =
      fulla::common::config::ConfigManager::getEnv("FULLA_SIGNING_KEY");
    REQUIRE(viaDotEnv != nullptr);
    CHECK(std::string(viaDotEnv) == "only-in-dot-env");
}

DROGON_TEST(Unit_P1_ConfigManager_GetProcessEnv_ReadsProcessEnvironment)
{
    DotEnvTableRestore restore;
    ScopedEnvVar autoMigrate("FULLA_AUTO_MIGRATE", "true");

    const char *value =
      fulla::common::config::ConfigManager::getProcessEnv("FULLA_AUTO_MIGRATE");
    REQUIRE(value != nullptr);
    CHECK(std::string(value) == "true");
}

// FULLA_ENV is the security switch: every reader settles on ONE value, the
// .env value when both sources are set (ConfigManager's documented
// precedence). The readers used to disagree -- two honored .env and two read
// the raw process environment.
DROGON_TEST(Unit_P1_ConfigManager_GetEnv_FullaEnv_DotEnvWinsOverProcess)
{
    DotEnvTableRestore restore;
    ScopedEnvVar processEnv("FULLA_ENV", "development");
    fulla::common::config::ConfigManager::setDotEnvForTesting({{"FULLA_ENV", "production"}});

    const char *value = fulla::common::config::ConfigManager::getEnv("FULLA_ENV");
    REQUIRE(value != nullptr);
    CHECK(std::string(value) == "production");
}

DROGON_TEST(Unit_P1_ConfigManager_DotEnvTableRestore_PreservesLoadedValues)
{
    DotEnvTableRestore restore;
    ScopedEnvVar processProbe("FULLA_DOTENV_RESTORE_PROBE", "");
    fulla::common::config::ConfigManager::setDotEnvForTesting(
      {{"FULLA_DOTENV_RESTORE_PROBE", "before"}});
    {
        DotEnvTableRestore innerRestore;
        fulla::common::config::ConfigManager::setDotEnvForTesting(
          {{"FULLA_DOTENV_RESTORE_PROBE", "during"}});
        const char *during =
          fulla::common::config::ConfigManager::getEnv("FULLA_DOTENV_RESTORE_PROBE");
        REQUIRE(during != nullptr);
        CHECK(std::string(during) == "during");
    }
    const char *after =
      fulla::common::config::ConfigManager::getEnv("FULLA_DOTENV_RESTORE_PROBE");
    REQUIRE(after != nullptr);
    CHECK(std::string(after) == "before");
}

DROGON_TEST(Unit_P0_ConfigManager_EnvOverride_PortalAliasesPreferCanonicalName)
{
    DotEnvTableRestore restore;
    fulla::common::config::ConfigManager::setDotEnvForTesting({});
    ScopedEnvVar canonicalSecret("FULLA_PORTAL_CLIENT_SECRET", "canonical-test-secret");
    ScopedEnvVar legacySecret("FULLA_VUE_CLIENT_SECRET", "legacy-test-secret");
    ScopedEnvVar canonicalRedirect("FULLA_PORTAL_REDIRECT_URI", "https://canonical.test/callback");
    ScopedEnvVar legacyRedirect("FULLA_VUE_REDIRECT_URI", "https://legacy.test/callback");

    std::vector<fulla::common::config::EnvOverride> rules;
    for (const auto &rule : fulla::common::config::FULLA_ENV_OVERRIDES)
    {
        const std::string name = rule.envVar;
        if (name == "FULLA_PORTAL_CLIENT_SECRET" || name == "FULLA_VUE_CLIENT_SECRET" ||
            name == "FULLA_PORTAL_REDIRECT_URI" || name == "FULLA_VUE_REDIRECT_URI")
        {
            rules.push_back(rule);
        }
    }

    const auto makeConfig = [] {
        Json::Value config;
        Json::Value plugin;
        plugin["name"] = "OAuth2Plugin";
        plugin["config"]["clients"]["fulla-portal"]["secret"] = "configured-secret";
        plugin["config"]["clients"]["fulla-portal"]["redirect_uri"] =
          "https://configured.test/callback";
        config["plugins"].append(plugin);
        return config;
    };
    const auto portalSecret = [](const Json::Value &config) {
        return fulla::common::config::ConfigManager::get<std::string>(
          config, "plugins[name=OAuth2Plugin].config.clients.fulla-portal.secret");
    };
    const auto portalRedirect = [](const Json::Value &config) {
        return fulla::common::config::ConfigManager::get<std::string>(
          config, "plugins[name=OAuth2Plugin].config.clients.fulla-portal.redirect_uri");
    };

    auto config = makeConfig();
    fulla::common::config::ConfigManager::applyEnvOverrides(config, rules);
    CHECK(portalSecret(config) == "canonical-test-secret");
    CHECK(portalRedirect(config) == "https://canonical.test/callback");

    setenv("FULLA_PORTAL_CLIENT_SECRET", "", 1);
    setenv("FULLA_PORTAL_REDIRECT_URI", "", 1);
    config = makeConfig();
    fulla::common::config::ConfigManager::applyEnvOverrides(config, rules);
    CHECK(portalSecret(config) == "legacy-test-secret");
    CHECK(portalRedirect(config) == "https://legacy.test/callback");

    setenv("FULLA_VUE_CLIENT_SECRET", "", 1);
    setenv("FULLA_VUE_REDIRECT_URI", "", 1);
    config = makeConfig();
    fulla::common::config::ConfigManager::applyEnvOverrides(config, rules);
    CHECK(portalSecret(config) == "configured-secret");
    CHECK(portalRedirect(config) == "https://configured.test/callback");
}
