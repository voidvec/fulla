// tests/integration/admin/AdminAuditExportHttpTest.cc
//
// P1-6 (#283): HTTP integration tests for GET /api/admin/logs/export
// (libs/drogon/src/admin/AuditService.cc exportLogs).
//
// Coverage: mandatory-window guard, window validation, CSV happy path with
// formula-injection fixtures (user_agent/ip/details are fully
// user-controlled — the export is the injection egress), the per-request
// `limit` cap branch (AUDIT_EXPORT_TOO_LARGE), all=true, auth (401 no
// token / 403 regular user via the AuthorizationFilter RBAC gate), and the
// audit_exported self-audit row.
//
// Storage: Postgres-only; skips cleanly under memory/unreachable server.
// Fixtures are inserted directly and removed in the same case.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <fulla/drogon/utils/CryptoUtils.h>

#include "HttpTestClient.h"

#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <vector>

using fulla::test::http::loginAsAdmin;
using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendGet;
using fulla::test::http::sendPostForm;
using fulla::test::http::sendPostJson;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define AUDIT_EXPORT_SKIP_GUARD                                 \
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
    return std::to_string(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count() % 100000000);
}

// Same register + verify + 2-step login recipe as
// AdminOrganizationOwnerTransferHttpTest (regular-user token for the RBAC
// 403 case).
bool createVerifiedUser(const std::string &username, const std::string &password)
{
    Json::Value body;
    body["username"] = username;
    body["password"] = password;
    body["email"] = username + "@qa.example";
    auto resp = fulla::test::http::sendPostJson("/api/register", body);
    if (resp == nullptr || resp->getStatusCode() >= 300)
        return false;
    auto db = ::drogon::app().getDbClient();
    std::promise<bool> done;
    db->execSqlAsync(
      "UPDATE users SET email_verified = true WHERE username = $1",
      [&done](const ::drogon::orm::Result &) { done.set_value(true); },
      [&done](const ::drogon::orm::DrogonDbException &) { done.set_value(false); },
      username);
    return done.get_future().get();
}

std::optional<std::string> loginTokenVerbose(const std::string &username, const std::string &password)
{
    const std::string codeVerifier = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string codeChallenge =
      ::fulla::drogon::utils::computeCodeChallenge(codeVerifier, "S256");
    const std::string loginForm =
      "username=" + username + "&password=" + password +
      "&client_id=fulla-admin-console"
      "&redirect_uri=http://localhost:5174/admin/callback"
      "&scope=openid%20profile&state=t1"
      "&code_challenge=" + codeChallenge + "&code_challenge_method=S256";
    auto loginResp = fulla::test::http::sendPostForm("/oauth2/login?json=true", loginForm);
    if (loginResp == nullptr || loginResp->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    Json::Value loginJson;
    if (!parseJsonBody(loginResp, loginJson))
        return std::nullopt;
    const std::string code = loginJson.get("code", "").asString();
    if (code.empty())
        return std::nullopt;
    const std::string tokenForm =
      "grant_type=authorization_code&code=" + code +
      "&redirect_uri=http://localhost:5174/admin/callback"
      "&client_id=fulla-admin-console&client_secret=&code_verifier=" + codeVerifier;
    auto tokenResp = fulla::test::http::sendPostForm("/oauth2/token", tokenForm);
    if (tokenResp == nullptr || tokenResp->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    Json::Value tokenJson;
    if (!parseJsonBody(tokenResp, tokenJson))
        return std::nullopt;
    const std::string access = tokenJson.get("access_token", "").asString();
    if (access.empty())
        return std::nullopt;
    return access;
}

// Insert audit rows with malicious user-controlled fields (cleanup is by
// marker; the ids are not needed). Synchronous futures are acceptable in
// test code.
void insertMaliciousFixtures(const std::string &marker)
{
    auto db = ::drogon::app().getDbClient();
    std::vector<std::pair<std::string, std::string>> fixtures = {
      {"=SUM(A1:A9)|cmd", "+1.234"},
      {"@import 'evil'", "-2+3-4"},
      {"\ttab-prefixed", "@SUMMARY(1,2)"},
    };
    for (const auto &[ua, ip] : fixtures)
    {
        auto f = db->execSqlAsyncFuture(
          "INSERT INTO audit_logs (actor_type, actor_id, action, target_type, target_id, "
          "outcome, ip, user_agent, details, timestamp) VALUES "
          "('user', 'fixture', $1, 'fixture', $2, 'success', $3, $4, "
          "'{\"formula\": \"=HYPERLINK(\\\"http://evil\\\")\"}', now()) RETURNING id",
          marker, marker, ip, ua);
        f.get();
    }
}

void deleteFixtures(const std::string &marker)
{
    auto db = ::drogon::app().getDbClient();
    auto f = db->execSqlAsyncFuture(
      "DELETE FROM audit_logs WHERE target_id = $1 AND actor_id = 'fixture'", marker);
    f.get();
}
}  // namespace

// ---------------------------------------------------------------------------
// Window guards.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AuditExport_WindowRequired_WithoutAll)
{
    AUDIT_EXPORT_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto resp = sendGet("/api/admin/logs/export", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, ::drogon::k400BadRequest));
    Json::Value err;
    REQUIRE(parseJsonBody(resp, err));
    CHECK(err["error"]["code"].asString() == "AUDIT_EXPORT_WINDOW_REQUIRED");

    // Reversed window is rejected as plain invalid input.
    const int64_t now = ::trantor::Date::now().secondsSinceEpoch();
    auto reversed =
      sendGet("/api/admin/logs/export?from=" + std::to_string(now) + "&to=" + std::to_string(now - 60), *token);
    REQUIRE(reversed != nullptr);
    CHECK(statusIs(reversed, ::drogon::k400BadRequest));
}

// ---------------------------------------------------------------------------
// Happy path: CSV body, exact header columns, formula escaping of the
// malicious fixtures, self-audit row.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AuditExport_CsvWithFormulaEscaping)
{
    AUDIT_EXPORT_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    const std::string marker = "p1-6-export-" + uniqueSuffix();
    insertMaliciousFixtures(marker);

    const int64_t now = ::trantor::Date::now().secondsSinceEpoch();
    const std::string url = "/api/admin/logs/export?from=" + std::to_string(now - 60) +
                            "&to=" + std::to_string(now + 60) +
                            "&actor_id=fixture&action=" + marker;
    auto resp = sendGet(url, *token);

    // Cleanup before assertions (a failed CHECK must still remove fixtures).
    deleteFixtures(marker);

    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, ::drogon::k200OK));
    CHECK(resp->contentTypeString().find("text/csv") != std::string::npos);
    const std::string body(resp->getBody());

    // Header: the full audit row surface, exact order.
    CHECK(body.find(
            "id,timestamp,actor_type,actor_id,action,target_type,target_id,outcome,ip,"
            "user_agent,request_id,org_id,details\r\n") == 0);

    // Every malicious leading char is apostrophe-prefixed.
    CHECK(body.find("'=SUM(A1:A9)|cmd") != std::string::npos);
    CHECK(body.find("'+1.234") != std::string::npos);
    CHECK(body.find("'@import 'evil'") != std::string::npos);
    CHECK(body.find("'-2+3-4") != std::string::npos);
    CHECK(body.find("'\ttab-prefixed") != std::string::npos);
    CHECK(body.find("'@SUMMARY(1,2)") != std::string::npos);
    // The details cell (starts with '{') is JSON-quoted CSV: formula chars
    // INSIDE a cell are inert — only LEADING chars were prefixed.
    CHECK(body.find("=HYPERLINK") != std::string::npos);
    // No un-prefixed dangerous cell start appears at a field boundary.
    CHECK(body.find(",=SUM(A1:A9)") == std::string::npos);

    // Self-audit row: the export itself was audited.
    {
        auto db = ::drogon::app().getDbClient();
        auto f = db->execSqlAsyncFuture(
          "SELECT count(*) AS n FROM audit_logs WHERE action = 'audit_exported' "
          "AND actor_id = (SELECT public_sub::text FROM users WHERE username = 'admin')");
        CHECK(f.get()[0]["n"].as<int64_t>() >= 1);
    }
}

// ---------------------------------------------------------------------------
// Row cap: per-request `limit` may only lower the cap; over-limit export
// answers AUDIT_EXPORT_TOO_LARGE with a narrowing hint.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AuditExport_RowCapTooLarge)
{
    AUDIT_EXPORT_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    const std::string marker = "p1-6-cap-" + uniqueSuffix();
    insertMaliciousFixtures(marker);

    const int64_t now = ::trantor::Date::now().secondsSinceEpoch();
    // 3 fixture rows in the window; limit=2 must trip the cap branch.
    auto resp = sendGet(
      "/api/admin/logs/export?from=" + std::to_string(now - 60) + "&to=" +
        std::to_string(now + 60) + "&actor_id=fixture&action=" + marker + "&limit=2",
      *token);

    deleteFixtures(marker);

    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, ::drogon::k400BadRequest));
    Json::Value err;
    REQUIRE(parseJsonBody(resp, err));
    CHECK(err["error"]["code"].asString() == "AUDIT_EXPORT_TOO_LARGE");

    // limit=0 is rejected as invalid, not as a cap trip.
    auto bad = sendGet("/api/admin/logs/export?all=true&limit=0", *token);
    REQUIRE(bad != nullptr);
    CHECK(statusIs(bad, ::drogon::k400BadRequest));
}

// ---------------------------------------------------------------------------
// all=true waives the window requirement (cap still applies).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AuditExport_AllTrueFullTable)
{
    AUDIT_EXPORT_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto resp = sendGet("/api/admin/logs/export?all=true", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, ::drogon::k200OK));
    const std::string body(resp->getBody());
    CHECK(body.find("id,timestamp,actor_type,actor_id,action,target_type,target_id,outcome,ip,"
                    "user_agent,request_id,org_id,details\r\n") == 0);
    // The full table always has at least the seeded audit history rows.
    CHECK(body.size() > 200);
}

// ---------------------------------------------------------------------------
// Auth: 401 without a token; 403 for a regular (non-admin) user via the
// AuthorizationFilter RBAC gate on /api/admin/.*.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AuditExport_AuthGates)
{
    AUDIT_EXPORT_SKIP_GUARD;

    // No token -> 401 before RBAC evaluates.
    auto anon = sendGet("/api/admin/logs/export");
    REQUIRE(anon != nullptr);
    CHECK(statusIs(anon, ::drogon::k401Unauthorized));

    // Regular user -> 403 (rbac_rules: /api/admin/.* -> admin).
    const std::string user = "qa_exp_" + uniqueSuffix();
    const std::string pass = "qa-exp-Zx9-" + uniqueSuffix();
    REQUIRE(createVerifiedUser(user, pass));
    auto userToken = loginTokenVerbose(user, pass);
    REQUIRE(userToken.has_value());
    auto denied = sendGet("/api/admin/logs/export?all=true", *userToken);
    REQUIRE(denied != nullptr);
    CHECK(statusIs(denied, ::drogon::k403Forbidden));
}
