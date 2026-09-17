// tests/common/HttpTestClient.h
//
// Shared HTTP integration-test plumbing for the fulla backend suite.
//
// Scope: in-process HTTP integration tests that hit the Drogon app the test
// binary itself starts (tests/test_main.cc boots `app().run()` on a background
// thread, listening on port 5555). There is NO second process and NO port
// readiness race -- the readiness handshake lives in test_main.cc (promise +
// queueInLoop + a fixed prewarm sleep). These helpers only build requests,
// send them through the framework-owned HttpClient, and parse responses.
//
// Convention follows tests/contract/ContractFixtures.h: header-only `inline`
// free functions in a namespace (fulla::test::http). Unlike the contract
// helpers, the request builders here do NOT take a TEST_CTX parameter because
// they perform no assertions -- they just return an HttpResponsePtr (or nullptr
// on transport failure) and the caller runs CHECK/REQUIRE against the result.
// This keeps the call sites readable while leaving every assertion visible in
// the DROGON_TEST body, which is where failures should be reported.
//
// Thread-safety note (verified): test_main.cc calls `test::run(argc, argv)` on
// the MAIN thread (tests/test_main.cc:437), while the Drogon event loop runs on
// a separate background thread. The synchronous HttpClient::sendRequest(req,
// timeout) overload asserts `!isInLoopThread()` and would deadlock if invoked
// from the loop thread -- but we are on the main thread, so it is safe. The
// existing tests/integration/auth/MfaCrossClientAuthFix_IntegrationTest.cc
// relies on exactly this and works.
//
// storage_type skip: every helper that needs Postgres (login, authed admin
// calls) returns nullopt/nullptr under memory mode (Windows/macOS CI legs) so
// callers can `REQUIRE(x.has_value())` and no-op cleanly instead of crashing.
// Memory-mode CI thus stays green with zero admin-route coverage -- admin
// coverage is a Linux-leg concern (the Windows/macOS runners cannot run
// Postgres/Redis via Docker; see docs/history/design/http-integration-test-
// coverage-plan.md B2).

#pragma once

#include <drogon/drogon.h>
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/utils/CryptoUtils.h>

#include <json/json.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace fulla::test::http
{

// Namespace-visibility trap (see tests/contract/ContractFixtures.h:92-95):
// inside `fulla::test::http`, an unqualified `drogon::` resolves to
// `fulla::drogon` (the SDK adapter namespace) before the global
// `::drogon` framework namespace, so EVERY drogon reference below is
// explicitly qualified `::drogon::` to reach the framework.

// Single source of truth for the in-process test server's base URL. Retires
// the inconsistent "http://localhost:5555" / "http://127.0.0.1:5555" /
// "http://127.0.0.1:8080" literals previously copy-pasted across the
// integration test files (the 8080 variant was stale -- config.json listens
// on 5555 -- and silently caused several tests to skip via serverReachable()).
constexpr const char *kTestBaseUrl = "http://127.0.0.1:5555";

// Admin Console OAuth2 client (seeded by apps/server/seed/dev_admin_console_
// client.sql) and its registered redirect URI. The 2-step admin login recipe
// (login -> token -> bearer) uses these for both legs.
constexpr const char *kAdminClientId = "fulla-admin-console";
// F-014: the seed registers the loopback IP literal (RFC 8252 §7.3), not
// "localhost" -- the authorize/login validators now reject the hostname form.
constexpr const char *kAdminRedirectUri = "http://127.0.0.1:5174/admin/callback";

// ---------------------------------------------------------------------------
// Server reachability probe
//
// Mirrors MfaCrossClientAuthFix_IntegrationTest.cc's serverReachable(): polls
// the listener for ~5s so a slow app startup (the prewarm sleep in
// test_main.cc is fixed at 500ms; under load this can be optimistic) does not
// translate into a spurious test failure. Returns true as soon as ANY HTTP
// response (including a 404) is received -- we only care that the listener
// is bound and accepting connections, not that a particular route resolves.
// ---------------------------------------------------------------------------
inline bool serverReachable()
{
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        try
        {
            auto client = ::drogon::HttpClient::newHttpClient(
              kTestBaseUrl, ::drogon::app().getLoop());
            auto req = ::drogon::HttpRequest::newHttpRequest();
            req->setMethod(::drogon::Get);
            req->setPath("/health/live");  // always-200 liveness probe
            auto [result, resp] = client->sendRequest(req, 5.0);
            if (result == ::drogon::ReqResult::Ok && resp != nullptr)
                return true;
        }
        catch (...)
        {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

// ---------------------------------------------------------------------------
// Storage-type guard
//
// Returns true when the running app is backed by a real Postgres (or Redis)
// store, i.e. when admin / DB-coupled routes can actually be exercised.
// Memory mode (config.ci.json, "storage_type":"memory") has no admin user that
// can log in AND the admin services call getDbClient() directly -- so admin
// routes cannot be tested there. Callers use this as an early-return gate:
//   if (!postgresAvailable()) { CHECK(true); return; }
// ---------------------------------------------------------------------------
inline bool postgresAvailable()
{
    auto plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
    return plugin && plugin->getStorageType() != "memory";
}

// ---------------------------------------------------------------------------
// Request builders (synchronous sendRequest, returns HttpResponsePtr or nullptr)
//
// Each builder constructs an HttpClient pinned to the framework loop (so the
// request/response I/O shares the same in-process loop the server runs on),
// builds an HttpRequest, and calls the synchronous sendRequest(req, timeout)
// overload. timeout is in seconds (double). nullptr is returned only on
// transport failure (ReqResult != Ok) or exception -- the caller decides how
// to assert (typically REQUIRE(resp != nullptr)).
// ---------------------------------------------------------------------------

inline ::drogon::HttpResponsePtr sendGet(
  const std::string &path,
  const std::string &bearerToken = "",
  double timeout = 30.0)
{
    try
    {
        auto client =
          ::drogon::HttpClient::newHttpClient(kTestBaseUrl, ::drogon::app().getLoop());
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Get);
        req->setPath(path);
        if (!bearerToken.empty())
            req->addHeader("Authorization", "Bearer " + bearerToken);
        auto [result, resp] = client->sendRequest(req, timeout);
        if (result != ::drogon::ReqResult::Ok || resp == nullptr)
            return nullptr;
        return resp;
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "sendGet(" << path << ") failed: " << e.what();
        return nullptr;
    }
}

inline ::drogon::HttpResponsePtr sendDelete(
  const std::string &path,
  const std::string &bearerToken = "",
  double timeout = 30.0)
{
    try
    {
        auto client =
          ::drogon::HttpClient::newHttpClient(kTestBaseUrl, ::drogon::app().getLoop());
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Delete);
        req->setPath(path);
        if (!bearerToken.empty())
            req->addHeader("Authorization", "Bearer " + bearerToken);
        auto [result, resp] = client->sendRequest(req, timeout);
        if (result != ::drogon::ReqResult::Ok || resp == nullptr)
            return nullptr;
        return resp;
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "sendDelete(" << path << ") failed: " << e.what();
        return nullptr;
    }
}

// Form-encoded POST. Use this for OAuth2 endpoints (/oauth2/login,
// /oauth2/token) which read req->getParameters() (form fields). Note
// SessionController.cc:394 branches on Content-Type: a JSON body never
// reaches the getParameter("json") check at :612, so the json=true flag
// MUST ride on the query string or the form body -- never a JSON field.
// We put it on the path's query string for clarity.
inline ::drogon::HttpResponsePtr sendPostForm(
  const std::string &pathAndQuery,
  const std::string &formBody,
  const std::string &bearerToken = "",
  double timeout = 30.0)
{
    try
    {
        auto client =
          ::drogon::HttpClient::newHttpClient(kTestBaseUrl, ::drogon::app().getLoop());
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Post);
        req->setPath(pathAndQuery);
        req->setContentTypeCode(::drogon::CT_APPLICATION_X_FORM);
        req->setBody(formBody);
        if (!bearerToken.empty())
            req->addHeader("Authorization", "Bearer " + bearerToken);
        auto [result, resp] = client->sendRequest(req, timeout);
        if (result != ::drogon::ReqResult::Ok || resp == nullptr)
            return nullptr;
        return resp;
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "sendPostForm(" << pathAndQuery << ") failed: " << e.what();
        return nullptr;
    }
}

inline ::drogon::HttpResponsePtr sendPostJson(
  const std::string &path,
  const Json::Value &json,
  const std::string &bearerToken = "",
  double timeout = 30.0)
{
    try
    {
        auto client =
          ::drogon::HttpClient::newHttpClient(kTestBaseUrl, ::drogon::app().getLoop());
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Post);
        req->setPath(path);
        req->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
        Json::StreamWriterBuilder wb;
        req->setBody(Json::writeString(wb, json));
        if (!bearerToken.empty())
            req->addHeader("Authorization", "Bearer " + bearerToken);
        auto [result, resp] = client->sendRequest(req, timeout);
        if (result != ::drogon::ReqResult::Ok || resp == nullptr)
            return nullptr;
        return resp;
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "sendPostJson(" << path << ") failed: " << e.what();
        return nullptr;
    }
}

inline ::drogon::HttpResponsePtr sendPutJson(
  const std::string &path,
  const Json::Value &json,
  const std::string &bearerToken = "",
  double timeout = 30.0)
{
    try
    {
        auto client =
          ::drogon::HttpClient::newHttpClient(kTestBaseUrl, ::drogon::app().getLoop());
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Put);
        req->setPath(path);
        req->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
        Json::StreamWriterBuilder wb;
        req->setBody(Json::writeString(wb, json));
        if (!bearerToken.empty())
            req->addHeader("Authorization", "Bearer " + bearerToken);
        auto [result, resp] = client->sendRequest(req, timeout);
        if (result != ::drogon::ReqResult::Ok || resp == nullptr)
            return nullptr;
        return resp;
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "sendPutJson(" << path << ") failed: " << e.what();
        return nullptr;
    }
}

// v1.4.0: PATCH counterpart of sendPutJson (same shape/timeout semantics).
inline ::drogon::HttpResponsePtr sendPatchJson(
  const std::string &path,
  const Json::Value &json,
  const std::string &bearerToken = "",
  double timeout = 30.0)
{
    try
    {
        auto client =
          ::drogon::HttpClient::newHttpClient(kTestBaseUrl, ::drogon::app().getLoop());
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Patch);
        req->setPath(path);
        req->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
        Json::StreamWriterBuilder wb;
        req->setBody(Json::writeString(wb, json));
        if (!bearerToken.empty())
            req->addHeader("Authorization", "Bearer " + bearerToken);
        auto [result, resp] = client->sendRequest(req, timeout);
        if (result != ::drogon::ReqResult::Ok || resp == nullptr)
            return nullptr;
        return resp;
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "sendPatchJson(" << path << ") failed: " << e.what();
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Response body parsing
// ---------------------------------------------------------------------------

// Parse resp's body as JSON into `out`. Returns false on null response or
// parse error. Replaces the copy-pasted `parseBody` helpers across the suite.
inline bool parseJsonBody(const ::drogon::HttpResponsePtr &resp, Json::Value &out)
{
    if (!resp)
        return false;
    const std::string body(resp->getBody());
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;
    return reader->parse(body.data(), body.data() + body.size(), &out, &errs);
}

// Convenience: status code check. Returns true if resp is non-null and has the
// expected status. Centralizes the most common assertion so call sites read as
// `CHECK(statusIs(resp, k200OK))`.
inline bool statusIs(const ::drogon::HttpResponsePtr &resp, ::drogon::HttpStatusCode code)
{
    return resp && resp->getStatusCode() == code;
}

// ---------------------------------------------------------------------------
// Admin login recipe
//
// Performs the 2-step OAuth2 authorization-code grant against the in-process
// server to obtain a user-facing access token carrying the `admin` role:
//   1. POST /oauth2/login?json=true (form: username/password/client_id/
//      redirect_uri/scope) -> {"code": "...", "location": "..."}
//   2. POST /oauth2/token (form: grant_type=authorization_code&code=...&
//      redirect_uri=...&client_id=...) -> {"access_token": "...", ...}
//
// The seeded admin user (apps/server/seed/dev_admin_user.sql, username=admin
// password=admin) must exist -- the in-process seeder in StorageSeed.h applies
// that seed. Under memory mode there is no admin user (MemoryIdentityRepository
// always returns nullopt from findByUsername) and the admin services call
// getDbClient() directly anyway, so this returns nullopt and callers skip.
//
// `json=true` is sent on the QUERY STRING, not the form body: the login
// handler (SessionController.cc:612) reads it via req->getParameter("json"),
// which sees form fields AND query params, but the JSON-vs-302 branch is only
// reachable when Content-Type is NOT application/json (see the comment on
// sendPostForm). Keeping it on the query string makes the intent unambiguous.
//
// Returns the access token, or nullopt on any failure (memory mode, wrong
// credentials, server unreachable, malformed response).
// ---------------------------------------------------------------------------
// Generalized login (see definition below): password login as an arbitrary
// user; returns the full token JSON (access_token, refresh_token, ...).
inline std::optional<Json::Value> loginAsUserTokens(
  const std::string &username,
  const std::string &password,
  const std::string &scope
);

inline std::optional<std::string> loginAsAdminWithScope(const std::string &scope)
{
    auto tokens = loginAsUserTokens("admin", "admin", scope);
    if (!tokens)
        return std::nullopt;
    return tokens->get("access_token", "").asString();
}

// Generalized login (issues-53-60 hardening tests): password login as an
// arbitrary seeded/created user via the same authorization-code + PKCE
// recipe as the admin login. Returns the FULL token JSON (access_token,
// refresh_token, ...) so revocation tests can keep the refresh token.
inline std::optional<Json::Value> loginAsUserTokens(
  const std::string &username,
  const std::string &password,
  const std::string &scope
)
{
    if (!postgresAvailable())
        return std::nullopt;
    if (!serverReachable())
        return std::nullopt;

    // F-011: PKCE is mandatory (auth.require_pkce_for_public defaults true),
    // so this recipe always runs the S256 flow. generateSecureToken(32)
    // yields a 43-char base64url string -- a valid RFC 7636 §4.1 verifier
    // charset-wise -- and CryptoUtils::computeCodeChallenge matches the
    // server's spec-correct BASE64URL(SHA256(verifier)) verification.
    const std::string codeVerifier = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string codeChallenge =
      ::fulla::drogon::utils::computeCodeChallenge(codeVerifier, "S256");

    // Step 1: login -> authorization code.
    const std::string loginForm =
      "username=" + username + "&password=" + password +
      "&client_id=" +
      std::string(kAdminClientId) +
      "&redirect_uri=" + std::string(kAdminRedirectUri) +
      "&scope=" + scope + "&state=t1"
      "&code_challenge=" + codeChallenge +
      "&code_challenge_method=S256";
    auto loginResp = sendPostForm("/oauth2/login?json=true", loginForm);
    if (!loginResp || loginResp->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    Json::Value loginJson;
    if (!parseJsonBody(loginResp, loginJson))
        return std::nullopt;
    // MFA-enabled users return mfa_required=true instead of a code.
    if (loginJson.isMember("mfa_required") && loginJson["mfa_required"].asBool())
        return std::nullopt;
    const std::string code = loginJson.get("code", "").asString();
    if (code.empty())
        return std::nullopt;

    // Step 2: token exchange.
    const std::string tokenForm =
      "grant_type=authorization_code"
      "&code=" +
      code +
      "&redirect_uri=" + std::string(kAdminRedirectUri) +
      "&client_id=" + std::string(kAdminClientId) +
      "&client_secret="  // admin-console is a PUBLIC client -> empty secret
      "&code_verifier=" + codeVerifier;
    auto tokenResp = sendPostForm("/oauth2/token", tokenForm);
    if (!tokenResp || tokenResp->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    Json::Value tokenJson;
    if (!parseJsonBody(tokenResp, tokenJson))
        return std::nullopt;
    if (tokenJson.get("access_token", "").asString().empty())
        return std::nullopt;
    return tokenJson;
}

// Default admin login: requests the full `openid profile admin` scope set
// (the scopes the admin-console seed grants). Most tests want this. Tests that
// need a NARROWER token for F-010 insufficient_scope coverage call
// loginAsAdminWithScope() directly with a subset (e.g. "openid" only).
inline std::optional<std::string> loginAsAdmin()
{
    return loginAsAdminWithScope("openid profile admin");
}

// ---------------------------------------------------------------------------
// Authed convenience wrappers
//
// Thin shims that obtain an admin token once and attach it as a Bearer header.
// Prefer calling loginAsAdmin() once per test and passing the token to
// sendGet/sendPostJson(..., token) -- this avoids re-running the 2-step flow
// for every request. These are for one-shot checks.
// ---------------------------------------------------------------------------
inline ::drogon::HttpResponsePtr authedGet(const std::string &path)
{
    auto token = loginAsAdmin();
    if (!token)
        return nullptr;
    return sendGet(path, *token);
}

inline ::drogon::HttpResponsePtr authedPostJson(
  const std::string &path,
  const Json::Value &json)
{
    auto token = loginAsAdmin();
    if (!token)
        return nullptr;
    return sendPostJson(path, json, *token);
}

inline ::drogon::HttpResponsePtr authedPutJson(
  const std::string &path,
  const Json::Value &json)
{
    auto token = loginAsAdmin();
    if (!token)
        return nullptr;
    return sendPutJson(path, json, *token);
}

inline ::drogon::HttpResponsePtr authedDelete(const std::string &path)
{
    auto token = loginAsAdmin();
    if (!token)
        return nullptr;
    return sendDelete(path, *token);
}

}  // namespace fulla::test::http
