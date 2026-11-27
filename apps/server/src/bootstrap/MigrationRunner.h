#pragma once

// M3 Task 25 (fulla-sdk-refactor): extracted from main.cc's inline
// migrations-directory-lookup + auto-migrate block. Locates the SQL
// migrations directory (tries several relative paths) and, if
// FULLA_AUTO_MIGRATE=true, registers a beginning advice that runs
// schema::SchemaManager::migrate() on a detached thread shortly after
// startup.
//
// Task 37 (fulla-sdk-refactor): grew two production entry points --
// runMigrateOnly() backs the `fulla-server --migrate-only` CLI flag
// used by the Helm pre-install/pre-upgrade hook Job (synchronous, own
// DbClient, real exit code), and setupSchemaSelfCheck() is the startup
// self-check for FULLA_AUTO_MIGRATE=false deployments where migrations
// run externally. setupMigrations() now terminates the process when an
// auto-migration fails instead of logging and carrying on.

#include <json/json.h>

namespace bootstrap
{

// Locates the migrations directory and registers the auto-migration
// beginning advice (opt-in via the FULLA_AUTO_MIGRATE=true environment
// variable). Must be called before drogon::app().run(). When auto-migrate
// is disabled, registers a schema self-check instead (see
// setupSchemaSelfCheck()).
void setupMigrations();

// Registers a beginning advice that compares the on-disk migration files
// against schema_migrations and logs an ERROR when the database schema is
// behind the binary (deployments are expected to run the migration hook
// Job first). Never terminates the process -- a lagging replica must not
// crashloop during a rolling upgrade window.
void setupSchemaSelfCheck();

// Wakes and joins any background migration or schema self-check threads so
// shutdown does not leave detached threads racing static teardown.
void shutdownMigrations();

// Synchronously applies all pending migrations using a self-constructed
// DbClient built from config["db_clients"][0] (drogon::app().run() is
// never entered). Returns a process exit code: 0 = schema up to date,
// 1 = failure. Connection retries are unbounded inside the drogon client,
// so callers (the K8s hook Job) must bound the wall clock externally
// (activeDeadlineSeconds).
int runMigrateOnly(const Json::Value &config);

}  // namespace bootstrap
