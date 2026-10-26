// tests/integration/oidc/UserinfoScopeGatingHttpTest.cc
//
// P0-2 (#256, OIDC Core §5.4): userinfo claims are scope-gated. Before this,
// a bare `openid` token released name/username/picture/email/email_verified
// unconditionally. The matrix here pins:
//   - openid only        -> sub only
//   - openid profile     -> name/preferred_username (+picture when set), NO email
//   - openid email       -> email/email_verified, NO profile claims
// The legacy `username` key dual-emits next to preferred_username until
// v1.6.0.
//
// Same guard as the other HTTP batches: requires Postgres (seed clients) +
// the live listener on 127.0.0.1:5555; skips cleanly otherwise.

#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <json/json.h>

#include "HttpTestClient.h"

#include <fulla/drogon/utils/CryptoUtils.h>

#include <optional>
#include <string>

using fulla::test::http::loginAsAdminWithScope;
using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendGet;
using fulla::test::http::sendPostForm;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define USERINFO_GATING_SKIP_GUARD                              \
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

// The seed admin-console client's granted scope set (openid profile admin)
// has no `email`, so an email-scoped login is rejected by the client scope
// allowlist before it ever reaches userinfo. The email leg of the matrix
// therefore logs in through the fulla-portal client (PUBLIC; openid/profile/
// email granted by dev_portal_client.sql; loopback redirect registered).
std::optional<std::string> loginAsAdminViaPortal(const std::string &scope)
{
    const std::string codeVerifier = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string codeChallenge =
      ::fulla::drogon::utils::computeCodeChallenge(codeVerifier, "S256");
    const std::string redirectUri = "http://localhost:5173/callback";

    const std::string loginForm =
      "username=admin&password=admin"
      "&client_id=fulla-portal&redirect_uri=" +
      redirectUri + "&scope=" + scope + "&state=t1"
      "&code_challenge=" + codeChallenge + "&code_challenge_method=S256";
    auto loginResp = sendPostForm("/oauth2/login?json=true", loginForm);
    if (!loginResp || loginResp->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    Json::Value loginJson;
    if (!parseJsonBody(loginResp, loginJson))
        return std::nullopt;
    if (loginJson.isMember("mfa_required") && loginJson["mfa_required"].asBool())
        return std::nullopt;
    const std::string code = loginJson.get("code", "").asString();
    if (code.empty())
        return std::nullopt;

    const std::string tokenForm =
      "grant_type=authorization_code&code=" + code +
      "&redirect_uri=" + redirectUri +
      "&client_id=fulla-portal&client_secret=&code_verifier=" + codeVerifier;
    auto tokenResp = sendPostForm("/oauth2/token", tokenForm);
    if (!tokenResp || tokenResp->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    Json::Value tokenJson;
    if (!parseJsonBody(tokenResp, tokenJson))
        return std::nullopt;
    const std::string token = tokenJson.get("access_token", "").asString();
    if (token.empty())
        return std::nullopt;
    return token;
}

}  // namespace

DROGON_TEST(Integration_P0_Userinfo_OpenIdOnly_ReturnsSubOnly)
{
    USERINFO_GATING_SKIP_GUARD;

    auto token = loginAsAdminWithScope("openid");
    REQUIRE(token.has_value());

    auto resp = sendGet("/oauth2/userinfo", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));

    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body.isMember("sub"));
    CHECK(!body.isMember("name"));
    CHECK(!body.isMember("preferred_username"));
    CHECK(!body.isMember("username"));
    CHECK(!body.isMember("picture"));
    CHECK(!body.isMember("email"));
    CHECK(!body.isMember("email_verified"));
}

DROGON_TEST(Integration_P0_Userinfo_ProfileScope_ReleasesProfileClaimsOnly)
{
    USERINFO_GATING_SKIP_GUARD;

    auto token = loginAsAdminWithScope("openid profile");
    REQUIRE(token.has_value());

    auto resp = sendGet("/oauth2/userinfo", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));

    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body.isMember("name"));
    // Standard claim only: the legacy `username` dual-emission was REMOVED
    // in v1.6.0 (P1-3/D5, pre-announced in the v1.5.0 CHANGELOG).
    CHECK(body.isMember("preferred_username"));
    CHECK(!body.isMember("username"));
    // The seed admin has an email address — the email claims must NOT be
    // released without the email scope.
    CHECK(!body.isMember("email"));
    CHECK(!body.isMember("email_verified"));
}

DROGON_TEST(Integration_P0_Userinfo_EmailScope_ReleasesEmailClaimsOnly)
{
    USERINFO_GATING_SKIP_GUARD;

    auto token = loginAsAdminViaPortal("openid email");
    REQUIRE(token.has_value());

    auto resp = sendGet("/oauth2/userinfo", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));

    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body.isMember("email"));
    CHECK(body.isMember("email_verified"));
    CHECK(!body.isMember("name"));
    CHECK(!body.isMember("preferred_username"));
    CHECK(!body.isMember("username"));
    CHECK(!body.isMember("picture"));
}
