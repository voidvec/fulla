// See AdvisoryLock.h for the #219 / R-M2-5 rationale and the raw-SQL
// note (the lock SELECT is a user-ruled seventh use outside the six
// documented exemptions).

#include <fulla/storage/postgres/AdvisoryLock.h>

#include <memory>

namespace fulla::storage::postgres
{

namespace
{

// Takes the locks one statement at a time, in the caller's order, then
// delivers the transaction. Recursive-by-callback (bounded by keys.size(),
// which is at most 2 today).
void takeNextLock(
  const std::shared_ptr<::drogon::orm::Transaction> &txn,
  const std::shared_ptr<std::vector<std::string>> &keys,
  size_t index,
  const std::shared_ptr<AdvisoryLockContinuation> &onLocked,
  const std::shared_ptr<AdvisoryLockError> &onError
)
{
    if (index >= keys->size())
    {
        // All locks held; hand the transaction over. Callers capture this
        // handle through their statement chains (the refcount keeps the
        // transaction open); the commit fires at the last release.
        (*onLocked)(txn);
        return;
    }
    // Parameter-bound key (never string-assembled SQL); hashtext maps
    // it into the int4 advisory-lock space. The lock is held for the
    // transaction's lifetime -- no explicit unlock.
    txn->execSqlAsync(
      "SELECT pg_advisory_xact_lock(hashtext($1))",
      [txn, keys, index, onLocked, onError](const ::drogon::orm::Result &) {
          takeNextLock(txn, keys, index + 1, onLocked, onError);
      },
      [onError, keys, index](const ::drogon::orm::DrogonDbException &e) {
          (*onError)(
            "advisory lock failed for key " + (*keys)[index] + ": " + e.base().what()
          );
      },
      (*keys)[index]
    );
}

}  // namespace

void withAdvisoryXactLock(
  const ::drogon::orm::DbClientPtr &db,
  const std::vector<std::string> &keys,
  AdvisoryLockContinuation &&onLocked,
  AdvisoryLockError &&onError
)
{
    if (!db)
    {
        onError("no database client");
        return;
    }

    auto sharedLocked = std::make_shared<AdvisoryLockContinuation>(std::move(onLocked));
    auto sharedError = std::make_shared<AdvisoryLockError>(std::move(onError));
    auto sharedKeys = std::make_shared<std::vector<std::string>>(keys);

    // Async acquisition: callers sit inside DB result callbacks, and the
    // blocking newTransaction() overload would stall the very event loop
    // that has to fulfill it (PostgresTokenRepository's saveTokenPair
    // deadlock analysis).
    db->newTransactionAsync(
      [sharedLocked, sharedError, sharedKeys](
        const std::shared_ptr<::drogon::orm::Transaction> &txn) {
          if (!txn)
          {
              (*sharedError)("failed to acquire transaction (timeout)");
              return;
          }
          takeNextLock(txn, sharedKeys, 0, sharedLocked, sharedError);
      }
    );
}

}  // namespace fulla::storage::postgres
