// tests/integration/admin/AdminTokenApiHttpTest.cc
//
// HTTP integration tests for the token-management admin API
// (libs/drogon/src/admin/TokenManagementService.cc +
// controllers/TokenAdminController.cc).
//
// Coverage target: TokenManagementService (339 LOC, 0% today) +
// TokenAdminController (128 LOC).
//
// Storage: Postgres-only. listTokens / revokeToken / revokeByClient /
// revokeByUser all call getDbClient() directly; getOidcKeys is metadata-only
// but still lives under /api/admin/* (requires the admin token, which memory
// mode cannot mint). All cases skip cleanly under memory.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include "HttpTestClient.h"

#include <string>

using fulla::test::http::loginAsAdmin;
using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendDelete;
using fulla::test::http::sendGet;
using fulla::test::http::sendPostJson;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define ADMIN_TOKEN_SKIP_GUARD                                \
    do                                                        \
    {                                                         \
        if (!postgresAvailable() || !serverReachable())       \
        {                                                     \
            CHECK(true);                                      \
            return;                                           \
        }                                                     \
    } while (0)

// ---------------------------------------------------------------------------
// listTokens happy path: GET /api/admin/tokens -> 200 with the paginated
// token list shape {status, tokens, total, page, per_page}. A preceding
// admin login mints an access token, so at least one token exists.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_AdminToken_List_WithAdminToken_Returns200)
{
    ADMIN_TOKEN_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto resp = sendGet("/api/admin/tokens", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    // NOTE: listTokens (unlike the other admin list endpoints) does NOT
    // include a "status":"success" field -- its response shape is just
    // {tokens, total, page, per_page}. Assert the actual fields.
    CHECK(body.isMember("tokens"));
    CHECK(body.isMember("total"));
    CHECK(body.isMember("page"));
    CHECK(body.isMember("per_page"));
}

// ---------------------------------------------------------------------------
// listTokens auth guard: no token -> 401.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminToken_List_NoToken_Returns401)
{
    ADMIN_TOKEN_SKIP_GUARD;

    auto resp = sendGet("/api/admin/tokens");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k401Unauthorized));
}

// ---------------------------------------------------------------------------
// revokeToken not-found branch: DELETE /api/admin/tokens/{unknownPrefix}
// returns 404 VALIDATION_RESOURCE_NOT_FOUND ("Token not found"). Uses a
// plausible-looking prefix that no real token matches.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminToken_Revoke_UnknownPrefix_Returns404)
{
    ADMIN_TOKEN_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto resp = sendDelete("/api/admin/tokens/nonexistentprefix0000000000000000", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k404NotFound));
}

// ---------------------------------------------------------------------------
// revokeTokensByClient empty-client_id branch: POST /api/admin/tokens/
// revoke-by-client with no client_id returns 400 VALIDATION_MISSING_REQUIRED_
// FIELD ("client_id cannot be empty"). Covers the early-validation rejection.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminToken_RevokeByClient_EmptyClientId_Returns400)
{
    ADMIN_TOKEN_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    Json::Value body;
    body["client_id"] = "";
    auto resp = sendPostJson("/api/admin/tokens/revoke-by-client", body, *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k400BadRequest));
}

// ---------------------------------------------------------------------------
// revokeTokensByUser empty-user_id branch: POST /api/admin/tokens/
// revoke-by-user with no user_id returns 400 VALIDATION_MISSING_REQUIRED_FIELD
// ("user_id cannot be empty").
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminToken_RevokeByUser_EmptyUserId_Returns400)
{
    ADMIN_TOKEN_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    Json::Value body;
    body["user_id"] = "";
    auto resp = sendPostJson("/api/admin/tokens/revoke-by-user", body, *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k400BadRequest));
}

// ---------------------------------------------------------------------------
// revokeTokensByClient happy path: POST revoke-by-client against the seeded
// fulla-admin-console client (which has no active tokens at rest after the login
// flow's token was issued to the user, not the client) returns 200 with a
// count. The handler revokes access + refresh tokens by client_id; with zero
// matching rows the count is 0 but the response is still 200 success.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminToken_RevokeByClient_AdminConsole_Returns200)
{
    ADMIN_TOKEN_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    Json::Value body;
    body["client_id"] = "fulla-admin-console";
    auto resp = sendPostJson("/api/admin/tokens/revoke-by-client", body, *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));
    Json::Value out;
    REQUIRE(parseJsonBody(resp, out));
    CHECK(out["status"].asString() == "success");
    CHECK(out.isMember("count"));
}

// ---------------------------------------------------------------------------
// getOidcKeys happy path: GET /api/admin/oidc/keys -> 200 with the JWKS-style
// metadata. This endpoint is metadata-only (no DB), but it still requires the
// admin token, so it runs only under Postgres.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_AdminToken_GetOidcKeys_Returns200)
{
    ADMIN_TOKEN_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto resp = sendGet("/api/admin/oidc/keys", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));
}
