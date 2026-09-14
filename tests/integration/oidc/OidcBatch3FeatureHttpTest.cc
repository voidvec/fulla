// tests/integration/oidc/OidcBatch3FeatureHttpTest.cc
//
// HTTP integration tests for the OAuth/OIDC compliance Batch 3 features:
//   - F-010: minimal path -> required-scope enforcement
//       /oauth2/userinfo requires "openid"
//       /api/me and /api/me/* require "profile"
//       /api/admin/* require "admin" scope (in addition to the RBAC role gate)
//     A token missing the required scope is rejected with 403
//     insufficient_scope + a RFC 6750 §3.1 WWW-Authenticate challenge whose
//     `scope` attribute names the missing scope.
//   - F-018: in-process sliding-window failure rate limiter for
//     /oauth2/token, /oauth2/introspect, /oauth2/revoke, and device-code
//     polling. After `max_failures` (default 30) per (ip, client_id) within
//     the rolling window, subsequent attempts return 429 + Retry-After.
//
// These tests require Postgres (seed clients) + the live HTTP listener on
// 127.0.0.1:5555; they skip cleanly otherwise (same guard as Batch 2).

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include "HttpTestClient.h"

#include <string>

using fulla::test::http::loginAsAdmin;
using fulla::test::http::loginAsAdminWithScope;
using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendGet;
using fulla::test::http::sendPostForm;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define OIDC_BATCH3_SKIP_GUARD                                 \
    do                                                         \
    {                                                          \
        if (!postgresAvailable() || !serverReachable())        \
        {                                                      \
            CHECK(true);                                       \
            return;                                            \
        }                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// F-010 (RFC 6750 §3.1): /api/me requires the `profile` scope. A token that
// carries ONLY `openid` (no profile) must be rejected with 403
// insufficient_scope, and the WWW-Authenticate challenge MUST name `profile`
// as the required scope (RFC 6750 §3.1 REQUIRES the `scope` attribute).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OidcBatch3_ApiMe_TokenWithoutProfile_Returns403InsufficientScope)
{
    OIDC_BATCH3_SKIP_GUARD;

    // Request a token that carries ONLY openid (no profile/admin) -- the
    // fulla-admin-console seed grants openid/profile/admin, but the SCOPE WE PASS
    // to authorize is the gate, and the resulting token carries only what
    // was authorized.
    auto token = loginAsAdminWithScope("openid");
    REQUIRE(token.has_value());

    auto resp = sendGet("/api/me", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k403Forbidden));
    auto wwwAuth = resp->getHeader("WWW-Authenticate");
    CHECK(wwwAuth.find("insufficient_scope") != std::string::npos);
    CHECK(wwwAuth.find("scope=\"profile\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// #43 (RFC 6750 §3.1): /api/admin/clients requires the `clients:read` scope
// (or the `admin` super-scope via impliedBy). A token that carries
// `openid profile` (no admin-family scope) must be rejected with 403
// insufficient_scope even though the admin USER has the admin role -- the
// scope gate is independent of RBAC.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OidcBatch3_ApiAdmin_TokenWithoutAdminScope_Returns403InsufficientScope)
{
    OIDC_BATCH3_SKIP_GUARD;

    auto token = loginAsAdminWithScope("openid profile");
    REQUIRE(token.has_value());

    auto resp = sendGet("/api/admin/clients", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k403Forbidden));
    auto wwwAuth = resp->getHeader("WWW-Authenticate");
    CHECK(wwwAuth.find("insufficient_scope") != std::string::npos);
    // #43: the challenge names the concrete required scope (clients:read),
    // not the former blanket "admin".
    CHECK(wwwAuth.find("scope=\"clients:read\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// F-010 positive control: the default admin token (openid profile admin) CAN
// still reach /api/me and /api/admin/* -- the new scope gate does not lock
// out the legitimate fulla-admin-console login chain. This guards against an
// over-broad scope regex accidentally rejecting valid tokens.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OidcBatch3_AdminToken_StillReachesProtectedRoutes)
{
    OIDC_BATCH3_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto meResp = sendGet("/api/me", *token);
    REQUIRE(meResp != nullptr);
    CHECK(statusIs(meResp, drogon::k200OK));

    auto adminResp = sendGet("/api/admin/clients", *token);
    REQUIRE(adminResp != nullptr);
    CHECK(statusIs(adminResp, drogon::k200OK));
}

// ---------------------------------------------------------------------------
// F-018: after `max_failures` (default 30) failing token requests for the
// same (ip, client_id), the next attempt is throttled with 429 + Retry-After.
//
// Uses a synthetic client_id so its failure bucket is isolated from every
// other test (the bucket key is (ip, client_id)). Sends 35 failing requests
// (invalid_client -- the synthetic client does not exist) and asserts that
// at least one 429 is returned. The first ~30 are 401 (the genuine
// invalid-client rejection); the 31st onward trip the limiter.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_OidcBatch3_TokenEndpoint_RateLimitsAfterFailureThreshold)
{
    OIDC_BATCH3_SKIP_GUARD;

    // Unique-per-run client_id so this test's failure bucket cannot pollute,
    // or be polluted by, any other test that makes failing token requests.
    // The rate limiter is a process-wide singleton shared across all tests.
    const std::string syntheticClientId =
      "rate-limit-test-" + ::drogon::utils::getUuid();

    int throttledCount = 0;
    int rejectedCount = 0;
    // 35 > 30 (default max_failures) -- the tail of this loop MUST trip 429.
    for (int i = 0; i < 35; ++i)
    {
        // F-018: a request with a syntactically-valid but unknown client_id
        // is the canonical "failed auth attempt" the limiter counts -- the
        // client does not exist, so getClient() returns nullopt -> 401
        // invalid_client -> recorded as a failure against (ip, client_id).
        std::string form =
          "grant_type=client_credentials&client_id=" + syntheticClientId +
          "&client_secret=anything&scope=tokens:read";
        auto resp = sendPostForm("/oauth2/token", form);
        if (!resp)
            continue;
        auto code = resp->getStatusCode();
        if (code == drogon::k429TooManyRequests)
        {
            ++throttledCount;
            // RFC 6585 §4: 429 MUST carry Retry-After.
            auto retryAfter = resp->getHeader("Retry-After");
            CHECK(!retryAfter.empty());
        }
        else if (code == drogon::k401Unauthorized)
        {
            ++rejectedCount;
        }
    }
    CHECK(throttledCount > 0);   // the limiter DID fire
    CHECK(rejectedCount > 0);    // the genuine invalid_client path also ran
}
