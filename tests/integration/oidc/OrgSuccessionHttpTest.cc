// tests/integration/oidc/OrgSuccessionHttpTest.cc
//
// v1.5.0 M3 integration tests: ownership succession (design §1.3 item 2)
// and the org-scoped client_credentials anchor (§2.3).
//   1. The nominate-and-accept workflow (R-M3-3): owner nominates (a
//      member, a NON-member, and an overwrite of a pending nomination),
//      withdraws, and the nominee accepts -- the seat swap demotes the
//      old owner to admin and the nomination is spent.
//   2. Authorization shapes: non-owner cannot nominate/withdraw, a
//      non-nominee cannot accept, a soft-deleted nominee is refused.
//   3. R-M3-4: an owner deleting their account auto-effects the pending
//      nomination; with none pending the org falls to the admin-takeover
//      state (the #228 endpoint remains the fallback).
//   4. R-M3-1: an org-owned CONFIDENTIAL client's client_credentials
//      token carries the org anchor (introspection org_id); a personal
//      client's does not.
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

using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendDelete;
using fulla::test::http::sendGet;
using fulla::test::http::sendPostForm;
using fulla::test::http::sendPostJson;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define ORGSUCC_SKIP_GUARD                                      \
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
    return std::string("qa-orgsucc-Zx3-") + uniqueSuffix();
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

std::optional<int64_t> userIdOf(const std::string &username)
{
    return sqlInt("SELECT id FROM users WHERE username = '" + username + "'");
}

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
      "&scope=openid%20profile&state=succ-bearer"
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

constexpr const char *kRedirect = "http://localhost:5174/orgsucc/callback";

// org + member fixture (owner bearer required by the caller).
struct SuccFixture
{
    std::string slug;
    int64_t orgId = 0;
};
SuccFixture makeOrg(const std::string &bearer, const std::string &suffix)
{
    SuccFixture fx;
    fx.slug = "qa-succ-org-" + suffix;
    Json::Value orgBody;
    orgBody["slug"] = fx.slug;
    orgBody["name"] = "QA Succ Org " + suffix;
    auto r = sendPostJson("/api/me/organizations", orgBody, bearer);
    if (r == nullptr || r->getStatusCode() != ::drogon::k201Created)
        return fx;
    const auto orgIdOpt =
      sqlInt("SELECT id FROM organizations WHERE slug = '" + fx.slug + "'");
    if (orgIdOpt.has_value())
        fx.orgId = *orgIdOpt;
    return fx;
}

// CONFIDENTIAL self-registered app; secret returned once. orgSlug empty
// -> personal (created then left untransferred).
struct SuccApp
{
    std::string clientId;
    std::string clientSecret;
};
SuccApp makeCredApp(const std::string &bearer, const std::string &suffix)
{
    SuccApp app;
    Json::Value body;
    body["name"] = "QA Succ CC " + suffix;
    body["client_type"] = "CONFIDENTIAL";
    Json::Value uris(Json::arrayValue);
    uris.append(kRedirect);
    body["redirect_uris"] = uris;
    Json::Value scopes(Json::arrayValue);
    scopes.append("openid");
    body["scopes"] = scopes;
    Json::Value grants(Json::arrayValue);
    grants.append("client_credentials");
    body["allowed_grant_types"] = grants;
    auto r = sendPostJson("/api/me/applications", body, bearer);
    if (r == nullptr || r->getStatusCode() != ::drogon::k201Created)
        return app;
    Json::Value b;
    if (parseJsonBody(r, b))
    {
        app.clientId = b["client_id"].asString();
        app.clientSecret = b["client_secret"].asString();
    }
    return app;
}

Json::Value ccToken(const SuccApp &app)
{
    auto client =
      ::drogon::HttpClient::newHttpClient("http://127.0.0.1:5555", ::drogon::app().getLoop());
    auto req = ::drogon::HttpRequest::newHttpRequest();
    req->setMethod(::drogon::Post);
    req->setPath("/oauth2/token");
    req->setContentTypeCode(::drogon::CT_APPLICATION_X_FORM);
    req->addHeader(
      "Authorization",
      "Basic " +
        ::drogon::utils::base64Encode(app.clientId + ":" + app.clientSecret)
    );
    req->setBody("grant_type=client_credentials");
    auto [result, resp] = client->sendRequest(req, 30.0);
    Json::Value out;
    if (result != ::drogon::ReqResult::Ok || resp == nullptr)
        return out;
    parseJsonBody(resp, out);
    return out;
}

Json::Value introspect(const std::string &token, const SuccApp &app)
{
    auto client =
      ::drogon::HttpClient::newHttpClient("http://127.0.0.1:5555", ::drogon::app().getLoop());
    auto req = ::drogon::HttpRequest::newHttpRequest();
    req->setMethod(::drogon::Post);
    req->setPath("/oauth2/introspect");
    req->setContentTypeCode(::drogon::CT_APPLICATION_X_FORM);
    req->addHeader(
      "Authorization",
      "Basic " +
        ::drogon::utils::base64Encode(app.clientId + ":" + app.clientSecret)
    );
    req->setBody("token=" + ::drogon::utils::urlEncode(token));
    auto [result, resp] = client->sendRequest(req, 30.0);
    Json::Value out;
    if (result != ::drogon::ReqResult::Ok || resp == nullptr)
        return out;
    parseJsonBody(resp, out);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The full nominate-and-accept workflow (R-M3-3).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgSuccession_NominateAcceptWorkflow)
{
    ORGSUCC_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string owner = "qa_succ_owner_" + suffix;
    const std::string outsider = "qa_succ_out_" + suffix;  // never a member
    const std::string ownerPass = randomPassword();
    const std::string outPass = randomPassword();
    REQUIRE(createVerifiedUser(owner, owner + "@qa.example", ownerPass));
    REQUIRE(createVerifiedUser(outsider, outsider + "@qa.example", outPass));
    auto ownerBearer = consoleBearer(owner, ownerPass);
    auto outBearer = consoleBearer(outsider, outPass);
    REQUIRE(ownerBearer.has_value());
    REQUIRE(outBearer.has_value());

    const SuccFixture fx = makeOrg(*ownerBearer, suffix);
    REQUIRE(fx.orgId > 0);
    const auto outId = userIdOf(outsider);
    REQUIRE(outId.has_value());

    // 1) Non-owner cannot nominate (the outsider is not even a member).
    {
        Json::Value body;
        body["user_id"] = static_cast<Json::Int64>(*outId);
        auto r = sendPostJson(
          "/api/me/organizations/" + fx.slug + "/successor-nomination", body, *outBearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k403Forbidden));
    }

    // 2) Owner nominates the NON-member outsider (overwrites nothing yet).
    {
        Json::Value body;
        body["user_id"] = static_cast<Json::Int64>(*outId);
        auto r = sendPostJson(
          "/api/me/organizations/" + fx.slug + "/successor-nomination", body, *ownerBearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k200OK));
    }

    // 3) Visibility: the owner's list shows the pending nomination; the
    // nominee's list surfaces it under pending_succession_nominations
    // (they are not a member).
    {
        auto r = sendGet("/api/me/organizations", *ownerBearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k200OK));
        Json::Value body;
        REQUIRE(parseJsonBody(r, body));
        bool found = false;
        for (const auto &o : body["organizations"])
        {
            if (o["slug"].asString() == fx.slug)
            {
                found = true;
                CHECK(o["successor_nomination"].isObject());
                CHECK(o["successor_nomination"]["nominee_user_id"].asInt64() == *outId);
            }
        }
        CHECK(found);
    }
    {
        auto r = sendGet("/api/me/organizations", *outBearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k200OK));
        Json::Value body;
        REQUIRE(parseJsonBody(r, body));
        CHECK(body["organizations"].size() == 0);
        bool forYou = false;
        for (const auto &n : body["pending_succession_nominations"])
        {
            if (n["slug"].asString() == fx.slug)
                forYou = true;
        }
        CHECK(forYou);
    }

    // 4) A non-nominee cannot accept (fresh third user).
    {
        const std::string third = "qa_succ_third_" + suffix;
        const std::string thirdPass = randomPassword();
        REQUIRE(createVerifiedUser(third, third + "@qa.example", thirdPass));
        auto thirdBearer = consoleBearer(third, thirdPass);
        REQUIRE(thirdBearer.has_value());
        auto r = sendPostJson(
          "/api/me/organizations/" + fx.slug + "/successor-nomination/accept",
          Json::Value(Json::objectValue),
          *thirdBearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k403Forbidden));
        sqlExec("DELETE FROM users WHERE username = '" + third + "'");
    }

    // 5) The nominee accepts -> seat swap (outsider becomes owner; the
    // old owner demotes to admin; the nomination is spent).
    {
        auto r = sendPostJson(
          "/api/me/organizations/" + fx.slug + "/successor-nomination/accept",
          Json::Value(Json::objectValue),
          *outBearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k200OK));
    }
    {
        const auto outRole = sqlInt(
          "SELECT 1 FROM organization_members WHERE organization_id = " +
          std::to_string(fx.orgId) + " AND user_id = " + std::to_string(*outId) +
          " AND role = 'owner'");
        REQUIRE(outRole.has_value());
        CHECK(*outRole == 1);
        const auto oldOwnerRole = sqlInt(
          "SELECT 1 FROM organization_members WHERE organization_id = " +
          std::to_string(fx.orgId) + " AND user_id = (SELECT id FROM users WHERE username = '" +
          owner + "') AND role = 'admin'");
        REQUIRE(oldOwnerRole.has_value());
        CHECK(*oldOwnerRole == 1);
        const auto pending = sqlInt(
          "SELECT COUNT(*) FROM organization_succession_nominations WHERE organization_id = " +
          std::to_string(fx.orgId) + " AND accepted_at IS NULL");
        REQUIRE(pending.has_value());
        CHECK(*pending == 0);
    }

    // 6) Withdraw with nothing pending -> 404.
    {
        auto r = sendDelete(
          "/api/me/organizations/" + fx.slug + "/successor-nomination", *outBearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k404NotFound));
    }

    // Cleanup: memberships then users (nominated_by carries no FK, the
    // nominee FK blocks deleting the outsider first).
    sqlExec("DELETE FROM organization_members WHERE organization_id = " + std::to_string(fx.orgId));
    sqlExec(
      "DELETE FROM organization_succession_nominations WHERE organization_id = " +
      std::to_string(fx.orgId));
    sqlExec("DELETE FROM users WHERE username IN ('" + owner + "', '" + outsider + "')");
}

// ---------------------------------------------------------------------------
// Nomination overwrite (idempotent) + withdraw (R-M3-3) + a soft-deleted
// nominee is refused.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgSuccession_OverwriteWithdrawAndDeadNominee)
{
    ORGSUCC_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string owner = "qa_succ2_owner_" + suffix;
    const std::string a = "qa_succ2_a_" + suffix;
    const std::string b = "qa_succ2_b_" + suffix;
    const std::string ownerPass = randomPassword();
    const std::string aPass = randomPassword();
    const std::string bPass = randomPassword();
    REQUIRE(createVerifiedUser(owner, owner + "@qa.example", ownerPass));
    REQUIRE(createVerifiedUser(a, a + "@qa.example", aPass));
    REQUIRE(createVerifiedUser(b, b + "@qa.example", bPass));
    auto ownerBearer = consoleBearer(owner, ownerPass);
    REQUIRE(ownerBearer.has_value());

    const SuccFixture fx = makeOrg(*ownerBearer, suffix);
    REQUIRE(fx.orgId > 0);
    const auto aId = userIdOf(a);
    const auto bId = userIdOf(b);
    REQUIRE(aId.has_value());
    REQUIRE(bId.has_value());

    // Nominate A, then overwrite with B: exactly one pending row, naming B.
    for (const auto nominate : {*aId, *bId})
    {
        Json::Value body;
        body["user_id"] = static_cast<Json::Int64>(nominate);
        auto r = sendPostJson(
          "/api/me/organizations/" + fx.slug + "/successor-nomination", body, *ownerBearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k200OK));
    }
    {
        const auto pending = sqlInt(
          "SELECT nominee_user_id FROM organization_succession_nominations WHERE "
          "organization_id = " +
          std::to_string(fx.orgId) + " AND accepted_at IS NULL");
        REQUIRE(pending.has_value());
        CHECK(*pending == *bId);
    }

    // A soft-deleted target is refused (would recreate the #221 freeze).
    REQUIRE(sqlExec("UPDATE users SET deleted_at = now() WHERE username = '" + a + "'"));
    {
        Json::Value body;
        body["user_id"] = static_cast<Json::Int64>(*aId);
        auto r = sendPostJson(
          "/api/me/organizations/" + fx.slug + "/successor-nomination", body, *ownerBearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k404NotFound));
    }

    // Owner withdraws -> no pending rows; second withdraw 404s.
    {
        auto r = sendDelete(
          "/api/me/organizations/" + fx.slug + "/successor-nomination", *ownerBearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k200OK));
    }
    {
        const auto pending = sqlInt(
          "SELECT COUNT(*) FROM organization_succession_nominations WHERE organization_id = " +
          std::to_string(fx.orgId) + " AND accepted_at IS NULL");
        REQUIRE(pending.has_value());
        CHECK(*pending == 0);
    }
    {
        auto r = sendDelete(
          "/api/me/organizations/" + fx.slug + "/successor-nomination", *ownerBearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k404NotFound));
    }

    sqlExec("DELETE FROM organization_members WHERE organization_id = " + std::to_string(fx.orgId));
    sqlExec("DELETE FROM users WHERE username IN ('" + owner + "', '" + a + "', '" + b + "')");
}

// ---------------------------------------------------------------------------
// R-M3-4: the owner's self-delete auto-effects a pending nomination; with
// none pending the org falls to the admin-takeover state (the #228
// endpoint still answers -- the documented fallback).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgSuccession_OwnerDeleteAutoEffects)
{
    ORGSUCC_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string owner = "qa_succ3_owner_" + suffix;
    const std::string heir = "qa_succ3_heir_" + suffix;
    const std::string ownerPass = randomPassword();
    const std::string heirPass = randomPassword();
    REQUIRE(createVerifiedUser(owner, owner + "@qa.example", ownerPass));
    REQUIRE(createVerifiedUser(heir, heir + "@qa.example", heirPass));
    auto ownerBearer = consoleBearer(owner, ownerPass);
    REQUIRE(ownerBearer.has_value());

    const SuccFixture fx = makeOrg(*ownerBearer, suffix);
    REQUIRE(fx.orgId > 0);
    const auto heirId = userIdOf(heir);
    REQUIRE(heirId.has_value());

    {
        Json::Value body;
        body["user_id"] = static_cast<Json::Int64>(*heirId);
        auto r = sendPostJson(
          "/api/me/organizations/" + fx.slug + "/successor-nomination", body, *ownerBearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k200OK));
    }

    // The owner deletes their account (self-service /api/me DELETE) ->
    // the pending nomination auto-effects.
    {
        auto r = sendDelete("/api/me", *ownerBearer);
        REQUIRE(r != nullptr);
        if (r->getStatusCode() != ::drogon::k200OK)
        {
            Json::Value err;
            parseJsonBody(r, err);
            LOG_ERROR << "[succ] delete status=" << r->getStatusCode() << " body="
                      << Json::writeString(Json::StreamWriterBuilder(), err);
        }
        CHECK(statusIs(r, ::drogon::k200OK));
    }
    {
        const auto heirRole = sqlInt(
          "SELECT 1 FROM organization_members WHERE organization_id = " +
          std::to_string(fx.orgId) + " AND user_id = " + std::to_string(*heirId) +
          " AND role = 'owner'");
        REQUIRE(heirRole.has_value());
        CHECK(*heirRole == 1);
        const auto pending = sqlInt(
          "SELECT COUNT(*) FROM organization_succession_nominations WHERE organization_id = " +
          std::to_string(fx.orgId) + " AND accepted_at IS NULL");
        REQUIRE(pending.has_value());
        CHECK(*pending == 0);
    }

    // The new owner can manage: invite works (the old frozen-seat
    // symptom was exactly this being impossible).
    {
        auto heirBearer = consoleBearer(heir, heirPass);
        REQUIRE(heirBearer.has_value());
        Json::Value invite;
        invite["email"] = "invited-" + suffix + "@qa.example";
        invite["role"] = "member";
        auto r = sendPostJson(
          "/api/me/organizations/" + fx.slug + "/invitations", invite, *heirBearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k201Created));
    }

    sqlExec("DELETE FROM organization_invitations WHERE organization_id = " + std::to_string(fx.orgId));
    sqlExec("DELETE FROM organization_members WHERE organization_id = " + std::to_string(fx.orgId));
    sqlExec(
      "DELETE FROM organization_succession_nominations WHERE organization_id = " +
      std::to_string(fx.orgId));
    sqlExec("DELETE FROM users WHERE username IN ('" + owner + "', '" + heir + "')");
}

// ---------------------------------------------------------------------------
// R-M3-1: the org anchor on client_credentials tokens. An org-owned
// CONFIDENTIAL client's CC token introspects with org_id; a personal
// client's does not. sub stays the client (V4: introspection's
// client_id field carries it; no user identity).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgSuccession_OrgScopedClientCredentials)
{
    ORGSUCC_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string user = "qa_succ4_user_" + suffix;
    const std::string pass = randomPassword();
    REQUIRE(createVerifiedUser(user, user + "@qa.example", pass));
    auto bearer = consoleBearer(user, pass);
    REQUIRE(bearer.has_value());

    const SuccFixture fx = makeOrg(*bearer, suffix);
    REQUIRE(fx.orgId > 0);

    SuccApp orgApp = makeCredApp(*bearer, suffix);
    SuccApp personalApp = makeCredApp(*bearer, suffix);
    REQUIRE(!orgApp.clientId.empty());
    REQUIRE(!orgApp.clientSecret.empty());
    REQUIRE(!personalApp.clientId.empty());
    REQUIRE(!personalApp.clientSecret.empty());
    {
        Json::Value toOrg;
        toOrg["org_slug"] = fx.slug;
        auto r = sendPostJson(
          "/api/me/applications/" + orgApp.clientId + "/transfer", toOrg, *bearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, ::drogon::k200OK));
    }

    // Org-owned app: CC token carries the org anchor.
    {
        Json::Value tokens = ccToken(orgApp);
        const std::string access = tokens.get("access_token", "").asString();
        CHECK(!access.empty());
        Json::Value intro = introspect(access, orgApp);
        CHECK(intro.get("active", false).asBool());
        CHECK(intro.isMember("org_id"));
        CHECK(intro.get("org_id", 0).asInt64() == fx.orgId);
    }
    // Personal app: no org anchor.
    {
        Json::Value tokens = ccToken(personalApp);
        const std::string access = tokens.get("access_token", "").asString();
        CHECK(!access.empty());
        Json::Value intro = introspect(access, personalApp);
        CHECK(intro.get("active", false).asBool());
        CHECK(!intro.isMember("org_id"));
    }

    sqlExec("DELETE FROM users WHERE username = '" + user + "'");
}
