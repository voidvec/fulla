// tests/common/DbDiagnostics.h
//
// Issue #299 deep-diagnosis tooling: at the moment an HTTP request to the
// in-process server dies with NO response (the "null-resp" signature that
// rotates across one login-family test per full-suite run), dump the REAL
// server-side database state so the failure can be attributed instead of
// guessed at.
//
// Why a dedicated out-of-pool connection: the leading hypotheses for the
// null-resp are (a) the shared 10-connection pool being fully busy (drogon's
// `timeout: -1` config makes the client-side sqlCmdBuffer_ an UNBOUNDED-TIME
// queue, so a query that never gets a connection never errors) and (b) a
// server-side lock pileup. A diagnostic that borrows the very pool under
// suspicion would queue behind the wedge and produce nothing. This header
// therefore creates a SEPARATE 1-connection drogon PgClient (its own
// "DbLoop" thread, independent of the app's event loops and pool) from a
// conninfo string that tests/test_main.cc captures while rewriting the
// runtime config.
//
// Everything here is best-effort: any failure to snapshot is logged and
// swallowed -- diagnostics must never turn a 1-failure run into a crash.
//
// Convention: header-only `inline` free functions in namespace
// fulla::test::diag, mirroring tests/common/HttpTestClient.h.

#pragma once

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace fulla::test::diag
{
// Monotonic milliseconds since first call. Every diagnostic line prints it so
// chronology survives the heavy stdout/log buffering that made the CI logs of
// the v1.6.0-era failures unreadable (drogon's async logger and std::cout
// interleave non-chronologically across threads; the tail cluster of a failed
// run stamps ~everything with the same wall-clock second).
inline long uptimeMs()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

// Set by tests/test_main.cc's loadConfigWithEnv() after env overrides are
// applied (host/port/dbname/user/passwd of db_clients[0]). Empty in
// memory-storage mode -- every dump call no-ops then.
inline std::string &pgConnInfoStorage()
{
    static std::string connInfo;
    return connInfo;
}

namespace detail
{
// The out-of-pool client. Created lazily on first dump, never destroyed
// (process-lifetime static; tearing it down mid-run would join its DbLoop
// thread for no benefit).
inline ::drogon::orm::DbClientPtr diagnosticPgClient()
{
    static ::drogon::orm::DbClientPtr client;
    static std::once_flag once;
    std::call_once(once, []() {
        const std::string connInfo = pgConnInfoStorage();
        if (connInfo.empty())
            return;
        try
        {
            // autoBatch=false: one statement in flight keeps the snapshot
            // logic trivially correct on this throwaway connection.
            client = ::drogon::orm::DbClient::newPgClient(connInfo, 1, false);
        }
        catch (const std::exception &e)
        {
            std::cout << "[dbdiag] failed to create diagnostic PgClient: "
                      << e.what() << std::endl;
        }
    });
    return client;
}

// Run one query on the diagnostic client with a hard 5s wait. Returns an
// empty optional on timeout/error (after logging) -- see file header for why
// this must never throw or hang. (drogon's Result has no default ctor, hence
// optional rather than an empty Result.)
inline std::optional<::drogon::orm::Result> queryWithDeadline(const std::string &sql)
{
    auto client = diagnosticPgClient();
    if (!client)
        return std::nullopt;
    auto future = client->execSqlAsyncFuture(sql);
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
    {
        std::cout << "[dbdiag] query timed out after 5s (pool/DB wedged past "
                     "even the out-of-pool probe): "
                  << sql.substr(0, 80) << std::endl;
        return std::nullopt;
    }
    try
    {
        return future.get();
    }
    catch (const std::exception &e)
    {
        std::cout << "[dbdiag] query failed: " << e.what() << std::endl;
        return std::nullopt;
    }
}

inline std::string col(const ::drogon::orm::Row &row, const char *name)
{
    if (row[name].isNull())
        return "-";
    return row[name].as<std::string>();
}
}  // namespace detail

// Snapshot the database state that discriminates the #299 hypotheses:
//   - statements actually RUNNING / queued on locks server-side
//     (wait_event = Lock, or pg_blocking_pids non-empty)  -> lock-pileup side
//   - everything `idle` while the app pool is suspected busy -> drogon-side
//     pool leak / pipeline desync (the app holds connections it never uses)
//   - `idle in transaction` leftovers                        -> leaked txn
// Printed with steady-clock ms so the moment can be placed on the suite
// timeline regardless of std::cout buffering.
inline void dumpPgDiagnostics(const std::string &where)
{
    if (pgConnInfoStorage().empty())
        return;  // memory-storage leg: nothing to inspect

    std::cout << "[dbdiag] === " << where << " (t+" << uptimeMs() << "ms) ==="
              << std::endl;

    // 1) Per-connection activity. `state` + `wait_event_type` is the core
    //    discriminator; query_start/xact_start age the wedge.
    auto rows = detail::queryWithDeadline(
      "SELECT pid, state, "
      "coalesce(wait_event_type, '-') AS wait_type, "
      "coalesce(wait_event, '-') AS wait_event, "
      "coalesce(extract(epoch from now() - xact_start)::text, '-') AS xact_age_s, "
      "coalesce(extract(epoch from now() - query_start)::text, '-') AS query_age_s, "
      "left(coalesce(query, ''), 140) AS query "
      "FROM pg_stat_activity "
      "WHERE datname = current_database() AND pid <> pg_backend_pid() "
      "ORDER BY query_start NULLS LAST");
    if (rows)
    {
        std::cout << "[dbdiag] pg_stat_activity: " << rows->size()
                  << " connection(s)" << std::endl;
        for (const auto &r : *rows)
        {
            std::cout << "[dbdiag]   pid=" << detail::col(r, "pid")
                      << " state=" << detail::col(r, "state")
                      << " wait=" << detail::col(r, "wait_type") << "/"
                      << detail::col(r, "wait_event")
                      << " xact_age=" << detail::col(r, "xact_age_s")
                      << "s query_age=" << detail::col(r, "query_age_s")
                      << "s :: " << detail::col(r, "query") << std::endl;
        }
    }
    else
    {
        std::cout << "[dbdiag] pg_stat_activity: snapshot unavailable"
                  << std::endl;
    }

    // 2) Who is blocking whom (the head of any lock queue).
    rows = detail::queryWithDeadline(
      "SELECT pid, pg_blocking_pids(pid) AS blocked_by, "
      "coalesce(wait_event_type, '-') AS wait_type, state, "
      "left(coalesce(query, ''), 120) AS query "
      "FROM pg_stat_activity "
      "WHERE cardinality(pg_blocking_pids(pid)) > 0");
    if (rows && !rows->empty())
    {
        std::cout << "[dbdiag] BLOCKED statements:" << std::endl;
        for (const auto &r : *rows)
        {
            std::cout << "[dbdiag]   pid=" << detail::col(r, "pid")
                      << " blocked_by=" << detail::col(r, "blocked_by")
                      << " wait=" << detail::col(r, "wait_type")
                      << " state=" << detail::col(r, "state")
                      << " :: " << detail::col(r, "query") << std::endl;
        }
    }
    else
    {
        std::cout << "[dbdiag] no blocked statements" << std::endl;
    }

    // 3) Ungranted locks (the queued side of any contention).
    rows = detail::queryWithDeadline(
      "SELECT pid, locktype, coalesce(relation::regclass::text, '-') AS rel, "
      "mode, granted FROM pg_locks WHERE NOT granted");
    if (rows && !rows->empty())
    {
        std::cout << "[dbdiag] UNGRANTED locks:" << std::endl;
        for (const auto &r : *rows)
        {
            std::cout << "[dbdiag]   pid=" << detail::col(r, "pid")
                      << " " << detail::col(r, "locktype") << " "
                      << detail::col(r, "rel") << " " << detail::col(r, "mode")
                      << std::endl;
        }
    }
    std::cout << "[dbdiag] === end snapshot (t+" << uptimeMs() << "ms) ==="
              << std::endl;
}

}  // namespace fulla::test::diag
