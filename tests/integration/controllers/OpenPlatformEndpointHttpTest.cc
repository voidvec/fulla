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
    // app is denied WITH a response (the old code silently hung).
    const std::string userB = "qa_app_b_" + suffix;
    const std::string passB = randomPassword();
    REQUIRE(createVerifiedUser(userB, userB + "@qa.example", passB));
    auto tokenB = loginTokenVerbose(userB, passB);
    REQUIRE(tokenB.has_value());
    auto idorRotate =
      sendPostJson("/api/me/applications/" + confClientId + "/rotate-secret",
                   Json::Value(Json::objectValue), *tokenB);
    REQUIRE(idorRotate != nullptr);
    CHECK(statusIs(idorRotate, drogon::k403Forbidden));
    auto idorDelete = sendDelete("/api/me/applications/" + confClientId, *tokenB);
    REQUIRE(idorDelete != nullptr);
    CHECK(statusIs(idorDelete, drogon::k403Forbidden));

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
