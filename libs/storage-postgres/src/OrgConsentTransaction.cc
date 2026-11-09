#include "OrgConsentTransaction.h"

#include <atomic>
#include <memory>

namespace fulla::storage::postgres::detail
{
namespace
{
struct LockChain
{
    std::vector<std::string> keys;
    std::function<void(const ConsentTransaction &)> locked;
    std::function<void()> failed;
    std::atomic<bool> stopped{false};

    void fail(const ConsentTransaction &txn)
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
        auto deliver = std::move(failed);
        locked = {};
        if (deliver)
            deliver();
    }
};

void takeLock(const ConsentTransaction &txn, const std::shared_ptr<LockChain> &chain, size_t index)
{
    if (chain->stopped.load())
        return;
    try
    {
        if (index == chain->keys.size())
        {
            chain->locked(txn);
            return;
        }
        // Parameter-bound advisory concurrency primitive, held to COMMIT.
        txn->execSqlAsync(
          "SELECT pg_advisory_xact_lock(hashtext($1))",
          [txn, chain, index](const ::drogon::orm::Result &) { takeLock(txn, chain, index + 1); },
          [txn, chain](const ::drogon::orm::DrogonDbException &) { chain->fail(txn); },
          chain->keys[index]
        );
    }
    catch (const std::exception &)
    {
        chain->fail(txn);
    }
    catch (...)
    {
        chain->fail(txn);
    }
}
}  // namespace

void withConsentTransaction(
  const ::drogon::orm::DbClientPtr &db,
  std::vector<std::string> keys,
  std::function<void(const ConsentTransaction &)> locked,
  std::function<void()> failed
)
{
    auto chain = std::make_shared<LockChain>();
    chain->keys = std::move(keys);
    chain->locked = std::move(locked);
    chain->failed = std::move(failed);
    if (!db)
    {
        chain->fail(nullptr);
        return;
    }
    try
    {
        db->newTransactionAsync([chain](const ConsentTransaction &txn) {
            if (!txn)
                chain->fail(nullptr);
            else
                takeLock(txn, chain, 0);
        });
    }
    catch (const std::exception &)
    {
        chain->fail(nullptr);
    }
    catch (...)
    {
        chain->fail(nullptr);
    }
}
}  // namespace fulla::storage::postgres::detail
