// tests/integration/auth/ConfidentialAuthorizeFlowTest.cc
//
// #233 regression suite: /oauth2/authorize used to validate the client with
// an empty secret (validateClient(clientId, "")), which rejected every
// CONFIDENTIAL client with 400 "Invalid client_id" before any redirect_uri
// or session logic ran. RFC 6749 3.1 keeps client authentication at the
// authorization endpoint optional (and the front channel cannot present a
// confidential client's secret anyway), so the endpoint now gates on client
// existence + governance only (validateClientForAuthorize -> getClient;
// V035 soft-deleted and suspended clients resolve to nullopt there).
//
// Case 1 drives a self-registered CONFIDENTIAL app through the complete
// browser flow: authorize (PKCE S256, prompt=consent) -> consent approve ->
// code -> client_secret_basic exchange (F-017: a body secret is rejected;
// the form client_id fallback is PUBLIC-only) -> introspection. Note the
// explicit allowed_grant_types: self-registered apps default to
// client_credentials only, which the token endpoint's grant gate would
// reject -- a pitfall this suite pins down.
//
// Case 2 proves governance still bites at the authorize gate: admin
// suspension and the owner's soft delete each restore the 400 "Invalid
// client_id" rejection.
//
// Storage: Postgres-only (self-registration + governance rows); cases skip
// cleanly under memory.

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

#define CONF_AUTHZ_SKIP_GUARD                                  \
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
    return std::string("qa-233-Zx5-") + uniqueSuffix();
}

void dumpBody(const ::drogon::HttpResponsePtr &resp, const char *where)
{
    Json::Value body;
    const bool isJson = parseJsonBody(resp, body);
    LOG_ERROR << "[conf-authz:" << where << "] status="
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

struct ConfidentialApp
{
    std::string clientId;
    std::string clientSecret;
};

// Register a CONFIDENTIAL app with the full authorization_code grant set
// (the self-registration default is client_credentials only).
std::optional<ConfidentialApp> createConfidentialApp(const std::string &bearer,
                                                     const std::string &name)
{
    Json::Value app;
    app["name"] = name;
    app["client_type"] = "CONFIDENTIAL";
    Json::Value uris(Json::arrayValue);
    uris.append("https://qa-233.example/callback");
    app["redirect_uris"] = uris;
    Json::Value scopes(Json::arrayValue);
    scopes.append("openid");
    scopes.append("profile");
    app["scopes"] = scopes;
    Json::Value grants(Json::arrayValue);
    grants.append("authorization_code");
    grants.append("refresh_token");
    app["allowed_grant_types"] = grants;
    auto resp = sendPostJson("/api/me/applications", app, bearer);
    if (resp == nullptr || resp->getStatusCode() != ::drogon::k201Created)
    {
        dumpBody(resp, "create app");
        return std::nullopt;
    }
    Json::Value body;
    if (!parseJsonBody(resp, body))
        return std::nullopt;
    ConfidentialApp out;
    out.clientId = body["client_id"].asString();
    out.clientSecret = body["client_secret"].asString();
    if (out.clientId.empty() || out.clientSecret.empty())
        return std::nullopt;
    return out;
}

// POST a form with client authentication: Basic when a secret is supplied
// (F-017: self-registered CONFIDENTIAL apps declare
// token_endpoint_auth_method=client_secret_basic), form client_id for
// clients without secrets.
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

// POST a form with an explicit session cookie (the consent round trip needs
// the browser session).
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

// GET /oauth2/authorize carrying an explicit session cookie; the response
// 302-redirects and any re-issued cookies are merged back into `cookie`
// (the authorize flow writes session state and re-sends the session cookie;
// the consent POST must carry the latest value).
::drogon::HttpResponsePtr authorizeWithCookie(const std::string &clientId,
                                              const std::string &redirectUri,
                                              const std::string &scope,
                                              const std::string &state,
                                              const std::string &prompt,
                                              const std::string &codeChallenge,
                                              std::string &cookie)
{
    auto client =
      ::drogon::HttpClient::newHttpClient("http://127.0.0.1:5555", ::drogon::app().getLoop());
    auto req = ::drogon::HttpRequest::newHttpRequest();
    req->setMethod(::drogon::Get);
    req->setPath("/oauth2/authorize");
    req->setParameter("response_type", "code");
    req->setParameter("client_id", clientId);
    req->setParameter("redirect_uri", redirectUri);
    req->setParameter("scope", scope);
    req->setParameter("state", state);
    if (!prompt.empty())
        req->setParameter("prompt", prompt);
    req->setParameter("code_challenge", codeChallenge);
    req->setParameter("code_challenge_method", "S256");
    req->addHeader("Cookie", cookie);
    auto [result, resp] = client->sendRequest(req, 30.0);
    if (result != ::drogon::ReqResult::Ok || resp == nullptr)
        return nullptr;
    for (const auto &entry : resp->getCookies())
    {
        const std::string name = entry.first + "=";
        if (cookie.find(name) == std::string::npos)
            cookie += "; " + name + entry.second.value();
    }
    return resp;
}

constexpr const char *kRedirect = "https://qa-233.example/callback";

}  // namespace

// ---------------------------------------------------------------------------
// #233 acceptance: a self-registered CONFIDENTIAL app completes the whole
// authorization-code flow. Before the fix the authorize leg below died with
// 400 "Invalid client_id" (empty-secret validateClient rejected every
// CONFIDENTIAL client).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_Authorize_ConfidentialClient_FullChain)
{
    CONF_AUTHZ_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string user = "qa_233_chain_" + suffix;
    const std::string pass = randomPassword();
    REQUIRE(createVerifiedUser(user, user + "@qa.example", pass));
    auto consoleTokens = fulla::test::http::loginAsUserTokens(user, pass, "openid profile");
    REQUIRE(consoleTokens.has_value());
    const std::string bearer = consoleTokens->get("access_token", "").asString();
    CHECK(!bearer.empty());
    auto app = createConfidentialApp(bearer, "QA 233 Chain App " + suffix);
    REQUIRE(app.has_value());

    // Establish a browser session (login against the console client only
    // harvests the session cookie; the authorize below targets the app).
    std::string cookie;
    {
        const std::string verifier = ::fulla::drogon::utils::generateSecureToken(32);
        const std::string challenge =
          ::fulla::drogon::utils::computeCodeChallenge(verifier, "S256");
        auto resp = sendPostForm(
          "/oauth2/login?json=true",
          "username=" + user + "&password=" + pass +
            "&client_id=fulla-admin-console"
            "&redirect_uri=http%3A%2F%2F127.0.0.1%3A5174%2Fadmin%2Fcallback"
            "&scope=openid&state=cookieharvest233&code_challenge=" + challenge +
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

    // THE #233 leg: authorize with prompt=consent against the CONFIDENTIAL
    // app -> 302 to the consent page (not 400 "Invalid client_id").
    std::string consentCsrf;
    std::string userIdParam;
    const std::string verifier1 = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string challenge1 =
      ::fulla::drogon::utils::computeCodeChallenge(verifier1, "S256");
    {
        auto resp = authorizeWithCookie(
          app->clientId, kRedirect, "openid profile", "conf233state01", "consent",
          challenge1, cookie
        );
        REQUIRE(resp != nullptr);
        if (resp->getStatusCode() != ::drogon::k302Found)
        {
            dumpBody(resp, "authorize leg");
        }
        CHECK(statusIs(resp, ::drogon::k302Found));
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

    // Consent approve -> code.
    std::string code;
    {
        auto resp = postFormCookie(
          "/oauth2/consent",
          "client_id=" + ::drogon::utils::urlEncode(app->clientId) +
            "&user_id=" + ::drogon::utils::urlEncode(userIdParam) +
            "&scope=" + ::drogon::utils::urlEncode("openid profile") +
            "&redirect_uri=" + ::drogon::utils::urlEncode(kRedirect) +
            "&state=conf233state01&action=approve" +
            "&code_challenge=" + ::drogon::utils::urlEncode(challenge1) +
            "&code_challenge_method=S256&consent_csrf=" +
            ::drogon::utils::urlEncode(consentCsrf),
          cookie
        );
        REQUIRE(resp != nullptr);
        if (resp->getStatusCode() != ::drogon::k302Found)
        {
            dumpBody(resp, "consent approve");
        }
        CHECK(statusIs(resp, ::drogon::k302Found));
        const std::string location = resp->getHeader("Location");
        const size_t codePos = location.find("?code=");
        REQUIRE(codePos != std::string::npos);
        const size_t codeEnd = location.find('&', codePos);
        code = location.substr(
          codePos + 6,
          (codeEnd == std::string::npos ? location.size() : codeEnd) - codePos - 6
        );
        code = ::drogon::utils::urlDecode(code);
        CHECK(!code.empty());
    }

    const std::string exchangeForm =
      "grant_type=authorization_code&code=" + ::drogon::utils::urlEncode(code) +
      "&redirect_uri=" + ::drogon::utils::urlEncode(kRedirect) +
      "&code_verifier=" + ::drogon::utils::urlEncode(verifier1);

    // F-017: the app declares client_secret_basic, so a secret in the form
    // body (with form client_id, the PUBLIC fallback shape) is rejected.
    {
        auto resp = sendPostForm(
          "/oauth2/token",
          exchangeForm + "&client_id=" + ::drogon::utils::urlEncode(app->clientId) +
            "&client_secret=" + ::drogon::utils::urlEncode(app->clientSecret)
        );
        REQUIRE(resp != nullptr);
        if (resp->getStatusCode() != ::drogon::k401Unauthorized)
        {
            dumpBody(resp, "F-017 body-secret leg");
        }
        CHECK(statusIs(resp, ::drogon::k401Unauthorized));
    }

    // client_secret_basic exchange -> token pair.
    std::string accessToken;
    {
        auto resp =
          sendPostFormClientAuth("/oauth2/token", exchangeForm, app->clientId, app->clientSecret);
        REQUIRE(resp != nullptr);
        if (resp->getStatusCode() != ::drogon::k200OK)
        {
            dumpBody(resp, "exchange");
        }
        CHECK(statusIs(resp, ::drogon::k200OK));
        Json::Value tokens;
        REQUIRE(parseJsonBody(resp, tokens));
        accessToken = tokens.get("access_token", "").asString();
        CHECK(!accessToken.empty());
        CHECK(!tokens.get("refresh_token", "").asString().empty());
        CHECK(!tokens.get("id_token", "").asString().empty());
    }

    // Introspection with the same Basic credentials.
    {
        auto resp = sendPostFormClientAuth(
          "/oauth2/introspect",
          "token=" + ::drogon::utils::urlEncode(accessToken),
          app->clientId,
          app->clientSecret
        );
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
        Json::Value intro;
        REQUIRE(parseJsonBody(resp, intro));
        CHECK(intro.get("active", false).asBool());
        CHECK(intro.get("client_id", "").asString() == app->clientId);
    }

    sqlExec("DELETE FROM users WHERE username = '" + user + "'");
}

// ---------------------------------------------------------------------------
// #233 governance regression: existence-only must not mean anything-goes.
// Admin suspension (oauth2_client_owners.status) and the owner's soft delete
// (deleted_at, V035) each make getClient resolve nullopt, which restores the
// 400 "Invalid client_id" rejection at the authorize gate.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_Authorize_ConfidentialClient_GovernanceStillRejects)
{
    CONF_AUTHZ_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string user = "qa_233_gov_" + suffix;
    const std::string pass = randomPassword();
    REQUIRE(createVerifiedUser(user, user + "@qa.example", pass));
    auto consoleTokens = fulla::test::http::loginAsUserTokens(user, pass, "openid profile");
    REQUIRE(consoleTokens.has_value());
    const std::string bearer = consoleTokens->get("access_token", "").asString();
    CHECK(!bearer.empty());
    auto app = createConfidentialApp(bearer, "QA 233 Gov App " + suffix);
    REQUIRE(app.has_value());

    // The gate verdict on a session-less authorize with a well-formed PKCE
    // S256 challenge: pass = redirect to the login page (302, or a 200
    // login page); reject = 400 "Invalid client_id". Query values ride the
    // string RAW (the suite's established sendGet pattern: Drogon does not
    // percent-decode query values here, and the values contain no & or =
    // separators).
    auto authorizeGatePasses = [&]() -> bool {
        const std::string challenge = ::fulla::drogon::utils::computeCodeChallenge(
          ::fulla::drogon::utils::generateSecureToken(32), "S256");
        auto resp = sendGet(
          "/oauth2/authorize?response_type=code&client_id=" + app->clientId +
          "&redirect_uri=" + kRedirect +
          "&scope=openid&state=gov233state01"
          "&code_challenge=" + challenge + "&code_challenge_method=S256");
        if (resp == nullptr)
            return false;
        const std::string body(resp->getBody());
        return resp->getStatusCode() != ::drogon::k400BadRequest &&
               body.find("Invalid client_id") == std::string::npos;
    };

    // Baseline: the active CONFIDENTIAL client passes the gate (#233 fixed).
    CHECK(authorizeGatePasses());

    // Admin suspend -> the gate rejects again.
    auto adminToken = fulla::test::http::loginAsAdmin();
    REQUIRE(adminToken.has_value());
    {
        auto suspendResp = sendPostJson(
          "/api/admin/clients/" + app->clientId + "/suspend",
          Json::Value(Json::objectValue),
          *adminToken
        );
        REQUIRE(suspendResp != nullptr);
        CHECK(statusIs(suspendResp, ::drogon::k200OK));
    }
    CHECK(!authorizeGatePasses());

    // Resume -> the gate passes again (suspension alone was the cause).
    {
        auto resumeResp = sendPostJson(
          "/api/admin/clients/" + app->clientId + "/resume",
          Json::Value(Json::objectValue),
          *adminToken
        );
        REQUIRE(resumeResp != nullptr);
        CHECK(statusIs(resumeResp, ::drogon::k200OK));
    }
    CHECK(authorizeGatePasses());

    // Owner soft-deletes the app (V035: deleted_at set, row invisible to
    // every flow) -> the gate rejects for good.
    {
        auto delResp = sendDelete("/api/me/applications/" + app->clientId, bearer);
        REQUIRE(delResp != nullptr);
        CHECK(statusIs(delResp, ::drogon::k200OK));
    }
    CHECK(!authorizeGatePasses());

    sqlExec("DELETE FROM users WHERE username = '" + user + "'");
}
