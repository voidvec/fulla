// tests/integration/admin/AdminRoleScopeApiHttpTest.cc
//
// HTTP integration tests for the role/scope-management admin API
// (libs/drogon/src/admin/RoleScopeAdminService.cc +
// controllers/RoleScopeAdminController.cc).
//
// Coverage target: RoleScopeAdminService (593 LOC, 0% today) +
// RoleScopeAdminController (184 LOC).
//
// Storage: Postgres-only (admin services call getDbClient() directly; memory
// mode has no admin login). All cases skip cleanly under memory.
//
// Test-data isolation: roles and scopes have create + delete admin routes, so
// mutating cases create a uniquely-named row, exercise it, and delete it in
// the same case (no rollback pattern exists; the dev-seeded `admin` role and
// default scopes are left untouched). Scope create uses a unique name suffix
// to avoid colliding with seed scopes or prior runs.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include "HttpTestClient.h"

#include <chrono>
#include <future>
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

#define ADMIN_ROLESCOPE_SKIP_GUARD                            \
    do                                                        \
    {                                                         \
        if (!postgresAvailable() || !serverReachable())       \
        {                                                     \
            CHECK(true);                                      \
            return;                                           \
        }                                                     \
    } while (0)

namespace
{
// Collision-resistant suffix for created role/scope names so repeated runs
// against a persistent DB do not trip the 409 "name already exists" branch
// unintentionally (that branch IS tested, but deliberately, below).
std::string uniqueSuffix()
{
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::to_string(now % 1000000);
}
}  // namespace

// ---------------------------------------------------------------------------
// listRoles happy path: GET /api/admin/roles -> 200 with a roles array. The
// seeded DB always has the `admin` and `user` roles, so this exercises the
// roles-with-user-counts branch.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_AdminRole_List_WithAdminToken_Returns200)
{
    ADMIN_ROLESCOPE_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto resp = sendGet("/api/admin/roles", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body["status"].asString() == "success");
    CHECK(body["roles"].isArray());
    CHECK(body.isMember("total"));
}

// ---------------------------------------------------------------------------
// listRoles auth guard: no token -> 401.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminRole_List_NoToken_Returns401)
{
    ADMIN_ROLESCOPE_SKIP_GUARD;

    auto resp = sendGet("/api/admin/roles");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k401Unauthorized));
}

// ---------------------------------------------------------------------------
// createRole -> deleteRole round-trip (happy path): POST /api/admin/roles
// with a unique name returns 201 with the new id; DELETE /api/admin/roles/{id}
// returns 200. Covers both branches. Name uniqueness avoids the 409.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_AdminRole_CreateDelete_RoundTrip)
{
    ADMIN_ROLESCOPE_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    Json::Value createBody;
    createBody["name"] = "test-role-" + uniqueSuffix();
    createBody["description"] = "integration-test role";
    auto createResp = sendPostJson("/api/admin/roles", createBody, *token);
    REQUIRE(createResp != nullptr);
    CHECK(statusIs(createResp, drogon::k201Created));
    Json::Value created;
    REQUIRE(parseJsonBody(createResp, created));
    REQUIRE(created.isMember("id"));
    const int roleId = created["id"].asInt();
    CHECK(created["name"].asString() == createBody["name"].asString());

    // Delete -> 200.
    auto delResp = sendDelete("/api/admin/roles/" + std::to_string(roleId), *token);
    REQUIRE(delResp != nullptr);
    CHECK(statusIs(delResp, drogon::k200OK));
}

// ---------------------------------------------------------------------------
// createRole empty-name branch: name="" -> 400 VALIDATION_MISSING_REQUIRED_FIELD
// ("Role name cannot be empty"). Covers the early-validation rejection.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminRole_Create_EmptyName_Returns400)
{
    ADMIN_ROLESCOPE_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    Json::Value badBody;
    badBody["name"] = "";
    auto resp = sendPostJson("/api/admin/roles", badBody, *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k400BadRequest));
}

// ---------------------------------------------------------------------------
// updateRole non-integer id branch: PUT /api/admin/roles/not-an-int -> 400
// ("roleId must be an integer"). Covers the stoi exception path.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminRole_Update_NonIntegerId_Returns400)
{
    ADMIN_ROLESCOPE_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    Json::Value body;
    body["description"] = "x";
    auto resp = sendPutJson("/api/admin/roles/not-an-int", body, *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k400BadRequest));
}

// ---------------------------------------------------------------------------
// listScopes happy path: GET /api/admin/scopes -> 200 with a scopes array.
// The seeded DB has openid/profile/email/admin scopes.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_AdminScope_List_WithAdminToken_Returns200)
{
    ADMIN_ROLESCOPE_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto resp = sendGet("/api/admin/scopes", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body["status"].asString() == "success");
    CHECK(body["scopes"].isArray());
    CHECK(body.isMember("total"));
}

// ---------------------------------------------------------------------------
// createScope -> deleteScope round-trip: POST /api/admin/scopes with a unique
// name returns 201; DELETE removes it. Covers the scope CRUD happy path.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_AdminScope_CreateDelete_RoundTrip)
{
    ADMIN_ROLESCOPE_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    Json::Value createBody;
    createBody["name"] = "test-scope-" + uniqueSuffix();
    createBody["description"] = "integration-test scope";
    createBody["mapped_role"] = "admin";
    createBody["is_default"] = false;
    createBody["requires_admin_role"] = true;
    auto createResp = sendPostJson("/api/admin/scopes", createBody, *token);
    REQUIRE(createResp != nullptr);
    CHECK(statusIs(createResp, drogon::k201Created));
    Json::Value created;
    REQUIRE(parseJsonBody(createResp, created));
    REQUIRE(created.isMember("id"));
    const int scopeId = created["id"].asInt();
    CHECK(created["name"].asString() == createBody["name"].asString());

    // Delete -> 200.
    auto delResp = sendDelete("/api/admin/scopes/" + std::to_string(scopeId), *token);
    REQUIRE(delResp != nullptr);
    CHECK(statusIs(delResp, drogon::k200OK));
}

// ---------------------------------------------------------------------------
// createScope duplicate-name branch: creating a scope whose name already
// exists returns 409 VALIDATION_RESOURCE_CONFLICT. Uses the seeded `openid`
// scope (guaranteed to exist), so no cleanup is needed.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminScope_Create_DuplicateName_Returns409)
{
    ADMIN_ROLESCOPE_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    Json::Value dupBody;
    dupBody["name"] = "openid";  // seeded by V006__oauth2_scopes.sql
    dupBody["mapped_role"] = "admin";
    auto resp = sendPostJson("/api/admin/scopes", dupBody, *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k409Conflict));
}

// ---------------------------------------------------------------------------
// V036 org scope seed: the `org` scope exists in the admin listing AND is
// flagged self_service (the self-registered-application allowlist) in the
// DB. The list API does not expose self_service, so the flag is asserted
// by direct SQL -- the API leg proves the seed is visible where M1 will
// consume it. Idempotency (ON CONFLICT DO NOTHING) is exercised by every
// db-reset chain replay; this case pins the END state.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P2_AdminScope_OrgSeed_PlantedSelfService)
{
    ADMIN_ROLESCOPE_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto resp = sendGet("/api/admin/scopes", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body["scopes"].isArray());
    bool hasOrg = false;
    for (const auto &scope : body["scopes"])
    {
        if (scope["name"].asString() == "org")
            hasOrg = true;
    }
    CHECK(hasOrg);

    auto db = ::drogon::app().getDbClient();
    std::promise<bool> selfService;
    db->execSqlAsync(
      "SELECT self_service FROM oauth2_scopes WHERE name = 'org'",
      [&selfService](const ::drogon::orm::Result &r) {
          selfService.set_value(!r.empty() && r[0][0].as<bool>());
      },
      [&selfService](const ::drogon::orm::DrogonDbException &) {
          selfService.set_value(false);
      });
    CHECK(selfService.get_future().get());
}
