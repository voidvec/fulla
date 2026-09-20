// tests/integration/admin/AdminOrganizationOwnerTransferHttpTest.cc
//
// #221 minimal slice (v1.5.0 stage-one plan): a soft-deleted organization
// owner leaves the org unmanageable; the system-admin ownership-transfer
// endpoint breaks the deadlock. Acceptance (per the issue): owner-deleted ->
// admin reassign -> org manageable.
//
//   1. A (owner) creates the org, B joins as member; A is soft-deleted
//      -> the member seat cannot manage (deadlock demonstrated).
//   2. transfer-ownership to the SOFT-DELETED user -> 404 (would recreate
//      the deadlock); to a NON-EXISTENT user -> 404; bad body -> 400.
//   3. transfer-ownership to B -> 200; B can now manage (invite -> 201)
//      and the members list shows B as owner (previous owner demoted /
//      filtered as soft-deleted).
//
// Storage: Postgres-only (real org/member rows); cases skip when the
// endpoint-test server on 5555 is not reachable.

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
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

namespace
{

std::string uniqueSuffix()
{
    return std::to_string(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count());
}

std::string randomPassword()
{
    return std::string("qa-org-Zx9-") + uniqueSuffix();
}

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

}  // namespace

DROGON_TEST(Integration_P1_Admin_OrgOwnerTransfer_SoftDeletedOwnerDeadlockResolved)
{
    if (!postgresAvailable() || !serverReachable())
    {
        CHECK(true);
        return;
    }

    const std::string suffix = uniqueSuffix();
    const std::string orgSlug = "qa-org-ow-" + suffix;
    const std::string userA = "qa_ow_a_" + suffix;
    const std::string userB = "qa_ow_b_" + suffix;
    const std::string passA = randomPassword();
    const std::string passB = randomPassword();
    REQUIRE(createVerifiedUser(userA, userA + "@qa.example", passA));
    REQUIRE(createVerifiedUser(userB, userB + "@qa.example", passB));

    auto tokenA = loginTokenVerbose(userA, passA);
    auto tokenB = loginTokenVerbose(userB, passB);
    REQUIRE(tokenA.has_value());
    REQUIRE(tokenB.has_value());

    // A creates the org (owner), invites B (member), B accepts.
    {
        Json::Value createBody;
        createBody["slug"] = orgSlug;
        createBody["name"] = "QA Owner Transfer " + suffix;
        auto createResp = sendPostJson("/api/me/organizations", createBody, *tokenA);
        REQUIRE(createResp != nullptr);
        CHECK(statusIs(createResp, ::drogon::k201Created));
    }
    std::string aUserId, bUserId;
    {
        Json::Value invite;
        invite["email"] = userB + "@qa.example";
        invite["role"] = "member";
        auto inviteResp = sendPostJson("/api/me/organizations/" + orgSlug + "/invitations",
                                       invite, *tokenA);
        REQUIRE(inviteResp != nullptr);
        CHECK(statusIs(inviteResp, ::drogon::k201Created));
        Json::Value inviteBody;
        REQUIRE(parseJsonBody(inviteResp, inviteBody));

        Json::Value accept;
        accept["token"] = inviteBody["token"].asString();
        auto acceptResp = sendPostJson("/api/me/org-invitations/accept", accept, *tokenB);
        REQUIRE(acceptResp != nullptr);
        CHECK(statusIs(acceptResp, ::drogon::k200OK));

        // Capture both user ids from the members list.
        auto membersResp = sendGet("/api/me/organizations/" + orgSlug + "/members", *tokenA);
        REQUIRE(membersResp != nullptr);
        CHECK(statusIs(membersResp, ::drogon::k200OK));
        Json::Value membersBody;
        REQUIRE(parseJsonBody(membersResp, membersBody));
        for (const auto &m : membersBody["members"])
        {
            if (m["username"].asString() == userA)
                aUserId = std::to_string(m["user_id"].asInt64());
            if (m["username"].asString() == userB)
                bUserId = std::to_string(m["user_id"].asInt64());
        }
        REQUIRE(!aUserId.empty());
        REQUIRE(!bUserId.empty());
    }

    auto adminToken = loginAsAdmin();
    REQUIRE(adminToken.has_value());

    // Admin soft-deletes the owner (#221 premise).
    {
        auto delResp = sendDelete("/api/admin/users/" + aUserId, *adminToken);
        REQUIRE(delResp != nullptr);
        CHECK(statusIs(delResp, ::drogon::k200OK));
    }

    // Deadlock demonstrated: the remaining member cannot manage the org.
    {
        Json::Value invite;
        invite["email"] = "nobody@qa.example";
        invite["role"] = "member";
        auto denyResp =
          sendPostJson("/api/me/organizations/" + orgSlug + "/invitations", invite, *tokenB);
        REQUIRE(denyResp != nullptr);
        CHECK(statusIs(denyResp, ::drogon::k403Forbidden));
    }

    const std::string transferPath = "/api/admin/organizations/" + orgSlug + "/transfer-ownership";

    // Negative: transferring to the SOFT-DELETED user would recreate the
    // deadlock — rejected as user-not-found (404).
    {
        Json::Value body;
        body["user_id"] = std::stoi(aUserId);
        auto resp = sendPostJson(transferPath, body, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k404NotFound));
    }
    // Negative: non-existent target user -> 404.
    {
        Json::Value body;
        body["user_id"] = 99999999;
        auto resp = sendPostJson(transferPath, body, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k404NotFound));
    }
    // Negative: non-existent org -> 404.
    {
        Json::Value body;
        body["user_id"] = std::stoi(bUserId);
        auto resp = sendPostJson("/api/admin/organizations/no-such-org-" + suffix +
                                   "/transfer-ownership",
                                 body, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k404NotFound));
    }
    // Negative: missing user_id -> 400.
    {
        auto resp = sendPostJson(transferPath, Json::Value(Json::objectValue), *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k400BadRequest));
    }

    // The fix: admin reassigns ownership to the live member.
    {
        Json::Value body;
        body["user_id"] = std::stoi(bUserId);
        auto resp = sendPostJson(transferPath, body, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
        Json::Value ok;
        REQUIRE(parseJsonBody(resp, ok));
        CHECK(ok["slug"].asString() == orgSlug);
        CHECK(ok["owner_user_id"].asInt64() == std::stoll(bUserId));
    }

    // Issue acceptance criterion: the org is manageable again —
    // the new owner can invite, and the members list shows B as owner
    // (the soft-deleted previous owner is filtered from listings).
    {
        Json::Value invite;
        invite["email"] = "qa_ow_c_" + suffix + "@qa.example";
        invite["role"] = "member";
        auto inviteResp =
          sendPostJson("/api/me/organizations/" + orgSlug + "/invitations", invite, *tokenB);
        REQUIRE(inviteResp != nullptr);
        CHECK(statusIs(inviteResp, ::drogon::k201Created));

        auto membersResp = sendGet("/api/me/organizations/" + orgSlug + "/members", *tokenB);
        REQUIRE(membersResp != nullptr);
        CHECK(statusIs(membersResp, ::drogon::k200OK));
        Json::Value membersBody;
        REQUIRE(parseJsonBody(membersResp, membersBody));
        bool bIsOwner = false;
        bool aStillListed = false;
        for (const auto &m : membersBody["members"])
        {
            if (m["username"].asString() == userB && m["role"].asString() == "owner")
                bIsOwner = true;
            if (m["username"].asString() == userA)
                aStillListed = true;
        }
        CHECK(bIsOwner);
        CHECK(!aStillListed);
    }

    // Idempotency: re-running the same transfer succeeds (single owner row
    // stays single; the demote step matches nothing).
    {
        Json::Value body;
        body["user_id"] = std::stoi(bUserId);
        auto resp = sendPostJson(transferPath, body, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
    }

    // Review coverage: a NON-ADMIN user token is rejected by the admin
    // filter (the endpoint's RBAC + scope gates).
    {
        Json::Value body;
        body["user_id"] = std::stoi(bUserId);
        auto resp = sendPostJson(transferPath, body, *tokenB);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k403Forbidden));
    }

    // Review coverage: transfer to a NON-MEMBER takes the insert path (the
    // admin override must resolve orgs where nobody is left to invite), and
    // the LIVING previous owner is demoted to 'admin' — observable now that
    // B has not been soft-deleted.
    const std::string userD = "qa_ow_d_" + suffix;
    const std::string passD = randomPassword();
    std::string dUserId;
    {
        REQUIRE(createVerifiedUser(userD, userD + "@qa.example", passD));
        auto db = ::drogon::app().getDbClient();
        std::promise<std::string> got;
        db->execSqlAsync(
          "SELECT id FROM users WHERE username = $1",
          [&got](const ::drogon::orm::Result &r) {
              got.set_value(r.empty() ? "" : std::to_string(r[0]["id"].as<int64_t>()));
          },
          [&got](const ::drogon::orm::DrogonDbException &) { got.set_value(""); },
          userD);
        dUserId = got.get_future().get();
        REQUIRE(!dUserId.empty());

        Json::Value body;
        body["user_id"] = std::stoi(dUserId);
        auto resp = sendPostJson(transferPath, body, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));

        auto membersResp = sendGet("/api/me/organizations/" + orgSlug + "/members", *tokenB);
        REQUIRE(membersResp != nullptr);
        CHECK(statusIs(membersResp, ::drogon::k200OK));
        Json::Value membersBody;
        REQUIRE(parseJsonBody(membersResp, membersBody));
        bool dIsOwner = false;
        bool bIsAdmin = false;
        int ownerCount = 0;
        for (const auto &m : membersBody["members"])
        {
            if (m["role"].asString() == "owner")
                ++ownerCount;
            if (m["username"].asString() == userD && m["role"].asString() == "owner")
                dIsOwner = true;
            if (m["username"].asString() == userB && m["role"].asString() == "admin")
                bIsAdmin = true;
        }
        CHECK(dIsOwner);   // insert path: non-member became owner
        CHECK(bIsAdmin);   // living previous owner demoted (not dropped)
        CHECK(ownerCount == 1);
    }

    // Review coverage: idempotent re-run leaves exactly one owner.
    {
        Json::Value body;
        body["user_id"] = std::stoi(dUserId);
        auto resp = sendPostJson(transferPath, body, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));

        auto tokenD = loginTokenVerbose(userD, passD);
        REQUIRE(tokenD.has_value());
        auto membersResp = sendGet("/api/me/organizations/" + orgSlug + "/members", *tokenD);
        REQUIRE(membersResp != nullptr);
        CHECK(statusIs(membersResp, ::drogon::k200OK));
        Json::Value membersBody;
        REQUIRE(parseJsonBody(membersResp, membersBody));
        int ownerCount = 0;
        for (const auto &m : membersBody["members"])
            if (m["role"].asString() == "owner")
                ++ownerCount;
        CHECK(ownerCount == 1);
    }

    // Cleanup: hard-delete the throwaway users (member rows FK-cascade; the
    // org row stays — slugs are timestamp-unique, same as the org flow test).
    {
        auto db = ::drogon::app().getDbClient();
        std::promise<bool> cleaned;
        db->execSqlAsync(
          "DELETE FROM users WHERE username IN ($1, $2, $3)",
          [&cleaned](const ::drogon::orm::Result &) { cleaned.set_value(true); },
          [&cleaned](const ::drogon::orm::DrogonDbException &) { cleaned.set_value(false); },
          userA, userB, userD);
        CHECK(cleaned.get_future().get());
    }
}
