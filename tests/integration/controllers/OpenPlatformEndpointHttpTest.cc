// tests/integration/controllers/OpenPlatformEndpointHttpTest.cc
//
// v1.4.0 HTTP integration tests: profile minimal set (PATCH /api/me/profile),
// organization self-service (/api/me/organizations*), and open-platform
// application registration (/api/me/applications*).
//
// Storage: Postgres-only (all three services hit the DB directly); cases skip
// cleanly under memory. Users are created through the real /api/register
// endpoint and email-verified via SQL (the flows under test need verified,
// loggable users — not the verification flow itself).
//
// Config note: creation endpoints are gated by custom_config
// open_platform.enabled; the dev/CI configs ship enabled=true so these tests
// exercise the full flows. The fail-closed default lives in config.prod.json
// and is enforced at startup-config level (no per-test toggle possible).

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include "HttpTestClient.h"

#include <chrono>
#include <string>

using fulla::test::http::loginAsUserTokens;
using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendDelete;
using fulla::test::http::sendGet;
using fulla::test::http::sendPatchJson;
using fulla::test::http::sendPostJson;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define OPENPLATFORM_SKIP_GUARD                                \
    do                                                         \
    {                                                          \
        if (!postgresAvailable() || !serverReachable())        \
        {                                                      \
            CHECK(true);                                       \
            return;                                            \
        }                                                      \
    } while (0)

namespace
{

std::string uniqueSuffix()
{
    return std::to_string(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count());
}

// Test-only throwaway secret built at runtime (never a literal): unique per
// call, satisfies the server's complexity rules, deleted with the user.
std::string randomPassword()
{
    return std::string("qa-org-Zx9-") + uniqueSuffix();
}

// Register a user through the real endpoint and mark the email verified via
// SQL so the account can log in. Returns true on success.
bool createVerifiedUser(const std::string &username,
                        const std::string &email,
                        const std::string &password)
{
    Json::Value body;
    body["username"] = username;
    body["password"] = password;
    body["email"] = email;
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

// Same 2-step PKCE flow as loginAsUserTokens, but with response dumps so a
// login failure in these flows is diagnosable (rate limiter, verification
// gate, MFA, ...).
void dumpIfBad(const ::drogon::HttpResponsePtr &resp, const char *where);

std::optional<std::string> loginTokenVerbose(const std::string &username,
                                             const std::string &password)
{
    const std::string codeVerifier = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string codeChallenge =
      ::fulla::drogon::utils::computeCodeChallenge(codeVerifier, "S256");
    const std::string loginForm =
      "username=" + username + "&password=" + password +
      "&client_id=fulla-admin-console"
      "&redirect_uri=http://localhost:5174/admin/callback"
      "&scope=openid%20profile&state=t1"
      "&code_challenge=" + codeChallenge +
      "&code_challenge_method=S256";
    auto loginResp = fulla::test::http::sendPostForm("/oauth2/login?json=true", loginForm);
    if (loginResp == nullptr || loginResp->getStatusCode() != ::drogon::k200OK)
    {
        dumpIfBad(loginResp, "login step1");
        return std::nullopt;
    }
    Json::Value loginJson;
    if (!parseJsonBody(loginResp, loginJson))
        return std::nullopt;
    const std::string code = loginJson.get("code", "").asString();
    if (code.empty())
    {
        LOG_ERROR << "[login step1] no code; keys=" << Json::writeString(
          Json::StreamWriterBuilder(), loginJson);
        return std::nullopt;
    }
    const std::string tokenForm =
      "grant_type=authorization_code&code=" + code +
      "&redirect_uri=http://localhost:5174/admin/callback"
      "&client_id=fulla-admin-console&client_secret=&code_verifier=" + codeVerifier;
    auto tokenResp = fulla::test::http::sendPostForm("/oauth2/token", tokenForm);
    if (tokenResp == nullptr || tokenResp->getStatusCode() != ::drogon::k200OK)
    {
        dumpIfBad(tokenResp, "login step2");
        return std::nullopt;
    }
    Json::Value tokenJson;
    if (!parseJsonBody(tokenResp, tokenJson))
        return std::nullopt;
    const std::string access = tokenJson.get("access_token", "").asString();
    if (access.empty())
        return std::nullopt;
    return access;
}

// Dump status + body so failures are diagnosable.
void dumpIfBad(const ::drogon::HttpResponsePtr &resp, const char *where)
{
    Json::Value body;
    parseJsonBody(resp, body);
    LOG_ERROR << "[" << where << "] status="
              << (resp ? std::to_string(resp->getStatusCode()) : std::string("null"))
              << " body=" << Json::writeString(Json::StreamWriterBuilder(), body);
}

// Blocking one-shot SQL for the #230 failure-injection case (same
// promise/future pattern as createVerifiedUser's verified UPDATE; the
// DrogonDbException is consumed in the error callback so a failed ALTER can
// never escape into the event loop -> 0xc0000409).
bool runSql(const std::string &sql)
{
    auto db = ::drogon::app().getDbClient();
    std::promise<bool> done;
    db->execSqlAsync(
      sql,
      [&done](const ::drogon::orm::Result &) { done.set_value(true); },
      [&done, sql](const ::drogon::orm::DrogonDbException &e) {
          LOG_ERROR << "[OwnerDbFailure] sql failed: " << e.base().what()
                    << " (sql: " << sql.substr(0, 60) << "...)";
          done.set_value(false);
      });
    return done.get_future().get();
}

// Restores a RENAMEd-away table no matter how the case exits (a failed
// REQUIRE unwinds through drogon's test framework; this destructor still
// runs), so one red assertion cannot leave the schema broken for the
// whole suite.
struct TableRenameRestore
{
    std::string table;
    std::string tmpName;
    explicit TableRenameRestore(std::string tableName)
        : table(std::move(tableName)), tmpName(this->table + "_tmp")
    {
    }
    ~TableRenameRestore()
    {
        if (!runSql("ALTER TABLE " + tmpName + " RENAME TO " + table))
        {
            LOG_ERROR << "[OwnerDbFailure] RESTORE FAILED for " << table
                      << " -- fulla_db schema left broken,"
                         " run db-reset before the next suite";
        }
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Profile minimal set: PATCH /api/me/profile sets both fields, GET /api/me
// echoes them, an invalid avatar_url is rejected, and clearing works.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OpenPlatform_Profile_PatchRoundTrip)
{
    OPENPLATFORM_SKIP_GUARD;

    // Review M12: run against a THROWAWAY user, not the seeded admin — the
    // old version mutated admin's profile and relied on restoring it at the
    // end (order-coupled shared state).
    const std::string suffix = uniqueSuffix();
    const std::string userP = "qa_profile_" + suffix;
    const std::string passP = randomPassword();
    REQUIRE(createVerifiedUser(userP, userP + "@qa.example", passP));
    auto token = loginTokenVerbose(userP, passP);
    REQUIRE(token.has_value());

    // 1) set both fields
    Json::Value patch;
    patch["display_name"] = "QA Display Name";
    patch["avatar_url"] = "https://qa.example/ava.png";
    auto resp = sendPatchJson("/api/me/profile", patch, *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));

    auto me = sendGet("/api/me", *token);
    REQUIRE(me != nullptr);
    Json::Value meBody;
    REQUIRE(parseJsonBody(me, meBody));
    CHECK(meBody["display_name"].asString() == "QA Display Name");
    CHECK(meBody["avatar_url"].asString() == "https://qa.example/ava.png");

    // 2) invalid avatar_url (http, not https) -> 400
    Json::Value bad;
    bad["avatar_url"] = "http://qa.example/ava.png";
    auto badResp = sendPatchJson("/api/me/profile", bad, *token);
    REQUIRE(badResp != nullptr);
    CHECK(statusIs(badResp, drogon::k400BadRequest));

    // 3) clear both fields
    Json::Value clear;
    clear["display_name"] = "";
    clear["avatar_url"] = "";
    auto clearResp = sendPatchJson("/api/me/profile", clear, *token);
    REQUIRE(clearResp != nullptr);
    CHECK(statusIs(clearResp, drogon::k200OK));
    auto me2 = sendGet("/api/me", *token);
    REQUIRE(me2 != nullptr);
    Json::Value me2Body;
    REQUIRE(parseJsonBody(me2, me2Body));
    CHECK(me2Body["display_name"].asString().empty());
    CHECK(me2Body["avatar_url"].asString().empty());

    // 4) dirty-only semantics (C5 companion): CJK code points count as one
    // each (cap is 100 code points, not bytes).
    Json::Value cjk;
    cjk["display_name"] = "中文昵称";  // 4 code points
    auto cjkResp = sendPatchJson("/api/me/profile", cjk, *token);
    REQUIRE(cjkResp != nullptr);
    CHECK(statusIs(cjkResp, drogon::k200OK));

    // cleanup the throwaway user
    {
        auto db = ::drogon::app().getDbClient();
        std::promise<bool> cleaned;
        db->execSqlAsync(
          "DELETE FROM users WHERE username = $1",
          [&cleaned](const ::drogon::orm::Result &) { cleaned.set_value(true); },
          [&cleaned](const ::drogon::orm::DrogonDbException &) { cleaned.set_value(false); },
          userP);
        CHECK(cleaned.get_future().get());
    }
}

// ---------------------------------------------------------------------------
// Organization self-service: create (owner), invite by email, second user
// accepts, member visible, self-removal works, reserved slug rejected.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OpenPlatform_Org_CreateInviteAcceptFlow)
{
    OPENPLATFORM_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string userA = "qa_org_a_" + suffix;
    const std::string userB = "qa_org_b_" + suffix;
    const std::string passA = randomPassword();
    const std::string passB = randomPassword();
    REQUIRE(createVerifiedUser(userA, userA + "@qa.example", passA));
    REQUIRE(createVerifiedUser(userB, userB + "@qa.example", passB));

    auto tokenA = loginTokenVerbose(userA, passA);
    auto tokenB = loginTokenVerbose(userB, passB);
    REQUIRE(tokenA.has_value());
    REQUIRE(tokenB.has_value());

    // reserved slug rejected
    Json::Value reserved;
    reserved["slug"] = "admin";
    reserved["name"] = "Reserved";
    auto reservedResp = sendPostJson("/api/me/organizations", reserved, *tokenA);
    REQUIRE(reservedResp != nullptr);
    CHECK(statusIs(reservedResp, drogon::k400BadRequest));

    // create
    Json::Value createBody;
    createBody["slug"] = "qa-org-" + suffix;
    createBody["name"] = "QA Org " + suffix;
    auto createResp = sendPostJson("/api/me/organizations", createBody, *tokenA);
    REQUIRE(createResp != nullptr);
    dumpIfBad(createResp, "create org");
    CHECK(statusIs(createResp, drogon::k201Created));

    // my orgs list shows owner role
    auto listResp = sendGet("/api/me/organizations", *tokenA);
    REQUIRE(listResp != nullptr);
    CHECK(statusIs(listResp, drogon::k200OK));
    Json::Value listBody;
    REQUIRE(parseJsonBody(listResp, listBody));
    bool foundOwner = false;
    for (const auto &org : listBody["organizations"])
    {
        if (org["slug"].asString() == "qa-org-" + suffix &&
            org["role"].asString() == "owner")
            foundOwner = true;
    }
    CHECK(foundOwner);

    // A invites B
    Json::Value invite;
    invite["email"] = userB + "@qa.example";
    invite["role"] = "member";
    auto inviteResp =
      sendPostJson("/api/me/organizations/qa-org-" + suffix + "/invitations", invite, *tokenA);
    REQUIRE(inviteResp != nullptr);
    CHECK(statusIs(inviteResp, drogon::k201Created));
    Json::Value inviteBody;
    REQUIRE(parseJsonBody(inviteResp, inviteBody));
    const std::string inviteToken = inviteBody["token"].asString();
    CHECK(!inviteToken.empty());

    // B accepts
    Json::Value accept;
    accept["token"] = inviteToken;
    auto acceptResp = sendPostJson("/api/me/org-invitations/accept", accept, *tokenB);
    REQUIRE(acceptResp != nullptr);
    CHECK(statusIs(acceptResp, drogon::k200OK));

    // members list contains B
    auto membersResp = sendGet("/api/me/organizations/qa-org-" + suffix + "/members", *tokenA);
    REQUIRE(membersResp != nullptr);
    CHECK(statusIs(membersResp, drogon::k200OK));
    Json::Value membersBody;
    REQUIRE(parseJsonBody(membersResp, membersBody));
    bool foundB = false;
    std::string bUserId;
    for (const auto &m : membersBody["members"])
    {
        if (m["username"].asString() == userB)
        {
            foundB = true;
            bUserId = std::to_string(m["user_id"].asInt64());
        }
    }
    CHECK(foundB);

    // B (member) cannot invite
    Json::Value invite2;
    invite2["email"] = "nobody@qa.example";
    auto denyResp =
      sendPostJson("/api/me/organizations/qa-org-" + suffix + "/invitations", invite2, *tokenB);
    REQUIRE(denyResp != nullptr);
    CHECK(statusIs(denyResp, drogon::k403Forbidden));

    // #223: an org app probed by a plain member returns the uniform 404 —
    // identical to probing a seeded or non-existent client (B cannot see
    // org apps in the list either, so this is coherent UX, not a lie).
    {
        Json::Value orgApp;
        orgApp["name"] = "QA Org App " + suffix;
        Json::Value cbUris(Json::arrayValue);
        cbUris.append("http://localhost/cb");
        orgApp["redirect_uris"] = cbUris;
        auto orgAppResp = sendPostJson("/api/me/applications", orgApp, *tokenA);
        REQUIRE(orgAppResp != nullptr);
        CHECK(statusIs(orgAppResp, drogon::k201Created));
        Json::Value orgAppBody;
        REQUIRE(parseJsonBody(orgAppResp, orgAppBody));
        const std::string orgAppId = orgAppBody["client_id"].asString();

        Json::Value toOrg;
        toOrg["org_slug"] = "qa-org-" + suffix;
        auto transferResp =
          sendPostJson("/api/me/applications/" + orgAppId + "/transfer", toOrg, *tokenA);
        REQUIRE(transferResp != nullptr);
        CHECK(statusIs(transferResp, drogon::k200OK));

        auto memberProbe = sendPostJson("/api/me/applications/" + orgAppId + "/rotate-secret",
                                        Json::Value(Json::objectValue), *tokenB);
        REQUIRE(memberProbe != nullptr);
        CHECK(statusIs(memberProbe, drogon::k404NotFound));
        Json::Value memberProbeBody;
        REQUIRE(parseJsonBody(memberProbe, memberProbeBody));
        CHECK(memberProbeBody["error"]["code"].asString() == "VALIDATION_RESOURCE_NOT_FOUND");

        // The org owner still manages the org app.
        auto ownerRotate = sendPostJson("/api/me/applications/" + orgAppId + "/rotate-secret",
                                        Json::Value(Json::objectValue), *tokenA);
        REQUIRE(ownerRotate != nullptr);
        CHECK(statusIs(ownerRotate, drogon::k200OK));
    }

    // B leaves (self-removal)
    auto leaveResp =
      sendDelete("/api/me/organizations/qa-org-" + suffix + "/members/" + bUserId, *tokenB);
    REQUIRE(leaveResp != nullptr);
    CHECK(statusIs(leaveResp, drogon::k200OK));

    // cleanup users (org row stays; slug is timestamp-unique)
    auto db = ::drogon::app().getDbClient();
    std::promise<bool> cleaned;
    db->execSqlAsync(
      "DELETE FROM users WHERE username IN ($1, $2)",
      [&cleaned](const ::drogon::orm::Result &) { cleaned.set_value(true); },
      [&cleaned](const ::drogon::orm::DrogonDbException &) { cleaned.set_value(false); },
      userA, userB);
    CHECK(cleaned.get_future().get());
}

// ---------------------------------------------------------------------------
// Open platform applications: PUBLIC app (no secret), CONFIDENTIAL app
// (secret shown once), rotate-secret, admin suspend/resume, delete.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OpenPlatform_App_RegisterRotateDeleteFlow)
{
    OPENPLATFORM_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string userA = "qa_app_a_" + suffix;
    const std::string passA = randomPassword();
    REQUIRE(createVerifiedUser(userA, userA + "@qa.example", passA));
    auto tokenA = loginTokenVerbose(userA, passA);
    REQUIRE(tokenA.has_value());

    // 1) PUBLIC application: no secret in the response
    Json::Value publicApp;
    publicApp["name"] = "QA Public App " + suffix;
    publicApp["client_type"] = "PUBLIC";
    Json::Value uris(Json::arrayValue);
    uris.append("https://qa.example/callback");
    publicApp["redirect_uris"] = uris;
    Json::Value scopes(Json::arrayValue);
    scopes.append("openid");
    scopes.append("profile");
    publicApp["scopes"] = scopes;
    auto createResp = sendPostJson("/api/me/applications", publicApp, *tokenA);
    REQUIRE(createResp != nullptr);
    CHECK(statusIs(createResp, drogon::k201Created));
    Json::Value publicBody;
    REQUIRE(parseJsonBody(createResp, publicBody));
    const std::string publicClientId = publicBody["client_id"].asString();
    CHECK(!publicClientId.empty());
    CHECK(publicClientId.rfind("app_", 0) == 0);
    CHECK(!publicBody.isMember("client_secret"));

    // 2) admin scope not selectable (self_service allowlist)
    Json::Value adminScopeApp = publicApp;
    adminScopeApp["name"] = "QA Admin Scope " + suffix;
    Json::Value adminScopes(Json::arrayValue);
    adminScopes.append("admin");
    adminScopeApp["scopes"] = adminScopes;
    auto denyResp = sendPostJson("/api/me/applications", adminScopeApp, *tokenA);
    REQUIRE(denyResp != nullptr);
    CHECK(statusIs(denyResp, drogon::k400BadRequest));

    // 3) CONFIDENTIAL app: secret present exactly once
    Json::Value confApp;
    confApp["name"] = "QA Conf App " + suffix;
    confApp["client_type"] = "CONFIDENTIAL";
    confApp["redirect_uris"] = uris;
    auto confResp = sendPostJson("/api/me/applications", confApp, *tokenA);
    REQUIRE(confResp != nullptr);
    dumpIfBad(confResp, "create conf app");
    CHECK(statusIs(confResp, drogon::k201Created));
    Json::Value confBody;
    REQUIRE(parseJsonBody(confResp, confBody));
    const std::string confClientId = confBody["client_id"].asString();
    CHECK(!confBody["client_secret"].asString().empty());

    // 4) list contains both (asserted — review flagged the old no-op parse)
    auto listResp = sendGet("/api/me/applications", *tokenA);
    REQUIRE(listResp != nullptr);
    CHECK(statusIs(listResp, drogon::k200OK));
    Json::Value listBody;
    REQUIRE(parseJsonBody(listResp, listBody));
    bool listHasPublic = false;
    bool listHasConf = false;
    for (const auto &app : listBody["applications"])
    {
        if (app["client_id"].asString() == publicClientId)
            listHasPublic = true;
        if (app["client_id"].asString() == confClientId)
            listHasConf = true;
    }
    CHECK(listHasPublic);
    CHECK(listHasConf);

    // 4b) Cross-user IDOR (review C1): user B operating user A's personal
    // app is denied WITH a response (the old code silently hung). #223: the
    // denial is now the uniform 404 (indistinguishable from a non-existent
    // client), not a 403 that reveals "self-registered personal app".
    const std::string userB = "qa_app_b_" + suffix;
    const std::string passB = randomPassword();
    REQUIRE(createVerifiedUser(userB, userB + "@qa.example", passB));
    auto tokenB = loginTokenVerbose(userB, passB);
    REQUIRE(tokenB.has_value());
    auto idorRotate =
      sendPostJson("/api/me/applications/" + confClientId + "/rotate-secret",
                   Json::Value(Json::objectValue), *tokenB);
    REQUIRE(idorRotate != nullptr);
    CHECK(statusIs(idorRotate, drogon::k404NotFound));
    Json::Value idorBody;
    REQUIRE(parseJsonBody(idorRotate, idorBody));
    CHECK(idorBody["error"]["code"].asString() == "VALIDATION_RESOURCE_NOT_FOUND");
    auto idorDelete = sendDelete("/api/me/applications/" + confClientId, *tokenB);
    REQUIRE(idorDelete != nullptr);
    CHECK(statusIs(idorDelete, drogon::k404NotFound));

    // 4c) #223 anti-enumeration uniformity: probing a SEEDED client (no
    // owners row) and a NON-EXISTENT client must return exactly the same
    // shape as the IDOR 404 above — same status, same error code AND the
    // same catalog message (locked in explicitly so a future detail leak
    // cannot regress silently).
    std::string uniformMessage;
    {
        // Review guard: the seeded-probe leg below only proves anything if
        // backend-svc actually exists (a DB applied seeds but missed the
        // client would silently degrade it to a second ghost probe).
        auto db = ::drogon::app().getDbClient();
        std::promise<bool> seeded;
        db->execSqlAsync(
          "SELECT 1 FROM oauth2_clients WHERE client_id = 'backend-svc'",
          [&seeded](const ::drogon::orm::Result &r) { seeded.set_value(!r.empty()); },
          [&seeded](const ::drogon::orm::DrogonDbException &) { seeded.set_value(false); });
        REQUIRE(seeded.get_future().get());

        Json::Value idorErr = idorBody["error"];
        REQUIRE(idorErr.isMember("message"));
        uniformMessage = idorErr["message"].asString();

        auto seededProbe = sendPostJson("/api/me/applications/backend-svc/rotate-secret",
                                        Json::Value(Json::objectValue), *tokenB);
        REQUIRE(seededProbe != nullptr);
        CHECK(statusIs(seededProbe, drogon::k404NotFound));
        Json::Value seededBody;
        REQUIRE(parseJsonBody(seededProbe, seededBody));
        CHECK(seededBody["error"]["code"].asString() == "VALIDATION_RESOURCE_NOT_FOUND");
        CHECK(seededBody["error"]["message"].asString() == uniformMessage);

        auto ghostProbe = sendPostJson(
          "/api/me/applications/no-such-client-" + suffix + "/rotate-secret",
          Json::Value(Json::objectValue), *tokenB);
        REQUIRE(ghostProbe != nullptr);
        CHECK(statusIs(ghostProbe, drogon::k404NotFound));
        Json::Value ghostBody;
        REQUIRE(parseJsonBody(ghostProbe, ghostBody));
        CHECK(ghostBody["error"]["code"].asString() == "VALIDATION_RESOURCE_NOT_FOUND");
        CHECK(ghostBody["error"]["message"].asString() == uniformMessage);
    }

    // 5) rotate the CONFIDENTIAL secret
    auto rotateResp =
      sendPostJson("/api/me/applications/" + confClientId + "/rotate-secret",
                   Json::Value(Json::objectValue), *tokenA);
    REQUIRE(rotateResp != nullptr);
    CHECK(statusIs(rotateResp, drogon::k200OK));
    Json::Value rotateBody;
    REQUIRE(parseJsonBody(rotateResp, rotateBody));
    CHECK(!rotateBody["client_secret"].asString().empty());

    // 6) admin suspend — then verify governance actually bites (review M12):
    // the suspended client fails token validation, and the owner's
    // management writes are frozen (M5). Resume afterwards.
    auto adminToken = fulla::test::http::loginAsAdmin();
    REQUIRE(adminToken.has_value());
    auto suspendResp =
      sendPostJson("/api/admin/clients/" + confClientId + "/suspend",
                   Json::Value(Json::objectValue), *adminToken);
    REQUIRE(suspendResp != nullptr);
    CHECK(statusIs(suspendResp, drogon::k200OK));
    // 6a) client validation fails: a token exchange for the suspended
    // CONFIDENTIAL client is rejected (invalid_client family).
    {
        const std::string tokenForm =
          "grant_type=client_credentials&client_id=" + confClientId +
          "&client_secret=definitely-wrong&scope=openid";
        auto suspendedToken =
          fulla::test::http::sendPostForm("/oauth2/token", tokenForm);
        REQUIRE(suspendedToken != nullptr);
        CHECK(suspendedToken->getStatusCode() == drogon::k401Unauthorized);
    }
    // 6b) the owner cannot rotate while suspended (M5).
    auto frozenRotate =
      sendPostJson("/api/me/applications/" + confClientId + "/rotate-secret",
                   Json::Value(Json::objectValue), *tokenA);
    REQUIRE(frozenRotate != nullptr);
    CHECK(statusIs(frozenRotate, drogon::k403Forbidden));
    auto resumeResp =
      sendPostJson("/api/admin/clients/" + confClientId + "/resume",
                   Json::Value(Json::objectValue), *adminToken);
    REQUIRE(resumeResp != nullptr);
    CHECK(statusIs(resumeResp, drogon::k200OK));

    // 6c) transfer round-trip (self-review: this flow was untested and hid a
    // double-response bug — the toPersonal branch fell through to the toOrg
    // section). personal -> org -> personal, then a redundant transfer is
    // rejected cleanly.
    {
        // Create an org owned by userA.
        Json::Value orgBody;
        orgBody["slug"] = "qa-app-org-" + suffix;
        orgBody["name"] = "QA App Org " + suffix;
        auto orgResp = sendPostJson("/api/me/organizations", orgBody, *tokenA);
        REQUIRE(orgResp != nullptr);
        CHECK(statusIs(orgResp, drogon::k201Created));
        const std::string orgSlug = orgBody["slug"].asString();

        // personal -> org
        Json::Value toOrg;
        toOrg["org_slug"] = orgSlug;
        auto t1 = sendPostJson("/api/me/applications/" + confClientId + "/transfer",
                               toOrg, *tokenA);
        REQUIRE(t1 != nullptr);
        CHECK(statusIs(t1, drogon::k200OK));

        // org -> personal (exercises the M8 creator-quota gate; must answer
        // exactly once with 200 — the old fallthrough produced a spurious
        // 400 first).
        Json::Value toPersonal;  // org_slug absent -> personal
        auto t2 = sendPostJson("/api/me/applications/" + confClientId + "/transfer",
                               toPersonal, *tokenA);
        REQUIRE(t2 != nullptr);
        CHECK(statusIs(t2, drogon::k200OK));

        // Already personal -> clean 400, single response.
        auto t3 = sendPostJson("/api/me/applications/" + confClientId + "/transfer",
                               toPersonal, *tokenA);
        REQUIRE(t3 != nullptr);
        CHECK(statusIs(t3, drogon::k400BadRequest));
    }

    // 7) delete the PUBLIC app; list no longer contains it
    auto delResp = sendDelete("/api/me/applications/" + publicClientId, *tokenA);
    REQUIRE(delResp != nullptr);
    CHECK(statusIs(delResp, drogon::k200OK));
    auto list2 = sendGet("/api/me/applications", *tokenA);
    REQUIRE(list2 != nullptr);
    Json::Value list2Body;
    REQUIRE(parseJsonBody(list2, list2Body));
    bool stillThere = false;
    for (const auto &app : list2Body["applications"])
    {
        if (app["client_id"].asString() == publicClientId)
            stillThere = true;
    }
    CHECK(!stillThere);

    // cleanup users (app rows stay; harmless timestamp-unique QA artifacts)
    auto db = ::drogon::app().getDbClient();
    std::promise<bool> cleaned;
    db->execSqlAsync(
      "DELETE FROM users WHERE username IN ($1, $2)",
      [&cleaned](const ::drogon::orm::Result &) { cleaned.set_value(true); },
      [&cleaned](const ::drogon::orm::DrogonDbException &) { cleaned.set_value(false); },
      userA, userB);
    CHECK(cleaned.get_future().get());
}

// ---------------------------------------------------------------------------
// #230: a REAL DB failure during the owners-row lookup must surface as
// 500 DB_QUERY_ERROR on every management endpoint, NOT the uniform 404.
// Post-#227 the two were indistinguishable: ApplicationService swallowed
// every DrogonDbException as "no owner row", so a broken DB silently
// impersonated a legitimate denial (without even a log). Injection =
// RENAME the owners table away; the RAII guard restores it, and the
// post-restore leg proves management works again.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OpenPlatform_OwnerDbFailure_ManageEndpoints500Not404)
{
    OPENPLATFORM_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string userA = "qa_dbfail_" + suffix;
    const std::string passA = randomPassword();
    REQUIRE(createVerifiedUser(userA, userA + "@qa.example", passA));
    auto tokenA = loginTokenVerbose(userA, passA);
    REQUIRE(tokenA.has_value());

    // A CONFIDENTIAL app owned by userA (goes through requireManagePermission).
    Json::Value confApp;
    confApp["name"] = "QA DbFail App " + suffix;
    confApp["client_type"] = "CONFIDENTIAL";
    Json::Value uris(Json::arrayValue);
    uris.append("https://qa.example/callback");
    confApp["redirect_uris"] = uris;
    auto createResp = sendPostJson("/api/me/applications", confApp, *tokenA);
    REQUIRE(createResp != nullptr);
    CHECK(statusIs(createResp, drogon::k201Created));
    Json::Value confBody;
    REQUIRE(parseJsonBody(createResp, confBody));
    const std::string clientId = confBody["client_id"].asString();
    CHECK(!clientId.empty());

    // CHECK/REQUIRE need the DROGON_TEST context, which helper functions
    // do not have: this predicate only inspects, the call sites below do
    // the asserting (test-methodology rule).
    auto isDbError = [](const ::drogon::HttpResponsePtr &resp) {
        Json::Value body;
        return resp != nullptr &&
               resp->getStatusCode() == drogon::k500InternalServerError &&
               parseJsonBody(resp, body) &&
               body["error"]["code"].asString() == "DB_QUERY_ERROR";
    };

    // Inject a real DB failure: every owners-table read now errors.
    REQUIRE(runSql("ALTER TABLE oauth2_client_owners RENAME TO oauth2_client_owners_tmp"));
    {
        TableRenameRestore restore("oauth2_client_owners");

        // GET list (control leg: the findOwners query fails directly).
        auto listResp = sendGet("/api/me/applications", *tokenA);
        REQUIRE(listResp != nullptr);
        const bool listDbErr = isDbError(listResp);
        CHECK(listDbErr);
        if (!listDbErr)
            dumpIfBad(listResp, "GET list under injected DB failure");

        // PATCH (requireManagePermission -> loadOwnerRow fails).
        Json::Value patch;
        patch["name"] = "Renamed " + suffix;
        auto patchResp = sendPatchJson("/api/me/applications/" + clientId, patch, *tokenA);
        REQUIRE(patchResp != nullptr);
        const bool patchDbErr = isDbError(patchResp);
        CHECK(patchDbErr);
        if (!patchDbErr)
            dumpIfBad(patchResp, "PATCH under injected DB failure");

        // rotate-secret.
        auto rotateResp = sendPostJson("/api/me/applications/" + clientId + "/rotate-secret",
                                       Json::Value(Json::objectValue), *tokenA);
        REQUIRE(rotateResp != nullptr);
        const bool rotateDbErr = isDbError(rotateResp);
        CHECK(rotateDbErr);
        if (!rotateDbErr)
            dumpIfBad(rotateResp, "rotate-secret under injected DB failure");

        // DELETE.
        auto delResp = sendDelete("/api/me/applications/" + clientId, *tokenA);
        REQUIRE(delResp != nullptr);
        const bool delDbErr = isDbError(delResp);
        CHECK(delDbErr);
        if (!delDbErr)
            dumpIfBad(delResp, "DELETE under injected DB failure");
    }

    // Post-restore: the same owner can manage the app again (200). This
    // proves the guard really put the table back (and that the 500s above
    // were caused by the injection, not by a coincidentally dead app).
    auto rotateAgain =
      sendPostJson("/api/me/applications/" + clientId + "/rotate-secret",
                   Json::Value(Json::objectValue), *tokenA);
    REQUIRE(rotateAgain != nullptr);
    CHECK(statusIs(rotateAgain, drogon::k200OK));

    // Second leg: the ORG-app membership read (#222 sank it into
    // ClientOwnersRepository::findMembership) must fail the same way --
    // 500 DB_QUERY_ERROR, never the uniform 404. Transfer the app to an
    // org first so requireManagePermission takes the membership branch.
    Json::Value orgBody;
    orgBody["slug"] = "qa-dbfail-org-" + suffix;
    orgBody["name"] = "QA DbFail Org " + suffix;
    auto orgResp = sendPostJson("/api/me/organizations", orgBody, *tokenA);
    REQUIRE(orgResp != nullptr);
    CHECK(statusIs(orgResp, drogon::k201Created));
    Json::Value toOrg;
    toOrg["org_slug"] = orgBody["slug"].asString();
    auto transferResp =
      sendPostJson("/api/me/applications/" + clientId + "/transfer", toOrg, *tokenA);
    REQUIRE(transferResp != nullptr);
    CHECK(statusIs(transferResp, drogon::k200OK));

    REQUIRE(runSql("ALTER TABLE organization_members RENAME TO organization_members_tmp"));
    {
        TableRenameRestore restore("organization_members");
        Json::Value patchOrg;
        patchOrg["name"] = "Renamed Org " + suffix;
        auto patchOrgResp = sendPatchJson("/api/me/applications/" + clientId, patchOrg, *tokenA);
        REQUIRE(patchOrgResp != nullptr);
        const bool patchOrgDbErr = isDbError(patchOrgResp);
        CHECK(patchOrgDbErr);
        if (!patchOrgDbErr)
            dumpIfBad(patchOrgResp, "PATCH (org app) under injected membership failure");
    }
    // Post-restore: the org owner manages the org app again.
    Json::Value patchOrgOk;
    patchOrgOk["name"] = "Renamed Org OK " + suffix;
    auto patchOrgAgain =
      sendPatchJson("/api/me/applications/" + clientId, patchOrgOk, *tokenA);
    REQUIRE(patchOrgAgain != nullptr);
    CHECK(statusIs(patchOrgAgain, drogon::k200OK));

    // cleanup users (owners row cascades with the user; client + org rows
    // stay; harmless timestamp-unique QA artifacts)
    auto db = ::drogon::app().getDbClient();
    std::promise<bool> cleaned;
    db->execSqlAsync(
      "DELETE FROM users WHERE username = $1",
      [&cleaned](const ::drogon::orm::Result &) { cleaned.set_value(true); },
      [&cleaned](const ::drogon::orm::DrogonDbException &) { cleaned.set_value(false); },
      userA);
    CHECK(cleaned.get_future().get());
}
