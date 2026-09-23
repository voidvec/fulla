// tests/integration/oidc/OrgConsentHttpTest.cc
//
// v1.5.0 M2 integration tests: organization admin consent (design §2.2).
//   1. The R-M2-2 write gate -- an org-bound approve by the org's
//      owner/admin records organization_consents rows (never personal
//      rows); a regular member's org-bound approve records personal rows.
//   2. The R-M2-1 consent UNION -- org consents cover the org's members
//      (the silent authorize path issues a code instead of routing to
//      the consent screen); revoking the pair brings the prompt back.
//   3. The R-M2-4 portal surface (list grouped by client, revoke the
//      pair, role guards, 404 shapes).
//   4. The #219 quota TOCTOU fix -- concurrent creations at the quota
//      boundary leave exactly N rows (org creation, app creation,
//      invitation cap; R-M2-5 advisory locks).
//
// Storage: Postgres-only (org/member/consent rows); cases skip cleanly
// under memory. The union assertions ride the AUTHORIZE silent path --
// the login JSON endpoint issues codes directly without a consent gate,
// so it cannot observe the union.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include "HttpTestClient.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendDelete;
using fulla::test::http::sendGet;
using fulla::test::http::sendPostForm;
using fulla::test::http::sendPostJson;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define ORGCONSENT_SKIP_GUARD                                   \
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
    return std::string("qa-orgconsent-Zx5-") + uniqueSuffix();
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

// Poll sqlInt until it yields `want` (or attempts run out). The consent
// POST returns after the FIRST scope's upsert commits; the remaining
// scopes are fire-and-forget writes that may land milliseconds later.
std::optional<int64_t> awaitSqlInt(const std::string &sql, int64_t want, int attempts = 20)
{
    for (int i = 0; i < attempts; ++i)
    {
        const auto v = sqlInt(sql);
        if (v.has_value() && *v == want)
            return v;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return sqlInt(sql);
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

// Console-client bearer for the /api/me setup APIs (same flow as the
// suite's loginTokenVerbose helpers).
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
      "&scope=openid%20profile&state=oc-bearer"
      "&code_challenge=" + challenge +
      "&code_challenge_method=S256&json=true";
    auto loginResp = sendPostForm("/oauth2/login", loginForm);
    if (loginResp == nullptr || loginResp->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    Json::Value lj;
    if (!parseJsonBody(loginResp, lj))
        return std::nullopt;
    const std::string code = lj.get("code", "").asString();
    if (code.empty())
        return std::nullopt;
    const std::string tokenForm =
      "grant_type=authorization_code&code=" + ::drogon::utils::urlEncode(code) +
      "&redirect_uri=http%3A%2F%2F127.0.0.1%3A5174%2Fadmin%2Fcallback"
      "&client_id=fulla-admin-console&code_verifier=" +
      ::drogon::utils::urlEncode(verifier);
    auto tokenResp = sendPostForm("/oauth2/token", tokenForm);
    if (tokenResp == nullptr || tokenResp->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    Json::Value tj;
    if (!parseJsonBody(tokenResp, tj))
        return std::nullopt;
    const std::string access = tj.get("access_token", "").asString();
    if (access.empty())
        return std::nullopt;
    return access;
}

// POST a form with an explicit session cookie.
::drogon::HttpResponsePtr postFormCookie(const std::string &path,
                                         const std::string &formBody,
                                         const std::string &cookie)
{
    auto client =
      ::drogon::HttpClient::newHttpClient("http://127.0.0.1:5555", ::drogon::app().getLoop());
    auto req = ::drogon::HttpRequest::newHttpRequest();
    req->setMethod(::drogon::Post);
    req->setPath(path);
    req->setContentTypeCode(::drogon::CT_APPLICATION_X_FORM);
    req->setBody(formBody);
    req->addHeader("Cookie", cookie);
    auto [result, resp] = client->sendRequest(req, 30.0);
    if (result != ::drogon::ReqResult::Ok)
        return nullptr;
    return resp;
}

// Establish a browser session for `username` (console login harvests the
// session cookie; the authorize legs below target the org app).
std::string harvestSessionCookie(const std::string &username, const std::string &password)
{
    const std::string verifier = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string challenge =
      ::fulla::drogon::utils::computeCodeChallenge(verifier, "S256");
    auto resp = sendPostForm(
      "/oauth2/login?json=true",
      "username=" + username + "&password=" + password +
        "&client_id=fulla-admin-console"
        "&redirect_uri=http%3A%2F%2F127.0.0.1%3A5174%2Fadmin%2Fcallback"
        "&scope=openid&state=oc-cookie"
        "&code_challenge=" + challenge +
        "&code_challenge_method=S256"
    );
    if (resp == nullptr || resp->getStatusCode() != ::drogon::k200OK)
        return "";
    std::string cookie;
    for (const auto &entry : resp->getCookies())
    {
        if (!cookie.empty())
            cookie += "; ";
        cookie += entry.first + "=" + entry.second.value();
    }
    return cookie;
}

constexpr const char *kRedirect = "http://localhost:5174/orgconsent/callback";

// Drive authorize (prompt=consent + org hint) to the consent redirect and
// return the harvested (consentCsrf, userIdParam, updatedCookie). Empty
// csrf on failure.
struct ConsentRedirect
{
    std::string csrf;
    std::string userIdParam;
    std::string cookie;
};
ConsentRedirect beginConsentFlow(const std::string &cookie,
                                 const std::string &clientId,
                                 const std::string &scope,
                                 const std::string &state,
                                 const std::string &orgSlug,
                                 const std::string &challenge)
{
    ConsentRedirect out;
    out.cookie = cookie;
    auto client =
      ::drogon::HttpClient::newHttpClient("http://127.0.0.1:5555", ::drogon::app().getLoop());
    auto req = ::drogon::HttpRequest::newHttpRequest();
    req->setMethod(::drogon::Get);
    req->setPath("/oauth2/authorize");
    req->setParameter("response_type", "code");
    req->setParameter("client_id", clientId);
    req->setParameter("redirect_uri", kRedirect);
    req->setParameter("scope", scope);
    req->setParameter("state", state);
    req->setParameter("prompt", "consent");
    req->setParameter("org_id", orgSlug);
    req->setParameter("code_challenge", challenge);
    req->setParameter("code_challenge_method", "S256");
    req->addHeader("Cookie", cookie);
    auto [result, resp] = client->sendRequest(req, 30.0);
    if (result != ::drogon::ReqResult::Ok || resp == nullptr)
        return out;
    if (resp->getStatusCode() != ::drogon::k302Found)
        return out;
    for (const auto &entry : resp->getCookies())
    {
        const std::string name = entry.first + "=";
        if (out.cookie.find(name) == std::string::npos)
            out.cookie += "; " + name + entry.second.value();
    }
    const std::string location = resp->getHeader("Location");
    const size_t csrfPos = location.find("consent_csrf=");
    const size_t uidPos = location.find("user_id=");
    if (csrfPos == std::string::npos || uidPos == std::string::npos)
        return out;
    auto part = [&location](size_t start) -> std::string {
        const size_t end = location.find('&', start);
        return location.substr(start, (end == std::string::npos ? location.size() : end) - start);
    };
    out.csrf = part(csrfPos + 13);
    out.userIdParam = ::drogon::utils::urlDecode(part(uidPos + 8));
    return out;
}

// Silent authorize (no prompt) with the org hint; returns the redirect
// Location ("" on transport failure).
std::string silentAuthorizeLocation(const std::string &cookie,
                                    const std::string &clientId,
                                    const std::string &scope,
                                    const std::string &state,
                                    const std::string &orgSlug)
{
    const std::string verifier = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string challenge =
      ::fulla::drogon::utils::computeCodeChallenge(verifier, "S256");
    auto client =
      ::drogon::HttpClient::newHttpClient("http://127.0.0.1:5555", ::drogon::app().getLoop());
    auto req = ::drogon::HttpRequest::newHttpRequest();
    req->setMethod(::drogon::Get);
    req->setPath("/oauth2/authorize");
    req->setParameter("response_type", "code");
    req->setParameter("client_id", clientId);
    req->setParameter("redirect_uri", kRedirect);
    req->setParameter("scope", scope);
    req->setParameter("state", state);
    req->setParameter("org_id", orgSlug);
    req->setParameter("code_challenge", challenge);
    req->setParameter("code_challenge_method", "S256");
    req->addHeader("Cookie", cookie);
    auto [result, resp] = client->sendRequest(req, 30.0);
    if (result != ::drogon::ReqResult::Ok || resp == nullptr)
        return "";
    if (resp->getStatusCode() != ::drogon::k302Found)
        return "";
    return resp->getHeader("Location");
}

// Shared fixture legs: create org + CONFIDENTIAL org-owned app for the
// owner's bearer. Returns the app credentials via out-params.
struct OrgAppFixture
{
    std::string slug;
    int64_t orgId = 0;
    std::string clientId;
    std::string clientSecret;
};
OrgAppFixture makeOrgApp(const std::string &bearer, const std::string &suffix)
{
    OrgAppFixture fx;
    fx.slug = "qa-oc-org-" + suffix;
    {
        Json::Value orgBody;
        orgBody["slug"] = fx.slug;
        orgBody["name"] = "QA OrgConsent " + suffix;
        auto r = sendPostJson("/api/me/organizations", orgBody, bearer);
        if (r == nullptr || r->getStatusCode() != ::drogon::k201Created)
            return fx;
    }
    const auto orgIdOpt = sqlInt("SELECT id FROM organizations WHERE slug = '" + fx.slug + "'");
    if (orgIdOpt.has_value())
        fx.orgId = *orgIdOpt;
    {
        Json::Value app;
        app["name"] = "QA OC App " + suffix;
        app["client_type"] = "CONFIDENTIAL";
        Json::Value uris(Json::arrayValue);
        uris.append(kRedirect);
        app["redirect_uris"] = uris;
        Json::Value scopes(Json::arrayValue);
        scopes.append("openid");
        scopes.append("profile");
        scopes.append("org");
        app["scopes"] = scopes;
        Json::Value grants(Json::arrayValue);
        grants.append("authorization_code");
        grants.append("refresh_token");
        app["allowed_grant_types"] = grants;
        auto r = sendPostJson("/api/me/applications", app, bearer);
        if (r == nullptr || r->getStatusCode() != ::drogon::k201Created)
            return fx;
        Json::Value b;
        if (parseJsonBody(r, b))
        {
            fx.clientId = b["client_id"].asString();
            fx.clientSecret = b["client_secret"].asString();
        }
    }
    {
        Json::Value toOrg;
        toOrg["org_slug"] = fx.slug;
        sendPostJson("/api/me/applications/" + fx.clientId + "/transfer", toOrg, bearer);
    }
    return fx;
}

// Fire N JSON POSTs concurrently (per-request bodies), harvest the status
// codes. The #219 AC leg: all requests are in flight before any response
// is processed by the test thread.
std::vector<int> concurrentPostJson(const std::string &path,
                                    const std::vector<Json::Value> &bodies,
                                    const std::string &bearer)
{
    const int n = static_cast<int>(bodies.size());
    auto client =
      ::drogon::HttpClient::newHttpClient("http://127.0.0.1:5555", ::drogon::app().getLoop());
    auto remaining = std::make_shared<std::atomic<int>>(n);
    auto mu = std::make_shared<std::mutex>();
    auto statuses = std::make_shared<std::vector<int>>();
    auto done = std::make_shared<std::promise<void>>();
    for (int i = 0; i < n; ++i)
    {
        auto req = ::drogon::HttpRequest::newHttpJsonRequest(bodies[i]);
        req->setMethod(::drogon::Post);
        req->setPath(path);
        req->addHeader("Authorization", "Bearer " + bearer);
        client->sendRequest(
          req,
          [remaining, mu, statuses, done](
            ::drogon::ReqResult r, const ::drogon::HttpResponsePtr &resp) {
              int st = 0;
              if (r == ::drogon::ReqResult::Ok && resp)
                  st = resp->getStatusCode();
              {
                  std::lock_guard<std::mutex> lock(*mu);
                  statuses->push_back(st);
              }
              if (remaining->fetch_sub(1) == 1)
                  done->set_value();
          },
          60.0
        );
    }
    done->get_future().wait();
    return *statuses;
}

}  // namespace

// ---------------------------------------------------------------------------
// The core M2 flow: the org owner approves an org-bound consent -> rows
// land in organization_consents (granted_by = owner; NO personal rows for
// the owner) -> a plain member's silent authorize in org context issues
// a code directly (the union covers the member; R-M2-1) -> revoking the
// pair (R-M2-4) brings the member's consent prompt back.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgConsent_AdminApproveUnionCoversMember)
{
    ORGCONSENT_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string owner = "qa_oc_owner_" + suffix;
    const std::string member = "qa_oc_member_" + suffix;
    const std::string ownerPass = randomPassword();
    const std::string memberPass = randomPassword();
    REQUIRE(createVerifiedUser(owner, owner + "@qa.example", ownerPass));
    REQUIRE(createVerifiedUser(member, member + "@qa.example", memberPass));
    auto ownerBearer = consoleBearer(owner, ownerPass);
    REQUIRE(ownerBearer.has_value());

    const OrgAppFixture fx = makeOrgApp(*ownerBearer, suffix);
    REQUIRE(!fx.clientId.empty());
    REQUIRE(fx.orgId > 0);

    // Add the member directly (test fixture; the invite flow is covered
    // elsewhere).
    REQUIRE(sqlExec(
      "INSERT INTO organization_members (organization_id, user_id, role) "
      "SELECT " +
      std::to_string(fx.orgId) + ", id, 'member' FROM users WHERE username = '" + member +
      "'"));

    // 1) Owner drives the org-bound consent round trip (R-M2-2 owner path).
    const std::string ownerCookie = harvestSessionCookie(owner, ownerPass);
    CHECK(!ownerCookie.empty());
    const std::string verifier = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string challenge = ::fulla::drogon::utils::computeCodeChallenge(verifier, "S256");
    const ConsentRedirect cr =
      beginConsentFlow(ownerCookie, fx.clientId, "openid profile org", "oc-grant-1", fx.slug, challenge);
    REQUIRE(!cr.csrf.empty());
    REQUIRE(!cr.userIdParam.empty());
    {
        auto resp = postFormCookie(
          "/oauth2/consent",
          "client_id=" + ::drogon::utils::urlEncode(fx.clientId) +
            "&user_id=" + ::drogon::utils::urlEncode(cr.userIdParam) +
            "&scope=" + ::drogon::utils::urlEncode("openid profile org") +
            "&redirect_uri=" + ::drogon::utils::urlEncode(kRedirect) +
            "&state=oc-grant-1&action=approve" +
            "&code_challenge=" + ::drogon::utils::urlEncode(challenge) +
            "&code_challenge_method=S256&consent_csrf=" + ::drogon::utils::urlEncode(cr.csrf),
          cr.cookie
        );
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k302Found));
        CHECK(resp->getHeader("Location").find("?code=") != std::string::npos);
    }

    // 2) The write went to organization_consents, not personal rows (the
    // trailing scopes are fire-and-forget writes, hence the poll).
    const auto orgRows = awaitSqlInt(
      "SELECT COUNT(*) FROM organization_consents WHERE organization_id = " +
      std::to_string(fx.orgId) + " AND client_id = '" + fx.clientId + "'",
      3);
    REQUIRE(orgRows.has_value());
    CHECK(*orgRows == 3);
    const auto ownerInternal = sqlInt(
      "SELECT id FROM users WHERE username = '" + owner + "'");
    REQUIRE(ownerInternal.has_value());
    const auto grantedByRows = sqlInt(
      "SELECT COUNT(*) FROM organization_consents WHERE organization_id = " +
      std::to_string(fx.orgId) + " AND client_id = '" + fx.clientId +
      "' AND granted_by = " + std::to_string(*ownerInternal));
    REQUIRE(grantedByRows.has_value());
    CHECK(*grantedByRows == 3);
    const auto ownerPersonal = sqlInt(
      "SELECT COUNT(*) FROM oauth2_user_consents WHERE client_id = '" + fx.clientId +
      "' AND internal_user_id = " + std::to_string(*ownerInternal));
    REQUIRE(ownerPersonal.has_value());
    CHECK(*ownerPersonal == 0);

    // 3) The member's silent authorize in org context issues DIRECTLY
    // (the union covers openid+profile; no consent screen).
    const std::string memberCookie = harvestSessionCookie(member, memberPass);
    CHECK(!memberCookie.empty());
    {
        const std::string loc =
          silentAuthorizeLocation(memberCookie, fx.clientId, "openid profile", "oc-silent-1", fx.slug);
        REQUIRE(!loc.empty());
        CHECK(loc.find("?code=") != std::string::npos);
        CHECK(loc.find("consent_csrf=") == std::string::npos);
    }

    // 4) The member's own personal consent rows were never needed.
    const auto memberInternal = sqlInt(
      "SELECT id FROM users WHERE username = '" + member + "'");
    REQUIRE(memberInternal.has_value());
    const auto memberPersonal = sqlInt(
      "SELECT COUNT(*) FROM oauth2_user_consents WHERE client_id = '" + fx.clientId +
      "' AND internal_user_id = " + std::to_string(*memberInternal));
    REQUIRE(memberPersonal.has_value());
    CHECK(*memberPersonal == 0);

    // 5) List (owner) shows the grouped shape; the member is refused.
    {
        auto resp = sendGet("/api/me/organizations/" + fx.slug + "/consents", *ownerBearer);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
        Json::Value body;
        REQUIRE(parseJsonBody(resp, body));
        CHECK(body["total"].asInt() == 1);
        CHECK(body["consents"].isArray());
        CHECK(body["consents"].size() == 1);
        CHECK(body["consents"][0]["client_id"].asString() == fx.clientId);
        CHECK(body["consents"][0]["scopes"].size() == 3);
    }
    {
        auto memberBearer = consoleBearer(member, memberPass);
        REQUIRE(memberBearer.has_value());
        auto resp = sendGet("/api/me/organizations/" + fx.slug + "/consents", *memberBearer);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k403Forbidden));
    }

    // 6) Revoke the pair (owner) -> the member's next silent authorize
    // routes back to the consent screen (O4: future authorizations only).
    {
        auto resp = sendDelete(
          "/api/me/organizations/" + fx.slug + "/consents/" + fx.clientId, *ownerBearer);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
        Json::Value body;
        REQUIRE(parseJsonBody(resp, body));
        CHECK(body["revoked"].asInt64() == 3);
    }
    {
        const std::string loc =
          silentAuthorizeLocation(memberCookie, fx.clientId, "openid profile", "oc-silent-2", fx.slug);
        REQUIRE(!loc.empty());
        CHECK(loc.find("consent_csrf=") != std::string::npos);
    }

    // 7) Second revoke of the now-empty pair -> 404 (count==0 family
    // convention); the list is empty again.
    {
        auto resp = sendDelete(
          "/api/me/organizations/" + fx.slug + "/consents/" + fx.clientId, *ownerBearer);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k404NotFound));
    }
    {
        auto resp = sendGet("/api/me/organizations/" + fx.slug + "/consents", *ownerBearer);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
        Json::Value body;
        REQUIRE(parseJsonBody(resp, body));
        CHECK(body["total"].asInt() == 0);
    }

    // Cleanup. granted_by has no ON DELETE (V036), so the org consent
    // rows must go before the users; owner rows cascade with the users.
    sqlExec("DELETE FROM organization_consents WHERE organization_id = " + std::to_string(fx.orgId));
    sqlExec("DELETE FROM organization_members WHERE organization_id = " + std::to_string(fx.orgId));
    sqlExec("DELETE FROM users WHERE username IN ('" + owner + "', '" + member + "')");
}

// ---------------------------------------------------------------------------
// R-M2-2 member leg: a REGULAR member's org-bound approve writes personal
// rows only (a member cannot grant on the org's behalf).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgConsent_MemberOrgBoundApproveWritesPersonalRows)
{
    ORGCONSENT_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string owner = "qa_oc2_owner_" + suffix;
    const std::string member = "qa_oc2_member_" + suffix;
    const std::string ownerPass = randomPassword();
    const std::string memberPass = randomPassword();
    REQUIRE(createVerifiedUser(owner, owner + "@qa.example", ownerPass));
    REQUIRE(createVerifiedUser(member, member + "@qa.example", memberPass));
    auto ownerBearer = consoleBearer(owner, ownerPass);
    REQUIRE(ownerBearer.has_value());

    const OrgAppFixture fx = makeOrgApp(*ownerBearer, suffix);
    REQUIRE(!fx.clientId.empty());
    REQUIRE(fx.orgId > 0);
    REQUIRE(sqlExec(
      "INSERT INTO organization_members (organization_id, user_id, role) "
      "SELECT " +
      std::to_string(fx.orgId) + ", id, 'member' FROM users WHERE username = '" + member +
      "'"));

    const std::string memberCookie = harvestSessionCookie(member, memberPass);
    CHECK(!memberCookie.empty());
    const std::string verifier = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string challenge = ::fulla::drogon::utils::computeCodeChallenge(verifier, "S256");
    const ConsentRedirect cr =
      beginConsentFlow(memberCookie, fx.clientId, "openid profile org", "oc2-grant-1", fx.slug, challenge);
    REQUIRE(!cr.csrf.empty());
    REQUIRE(!cr.userIdParam.empty());
    {
        auto resp = postFormCookie(
          "/oauth2/consent",
          "client_id=" + ::drogon::utils::urlEncode(fx.clientId) +
            "&user_id=" + ::drogon::utils::urlEncode(cr.userIdParam) +
            "&scope=" + ::drogon::utils::urlEncode("openid profile org") +
            "&redirect_uri=" + ::drogon::utils::urlEncode(kRedirect) +
            "&state=oc2-grant-1&action=approve" +
            "&code_challenge=" + ::drogon::utils::urlEncode(challenge) +
            "&code_challenge_method=S256&consent_csrf=" + ::drogon::utils::urlEncode(cr.csrf),
          cr.cookie
        );
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k302Found));
        CHECK(resp->getHeader("Location").find("?code=") != std::string::npos);
    }

    // Personal rows for the member; zero org rows for this client (the
    // member path is fully sequential, but reuse the poll for symmetry).
    const auto memberInternal = sqlInt(
      "SELECT id FROM users WHERE username = '" + member + "'");
    REQUIRE(memberInternal.has_value());
    const auto personal = awaitSqlInt(
      "SELECT COUNT(*) FROM oauth2_user_consents WHERE client_id = '" + fx.clientId +
      "' AND internal_user_id = " + std::to_string(*memberInternal),
      3);
    REQUIRE(personal.has_value());
    CHECK(*personal == 3);
    const auto orgRows = sqlInt(
      "SELECT COUNT(*) FROM organization_consents WHERE organization_id = " +
      std::to_string(fx.orgId) + " AND client_id = '" + fx.clientId + "'");
    REQUIRE(orgRows.has_value());
    CHECK(*orgRows == 0);

    sqlExec("DELETE FROM organization_members WHERE organization_id = " + std::to_string(fx.orgId));
    sqlExec("DELETE FROM users WHERE username IN ('" + owner + "', '" + member + "')");
}

// ---------------------------------------------------------------------------
// Portal guards: unknown org 404; the list's 401 without a token.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P2_OrgConsent_PortalGuards)
{
    ORGCONSENT_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string owner = "qa_oc3_owner_" + suffix;
    const std::string ownerPass = randomPassword();
    REQUIRE(createVerifiedUser(owner, owner + "@qa.example", ownerPass));
    auto ownerBearer = consoleBearer(owner, ownerPass);
    REQUIRE(ownerBearer.has_value());

    {
        auto resp =
          sendGet("/api/me/organizations/no-such-org-xyz/consents", *ownerBearer);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k404NotFound));
    }
    {
        auto resp = sendGet("/api/me/organizations/no-such-org-xyz/consents", "");
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k401Unauthorized));
    }
    {
        auto resp = sendDelete(
          "/api/me/organizations/no-such-org-xyz/consents/app_x", *ownerBearer);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k404NotFound));
    }

    sqlExec("DELETE FROM users WHERE username = '" + owner + "'");
}

// ---------------------------------------------------------------------------
// #219 / R-M2-5, leg 1: concurrent org creation at the max_orgs_per_user
// boundary (default 3; 2 pre-existing) -- exactly one of three concurrent
// creates succeeds and the owner ends with exactly 3 orgs.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgConsent_Quota219_ConcurrentOrgCreation)
{
    ORGCONSENT_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string user = "qa_oc4_user_" + suffix;
    const std::string pass = randomPassword();
    REQUIRE(createVerifiedUser(user, user + "@qa.example", pass));
    auto bearer = consoleBearer(user, pass);
    REQUIRE(bearer.has_value());

    for (int i = 0; i < 2; ++i)
    {
        Json::Value orgBody;
        orgBody["slug"] = "qa-oc4-org-" + std::to_string(i) + "-" + suffix;
        orgBody["name"] = "QA OC4 Org " + std::to_string(i);
        auto r = sendPostJson("/api/me/organizations", orgBody, *bearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k201Created));
    }

    std::vector<Json::Value> bodies;
    for (int i = 0; i < 3; ++i)
    {
        Json::Value orgBody;
        orgBody["slug"] = "qa-oc4-race-" + std::to_string(i) + "-" + suffix;
        orgBody["name"] = "QA OC4 Race " + std::to_string(i);
        bodies.push_back(orgBody);
    }
    const std::vector<int> statuses =
      concurrentPostJson("/api/me/organizations", bodies, *bearer);
    REQUIRE(statuses.size() == 3);
    int created = 0;
    int conflict = 0;
    for (const int st : statuses)
    {
        if (st == 201)
            ++created;
        if (st == 409)
            ++conflict;
    }
    CHECK(created == 1);
    CHECK(conflict == 2);

    const auto owned = sqlInt(
      "SELECT COUNT(*) FROM organization_members m JOIN users u ON u.id = m.user_id "
      "WHERE u.username = '" + user + "' AND m.role = 'owner'");
    REQUIRE(owned.has_value());
    CHECK(*owned == 3);

    sqlExec(
      "DELETE FROM organization_members WHERE user_id = (SELECT id FROM users WHERE username = '" +
      user + "')");
    sqlExec("DELETE FROM users WHERE username = '" + user + "'");
}

// ---------------------------------------------------------------------------
// #219 / R-M2-5, leg 2: concurrent app creation at the personal quota
// boundary (default 5; 4 pre-existing). The losers hit either the quota
// or the 24h rate limit (both 4xx) -- the invariant is exactly 5 rows.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgConsent_Quota219_ConcurrentAppCreation)
{
    ORGCONSENT_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string user = "qa_oc5_user_" + suffix;
    const std::string pass = randomPassword();
    REQUIRE(createVerifiedUser(user, user + "@qa.example", pass));
    auto bearer = consoleBearer(user, pass);
    REQUIRE(bearer.has_value());

    auto appBody = [&suffix](int i) -> Json::Value {
        Json::Value app;
        app["name"] = "QA OC5 App " + std::to_string(i) + " " + suffix;
        app["client_type"] = "CONFIDENTIAL";
        Json::Value uris(Json::arrayValue);
        uris.append(kRedirect);
        app["redirect_uris"] = uris;
        Json::Value scopes(Json::arrayValue);
        scopes.append("openid");
        app["scopes"] = scopes;
        Json::Value grants(Json::arrayValue);
        grants.append("client_credentials");
        app["allowed_grant_types"] = grants;
        return app;
    };

    for (int i = 0; i < 4; ++i)
    {
        auto r = sendPostJson("/api/me/applications", appBody(100 + i), *bearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k201Created));
    }

    std::vector<Json::Value> bodies;
    for (int i = 0; i < 3; ++i)
        bodies.push_back(appBody(200 + i));
    const std::vector<int> statuses =
      concurrentPostJson("/api/me/applications", bodies, *bearer);
    REQUIRE(statuses.size() == 3);
    int created = 0;
    for (const int st : statuses)
    {
        if (st == 201)
            ++created;
    }
    CHECK(created == 1);

    const auto alive = sqlInt(
      "SELECT COUNT(*) FROM oauth2_client_owners o JOIN users u ON u.id = o.creator_user_id "
      "JOIN oauth2_clients c ON c.client_id = o.client_id AND c.deleted_at IS NULL "
      "WHERE u.username = '" + user + "' AND o.org_id IS NULL");
    REQUIRE(alive.has_value());
    CHECK(*alive == 5);

    // Cleanup: owner rows cascade with the user (creator FK); the
    // client rows themselves stay (timestamp-unique, existing suite
    // convention).
    sqlExec("DELETE FROM users WHERE username = '" + user + "'");
}

// ---------------------------------------------------------------------------
// #219 / R-M2-5, leg 3: concurrent invitations at the per-org pending cap
// (default 20) -- exactly 20 pending rows survive 22 concurrent invites.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgConsent_Quota219_ConcurrentInvitationCap)
{
    ORGCONSENT_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string user = "qa_oc6_user_" + suffix;
    const std::string pass = randomPassword();
    REQUIRE(createVerifiedUser(user, user + "@qa.example", pass));
    auto bearer = consoleBearer(user, pass);
    REQUIRE(bearer.has_value());

    const std::string slug = "qa-oc6-org-" + suffix;
    {
        Json::Value orgBody;
        orgBody["slug"] = slug;
        orgBody["name"] = "QA OC6 Org " + suffix;
        auto r = sendPostJson("/api/me/organizations", orgBody, *bearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k201Created));
    }

    std::vector<Json::Value> bodies;
    for (int i = 0; i < 22; ++i)
    {
        Json::Value invite;
        invite["email"] = "race-" + std::to_string(i) + "-" + suffix + "@qa.example";
        invite["role"] = "member";
        bodies.push_back(invite);
    }
    const std::vector<int> statuses = concurrentPostJson(
      "/api/me/organizations/" + slug + "/invitations", bodies, *bearer);
    REQUIRE(statuses.size() == 22);
    int created = 0;
    for (const int st : statuses)
    {
        if (st == 201)
            ++created;
    }
    CHECK(created == 20);

    const auto pending = sqlInt(
      "SELECT COUNT(*) FROM organization_invitations i JOIN organizations o ON o.id = i.organization_id "
      "WHERE o.slug = '" + slug + "' AND i.accepted_at IS NULL");
    REQUIRE(pending.has_value());
    CHECK(*pending == 20);

    sqlExec("DELETE FROM organization_invitations WHERE organization_id = (SELECT id FROM organizations WHERE slug = '" + slug + "')");
    sqlExec("DELETE FROM organization_members WHERE organization_id = (SELECT id FROM organizations WHERE slug = '" + slug + "')");
    sqlExec("DELETE FROM users WHERE username = '" + user + "'");
}
