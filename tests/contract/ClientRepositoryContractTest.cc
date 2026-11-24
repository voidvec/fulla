// tests/contract/ClientRepositoryContractTest.cc
//
// Spec: fulla-sdk-refactor -- Task 12 (分档契约测试套件, design.md §7.3 / F5).
//
// Functional contract tests for IClientRepository across all three backends
// (Postgres/Redis/Memory). See ContractFixtures.h for the parameterization
// approach rationale.
//
// TESTABLE SURFACE (evaluated honestly per REPOSITORY_MAPPING.md /
// IClientRepository.h): the interface exposes only getClient/validateClient
// -- there is no insert/create method on IClientRepository itself (client
// registration is a config-time / migration-time concern in the current
// design, not a runtime repository operation). This means "write test data"
// for this interface is backend-specific:
//   - Postgres: relies on already-seeded rows (OAuth2Server/sql/seed/*.sql,
//     applied by CI's "Initialize Database" step and by local dev setup):
//     `fulla-portal` (PUBLIC) and `backend-svc` (CONFIDENTIAL, secret
//     "test-secret"). No SQL INSERT is issued by this test file itself --
//     doing so would duplicate migration/seed concerns and risks diverging
//     from the real seed data other tests already depend on.
//   - Redis: HSET a client hash directly via the raw RedisClient (the only
//     way to get data in, since IClientRepository has no write method and
//     RedisOAuth2Storage/fulla::storage::redis::RedisClientRepository never gained one either).
//   - Memory: fulla::storage::memory::MemoryClientRepository::initFromConfig() -- the one
//   repository
//     that DOES have a construction-time write path, so this test builds a
//     fresh config-based fixture instead of relying on seed data.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>

#include <fulla/storage/postgres/PostgresClientRepository.h>
#include <fulla/storage/redis/RedisClientRepository.h>
#include <fulla/storage/memory/MemoryClientRepository.h>

#include "ContractFixtures.h"

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>

using namespace fulla::oauth2::repository;
using namespace fulla::oauth2::model;
using namespace fulla::test::contract;
using namespace fulla::storage::postgres;

namespace
{

// ---------------------------------------------------------------------------
// Shared assertion functions (backend-agnostic body, backend-specific
// fixture data supplied by each DROGON_TEST case below).
// ---------------------------------------------------------------------------

// Universal: every implementation must report "not found" the same way for
// getClient (nullopt) and validateClient (false) on an id that was never
// registered. This has no fixture dependency, so it needs no seed data on
// any backend.
void runClientRepository_NotFoundContract(
  std::shared_ptr<::drogon::test::Case> TEST_CTX,
  std::shared_ptr<IClientRepository> repo
)
{
    const std::string missingId = "contract-nonexistent-client-" + uniqueSuffix();

    auto client = waitForValue<std::optional<OAuth2Client>>([&](auto cb) {
        repo->getClient(missingId, std::move(cb));
    });
    CHECK(!client.has_value());

    auto valid = waitForValue<bool>([&](auto cb) {
        repo->validateClient(missingId, "any-secret", std::move(cb));
    });
    CHECK(valid == false);
}

// PUBLIC clients accept ANY secret (including empty) per RFC 6749 -- PUBLIC
// clients are, by definition, incapable of keeping a secret confidential, so
// validateClient() must not gate on it. Verified true for Postgres and
// Memory (both branch on ClientType::PUBLIC and short-circuit to `true`
// before ever looking at the provided secret). Not run against Redis --
// fulla::storage::redis::RedisClientRepository never persists/reads a client_type field at all
// (see fulla::storage::redis::RedisClientRepository.h/.cc), so there is no PUBLIC/CONFIDENTIAL
// branch to exercise on that backend; Redis gets its own, differently-shaped
// contract test below (KnownClientHashValidationContract) that matches its
// real (type-agnostic) behavior instead of papering over the difference.
void runClientRepository_PublicClientAcceptsAnySecretContract(
  std::shared_ptr<::drogon::test::Case> TEST_CTX,
  std::shared_ptr<IClientRepository> repo,
  const std::string &publicClientId
)
{
    auto client = waitForValue<std::optional<OAuth2Client>>([&](auto cb) {
        repo->getClient(publicClientId, std::move(cb));
    });
    REQUIRE(client.has_value());
    CHECK(client->clientId == publicClientId);
    CHECK(client->clientType == ClientType::PUBLIC);

    auto validEmpty =
      waitForValue<bool>([&](auto cb) { repo->validateClient(publicClientId, "", std::move(cb)); });
    CHECK(validEmpty == true);

    auto validWrong = waitForValue<bool>([&](auto cb) {
        repo->validateClient(publicClientId, "totally-wrong-secret", std::move(cb));
    });
    CHECK(validWrong == true);
}

// CONFIDENTIAL clients MUST validate their secret: correct secret succeeds,
// wrong secret fails, empty secret fails. Verified true for Postgres (hashed
// comparison against DB-stored secret+salt) and Memory (plaintext constant-
// time comparison). Not run against Redis for the same reason as the PUBLIC
// test above.
void runClientRepository_ConfidentialClientValidatesSecretContract(
  std::shared_ptr<::drogon::test::Case> TEST_CTX,
  std::shared_ptr<IClientRepository> repo,
  const std::string &confidentialClientId,
  const std::string &correctSecret,
  const std::string &wrongSecret
)
{
    auto client = waitForValue<std::optional<OAuth2Client>>([&](auto cb) {
        repo->getClient(confidentialClientId, std::move(cb));
    });
    REQUIRE(client.has_value());
    CHECK(client->clientId == confidentialClientId);
    CHECK(client->clientType == ClientType::CONFIDENTIAL);

    auto validCorrect = waitForValue<bool>([&](auto cb) {
        repo->validateClient(confidentialClientId, correctSecret, std::move(cb));
    });
    CHECK(validCorrect == true);

    auto validWrong = waitForValue<bool>([&](auto cb) {
        repo->validateClient(confidentialClientId, wrongSecret, std::move(cb));
    });
    CHECK(validWrong == false);

    auto validEmpty = waitForValue<bool>([&](auto cb) {
        repo->validateClient(confidentialClientId, "", std::move(cb));
    });
    CHECK(validEmpty == false);
}

// Redis-specific: fulla::storage::redis::RedisClientRepository::validateClient has no notion of
// client_type at all (see its .cc: no PUBLIC/CONFIDENTIAL branch exists).
// Its REAL contract is: empty secret -> EXISTS check (true iff the client
// hash exists in Redis, regardless of what "type" it conceptually is); non-
// empty secret -> SHA-256(secret+salt) compared against the stored hash.
// This test asserts that actual, documented behavior rather than a PUBLIC/
// CONFIDENTIAL distinction Redis does not implement.
void runClientRepository_Redis_KnownClientHashValidationContract(
  std::shared_ptr<::drogon::test::Case> TEST_CTX,
  std::shared_ptr<IClientRepository> repo,
  const std::string &clientId,
  const std::string &correctSecret,
  const std::string &wrongSecret
)
{
    auto client = waitForValue<std::optional<OAuth2Client>>([&](auto cb) {
        repo->getClient(clientId, std::move(cb));
    });
    REQUIRE(client.has_value());
    CHECK(client->clientId == clientId);

    // Empty secret -> EXISTS-based check: true because the client hash key
    // exists in Redis (Redis does not gate this on a stored secret at all).
    auto validEmpty =
      waitForValue<bool>([&](auto cb) { repo->validateClient(clientId, "", std::move(cb)); });
    CHECK(validEmpty == true);

    auto validCorrect = waitForValue<bool>([&](auto cb) {
        repo->validateClient(clientId, correctSecret, std::move(cb));
    });
    CHECK(validCorrect == true);

    auto validWrong = waitForValue<bool>([&](auto cb) {
        repo->validateClient(clientId, wrongSecret, std::move(cb));
    });
    CHECK(validWrong == false);
}

}  // namespace

// ===========================================================================
// Postgres
// ===========================================================================

DROGON_TEST(Integration_P0_Contract_Functional_ClientRepository_Postgres_NotFoundReturnsNullopt)
{
    auto db = getPostgresClientOrNull();
    if (!db)
        return;

    auto repo = std::make_shared<PostgresClientRepository>();
    repo->initFromConfig(Json::Value());
    runClientRepository_NotFoundContract(TEST_CTX, repo);
}

// Fixture: apps/server/seed/dev_portal_client.sql -- `fulla-portal`, PUBLIC.
DROGON_TEST(
  Integration_P0_Contract_Functional_ClientRepository_Postgres_PublicClientAcceptsAnySecret
)
{
    auto db = getPostgresClientOrNull();
    if (!db)
        return;

    auto repo = std::make_shared<PostgresClientRepository>();
    repo->initFromConfig(Json::Value());
    runClientRepository_PublicClientAcceptsAnySecretContract(TEST_CTX, repo, "fulla-portal");
}

// Fixture: OAuth2Server/sql/seed/dev_backend_client.sql -- `backend-svc`,
// CONFIDENTIAL, secret "test-secret".
DROGON_TEST(
  Integration_P0_Contract_Functional_ClientRepository_Postgres_ConfidentialClientValidatesSecret
)
{
    auto db = getPostgresClientOrNull();
    if (!db)
        return;

    auto repo = std::make_shared<PostgresClientRepository>();
    repo->initFromConfig(Json::Value());
    runClientRepository_ConfidentialClientValidatesSecretContract(
      TEST_CTX, repo, "backend-svc", "test-secret", "wrong-secret"
    );
}

// ===========================================================================
// Redis
// ===========================================================================

DROGON_TEST(Integration_P0_Contract_Functional_ClientRepository_Redis_NotFoundReturnsNullopt)
{
    auto redis = getRedisClientOrNull();
    if (!redis)
        return;

    auto repo = std::make_shared<fulla::storage::redis::RedisClientRepository>("default");
    runClientRepository_NotFoundContract(TEST_CTX, repo);
}

DROGON_TEST(Integration_P0_Contract_Functional_ClientRepository_Redis_KnownClientValidatesSecret)
{
    auto redis = getRedisClientOrNull();
    if (!redis)
        return;

    const std::string clientId = "contract-redis-client-" + uniqueSuffix();
    const std::string secret = "contract-secret";
    const std::string salt = "contract-salt";
    const std::string hash = ::drogon::utils::getSha256(secret + salt);

    // Seed the client hash directly -- IClientRepository has no write
    // method, and fulla::storage::redis::RedisClientRepository never gained one (see file
    // header).
    waitForVoid([&](auto cb) {
        redis->execCommandAsync(
          [cb](const ::drogon::nosql::RedisResult &) { cb(); },
          [cb](const ::drogon::nosql::RedisException &) { cb(); },
          "HSET oauth2:client:%s secret %s salt %s redirect_uris %s",
          clientId.c_str(),
          hash.c_str(),
          salt.c_str(),
          "[\"http://localhost/cb\"]"
        );
    });

    auto repo = std::make_shared<fulla::storage::redis::RedisClientRepository>("default");
    runClientRepository_Redis_KnownClientHashValidationContract(
      TEST_CTX, repo, clientId, secret, "wrong-secret"
    );

    // Cleanup.
    waitForVoid([&](auto cb) {
        redis->execCommandAsync(
          [cb](const ::drogon::nosql::RedisResult &) { cb(); },
          [cb](const ::drogon::nosql::RedisException &) { cb(); },
          "DEL oauth2:client:%s",
          clientId.c_str()
        );
    });
}

// ===========================================================================
// Memory
// ===========================================================================

DROGON_TEST(Integration_P0_Contract_Functional_ClientRepository_Memory_NotFoundReturnsNullopt)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryClientRepository>();
    runClientRepository_NotFoundContract(TEST_CTX, repo);
}

DROGON_TEST(Integration_P0_Contract_Functional_ClientRepository_Memory_PublicClientAcceptsAnySecret)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryClientRepository>();
    Json::Value cfg;
    cfg["mem-public-client"]["type"] = "PUBLIC";
    cfg["mem-public-client"]["secret"] = "";
    cfg["mem-public-client"]["redirect_uri"] = "http://localhost/cb";
    repo->initFromConfig(cfg);

    runClientRepository_PublicClientAcceptsAnySecretContract(TEST_CTX, repo, "mem-public-client");
}

DROGON_TEST(
  Integration_P0_Contract_Functional_ClientRepository_Memory_ConfidentialClientValidatesSecret
)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryClientRepository>();
    Json::Value cfg;
    cfg["mem-confidential-client"]["type"] = "CONFIDENTIAL";
    cfg["mem-confidential-client"]["secret"] = "test-secret";
    cfg["mem-confidential-client"]["redirect_uri"] = "http://localhost/cb";
    repo->initFromConfig(cfg);

    runClientRepository_ConfidentialClientValidatesSecretContract(
      TEST_CTX, repo, "mem-confidential-client", "test-secret", "wrong-secret"
    );
}

// ===========================================================================
// Coverage additions (P1) -- Memory backend initFromConfig parsing
// branches the original tests did not exercise: redirect_uri as a single
// string vs array, allowed_scopes single-string, the fulla-portal default-
// scopes fallback, the invalid-type -> CONFIDENTIAL fallback, and a null
// config being a no-op.
// ===========================================================================

DROGON_TEST(Integration_P0_Contract_Functional_ClientRepository_Memory_InitFromConfig_RedirectUri_SingleString)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryClientRepository>();
    Json::Value cfg;
    cfg["single-uri-client"]["secret"] = "s";
    cfg["single-uri-client"]["redirect_uri"] = "http://localhost/single";  // string, not array
    repo->initFromConfig(cfg);

    auto client = waitForValue<std::optional<OAuth2Client>>([&](auto cb) {
        repo->getClient("single-uri-client", std::move(cb));
    });
    REQUIRE(client.has_value());
    REQUIRE(client->redirectUris.size() == 1u);
    CHECK(client->redirectUris[0] == "http://localhost/single");
}

DROGON_TEST(
  Integration_P0_Contract_Functional_ClientRepository_Memory_InitFromConfig_RedirectUri_ArrayPreservesAllValues
)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryClientRepository>();
    const std::string firstUri = "http://127.0.0.1:9987/callback";
    const std::string secondUri = "http://127.0.0.1:9987/alternate";
    Json::Value cfg;
    cfg["array-uri-client"]["client_type"] = "PUBLIC";
    cfg["array-uri-client"]["redirect_uri"] = Json::Value(Json::arrayValue);
    cfg["array-uri-client"]["redirect_uri"].append(firstUri);
    cfg["array-uri-client"]["redirect_uri"].append(secondUri);
    repo->initFromConfig(cfg);

    auto completion = std::make_shared<std::promise<std::optional<OAuth2Client>>>();
    auto result = completion->get_future();
    repo->getClient("array-uri-client", [completion](std::optional<OAuth2Client> client) {
        completion->set_value(std::move(client));
    });
    REQUIRE(result.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto client = result.get();
    REQUIRE(client.has_value());
    REQUIRE(client->redirectUris.size() == 2u);
    CHECK(client->redirectUris[0] == firstUri);
    CHECK(client->redirectUris[1] == secondUri);
}

DROGON_TEST(Integration_P0_Contract_Functional_ClientRepository_Memory_InitFromConfig_AllowedScopes_SingleString)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryClientRepository>();
    Json::Value cfg;
    cfg["single-scope-client"]["secret"] = "s";
    cfg["single-scope-client"]["allowed_scopes"] = "openid";  // string, not array
    repo->initFromConfig(cfg);

    auto client = waitForValue<std::optional<OAuth2Client>>([&](auto cb) {
        repo->getClient("single-scope-client", std::move(cb));
    });
    REQUIRE(client.has_value());
    REQUIRE(client->allowedScopes.size() == 1u);
    CHECK(client->allowedScopes[0] == "openid");
}

DROGON_TEST(Integration_P0_Contract_Functional_ClientRepository_Memory_InitFromConfig_VueClient_DefaultScopes)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryClientRepository>();
    Json::Value cfg;
    // fulla-portal with NO allowed_scopes -> default scopes injected.
    cfg["fulla-portal"]["type"] = "PUBLIC";
    repo->initFromConfig(cfg);

    auto client = waitForValue<std::optional<OAuth2Client>>([&](auto cb) {
        repo->getClient("fulla-portal", std::move(cb));
    });
    REQUIRE(client.has_value());
    REQUIRE(client->allowedScopes.size() == 3u);
    CHECK(client->allowedScopes[0] == "openid");
    CHECK(client->allowedScopes[1] == "profile");
    CHECK(client->allowedScopes[2] == "email");
}

DROGON_TEST(Integration_P0_Contract_Functional_ClientRepository_Memory_InitFromConfig_InvalidType_DefaultsToConfidential)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryClientRepository>();
    Json::Value cfg;
    cfg["bad-type-client"]["type"] = "TOTALLY_INVALID";
    cfg["bad-type-client"]["secret"] = "s";
    repo->initFromConfig(cfg);

    auto client = waitForValue<std::optional<OAuth2Client>>([&](auto cb) {
        repo->getClient("bad-type-client", std::move(cb));
    });
    REQUIRE(client.has_value());
    // Invalid type falls back to CONFIDENTIAL -> a non-empty secret is
    // required for validateClient to pass.
    CHECK(client->clientType == ::fulla::oauth2::model::ClientType::CONFIDENTIAL);
}

DROGON_TEST(Integration_P0_Contract_Functional_ClientRepository_Memory_InitFromConfig_NullConfig_NoOp)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryClientRepository>();
    repo->initFromConfig(Json::Value());  // null -> early return, no clients

    auto client = waitForValue<std::optional<OAuth2Client>>([&](auto cb) {
        repo->getClient("any-client", std::move(cb));
    });
    CHECK(!client.has_value());
}

// The in-memory repository applies the same
// token_endpoint_auth_method rule as the startup seeder, so memory storage
// and postgres storage load the same auth methods from the same config. It
// is also the only backend where this parsing is observable without a
// database. The token endpoint enforces the loaded value (F-017); an empty
// value would silently switch the enforcement off in memory mode.
DROGON_TEST(
  Integration_P1_Contract_Functional_ClientRepository_Memory_InitFromConfig_AuthMethod_DefaultsFollowClientType
)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryClientRepository>();
    Json::Value cfg;
    cfg["public-client"]["client_type"] = "PUBLIC";
    cfg["public-client"]["redirect_uri"] = "http://localhost/cb";
    cfg["confidential-client"]["client_type"] = "CONFIDENTIAL";
    cfg["confidential-client"]["secret"] = "s";
    cfg["confidential-client"]["redirect_uri"] = "http://localhost/cb";
    repo->initFromConfig(cfg);

    auto publicClient = waitForValue<std::optional<OAuth2Client>>([&](auto cb) {
        repo->getClient("public-client", std::move(cb));
    });
    REQUIRE(publicClient.has_value());
    CHECK(publicClient->tokenEndpointAuthMethod == "none");

    auto confidentialClient = waitForValue<std::optional<OAuth2Client>>([&](auto cb) {
        repo->getClient("confidential-client", std::move(cb));
    });
    REQUIRE(confidentialClient.has_value());
    CHECK(confidentialClient->tokenEndpointAuthMethod == "client_secret_basic");
}

DROGON_TEST(
  Integration_P1_Contract_Functional_ClientRepository_Memory_InitFromConfig_AuthMethod_ExplicitValueKept
)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryClientRepository>();
    Json::Value cfg;
    cfg["post-client"]["client_type"] = "CONFIDENTIAL";
    cfg["post-client"]["secret"] = "s";
    cfg["post-client"]["token_endpoint_auth_method"] = "client_secret_post";
    repo->initFromConfig(cfg);

    auto client = waitForValue<std::optional<OAuth2Client>>([&](auto cb) {
        repo->getClient("post-client", std::move(cb));
    });
    REQUIRE(client.has_value());
    CHECK(client->tokenEndpointAuthMethod == "client_secret_post");
}

// An explicit method that contradicts the client type, or an unknown one,
// must not load the client: the alternative is a client row whose declared
// method the token endpoint would then reject every request for.
DROGON_TEST(
  Integration_P1_Contract_Functional_ClientRepository_Memory_InitFromConfig_AuthMethod_IllegalValueSkipsClient
)
{
    auto repo = std::make_shared<fulla::storage::memory::MemoryClientRepository>();
    Json::Value cfg;
    // PUBLIC only allows "none".
    cfg["public-with-secret"]["client_type"] = "PUBLIC";
    cfg["public-with-secret"]["token_endpoint_auth_method"] = "client_secret_post";
    // CONFIDENTIAL only allows the two client_secret_* methods.
    cfg["confidential-none"]["client_type"] = "CONFIDENTIAL";
    cfg["confidential-none"]["secret"] = "s";
    cfg["confidential-none"]["token_endpoint_auth_method"] = "none";
    // Not a method this server knows.
    cfg["unknown-method"]["client_type"] = "CONFIDENTIAL";
    cfg["unknown-method"]["secret"] = "s";
    cfg["unknown-method"]["token_endpoint_auth_method"] = "client_secret_header";
    repo->initFromConfig(cfg);

    for (const char *clientId : {"public-with-secret", "confidential-none", "unknown-method"})
    {
        auto client = waitForValue<std::optional<OAuth2Client>>([&](auto cb) {
            repo->getClient(clientId, std::move(cb));
        });
        CHECK(!client.has_value());
    }
}
