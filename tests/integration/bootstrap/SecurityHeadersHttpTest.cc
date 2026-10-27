// tests/integration/bootstrap/SecurityHeadersHttpTest.cc
//
// P1-5 (#282) regression: the login page's Content-Security-Policy must let
// the browser follow the post-login 302 to the client's registered
// redirect_uri. Modern Chrome/Edge enforce `form-action` across redirects
// that follow a form submission, so the strict `form-action 'self'` broke
// EVERY browser-based RP login (the nextjs-demo Playwright driver caught it
// live; the json-mode interop drivers cannot). The broker profile keeps
// plain-http exfiltration blocked (https: plus explicit loopback entries).

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>

#include "HttpTestClient.h"

#include <string>

using fulla::test::http::serverReachable;

DROGON_TEST(Integration_Bootstrap_SecurityHeaders_LoginPageIsOauthBroker)
{
    if (!serverReachable())
    {
        CHECK(true);
        return;
    }
    auto resp = fulla::test::http::sendGet("/login?client_id=fulla-portal");
    REQUIRE(resp != nullptr);
    const std::string csp = resp->getHeader("Content-Security-Policy");
    CHECK(!csp.empty());

    // Broker profile: the redirect after the form POST may land on any
    // https redirect_uri, and on loopback origins for dev RPs.
    CHECK(csp.find("form-action 'self' https: http://localhost:* http://127.0.0.1:*")
          != std::string::npos);

    // The non-form directives stay strict (no script/style widening rode
    // along with the form-action fix).
    CHECK(csp.find("script-src 'self'") != std::string::npos);
    CHECK(csp.find("frame-ancestors 'none'") != std::string::npos);
}
