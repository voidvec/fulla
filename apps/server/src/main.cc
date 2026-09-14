// M3 Task 25 (fulla-sdk-refactor, design.md §6 "apps/server/src/main.cc
// # 仅装配：读配置 → 构造实现 → 注入端口 → run"): main() has been reduced
// to pure assembly -- CorsSetup/SecurityHeaders/ExceptionHandlerSetup/
// OpenApiSetup/MigrationRunner/ControllerRegistration are now independent
// bootstrap/ modules (see bootstrap/*.h). This file only wires them
// together in the correct order and starts the server.

#include <drogon/drogon.h>
#include <drogon/plugins/Hodor.h>
#include <json/json.h>
#include <fulla/common/config/ConfigManager.h>
#include <fulla/common/error/ErrorCatalog.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/common/error/ErrorTypes.h>
#include <fulla/drogon/error/RequestId.h>
#include <fulla/drogon/controllers/AuthorizationEndpointController.h>
#include <fulla/drogon/controllers/TokenEndpointController.h>
#include <fulla/drogon/controllers/DiscoveryController.h>
#include <fulla/drogon/controllers/UserAdminController.h>
#include <fulla/drogon/controllers/ClientAdminController.h>
#include <fulla/drogon/controllers/TokenAdminController.h>
#include <fulla/drogon/controllers/RoleScopeAdminController.h>
#include <fulla/drogon/controllers/AuditController.h>
#include <fulla/drogon/controllers/UserSelfServiceController.h>
#include <fulla/drogon/authz/ResourceScopeRegistry.h>
#include <OrganizationController.h>  // #43: product-app org controller scope decls

#include "bootstrap/AdminBootstrapper.h"
#include "bootstrap/ClientSeeder.h"
#include <fulla/drogon/validation/RuleSet.h>
#include "bootstrap/ControllerRegistration.h"
#include "bootstrap/CorsSetup.h"
#include "bootstrap/ExceptionHandlerSetup.h"
#include "bootstrap/IdentityAssembly.h"
#include "bootstrap/MigrationRunner.h"
#include "bootstrap/OpenApiSetup.h"
#include "bootstrap/SecurityHeaders.h"

#include <chrono>
#include <cstdlib>
#include <thread>
#include <future>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

// Helper to parse JSON (replaces deprecated Json::Reader)
static bool parseJsonString(std::istream &stream, Json::Value &json)
{
    Json::CharReaderBuilder builder;
    std::string errs;
    return Json::parseFromStream(builder, stream, &json, &errs);
}

// Helper to create log directory from config
static void createLogDirFromConfig(const std::string &configPath)
{
    std::ifstream configFile(configPath);
    if (!configFile.is_open())
        return;

    Json::Value root;
    if (parseJsonString(configFile, root))
    {
        const auto &logConfig = root["app"]["log"];
        if (!logConfig.isNull())
        {
            std::string logPath = logConfig.get("log_path", "").asString();
            if (!logPath.empty())
            {
                try
                {
                    if (!std::filesystem::exists(logPath))
                    {
                        std::filesystem::create_directories(logPath);
                        LOG_INFO << "Created log directory: " << logPath;
                    }
                }
                catch (const std::exception &e)
                {
                    LOG_ERROR << "Failed to create log directory: " << e.what();
                }
            }
        }
    }
}

// Load configuration with ConfigManager
static Json::Value loadConfiguration(const std::string &configPath)
{
    Json::Value config;

    if (!fulla::common::config::ConfigManager::load(configPath, config))
    {
        LOG_FATAL << "Failed to load configuration from: " << configPath;
        exit(1);
    }

    std::string validationError;
    if (!fulla::common::config::ConfigManager::validate(config, validationError))
    {
        LOG_FATAL << "Configuration validation failed: " << validationError;
        exit(1);
    }

    LOG_INFO << "Configuration loaded successfully";
    return config;
}

#ifdef FULLA_LEAK_DIAG
// Diagnostic-only build hook (docs/performance-optimization/
// backend-memory-retention-investigation.md): `kill -USR1 <worker>` runs a
// LeakSanitizer report on the live process. Registered in main() BEFORE any
// threads start; the check stops the world itself — call it with the server
// idle (threads parked in epoll) to avoid interrupting a malloc critical
// section. Enabled only by the diagnostic preset's -DFULLA_LEAK_DIAG.
#include <csignal>
extern "C" int __lsan_do_recoverable_leak_check(void);
extern "C" void __sanitizer_print_memory_profile(unsigned, unsigned);
static void leakDiagHandler(int)
{
    __lsan_do_recoverable_leak_check();
    // Heap profile of LIVE (reachable) allocations — the leak under
    // investigation is still-reachable, invisible to LSan's default report.
    __sanitizer_print_memory_profile(20, 10);
}
static void registerLeakDiagHook()
{
    signal(SIGUSR1, leakDiagHandler);
}
#else
static void registerLeakDiagHook()
{
}
#endif

int main(int argc, char *argv[])
{
    // Task 37 (fulla-sdk-refactor): --migrate-only runs all pending
    // schema migrations synchronously and exits 0/1 without starting the
    // HTTP server. This is the entry point for the Helm
    // pre-install/pre-upgrade hook Job; regular startup is unaffected.
    bool migrateOnly = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--migrate-only")
        {
            migrateOnly = true;
        }
        else
        {
            std::cerr << "Unknown argument: " << argv[i] << std::endl
                      << "Usage: fulla-server [--migrate-only]" << std::endl;
            return 1;
        }
    }

    // 1. Locate and load config.json (with env-var overrides)
    std::string configPath = "./config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../../../config.json";

    if (!std::filesystem::exists(configPath))
    {
        std::cerr << "WARNING: config.json not found during pre-start check." << std::endl;
        return 1;
    }

    createLogDirFromConfig(configPath);
    Json::Value config = loadConfiguration(configPath);

    if (migrateOnly)
        return bootstrap::runMigrateOnly(config);

    drogon::app().loadConfigJson(config);

    LOG_INFO << "Database host: "
             << fulla::common::config::ConfigManager::get<std::string>(
                  config, "db_clients.0.host", "localhost"
                );
    LOG_INFO
      << "Database port: "
      << fulla::common::config::ConfigManager::get<int>(config, "db_clients.0.port", 5432);
    LOG_INFO << "Redis host: "
             << fulla::common::config::ConfigManager::get<std::string>(
                  config, "redis_clients.0.host", "localhost"
                );

    // 2. Register every AutoCreation=false controller (must precede run())
    bootstrap::registerAllControllers();

    // 3. Wire cross-cutting concerns
    bootstrap::setupCors();
    bootstrap::setupSecurityHeaders();
    bootstrap::setupExceptionHandler();

    // Fail fast on a defective Error Catalog: validate the single source of
    // truth invariants at startup (Requirement 3.5). A violation aborts the
    // process so a defective build is never released.
    drogon::app().registerBeginningAdvice([]() {
        fulla::common::error::ErrorCatalog::validateInvariants();
        LOG_INFO << "ErrorCatalog invariants validated";
    });

    // #43: validate the resource-scope registry against the live route table.
    // Runs inside run() (after registerAllControllers) so getHandlersInfo()
    // is fully populated. A missing scope requirement on an auth-gated route,
    // or an orphan registry entry, LOG_FATAL-aborts -- a defective build is
    // never released (same loud-fail philosophy as ErrorCatalog above).
    drogon::app().registerBeginningAdvice([]() {
        fulla::drogon::authz::ResourceScopeRegistry::runConsistencyCheck();
    });

    // M3 Task 23 (fulla-sdk-refactor, evaluation H4): wire the
    // OAuth2Plugin pointer into every controller/filter that exposes a
    // setPlugin(), replacing their per-request
    // drogon::app().getPlugin<OAuth2Plugin>() lookups with a cached
    // pointer set once here. Must run AFTER plugins are constructed
    // (registerBeginningAdvice callbacks run once app().run() has
    // finished config-reflection plugin construction -- see
    // bootstrap::wireControllerPluginDependencies()'s header comment).
    drogon::app().registerBeginningAdvice([]() {
        bootstrap::wireControllerPluginDependencies();
        LOG_INFO << "Controller/filter plugin dependencies wired";
    });

    // Task 24 slice 4 (fulla-sdk-refactor): construct the identity-layer
    // services (fulla::identity::AuthService/SessionManager) and inject
    // them into SessionController. Must run after
    // wireControllerPluginDependencies() has registered every controller
    // (registerBeginningAdvice callbacks run in registration order) and,
    // like it, must be a registerBeginningAdvice callback itself --
    // drogon::app().getDbClient() is only reliably available once app().run()
    // has processed the db_clients config block.
    drogon::app().registerBeginningAdvice([]() { bootstrap::wireIdentityServices(); });

    // Report Hodor status after plugins have been initialized. Hodor is loaded
    // only by production configuration. When present, also wire its rejection
    // response factory so rate-limited requests get the standard Error Envelope
    // (VALIDATION_RATE_LIMITED / HTTP 429) instead of Hodor's plain-text body.
    // If the plugin is absent or its load state cannot be determined, startup
    // proceeds normally and rate-limit Envelope semantics are simply not
    // guaranteed (Requirements 6.4-6.7).
    drogon::app().registerBeginningAdvice([]() {
        try
        {
            auto hodor = drogon::app().getPlugin<drogon::plugin::Hodor>();
            if (hodor)
            {
                hodor->setRejectResponseFactory([](const drogon::HttpRequestPtr &req) {
                    fulla::common::error::Error error =
                      fulla::common::error::Error::fromCode(
                        "VALIDATION_RATE_LIMITED", fulla::common::error::RequestId::resolve(req)
                      );
                    return fulla::common::error::ErrorResponder::buildResponse(req, error);
                });
                LOG_INFO << "Hodor rate limiter enabled";
            }
            else
                LOG_INFO << "Hodor rate limiter not enabled by this config";
        }
        catch (const std::exception &)
        {
            LOG_INFO << "Hodor rate limiter not enabled by this config";
        }
    });

    // 4. OpenAPI docs (initApiDocs() is called here explicitly -- see
    // bootstrap/ControllerRegistration + OAuth2Plugin.cc's own comment on
    // why OAuth2Plugin no longer calls it itself: circular-dependency
    // avoidance between OAuth2Plugin and libs/drogon).
    LOG_INFO << "Initializing API documentation...";
    fulla::drogon::controllers::AuthorizationEndpointController::initApiDocs();
    fulla::drogon::controllers::TokenEndpointController::initApiDocs();
    fulla::drogon::controllers::DiscoveryController::initApiDocs();
    // #43 resource-scope authorization: the admin + user-self-service
    // controllers now declare their per-route scope requirements here (each
    // controller's initApiDocsImpl populates EndpointInfo.requiredScopes).
    fulla::drogon::controllers::UserAdminController::initApiDocs();
    fulla::drogon::controllers::ClientAdminController::initApiDocs();
    fulla::drogon::controllers::TokenAdminController::initApiDocs();
    fulla::drogon::controllers::RoleScopeAdminController::initApiDocs();
    fulla::drogon::controllers::AuditController::initApiDocs();
    fulla::drogon::controllers::UserSelfServiceController::initApiDocs();
    // #43: OrganizationController (product-app level, namespace `organization`).
    ::organization::OrganizationController::initApiDocs();
    bootstrap::setupOpenApi();

    // #43: build the resource-scope registry from the EndpointInfo set now
    // that all controllers have declared their scope requirements. The
    // registry is immutable after this point and consulted lock-free by the
    // filters at request time.
    fulla::drogon::authz::ResourceScopeRegistry::buildFromEndpoints();

    // #43: the old OAuth2AuthFilter gated ALL of /api/me/* (including MFA,
    // WebAuthn subpaths) with the `profile` scope by prefix matching. The
    // exact-template entries above cover the explicitly-declared /api/me
    // routes; this catch-all prefix preserves the blanket coverage for any
    // /api/me subpath not individually declared (e.g. /api/me/mfa/*,
    // /api/me/webauthn/*). Exact entries take priority over this prefix.
    {
        fulla::drogon::authz::ResourceScopeRequirement profileReq;
        profileReq.scopes = {"profile"};
        // No impliedBy -- admin does NOT satisfy user-self-service (RFC 6749
        // §3.3 / OIDC Core §5.4).
        fulla::drogon::authz::ResourceScopeRegistry::registerPrefix("/api/me", profileReq);
    }

    // Swagger UI is available at http://localhost:5555/docs/api
    // Static files are served from document_root configured in config.json

    // 5. Database migrations (opt-in via FULLA_AUTO_MIGRATE=true)
    bootstrap::setupMigrations();

    // 5a. Password policy (#103): auth.min_password_length (default 8).
    {
        const auto &custom = drogon::app().getCustomConfig();
        size_t minLen = 8;
        if (custom.isMember("auth") && custom["auth"].isMember("min_password_length"))
            minLen = static_cast<size_t>(custom["auth"]["min_password_length"].asUInt());
        fulla::drogon::validation::RuleSet::setPasswordMinLength(minLen);
    }

    // 5b. First-boot admin bootstrap (#103): when enabled and no user holds
    // the admin role, create 'admin' with a PBKDF2 password (env-provided or
    // random-printed-once). Delayed past the auto-migrate thread's start;
    // if migrations are still running the roles lookup fails soft and the
    // next restart retries. Dev/test seeds pre-create admin -> no-op.
    {
        const auto &custom = drogon::app().getCustomConfig();
        bool bootstrapEnabled = true;
        if (custom.isMember("auth") && custom["auth"].isMember("bootstrap_admin") &&
            custom["auth"]["bootstrap_admin"].isMember("enabled"))
        {
            bootstrapEnabled = custom["auth"]["bootstrap_admin"]["enabled"].asBool();
        }
        if (bootstrapEnabled)
        {
            drogon::app().registerBeginningAdvice([]() {
                std::thread([]() {
                    // Retry with backoff: on a cold boot the auto-migrate
                    // thread may still be creating tables; the roles lookup
                    // fails soft and is retried before giving up.
                    for (int attempt = 1; attempt <= 5; ++attempt)
                    {
                        std::this_thread::sleep_for(
                          std::chrono::seconds(2 * attempt)
                        );
                        const char *envPwd = std::getenv("FULLA_BOOTSTRAP_ADMIN_PASSWORD");
                        auto result = std::make_shared<std::promise<bool>>();
                        bootstrap::AdminBootstrapper::run(
                          envPwd ? std::string(envPwd) : std::string(),
                          [result](bool ok, const std::string &detail) {
                              if (!ok)
                                  LOG_WARN << "AdminBootstrap attempt: " << detail;
                              else
                                  LOG_INFO << "AdminBootstrap: " << detail;
                              result->set_value(ok);
                          }
                        );
                        if (result->get_future().get())
                            break;
                        LOG_WARN << "AdminBootstrap retry " << attempt << "/5 scheduled";
                    }
                }).detach();
            });
        }
    }

    // 5c. #143 startup self-heal: every user must own a (local, <id>) subject
    // mapping — consent resolves users exclusively through that table and a
    // user without a row 500s on authorize->consent. V027 backfills existing
    // rows once at migration time; this pass turns it into a startup
    // invariant that also converges transient creation-path failures.
    // Independent of (and runs alongside) the admin bootstrap above: it only
    // needs the users table to exist, and ON CONFLICT DO NOTHING makes it
    // idempotent against races with migrations or the bootstrap thread.
    {
        drogon::app().registerBeginningAdvice([]() {
            std::thread([]() {
                for (int attempt = 1; attempt <= 5; ++attempt)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(2 * attempt));
                    auto result = std::make_shared<std::promise<bool>>();
                    bootstrap::AdminBootstrapper::backfillLocalSubjectMappings(
                      [result](bool ok, const std::string &detail) {
                          if (ok)
                              LOG_INFO << "SubjectMappingHeal: " << detail;
                          else
                              LOG_WARN << "SubjectMappingHeal attempt: " << detail;
                          result->set_value(ok);
                      }
                    );
                    if (result->get_future().get())
                        break;
                }
            }).detach();
        });
    }

    // 5d. #204: seed the OAuth2 clients declared in the plugin config into
    // oauth2_clients / oauth2_client_scopes (postgres mode). Production had
    // no config->DB path for clients (the dev seed SQL is DEV-ONLY), so a
    // fresh deployment passed credential + email-verified checks at login
    // and then failed code issuance with 3001 (the P0-4 client-existence
    // guard) — a validation-class failure that logs nothing at ERROR level.
    // ON CONFLICT DO NOTHING keeps runtime/admin client edits authoritative
    // across restarts. Memory mode initializes clients from config
    // in-process and is skipped (no DB client).
    {
        drogon::app().registerBeginningAdvice([]() {
            std::thread([]() {
                auto plugin = drogon::app().getPlugin<fulla::drogon::OAuth2Plugin>();
                if (!plugin || plugin->getStorageType() == "memory")
                    return;
                const auto &clients = plugin->clientsSeedConfig();
                if (!clients.isObject() || clients.empty())
                    return;
                for (int attempt = 1; attempt <= 5; ++attempt)
                {
                    std::this_thread::sleep_for(
                      std::chrono::seconds(2 * attempt)
                    );
                    auto result = std::make_shared<std::promise<bool>>();
                    bootstrap::ClientSeeder::run(
                      clients,
                      [result](bool ok, const std::string &detail) {
                          if (ok)
                              LOG_INFO << "ClientSeeder: " << detail;
                          else
                              LOG_WARN << "ClientSeeder attempt: " << detail;
                          result->set_value(ok);
                      }
                    );
                    if (result->get_future().get())
                        break;
                    LOG_WARN << "ClientSeeder retry " << attempt << "/5 scheduled";
                }
            }).detach();
        });
    }

    registerLeakDiagHook();

    // 6. Start the server
    drogon::app().run();
    return 0;
}
