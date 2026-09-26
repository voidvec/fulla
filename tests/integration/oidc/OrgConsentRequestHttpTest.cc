// tests/integration/oidc/OrgConsentRequestHttpTest.cc
//
// #236 entry half (plan B) integration tests: the member-files-manager-
// approves org consent request workflow.
//   1. Filing: 200 on a new request, idempotent 200 on an identical
//      pending re-file, 403 non-member, 404 unknown client / unknown org
//      (uniform shapes), 409 already-org-owned / already-consented.
//   2. Manager surface: pending list with requester names (member list is
//      403), approve (writes organization_consents rows + auto-approves
//      sibling pendings + idempotent self-heal), reject (with reason,
//      re-file allowed, approve-after-reject 409), withdraw (own only;
//      anything else is the anti-enumeration 404).
//   3. FULL CIRCLE: after approval a member's authorize with the org hint
//      passes OrgContextGate condition 3 (#241) and the exchanged token
//      carries the org binding (introspection org_id).
//
// Storage: Postgres-only; cases skip cleanly under memory.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include "HttpTestClient.h"

#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <tuple>

using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendDelete;
using fulla::test::http::sendGet;
using fulla::test::http::sendPostForm;
using fulla::test::http::sendPostJson;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define ORGREQ_SKIP_GUARD                                       \
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
        .count());
}

std::string randomPassword()
{
    return std::string("qa-orgreq-Zx6-") + uniqueSuffix();
}

bool sqlExec(const std::string &sql)
{
    auto db = ::drogon::app().getDbClient();
    std::promise<bool> done;
    db->execSqlAsync(
      sql,
      [&done](const ::drogon::orm::Result &) { done.set_value(true); },
      [&done](const ::drogon::orm::DrogonDbException &) { done.set_value(false); });
    return done.get_future().get();
}

std::optional<int64_t> sqlInt(const std::string &sql)
{
    auto db = ::drogon::app().getDbClient();
    std::promise<std::optional<int64_t>> done;
    db->execSqlAsync(
      sql,
      [&done](const ::drogon::orm::Result &r) {
          if (r.empty() || r[0][0].isNull())
              done.set_value(std::nullopt);
          else
              done.set_value(r[0][0].as<int64_t>());
      },
      [&done](const ::drogon::orm::DrogonDbException &) {
          done.set_value(std::nullopt);
      });
    return done.get_future().get();
}

bool createVerifiedUser(const std::string &username,
                        const std::string &email,
                        const std::string &password)
{
    Json::Value body;
    body["username"] = username;
    body["password"] = password;
    body["email"] = email;
    auto resp = sendPostJson("/api/register", body);
    if (resp == nullptr || resp->getStatusCode() >= 300)
        return false;
    return sqlExec(
      "UPDATE users SET email_verified = true WHERE username = '" + username + "'");
}

// Console-client bearer for the /api/me setup APIs (the shared recipe).
std::optional<std::string> consoleBearer(const std::string &username,
                                         const std::string &password)
{
    const std::string verifier = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string challenge =
      ::fulla::drogon::utils::computeCodeChallenge(verifier, "S256");
    const std::string loginForm =
      "username=" + username + "&password=" + password +
      "&client_id=fulla-admin-console"
      "&redirect_uri=http%3A%2F%2F127.0.0.1%3A5174%2Fadmin%2Fcallback"
      "&scope=openid%20profile&state=orgreq-bearer"
      "&code_challenge=" + challenge +
      "&code_challenge_method=S256&json=true";
    auto loginResp = sendPostForm("/oauth2/login", loginForm);
    if (!loginResp || loginResp->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    Json::Value loginJson;
    if (!parseJsonBody(loginResp, loginJson))
        return std::nullopt;
    const std::string code = loginJson.get("code", "").asString();
    if (code.empty())
        return std::nullopt;
    const std::string tokenForm =
      "grant_type=authorization_code&code=" + code +
      "&redirect_uri=http%3A%2F%2F127.0.0.1%3A5174%2Fadmin%2Fcallback"
      "&client_id=fulla-admin-console&client_secret=&code_verifier=" + verifier;
    auto tokenResp = sendPostForm("/oauth2/token", tokenForm);
    if (!tokenResp || tokenResp->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    Json::Value tokenJson;
    if (!parseJsonBody(tokenResp, tokenJson))
        return std::nullopt;
    const std::string access = tokenJson.get("access_token", "").asString();
    if (access.empty())
        return std::nullopt;
    return access;
}

struct LoginResult
{
    ::drogon::HttpResponsePtr resp;
    std::string verifier;
};

// PKCE login (json=true) carrying the org_id hint (the #241 gate leg).
LoginResult pkceLogin(const std::string &username,
                      const std::string &password,
                      const std::string &clientId,
                      const std::string &redirectUri,
                      const std::string &scope,
                      const std::string &orgRef = "")
{
    LoginResult out;
    out.verifier = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string challenge =
      ::fulla::drogon::utils::computeCodeChallenge(out.verifier, "S256");
    std::string form =
      "username=" + username + "&password=" + password +
      "&client_id=" + ::drogon::utils::urlEncode(clientId) +
      "&redirect_uri=" + ::drogon::utils::urlEncode(redirectUri) +
      "&scope=" + ::drogon::utils::urlEncode(scope) +
      "&state=orgreq-state-1"
      "&code_challenge=" + challenge +
      "&code_challenge_method=S256&json=true";
    if (!orgRef.empty())
        form += "&org_id=" + ::drogon::utils::urlEncode(orgRef);
    out.resp = sendPostForm("/oauth2/login", form);
    return out;
}

// Token exchange with Basic client auth (CONFIDENTIAL self-registered app).
Json::Value exchangeCode(const std::string &code,
                         const std::string &verifier,
                         const std::string &clientId,
                         const std::string &clientSecret,
                         const std::string &redirectUri)
{
    const std::string form =
      "grant_type=authorization_code&code=" + ::drogon::utils::urlEncode(code) +
      "&redirect_uri=" + ::drogon::utils::urlEncode(redirectUri) +
      "&code_verifier=" + verifier;
    auto client =
      ::drogon::HttpClient::newHttpClient("http://127.0.0.1:5555", ::drogon::app().getLoop());
    auto req = ::drogon::HttpRequest::newHttpRequest();
    req->setMethod(::drogon::Post);
    req->setPath("/oauth2/token");
    req->setContentTypeCode(::drogon::CT_APPLICATION_X_FORM);
    req->setBody(form);
    req->addHeader(
      "Authorization",
      "Basic " + ::drogon::utils::base64Encode(clientId + ":" + clientSecret));
    Json::Value out;
    auto [result, resp] = client->sendRequest(req, 30.0);
    if (result != ::drogon::ReqResult::Ok || resp == nullptr ||
        !parseJsonBody(resp, out))
    {
        LOG_ERROR << "[orgreq:exchange] transport failure, result="
                  << static_cast<int>(result);
        return out;
    }
    if (!out.isMember("access_token"))
    {
        LOG_ERROR << "[orgreq:exchange] status="
                  << std::to_string(resp->getStatusCode())
                  << " body="
                  << Json::writeString(Json::StreamWriterBuilder(), out);
    }
    return out;
}

// RFC 7662 introspection with Basic client auth.
Json::Value introspectWith(const std::string &token,
                           const std::string &clientId,
                           const std::string &clientSecret)
{
    const std::string form = "token=" + ::drogon::utils::urlEncode(token);
    auto client =
      ::drogon::HttpClient::newHttpClient("http://127.0.0.1:5555", ::drogon::app().getLoop());
    auto req = ::drogon::HttpRequest::newHttpRequest();
    req->setMethod(::drogon::Post);
    req->setPath("/oauth2/introspect");
    req->setContentTypeCode(::drogon::CT_APPLICATION_X_FORM);
    req->setBody(form);
    req->addHeader(
      "Authorization",
      "Basic " + ::drogon::utils::base64Encode(clientId + ":" + clientSecret));
    Json::Value out;
    auto [result, resp] = client->sendRequest(req, 30.0);
    if (result != ::drogon::ReqResult::Ok || resp == nullptr ||
        !parseJsonBody(resp, out))
        return out;
    return out;
}

constexpr const char *kRedirect = "http://localhost:5174/orgreq/callback";

void dumpReqBody(const ::drogon::HttpResponsePtr &resp, const char *where)
{
    Json::Value body;
    const bool isJson = parseJsonBody(resp, body);
    LOG_ERROR << "[orgreq:" << where << "] status="
              << (resp ? std::to_string(resp->getStatusCode()) : std::string("null"))
              << " body="
              << (isJson ? Json::writeString(Json::StreamWriterBuilder(), body)
                         : std::string("<unparseable>"));
}

}  // namespace

// ---------------------------------------------------------------------------
// Filing guards: new-file 200, idempotent re-file 200 (same id), non-member
// 403, ghost client 404, ghost org 404, already-org-owned 409, already-
// consented 409.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgConsentRequest_FileGuards)
{
    ORGREQ_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string slug = "qa-orgreq-f-" + suffix;
    const std::string userA = "qa_req_a_" + suffix;  // org owner
    const std::string userB = "qa_req_b_" + suffix;  // member (requester)
    const std::string userC = "qa_req_c_" + suffix;  // non-member
    const std::string passA = randomPassword();
    const std::string passB = randomPassword();
    const std::string passC = randomPassword();
    REQUIRE(createVerifiedUser(userA, userA + "@qa.example", passA));
    REQUIRE(createVerifiedUser(userB, userB + "@qa.example", passB));
    REQUIRE(createVerifiedUser(userC, userC + "@qa.example", passC));

    auto bearerA = consoleBearer(userA, passA);
    auto bearerB = consoleBearer(userB, passB);
    auto bearerC = consoleBearer(userC, passC);
    REQUIRE(bearerA.has_value());
    REQUIRE(bearerB.has_value());
    REQUIRE(bearerC.has_value());

    // org (owner A) + member B.
    {
        Json::Value orgBody;
        orgBody["slug"] = slug;
        orgBody["name"] = "QA OrgReq F " + suffix;
        auto r = sendPostJson("/api/me/organizations", orgBody, *bearerA);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, drogon::k201Created));
    }
    {
        Json::Value invite;
        invite["email"] = userB + "@qa.example";
        invite["role"] = "member";
        auto r =
          sendPostJson("/api/me/organizations/" + slug + "/invitations", invite, *bearerA);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, drogon::k201Created));
        Json::Value inviteBody;
        REQUIRE(parseJsonBody(r, inviteBody));
        Json::Value accept;
        accept["token"] = inviteBody["token"].asString();
        auto r2 = sendPostJson("/api/me/org-invitations/accept", accept, *bearerB);
        REQUIRE(r2 != nullptr);
        CHECK(statusIs(r2, drogon::k200OK));
    }

    // A's personal app = the "third-party" application (never transferred).
    std::string appId;
    {
        Json::Value app;
        app["name"] = "QA ReqApp " + suffix;
        app["client_type"] = "CONFIDENTIAL";
        Json::Value uris(Json::arrayValue);
        uris.append(kRedirect);
        app["redirect_uris"] = uris;
        Json::Value scopes(Json::arrayValue);
        scopes.append("openid");
        scopes.append("profile");
        scopes.append("org");
        app["scopes"] = scopes;
        auto r = sendPostJson("/api/me/applications", app, *bearerA);
        REQUIRE(r != nullptr);
        Json::Value b;
        REQUIRE(parseJsonBody(r, b));
        appId = b["client_id"].asString();
        CHECK(!appId.empty());
    }

    // 1) B (member) files -> 200 with a pending row.
    Json::Value fileBody;
    fileBody["client_id"] = appId;
    auto file1 = sendPostJson("/api/me/organizations/" + slug + "/consent-requests",
                              fileBody, *bearerB);
    REQUIRE(file1 != nullptr);
    dumpReqBody(file1, "file request");
    CHECK(statusIs(file1, drogon::k200OK));
    Json::Value file1Body;
    REQUIRE(parseJsonBody(file1, file1Body));
    const Json::Int64 requestId = file1Body["id"].asInt64();
    CHECK(requestId > 0);
    CHECK(file1Body["status"].asString() == "pending");

    // 2) Idempotent re-file -> 200, SAME id.
    auto file2 = sendPostJson("/api/me/organizations/" + slug + "/consent-requests",
                              fileBody, *bearerB);
    REQUIRE(file2 != nullptr);
    CHECK(statusIs(file2, drogon::k200OK));
    Json::Value file2Body;
    REQUIRE(parseJsonBody(file2, file2Body));
    CHECK(file2Body["id"].asInt64() == requestId);

    // 3) Non-member files -> 403.
    auto file3 = sendPostJson("/api/me/organizations/" + slug + "/consent-requests",
                              fileBody, *bearerC);
    REQUIRE(file3 != nullptr);
    CHECK(statusIs(file3, drogon::k403Forbidden));

    // 4) Ghost client -> 404 (uniform "application not found").
    Json::Value ghostBody;
    ghostBody["client_id"] = "app_no-such-client-" + suffix;
    auto file4 = sendPostJson("/api/me/organizations/" + slug + "/consent-requests",
                              ghostBody, *bearerB);
    REQUIRE(file4 != nullptr);
    CHECK(statusIs(file4, drogon::k404NotFound));

    // 5) Ghost org slug -> 404.
    auto file5 = sendPostJson("/api/me/organizations/qa-no-such-org-" + suffix +
                                "/consent-requests",
                              fileBody, *bearerB);
    REQUIRE(file5 != nullptr);
    CHECK(statusIs(file5, drogon::k404NotFound));

    // 6) Transfer the app to the org, then filing must 409 (already owned).
    {
        Json::Value toOrg;
        toOrg["org_slug"] = slug;
        auto r = sendPostJson("/api/me/applications/" + appId + "/transfer", toOrg, *bearerA);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, drogon::k200OK));
    }
    auto file6 = sendPostJson("/api/me/organizations/" + slug + "/consent-requests",
                              fileBody, *bearerB);
    REQUIRE(file6 != nullptr);
    CHECK(statusIs(file6, drogon::k409Conflict));

    // 7) Seed an active consent row for a SECOND app, filing must 409.
    std::string appId2;
    {
        Json::Value app;
        app["name"] = "QA ReqApp2 " + suffix;
        Json::Value uris(Json::arrayValue);
        uris.append(kRedirect);
        app["redirect_uris"] = uris;
        auto r = sendPostJson("/api/me/applications", app, *bearerA);
        REQUIRE(r != nullptr);
        Json::Value b;
        REQUIRE(parseJsonBody(r, b));
        appId2 = b["client_id"].asString();
    }
    {
        const auto orgId = sqlInt("SELECT id FROM organizations WHERE slug = '" + slug + "'");
        const auto grantor = sqlInt("SELECT id FROM users WHERE username = '" + userA + "'");
        REQUIRE(orgId.has_value());
        REQUIRE(grantor.has_value());
        REQUIRE(sqlExec(
          "INSERT INTO organization_consents (organization_id, client_id, "
          "scope_name, granted_by) VALUES (" + std::to_string(*orgId) + ", '" +
          appId2 + "', 'org', " + std::to_string(*grantor) + ")"));
    }
    Json::Value file7Body;
    file7Body["client_id"] = appId2;
    auto file7 = sendPostJson("/api/me/organizations/" + slug + "/consent-requests",
                              file7Body, *bearerB);
    REQUIRE(file7 != nullptr);
    CHECK(statusIs(file7, drogon::k409Conflict));

    // Cleanup (requests first -- no FK on decided_by but requested_by pins
    // users; the transfer above makes the app org-owned so it dies with the
    // org, which we leave behind as a timestamp-unique row).
    sqlExec("DELETE FROM organization_consent_requests WHERE client_id IN ('" +
            appId + "', '" + appId2 + "')");
    sqlExec("DELETE FROM organization_consents WHERE client_id = '" + appId2 + "'");
    sqlExec("DELETE FROM oauth2_client_owners WHERE client_id IN ('" + appId +
            "', '" + appId2 + "')");
    sqlExec("DELETE FROM oauth2_client_scopes WHERE client_id IN ('" + appId +
            "', '" + appId2 + "')");
    sqlExec("DELETE FROM oauth2_clients WHERE client_id IN ('" + appId + "', '" +
            appId2 + "')");
    sqlExec("DELETE FROM users WHERE username IN ('" + userA + "', '" + userB +
            "', '" + userC + "')");
}

// ---------------------------------------------------------------------------
// Manager surface + FULL CIRCLE: list with usernames (member list 403),
// approve writes organization_consents and the #241 gate condition-3 then
// accepts a member's org-bound authorize (introspection org_id); siblings
// auto-approve; self-heal re-approve; reject with reason (re-file allowed,
// approve-after-reject 409); withdraw own 200 / anything else 404.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgConsentRequest_ApproveRejectWithdraw_FullCircle)
{
    ORGREQ_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string slug = "qa-orgreq-a-" + suffix;
    const std::string userA = "qa_req_d_" + suffix;  // org owner (approver)
    const std::string userB = "qa_req_e_" + suffix;  // member (requester 1)
    const std::string userD = "qa_req_f_" + suffix;  // member (requester 2)
    const std::string passA = randomPassword();
    const std::string passB = randomPassword();
    const std::string passD = randomPassword();
    REQUIRE(createVerifiedUser(userA, userA + "@qa.example", passA));
    REQUIRE(createVerifiedUser(userB, userB + "@qa.example", passB));
    REQUIRE(createVerifiedUser(userD, userD + "@qa.example", passD));

    auto bearerA = consoleBearer(userA, passA);
    auto bearerB = consoleBearer(userB, passB);
    auto bearerD = consoleBearer(userD, passD);
    REQUIRE(bearerA.has_value());
    REQUIRE(bearerB.has_value());
    REQUIRE(bearerD.has_value());

    // org (owner A) + members B and D.
    {
        Json::Value orgBody;
        orgBody["slug"] = slug;
        orgBody["name"] = "QA OrgReq A " + suffix;
        auto r = sendPostJson("/api/me/organizations", orgBody, *bearerA);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, drogon::k201Created));
    }
    for (const auto &[uname, bearer, pass] :
         std::vector<std::tuple<std::string, std::optional<std::string>, std::string>>{
           {userB, bearerB, passB}, {userD, bearerD, passD}})
    {
        (void)pass;
        Json::Value invite;
        invite["email"] = uname + "@qa.example";
        invite["role"] = "member";
        auto r =
          sendPostJson("/api/me/organizations/" + slug + "/invitations", invite, *bearerA);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, drogon::k201Created));
        Json::Value inviteBody;
        REQUIRE(parseJsonBody(r, inviteBody));
        Json::Value accept;
        accept["token"] = inviteBody["token"].asString();
        auto r2 = sendPostJson("/api/me/org-invitations/accept", accept, *bearer);
        REQUIRE(r2 != nullptr);
        CHECK(statusIs(r2, drogon::k200OK));
    }

    // A's personal CONFIDENTIAL app (the third party).
    std::string appId, appSecret;
    {
        Json::Value app;
        app["name"] = "QA ReqFlow App " + suffix;
        app["client_type"] = "CONFIDENTIAL";
        Json::Value uris(Json::arrayValue);
        uris.append(kRedirect);
        app["redirect_uris"] = uris;
        Json::Value scopes(Json::arrayValue);
        scopes.append("openid");
        scopes.append("profile");
        scopes.append("org");
        app["scopes"] = scopes;
        // CONFIDENTIAL self-service defaults to client_credentials only —
        // the authorization_code exchange below needs the grant registered.
        Json::Value grants(Json::arrayValue);
        grants.append("authorization_code");
        grants.append("refresh_token");
        app["allowed_grant_types"] = grants;
        auto r = sendPostJson("/api/me/applications", app, *bearerA);
        REQUIRE(r != nullptr);
        Json::Value b;
        REQUIRE(parseJsonBody(r, b));
        appId = b["client_id"].asString();
        appSecret = b["client_secret"].asString();
        CHECK(!appId.empty());
    }

    // A's SECOND personal app for the withdraw/reject flow — filing for the
    // first app after its approval would trip the B5 already-consented
    // guard (correctly), so the workflow tails need a clean target.
    std::string app2Id;
    {
        Json::Value app;
        app["name"] = "QA ReqFlow App2 " + suffix;
        Json::Value uris(Json::arrayValue);
        uris.append(kRedirect);
        app["redirect_uris"] = uris;
        auto r = sendPostJson("/api/me/applications", app, *bearerA);
        REQUIRE(r != nullptr);
        Json::Value b;
        REQUIRE(parseJsonBody(r, b));
        app2Id = b["client_id"].asString();
        CHECK(!app2Id.empty());
    }

    // B and D each file (siblings on the same (org, client)).
    Json::Value fileBody;
    fileBody["client_id"] = appId;
    auto fileB = sendPostJson("/api/me/organizations/" + slug + "/consent-requests",
                              fileBody, *bearerB);
    auto fileD = sendPostJson("/api/me/organizations/" + slug + "/consent-requests",
                              fileBody, *bearerD);
    REQUIRE(fileB != nullptr);
    REQUIRE(fileD != nullptr);
    dumpReqBody(fileB, "file B");
    dumpReqBody(fileD, "file D");
    CHECK(statusIs(fileB, drogon::k200OK));
    CHECK(statusIs(fileD, drogon::k200OK));
    Json::Value fileBBody, fileDBody;
    REQUIRE(parseJsonBody(fileB, fileBBody));
    REQUIRE(parseJsonBody(fileD, fileDBody));
    CHECK(fileBBody["id"].asInt64() != fileDBody["id"].asInt64());
    const Json::Int64 requestB = fileBBody["id"].asInt64();
    const Json::Int64 requestD = fileDBody["id"].asInt64();

    // Member list is the folded 403.
    auto memberList = sendGet("/api/me/organizations/" + slug + "/consent-requests", *bearerB);
    REQUIRE(memberList != nullptr);
    CHECK(statusIs(memberList, drogon::k403Forbidden));

    // Manager list shows both requests with requester names.
    auto list = sendGet("/api/me/organizations/" + slug + "/consent-requests", *bearerA);
    REQUIRE(list != nullptr);
    CHECK(statusIs(list, drogon::k200OK));
    Json::Value listBody;
    REQUIRE(parseJsonBody(list, listBody));
    CHECK(listBody["total"].asInt() == 2);
    bool sawB = false;
    bool sawD = false;
    for (const auto &item : listBody["requests"])
    {
        if (item["id"].asInt64() == requestB)
        {
            sawB = true;
            CHECK(item["requester_username"].asString() == userB);
        }
        if (item["id"].asInt64() == requestD)
            sawD = true;
    }
    CHECK(sawB);
    CHECK(sawD);

    // Approve B's request -> consent rows + auto-approved sibling.
    auto approve = sendPostJson(
      "/api/me/organizations/" + slug + "/consent-requests/" + std::to_string(requestB) +
        "/approve",
      Json::Value(Json::objectValue), *bearerA);
    REQUIRE(approve != nullptr);
    dumpReqBody(approve, "approve");
    CHECK(statusIs(approve, drogon::k200OK));
    Json::Value approveBody;
    REQUIRE(parseJsonBody(approve, approveBody));
    CHECK(approveBody["status"].asString() == "approved");
    CHECK(approveBody["scopes"].size() == 3);

    // Consent rows exist for the (org, client) pair.
    const auto orgId = sqlInt("SELECT id FROM organizations WHERE slug = '" + slug + "'");
    REQUIRE(orgId.has_value());
    const auto consentCount = sqlInt(
      "SELECT COUNT(*) FROM organization_consents WHERE organization_id = " +
      std::to_string(*orgId) + " AND client_id = '" + appId +
      "' AND revoked_at IS NULL");
    REQUIRE(consentCount.has_value());
    CHECK(*consentCount == 3);

    // FULL CIRCLE: member B's org-bound authorize now passes the gate
    // (#241 condition-3 alternative) and the token carries the binding.
    auto login = pkceLogin(userB, passB, appId, kRedirect, "openid profile org", slug);
    REQUIRE(login.resp != nullptr);
    dumpReqBody(login.resp, "full-circle login");
    CHECK(statusIs(login.resp, ::drogon::k200OK));
    Json::Value loginJson;
    REQUIRE(parseJsonBody(login.resp, loginJson));
    const std::string code = loginJson.get("code", "").asString();
    CHECK(!code.empty());
    if (!code.empty())
    {
        Json::Value tokens =
          exchangeCode(code, login.verifier, appId, appSecret, kRedirect);
        const std::string access = tokens.get("access_token", "").asString();
        CHECK(!access.empty());
        if (!access.empty())
        {
            Json::Value intro = introspectWith(access, appId, appSecret);
            CHECK(intro.get("active", false).asBool());
            CHECK(intro.get("org_id", 0).asInt64() == *orgId);
        }
    }

    // Sibling D auto-approved: pending list is empty and D's row flipped.
    auto listAfter = sendGet("/api/me/organizations/" + slug + "/consent-requests", *bearerA);
    REQUIRE(listAfter != nullptr);
    CHECK(statusIs(listAfter, drogon::k200OK));
    Json::Value listAfterBody;
    REQUIRE(parseJsonBody(listAfter, listAfterBody));
    CHECK(listAfterBody["total"].asInt() == 0);
    const auto dStatus = sqlInt(
      "SELECT 1 FROM organization_consent_requests WHERE id = " +
      std::to_string(requestD) + " AND status = 'approved'");
    REQUIRE(dStatus.has_value());

    // Idempotent self-heal: re-approving the approved request is 200.
    auto approveAgain = sendPostJson(
      "/api/me/organizations/" + slug + "/consent-requests/" + std::to_string(requestB) +
        "/approve",
      Json::Value(Json::objectValue), *bearerA);
    REQUIRE(approveAgain != nullptr);
    CHECK(statusIs(approveAgain, drogon::k200OK));

    // A files their own request (managers are members too) — for app2, the
    // not-yet-consented target; B (member) cannot withdraw it -> 404; A
    // withdraws their own -> 200.
    Json::Value file2Body;
    file2Body["client_id"] = app2Id;
    auto fileA = sendPostJson("/api/me/organizations/" + slug + "/consent-requests",
                              file2Body, *bearerA);
    REQUIRE(fileA != nullptr);
    CHECK(statusIs(fileA, drogon::k200OK));
    Json::Value fileABody;
    REQUIRE(parseJsonBody(fileA, fileABody));
    const Json::Int64 requestA = fileABody["id"].asInt64();
    auto foreignWithdraw = sendDelete(
      "/api/me/organizations/" + slug + "/consent-requests/" + std::to_string(requestA),
      *bearerB);
    REQUIRE(foreignWithdraw != nullptr);
    CHECK(statusIs(foreignWithdraw, drogon::k404NotFound));
    auto ownWithdraw = sendDelete(
      "/api/me/organizations/" + slug + "/consent-requests/" + std::to_string(requestA),
      *bearerA);
    REQUIRE(ownWithdraw != nullptr);
    CHECK(statusIs(ownWithdraw, drogon::k200OK));
    const auto goneRow = sqlInt(
      "SELECT 1 FROM organization_consent_requests WHERE id = " +
      std::to_string(requestA));
    CHECK(!goneRow.has_value());

    // Reject flow: D files for app2, A rejects with a reason, a re-file is
    // allowed (new pending), approving the REJECTED id is 409.
    auto fileD2 = sendPostJson("/api/me/organizations/" + slug + "/consent-requests",
                               file2Body, *bearerD);
    REQUIRE(fileD2 != nullptr);
    CHECK(statusIs(fileD2, drogon::k200OK));
    Json::Value fileD2Body;
    REQUIRE(parseJsonBody(fileD2, fileD2Body));
    const Json::Int64 requestD2 = fileD2Body["id"].asInt64();
    Json::Value rejectBody;
    rejectBody["reason"] = "not now";
    auto reject = sendPostJson(
      "/api/me/organizations/" + slug + "/consent-requests/" + std::to_string(requestD2) +
        "/reject",
      rejectBody, *bearerA);
    REQUIRE(reject != nullptr);
    CHECK(statusIs(reject, drogon::k200OK));
    auto fileD3 = sendPostJson("/api/me/organizations/" + slug + "/consent-requests",
                               file2Body, *bearerD);
    REQUIRE(fileD3 != nullptr);
    CHECK(statusIs(fileD3, drogon::k200OK));
    auto approveRejected = sendPostJson(
      "/api/me/organizations/" + slug + "/consent-requests/" + std::to_string(requestD2) +
        "/approve",
      Json::Value(Json::objectValue), *bearerA);
    REQUIRE(approveRejected != nullptr);
    CHECK(statusIs(approveRejected, drogon::k409Conflict));

    // Withdraw a non-pending (approved) request -> 404 (B7).
    auto withdrawApproved = sendDelete(
      "/api/me/organizations/" + slug + "/consent-requests/" + std::to_string(requestD),
      *bearerD);
    REQUIRE(withdrawApproved != nullptr);
    CHECK(statusIs(withdrawApproved, drogon::k404NotFound));

    // Cleanup.
    sqlExec("DELETE FROM organization_consent_requests WHERE client_id = '" + appId + "'");
    sqlExec("DELETE FROM organization_consents WHERE client_id = '" + appId + "'");
    sqlExec("DELETE FROM oauth2_codes WHERE client_id = '" + appId + "'");
    sqlExec("DELETE FROM oauth2_access_tokens WHERE client_id = '" + appId + "'");
    sqlExec("DELETE FROM oauth2_refresh_tokens WHERE client_id = '" + appId + "'");
    sqlExec("DELETE FROM oauth2_client_owners WHERE client_id = '" + appId + "'");
    sqlExec("DELETE FROM oauth2_client_scopes WHERE client_id = '" + appId + "'");
    sqlExec("DELETE FROM oauth2_clients WHERE client_id = '" + appId + "'");
    sqlExec("DELETE FROM users WHERE username IN ('" + userA + "', '" + userB +
            "', '" + userD + "')");
}
