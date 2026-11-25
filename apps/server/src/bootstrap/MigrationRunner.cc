#include "MigrationRunner.h"
#include <drogon/drogon.h>
#include <fulla/common/config/ConfigManager.h>
#include "../SchemaManager.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>

namespace bootstrap
{

namespace
{

// Locates the migrations directory by probing the same relative paths the
// repo layout and the docker image layout (/app/sql/migrations) use.
// Returns an empty path when nothing is found.
std::filesystem::path locateMigrationsDir()
{
    if (std::filesystem::exists("migrations"))
        return "migrations";
    if (std::filesystem::exists("sql/migrations"))
        return "sql/migrations";
    if (std::filesystem::exists("../migrations"))
        return "../migrations";
    if (std::filesystem::exists("../../apps/server/migrations"))
        return "../../apps/server/migrations";
    if (std::filesystem::exists("../../../apps/server/migrations"))
        return "../../../apps/server/migrations";
    return {};
}

// Builds a libpq connection string from config["db_clients"][0]. Values
// have already passed through ConfigManager's env-override rules
// (FULLA_DB_HOST/PORT/NAME/USER/PASSWORD), so this is the same target the
// server itself would connect to. The password is never logged.
std::string buildPgConnInfo(const Json::Value &config)
{
    const Json::Value &db = config["db_clients"][0u];
    std::ostringstream conn;
    conn << "host=" << db.get("host", "localhost").asString()
         << " port=" << db.get("port", 5432).asInt()
         << " dbname=" << db.get("dbname", "").asString()
         << " user=" << db.get("user", "").asString()
         << " password=" << db.get("passwd", "").asString();
    return conn.str();
}

}  // namespace

void setupMigrations()
{
    auto migrationsDir = locateMigrationsDir();
    if (migrationsDir.empty())
    {
        LOG_WARN << "Migrations directory not found, skipping schema migration";
        return;
    }

    LOG_INFO << "Schema migrations directory found: "
             << std::filesystem::absolute(migrationsDir).string();

    // Auto-migration is opt-in via FULLA_AUTO_MIGRATE=true. Production
    // deployments (Helm) keep it false and run migrations via the
    // pre-install/pre-upgrade hook Job (`fulla-server --migrate-only`);
    // the app then only self-checks that the schema is current.
    const char *autoMigrate =
      fulla::common::config::ConfigManager::getProcessEnv("FULLA_AUTO_MIGRATE");
    if (!autoMigrate || std::string(autoMigrate) != "true")
    {
        LOG_INFO << "Auto-migration disabled. Set FULLA_AUTO_MIGRATE=true to enable.";
        setupSchemaSelfCheck();
        return;
    }

    std::string migrationsDirStr = migrationsDir.string();
    drogon::app().registerBeginningAdvice([migrationsDirStr]() {
        // Run in a detached thread to avoid blocking the event loop
        std::thread([migrationsDirStr]() {
            // Small delay to ensure DB pool is ready
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!schema::SchemaManager::migrate(migrationsDirStr))
            {
                // Task 37: a half-migrated schema serving traffic is worse
                // than a crashloop -- fail loudly so the orchestrator
                // restarts us instead of running against a broken schema.
                LOG_FATAL << "Schema migration failed! Terminating server.";
                std::exit(1);
            }
        }).detach();
    });
}

void setupSchemaSelfCheck()
{
    auto migrationsDir = locateMigrationsDir();
    if (migrationsDir.empty())
        return;

    std::string migrationsDirStr = migrationsDir.string();
    drogon::app().registerBeginningAdvice([migrationsDirStr]() {
        std::thread([migrationsDirStr]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto db = drogon::app().getDbClient();
            if (!db)
                return;
            int pending = schema::SchemaManager::countPendingMigrations(db, migrationsDirStr);
            if (pending > 0)
            {
                // ERROR, not FATAL: during a rolling upgrade old replicas
                // legitimately observe a newer schema state; migrations are
                // forward-compatible within a major (migration-check M4).
                LOG_ERROR << "Schema self-check: " << pending
                          << " migration(s) on disk are not applied to the database. "
                          << "Run the migration Job (fulla-server --migrate-only).";
            }
            else if (pending == 0)
            {
                LOG_INFO << "Schema self-check: database schema is up to date";
            }
            else
            {
                LOG_WARN << "Schema self-check could not be completed";
            }
        }).detach();
    });
}

int runMigrateOnly(const Json::Value &config)
{
    auto migrationsDir = locateMigrationsDir();
    if (migrationsDir.empty())
    {
        LOG_ERROR << "migrate-only: migrations directory not found";
        return 1;
    }
    LOG_INFO << "migrate-only: using migrations from "
             << std::filesystem::absolute(migrationsDir).string();

    if (!config.isMember("db_clients") || config["db_clients"].empty())
    {
        LOG_ERROR << "migrate-only: no db_clients configured";
        return 1;
    }

    // Own client, own event-loop thread -- drogon::app().run() is never
    // entered in migrate-only mode. execSqlSync() blocks the main thread
    // until the loop thread delivers each result.
    auto db = drogon::orm::DbClient::newPgClient(buildPgConnInfo(config), 1);
    if (!db)
    {
        LOG_ERROR << "migrate-only: failed to construct database client";
        return 1;
    }

    if (!schema::SchemaManager::migrate(db, migrationsDir.string()))
    {
        LOG_ERROR << "migrate-only: migration failed";
        return 1;
    }

    LOG_INFO << "migrate-only: schema is up to date";
    return 0;
}

}  // namespace bootstrap
