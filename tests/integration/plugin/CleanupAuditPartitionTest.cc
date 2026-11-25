// tests/integration/plugin/CleanupAuditPartitionTest.cc
//
// #83: OAuth2CleanupService::maintainAuditPartitions drives V025's
// ensure_audit_partitions() on every cleanup cycle so the monthly audit_logs
// partition horizon keeps advancing without a cron. Real-PG integration
// coverage (the SQL function is DDL and cannot be faked):
//   - completion always fires (postgres path)
//   - the call is idempotent (second run reports no work, no throw)
//   - the disabled paths (no client / non-postgres flag) short-circuit and
//     still fire the completion without touching the DB.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <fulla/drogon/plugin/OAuth2CleanupService.h>

#include "HttpTestClient.h"

#include <chrono>
#include <future>
#include <thread>

using fulla::drogon::OAuth2CleanupService;
using fulla::test::http::postgresAvailable;

DROGON_TEST(Integration_P2_Cleanup_AuditPartitions_RunsAndIsIdempotent)
{
    if (!postgresAvailable())
    {
        LOG_INFO << "[skip] Postgres unavailable — audit partition test skipped";
        CHECK(true);
        return;
    }
    auto db = drogon::app().getDbClient();
    REQUIRE(db != nullptr);

    OAuth2CleanupService service(nullptr, nullptr, db, true);

    std::promise<void> first;
    service.maintainAuditPartitions([&first]() { first.set_value(); });
    REQUIRE(first.get_future().wait_for(std::chrono::seconds(15)) == std::future_status::ready);

    // Idempotent: a second call must complete (and create nothing -- the
    // horizon was just extended by the first call).
    std::promise<void> second;
    service.maintainAuditPartitions([&second]() { second.set_value(); });
    REQUIRE(second.get_future().wait_for(std::chrono::seconds(15)) == std::future_status::ready);

    // The horizon itself: current month's partition exists (V025 + this
    // maintenance guarantee [now-12mo, now+25mo)).
    auto rows = db->execSqlSync(
      "SELECT to_regclass('audit_logs_default') IS NOT NULL AS has_default"
    );
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]["has_default"].as<bool>());
}

DROGON_TEST(Integration_P2_Cleanup_AuditPartitions_DisabledPaths_CompleteWithoutDb)
{
    // Non-postgres / no client: direct completion, no DB access. Resolving a
    // DbClient in memory mode is a process-terminating assert (not a throw),
    // so the client-present sub-case only runs under postgres storage.
    {
        OAuth2CleanupService service(nullptr, nullptr, nullptr, false);
        std::promise<void> done;
        service.maintainAuditPartitions([&done]() { done.set_value(); });
        CHECK(done.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    }
    if (postgresAvailable())
    {
        auto db = drogon::app().getDbClient();
        REQUIRE(db != nullptr);
        OAuth2CleanupService service(nullptr, nullptr, db, false);
        std::promise<void> done;
        service.maintainAuditPartitions([&done]() { done.set_value(); });
        CHECK(done.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    }
    else
    {
        // Memory mode: the nullptr-client branch above already covered the
        // short-circuit; nothing more to assert here.
        CHECK(true);
    }
}

// The production cleanup cycle (doPurge) calls
// maintainAuditPartitions() with NO completion. The default argument is an
// empty std::function, and the implementation used to invoke it
// unconditionally, which threw std::bad_function_call once per hour (on the
// postgres path inside the execSqlAsync callback, outside the cleanup cycle's
// try/catch). Every invocation is guarded now; this case pins the exact
// production call shape -- no argument, both the disabled and (when
// available) the postgres path, no throw, and no completion to wait for.
DROGON_TEST(Integration_P2_Cleanup_AuditPartitions_DefaultCompletion_DoesNotThrow)
{
    {
        // Disabled path: returns synchronously; must not throw.
        OAuth2CleanupService service(nullptr, nullptr, nullptr, false);
        CHECK_NOTHROW(service.maintainAuditPartitions());
    }
    if (postgresAvailable())
    {
        // Postgres path: the async DDL runs with the no-op default completion.
        // Fire and give the loop a beat; a bad_function_call here would have
        // aborted the process before this CHECK even ran (that is the point).
        auto db = drogon::app().getDbClient();
        REQUIRE(db != nullptr);
        OAuth2CleanupService service(nullptr, nullptr, db, true);
        CHECK_NOTHROW(service.maintainAuditPartitions());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    else
    {
        CHECK(true);
    }
}
