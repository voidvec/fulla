#include "../../contract/ContractFixtures.h"

#include <atomic>

namespace
{
using fulla::drogon::OwnerStatusCode;
using fulla::drogon::OwnerStatusResult;
using namespace fulla::test::contract;

OwnerStatusResult lookup(OAuth2Plugin *plugin, const std::string &clientId)
{
    auto promise = std::make_shared<std::promise<OwnerStatusResult>>();
    auto future = promise->get_future();
    plugin->lookupOwnerStatusChecked(clientId, [promise](OwnerStatusResult result) {
        promise->set_value(std::move(result));
    });
    if (future.wait_for(std::chrono::seconds(7)) != std::future_status::ready)
        throw std::runtime_error("owner status deadline did not complete");
    return future.get();
}
}  // namespace

DROGON_TEST(Integration_P0_OwnerStatus_LockTimeoutCannotBecomeCachedUnmanaged)
{
    auto db = getPostgresClientOrNull();
    if (!db)
        return;
    auto plugin = ::drogon::app().getPlugin<OAuth2Plugin>();
    plugin->invalidateOwnerStatusCache();
    const auto clientId = "s03-owner-" + uniqueSuffix();
    auto locked = std::make_shared<std::promise<std::shared_ptr<::drogon::orm::Transaction>>>();
    auto acquired = locked->get_future();
    db->newTransactionAsync([locked](const auto &txn) {
        if (!txn)
        {
            locked->set_value(nullptr);
            return;
        }
        // Test-only concurrency fault: a shared fixture table is held only
        // within this transaction; no schema or data is changed.
        txn->execSqlAsync(
          "LOCK TABLE oauth2_client_owners IN ACCESS EXCLUSIVE MODE",
          [locked, txn](const ::drogon::orm::Result &) { locked->set_value(txn); },
          [locked](const ::drogon::orm::DrogonDbException &) { locked->set_value(nullptr); }
        );
    });
    if (acquired.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        throw std::runtime_error("owner table fault lock not acquired");
    auto hold = acquired.get();
    if (!hold)
        throw std::runtime_error("owner table fault lock failed");
    auto promise = std::make_shared<std::promise<OwnerStatusResult>>();
    auto result = promise->get_future();
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto witness = std::make_shared<int>(1);
    std::weak_ptr<int> lifetime = witness;
    const auto start = std::chrono::steady_clock::now();
    plugin->lookupOwnerStatusChecked(clientId, [promise, calls, witness](OwnerStatusResult value) {
        (void)witness;
        ++*calls;
        promise->set_value(std::move(value));
    });
    witness.reset();
    if (result.wait_for(std::chrono::seconds(7)) != std::future_status::ready)
        throw std::runtime_error("owner status timeout missing");
    CHECK(result.get().code == OwnerStatusCode::Unavailable);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(7));
    // Keeping the table locked proves an error was not cached as a
    // successful NoRow: a second lookup must also reject, never hit Unmanaged.
    CHECK(lookup(plugin, clientId).code == OwnerStatusCode::Unavailable);
    hold.reset();
    CHECK(lookup(plugin, clientId).code == OwnerStatusCode::Unmanaged);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!lifetime.expired() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(lifetime.expired());
    CHECK(calls->load() == 1);
    plugin->invalidateOwnerStatusCache();
}
