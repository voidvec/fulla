// tests/integration/controllers/AuthorizeEndpointHttpTest.cc
//
// HTTP integration tests for the OAuth2 authorization endpoint's validation
// branches (libs/drogon/src/controllers/AuthorizationEndpointController.cc,
// 279 LOC, 47% covered). The happy path (logged-in user -> code -> redirect)
// is covered by the existing tests/integration/auth/AuthorizePkcePassthrough
// test; this file targets the early-rejection branches that gcov shows as
// uncovered: missing/invalid/length checks on the `state` CSRF
// parameter, invalid client_id, and invalid redirect_uri.
//
// Route: GET /oauth2/authorize (no user auth at this stage -- it is the
// authorization-page entry point; rejected requests return 400 before any
// session check).

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <fulla/drogon/utils/PortalUrl.h>
#include <json/json.h>

#include "HttpTestClient.h"

#include <string>

using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendGet;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define AUTHORIZE_SKIP_GUARD                                   \
    do                                                         \
    {                                                          \
        if (!postgresAvailable() || !serverReachable())        \
        {                                                      \
            CHECK(true);                                       \
            return;                                            \
        }                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// missing state: a request with no `state` parameter returns 400 (the
// "state parameter is required for CSRF protection" branch, ~line 59).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_Authorize_MissingState_Returns400)
{
    AUTHORIZE_SKIP_GUARD;

    auto resp = sendGet(
      "/oauth2/authorize?response_type=code&client_id=fulla-admin-console"
      "&redirect_uri=http://127.0.0.1:5174/admin/callback");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k400BadRequest));
    const std::string body(resp->getBody());
    CHECK(body.find("state") != std::string::npos);
}

// ---------------------------------------------------------------------------
// too-short state: a `state` shorter than 8 chars returns 400 (the length
// check branch, ~line 70).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_Authorize_StateTooShort_Returns400)
{
    AUTHORIZE_SKIP_GUARD;

    auto resp = sendGet(
      "/oauth2/authorize?response_type=code&client_id=fulla-admin-console"
      "&redirect_uri=http://localhost:5174/admin/callback&state=short");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k400BadRequest));
}

// ---------------------------------------------------------------------------
// Opaque delimiter-state round trips are exercised with component-encoded
// real HTTP requests in OpaqueOAuthStateHttpTest.cc. Delimiters are values,
// not a separate validation failure; missing/length/client checks stay here.

// ---------------------------------------------------------------------------
// invalid client_id: a request with a valid state but an unknown client_id
// returns 400 ("Invalid client_id", ~line 162). Covers the
// validateClient-rejects branch.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_Authorize_UnknownClientId_Returns400)
{
    AUTHORIZE_SKIP_GUARD;

    auto resp = sendGet(
      "/oauth2/authorize?response_type=code&client_id=nonexistent-client-xyz"
      "&redirect_uri=http://localhost:5174/admin/callback&state=validstate1234");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k400BadRequest));
    const std::string body(resp->getBody());
    CHECK(body.find("client_id") != std::string::npos);
}

// ---------------------------------------------------------------------------
// not-logged-in happy branch: a fully-valid request with NO session (the test
// client shares no session with a browser) is redirected to the login page
// rather than 400-rejected. Asserts a 302 (or the login-redirect body) -- the
// exact response is a redirect to /login or a 200 login page. Accept 302/200.
// Covers the "userId.empty() -> redirect to login" branch (~line 215).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_Authorize_ValidRequest_NoSession_RedirectsToLogin)
{
    AUTHORIZE_SKIP_GUARD;

    auto resp = sendGet(
      "/oauth2/authorize?response_type=code&client_id=fulla-admin-console"
      "&redirect_uri=http://127.0.0.1:5174/admin/callback&scope=openid"
      "&state=validstate1234"
      "&code_challenge=authorize-http-test-challenge-fixed-padding__&code_challenge_method=S256");
    REQUIRE(resp != nullptr);
    const auto code = resp->getStatusCode();
    // Not logged in -> redirect (302) to login, or a 200 login page. Crucially
    // NOT a 400 (the request is valid; the user just isn't authenticated yet).
    CHECK((code == drogon::k302Found || code == drogon::k200OK));
    CHECK(code != drogon::k400BadRequest);
}

// ---------------------------------------------------------------------------
// The authorize login/consent targets are configurable
// (custom_config.oauth2.login_url / consent_url, also from the environment).
// The DEFAULTS must stay what they always were -- the backend-relative
// "/login" and "/consent" -- because running code depends on them: the
// embedded host example and the browser demo drive the backend's own login
// form, and the capacity harness checks the consent redirect against the
// configured consent_url. The shipped configs carry both keys as "" so that
// the overrides land; "" has to read as "not set", or the redirect target
// would be an empty string.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_Authorize_LoginRedirectDefaultIsBackendLogin)
{
    AUTHORIZE_SKIP_GUARD;

    const Json::Value customConfig = drogon::app().getCustomConfig();
    REQUIRE(customConfig.isMember("oauth2"));
    CHECK(customConfig["oauth2"]["login_url"].isString());
    CHECK(customConfig["oauth2"]["login_url"].asString().empty());
    CHECK(customConfig["oauth2"]["consent_url"].isString());
    CHECK(customConfig["oauth2"]["consent_url"].asString().empty());
    CHECK(::fulla::drogon::utils::authorizeLoginUrl() == "/login");
    CHECK(::fulla::drogon::utils::authorizeConsentUrl() == "/consent");

    // A configured target may already carry a query string; the first
    // authorize parameter is then appended with '&'.
    CHECK(::fulla::drogon::utils::authorizeUrlQuerySeparator("/login") == '?');
    CHECK(
      ::fulla::drogon::utils::authorizeUrlQuerySeparator("https://sso.example.com/a?x=1") ==
      '&'
    );

    auto resp = sendGet(
      "/oauth2/authorize?response_type=code&client_id=fulla-admin-console"
      "&redirect_uri=http://127.0.0.1:5174/admin/callback&scope=openid"
      "&state=validstate1234"
      "&code_challenge=authorize-http-test-challenge-fixed-padding__&code_challenge_method=S256");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k302Found));
    // The expected target is spelled out, not derived through the helper
    // under test.
    CHECK(resp->getHeader("Location").find("/login?client_id=") == 0);
}
