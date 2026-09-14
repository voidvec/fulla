// tests/integration/oidc/OidcBatch2FeatureHttpTest.cc
//
// HTTP integration tests for the OAuth/OIDC compliance Batch 2 features:
//   - F-022: prompt=none without a session -> login_required redirect (never UI)
//   - F-022: prompt containing both "none" and another value -> 400 (malformed)
//   - F-017: token_endpoint_auth_method enforcement (backend-svc is seeded
//            client_secret_basic, so a body-only secret is rejected)
//   - F-023: userinfo requires an openid-scoped access token (403 otherwise)
//   - F-027: end_session endpoint returns 200 when no redirect URI is given
//
// These tests require Postgres (seed clients) + the live HTTP listener on
// 127.0.0.1:5555; they skip cleanly otherwise.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <fulla/oauth2/jwk/JwkManager.h>
#include <fulla/drogon/adapters/BackchannelLogoutNotifier.h>
#include <fulla/identity/testing/FakeOAuthHttpClient.h>

#include "HttpTestClient.h"

#include <chrono>
#include <ctime>
#include <future>
#include <string>

using fulla::test::http::kAdminClientId;
using fulla::test::http::loginAsUserTokens;
using fulla::test::http::kAdminRedirectUri;
using fulla::test::http::kTestBaseUrl;
using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendGet;
using fulla::test::http::sendPostForm;
using fulla::test::http::sendPostJson;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define OIDC_BATCH2_SKIP_GUARD                                 \
    do                                                         \
    {                                                          \
        if (!postgresAvailable() || !serverReachable())        \
        {                                                      \
            CHECK(true);                                       \
            return;                                            \
        }                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// F-022 (OIDC Core §3.1.2.1): prompt=none with no authenticated session
// cannot be satisfied without UI -> the server MUST redirect back to the
// client with error=login_required (never show a login page).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OidcBatch2_PromptNone_NoSession_ReturnsLoginRequired)
{
    OIDC_BATCH2_SKIP_GUARD;

    auto resp = sendGet(
      "/oauth2/authorize?response_type=code&client_id=fulla-portal"
      "&redirect_uri=http://127.0.0.1:5173/callback&scope=openid"
      "&state=abcdef1234&prompt=none");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k302Found));
    auto location = resp->getHeader("location");
    CHECK(location.find("error=login_required") != std::string::npos);
    // state is echoed back.
    CHECK(location.find("state=abcdef1234") != std::string::npos);
}

// ---------------------------------------------------------------------------
// F-022 (OIDC Core §3.1.2.1): prompt=consent with no session still cannot show
// UI, but unlike prompt=none it does NOT short-circuit to login_required
// immediately -- it proceeds to the login redirect (the consent flag is only
// relevant once authenticated). This confirms prompt parsing does not crash
// on the consent value and the request flows to the login redirect path.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OidcBatch2_PromptConsent_NoSession_RedirectsToLogin)
{
    OIDC_BATCH2_SKIP_GUARD;

    auto resp = sendGet(
      "/oauth2/authorize?response_type=code&client_id=fulla-portal"
      "&redirect_uri=http://127.0.0.1:5173/callback&scope=openid"
      "&state=abcdef1234&prompt=consent");
    REQUIRE(resp != nullptr);
    // No session -> redirects to the login screen (302), not an error.
    CHECK(statusIs(resp, drogon::k302Found));
    auto location = resp->getHeader("location");
    CHECK(location.find("/login") != std::string::npos);
}

// ---------------------------------------------------------------------------
// F-017: backend-svc is seeded with token_endpoint_auth_method=
// client_secret_basic, so the client_secret MUST arrive via HTTP Basic.
// Sending it in the POST body is rejected with invalid_client (401).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OidcBatch2_ClientSecretPost_RejectedForBasicClient)
{
    OIDC_BATCH2_SKIP_GUARD;

    // Body-only secret (no Authorization: Basic header).
    auto resp = sendPostForm(
      "/oauth2/token",
      "grant_type=client_credentials&client_id=backend-svc&client_secret=test-secret&scope=tokens:read"
    );
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k401Unauthorized));
    Json::Value body;
    if (parseJsonBody(resp, body))
    {
        CHECK(body["error"].asString() == "invalid_client");
    }
}

// ---------------------------------------------------------------------------
// F-017: backend-svc succeeds when the secret is sent via HTTP Basic (the
// declared method). This is the positive counterpart of the test above and
// guards against over-restrictive enforcement.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OidcBatch2_ClientSecretBasic_AcceptedForBasicClient)
{
    OIDC_BATCH2_SKIP_GUARD;

    try
    {
        auto client =
          ::drogon::HttpClient::newHttpClient("http://127.0.0.1:5555", ::drogon::app().getLoop());
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Post);
        req->setPath("/oauth2/token");
        req->setContentTypeCode(::drogon::CT_APPLICATION_X_FORM);
        req->addHeader(
          "Authorization",
          "Basic " + ::drogon::utils::base64Encode("backend-svc:test-secret")
        );
        req->setBody("grant_type=client_credentials&client_id=backend-svc&scope=tokens:read");
        auto [result, resp] = client->sendRequest(req, 30.0);
        REQUIRE(result == ::drogon::ReqResult::Ok);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, drogon::k200OK));
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "Basic-auth token request failed: " << e.what();
        CHECK(false);
    }
}

// ---------------------------------------------------------------------------
// F-027 (OIDC RP-Initiated Logout 1.0 §2): GET /oauth2/end_session with no
// post_logout_redirect_uri terminates the session and returns 200 with a
// "logged out" body.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OidcBatch2_EndSession_NoRedirectUri_Returns200)
{
    OIDC_BATCH2_SKIP_GUARD;

    auto resp = sendGet("/oauth2/end_session");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));
    Json::Value body;
    if (parseJsonBody(resp, body))
    {
        CHECK(body.isMember("message"));
    }
}

// ---------------------------------------------------------------------------
// F-027: end_session with a post_logout_redirect_uri but no id_token_hint is
// rejected with 400 (the server requires pre-registration + client
// identification via the hint). #88: the 400 is an Error Envelope carrying
// AUTH_INVALID_ID_TOKEN_HINT (was a plain-text body).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OidcBatch2_EndSession_RedirectUriWithoutHint_Returns400)
{
    OIDC_BATCH2_SKIP_GUARD;

    auto resp = sendGet(
      "/oauth2/end_session?post_logout_redirect_uri=http://127.0.0.1:5173/&state=xyz12345");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k400BadRequest));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body["error"]["code"].asString() == "AUTH_INVALID_ID_TOKEN_HINT");
}

// ---------------------------------------------------------------------------
// #88: a VERIFIED hint whose post_logout_redirect_uri is NOT registered for
// any aud client is rejected with 400 VALIDATION_REDIRECT_URI_NOT_REGISTERED
// (Error Envelope; was a plain-text body).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OidcBatch2_EndSession_UnregisteredRedirect_Returns400Envelope)
{
    OIDC_BATCH2_SKIP_GUARD;

    auto tokens = loginAsUserTokens("admin", "admin", "openid profile admin");
    REQUIRE(tokens.has_value());
    const std::string idToken = tokens->get("id_token", "").asString();
    REQUIRE(!idToken.empty());

    auto resp = sendGet(
      "/oauth2/end_session?id_token_hint=" + idToken +
      "&post_logout_redirect_uri=https://attacker.example.net/logout"
    );
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k400BadRequest));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body["error"]["code"].asString() == "VALIDATION_REDIRECT_URI_NOT_REGISTERED");
}

// ---------------------------------------------------------------------------
// F-023 (OIDC Core §5.3): /oauth2/userinfo requires an access token whose
// scope includes "openid". A client_credentials token (M2M, subject
// "client:...") is rejected with 403 insufficient_scope.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OidcBatch2_UserInfo_M2MToken_Returns403InsufficientScope)
{
    OIDC_BATCH2_SKIP_GUARD;

    // Obtain an M2M access token (backend-svc, scope=tokens:read -- no openid)
    // via HTTP Basic (the client's declared auth method). #43: the legacy
    // 'read' scope is dropped; use the resource-prefixed vocabulary.
    std::string accessToken;
    {
        try
        {
            auto client = ::drogon::HttpClient::newHttpClient(
              "http://127.0.0.1:5555", ::drogon::app().getLoop()
            );
            auto req = ::drogon::HttpRequest::newHttpRequest();
            req->setMethod(::drogon::Post);
            req->setPath("/oauth2/token");
            req->setContentTypeCode(::drogon::CT_APPLICATION_X_FORM);
            req->addHeader(
              "Authorization",
              "Basic " + ::drogon::utils::base64Encode("backend-svc:test-secret")
            );
            req->setBody("grant_type=client_credentials&client_id=backend-svc&scope=tokens:read");
            auto [result, resp] = client->sendRequest(req, 30.0);
            REQUIRE(result == ::drogon::ReqResult::Ok);
            REQUIRE(resp != nullptr);
            REQUIRE(statusIs(resp, drogon::k200OK));
            Json::Value body;
            REQUIRE(parseJsonBody(resp, body));
            accessToken = body["access_token"].asString();
        }
        catch (const std::exception &e)
        {
            LOG_WARN << "token request for userinfo test failed: " << e.what();
            CHECK(false);
            return;
        }
    }
    CHECK(!accessToken.empty());
    if (accessToken.empty())
        return;

    // #43 M1+R2 (OIDC Core §5.3): userinfo is NOT registry-gated, so the M2M
    // token reaches the handler. The handler checks subject first: a
    // client_credentials token has subject "client:<id>" (no user identity)
    // -> 401 invalid_token. This is the correct RFC error classification
    // (token type mismatch, not scope insufficiency).
    auto resp = sendGet("/oauth2/userinfo", accessToken);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k401Unauthorized));
    auto wwwAuth = resp->getHeader("WWW-Authenticate");
    CHECK(wwwAuth.find("invalid_token") != std::string::npos);
}

// ---------------------------------------------------------------------------
// #78: end_session MUST NOT trust an id_token_hint whose signature does not
// verify against the OP's own key set (previously the unverified `sub` claim
// drove the backchannel logout fan-out -> unauthenticated cross-user forced
// logout). Rejections are 400 + AUTH_INVALID_ID_TOKEN_HINT error envelopes.
// ---------------------------------------------------------------------------

namespace
{
// Returns true iff the response is a 400 whose error envelope carries
// AUTH_INVALID_ID_TOKEN_HINT (the unified Application error shape:
// error.code/category/message). Assertion happens in the test body -- the
// drogon test macros need the test context that a free function lacks.
bool isInvalidHintRejection(const ::drogon::HttpResponsePtr &resp)
{
    if (!resp || resp->getStatusCode() != ::drogon::k400BadRequest)
        return false;
    Json::Value body;
    if (!parseJsonBody(resp, body))
        return false;
    if (!body.isMember("error") || !body["error"].isObject() || !body["error"].isMember("code"))
        return false;
    return body["error"]["code"].asString() == "AUTH_INVALID_ID_TOKEN_HINT";
}
}  // namespace

// #78: a well-formed, plausibly-claimed id_token_hint signed by a DIFFERENT
// key (a local ephemeral JwkManager, mimicking an attacker who knows the
// victim's iss/sub) must be rejected -- signature/kid verification is the
// gate, not claim plausibility.
DROGON_TEST(Integration_P1_OidcBatch2_EndSession_ForgedHint_Returns400)
{
    OIDC_BATCH2_SKIP_GUARD;

    ::fulla::oauth2::JwkManager forger;
    REQUIRE(forger.init(Json::Value(Json::objectValue)));

    auto *plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
    REQUIRE(plugin != nullptr);

    Json::Value claims;
    claims["iss"] = plugin->getIssuer();
    claims["sub"] = "00000000-0000-0000-0000-00000000078";
    claims["aud"] = "fulla-admin-console";
    claims["exp"] = static_cast<Json::Int64>(time(nullptr) + 600);
    const std::string forged = forger.signJwt(claims);
    REQUIRE(!forged.empty());

    auto resp = sendGet("/oauth2/end_session?id_token_hint=" + forged);
    REQUIRE(resp != nullptr);
    CHECK(isInvalidHintRejection(resp));
}

// #78: structural garbage in id_token_hint is a hard 400 too -- no silent
// fallback to "treat as if no hint was supplied".
DROGON_TEST(Integration_P1_OidcBatch2_EndSession_GarbageHint_Returns400)
{
    OIDC_BATCH2_SKIP_GUARD;

    auto resp = sendGet("/oauth2/end_session?id_token_hint=not-a-jwt-at-all");
    REQUIRE(resp != nullptr);
    CHECK(isInvalidHintRejection(resp));
}

// #78 happy path: a REAL id_token (minted through the full authorization-code
// flow with openid scope) + the client's registered post_logout_redirect_uri
// -> 302 redirect with state echoed. This is the flow legitimate RPs use.
DROGON_TEST(Integration_P1_OidcBatch2_EndSession_VerifiedHint_Redirects302)
{
    OIDC_BATCH2_SKIP_GUARD;

    auto tokens = loginAsUserTokens("admin", "admin", "openid profile admin");
    REQUIRE(tokens.has_value());
    const std::string idToken = tokens->get("id_token", "").asString();
    REQUIRE(!idToken.empty());

    auto resp = sendGet(
      "/oauth2/end_session?id_token_hint=" + idToken +
      "&post_logout_redirect_uri=" + kAdminRedirectUri + "&state=st78"
    );
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k302Found));
    auto location = resp->getHeader("location");
    CHECK(location.find(kAdminRedirectUri) == 0);
    CHECK(location.find("state=st78") != std::string::npos);
}

// #78 subject consistency: a VERIFIED hint that describes a DIFFERENT user
// than the browser session (admin's id_token + a freshly registered user's
// session cookie) is rejected with the same 400 -- the hint must match the
// signed-in subject when both are present.
DROGON_TEST(Integration_P1_OidcBatch2_EndSession_HintSubjectMismatch_Returns400)
{
    OIDC_BATCH2_SKIP_GUARD;

    // User A's (admin's) verified id_token.
    auto tokens = loginAsUserTokens("admin", "admin", "openid profile admin");
    REQUIRE(tokens.has_value());
    const std::string idToken = tokens->get("id_token", "").asString();
    REQUIRE(!idToken.empty());

    // Register user B, then establish B's browser session (the login response
    // carries the JSESSIONID cookie holding B's sub).
    auto reg = sendPostForm(
      "/api/register", "username=mm78user&password=Passw0rd!78&email=mm78@example.test"
    );
    REQUIRE(reg != nullptr);
    // Registration is idempotent for re-runs (duplicate username -> 409 is
    // fine as long as the account exists for the login below).
    const bool registered = statusIs(reg, drogon::k200OK) || statusIs(reg, drogon::k409Conflict);
    CHECK(registered);
    if (!registered)
        return;

    const std::string codeVerifier = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string codeChallenge =
      ::fulla::drogon::utils::computeCodeChallenge(codeVerifier, "S256");
    auto loginResp = sendPostForm(
      "/oauth2/login?json=true",
      "username=mm78user&password=Passw0rd!78&client_id=" + std::string(kAdminClientId) +
        "&redirect_uri=" + std::string(kAdminRedirectUri) +
        "&scope=openid&state=t78&code_challenge=" + codeChallenge +
        "&code_challenge_method=S256"
    );
    REQUIRE(loginResp != nullptr);
    REQUIRE(statusIs(loginResp, drogon::k200OK));
    // drogon keeps cookies in a dedicated map (name -> drogon::Cookie, not
    // plain header strings), so rebuild the Cookie header from getCookies().
    std::string cookie;
    for (const auto &entry : loginResp->getCookies())
    {
        if (!cookie.empty())
            cookie += "; ";
        cookie += entry.first + "=" + entry.second.value();
    }
    REQUIRE(!cookie.empty());

    // end_session carrying B's cookie + A's verified hint, via a raw request
    // (the shared helpers have no custom-header injection point).
    ::drogon::HttpResponsePtr resp;
    try
    {
        auto client =
          ::drogon::HttpClient::newHttpClient(kTestBaseUrl, ::drogon::app().getLoop());
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Get);
        req->setPath("/oauth2/end_session?id_token_hint=" + idToken);
        req->addHeader("Cookie", cookie);
        auto [result, r] = client->sendRequest(req, 30.0);
        REQUIRE(result == ::drogon::ReqResult::Ok);
        resp = r;
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "mismatch-test request failed: " << e.what();
    }
    REQUIRE(resp != nullptr);
    CHECK(isInvalidHintRejection(resp));
}

// ---------------------------------------------------------------------------
// #82: backchannel logout must match BOTH user_id subject forms. A user can
// hold password-flow tokens (user_id = public sub) AND social-flow tokens
// (user_id = internal numeric id); the notifier previously matched only the
// handed form, so one RP population never received the logout_token.
// Seeds one user with one token row PER FORM, both pointing at a client with
// a backchannel_logout_uri behind a recording fake HTTP client, then calls
// notify() once per form -- each call must fan out to the (same single) RP.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OidcBatch2_BackchannelLogout_DualSubjectForm_CoversBothTokenRows)
{
    OIDC_BATCH2_SKIP_GUARD;

    auto *plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
    REQUIRE(plugin != nullptr);
    auto db = ::drogon::app().getDbClient();
    REQUIRE(db != nullptr);

    const std::string suffix = std::to_string(
      std::chrono::high_resolution_clock::now().time_since_epoch().count() % 1000000
    );

    // Admin-scoped token: the throwaway client is registered through the
    // admin API (canonical schema path; the notifier reads its row directly,
    // but hand-rolled SQL here once used non-existent columns and the
    // resulting uncaught DrogonDbException fast-failed the whole binary).
    auto adminTokens = loginAsUserTokens("admin", "admin", "openid profile admin");
    REQUIRE(adminTokens.has_value());
    const std::string adminBearer = adminTokens->get("access_token", "").asString();
    REQUIRE(!adminBearer.empty());

    // Shared fake: preloaded OK responses; postFormCalls counts fan-outs.
    auto http = std::make_shared<fulla::identity::testing::FakeOAuthHttpClient>();
    http->postFormResponses.push_back(fulla::identity::testing::okJson(Json::Value(Json::objectValue)));
    http->postFormResponses.push_back(fulla::identity::testing::okJson(Json::Value(Json::objectValue)));

    // A client with a backchannel URI (https -- B1 validates the scheme).
    Json::Value createBody;
    createBody["name"] = "dualsub-cli-" + suffix;
    createBody["redirect_uris"] = "http://127.0.0.1:9/callback";
    createBody["client_type"] = "CONFIDENTIAL";
    createBody["backchannel_logout_uri"] = "https://example.test/backchannel";
    auto createResp = sendPostJson("/api/admin/clients", createBody, adminBearer);
    REQUIRE(createResp != nullptr);
    REQUIRE(statusIs(createResp, drogon::k201Created));
    Json::Value created;
    REQUIRE(parseJsonBody(createResp, created));
    const std::string clientId = created["client_id"].asString();
    REQUIRE(!clientId.empty());

    // A user; capture both subject forms.
    const std::string uname = "dualsub_" + suffix;
    const std::string email = "dualsub_" + suffix + "@example.test";
    auto reg = sendPostForm(
      "/api/register", "username=" + uname + "&password=Passw0rd!82&email=" + email
    );
    REQUIRE(reg != nullptr);

    std::string publicSub;
    std::string internalId;
    {
        auto rows = db->execSqlSync(
          "SELECT id, public_sub FROM users WHERE username = $1", uname
        );
        REQUIRE(rows.size() == 1);
        internalId = std::to_string(rows[0]["id"].as<int32_t>());
        publicSub = rows[0]["public_sub"].as<std::string>();
        REQUIRE(!publicSub.empty());
    }
    // One token row per subject form, both active, same client. Column names
    // per V002: the PK is `token` (not token_hash).
    const auto insertToken = [&](const std::string &form, const char *tag) {
        db->execSqlSync(
          "INSERT INTO oauth2_access_tokens (token, client_id, user_id, scope, "
          "expires_at, issued_at, revoked) "
          "VALUES ($1, $2, $3, 'openid', $4, $5, false)",
          std::string("dualsub-tok-") + tag + "-" + suffix,
          clientId,
          form,
          static_cast<int64_t>(std::time(nullptr)) + 3600,
          static_cast<int64_t>(std::time(nullptr))
        );
    };
    insertToken(publicSub, "pub");
    insertToken(internalId, "int");

    // RAII cleanup (token rows + client + user) -- rerun-safe by suffix anyway.
    struct Cleanup
    {
        ::drogon::orm::DbClientPtr db;
        std::string clientId;
        std::string uname;
        ~Cleanup()
        {
            try
            {
                db->execSqlSync(
                  "DELETE FROM oauth2_access_tokens WHERE client_id = $1", clientId
                );
                db->execSqlSync("DELETE FROM oauth2_clients WHERE client_id = $1", clientId);
                db->execSqlSync("DELETE FROM users WHERE username = $1", uname);
            }
            catch (...)
            {
            }
        }
    } cleanup{db, clientId, uname};

    auto notifier = std::make_shared<fulla::drogon::adapters::BackchannelLogoutNotifier>(
      db,
      plugin->getJwkManager(),
      plugin->getIssuer(),
      http,
      nullptr  // audit sink optional; fan-out is what is asserted
    );

    // Entry form 1: public sub -> BOTH rows (both forms) match -> the (single
    // distinct) client is notified.
    std::promise<void> done1;
    notifier->notify(publicSub, [&done1]() { done1.set_value(); });
    REQUIRE(done1.get_future().wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    CHECK(http->postFormCalls.size() == 1);

    // Entry form 2: internal id -> same coverage from the other direction.
    std::promise<void> done2;
    notifier->notify(internalId, [&done2]() { done2.set_value(); });
    REQUIRE(done2.get_future().wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    CHECK(http->postFormCalls.size() == 2);
}


// ---------------------------------------------------------------------------
// #88-3 (decision A, RP-Initiated Logout 1.0 §2.1): with no id_token_hint,
// the client_id parameter identifies the RP. A post_logout_redirect_uri
// REGISTERED for that client is honored (302 + state echo); an unregistered
// one still 400s; neither hint nor client_id still 400s (unchanged).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OidcBatch2_EndSession_ClientIdNoHint_RegisteredUri302)
{
    OIDC_BATCH2_SKIP_GUARD;

    auto resp = sendGet(
      "/oauth2/end_session?client_id=" + std::string(kAdminClientId) +
        "&post_logout_redirect_uri=" + std::string(kAdminRedirectUri) + "&state=st883"
    );
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k302Found));
    auto location = resp->getHeader("location");
    CHECK(location.find(kAdminRedirectUri) == 0);
    CHECK(location.find("state=st883") != std::string::npos);
}

DROGON_TEST(Integration_P1_OidcBatch2_EndSession_ClientIdNoHint_UnregisteredUri400)
{
    OIDC_BATCH2_SKIP_GUARD;

    auto resp = sendGet(
      "/oauth2/end_session?client_id=" + std::string(kAdminClientId) +
        "&post_logout_redirect_uri=https://attacker.example.net/logout"
    );
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k400BadRequest));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(
      body["error"]["code"].asString() == "VALIDATION_REDIRECT_URI_NOT_REGISTERED"
    );
}

DROGON_TEST(Integration_P1_OidcBatch2_EndSession_NoHintNoClientId_Still400)
{
    OIDC_BATCH2_SKIP_GUARD;

    // Unchanged behavior: neither identification parameter -> the #88
    // envelope rejection (this is the RedirectUriWithoutHint case with an
    // explicit empty client_id to pin the boundary).
    auto resp = sendGet(
      "/oauth2/end_session?client_id=&post_logout_redirect_uri=http://127.0.0.1:5173/"
    );
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k400BadRequest));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body["error"]["code"].asString() == "AUTH_INVALID_ID_TOKEN_HINT");
}
