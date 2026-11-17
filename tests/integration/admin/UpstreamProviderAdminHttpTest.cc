// UpstreamProviderAdminHttpTest.cc
//
// P2-1 integration coverage for the OIDC-in upstream registry over the real
// HTTP surface (postgres legs): admin CRUD lifecycle, secret write-only
// semantics, login-page listing merge, and the authorize transaction for a
// registry-configured provider (endpoint_overrides pin the endpoints, so no
// live upstream is needed). The full code->token->userinfo protocol is
// covered by OidcUpstreamAuthServiceTest (fakes) and the batch-B Keycloak
// E2E; this file stays hermetic.

#include "HttpTestClient.h"
#include "SocialMockFixture.h"

#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include <future>
#include <optional>
#include <string>

namespace
{

using namespace drogon;

bool providerRowMatches(
  const Json::Value &providers,
  const std::string &key,
  Json::Value &out)
{
    for (const auto &provider : providers)
    {
        if (provider.get("provider_key", "").asString() == key)
        {
            out = provider;
            return true;
        }
    }
    return false;
}

// The 200-leg of the binding dance, returned so the caller can inspect
// authorize_url (LoginBrowser::begin only keeps the state).
std::optional<Json::Value> authorizeTransaction(const std::string &key)
{
    fulla::test::social::LoginBrowser browser;
    const std::string path = "/api/auth/providers/" + key + "/authorize";
    auto response = browser.send(path);
    if (!response || response->statusCode() != k204NoContent)
        return std::nullopt;
    const auto binding = response->getCookie("fulla-social-binding-dev");
    if (binding.value().empty())
        return std::nullopt;
    browser.cookie = "fulla-social-binding-dev=" + binding.value();
    response = browser.send(path);
    if (!response || response->statusCode() != k200OK)
        return std::nullopt;
    auto json = response->getJsonObject();
    if (!json)
        return std::nullopt;
    return std::optional<Json::Value>(*json);
}

}  // namespace

DROGON_TEST(Integration_P2_UpstreamRegistry_AdminCrudLifecycle)
{
    if (!fulla::test::http::postgresAvailable())
    {
        LOG_INFO << "Skipping P2 upstream registry CRUD (memory storage)";
        return;
    }
    auto token = fulla::test::http::loginAsAdmin();
    REQUIRE(token.has_value());

    const std::string key = "qa-upstream-a";
    auto cleanup = [&key, &token]() {
        fulla::test::http::sendDelete("/api/admin/upstream-providers/" + key, *token);
    };
    cleanup();  // idempotent: previous failed runs may have left the row

    // Create: pinned endpoints (hermetic -- no discovery, no live upstream).
    Json::Value body;
    body["provider_key"] = key;
    body["display_name"] = "QA Upstream";
    body["issuer"] = "https://upstream.invalid";
    body["client_id"] = "qa-client-id";
    body["client_secret"] = "qa-client-secret";
    body["endpoint_overrides"] =
      "{\"authorize\":\"https://upstream.invalid/authorize\","
      "\"token\":\"https://upstream.invalid/token\","
      "\"userinfo\":\"https://upstream.invalid/userinfo\"}";
    auto createResp = fulla::test::http::sendPostJson("/api/admin/upstream-providers", body, *token);
    REQUIRE(createResp != nullptr);
    CHECK(createResp->getStatusCode() == drogon::k201Created);
    auto created = createResp->getJsonObject();
    REQUIRE(created);
    CHECK((*created)["provider_key"].asString() == key);
    CHECK((*created)["client_secret_set"].asBool());
    // The secret itself is never echoed.
    CHECK(createResp->getBody().find("qa-client-secret") == std::string::npos);

    // List + get.
    auto listResp = fulla::test::http::sendGet("/api/admin/upstream-providers", *token);
    REQUIRE(listResp != nullptr);
    CHECK(listResp->getStatusCode() == drogon::k200OK);
    auto listJson = listResp->getJsonObject();
    REQUIRE(listJson);
    Json::Value row;
    CHECK(providerRowMatches((*listJson)["providers"], key, row));
    CHECK(row.get("issuer", "").asString() == "https://upstream.invalid");
    CHECK(listResp->getBody().find("qa-client-secret") == std::string::npos);

    auto getResp = fulla::test::http::sendGet("/api/admin/upstream-providers/" + key, *token);
    REQUIRE(getResp != nullptr);
    CHECK(getResp->getStatusCode() == drogon::k200OK);

    // Duplicate key -> conflict.
    auto dupResp = fulla::test::http::sendPostJson("/api/admin/upstream-providers", body, *token);
    REQUIRE(dupResp != nullptr);
    CHECK(dupResp->getStatusCode() == drogon::k409Conflict);

    // Reserved social keys are rejected upfront.
    Json::Value reserved = body;
    reserved["provider_key"] = "google";
    auto reservedResp =
      fulla::test::http::sendPostJson("/api/admin/upstream-providers", reserved, *token);
    REQUIRE(reservedResp != nullptr);
    CHECK(reservedResp->getStatusCode() == drogon::k400BadRequest);

    // Update: display_name change; empty client_secret keeps the current one.
    Json::Value update;
    update["display_name"] = "QA Upstream Renamed";
    update["issuer"] = "https://upstream.invalid";
    update["client_id"] = "qa-client-id";
    update["client_secret"] = "";
    update["enabled"] = true;
    auto updateResp =
      fulla::test::http::sendPutJson("/api/admin/upstream-providers/" + key, update, *token);
    REQUIRE(updateResp != nullptr);
    CHECK(updateResp->getStatusCode() == drogon::k200OK);
    auto renamed = fulla::test::http::sendGet("/api/admin/upstream-providers/" + key, *token);
    REQUIRE(renamed != nullptr);
    auto renamedJson = renamed->getJsonObject();
    REQUIRE(renamedJson);
    CHECK((*renamedJson)["display_name"].asString() == "QA Upstream Renamed");
    CHECK((*renamedJson)["client_secret_set"].asBool());

    // Login page merges the registry entry (public listing, no auth).
    auto pageResp = fulla::test::http::sendGet("/api/auth/providers");
    REQUIRE(pageResp != nullptr);
    auto pageJson = pageResp->getJsonObject();
    REQUIRE(pageJson);
    bool found = false;
    for (const auto &provider : (*pageJson)["providers"])
    {
        if (provider.get("provider", "").asString() == key)
        {
            found = true;
            CHECK(provider.get("display_name", "").asString() == "QA Upstream Renamed");
            CHECK(provider.get("type", "").asString() == "oidc");
        }
    }
    CHECK(found);

    // Authorize transaction for the registry provider rides the production
    // guard: state issued, URL built from the pinned endpoint.
    auto transaction = authorizeTransaction(key);
    REQUIRE(transaction.has_value());
    CHECK((*transaction)["state"].asString().size() == 64);
    const std::string authorizeUrl = (*transaction)["authorize_url"].asString();
    CHECK(authorizeUrl.rfind("https://upstream.invalid/authorize", 0) == 0);
    CHECK(authorizeUrl.find("client_id=qa-client-id") != std::string::npos);
    CHECK(authorizeUrl.find("state=" + (*transaction)["state"].asString()) != std::string::npos);

    // Delete -> gone everywhere.
    auto deleteResp =
      fulla::test::http::sendDelete("/api/admin/upstream-providers/" + key, *token);
    REQUIRE(deleteResp != nullptr);
    CHECK(deleteResp->getStatusCode() == drogon::k200OK);
    auto gone = fulla::test::http::sendGet("/api/admin/upstream-providers/" + key, *token);
    REQUIRE(gone != nullptr);
    CHECK(gone->getStatusCode() == drogon::k404NotFound);
    auto pageAfter = fulla::test::http::sendGet("/api/auth/providers");
    REQUIRE(pageAfter != nullptr);
    auto pageAfterJson = pageAfter->getJsonObject();
    REQUIRE(pageAfterJson);
    bool stillThere = false;
    for (const auto &provider : (*pageAfterJson)["providers"])
    {
        if (provider.get("provider", "").asString() == key)
            stillThere = true;
    }
    CHECK(!stillThere);
}

DROGON_TEST(Integration_P2_UpstreamRegistry_LoginExchangeValidation)
{
    // Guard-level validation of the login-exchange endpoint: the state/origin
    // machinery must refuse registry logins exactly like social logins
    // (without contacting any upstream).
    if (!fulla::test::http::postgresAvailable())
    {
        LOG_INFO << "Skipping P2 upstream login validation (memory storage)";
        return;
    }
    auto token = fulla::test::http::loginAsAdmin();
    REQUIRE(token.has_value());

    const std::string key = "qa-upstream-b";
    auto cleanup = [&key, &token]() {
        fulla::test::http::sendDelete("/api/admin/upstream-providers/" + key, *token);
    };
    cleanup();

    Json::Value body;
    body["provider_key"] = key;
    body["display_name"] = "QA Upstream B";
    body["issuer"] = "https://upstream.invalid";
    body["client_id"] = "qa-b";
    body["client_secret"] = "qa-b-secret";
    body["endpoint_overrides"] =
      "{\"authorize\":\"https://upstream.invalid/authorize\","
      "\"token\":\"https://upstream.invalid/token\","
      "\"userinfo\":\"https://upstream.invalid/userinfo\"}";
    auto createResp = fulla::test::http::sendPostJson("/api/admin/upstream-providers", body, *token);
    REQUIRE(createResp != nullptr);
    CHECK(createResp->getStatusCode() == drogon::k201Created);

    const std::string path = "/api/auth/oidc/" + key + "/login";

    // Unknown provider -> 404 before any upstream contact.
    fulla::test::social::LoginBrowser stranger;
    REQUIRE(stranger.begin(key));
    auto unknown = stranger.send("/api/auth/oidc/definitely-not-registered/login",
                                 "{\"code\":\"x\",\"state\":\"" + stranger.state + "\"}");
    REQUIRE(unknown != nullptr);
    CHECK(unknown->getStatusCode() == drogon::k404NotFound);

    // Cross-site origin refused.
    auto crossSite = stranger.send(path, "{\"code\":\"x\",\"state\":\"" + stranger.state + "\"}",
                                   "application/json", "https://attacker.test");
    REQUIRE(crossSite != nullptr);
    CHECK(crossSite->getStatusCode() == drogon::k403Forbidden);

    // Replayed / mismatched state refused (a different transaction's state).
    fulla::test::social::LoginBrowser other;
    REQUIRE(other.begin(key));
    auto mismatched = stranger.send(path, "{\"code\":\"x\",\"state\":\"" + other.state + "\"}");
    REQUIRE(mismatched != nullptr);
    CHECK(mismatched->getStatusCode() == drogon::k400BadRequest);

    cleanup();
}

// ---------------------------------------------------------------------------
// P2-1c: the three legacy social keys migrate onto the registry. A row can
// ENABLE a provider the config never configured (the CI config carries no
// social credentials, so github starts absent everywhere); admin PUT can
// then disable it at runtime. Admin CREATE still rejects the reserved keys
// -- rows are born from the startup seed or (here, for test setup) direct
// SQL.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P2_UpstreamRegistry_LegacySocialRowDrivesStaticSurface)
{
    if (!fulla::test::http::postgresAvailable())
    {
        LOG_INFO << "Skipping P2 legacy-social registry test (memory storage)";
        return;
    }
    auto db = ::drogon::app().getDbClient();
    if (!db)
    {
        LOG_INFO << "Skipping P2 legacy-social registry test (no DB client)";
        return;
    }
    auto token = fulla::test::http::loginAsAdmin();
    REQUIRE(token.has_value());

    // Reset leftovers from prior (possibly failed) runs FIRST -- the github
    // row is (re)created below and must survive until the case's own end.
    auto cleanup = [&token]() {
        fulla::test::http::sendDelete("/api/admin/upstream-providers/qa-refresh-dummy", *token);
        fulla::test::http::sendDelete("/api/admin/upstream-providers/github", *token);
    };
    cleanup();

    // Setup: a github row, enabled, with known credentials. The admin API
    // refuses to CREATE reserved keys, so tests seed via SQL (the startup
    // seed is the production birth path).
    std::promise<bool> seeded;
    db->execSqlAsync(
      "INSERT INTO upstream_providers (provider_key, display_name, issuer, client_id, "
      "client_secret, scopes, endpoint_overrides, enabled) VALUES "
      "('github', 'GitHub', 'https://github.com', 'row-client-id', 'row-client-secret', "
      "'user:email', '{}', true) ON CONFLICT (provider_key) DO UPDATE "
      "SET client_id = 'row-client-id', client_secret = 'row-client-secret', enabled = true",
      [&seeded](const ::drogon::orm::Result &) { seeded.set_value(true); },
      [&seeded](const ::drogon::orm::DrogonDbException &) { seeded.set_value(false); });
    REQUIRE(seeded.get_future().get());

    // Trigger a runtime refresh through the admin surface (any registry
    // mutation refreshes; create+delete of a dummy is the lightest one).
    {
        Json::Value dummy;
        dummy["provider_key"] = "qa-refresh-dummy";
        dummy["display_name"] = "Refresh Trigger";
        dummy["issuer"] = "https://refresh.invalid";
        dummy["client_id"] = "x";
        dummy["client_secret"] = "y";
        REQUIRE(fulla::test::http::sendPostJson("/api/admin/upstream-providers", dummy, *token)
                  ->getStatusCode() == drogon::k201Created);
    }

    // Login page advertises github (it was NOT configured in config.json).
    bool listed = false;
    {
        auto pageResp = fulla::test::http::sendGet("/api/auth/providers");
        REQUIRE(pageResp != nullptr);
        auto json = pageResp->getJsonObject();
        REQUIRE(json);
        for (const auto &provider : (*json)["providers"])
        {
            if (provider.get("provider", "").asString() == "github")
                listed = true;
        }
    }
    CHECK(listed);

    // The authorize transaction rides the STATIC surface with the ROW's
    // credentials and the pinned GitHub URL shape.
    {
        auto transaction = authorizeTransaction("github");
        REQUIRE(transaction.has_value());
        const std::string authorizeUrl = (*transaction)["authorize_url"].asString();
        CHECK(authorizeUrl.rfind(
                "https://github.com/login/oauth/authorize?client_id=row-client-id", 0) == 0);
        CHECK(authorizeUrl.find("&scope=user%3Aemail&state=") != std::string::npos);
    }

    // Admin PUT disables the row at runtime: listing drops it and the
    // authorize endpoint answers 404 -- no restart anywhere.
    {
        Json::Value update;
        update["display_name"] = "GitHub";
        update["issuer"] = "https://github.com";
        update["client_id"] = "row-client-id";
        update["client_secret"] = "";  // keep current
        update["enabled"] = false;
        auto updateResp =
          fulla::test::http::sendPutJson("/api/admin/upstream-providers/github", update, *token);
        REQUIRE(updateResp != nullptr);
        CHECK(updateResp->getStatusCode() == drogon::k200OK);

        auto pageResp = fulla::test::http::sendGet("/api/auth/providers");
        REQUIRE(pageResp != nullptr);
        auto json = pageResp->getJsonObject();
        REQUIRE(json);
        bool stillThere = false;
        for (const auto &provider : (*json)["providers"])
        {
            if (provider.get("provider", "").asString() == "github")
                stillThere = true;
        }
        CHECK(!stillThere);

        fulla::test::social::LoginBrowser browser;
        auto response = browser.send("/api/auth/providers/github/authorize");
        REQUIRE(response != nullptr);
        CHECK(response->getStatusCode() == drogon::k404NotFound);
    }

    // Admin CREATE still rejects reserved keys.
    {
        Json::Value reserved;
        reserved["provider_key"] = "github";
        reserved["display_name"] = "Dup";
        reserved["issuer"] = "https://github.com";
        reserved["client_id"] = "x";
        reserved["client_secret"] = "y";
        auto resp = fulla::test::http::sendPostJson("/api/admin/upstream-providers", reserved, *token);
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() == drogon::k400BadRequest);
    }

    cleanup();
}
