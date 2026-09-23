#pragma once

// Advisory transaction locks for the quota TOCTOU fix (#219, v1.5.0 M2
// ruling R-M2-5). The open-platform creation paths used to be
// count-then-insert sequences; concurrent requests could exceed the
// per-user app quota, the org app quota, the 24h creation rate limit,
// the per-user org quota, and the per-org pending-invitation cap.
//
// withAdvisoryXactLock() serializes those sequences per quota domain:
// it opens ONE transaction, takes pg_advisory_xact_lock(hashtext(key))
// for every key in order, and hands the TRANSACTION (as a DbClient) to
// the caller's continuation -- every Mapper query the caller then runs
// executes inside the locked transaction, and the transaction commits
// when the last shared_ptr reference drops after the caller's final
// statement completes, releasing all locks atomically with the durable
// write.
//
// Multiple keys (e.g. app creation under an org needs the user-domain
// rate limit AND the org-domain app quota) MUST be taken through this
// single-transaction helper rather than nested helper calls: two
// separate transactions would release their locks at destructor order,
// and a lock released before the OTHER transaction's insert commits
// reopens the exact race being fixed. Callers taking overlapping lock
// sets must pass them in a CONSISTENT order (user-domain before
// org-domain everywhere) so concurrent flows cannot deadlock.
//
// Raw-SQL note: the lock SELECT below is deliberately outside the six
// documented raw-SQL exemptions of .claude/rules/db-operations.md. It is
// a user-ruled seventh use (#219 / R-M2-5): an advisory lock is a
// concurrency primitive, not CRUD, and cannot be expressed through
// Mapper. Flagged in the PR body for reviewer sign-off.
//
// Acquisition MUST be asynchronous: these call sites sit inside DB
// result callbacks (controller/service async chains), and the blocking
// newTransaction() overload deadlocks there (see
// PostgresTokenRepository.cc's saveTokenPair fix for the analysis --
// the blocking future is only fulfilled by the DB event loops the call
// would stall).
//
// Key format (R-M2-5), one lock domain per quota's natural scope:
//   per-user quota / 24h rate limit -> "quota:user:<user_id>"
//   per-org quota / invitation cap  -> "quota:org:<org_id>"
//   org consent upsert              -> "orgconsent:<org_id>:<client_id>"

#include <drogon/orm/DbClient.h>

#include <functional>
#include <string>
#include <vector>

namespace fulla::storage::postgres
{

/// Delivers the locked transaction to `onLocked`. The handle handed over
/// IS the transaction (a Transaction is a DbClient): run every Mapper
/// query of the guarded sequence on it, and RESPOND TO THE CALLER FROM
/// setCommitCallback -- the commit fires when the last shared_ptr
/// reference drops (after the terminal statement callback returns), so
/// an inline response goes out BEFORE the commit and an immediate
/// follow-up read (the creating client refreshing its list) can miss
/// the new rows. Statement-error paths may respond inline (the
/// transaction rolls back; a commit callback registered before the
/// error never fires, so there is no double response).
using AdvisoryLockContinuation =
  std::function<void(const std::shared_ptr<::drogon::orm::Transaction> &)>;

/// Reports lock/transaction-acquisition failure (timeout or a lock
/// statement errored) with a human-readable detail.
using AdvisoryLockError = std::function<void(const std::string &)>;

void withAdvisoryXactLock(
  const ::drogon::orm::DbClientPtr &db,
  const std::vector<std::string> &keys,
  AdvisoryLockContinuation &&onLocked,
  AdvisoryLockError &&onError
);

}  // namespace fulla::storage::postgres
