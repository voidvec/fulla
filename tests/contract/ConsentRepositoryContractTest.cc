// tests/contract/ConsentRepositoryContractTest.cc
//
// Spec: fulla-sdk-refactor -- Task 12 (分档契约测试套件, design.md §7.3 / F5).
//
// Functional contract tests for IConsentRepository across all three
// backends (Postgres/Redis/Memory): save -> hasUserConsent -> revoke ->
// hasUserConsent round trip, using UserRef (F4, design.md §7.2) instead of a
// bare internalUserId.
//
// Fixture note (Postgres only): oauth2_user_consents.internal_user_id is a
// real FK into users(id) (see OAuth2Server/sql/migrations/V006__oauth2_scopes.sql),
// so this test inserts and cleans up a throwaway `users` row per test run
// (ON DELETE CASCADE on oauth2_user_consents means deleting the user also
// removes any consent rows this test created, but the test deletes them
// explicitly first for clarity and to avoid relying on cascade ordering).
// Redis/Memory have no such FK constraint -- UserRef::internalUserId is
// just an opaque integer baked into a key/map-key string for those two
// backends, so no fixture user is needed there.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>

#include <fulla/storage/postgres/PostgresConsentRepository.h>
#include <fulla/storage/redis/RedisConsentRepository.h>
#include <fulla/storage/memory/MemoryConsentRepository.h>
#include <fulla/oauth2/model/UserRef.h>

#include "ContractFixtures.h"

#include <string>

using namespace fulla::oauth2::repository;
using namespace fulla::oauth2::model;
using namespace fulla::test::contract;
using namespace fulla::storage::postgres;

namespace
{

// save -> hasUserConsent(true) -> revoke -> hasUserConsent(false). Shared
// across all three backends: no divergence found in the actual .cc
// implementations for this behavior (all three implement it as a
// straightforward existence-flag per (user, client, scope) key/row).
void runConsentRepository_SaveHasRevokeRoundTripContract(
  std::shared_ptr<::drogon::test::Case> TEST_CTX,
  std::shared_ptr<IConsentRepository> repo,
  const UserRef &user,
  const std::string &clientId,
  const std::string &scope
)
{
    // Not yet granted.
    auto before = waitForValue<bool>([&](auto cb) {
        repo->hasUserConsent(user, clientId, scope, std::move(cb));
    });
    CHECK(before == false);

    // Grant.
    auto saved = waitForValue<bool>([&](auto cb) {
        repo->saveUserConsent(user, clientId, scope, std::move(cb));
    });
    CHECK(saved == true);

    auto afterSave = waitForValue<bool>([&](auto cb) {
        repo->hasUserConsent(user, clientId, scope, std::move(cb));
    });
    CHECK(afterSave == true);

    // Revoke.
    waitForVoid([&](auto cb) { repo->revokeUserConsent(user, clientId, scope, std::move(cb)); });

    auto afterRevoke = waitForValue<bool>([&](auto cb) {
        repo->hasUserConsent(user, clientId, scope, std::move(cb));
    });
    CHECK(afterRevoke == false);
}

}  // namespace

// ===========================================================================
// Postgres
// ===========================================================================

DROGON_TEST(Integration_P0_Contract_Functional_ConsentRepository_Postgres_SaveHasRevokeRoundTrip)
{
    auto db = getPostgresClientOrNull();
    if (!db)
        return;

    // Fixture: throwaway users row (FK target for oauth2_user_consents).
    const std::string username = "contract_consent_" + uniqueSuffix();
    int32_t internalUserId = waitForValue<int32_t>([&](auto cb) {
        db->execSqlAsync(
          "INSERT INTO users (username, password_hash, salt) VALUES ($1, $2, $3) "
          "RETURNING id",
          [cb](const ::drogon::orm::Result &r) { cb(r.empty() ? -1 : r[0]["id"].as<int32_t>()); },
          [cb](const ::drogon::orm::DrogonDbException &) { cb(-1); },
          username,
          std::string("contract-test-hash"),
          std::string("contract-test-salt")
        );
    });
    REQUIRE(internalUserId > 0);

    UserRef user;
    user.internalUserId = internalUserId;

    auto repo = std::make_shared<PostgresConsentRepository>();
    repo->initFromConfig(Json::Value());
    runConsentRepository_SaveHasRevokeRoundTripContract(
      TEST_CTX, repo, user, "fulla-portal", "openid"
    );

    // Cleanup: consent rows first (defensive; ON DELETE CASCADE on the users
    // FK would also remove them), then the throwaway user itself.
    waitForVoid([&](auto cb) {
        db->execSqlAsync(
          "DELETE FROM oauth2_user_consents WHERE internal_user_id = $1",
          [cb](const ::drogon::orm::Result &) { cb(); },
          [cb](const ::drogon::orm::DrogonDbException &) { cb(); },
          internalUserId
        );
    });
    waitForVoid([&](auto cb) {
        db->execSqlAsync(
          "DELETE FROM users WHERE id = $1",
          [cb](const ::drogon::orm::Result &) { cb(); },
          [cb](const ::drogon::orm::DrogonDbException &) { cb(); },
          internalUserId
        );
    });
}

// ===========================================================================
// Redis
// ===========================================================================

DROGON_TEST(Integration_P0_Contract_Functional_ConsentRepository_Redis_SaveHasRevokeRoundTrip)
{
    auto redis = getRedisClientOrNull();
    if (!redis)
        return;

    UserRef user;
    user.internalUserId = 900001;  // opaque; no FK on this backend

    auto repo = std::make_shared<fulla::storage::redis::RedisConsentRepository>("default");
    runConsentRepository_SaveHasRevokeRoundTripContract(
      TEST_CTX, repo, user, "fulla-portal", "contract-scope-" + uniqueSuffix()
    );
}

// ===========================================================================
// Memory
// ===========================================================================

DROGON_TEST(Integration_P0_Contract_Functional_ConsentRepository_Memory_SaveHasRevokeRoundTrip)
{
    UserRef user;
    user.internalUserId = 900002;  // opaque; no FK on this backend

    auto repo = std::make_shared<fulla::storage::memory::MemoryConsentRepository>();
    runConsentRepository_SaveHasRevokeRoundTripContract(
      TEST_CTX, repo, user, "mem-client", "contract-scope-" + uniqueSuffix()
    );
}

// ===========================================================================
// Coverage additions (P2/P3, Memory backend): revokeUserConsent of a key
// that was never stored is a safe no-op (erase returns 0), and consents are
// independent per (user, client, scope) -- revoking one does not affect a
// consent recorded under a different scope.
// ===========================================================================

DROGON_TEST(Integration_P0_Contract_Functional_ConsentRepository_Memory_RevokeNonexistent_IsNoOp)
{
    UserRef user;
    user.internalUserId = 900010;
    auto repo = std::make_shared<fulla::storage::memory::MemoryConsentRepository>();

    const std::string scope = "revokescope-" + uniqueSuffix();
    // Revoke a consent that was never saved -> must not throw and the
    // callback must fire.
    bool called = false;
    repo->revokeUserConsent(user, "mem-client", scope, [&]() { called = true; });
    CHECK(called == true);

    // hasUserConsent still reports false for the never-saved consent.
    bool has = true;
    repo->hasUserConsent(user, "mem-client", scope, [&](bool v) { has = v; });
    CHECK(has == false);
}

DROGON_TEST(Integration_P0_Contract_Functional_ConsentRepository_Memory_PerScopeIndependence)
{
    UserRef user;
    user.internalUserId = 900011;
    auto repo = std::make_shared<fulla::storage::memory::MemoryConsentRepository>();
    const std::string scopeA = "scope-a-" + uniqueSuffix();
    const std::string scopeB = "scope-b-" + uniqueSuffix();

    waitForValue<bool>([&](auto cb) { repo->saveUserConsent(user, "mem-client", scopeA, std::move(cb)); });
    waitForValue<bool>([&](auto cb) { repo->saveUserConsent(user, "mem-client", scopeB, std::move(cb)); });

    // Revoke only scopeA.
    waitForVoid([&](auto cb) { repo->revokeUserConsent(user, "mem-client", scopeA, std::move(cb)); });

    bool hasA = true;
    repo->hasUserConsent(user, "mem-client", scopeA, [&](bool v) { hasA = v; });
    CHECK(hasA == false);

    // scopeB is unaffected.
    bool hasB = false;
    repo->hasUserConsent(user, "mem-client", scopeB, [&](bool v) { hasB = v; });
    CHECK(hasB == true);
}
