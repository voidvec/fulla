// See AdvisoryLock.h for the #219 / R-M2-5 rationale and the raw-SQL
// note (the lock SELECT is a user-ruled seventh use outside the six
// documented exemptions).

#include <fulla/storage/postgres/AdvisoryLock.h>

#include <atomic>
#include <memory>
#include <utility>

namespace fulla::storage::postgres
{

namespace
{

// One shared failure gate per acquisition: the first failure rolls the
// transaction back and delivers onError exactly once; late lock-chain
// callbacks (statement error, synchronous throw, continuation throw) are
// all absorbed. Clearing onLocked before delivering drops the caller's
// continuation — including any Transaction handles it captured — so a
// partially issued write sequence cannot reach the destructor-time COMMIT.
struct AdvisoryLockChain
{
    std::shared_ptr<std::vector<std::string>> keys;
    std::shared_ptr<AdvisoryLockContinuation> onLocked;
    std::shared_ptr<AdvisoryLockError> onError;
    std::atomic<bool> stopped{false};

    void fail(
      const std::shared_ptr<::drogon::orm::Transaction> &txn,
      std::string detail
    )
    {
        if (stopped.exchange(true))
            return;
        if (txn)
        {
            try
            {
                txn->rollback();
            }
            catch (...)
            {
            }
        }
        auto deliver = std::move(*onError);
        onLocked = {};
        if (deliver)
            deliver(std::move(detail));
    }
};

// Takes the locks one statement at a time, in the caller's order, then
// delivers the transaction. Recursive-by-callback (bounded by keys.size(),
// which is at most 2 today). Every continuation is wrapped so a
// synchronous throw (execSqlAsync rejection, a caller's onLocked body)
// cannot escape into the Drogon event loop (the Mapper-construction rule
// of db-operations.md applied to arbitrary continuation bodies).
void takeNextLock(
  const std::shared_ptr<::drogon::orm::Transaction> &txn,
  const std::shared_ptr<AdvisoryLockChain> &chain,
  size_t index
)
{
    if (chain->stopped.load())
        return;
    try
    {
        if (index >= chain->keys->size())
        {
            // All locks held; hand the transaction over. Callers capture
            // this handle through their statement chains (the refcount
            // keeps the transaction open); the commit fires at the last
            // release.
            (*chain->onLocked)(txn);
            return;
        }
        // Parameter-bound key (never string-assembled SQL); hashtext maps
        // it into the int4 advisory-lock space. The lock is held for the
        // transaction's lifetime -- no explicit unlock.
        txn->execSqlAsync(
          "SELECT pg_advisory_xact_lock(hashtext($1))",
          [txn, chain, index](const ::drogon::orm::Result &) {
              takeNextLock(txn, chain, index + 1);
          },
          [txn, chain, index](const ::drogon::orm::DrogonDbException &e) {
              chain->fail(
                txn, "advisory lock failed for key " + (*chain->keys)[index] + ": " +
                       e.base().what()
              );
          },
          (*chain->keys)[index]
        );
    }
    catch (const std::exception &e)
    {
        chain->fail(txn, std::string("advisory lock chain threw: ") + e.what());
    }
    catch (...)
    {
        chain->fail(txn, "advisory lock chain threw");
    }
}

}  // namespace

void withAdvisoryXactLock(
  const ::drogon::orm::DbClientPtr &db,
  const std::vector<std::string> &keys,
  AdvisoryLockContinuation &&onLocked,
  AdvisoryLockError &&onError
)
{
    auto chain = std::make_shared<AdvisoryLockChain>();
    chain->keys = std::make_shared<std::vector<std::string>>(keys);
    chain->onLocked = std::make_shared<AdvisoryLockContinuation>(std::move(onLocked));
    chain->onError = std::make_shared<AdvisoryLockError>(std::move(onError));
    if (!db)
    {
        chain->fail(nullptr, "no database client");
        return;
    }

    // Async acquisition: callers sit inside DB result callbacks, and the
    // blocking newTransaction() overload would stall the very event loop
    // that has to fulfill it (PostgresTokenRepository's saveTokenPair
    // deadlock analysis).
    try
    {
        db->newTransactionAsync([chain](
                                  const std::shared_ptr<::drogon::orm::Transaction> &txn
                                ) {
            if (!txn)
            {
                chain->fail(nullptr, "failed to acquire transaction (timeout)");
                return;
            }
            takeNextLock(txn, chain, 0);
        });
    }
    catch (const std::exception &e)
    {
        chain->fail(nullptr, std::string("transaction acquisition threw: ") + e.what());
    }
    catch (...)
    {
        chain->fail(nullptr, "transaction acquisition threw");
    }
}

}  // namespace fulla::storage::postgres
