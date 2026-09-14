// tests/integration/admin/UserAdminHardeningTest.cc
//
// Integration tests for the issues-53-60 hardening batch on the user admin
// API + self-service surface (design: .zcode/plans/issues-53-60-design.md):
//
//   #53  strict JSON type validation on updateUser/createUser (400, no crash)
//   #56  deleteUser revokes tokens durably (dual key) before responding
//   #58  case-insensitive user search (lower() on both sides)
//   #59  org_id nullable semantics (null ≠ 0, null clears, string type → 400)
//   #54  soft-deleted user's self-service token no longer returns data
//   #60  createUser role-assignment reporting + last-admin guard (409)
//
// Storage: Postgres-only (same guard pattern as AdminUserApiHttpTest.cc).
// Throwaway users use the same conventional ephemeral test credential as
// AdminUserApiHttpTest.cc (never a real secret; users are deleted or remain
// inert rows in the dev-only test DB).

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include "HttpTestClient.h"

#include <chrono>
#include <string>
#include <vector>

using fulla::test::http::loginAsAdmin;
using fulla::test::http::loginAsUserTokens;
using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendDelete;
using fulla::test::http::sendGet;
using fulla::test::http::sendPostForm;
using fulla::test::http::sendPostJson;
using fulla::test::http::sendPutJson;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define HARDENING_SKIP_GUARD                                    \
    do                                                          \
    {                                                           \
        if (!postgresAvailable() || !serverReachable())         \
        {                                                       \
            CHECK(true);                                        \
            return;                                             \
        }                                                       \
    } while (0)

namespace
{
std::string uniqueSuffix()
{
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::to_string(now % 1000000);
}

// Create a throwaway user via the admin API; returns its id (-1 on failure).
int createThrowawayUser(const std::string &token, const std::string &prefix)
{
    Json::Value body;
    body["username"] = prefix + "_" + uniqueSuffix();
    body["password"] = "TestPass123!";
    auto resp = sendPostJson("/api/admin/users", body, token);
    if (!resp || !statusIs(resp, drogon::k201Created))
        return -1;
    Json::Value respBody;
    if (!parseJsonBody(resp, respBody))
        return -1;
    return respBody["user"]["id"].asInt();
}

// Create a throwaway organization via the admin API and return its id (-1 on
// failure). users.org_id has an FK to organizations(id), so org tests must
// reference a REAL org.
int createThrowawayOrg(const std::string &token)
{
    Json::Value body;
    body["name"] = "HardeningOrg_" + uniqueSuffix();
    body["slug"] = "hardening-org-" + uniqueSuffix();
    auto resp = sendPostJson("/api/admin/organizations", body, token);
    if (!resp || !statusIs(resp, drogon::k201Created))
        return -1;
    Json::Value respBody;
    if (!parseJsonBody(resp, respBody))
        return -1;
    return respBody.get("id", -1).asInt();
}
}  // namespace

// ---------------------------------------------------------------------------
// #53: wrong-typed optional fields are a 400, never a crash (previously
// asString/asBool threw Json::LogicError inside the async DB callback ->
// SIGABRT) and never a silent skip that answers 200.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_AdminUser_Update_TypeMismatch_Returns400)
{
    HARDENING_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    const int userId = createThrowawayUser(*token, "typetest");
    REQUIRE(userId > 0);

    struct Case
    {
        const char *field;
        Json::Value value;
    };
    Json::Value objVal;
    objVal["a"] = 1;
    Json::Value intVal(123);
    Json::Value strVal1("yes");
    Json::Value strVal2("true");
    Json::Value intVal2(1);
    Json::Value strVal3("abc");
    std::vector<Case> cases = {
        {"email", objVal},
        {"email_verified", strVal1},
        {"username", intVal},
        {"mfa_enabled", strVal2},
        {"locked", intVal2},
        {"org_id", strVal3},
    };
    for (const auto &c : cases)
    {
        Json::Value body;
        body[c.field] = c.value;
        auto resp = sendPutJson("/api/admin/users/" + std::to_string(userId), body, *token);
        REQUIRE(resp != nullptr);
        // Wrong-typed 'field' must be rejected with 400 (not 200, not a crash).
        CHECK(statusIs(resp, drogon::k400BadRequest));
    }
}

// ---------------------------------------------------------------------------
// #59: org_id is a nullable integer. NULL serializes as JSON null (never the
// default 0), explicit null clears, non-int types are a 400.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_AdminUser_OrgId_NullSemantics)
{
    HARDENING_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    const int userId = createThrowawayUser(*token, "orgtest");
    REQUIRE(userId > 0);
    // org_id has an FK to organizations(id) — use a real org.
    const int orgId = createThrowawayOrg(*token);
    REQUIRE(orgId > 0);

    // Fresh user: org_id must be JSON null (not 0).
    {
        auto getResp = sendGet("/api/admin/users/" + std::to_string(userId), *token);
        REQUIRE(getResp != nullptr);
        Json::Value body;
        REQUIRE(parseJsonBody(getResp, body));
        CHECK(body.isMember("org_id"));
        CHECK(body["org_id"].isNull());
    }

    // Set to the real org id.
    {
        Json::Value body;
        body["org_id"] = orgId;
        auto resp = sendPutJson("/api/admin/users/" + std::to_string(userId), body, *token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, drogon::k200OK));
        auto getResp = sendGet("/api/admin/users/" + std::to_string(userId), *token);
        REQUIRE(getResp != nullptr);
        Json::Value body2;
        REQUIRE(parseJsonBody(getResp, body2));
        CHECK(body2["org_id"].isInt());
        CHECK(body2["org_id"].asInt() == orgId);
    }

    // Explicit null clears.
    {
        Json::Value body;
        body["org_id"] = Json::Value(Json::nullValue);
        auto resp = sendPutJson("/api/admin/users/" + std::to_string(userId), body, *token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, drogon::k200OK));
        auto getResp = sendGet("/api/admin/users/" + std::to_string(userId), *token);
        REQUIRE(getResp != nullptr);
        Json::Value body2;
        REQUIRE(parseJsonBody(getResp, body2));
        CHECK(body2["org_id"].isNull());
    }
}

// ---------------------------------------------------------------------------
// #58: search is case-insensitive on both sides (lower(col) LIKE lower(q)).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_AdminUser_Search_CaseInsensitive)
{
    HARDENING_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    // Username with mixed case; the unique suffix makes collisions impossible.
    // "CaseProbe_" is 10 chars.
    const std::string name = "CaseProbe_" + uniqueSuffix();
    const std::string lowered = "caseprobe_" + name.substr(10);
    Json::Value body;
    body["username"] = name;
    body["password"] = "TestPass123!";
    auto cr = sendPostJson("/api/admin/users", body, *token);
    REQUIRE(cr != nullptr);
    REQUIRE(statusIs(cr, drogon::k201Created));

    for (const std::string &q : {name, lowered, "CASEPROBE_" + lowered.substr(10)})
    {
        auto resp = sendGet("/api/admin/users?q=" + q, *token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, drogon::k200OK));
        Json::Value respBody;
        REQUIRE(parseJsonBody(resp, respBody));
        bool found = false;
        for (const auto &u : respBody["users"])
        {
            if (u.get("username", "").asString() == name)
            {
                found = true;
                break;
            }
        }
        // Case-insensitive search must find the user for every casing of q.
        CHECK(found);
    }
}

// ---------------------------------------------------------------------------
// #60 item 1: createUser reports role-assignment outcome honestly.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminUser_Create_RoleAssignmentReporting)
{
    HARDENING_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    // Normal create: default 'user' role lands and is reported.
    {
        const std::string username = "roletest_" + uniqueSuffix();
        Json::Value body;
        body["username"] = username;
        body["password"] = "TestPass123!";
        auto resp = sendPostJson("/api/admin/users", body, *token);
        REQUIRE(resp != nullptr);
        REQUIRE(statusIs(resp, drogon::k201Created));
        Json::Value respBody;
        REQUIRE(parseJsonBody(resp, respBody));
        CHECK(respBody.isMember("roles_assigned"));
        bool userAssigned = false;
        for (const auto &r : respBody["roles_assigned"])
        {
            if (r.asString() == "user")
                userAssigned = true;
        }
        CHECK(userAssigned);
    }
    // Unknown role name: user created, but the failure is reported.
    {
        Json::Value body;
        body["username"] = "roletest2_" + uniqueSuffix();
        body["password"] = "TestPass123!";
        body["roles"] = Json::Value(Json::arrayValue);
        body["roles"].append("ghost_role_zzz");
        auto resp = sendPostJson("/api/admin/users", body, *token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, drogon::k201Created));
        Json::Value respBody;
        REQUIRE(parseJsonBody(resp, respBody));
        CHECK(respBody.isMember("roles_assigned"));
        CHECK(respBody.isMember("roles_failed"));
        bool ghostFailed = false;
        for (const auto &r : respBody["roles_failed"])
        {
            if (r.asString() == "ghost_role_zzz")
                ghostFailed = true;
        }
        CHECK(ghostFailed);
    }
}

// ---------------------------------------------------------------------------
// #56: deleteUser revokes outstanding tokens (dual key: public sub + internal
// id) BEFORE responding; the response reports tokens_revoked.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_AdminUser_Delete_RevokesRefreshToken)
{
    HARDENING_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    // Create a user with a known credential and log in to obtain a refresh
    // token.
    const std::string username = "revoketest_" + uniqueSuffix();
    Json::Value createBody;
    createBody["username"] = username;
    createBody["password"] = "TestPass123!";
    auto cr = sendPostJson("/api/admin/users", createBody, *token);
    REQUIRE(cr != nullptr);
    REQUIRE(statusIs(cr, drogon::k201Created));
    Json::Value crBody;
    REQUIRE(parseJsonBody(cr, crBody));
    const int userId = crBody["user"]["id"].asInt();

    auto tokens = loginAsUserTokens(username, "TestPass123!", "openid profile");
    REQUIRE(tokens.has_value());
    const std::string refreshToken = tokens->get("refresh_token", "").asString();
    REQUIRE(!refreshToken.empty());

    // Soft-delete the user.
    auto delResp = sendDelete("/api/admin/users/" + std::to_string(userId), *token);
    REQUIRE(delResp != nullptr);
    CHECK(statusIs(delResp, drogon::k200OK));
    Json::Value delBody;
    REQUIRE(parseJsonBody(delResp, delBody));
    CHECK(delBody.get("tokens_revoked", false).asBool());

    // The old refresh token must now be refused (revoked, durable).
    const std::string refreshForm =
      "grant_type=refresh_token&refresh_token=" + refreshToken +
      "&client_id=fulla-portal&client_secret=123456";
    auto refreshResp = sendPostForm("/oauth2/token", refreshForm);
    REQUIRE(refreshResp != nullptr);
    CHECK(refreshResp->getStatusCode() != drogon::k200OK);
}

// ---------------------------------------------------------------------------
// #54: after soft-delete, the user's self-service endpoint never returns
// their data (token revoked -> 401, or 404 through the deleted filter).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_SelfService_DeletedUser_NoDataLeak)
{
    HARDENING_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    const std::string username = "medeletetest_" + uniqueSuffix();
    Json::Value createBody;
    createBody["username"] = username;
    createBody["password"] = "TestPass123!";
    auto cr = sendPostJson("/api/admin/users", createBody, *token);
    REQUIRE(cr != nullptr);
    REQUIRE(statusIs(cr, drogon::k201Created));
    Json::Value crBody;
    REQUIRE(parseJsonBody(cr, crBody));
    const int userId = crBody["user"]["id"].asInt();

    auto tokens = loginAsUserTokens(username, "TestPass123!", "openid profile");
    REQUIRE(tokens.has_value());
    const std::string accessToken = tokens->get("access_token", "").asString();
    REQUIRE(!accessToken.empty());

    // Sanity: profile readable before delete.
    {
        auto resp = sendGet("/api/me", accessToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, drogon::k200OK));
    }

    // Soft-delete via admin.
    auto delResp = sendDelete("/api/admin/users/" + std::to_string(userId), *token);
    REQUIRE(delResp != nullptr);
    CHECK(statusIs(delResp, drogon::k200OK));

    // After delete: 401 (revoked token) or 404 (deleted filter) — never 200
    // with profile data.
    auto resp = sendGet("/api/me", accessToken);
    REQUIRE(resp != nullptr);
    // finalStatus shows up in the failure expansion for diagnosability.
    const int finalStatus = static_cast<int>(resp->getStatusCode());
    const bool acceptable =
      (finalStatus == static_cast<int>(drogon::k401Unauthorized) ||
       finalStatus == static_cast<int>(drogon::k404NotFound));
    CHECK(acceptable);
}

// ---------------------------------------------------------------------------
// #60 item 2: last-active-admin guard. The seeded admin is the only active
// admin in the test DB, so self-targeted lockout operations must 409; once a
// second admin exists they succeed; the throwaway admin is cleaned up.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_AdminUser_LastAdminGuard_409)
{
    HARDENING_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    // #72 rerun-safety sweep: this test's premise is "the seeded admin is
    // the ONLY active admin". A previous full-test round leaves admin-role
    // testuser_* rows behind (the endpoint suites create them for
    // role/lockout tests; their lockouts then EXPIRE, which the
    // isLastActiveAdmin liveness rule counts as active again), silently
    // defeating every 409 below. Demote every non-seed admin (including
    // this test's own leftover throwaway from an aborted run) so the
    // premise is re-established regardless of prior state.
    try
    {
        auto db = drogon::app().getDbClient();
        // DELETE the grant (not UPDATE-to-user): most strays already hold the
        // 'user' role, and repointing would violate user_roles_pkey
        // (user_id, role_id) -- the first version of this sweep threw exactly
        // there and silently no-opped behind this catch.
        db->execSqlSync(
          "DELETE FROM user_roles "
          "WHERE role_id = (SELECT id FROM roles WHERE name = 'admin') "
          "AND user_id IN (SELECT id FROM users WHERE username <> 'admin')"
        );
    }
    catch (const std::exception &e)
    {
        // Non-fatal by design: on a clean DB the sweep is a no-op; if it
        // fails here the assertions below will name the real problem.
        LOG_WARN << "LastAdminGuard pre-sweep failed (continuing): " << e.what();
    }

    // Resolve the seeded admin's id.
    auto listResp = sendGet("/api/admin/users?q=admin", *token);
    REQUIRE(listResp != nullptr);
    Json::Value listBody;
    REQUIRE(parseJsonBody(listResp, listBody));
    int adminId = -1;
    for (const auto &u : listBody["users"])
    {
        if (u.get("username", "").asString() == "admin")
            adminId = u.get("id", -1).asInt();
    }
    REQUIRE(adminId > 0);

    // Self-disable while last admin -> 409.
    {
        auto resp = sendPutJson(
          "/api/admin/users/" + std::to_string(adminId) + "/disable",
          Json::Value::nullSingleton(), *token
        );
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, drogon::k409Conflict));
    }
    // Self-lock while last admin -> 409.
    {
        Json::Value body;
        body["locked"] = true;
        auto resp = sendPutJson("/api/admin/users/" + std::to_string(adminId), body, *token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, drogon::k409Conflict));
    }
    // Strip own admin role while last admin -> 409.
    {
        Json::Value body;
        body["roles"] = Json::Value(Json::arrayValue);
        body["roles"].append("user");
        auto resp =
          sendPutJson("/api/admin/users/" + std::to_string(adminId) + "/roles", body, *token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, drogon::k409Conflict));
    }
    // Self-delete remains blocked by the (older) self-delete guard -> 400.
    {
        auto resp = sendDelete("/api/admin/users/" + std::to_string(adminId), *token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, drogon::k400BadRequest));
    }

    // Promote a second admin: the same operations on the throwaway admin are
    // now allowed (another active admin exists).
    const int userId = createThrowawayUser(*token, "lastadm");
    REQUIRE(userId > 0);
    {
        Json::Value body;
        body["roles"] = Json::Value(Json::arrayValue);
        body["roles"].append("admin");
        auto resp =
          sendPutJson("/api/admin/users/" + std::to_string(userId) + "/roles", body, *token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, drogon::k200OK));
    }
    {
        auto resp = sendPutJson(
          "/api/admin/users/" + std::to_string(userId) + "/disable",
          Json::Value::nullSingleton(), *token
        );
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, drogon::k200OK));
    }
    // Re-enable, then delete (the seeded admin stays active throughout).
    {
        auto resp = sendPostJson(
          "/api/admin/users/" + std::to_string(userId) + "/enable",
          Json::Value::nullSingleton(), *token
        );
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, drogon::k200OK));
    }
    {
        auto resp = sendDelete("/api/admin/users/" + std::to_string(userId), *token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, drogon::k200OK));
    }
}
