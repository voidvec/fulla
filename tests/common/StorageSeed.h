// tests/common/StorageSeed.h
//
// In-process database seeding for the HTTP integration-test coverage push
// (docs/history/design/http-integration-test-coverage-plan.md, Phase 1, B1 fix).
//
// Problem this solves: the test binary (fulla-tests) does NOT seed the
// admin user on its own. tests/SchemaSetup.cc only creates a bare `users`
// table; the `admin`/password-`admin` user + role + the `admin-console`
// OAuth2 client come from apps/server/seed/dev_admin_user.sql and
// dev_admin_console_client.sql, which the Linux CI shell step applies via
// psql (scripts/backend/setup-database.sh, _build-test.yml:261-274). That
// shell step is Linux-only (`if: inputs.use_database`), so running the test
// binary directly -- locally on any OS, or on the Windows/macOS CI legs --
// left the DB without the admin user and every admin integration test fell
// through its memory-skip guard, exercising nothing.
//
// This header's seedDatabase() applies the migrations + seed SQL from inside
// the test process (via SchemaManager::migrate + SchemaManager::splitSqlStatements
// + execSqlSync), so admin integration tests work wherever a real Postgres is
// reachable, with no shell step. It is idempotent: migrations are tracked in
// the schema_migrations table, and every seed statement uses
// ON CONFLICT ... DO NOTHING.
//
// Paths FULLA_MIGRATIONS_DIR and FULLA_SEED_DIR are injected as compile
// definitions (absolute repo-source paths) in tests/CMakeLists.txt.
//
// Convention: header-only `inline` free functions in namespace
// fulla::test::seed, mirroring tests/contract/ContractFixtures.h. No
// TEST_CTX parameter -- seedDatabase() is called from a DROGON_TEST setup
// case which itself asserts success/failure.

#pragma once

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>

#include "SchemaManager.h"  // apps/server/src/, already in the test target's include dirs

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fulla::test::seed
{

namespace detail
{
// Read a text file into a string. Returns empty string on failure (caller
// checks and reports). Centralizes the read so seedDatabase() stays linear.
inline std::string readFile(const std::string &path)
{
    std::ifstream f(path);
    if (!f.is_open())
        return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Apply every statement in `sqlScript` against `db` synchronously. Uses
// SchemaManager::splitSqlStatements (dollar-quote + comment-aware splitter,
// see apps/server/src/SchemaManager.cc) so multi-statement seed files run
// correctly under Postgres prepared-statement semantics. Throws on any
// statement failure so the caller surfaces the error via FAIL().
inline void applyScript(
  const ::drogon::orm::DbClientPtr &db,
  const std::string &sqlScript,
  const std::string &label)
{
    auto stmts = schema::SchemaManager::splitSqlStatements(sqlScript);
    for (const auto &stmt : stmts)
    {
        db->execSqlSync(stmt);
    }
    LOG_INFO << "[seed] Applied " << stmts.size() << " statement(s) from " << label;
}
}  // namespace detail

// ---------------------------------------------------------------------------
// Seed the test database for HTTP admin integration tests.
//
// Runs (all idempotent):
//   1. SchemaManager::migrate(db, FULLA_MIGRATIONS_DIR) -- applies any
//      pending V*.sql migrations, tracked via the schema_migrations table.
//   2. apps/server/seed/dev_*.sql -- the dev admin user, fulla-admin-console
//      client, fulla-portal client, backend client (each ON CONFLICT DO NOTHING).
//   3. Reset the admin user's login-lockout counters so a prior failed-login
//      run (e.g. from a flaky or interrupted test) does not leave the admin
//      account locked. Mirrors Reset-AdminAccount in
//      scripts/backend/common-test-functions.ps1:28-72.
//
// Returns true on success, false on any failure (missing file, SQL error,
// no DbClient). Callers (the Database_P0_Seed_Setup DROGON_TEST case in
// SchemaSetup.cc) assert the return value. Under memory mode (no DbClient)
// returns true without doing anything -- callers gate on getStorageType()
// before invoking.
//
// Throws nothing: catches DrogonDbException internally and returns false so
// the calling DROGON_TEST can FAIL() with a clear message rather than abort
// the whole process from an uncaught exception.
// ---------------------------------------------------------------------------
inline bool seedDatabase()
{
    auto plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
    if (plugin && plugin->getStorageType() == "memory")
    {
        // Memory mode: no DbClient, no migrations, no seed. Memory-side
        // fixtures are constructed in-process by the plugin itself
        // (MemoryRoleRepository::initFromConfig etc.), so there is nothing
        // to do here.
        return true;
    }

    ::drogon::orm::DbClientPtr db;
    try
    {
        db = ::drogon::app().getDbClient();
    }
    catch (...)
    {
        LOG_WARN << "[seed] No DbClient available (DB not configured). "
                    "Skipping in-process seed.";
        return false;
    }
    if (!db)
        return false;

    // 1. Migrations.
    const std::string migrationsDir = FULLA_MIGRATIONS_DIR;
    if (!schema::SchemaManager::migrate(db, migrationsDir))
    {
        LOG_ERROR << "[seed] SchemaManager::migrate failed for " << migrationsDir;
        return false;
    }

    // 2. Dev seed files. Apply all of them so both the admin user (needed for
    // admin-route tests) and the vue/backend clients (needed for non-admin
    // flows) exist. Each is ON CONFLICT DO NOTHING, so re-runs are safe.
    const std::string seedDir = FULLA_SEED_DIR;
    const std::vector<std::string> seedFiles = {
        "dev_admin_user.sql",
        "dev_admin_console_client.sql",
        "dev_portal_client.sql",
        "dev_backend_client.sql",
    };
    for (const auto &fname : seedFiles)
    {
        const std::string path = seedDir + "/" + fname;
        const std::string sql = detail::readFile(path);
        if (sql.empty())
        {
            LOG_ERROR << "[seed] Could not read seed file: " << path;
            return false;
        }
        try
        {
            detail::applyScript(db, sql, fname);
        }
        catch (const ::drogon::orm::DrogonDbException &e)
        {
            LOG_ERROR << "[seed] Seed file " << fname
                      << " failed: " << e.base().what();
            return false;
        }
    }

    // 3. Reset admin login-lockout state. The `users` table's lockout columns
    // accumulate failed_login_count / locked_until across runs; a prior
    // interrupted test can leave the admin account locked, making every
    // subsequent admin test's login fail with AUTH_LOCKED. Reset
    // unconditionally (the dev seed password is known, so this is safe in the
    // test DB). Wrapped in try/catch: older migration sets may not have all
    // these columns, and a column-missing error should not be fatal (the
    // admin user still exists; only the lockout reset is best-effort).
    try
    {
        db->execSqlSync(
          "UPDATE users "
          "SET failed_login_count = 0, locked_until = NULL "
          "WHERE username = 'admin'");
    }
    catch (const ::drogon::orm::DrogonDbException &e)
    {
        LOG_WARN << "[seed] admin lockout reset skipped (column may be "
                    "absent in this migration set): "
                 << e.base().what();
    }

    return true;
}

}  // namespace fulla::test::seed
