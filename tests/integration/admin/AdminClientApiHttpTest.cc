// tests/integration/admin/AdminClientApiHttpTest.cc
//
// HTTP integration tests for the client-management admin API
// (libs/drogon/src/admin/ClientManagementService.cc + the controller at
// libs/drogon/src/controllers/ClientAdminController.cc). Drives the real
// in-process Drogon app (booted by tests/test_main.cc on port 5555) through
// the 2-step admin OAuth2 login recipe in tests/common/HttpTestClient.h.
//
// Coverage target: ClientManagementService (568 LOC, 0% today) +
// ClientAdminController (188 LOC) + the AuthorizationFilter admin-branch.
// Each case below maps to one branch of the service so the line/branch gain
// is explicit (docs/history/design/http-integration-test-coverage-plan.md
// Phase 2.4 branch matrix).
//
// Storage: Postgres-only. The admin services call drogon::app().getDbClient()
// directly and have no memory path, and memory mode has no admin user that
// can log in. loginAsAdmin() returns nullopt under memory, so every case
// no-ops cleanly with CHECK(true) -- Windows/macOS CI legs stay green while
// contributing zero admin coverage (admin coverage is a Linux-leg concern;
// see plan §B2).
//
// Test-data isolation: create-cases use the service's own UUID-generating
// createClient() (no fixed client_id), then DELETE the created row in the
// test body (no transaction-rollback pattern exists in this suite; see plan
// §I3). The created client_id is captured from the create response.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include "HttpTestClient.h"  // tests/common/

#include <string>

using fulla::test::http::loginAsAdmin;
using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendDelete;
using fulla::test::http::sendGet;
using fulla::test::http::sendPostJson;
using fulla::test::http::sendPutJson;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

// Common guard for every admin client case: skip cleanly when there is no
// Postgres-backed server reachable (memory mode, Windows/macOS CI, or a local
// run without Docker). Returns true when the case should proceed.
#define ADMIN_CLIENT_SKIP_GUARD                                  \
    do                                                           \
    {                                                            \
        if (!postgresAvailable() || !serverReachable())          \
        {                                                        \
            CHECK(true);                                         \
            return;                                              \
        }                                                        \
    } while (0)

// ---------------------------------------------------------------------------
// Phase 1.4 smoke test + Phase 2.4 list branch: GET /api/admin/clients with a
// valid admin token returns 200 and a JSON array body. This is the proof
// point that the whole Phase 1 foundation (HttpTestClient + in-process
// seeder + admin login recipe) works end-to-end against Postgres.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_AdminClient_List_WithAdminToken_Returns200)
{
    ADMIN_CLIENT_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto resp = sendGet("/api/admin/clients", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body["status"].asString() == "success");
    CHECK(body.isMember("clients"));
    CHECK(body["clients"].isArray());
    CHECK(body.isMember("total"));
}

// ---------------------------------------------------------------------------
// No Authorization header -> AuthorizationFilter rejects before the handler
// runs (VALIDATION/AUTH path -> 401 AUTH_TOKEN_INVALID). Covers the filter's
// missing-token branch and confirms the route is actually guarded.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminClient_List_NoToken_Returns401)
{
    ADMIN_CLIENT_SKIP_GUARD;

    auto resp = sendGet("/api/admin/clients");  // no bearer token
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k401Unauthorized));
}

// ---------------------------------------------------------------------------
// Invalid bearer token -> AuthorizationFilter rejects (validateAccessToken
// fails) with 401 AUTH_TOKEN_INVALID. Covers the filter's invalid-token
// branch without needing to mint a non-admin token.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminClient_List_InvalidToken_Returns401)
{
    ADMIN_CLIENT_SKIP_GUARD;

    auto resp = sendGet("/api/admin/clients", "not-a-real-token");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k401Unauthorized));
}

// ---------------------------------------------------------------------------
// Create -> Get -> Delete round-trip (the happy path for the three mutating
// endpoints). Uses the service's own UUID-generating createClient (no
// client_id in the request body), captures the returned client_id, then GETs
// and DELETEs it. Covers createClient + getClient + deleteClient branches.
//
// Combined into one case (rather than three) because they share fixture state
// (the created client_id): splitting would require re-creating the client in
// each case, and the create-then-delete lifecycle is naturally sequential.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_AdminClient_CreateGetDelete_RoundTrip)
{
    ADMIN_CLIENT_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    // Derive a unique-ish name from the token so repeated runs / parallel
    // cases do not collide on a fixed literal. The service assigns client_id
    // itself (a fresh UUID per call), so client_id collisions are impossible;
    // the name just needs to be human-identifiable in the list endpoint.
    Json::Value createBody;
    createBody["name"] = "test-client-" + token->substr(0, 8);
    createBody["redirect_uris"] = "http://localhost:9999/callback";
    createBody["allowed_grant_types"] = "authorization_code";
    createBody["client_type"] = "CONFIDENTIAL";

    auto createResp = sendPostJson("/api/admin/clients", createBody, *token);
    REQUIRE(createResp != nullptr);
    CHECK(statusIs(createResp, drogon::k201Created));
    Json::Value created;
    REQUIRE(parseJsonBody(createResp, created));
    REQUIRE(created.isMember("client_id"));
    REQUIRE(created.isMember("client_secret"));
    const std::string clientId = created["client_id"].asString();
    CHECK(!clientId.empty());

    // GET the created client -> 200 with matching fields.
    auto getResp = sendGet("/api/admin/clients/" + clientId, *token);
    REQUIRE(getResp != nullptr);
    CHECK(statusIs(getResp, drogon::k200OK));
    Json::Value fetched;
    REQUIRE(parseJsonBody(getResp, fetched));
    CHECK(fetched["client_id"].asString() == clientId);
    CHECK(fetched["name"].asString() == createBody["name"].asString());

    // DELETE the created client -> 200 success.
    auto delResp = sendDelete("/api/admin/clients/" + clientId, *token);
    REQUIRE(delResp != nullptr);
    CHECK(statusIs(delResp, drogon::k200OK));

    // Confirm the delete stuck: a follow-up GET now 404s. Also covers the
    // not-found branch of getClient.
    auto afterResp = sendGet("/api/admin/clients/" + clientId, *token);
    REQUIRE(afterResp != nullptr);
    CHECK(statusIs(afterResp, drogon::k404NotFound));
}

// ---------------------------------------------------------------------------
// getClient not-found branch (independent of the round-trip, for ctest -r
// filtering): an unknown client_id returns 404 VALIDATION_RESOURCE_NOT_FOUND.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminClient_Get_UnknownId_Returns404)
{
    ADMIN_CLIENT_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto resp = sendGet("/api/admin/clients/nonexistent-client-id-xyz", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k404NotFound));
}

// ---------------------------------------------------------------------------
// deleteClient not-found branch: deleting an unknown client_id returns 404
// (the service's `if (affected == 0)` branch).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminClient_Delete_UnknownId_Returns404)
{
    ADMIN_CLIENT_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto resp = sendDelete("/api/admin/clients/nonexistent-client-id-xyz", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k404NotFound));
}

// ---------------------------------------------------------------------------
// updateClient happy path + the "no fields to update" 400 branch. Creates a
// throwaway client, updates its name, then PUTs an empty body (400), then
// cleans up. Covers the update findOne -> update -> success path and the
// early VALIDATION_INVALID_INPUT rejection.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminClient_Update_AndEmptyBody_Returns200Then400)
{
    ADMIN_CLIENT_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    // Create throwaway client.
    Json::Value createBody;
    createBody["name"] = "update-target";
    createBody["redirect_uris"] = "http://localhost:9998/callback";
    createBody["client_type"] = "CONFIDENTIAL";
    auto createResp = sendPostJson("/api/admin/clients", createBody, *token);
    REQUIRE(createResp != nullptr);
    Json::Value created;
    REQUIRE(parseJsonBody(createResp, created));
    const std::string clientId = created["client_id"].asString();
    REQUIRE(!clientId.empty());

    // Update name -> 200.
    Json::Value updateBody;
    updateBody["name"] = "updated-name";
    auto updResp = sendPutJson("/api/admin/clients/" + clientId, updateBody, *token);
    REQUIRE(updResp != nullptr);
    CHECK(statusIs(updResp, drogon::k200OK));

    // Empty JSON object (no updatable fields) -> 400 VALIDATION_INVALID_INPUT.
    Json::Value emptyBody;
    auto badResp = sendPutJson("/api/admin/clients/" + clientId, emptyBody, *token);
    REQUIRE(badResp != nullptr);
    CHECK(statusIs(badResp, drogon::k400BadRequest));

    // Cleanup.
    sendDelete("/api/admin/clients/" + clientId, *token);
}

// ---------------------------------------------------------------------------
// token_endpoint_auth_method is an updatable field.
//   * CONFIDENTIAL client -> client_secret_post: 200, and a follow-up GET
//     shows the new value (the seeded default was client_secret_basic, which
//     is exactly why the WeKnora token exchange 401'd and had no API surface
//     to fix).
//   * Unknown value -> 400 VALIDATION_INVALID_INPUT.
//   * Illegal combination (CONFIDENTIAL row cannot become "none") -> 400.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminClient_Update_AuthMethod_EnforcedCombinations)
{
    ADMIN_CLIENT_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    // CONFIDENTIAL throwaway client.
    Json::Value createBody;
    createBody["name"] = "auth-method-target";
    createBody["redirect_uris"] = "http://localhost:9996/callback";
    createBody["client_type"] = "CONFIDENTIAL";
    auto createResp = sendPostJson("/api/admin/clients", createBody, *token);
    REQUIRE(createResp != nullptr);
    Json::Value created;
    REQUIRE(parseJsonBody(createResp, created));
    const std::string clientId = created["client_id"].asString();
    REQUIRE(!clientId.empty());

    // Switch the RP to client_secret_post -> 200, persisted.
    Json::Value postBody;
    postBody["token_endpoint_auth_method"] = "client_secret_post";
    auto postResp = sendPutJson("/api/admin/clients/" + clientId, postBody, *token);
    REQUIRE(postResp != nullptr);
    CHECK(statusIs(postResp, drogon::k200OK));
    auto getResp = sendGet("/api/admin/clients/" + clientId, *token);
    REQUIRE(getResp != nullptr);
    Json::Value fetched;
    REQUIRE(parseJsonBody(getResp, fetched));
    CHECK(fetched.get("token_endpoint_auth_method", "").asString() == "client_secret_post");

    // Unknown value -> 400.
    Json::Value bogusBody;
    bogusBody["token_endpoint_auth_method"] = "client_secret_header";
    auto bogusResp = sendPutJson("/api/admin/clients/" + clientId, bogusBody, *token);
    REQUIRE(bogusResp != nullptr);
    CHECK(statusIs(bogusResp, drogon::k400BadRequest));

    // Combination violation: a CONFIDENTIAL row cannot declare "none".
    Json::Value noneBody;
    noneBody["token_endpoint_auth_method"] = "none";
    auto noneResp = sendPutJson("/api/admin/clients/" + clientId, noneBody, *token);
    REQUIRE(noneResp != nullptr);
    CHECK(statusIs(noneResp, drogon::k400BadRequest));

    // Cleanup.
    sendDelete("/api/admin/clients/" + clientId, *token);
}

// ---------------------------------------------------------------------------
// resetClientSecret happy path: creates a client, resets its secret, asserts
// a new secret is returned. Covers the findOne -> update secret -> success
// branch and the not-found branch (delegated to the Get_UnknownId case above
// for getClient; resetClientSecret's own not-found is structurally identical).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminClient_ResetSecret_ReturnsNewSecret)
{
    ADMIN_CLIENT_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    // Create throwaway client.
    Json::Value createBody;
    createBody["name"] = "reset-target";
    createBody["redirect_uris"] = "http://localhost:9997/callback";
    createBody["client_type"] = "CONFIDENTIAL";
    auto createResp = sendPostJson("/api/admin/clients", createBody, *token);
    REQUIRE(createResp != nullptr);
    Json::Value created;
    REQUIRE(parseJsonBody(createResp, created));
    const std::string clientId = created["client_id"].asString();
    const std::string originalSecret = created["client_secret"].asString();
    REQUIRE(!clientId.empty());

    // Reset secret -> 200 with a NEW secret that differs from the original.
    auto resetResp = sendPostJson(
      "/api/admin/clients/" + clientId + "/reset-secret", Json::Value::nullSingleton(), *token);
    REQUIRE(resetResp != nullptr);
    CHECK(statusIs(resetResp, drogon::k200OK));
    Json::Value resetBody;
    REQUIRE(parseJsonBody(resetResp, resetBody));
    CHECK(resetBody["client_secret"].isString());
    CHECK(resetBody["client_secret"].asString() != originalSecret);

    // Cleanup.
    sendDelete("/api/admin/clients/" + clientId, *token);
}
