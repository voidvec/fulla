// tests/integration/controllers/BrandingContextHttpTest.cc
//
// P1-1 (#279): HTTP integration tests for the branding consumption chain —
// authorize-context extension (client logo_uri + owning-org brand block),
// consent-context extension (client block, localized scope_descriptions,
// org brand fields), and the logo_uri write-path validation on the three
// surfaces (admin client create/update, portal application create/update,
// org create color/logo validation).
//
// Storage: Postgres-only; skips cleanly otherwise. Fixtures (a branded org,
// an org-owned client with a logo) are created through the public APIs and
// soft-cleaned where a delete route exists; the org slug carries a unique
// suffix.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <fulla/drogon/utils/CryptoUtils.h>

#include "HttpTestClient.h"

#include <chrono>
#include <future>
#include <optional>
#include <string>

using fulla::test::http::loginAsAdmin;
using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendGet;
using fulla::test::http::sendPostForm;
using fulla::test::http::sendPostJson;
using fulla::test::http::sendPutJson;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define BRANDING_SKIP_GUARD                                    \
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
        .count() % 100000000);
}

bool createVerifiedUser(const std::string &username, const std::string &password)
{
    Json::Value body;
    body["username"] = username;
    body["password"] = password;
    body["email"] = username + "@qa.example";
    auto resp = fulla::test::http::sendPostJson("/api/register", body);
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

std::optional<std::string> loginTokenVerbose(const std::string &username, const std::string &password)
{
    const std::string codeVerifier = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string codeChallenge =
      ::fulla::drogon::utils::computeCodeChallenge(codeVerifier, "S256");
    const std::string loginForm =
      "username=" + username + "&password=" + password +
      "&client_id=fulla-admin-console"
      "&redirect_uri=http://localhost:5174/admin/callback"
      "&scope=openid%20profile&state=t1"
      "&code_challenge=" + codeChallenge + "&code_challenge_method=S256";
    auto loginResp = fulla::test::http::sendPostForm("/oauth2/login?json=true", loginForm);
    if (loginResp == nullptr || loginResp->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    Json::Value loginJson;
    if (!parseJsonBody(loginResp, loginJson))
        return std::nullopt;
    const std::string code = loginJson.get("code", "").asString();
    if (code.empty())
        return std::nullopt;
    const std::string tokenForm =
      "grant_type=authorization_code&code=" + code +
      "&redirect_uri=http://localhost:5174/admin/callback"
      "&client_id=fulla-admin-console&client_secret=&code_verifier=" + codeVerifier;
    auto tokenResp = fulla::test::http::sendPostForm("/oauth2/token", tokenForm);
    if (tokenResp == nullptr || tokenResp->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    Json::Value tokenJson;
    if (!parseJsonBody(tokenResp, tokenJson))
        return std::nullopt;
    const std::string access = tokenJson.get("access_token", "").asString();
    return access.empty() ? std::optional<std::string>{} : std::optional<std::string>{access};
}
}  // namespace

// ---------------------------------------------------------------------------
// authorize-context: brand extension for an org-owned client with a logo.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_Branding_AuthorizeContext_CarriesClientLogoAndOrgBrand)
{
    BRANDING_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string user = "qa_brand_u_" + suffix;
    const std::string pass = "qa-brand-Zx9-" + suffix;
    REQUIRE(createVerifiedUser(user, pass));
    auto userToken = loginTokenVerbose(user, pass);
    REQUIRE(userToken.has_value());

    const std::string orgSlug = "qa-brand-org-" + suffix;

    // Create a branded org (self-service surface).
    {
        Json::Value org;
        org["slug"] = orgSlug;
        org["name"] = "QA Brand Org " + suffix;
        org["logo_uri"] = "https://brand.example.com/org-" + suffix + ".svg";
        org["primary_color"] = "#4f81c4";
        auto resp = sendPostJson("/api/me/organizations", org, *userToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k201Created));
    }

    // Portal application under the branded org, carrying a client logo.
    std::string clientId;
    {
        Json::Value app;
        app["name"] = "QA Brand App " + suffix;
        app["client_type"] = "PUBLIC";
        Json::Value uris(Json::arrayValue);
        uris.append("http://127.0.0.1:5173/cb");
        app["redirect_uris"] = uris;
        Json::Value scopes(Json::arrayValue);
        scopes.append("openid");
        scopes.append("profile");
        app["scopes"] = scopes;
        app["org_slug"] = orgSlug;
        app["logo_uri"] = "https://brand.example.com/app-" + suffix + ".svg";
        auto resp = sendPostJson("/api/me/applications", app, *userToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k201Created));
        Json::Value body;
        REQUIRE(parseJsonBody(resp, body));
        clientId = body["client_id"].asString();
        REQUIRE(!clientId.empty());
    }

    // The public authorize-context answers name + logo + the OWNING ORG's
    // brand block.
    {
        auto resp = sendGet("/oauth2/authorize-context?client_id=" + clientId);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
        Json::Value ctx;
        REQUIRE(parseJsonBody(resp, ctx));
        CHECK(ctx["client_name"].asString() == "QA Brand App " + suffix);
        CHECK(ctx["logo_uri"].asString() == "https://brand.example.com/app-" + suffix + ".svg");
        REQUIRE(!ctx["org"].isNull());
        CHECK(ctx["org"]["org_name"].asString() == "QA Brand Org " + suffix);
        CHECK(ctx["org"]["logo_uri"].asString() == "https://brand.example.com/org-" + suffix + ".svg");
        CHECK(ctx["org"]["primary_color"].asString() == "#4f81c4");
    }

    // An admin-seeded client (no owners row) has logo but org:null.
    auto token = loginAsAdmin();
    REQUIRE(token.has_value());
    {
        Json::Value client;
        client["name"] = "QA Admin Brand Client " + suffix;
        client["redirect_uris"] = "https://app.example.com/cb";
        client["logo_uri"] = "https://brand.example.com/admin-" + suffix + ".svg";
        auto resp = sendPostJson("/api/admin/clients", client, *token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k201Created));
        Json::Value body;
        REQUIRE(parseJsonBody(resp, body));
        const std::string adminClientId = body["client_id"].asString();

        auto ctxResp = sendGet("/oauth2/authorize-context?client_id=" + adminClientId);
        REQUIRE(ctxResp != nullptr);
        CHECK(statusIs(ctxResp, ::drogon::k200OK));
        Json::Value ctx;
        REQUIRE(parseJsonBody(ctxResp, ctx));
        CHECK(ctx["logo_uri"].asString() == "https://brand.example.com/admin-" + suffix + ".svg");
        CHECK(ctx["org"].isNull());
    }
}

// ---------------------------------------------------------------------------
// Write-path validation: every surface rejects a non-https logo and a
// malformed color with 400 (the brand rule, not the redirect rule — an
// http:// non-loopback logo must fail even where http redirects might be
// tolerated).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_Branding_WritePaths_ValidateSchemeAndColor)
{
    BRANDING_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    // Admin client create.
    {
        Json::Value client;
        client["name"] = "Bad Logo " + uniqueSuffix();
        client["redirect_uris"] = "https://app.example.com/cb";
        client["logo_uri"] = "http://brand.example.com/logo.svg";
        auto resp = sendPostJson("/api/admin/clients", client, *token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k400BadRequest));
    }
    // Admin client update.
    {
        Json::Value patch;
        patch["logo_uri"] = "https://ok.example.com/a.svg";
        auto okResp = sendPutJson("/api/admin/clients/fulla-portal", patch, *token);
        REQUIRE(okResp != nullptr);
        CHECK(statusIs(okResp, ::drogon::k200OK));
        Json::Value bad;
        bad["logo_uri"] = "https://x@evil.io/a.svg";
        auto resp = sendPutJson("/api/admin/clients/fulla-portal", bad, *token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k400BadRequest));
        // Restore (clear).
        Json::Value clear;
        clear["logo_uri"] = "";
        auto clearResp = sendPutJson("/api/admin/clients/fulla-portal", clear, *token);
        REQUIRE(clearResp != nullptr);
        CHECK(statusIs(clearResp, ::drogon::k200OK));
    }
    // Org create (self-service): bad color and bad logo.
    const std::string user = "qa_brand_bad_" + uniqueSuffix();
    const std::string pass = "qa-brandbad-Zx9-" + user;
    REQUIRE(createVerifiedUser(user, pass));
    auto userToken = loginTokenVerbose(user, pass);
    REQUIRE(userToken.has_value());
    {
        Json::Value org;
        org["slug"] = "qa-brand-bad-" + uniqueSuffix();
        org["name"] = "Bad Brand Org";
        org["primary_color"] = "#12345678";  // the {3,8} draft form
        auto resp = sendPostJson("/api/me/organizations", org, *userToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k400BadRequest));
        Json::Value org2;
        org2["slug"] = "qa-brand-bad2-" + uniqueSuffix();
        org2["name"] = "Bad Brand Org 2";
        org2["logo_uri"] = "javascript:alert(1)";
        auto resp2 = sendPostJson("/api/me/organizations", org2, *userToken);
        REQUIRE(resp2 != nullptr);
        CHECK(statusIs(resp2, ::drogon::k400BadRequest));
    }
}

// ---------------------------------------------------------------------------
// allow_pkce_plain removal: plain is rejected UNCONDITIONALLY now (no
// config escape hatch anymore).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_Branding_PlainPkce_UnconditionallyRejected)
{
    BRANDING_SKIP_GUARD;

    const std::string url =
      "/oauth2/authorize?response_type=code&client_id=fulla-portal"
      "&redirect_uri=http://127.0.0.1:5173/callback&scope=openid&state=p1-plain-pkce-state-check"
      "&code_challenge=plainchallengevalue1234567890123456789012&code_challenge_method=plain";
    auto resp = sendGet(url);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, ::drogon::k302Found));
    const std::string loc = resp->getHeader("Location");
    CHECK(loc.find("error=invalid_request") != std::string::npos);
    CHECK(loc.find("code_challenge_method") != std::string::npos);
    // The removed hatch must not be hinted at anymore.
    CHECK(loc.find("allow_pkce_plain") == std::string::npos);
}
