// tests/integration/admin/AdminOrgLifecycleHttpTest.cc
//
// P1-4 (#281): HTTP integration tests for the org management completion —
// PATCH validation, the DELETE soft-delete CASCADE (invitation voiding,
// membership detachment, owned-client suspension + introspection overlay,
// slug tombstone), read-path filters, and member management.
//
// Storage: Postgres-only; skips cleanly otherwise. Fixtures ride the
// self-service org + open-platform application APIs under unique slugs.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <fulla/drogon/utils/CryptoUtils.h>

#include "HttpTestClient.h"

#include <chrono>
#include <future>
#include <optional>
#include <string>

using fulla::test::http::loginAsAdmin;
using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendDelete;
using fulla::test::http::sendGet;
using fulla::test::http::sendPatchJson;
using fulla::test::http::sendPostForm;
using fulla::test::http::sendPostFormWithAuth;
using fulla::test::http::sendPostJson;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define ORG_LIFECYCLE_SKIP_GUARD                                 \
    do                                                           \
    {                                                            \
        if (!postgresAvailable() || !serverReachable())          \
        {                                                        \
            CHECK(true);                                         \
            return;                                              \
        }                                                        \
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
    return access.empty() ? std::optional<std::string>{} : std::optional<std::string>{access};
}
}  // namespace

// ---------------------------------------------------------------------------
// The full lifecycle: create (self-service) -> PATCH (validations) ->
// members -> DELETE cascade -> tombstone -> filters.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgLifecycle_FullCascade)
{
    ORG_LIFECYCLE_SKIP_GUARD;

    auto adminToken = loginAsAdmin();
    REQUIRE(adminToken.has_value());

    const std::string suffix = uniqueSuffix();
    const std::string orgSlug = "qa-life-" + suffix;
    const std::string owner = "qa_life_owner_" + suffix;
    const std::string ownerPass = "qa-life-Zx9-" + suffix;
    REQUIRE(createVerifiedUser(owner, ownerPass));
    auto ownerToken = loginTokenVerbose(owner, ownerPass);
    REQUIRE(ownerToken.has_value());

    // Owner creates the org (self-service).
    {
        Json::Value org;
        org["slug"] = orgSlug;
        org["name"] = "Lifecycle Org " + suffix;
        auto resp = sendPostJson("/api/me/organizations", org, *ownerToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k201Created));
    }

    // PATCH: happy path (rename + color), then the validation negatives.
    {
        Json::Value patch;
        patch["name"] = "Renamed Org " + suffix;
        patch["primary_color"] = "#4f81c4";
        auto resp = sendPatchJson("/api/admin/organizations/" + orgSlug, patch, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));

        Json::Value badColor;
        badColor["primary_color"] = "#12345678";  // the {3,8} draft form
        auto badResp = sendPatchJson("/api/admin/organizations/" + orgSlug, badColor, *adminToken);
        REQUIRE(badResp != nullptr);
        CHECK(statusIs(badResp, ::drogon::k400BadRequest));

        Json::Value badSlug;
        badSlug["slug"] = "new-slug-" + suffix;
        auto slugResp = sendPatchJson("/api/admin/organizations/" + orgSlug, badSlug, *adminToken);
        REQUIRE(slugResp != nullptr);
        CHECK(statusIs(slugResp, ::drogon::k400BadRequest));

        auto getResp = sendGet("/api/admin/organizations/" + orgSlug, *adminToken);
        REQUIRE(getResp != nullptr);
        Json::Value getBody;
        REQUIRE(parseJsonBody(getResp, getBody));
        CHECK(getBody["name"].asString() == "Renamed Org " + suffix);
    }

    // Routing probe: DELETE on a NONEXISTENT org must answer the handler's
    // JSON 404 (nullptr here would mean the route never dispatches).
    {
        auto probe = sendDelete("/api/admin/organizations/no-such-" + suffix, *adminToken);
        REQUIRE(probe != nullptr);
        CHECK(statusIs(probe, ::drogon::k404NotFound));
    }

    // Members: the creator is the owner; a second member arrives via
    // invitation and accepts (pre-delete), so the DELETE has something to
    // void.
    const std::string member = "qa_life_member_" + suffix;
    const std::string memberPass = "qa-life-Mx9-" + suffix;
    REQUIRE(createVerifiedUser(member, memberPass));
    std::string inviteToken;
    {
        Json::Value invite;
        invite["email"] = member + "@qa.example";
        invite["role"] = "member";
        auto resp = sendPostJson("/api/me/organizations/" + orgSlug + "/invitations", invite,
                                 *ownerToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k201Created));
        Json::Value body;
        REQUIRE(parseJsonBody(resp, body));
        inviteToken = body["token"].asString();
        REQUIRE(!inviteToken.empty());

        auto memberToken = loginTokenVerbose(member, memberPass);
        REQUIRE(memberToken.has_value());
        Json::Value accept;
        accept["token"] = inviteToken;
        auto acceptResp = sendPostJson("/api/me/org-invitations/accept", accept, *memberToken);
        REQUIRE(acceptResp != nullptr);
        CHECK(statusIs(acceptResp, ::drogon::k200OK));
    }

    // Members roster (admin).
    {
        auto resp = sendGet("/api/admin/organizations/" + orgSlug + "/members", *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
        Json::Value body;
        REQUIRE(parseJsonBody(resp, body));
        CHECK(body["total"].asInt64() == 2);
    }

    // An org-owned CONFIDENTIAL application with a client-credentials token
    // (issued BEFORE the delete — the introspection overlay must retire it).
    std::string appClientId;
    std::string appSecret;
    std::string ccToken;
    {
        Json::Value app;
        app["name"] = "Lifecycle App " + suffix;
        app["client_type"] = "CONFIDENTIAL";
        Json::Value uris(Json::arrayValue);
        uris.append("https://qa-life.example/callback");
        app["redirect_uris"] = uris;
        Json::Value scopes(Json::arrayValue);
        scopes.append("openid");
        app["scopes"] = scopes;
        app["org_slug"] = orgSlug;
        auto resp = sendPostJson("/api/me/applications", app, *ownerToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k201Created));
        Json::Value body;
        REQUIRE(parseJsonBody(resp, body));
        appClientId = body["client_id"].asString();
        appSecret = body["client_secret"].asString();
        CHECK(appClientId.size() > 0);
        REQUIRE(appSecret.size() > 0);

        // client_secret_basic (CONFIDENTIAL default): credentials ride the
        // Authorization header, not the form body.
        const std::string cred = appClientId + ":" + appSecret;
        const std::string basic =
          "Basic " + ::drogon::utils::base64Encode(
                       reinterpret_cast<const unsigned char *>(cred.data()), cred.size());
        auto tokResp = fulla::test::http::sendPostFormWithAuth(
          "/oauth2/token", "grant_type=client_credentials&scope=openid", basic);
        REQUIRE(tokResp != nullptr);
        CHECK(statusIs(tokResp, ::drogon::k200OK));
        Json::Value tokBody;
        REQUIRE(parseJsonBody(tokResp, tokBody));
        ccToken = tokBody["access_token"].asString();
        REQUIRE(!ccToken.empty());
    }

    // DELETE the org (admin surface).
    {
        auto resp = sendDelete("/api/admin/organizations/" + orgSlug, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
    }

    // Cascade assertions.
    // (a) Read paths: GET /{slug} 404; admin list lacks the org.
    {
        auto resp = sendGet("/api/admin/organizations/" + orgSlug, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k404NotFound));

        auto listResp = sendGet("/api/admin/organizations", *adminToken);
        REQUIRE(listResp != nullptr);
        Json::Value listBody;
        REQUIRE(parseJsonBody(listResp, listBody));
        for (const auto &o : listBody["organizations"])
            CHECK(o["slug"].asString() != orgSlug);
    }

    // (b) Membership detached: roster 404 (org filtered).
    {
        auto resp = sendGet("/api/admin/organizations/" + orgSlug + "/members", *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k404NotFound));
    }

    // (c) Owned client: new client-credentials auth REJECTED (suspension
    // channel) and the pre-delete token introspects inactive (overlay).
    {
        const std::string cred2 = appClientId + ":" + appSecret;
        const std::string basic2 =
          "Basic " + ::drogon::utils::base64Encode(
                       reinterpret_cast<const unsigned char *>(cred2.data()), cred2.size());
        auto tokResp = fulla::test::http::sendPostFormWithAuth(
          "/oauth2/token", "grant_type=client_credentials&scope=openid", basic2);
        REQUIRE(tokResp != nullptr);
        // RFC 6749 §5.2: bad client credentials answer invalid_client (401).
        CHECK(statusIs(tokResp, ::drogon::k401Unauthorized));

        // Introspect the pre-delete token via backend-svc (client_secret_basic).
        const std::string svcCred = "backend-svc:test-secret";
        const std::string svcBasic =
          "Basic " + ::drogon::utils::base64Encode(
                       reinterpret_cast<const unsigned char *>(svcCred.data()), svcCred.size());
        auto introResp = fulla::test::http::sendPostFormWithAuth(
          "/oauth2/introspect", "token=" + ccToken, svcBasic);
        REQUIRE(introResp != nullptr);
        CHECK(statusIs(introResp, ::drogon::k200OK));
        Json::Value introBody;
        REQUIRE(parseJsonBody(introResp, introBody));
        CHECK(introBody["active"].asBool() == false);
    }

    // (d) Slug tombstone: re-registering the SAME slug is rejected (409) —
    // same-name impersonation guard.
    {
        Json::Value org;
        org["slug"] = orgSlug;
        org["name"] = "Impersonator Org";
        auto resp = sendPostJson("/api/me/organizations", org, *ownerToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k409Conflict));

        Json::Value adminOrg;
        adminOrg["slug"] = orgSlug;
        adminOrg["name"] = "Impersonator Org Admin";
        auto adminResp = sendPostJson("/api/admin/organizations", adminOrg, *adminToken);
        REQUIRE(adminResp != nullptr);
        CHECK(statusIs(adminResp, ::drogon::k409Conflict));
    }

    // (e) The pending-invitation resurrection path: a NEW invitation can no
    // longer be created for the deleted org (org not found via the
    // filtered resolver — the same guard the accept path uses).
    {
        Json::Value invite;
        invite["email"] = member + "@qa.example";
        invite["role"] = "member";
        auto resp = sendPostJson("/api/me/organizations/" + orgSlug + "/invitations", invite,
                                 *ownerToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k404NotFound));
    }
}

// ---------------------------------------------------------------------------
// Member removal: last owner is protected; a non-owner member is removable.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgLifecycle_RemoveMember)
{
    ORG_LIFECYCLE_SKIP_GUARD;

    auto adminToken = loginAsAdmin();
    REQUIRE(adminToken.has_value());

    const std::string suffix = uniqueSuffix();
    const std::string orgSlug = "qa-rem-" + suffix;
    const std::string owner = "qa_rem_owner_" + suffix;
    const std::string ownerPass = "qa-rem-Zx9-" + suffix;
    const std::string member = "qa_rem_member_" + suffix;
    const std::string memberPass = "qa-rem-Mx9-" + suffix;
    REQUIRE(createVerifiedUser(owner, ownerPass));
    REQUIRE(createVerifiedUser(member, memberPass));
    auto ownerToken = loginTokenVerbose(owner, ownerPass);
    REQUIRE(ownerToken.has_value());

    {
        Json::Value org;
        org["slug"] = orgSlug;
        org["name"] = "Remove Org " + suffix;
        auto resp = sendPostJson("/api/me/organizations", org, *ownerToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k201Created));
    }

    int32_t ownerUserId = 0;
    int32_t memberUserId = 0;
    {
        auto db = ::drogon::app().getDbClient();
        auto f1 = db->execSqlAsyncFuture("SELECT id FROM users WHERE username = $1", owner);
        ownerUserId = f1.get()[0]["id"].as<int32_t>();
        auto f2 = db->execSqlAsyncFuture("SELECT id FROM users WHERE username = $1", member);
        memberUserId = f2.get()[0]["id"].as<int32_t>();
    }

    // Owner invite + member accept (gives the org two members).
    {
        Json::Value invite;
        invite["email"] = member + "@qa.example";
        invite["role"] = "member";
        auto resp = sendPostJson("/api/me/organizations/" + orgSlug + "/invitations", invite,
                                 *ownerToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k201Created));
        Json::Value body;
        REQUIRE(parseJsonBody(resp, body));
        Json::Value accept;
        accept["token"] = body["token"].asString();
        auto memberToken = loginTokenVerbose(member, memberPass);
        REQUIRE(memberToken.has_value());
        auto acceptResp = sendPostJson("/api/me/org-invitations/accept", accept, *memberToken);
        REQUIRE(acceptResp != nullptr);
        CHECK(statusIs(acceptResp, ::drogon::k200OK));
    }

    // Last owner: rejected with 409.
    {
        auto resp = sendDelete("/api/admin/organizations/" + orgSlug + "/members/" +
                                 std::to_string(ownerUserId),
                               *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k409Conflict));
    }

    // Non-owner member: removed.
    {
        auto resp = sendDelete("/api/admin/organizations/" + orgSlug + "/members/" +
                                 std::to_string(memberUserId),
                               *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));

        auto roster = sendGet("/api/admin/organizations/" + orgSlug + "/members", *adminToken);
        REQUIRE(roster != nullptr);
        Json::Value body;
        REQUIRE(parseJsonBody(roster, body));
        CHECK(body["total"].asInt64() == 1);
    }

    // Cleanup: delete the org so the slug is tombstoned (idempotent for
    // repeated runs under the same millisecond-suffix discipline).
    {
        auto resp = sendDelete("/api/admin/organizations/" + orgSlug, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
    }
}
