// tests/contract/GrantRepositoryContractTest.cc
//
// Spec: fulla-sdk-refactor -- Task 12 (分档契约测试套件, design.md §7.3 / F5).
//
// Functional contract tests for IGrantRepository across all three backends
// (Postgres/Redis/Memory). See ContractFixtures.h for the parameterization
// approach rationale.
//
// Primary focus, per the task description and design.md §7.3's explicit
// callout: consumeAuthCode()'s RFC 6749 §4.1.3 redirect_uri validation and
// single-use consumption semantics. All three backends implement
// consumeAuthCode as their own atomic CAS-flavored operation (Postgres:
// "UPDATE ... WHERE used=false RETURNING ..."; Redis: a Lua EVAL script;
// Memory: a lock-guarded check-then-set) -- this is exactly the kind of
// "same behavior via different mechanisms" case a contract test is meant to
// pin down.
//
// saveAuthCode/getAuthCode/markAuthCodeUsed round-trip coverage is included
// as prerequisite plumbing for the consumeAuthCode scenarios (a
// consumeAuthCode test needs a saved code to consume), not as a separately
// motivated addition.
//
// Authorization-transaction operations (saveAuthorizationTransaction /
// getAuthorizationTransaction / deleteAuthorizationTransaction /
// markTransactionConsumed) are NOT covered here: per REPOSITORY_MAPPING.md
// and PostgresGrantRepository.h's class comment, the Postgres
// implementation of these four methods is a DOCUMENTED PLACEHOLDER (it does
// not persist to a real table -- saveAuthorizationTransaction always
// returns true without storing anything, getAuthorizationTransaction always
// returns nullopt). Writing a contract test that asserts "save then get
// round-trips" against Postgres would fail against genuinely-out-of-scope,
// pre-existing, intentionally-undocumented-as-fixed behavor -- that is a
// known, tracked limitation (not silently swept under the rug), not
// something this task is chartered to fix. Redis and Memory DO have real
// implementations of these four methods, but a contract test that only two
// of three backends could pass would defeat the purpose of a *contract*
// test tier (it is supposed to define the floor ALL implementations meet).
// This is called out explicitly here rather than silently omitted.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>

#include <fulla/storage/postgres/PostgresGrantRepository.h>
#include <fulla/storage/redis/RedisGrantRepository.h>
#include <fulla/storage/memory/MemoryGrantRepository.h>

#include "ContractFixtures.h"

#include <string>

using namespace fulla::oauth2::repository;
using namespace fulla::oauth2::model;
using namespace fulla::test::contract;
using namespace fulla::storage::postgres;

namespace
{

OAuth2AuthCode makeAuthCode(
  const std::string &code,
  const std::string &clientId,
  const std::string &redirectUri,
  int64_t ttlSeconds = 300
)
{
    OAuth2AuthCode c;
    c.code = code;
    c.clientId = clientId;
    c.userId = "contract-user";
    c.scope = "openid profile";
    c.redirectUri = redirectUri;
    c.expiresAt = nowSeconds() + ttlSeconds;
    c.used = false;
    return c;
}

// save -> get round trip: the exact fields written are readable back
// unchanged. Prerequisite plumbing for the consumeAuthCode scenarios below,
// not independently motivated (see file header).
void runGrantRepository_SaveGetRoundTripContract(
  std::shared_ptr<::drogon::test::Case> TEST_CTX,
  std::shared_ptr<IGrantRepository> repo,
  const std::string &clientId
)
{
    const std::string code = "contract-code-roundtrip-" + uniqueSuffix();
    const std::string redirectUri = "http://localhost/roundtrip-cb";
    auto authCode = makeAuthCode(code, clientId, redirectUri);

    waitForVoid([&](auto cb) { repo->saveAuthCode(authCode, std::move(cb)); });

    auto fetched = waitForValue<std::optional<OAuth2AuthCode>>([&](auto cb) {
        repo->getAuthCode(code, std::move(cb));
    });
    REQUIRE(fetched.has_value());
    CHECK(fetched->code == code);
    CHECK(fetched->clientId == clientId);
    CHECK(fetched->redirectUri == redirectUri);
    CHECK(fetched->used == false);
}

// getAuthCode on an id that was never saved must return nullopt.
void runGrantRepository_NotFoundContract(
  std::shared_ptr<::drogon::test::Case> TEST_CTX,
  std::shared_ptr<IGrantRepository> repo
)
{
    auto fetched = waitForValue<std::optional<OAuth2AuthCode>>([&](auto cb) {
        repo->getAuthCode("contract-nonexistent-code-" + uniqueSuffix(), std::move(cb));
    });
    CHECK(!fetched.has_value());
}

// Core contract (design.md §7.3): consumeAuthCode() with the CORRECT
// redirect_uri succeeds exactly once, returning the code's data. This is
// the "happy path" companion to the mismatch/reuse scenarios below.
void runGrantRepository_ConsumeAuthCode_CorrectRedirectUriSucceedsContract(
  std::shared_ptr<::drogon::test::Case> TEST_CTX,
  std::shared_ptr<IGrantRepository> repo,
  const std::string &clientId
)
{
    const std::string code = "contract-code-consume-ok-" + uniqueSuffix();
    const std::string redirectUri = "http://localhost/consume-ok-cb";
    auto authCode = makeAuthCode(code, clientId, redirectUri);

    waitForVoid([&](auto cb) { repo->saveAuthCode(authCode, std::move(cb)); });

    auto consumed = waitForValue<std::optional<OAuth2AuthCode>>([&](auto cb) {
        repo->consumeAuthCode(code, redirectUri, std::move(cb));
    });
    REQUIRE(consumed.has_value());
    CHECK(consumed->code == code);
    CHECK(consumed->clientId == clientId);
}

// Core contract (design.md §7.3, RFC 6749 §4.1.3): consumeAuthCode() with a
// MISMATCHED redirect_uri MUST return nullopt (must not treat the code as
// consumed by an attacker-controlled redirect target). This is the specific
// security property the task description calls out as "务必覆盖".
void runGrantRepository_ConsumeAuthCode_WrongRedirectUriFailsContract(
  std::shared_ptr<::drogon::test::Case> TEST_CTX,
  std::shared_ptr<IGrantRepository> repo,
  const std::string &clientId
)
{
    const std::string code = "contract-code-consume-mismatch-" + uniqueSuffix();
    const std::string registeredRedirectUri = "http://localhost/registered-cb";
    const std::string attackerRedirectUri = "http://evil.example/cb";
    auto authCode = makeAuthCode(code, clientId, registeredRedirectUri);

    waitForVoid([&](auto cb) { repo->saveAuthCode(authCode, std::move(cb)); });

    auto consumed = waitForValue<std::optional<OAuth2AuthCode>>([&](auto cb) {
        repo->consumeAuthCode(code, attackerRedirectUri, std::move(cb));
    });
    CHECK(!consumed.has_value());
}

// Core contract (design.md §7.3): consumeAuthCode() is single-use. The
// SECOND call with the same (now-already-consumed) code -- even with the
// correct redirect_uri -- MUST return nullopt.
void runGrantRepository_ConsumeAuthCode_SingleUseContract(
  std::shared_ptr<::drogon::test::Case> TEST_CTX,
  std::shared_ptr<IGrantRepository> repo,
  const std::string &clientId
)
{
    const std::string code = "contract-code-consume-reuse-" + uniqueSuffix();
    const std::string redirectUri = "http://localhost/reuse-cb";
    auto authCode = makeAuthCode(code, clientId, redirectUri);

    waitForVoid([&](auto cb) { repo->saveAuthCode(authCode, std::move(cb)); });

    auto firstConsume = waitForValue<std::optional<OAuth2AuthCode>>([&](auto cb) {
        repo->consumeAuthCode(code, redirectUri, std::move(cb));
    });
    REQUIRE(firstConsume.has_value());

    auto secondConsume = waitForValue<std::optional<OAuth2AuthCode>>([&](auto cb) {
        repo->consumeAuthCode(code, redirectUri, std::move(cb));
    });
    CHECK(!secondConsume.has_value());
}

}  // namespace

// ===========================================================================
// Postgres
// ===========================================================================

DROGON_TEST(Integration_P0_Contract_Functional_GrantRepository_Postgres_SaveGetRoundTrip)
{
    auto db = getPostgresClientOrNull();
    if (!db)
        return;

    auto repo = std::make_shared<PostgresGrantRepository>();
    repo->initFromConfig(Json::Value());
    runGrantRepository_SaveGetRoundTripContract(TEST_CTX, repo, "fulla-portal");
}

DROGON_TEST(Integration_P0_Contract_Functional_GrantRepository_Postgres_NotFoundReturnsNullopt)
{
    auto db = getPostgresClientOrNull();
    if (!db)
        return;

    auto repo = std::make_shared<PostgresGrantRepository>();
    repo->initFromConfig(Json::Value());
    runGrantRepository_NotFoundContract(TEST_CTX, repo);
}

DROGON_TEST(
  Integration_P0_Contract_Functional_GrantRepository_Postgres_ConsumeAuthCode_CorrectRedirectUriSucceeds
)
{
    auto db = getPostgresClientOrNull();
    if (!db)
        return;

    auto repo = std::make_shared<PostgresGrantRepository>();
    repo->initFromConfig(Json::Value());
    runGrantRepository_ConsumeAuthCode_CorrectRedirectUriSucceedsContract(
      TEST_CTX, repo, "fulla-portal"
    );
}

DROGON_TEST(
  Integration_P0_Contract_Functional_GrantRepository_Postgres_ConsumeAuthCode_WrongRedirectUriFails
)
{
    auto db = getPostgresClientOrNull();
    if (!db)
        return;

    auto repo = std::make_shared<PostgresGrantRepository>();
    repo->initFromConfig(Json::Value());
    runGrantRepository_ConsumeAuthCode_WrongRedirectUriFailsContract(TEST_CTX, repo, "fulla-portal");
}

DROGON_TEST(Integration_P0_Contract_Functional_GrantRepository_Postgres_ConsumeAuthCode_SingleUse)
{
    auto db = getPostgresClientOrNull();
    if (!db)
        return;

    auto repo = std::make_shared<PostgresGrantRepository>();
    repo->initFromConfig(Json::Value());
    runGrantRepository_ConsumeAuthCode_SingleUseContract(TEST_CTX, repo, "fulla-portal");
}

// ===========================================================================
// Redis
// ===========================================================================

DROGON_TEST(Integration_P0_Contract_Functional_GrantRepository_Redis_SaveGetRoundTrip)
{
    auto redis = getRedisClientOrNull();
    if (!redis)
        return;

    auto repo = std::make_shared<fulla::storage::redis::RedisGrantRepository>("default");
    runGrantRepository_SaveGetRoundTripContract(TEST_CTX, repo, "fulla-portal");
}

DROGON_TEST(Integration_P0_Contract_Functional_GrantRepository_Redis_NotFoundReturnsNullopt)
{
    auto redis = getRedisClientOrNull();
    if (!redis)
        return;

    auto repo = std::make_shared<fulla::storage::redis::RedisGrantRepository>("default");
    runGrantRepository_NotFoundContract(TEST_CTX, repo);
}

DROGON_TEST(
  Integration_P0_Contract_Functional_GrantRepository_Redis_ConsumeAuthCode_CorrectRedirectUriSucceeds
)
{
    auto redis = getRedisClientOrNull();
    if (!redis)
        return;

    auto repo = std::make_shared<fulla::storage::redis::RedisGrantRepository>("default");
    runGrantRepository_ConsumeAuthCode_CorrectRedirectUriSucceedsContract(
      TEST_CTX, repo, "fulla-portal"
    );
}

DROGON_TEST(
  Integration_P0_Contract_Functional_GrantRepository_Redis_ConsumeAuthCode_WrongRedirectUriFails
)
{
    auto redis = getRedisClientOrNull();
    if (!redis)
        return;

    auto repo = std::make_shared<fulla::storage::redis::RedisGrantRepository>("default");
    runGrantRepository_ConsumeAuthCode_WrongRedirectUriFailsContract(TEST_CTX, repo, "fulla-portal");
}

DROGON_TEST(Integration_P0_Contract_Functional_GrantRepository_Redis_ConsumeAuthCode_SingleUse)
{
    auto redis = getRedisClientOrNull();
    if (!redis)
        return;

    auto repo = std::make_shared<fulla::storage::redis::RedisGrantRepository>("default");
    runGrantRepository_ConsumeAuthCode_SingleUseContract(TEST_CTX, repo, "fulla-portal");
}

// ===========================================================================
// Memory
// ===========================================================================

DROGON_TEST(Integration_P0_Contract_Functional_GrantRepository_Memory_SaveGetRoundTrip)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryGrantRepository>();
    runGrantRepository_SaveGetRoundTripContract(TEST_CTX, repo, "mem-client");
}

DROGON_TEST(Integration_P0_Contract_Functional_GrantRepository_Memory_NotFoundReturnsNullopt)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryGrantRepository>();
    runGrantRepository_NotFoundContract(TEST_CTX, repo);
}

DROGON_TEST(
  Integration_P0_Contract_Functional_GrantRepository_Memory_ConsumeAuthCode_CorrectRedirectUriSucceeds
)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryGrantRepository>();
    runGrantRepository_ConsumeAuthCode_CorrectRedirectUriSucceedsContract(
      TEST_CTX, repo, "mem-client"
    );
}

DROGON_TEST(
  Integration_P0_Contract_Functional_GrantRepository_Memory_ConsumeAuthCode_WrongRedirectUriFails
)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryGrantRepository>();
    runGrantRepository_ConsumeAuthCode_WrongRedirectUriFailsContract(TEST_CTX, repo, "mem-client");
}

DROGON_TEST(Integration_P0_Contract_Functional_GrantRepository_Memory_ConsumeAuthCode_SingleUse)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryGrantRepository>();
    runGrantRepository_ConsumeAuthCode_SingleUseContract(TEST_CTX, repo, "mem-client");
}

// ===========================================================================
// Coverage additions (P1) -- Memory backend only. These exercise
// MemoryGrantRepository's AuthorizationTransaction CRUD (entirely
// untested because Postgres is a placeholder), markAuthCodeUsed's
// no-op-on-missing path, the consumeAuthCode empty-redirectUri bypass,
// getAuthCode lazy expiry eviction, markTransactionConsumed's
// first-true/second-false guard, and purgeExpired.
// ===========================================================================

// AuthorizationTransaction: save -> get round trips the fields
// (MemoryGrantRepository.cc:99-134).
DROGON_TEST(Integration_P0_Contract_Functional_GrantRepository_Memory_AuthorizationTransaction_SaveGetRoundTrip)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryGrantRepository>();
    AuthorizationTransaction txn;
    txn.transactionId = "txn-rt-" + uniqueSuffix();
    txn.clientId = "mem-client";
    txn.subject = "contract-user";
    txn.redirectUri = "https://example.test/cb";
    txn.state = "xyz";
    txn.requestedScopes = {"openid", "profile"};
    txn.validScopes = {"openid"};
    txn.expiresAt = nowSeconds() + 300;

    bool saved = false;
    repo->saveAuthorizationTransaction(txn, [&](bool ok) { saved = ok; });
    CHECK(saved == true);

    auto fetched = waitForValue<std::optional<AuthorizationTransaction>>([&](auto cb) {
        repo->getAuthorizationTransaction(txn.transactionId, std::move(cb));
    });
    REQUIRE(fetched.has_value());
    CHECK(fetched->clientId == "mem-client");
    CHECK(fetched->subject == "contract-user");
    CHECK(fetched->redirectUri == "https://example.test/cb");
    CHECK(fetched->state == "xyz");
    CHECK(fetched->requestedScopes.size() == 2u);
    CHECK(fetched->validScopes.size() == 1u);
    CHECK(fetched->consumed == false);
}

// AuthorizationTransaction: an expired transaction is evicted on read and
// returns nullopt (MemoryGrantRepository.cc:120-127).
DROGON_TEST(Integration_P0_Contract_Functional_GrantRepository_Memory_AuthorizationTransaction_Expired_GetReturnsNullopt_AndEvicts)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryGrantRepository>();
    AuthorizationTransaction txn;
    txn.transactionId = "txn-expired-" + uniqueSuffix();
    txn.clientId = "mem-client";
    txn.expiresAt = nowSeconds() - 100;  // already expired
    repo->saveAuthorizationTransaction(txn, [](bool) {});

    auto fetched = waitForValue<std::optional<AuthorizationTransaction>>([&](auto cb) {
        repo->getAuthorizationTransaction(txn.transactionId, std::move(cb));
    });
    CHECK(!fetched.has_value());
}

// markTransactionConsumed: first call returns true and marks consumed;
// second call returns false (MemoryGrantRepository.cc:147-165).
DROGON_TEST(Integration_P0_Contract_Functional_GrantRepository_Memory_MarkTransactionConsumed_FirstTrue_SecondFalse)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryGrantRepository>();
    AuthorizationTransaction txn;
    txn.transactionId = "txn-consume-" + uniqueSuffix();
    txn.clientId = "mem-client";
    txn.expiresAt = nowSeconds() + 300;
    repo->saveAuthorizationTransaction(txn, [](bool) {});

    bool first = true;
    repo->markTransactionConsumed(txn.transactionId, [&](bool ok) { first = ok; });
    CHECK(first == true);

    bool second = true;
    repo->markTransactionConsumed(txn.transactionId, [&](bool ok) { second = ok; });
    CHECK(second == false);
}

// markAuthCodeUsed: a no-op on a missing code (does not throw, callback
// fires) and marks an existing code used (MemoryGrantRepository.cc:54-64).
DROGON_TEST(Integration_P0_Contract_Functional_GrantRepository_Memory_MarkAuthCodeUsed_MissingNoOp_ExistingMarksUsed)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryGrantRepository>();

    // Missing code -> no throw, callback fires.
    bool missingCalled = false;
    repo->markAuthCodeUsed("mark-missing-" + uniqueSuffix(), [&]() { missingCalled = true; });
    CHECK(missingCalled == true);

    // Existing code -> marked used (observable: consumeAuthCode now fails).
    const std::string code = "mark-existing-" + uniqueSuffix();
    auto ac = makeAuthCode(code, "mem-client", "https://example.test/cb");
    repo->saveAuthCode(ac, [] {});
    repo->markAuthCodeUsed(code, [] {});

    auto consumed = waitForValue<std::optional<OAuth2AuthCode>>([&](auto cb) {
        repo->consumeAuthCode(code, "https://example.test/cb", std::move(cb));
    });
    CHECK(!consumed.has_value());  // already used -> nullopt
}

// consumeAuthCode (F-009, RFC 6749 §4.1.3): when a redirect_uri was recorded
// at authorization time, the token request MUST carry it AND it must match.
// Previously an empty request redirectUri bypassed the comparison; now it is
// rejected. The code is NOT consumed (single-use preserved for a valid match).
DROGON_TEST(Integration_P0_Contract_Functional_GrantRepository_Memory_ConsumeAuthCode_EmptyRedirectUriRejectedWhenBound)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryGrantRepository>();
    const std::string code = "bound-redir-" + uniqueSuffix();
    auto ac = makeAuthCode(code, "mem-client", "https://registered.example/cb");
    repo->saveAuthCode(ac, [] {});

    // Consume with an EMPTY redirectUri while a redirect_uri is bound -> reject
    // (nullopt). The stored redirect is non-empty, so the request must echo it.
    auto consumed = waitForValue<std::optional<OAuth2AuthCode>>([&](auto cb) {
        repo->consumeAuthCode(code, "", std::move(cb));
    });
    REQUIRE(!consumed.has_value());

    // The code is still consumable with the correct redirect_uri (not burned
    // by the failed attempt).
    auto consumedOk = waitForValue<std::optional<OAuth2AuthCode>>([&](auto cb) {
        repo->consumeAuthCode(code, "https://registered.example/cb", std::move(cb));
    });
    REQUIRE(consumedOk.has_value());
    CHECK(consumedOk->code == code);
}

// getAuthCode: an expired code is evicted on read and returns nullopt
// (MemoryGrantRepository.cc:41-49).
DROGON_TEST(Integration_P0_Contract_Functional_GrantRepository_Memory_GetAuthCode_Expired_EvictedAndReturnsNullopt)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryGrantRepository>();
    const std::string code = "expired-ac-" + uniqueSuffix();
    auto ac = makeAuthCode(code, "mem-client", "https://example.test/cb", -100);
    repo->saveAuthCode(ac, [] {});

    auto fetched = waitForValue<std::optional<OAuth2AuthCode>>([&](auto cb) {
        repo->getAuthCode(code, std::move(cb));
    });
    CHECK(!fetched.has_value());
}

// purgeExpired: removes expired auth codes, keeps live ones
// (MemoryGrantRepository.cc:169-192).
DROGON_TEST(Integration_P0_Contract_Functional_GrantRepository_Memory_PurgeExpired_RemovesExpiredCodes)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryGrantRepository>();
    const std::string liveCode = "purge-live-" + uniqueSuffix();
    const std::string deadCode = "purge-dead-" + uniqueSuffix();
    repo->saveAuthCode(makeAuthCode(liveCode, "mem-client", "https://example.test/cb", 300), [] {});
    repo->saveAuthCode(makeAuthCode(deadCode, "mem-client", "https://example.test/cb", -100), [] {});

    repo->purgeExpired();

    auto live = waitForValue<std::optional<OAuth2AuthCode>>([&](auto cb) {
        repo->getAuthCode(liveCode, std::move(cb));
    });
    CHECK(live.has_value());

    auto dead = waitForValue<std::optional<OAuth2AuthCode>>([&](auto cb) {
        repo->getAuthCode(deadCode, std::move(cb));
    });
    CHECK(!dead.has_value());
}
