// tests/integration/oidc/OrgContextHttpTest.cc
//
// v1.5.0 M1 integration tests: the authorize org_id parameter and the org
// context chain (design §2.1). The OrgContextGate logic is shared by all
// three issuance entries (authorize silent, login POST, consent POST);
// these cases drive it through the LOGIN path (stateless, no session
// juggling): authorize carries org_id into the login URL, the login POST
// re-runs the gate, and the issued code binds the org context onto the
// token chain (introspection org_id; userinfo/id_token org_ctx gated by
// the org scope; refresh preservation; O7 real-time membership).
//
// Storage: Postgres-only (org/member/owner rows); cases skip cleanly
// under memory.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include "HttpTestClient.h"

#include <chrono>
#include <future>
#include <optional>
#include <sstream>
#include <string>

using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendGet;
using fulla::test::http::sendPostForm;
using fulla::test::http::sendPostJson;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define ORGCTX_SKIP_GUARD                                       \
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
    return std::string("qa-orgctx-Zx7-") + uniqueSuffix();
}

void dumpBody(const ::drogon::HttpResponsePtr &resp, const char *where)
{
    Json::Value body;
    const bool isJson = parseJsonBody(resp, body);
    LOG_ERROR << "[orgctx:" << where << "] status="
              << (resp ? std::to_string(resp->getStatusCode()) : std::string("null"))
              << (isJson
                    ? " body=" + Json::writeString(Json::StreamWriterBuilder(), body)
                    : std::string(" raw=") +
                        std::string(resp->getBody().data(), resp->getBody().size()));
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
    auto db = ::drogon::app().getDbClient();
    std::promise<bool> done;
    db->execSqlAsync(
      "UPDATE users SET email_verified = true WHERE username = $1",
      [&done](const ::drogon::orm::Result &) { done.set_value(true); },
      [&done](const ::drogon::orm::DrogonDbException &) { done.set_value(false); },
      username);
    return done.get_future().get();
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

// PKCE login (json=true) against `clientId`; returns the response plus the
// verifier used (needed for the exchange). orgRef is the v1.5.0 org_id
// hint carried through the form.
struct LoginResult
{
    ::drogon::HttpResponsePtr resp;
    std::string verifier;
};

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
      "&state=orgctx-state-1"
      "&code_challenge=" + challenge +
      "&code_challenge_method=S256&json=true";
    if (!orgRef.empty())
        form += "&org_id=" + ::drogon::utils::urlEncode(orgRef);
    out.resp = sendPostForm("/oauth2/login", form);
    return out;
}

// POST a form with client authentication adaptive to the client type:
// Basic when a secret is supplied (F-017: self-registered CONFIDENTIAL apps
// declare token_endpoint_auth_method=client_secret_basic), form client_id
// for PUBLIC/seeded clients without secrets.
::drogon::HttpResponsePtr sendPostFormClientAuth(const std::string &path,
                                                 const std::string &formBody,
                                                 const std::string &clientId,
                                                 const std::string &clientSecret)
{
    try
    {
        auto client =
          ::drogon::HttpClient::newHttpClient("http://127.0.0.1:5555", ::drogon::app().getLoop());
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Post);
        req->setPath(path);
        req->setContentTypeCode(::drogon::CT_APPLICATION_X_FORM);
        std::string body = formBody;
        if (clientSecret.empty())
        {
            body += "&client_id=" + ::drogon::utils::urlEncode(clientId);
        }
        else
        {
            req->addHeader(
              "Authorization",
              "Basic " + ::drogon::utils::base64Encode(clientId + ":" + clientSecret)
            );
        }
        req->setBody(body);
        auto [result, resp] = client->sendRequest(req, 30.0);
        if (result != ::drogon::ReqResult::Ok || resp == nullptr)
            return nullptr;
        return resp;
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "sendPostFormClientAuth(" << path << ") failed: " << e.what();
        return nullptr;
    }
}

// Exchange a code for the token JSON (or the OAuth error JSON).
Json::Value exchangeCode(const std::string &code,
                         const std::string &verifier,
                         const std::string &clientId,
                         const std::string &clientSecret,
                         const std::string &redirectUri)
{
    const std::string form =
      "grant_type=authorization_code&code=" + ::drogon::utils::urlEncode(code) +
      "&redirect_uri=" + ::drogon::utils::urlEncode(redirectUri) +
      "&code_verifier=" + ::drogon::utils::urlEncode(verifier);
    auto resp = sendPostFormClientAuth("/oauth2/token", form, clientId, clientSecret);
    Json::Value out;
    if (resp == nullptr || !parseJsonBody(resp, out))
        return out;
    if (!out.isMember("access_token"))
    {
        LOG_ERROR << "[orgctx:exchange] status="
                  << (resp ? std::to_string(resp->getStatusCode()) : std::string("null"))
                  << " body="
                  << Json::writeString(Json::StreamWriterBuilder(), out);
    }
    return out;
}

// Refresh grant; returns the token JSON.
Json::Value refreshTokens(const std::string &refreshToken,
                          const std::string &clientId,
                          const std::string &clientSecret)
{
    const std::string form =
      "grant_type=refresh_token&refresh_token=" +
      ::drogon::utils::urlEncode(refreshToken);
    auto resp = sendPostFormClientAuth("/oauth2/token", form, clientId, clientSecret);
    Json::Value out;
    if (resp == nullptr || !parseJsonBody(resp, out))
        return out;
    return out;
}

// RFC 7662 introspection with client authentication.
Json::Value introspectWith(const std::string &token,
                           const std::string &clientId,
                           const std::string &clientSecret)
{
    const std::string form = "token=" + ::drogon::utils::urlEncode(token);
    auto resp = sendPostFormClientAuth("/oauth2/introspect", form, clientId, clientSecret);
    Json::Value out;
    if (resp == nullptr || !parseJsonBody(resp, out))
        return out;
    return out;
}

// GET /oauth2/userinfo with a bearer token.
Json::Value userInfoFor(const std::string &accessToken)
{
    auto resp = sendGet("/oauth2/userinfo", accessToken);
    Json::Value out;
    if (resp == nullptr || !parseJsonBody(resp, out))
        return out;
    return out;
}

// Decode a JWT's payload segment into JSON (no signature check needed
// here; the test only inspects claims).
Json::Value jwtPayload(const std::string &jwt)
{
    Json::Value out;
    const size_t dot = jwt.find('.');
    const size_t dot2 = jwt.find('.', dot + 1);
    if (dot == std::string::npos || dot2 == std::string::npos)
        return out;
    std::string seg = jwt.substr(dot + 1, dot2 - dot - 1);
    for (char &c : seg)
    {
        if (c == '-')
            c = '+';
        else if (c == '_')
            c = '/';
    }
    while (seg.size() % 4 != 0)
        seg += '=';
    const std::string decoded = ::drogon::utils::base64Decode(seg);
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream stream(decoded);
    if (!Json::parseFromStream(reader, stream, &out, &errors))
        return Json::Value();
    return out;
}

// Console-client bearer for the /api/me setup APIs (organization + app
// creation). Same flow as the suite's loginTokenVerbose helpers.
std::optional<std::string> consoleBearer(const std::string &username,
                                         const std::string &password)
{
    auto login = pkceLogin(username, password, "fulla-admin-console",
                           "http://localhost:5174/admin/callback", "openid profile");
    if (login.resp == nullptr || login.resp->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    Json::Value lj;
    if (!parseJsonBody(login.resp, lj))
        return std::nullopt;
    const std::string code = lj.get("code", "").asString();
    if (code.empty())
        return std::nullopt;
    Json::Value tj = exchangeCode(code, login.verifier, "fulla-admin-console", "",
                                  "http://localhost:5174/admin/callback");
    const std::string access = tj.get("access_token", "").asString();
    if (access.empty())
        return std::nullopt;
    return access;
}

// POST a form with an explicit session cookie (the consent round trip
// needs the browser session; ConsentMultiFlow's post() pattern).
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

constexpr const char *kRedirect = "http://localhost:5174/orgctx/callback";

}  // namespace

// ---------------------------------------------------------------------------
// Happy path + chain inheritance: the org owner authorizes the org's app
// with org_id=<slug> + the org scope. The login-issued code binds the org;
// the exchange inherits it onto the token pair; introspection exposes
// org_id; userinfo and the id_token carry org_ctx (id + name + role);
// refresh preserves the binding. O7: removing the membership drops
// org_ctx from userinfo immediately (introspection org_id is the token's
// own binding and stays).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgContext_LoginChain_CarriesOrgBinding)
{
    ORGCTX_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string userA = "qa_orgctx_" + suffix;
    const std::string passA = randomPassword();
    REQUIRE(createVerifiedUser(userA, userA + "@qa.example", passA));

    auto bearer = consoleBearer(userA, passA);
    REQUIRE(bearer.has_value());

    // org + org-owned CONFIDENTIAL app (secret shown once).
    const std::string slug = "qa-orgctx-org-" + suffix;
    {
        Json::Value orgBody;
        orgBody["slug"] = slug;
        orgBody["name"] = "QA OrgCtx Org " + suffix;
        auto orgResp = sendPostJson("/api/me/organizations", orgBody, *bearer);
        REQUIRE(orgResp != nullptr);
        CHECK(statusIs(orgResp, drogon::k201Created));
    }
    std::string clientId, clientSecret;
    {
        Json::Value app;
        app["name"] = "QA OrgCtx App " + suffix;
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
        auto appResp = sendPostJson("/api/me/applications", app, *bearer);
        REQUIRE(appResp != nullptr);
        dumpBody(appResp, "create app");
        CHECK(statusIs(appResp, drogon::k201Created));
        Json::Value appBody;
        REQUIRE(parseJsonBody(appResp, appBody));
        clientId = appBody["client_id"].asString();
        clientSecret = appBody["client_secret"].asString();
        CHECK(!clientId.empty());
        CHECK(!clientSecret.empty());
    }
    {
        Json::Value toOrg;
        toOrg["org_slug"] = slug;
        auto transferResp =
          sendPostJson("/api/me/applications/" + clientId + "/transfer", toOrg, *bearer);
        REQUIRE(transferResp != nullptr);
        CHECK(statusIs(transferResp, drogon::k200OK));
    }
    const auto orgIdOpt = sqlInt(
      "SELECT id FROM organizations WHERE slug = '" + slug + "'");
    REQUIRE(orgIdOpt.has_value());
    const int64_t orgId = *orgIdOpt;

    // 1) login with the org hint -> org-bound code.
    auto login = pkceLogin(userA, passA, clientId, kRedirect, "openid profile org", slug);
    REQUIRE(login.resp != nullptr);
    if (login.resp->getStatusCode() != ::drogon::k200OK)
        dumpBody(login.resp, "org login");
    CHECK(statusIs(login.resp, ::drogon::k200OK));
    Json::Value loginJson;
    REQUIRE(parseJsonBody(login.resp, loginJson));
    const std::string code = loginJson.get("code", "").asString();
    CHECK(!code.empty());

    // 2) exchange -> access/refresh/id_token.
    Json::Value tokens = exchangeCode(code, login.verifier, clientId, clientSecret, kRedirect);
    const std::string access = tokens.get("access_token", "").asString();
    const std::string refresh = tokens.get("refresh_token", "").asString();
    const std::string idToken = tokens.get("id_token", "").asString();
    CHECK(!access.empty());
    CHECK(!refresh.empty());
    CHECK(!idToken.empty());

    // 3) introspection exposes the org binding.
    {
        Json::Value intro = introspectWith(access, clientId, clientSecret);
        CHECK(intro.get("active", false).asBool());
        CHECK(intro.isMember("org_id"));
        CHECK(intro.get("org_id", 0).asInt64() == orgId);
    }

    // 4) userinfo carries org_ctx (O7: CURRENT membership + role).
    {
        Json::Value ui = userInfoFor(access);
        CHECK(ui.get("sub", "").asString().size() > 0);
        CHECK(ui.isMember("org_ctx"));
        CHECK(ui["org_ctx"].get("org_id", 0).asInt64() == orgId);
        CHECK(ui["org_ctx"].get("org_name", "").asString().find("QA OrgCtx") !=
              std::string::npos);
        bool roleOk = false;
        for (const auto &r : ui["org_ctx"]["roles"])
            if (r.asString() == "owner")
                roleOk = true;
        CHECK(roleOk);
    }

    // 5) id_token carries org_ctx.
    {
        Json::Value claims = jwtPayload(idToken);
        CHECK(claims.isMember("org_ctx"));
        CHECK(claims["org_ctx"].get("org_id", 0).asInt64() == orgId);
    }

    // 6) refresh preserves the org binding.
    std::string access2;
    {
        Json::Value rt = refreshTokens(refresh, clientId, clientSecret);
        access2 = rt.get("access_token", "").asString();
        CHECK(!access2.empty());
        Json::Value intro = introspectWith(access2, clientId, clientSecret);
        CHECK(intro.get("active", false).asBool());
        CHECK(intro.get("org_id", 0).asInt64() == orgId);
    }

    // 7) O7: remove the membership -> userinfo drops org_ctx immediately
    //    (introspection org_id is the token's own binding and remains).
    REQUIRE(sqlExec(
      "DELETE FROM organization_members WHERE organization_id = " +
      std::to_string(orgId) + " AND user_id = "
      "(SELECT id FROM users WHERE username = '" + userA + "')"));
    {
        Json::Value ui = userInfoFor(access2);
        CHECK(!ui.isMember("org_ctx"));
        Json::Value intro = introspectWith(access2, clientId, clientSecret);
        CHECK(intro.get("org_id", 0).asInt64() == orgId);
    }

    // cleanup (org/app rows stay; timestamp-unique)
    sqlExec("DELETE FROM users WHERE username = '" + userA + "'");
}

// ---------------------------------------------------------------------------
// Uniform rejection (O1 anti-enumeration): a non-member, a personal app,
// an org app matched against a DIFFERENT org, and a bogus org reference
// all produce the SAME 400 body bytes on the login path.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgContext_Gate_UniformRejection)
{
    ORGCTX_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string userA = "qa_orgctx2_" + suffix;
    const std::string userB = "qa_orgctx3_" + suffix;
    const std::string passA = randomPassword();
    const std::string passB = randomPassword();
    REQUIRE(createVerifiedUser(userA, userA + "@qa.example", passA));
    REQUIRE(createVerifiedUser(userB, userB + "@qa.example", passB));
    auto bearerA = consoleBearer(userA, passA);
    REQUIRE(bearerA.has_value());

    // org (owner A) + second org + org app + personal app.
    const std::string slug1 = "qa-orgctx-o1-" + suffix;
    const std::string slug2 = "qa-orgctx-o2-" + suffix;
    for (const auto &s : {slug1, slug2})
    {
        Json::Value orgBody;
        orgBody["slug"] = s;
        orgBody["name"] = "QA OrgCtx " + s;
        auto r = sendPostJson("/api/me/organizations", orgBody, *bearerA);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, drogon::k201Created));
    }
    auto makeApp = [&](const char *name) -> std::string {
        Json::Value app;
        app["name"] = name + suffix;
        app["client_type"] = "CONFIDENTIAL";
        Json::Value uris(Json::arrayValue);
        uris.append(kRedirect);
        app["redirect_uris"] = uris;
        // The org scope must be registered so rejections can only come from
        // the org gate (not the scope-allowlist guard).
        Json::Value scopes(Json::arrayValue);
        scopes.append("openid");
        scopes.append("profile");
        scopes.append("org");
        app["scopes"] = scopes;
        Json::Value grants(Json::arrayValue);
        grants.append("authorization_code");
        grants.append("refresh_token");
        app["allowed_grant_types"] = grants;
        auto r = sendPostJson("/api/me/applications", app, *bearerA);
        if (r == nullptr || r->getStatusCode() != ::drogon::k201Created)
        {
            dumpBody(r, "makeApp");
            return "";
        }
        Json::Value b;
        if (!parseJsonBody(r, b))
            return "";
        return b["client_id"].asString();
    };
    const std::string orgAppId = makeApp("QA OrgApp ");
    const std::string personalAppId = makeApp("QA PersonalApp ");
    CHECK(!orgAppId.empty());
    CHECK(!personalAppId.empty());
    {
        Json::Value toOrg;
        toOrg["org_slug"] = slug1;
        auto r = sendPostJson("/api/me/applications/" + orgAppId + "/transfer", toOrg, *bearerA);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, drogon::k200OK));
    }

    // Leg B: user B (NOT a member) probes the org app + slug1.
    auto legNonMember =
      pkceLogin(userB, passB, orgAppId, kRedirect, "openid profile org", slug1);
    REQUIRE(legNonMember.resp != nullptr);
    CHECK(statusIs(legNonMember.resp, ::drogon::k400BadRequest));

    // Normalized comparison: the error envelope carries a unique
    // request_id per response; every OTHER byte must be identical.
    auto normalized = [](::drogon::HttpResponsePtr resp) -> std::string {
        Json::Value body;
        if (!parseJsonBody(resp, body))
            return "<unparseable>";
        if (body.isObject() && body["error"].isObject())
            body["error"].removeMember("request_id");
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        return Json::writeString(w, body);
    };
    const std::string uniformBody = normalized(legNonMember.resp);

    // Leg C: owner A + personal app + slug1 (client unrelated to the org).
    auto legPersonal =
      pkceLogin(userA, passA, personalAppId, kRedirect, "openid profile org", slug1);
    REQUIRE(legPersonal.resp != nullptr);
    CHECK(statusIs(legPersonal.resp, ::drogon::k400BadRequest));

    // Leg D: owner A + org app(slug1) but org_id = slug2 (different org).
    auto legWrongOrg =
      pkceLogin(userA, passA, orgAppId, kRedirect, "openid profile org", slug2);
    REQUIRE(legWrongOrg.resp != nullptr);
    CHECK(statusIs(legWrongOrg.resp, ::drogon::k400BadRequest));

    // Leg E: bogus references (unknown slug; nonexistent integer id).
    auto legBogusSlug =
      pkceLogin(userA, passA, orgAppId, kRedirect, "openid profile org", "no-such-org-xyz");
    REQUIRE(legBogusSlug.resp != nullptr);
    CHECK(statusIs(legBogusSlug.resp, ::drogon::k400BadRequest));
    auto legBogusId =
      pkceLogin(userA, passA, orgAppId, kRedirect, "openid profile org", "99999999");
    REQUIRE(legBogusId.resp != nullptr);
    CHECK(statusIs(legBogusId.resp, ::drogon::k400BadRequest));

    // Uniformity: identical normalized bodies across every rejection cause.
    for (const auto *leg : {&legPersonal, &legWrongOrg, &legBogusSlug, &legBogusId})
    {
        const std::string body = normalized(leg->resp);
        if (body != uniformBody)
        {
            LOG_ERROR << "[orgctx] non-uniform rejection body: " << body;
        }
        CHECK(body == uniformBody);
    }

    // Cleanup.
    sqlExec("DELETE FROM users WHERE username IN ('" + userA + "', '" + userB + "')");
}

// ---------------------------------------------------------------------------
// Scope gating: org_id WITHOUT the org scope still binds the token
// (introspection org_id) but releases no org_ctx (userinfo, id_token).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P2_OrgContext_WithoutOrgScope_NoClaims)
{
    ORGCTX_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string userA = "qa_orgctx4_" + suffix;
    const std::string passA = randomPassword();
    REQUIRE(createVerifiedUser(userA, userA + "@qa.example", passA));
    auto bearer = consoleBearer(userA, passA);
    REQUIRE(bearer.has_value());

    const std::string slug = "qa-orgctx-o4-" + suffix;
    {
        Json::Value orgBody;
        orgBody["slug"] = slug;
        orgBody["name"] = "QA OrgCtx4 " + suffix;
        auto r = sendPostJson("/api/me/organizations", orgBody, *bearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, drogon::k201Created));
    }
    std::string clientId, clientSecret;
    {
        Json::Value app;
        app["name"] = "QA OrgCtx4 App " + suffix;
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
        auto r = sendPostJson("/api/me/applications", app, *bearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, drogon::k201Created));
        Json::Value b;
        REQUIRE(parseJsonBody(r, b));
        clientId = b["client_id"].asString();
        clientSecret = b["client_secret"].asString();
    }
    {
        Json::Value toOrg;
        toOrg["org_slug"] = slug;
        auto r = sendPostJson("/api/me/applications/" + clientId + "/transfer", toOrg, *bearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, drogon::k200OK));
    }

    // org_id present, org scope NOT requested.
    auto login = pkceLogin(userA, passA, clientId, kRedirect, "openid profile", slug);
    REQUIRE(login.resp != nullptr);
    if (login.resp->getStatusCode() != ::drogon::k200OK)
        dumpBody(login.resp, "no-org-scope login");
    CHECK(statusIs(login.resp, ::drogon::k200OK));
    Json::Value lj;
    REQUIRE(parseJsonBody(login.resp, lj));
    Json::Value tokens =
      exchangeCode(lj.get("code", "").asString(), login.verifier, clientId, clientSecret, kRedirect);
    const std::string access = tokens.get("access_token", "").asString();
    CHECK(!access.empty());

    {
        Json::Value intro = introspectWith(access, clientId, clientSecret);
        CHECK(intro.get("active", false).asBool());
        // The binding is the token's own property (design §2.1 item 4).
        CHECK(intro.isMember("org_id"));
    }
    {
        Json::Value ui = userInfoFor(access);
        CHECK(!ui.isMember("org_ctx"));
    }
    {
        Json::Value claims = jwtPayload(tokens.get("id_token", "").asString());
        CHECK(!claims.isMember("org_ctx"));
    }

    sqlExec("DELETE FROM users WHERE username = '" + userA + "'");
}

// ---------------------------------------------------------------------------
// Review 1.2: the §2.5 require_mfa policy branch. An org flagged
// require_mfa refuses to lend its context to a password-only session
// (amr lacks "mfa"): the login path answers 401 AUTH_MFA_REQUIRED.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgContext_RequireMfaGate_BlocksPwdSession)
{
    ORGCTX_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string userA = "qa_orgctx5_" + suffix;
    const std::string passA = randomPassword();
    REQUIRE(createVerifiedUser(userA, userA + "@qa.example", passA));
    auto bearer = consoleBearer(userA, passA);
    REQUIRE(bearer.has_value());

    const std::string slug = "qa-orgctx-o5-" + suffix;
    {
        Json::Value orgBody;
        orgBody["slug"] = slug;
        orgBody["name"] = "QA OrgCtx5 " + suffix;
        auto r = sendPostJson("/api/me/organizations", orgBody, *bearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, drogon::k201Created));
    }
    std::string clientId;
    {
        Json::Value app;
        app["name"] = "QA OrgCtx5 App " + suffix;
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
        auto r = sendPostJson("/api/me/applications", app, *bearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, drogon::k201Created));
        Json::Value b;
        REQUIRE(parseJsonBody(r, b));
        clientId = b["client_id"].asString();
    }
    {
        Json::Value toOrg;
        toOrg["org_slug"] = slug;
        auto r = sendPostJson("/api/me/applications/" + clientId + "/transfer", toOrg, *bearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, drogon::k200OK));
    }
    // Flag the org MFA-mandatory (V036 column; throwaway org, no restore
    // needed).
    REQUIRE(sqlExec("UPDATE organizations SET require_mfa = true WHERE slug = '" + slug + "'"));

    // Password-only session (amr="pwd" at login) + org hint -> 401
    // AUTH_MFA_REQUIRED (AUTHENTICATION catalog family).
    auto login = pkceLogin(userA, passA, clientId, kRedirect, "openid profile org", slug);
    REQUIRE(login.resp != nullptr);
    CHECK(statusIs(login.resp, ::drogon::k401Unauthorized));
    Json::Value err;
    REQUIRE(parseJsonBody(login.resp, err));
    CHECK(err["error"]["code"].asString() == "AUTH_MFA_REQUIRED");

    // Without the org hint the same login proceeds normally (the policy
    // gates the org CONTEXT, not the login itself).
    auto plain = pkceLogin(userA, passA, clientId, kRedirect, "openid profile org", "");
    REQUIRE(plain.resp != nullptr);
    CHECK(statusIs(plain.resp, ::drogon::k200OK));

    sqlExec("DELETE FROM users WHERE username = '" + userA + "'");
}

// ---------------------------------------------------------------------------
// Review 1.3: the consent round trip -- the only channel carrying the org
// binding from authorize to POST /oauth2/consent (OrgContextSlots mint at
// the consent redirect, one-shot consume + gate re-run at the POST). Drives
// the browser shape with explicit cookie handling (ConsentMultiFlow
// pattern): session login -> authorize (prompt=consent) -> consent approve
// -> org-bound code -> exchange -> introspection/userinfo. A second,
// silent authorize on the same session then issues directly with the org
// binding (covers the AEC silent path).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OrgContext_ConsentRoundTrip_BindingSurvives)
{
    ORGCTX_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string userA = "qa_orgctx6_" + suffix;
    const std::string passA = randomPassword();
    REQUIRE(createVerifiedUser(userA, userA + "@qa.example", passA));
    auto bearer = consoleBearer(userA, passA);
    REQUIRE(bearer.has_value());

    const std::string slug = "qa-orgctx-o6-" + suffix;
    {
        Json::Value orgBody;
        orgBody["slug"] = slug;
        orgBody["name"] = "QA OrgCtx6 " + suffix;
        auto r = sendPostJson("/api/me/organizations", orgBody, *bearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, drogon::k201Created));
    }
    const auto orgIdOpt = sqlInt("SELECT id FROM organizations WHERE slug = '" + slug + "'");
    REQUIRE(orgIdOpt.has_value());
    std::string clientId;
    {
        // PUBLIC app: /oauth2/authorize validates the client with an empty
        // secret, which fails for CONFIDENTIAL clients (pre-existing
        // behavior, registered as a tracking issue) -- the browser
        // authorize+consent flow only works for PUBLIC clients today.
        Json::Value app;
        app["name"] = "QA OrgCtx6 App " + suffix;
        app["client_type"] = "PUBLIC";
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
        auto r = sendPostJson("/api/me/applications", app, *bearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, drogon::k201Created));
        Json::Value b;
        REQUIRE(parseJsonBody(r, b));
        clientId = b["client_id"].asString();
    }
    const std::string clientSecret;  // PUBLIC: PKCE only
    {
        Json::Value toOrg;
        toOrg["org_slug"] = slug;
        auto r = sendPostJson("/api/me/applications/" + clientId + "/transfer", toOrg, *bearer);
        REQUIRE(r != nullptr);
        CHECK(statusIs(r, drogon::k200OK));
    }

    // Establish a browser session (login against the console client only
    // harvests the session cookie; the authorize below targets the org app).
    std::string cookie;
    {
        const std::string verifier = ::fulla::drogon::utils::generateSecureToken(32);
        const std::string challenge =
          ::fulla::drogon::utils::computeCodeChallenge(verifier, "S256");
        auto resp = sendPostForm(
          "/oauth2/login?json=true",
          "username=" + userA + "&password=" + passA +
            "&client_id=fulla-admin-console"
            "&redirect_uri=http%3A%2F%2F127.0.0.1%3A5174%2Fadmin%2Fcallback"
            "&scope=openid&state=cookieharvest1&code_challenge=" + challenge +
            "&code_challenge_method=S256"
        );
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
        for (const auto &entry : resp->getCookies())
        {
            if (!cookie.empty())
                cookie += "; ";
            cookie += entry.first + "=" + entry.second.value();
        }
        CHECK(!cookie.empty());
    }

    // authorize (prompt=consent) with the org hint -> 302 to the consent
    // page carrying consent_csrf (the org binding is stashed server-side).
    // verifier1/challenge1 stay coherent across authorize -> consent POST
    // -> token exchange (the consent form echoes the challenge).
    std::string consentCsrf;
    std::string userIdParam;
    const std::string verifier1 = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string challenge1 =
      ::fulla::drogon::utils::computeCodeChallenge(verifier1, "S256");
    {
        auto client = ::drogon::HttpClient::newHttpClient(
          "http://127.0.0.1:5555", ::drogon::app().getLoop()
        );
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Get);
        req->setPath("/oauth2/authorize");
        req->setParameter("response_type", "code");
        req->setParameter("client_id", clientId);
        req->setParameter("redirect_uri", kRedirect);
        req->setParameter("scope", "openid profile org");
        req->setParameter("state", "orgctxconsent1");
        req->setParameter("prompt", "consent");
        req->setParameter("org_id", slug);
        req->setParameter("code_challenge", challenge1);
        req->setParameter("code_challenge_method", "S256");
        req->addHeader("Cookie", cookie);
        auto [result, resp] = client->sendRequest(req, 30.0);
        REQUIRE(result == ::drogon::ReqResult::Ok);
        REQUIRE(resp != nullptr);
        if (resp->getStatusCode() != ::drogon::k302Found)
        {
            dumpBody(resp, "consent authorize leg");
        }
        CHECK(statusIs(resp, ::drogon::k302Found));
        // Harvest any re-issued cookies from the 302 (the authorize flow
        // writes session state -- ConsentCsrfSlots/org stash mint -- and
        // Drogon re-sends the session cookie; the consent POST must carry
        // the latest value, ConsentMultiFlow's pattern).
        for (const auto &entry : resp->getCookies())
        {
            const std::string name = entry.first + "=";
            if (cookie.find(name) == std::string::npos)
                cookie += "; " + name + entry.second.value();
        }
        const std::string location = resp->getHeader("Location");
        const size_t csrfPos = location.find("consent_csrf=");
        const size_t uidPos = location.find("user_id=");
        REQUIRE(csrfPos != std::string::npos);
        REQUIRE(uidPos != std::string::npos);
        auto part = [&location](size_t start) -> std::string {
            const size_t end = location.find('&', start);
            return location.substr(
              start, (end == std::string::npos ? location.size() : end) - start
            );
        };
        consentCsrf = part(csrfPos + 13);
        userIdParam = ::drogon::utils::urlDecode(part(uidPos + 8));
        CHECK(!consentCsrf.empty());
        CHECK(!userIdParam.empty());
    }

    // POST /oauth2/consent (approve) -> org-bound code: the stash is
    // consumed one-shot and the gate re-runs server-side.
    std::string code1;
    {
        auto resp = postFormCookie(
          "/oauth2/consent",
          "client_id=" + ::drogon::utils::urlEncode(clientId) +
            "&user_id=" + ::drogon::utils::urlEncode(userIdParam) +
            "&scope=" + ::drogon::utils::urlEncode("openid profile org") +
            "&redirect_uri=" + ::drogon::utils::urlEncode(kRedirect) +
            "&state=orgctxconsent1&action=approve" +
            "&code_challenge=" + ::drogon::utils::urlEncode(challenge1) +
            "&code_challenge_method=S256&consent_csrf=" +
            ::drogon::utils::urlEncode(consentCsrf),
          cookie
        );
        REQUIRE(resp != nullptr);
        if (resp->getStatusCode() != ::drogon::k302Found)
            dumpBody(resp, "consent approve");
        CHECK(statusIs(resp, ::drogon::k302Found));
        const std::string location = resp->getHeader("Location");
        const size_t codePos = location.find("?code=");
        REQUIRE(codePos != std::string::npos);
        const size_t codeEnd = location.find('&', codePos);
        code1 = location.substr(
          codePos + 6,
          (codeEnd == std::string::npos ? location.size() : codeEnd) - codePos - 6
        );
        code1 = ::drogon::utils::urlDecode(code1);
        CHECK(!code1.empty());
    }

    // Exchange -> userinfo + id_token org_ctx. (Introspection needs client
    // credentials, which a PUBLIC client has none of; that leg is covered
    // by the login-chain case against a CONFIDENTIAL app.)
    std::string access1;
    Json::Value tokens1;
    {
        tokens1 = exchangeCode(code1, verifier1, clientId, clientSecret, kRedirect);
        access1 = tokens1.get("access_token", "").asString();
        CHECK(!access1.empty());
        Json::Value ui = userInfoFor(access1);
        CHECK(ui.isMember("org_ctx"));
        CHECK(ui["org_ctx"].get("org_id", 0).asInt64() == *orgIdOpt);
        Json::Value claims = jwtPayload(tokens1.get("id_token", "").asString());
        CHECK(claims.isMember("org_ctx"));
        CHECK(claims["org_ctx"].get("org_id", 0).asInt64() == *orgIdOpt);
    }

    // Silent leg: a second authorize on the same session (prior consent now
    // recorded, no prompt) issues directly -- the AEC silent path with the
    // org binding.
    {
        const std::string verifier2 = ::fulla::drogon::utils::generateSecureToken(32);
        const std::string challenge2 =
          ::fulla::drogon::utils::computeCodeChallenge(verifier2, "S256");
        auto client = ::drogon::HttpClient::newHttpClient(
          "http://127.0.0.1:5555", ::drogon::app().getLoop()
        );
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Get);
        req->setPath("/oauth2/authorize");
        req->setParameter("response_type", "code");
        req->setParameter("client_id", clientId);
        req->setParameter("redirect_uri", kRedirect);
        req->setParameter("scope", "openid profile org");
        req->setParameter("state", "orgctxsilent01");
        req->setParameter("org_id", slug);
        req->setParameter("code_challenge", challenge2);
        req->setParameter("code_challenge_method", "S256");
        req->addHeader("Cookie", cookie);
        auto [result, resp] = client->sendRequest(req, 30.0);
        REQUIRE(result == ::drogon::ReqResult::Ok);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k302Found));
        const std::string location = resp->getHeader("Location");
        const size_t codePos = location.find("?code=");
        REQUIRE(codePos != std::string::npos);
        const size_t codeEnd = location.find('&', codePos);
        std::string code2 = location.substr(
          codePos + 6,
          (codeEnd == std::string::npos ? location.size() : codeEnd) - codePos - 6
        );
        code2 = ::drogon::utils::urlDecode(code2);
        CHECK(!code2.empty());

        Json::Value tokens =
          exchangeCode(code2, verifier2, clientId, clientSecret, kRedirect);
        const std::string access = tokens.get("access_token", "").asString();
        CHECK(!access.empty());
        Json::Value ui = userInfoFor(access);
        CHECK(ui.isMember("org_ctx"));
        CHECK(ui["org_ctx"].get("org_id", 0).asInt64() == *orgIdOpt);
    }

    sqlExec("DELETE FROM users WHERE username = '" + userA + "'");
}
