#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/utils/CryptoUtils.h>

#include <chrono>
#include <future>
#include <memory>
#include <string>

#include "bootstrap/ClientSeeder.h"  // apps/server/src on the include path

// #204: the OAuth2Plugin config's "clients" block must reach the database —
// production had no config->DB path (the dev seed SQL is DEV-ONLY), so login
// passed credential + email-verified checks and then failed code issuance
// with 3001 (the P0-4 client-existence guard). These cases exercise
// bootstrap::ClientSeeder directly against the real database:
//   * config clients are seeded with PUBLIC/none + their scope grants;
//   * a second run leaves the existing row untouched (no secret clobber,
//     no duplicate grants) — runtime/admin edits stay authoritative.
DROGON_TEST(Integration_P0_ClientSeeder_ConfigClients_Seeded_Idempotent)
{
    // Memory-storage guard: clients there init from config in-process.
    auto plugin = drogon::app().getPlugin<OAuth2Plugin>();
    if (plugin && plugin->getStorageType() == "memory")
    {
        LOG_INFO << "Skipping ClientSeeder test in memory storage mode";
        return;
    }
    auto dbClient = drogon::app().getDbClient();
    if (!dbClient)
    {
        LOG_WARN << "DB client not available. Skipping ClientSeeder test.";
        return;
    }

    Json::Value clients(Json::objectValue);
    Json::Value portal;
    portal["client_type"] = "PUBLIC";
    portal["redirect_uri"] = "http://127.0.0.1:5173/callback";
    portal["allowed_scopes"] = Json::Value(Json::arrayValue);
    portal["allowed_scopes"].append("openid");
    portal["allowed_scopes"].append("profile");
    portal["allowed_scopes"].append("email");
    clients["fulla-portal"] = portal;

    Json::Value admin;
    admin["client_type"] = "PUBLIC";
    admin["redirect_uri"] = "http://127.0.0.1:5174/admin/callback";
    admin["allowed_scopes"] = Json::Value(Json::arrayValue);
    admin["allowed_scopes"].append("openid");
    admin["allowed_scopes"].append("profile");
    admin["allowed_scopes"].append("admin");
    clients["fulla-admin-console"] = admin;

    auto seedOnce = [&clients]() -> bool {
        auto result = std::make_shared<std::promise<bool>>();
        bootstrap::ClientSeeder::run(
          clients,
          [result](bool ok, const std::string &detail) {
              LOG_INFO << "ClientSeeder test pass: ok=" << ok << " (" << detail << ")";
              result->set_value(ok);
          });
        return result->get_future().get();
    };

    // 1) First pass reports success.
    CHECK(seedOnce());

    // 2) The portal client row exists as PUBLIC with the none auth method.
    auto row = dbClient->execSqlSync(
      "SELECT client_type, token_endpoint_auth_method, client_secret "
      "FROM oauth2_clients WHERE client_id = 'fulla-portal'");
    REQUIRE(row.size() == 1);
    CHECK(row[0]["client_type"].as<std::string>() == "PUBLIC");
    CHECK(row[0]["token_endpoint_auth_method"].as<std::string>() == "none");
    const auto secretAfterFirstRun = row[0]["client_secret"].as<std::string>();

    // 3) Scope grants exist (only registry scopes are granted).
    auto grants = dbClient->execSqlSync(
      "SELECT scope_name FROM oauth2_client_scopes WHERE client_id = 'fulla-portal' "
      "ORDER BY scope_name");
    REQUIRE(grants.size() == 3);
    CHECK(grants[0]["scope_name"].as<std::string>() == "email");
    CHECK(grants[1]["scope_name"].as<std::string>() == "openid");
    CHECK(grants[2]["scope_name"].as<std::string>() == "profile");

    // 4) Second pass is a no-op on the row: ON CONFLICT DO NOTHING means the
    // admin-visible secret hash is never clobbered by a restart.
    CHECK(seedOnce());
    auto rowAgain = dbClient->execSqlSync(
      "SELECT client_secret FROM oauth2_clients WHERE client_id = 'fulla-portal'");
    REQUIRE(rowAgain.size() == 1);
    CHECK(rowAgain[0]["client_secret"].as<std::string>() == secretAfterFirstRun);
}

// The seeder honors an explicit token_endpoint_auth_method
// in the config block. It used to ignore the key and always write
// client_secret_basic for CONFIDENTIAL clients, so a relying party that
// authenticates with a body secret could not be seeded from config at all.
// The combination rule is enforced as well: a method that contradicts the
// client type is reported and that client is not seeded.
DROGON_TEST(Integration_P1_ClientSeeder_ExplicitAuthMethod_Seeded_IllegalCombinationRefused)
{
    auto plugin = drogon::app().getPlugin<OAuth2Plugin>();
    if (plugin && plugin->getStorageType() == "memory")
    {
        LOG_INFO << "Skipping ClientSeeder test in memory storage mode";
        return;
    }
    auto dbClient = drogon::app().getDbClient();
    if (!dbClient)
    {
        LOG_WARN << "DB client not available. Skipping ClientSeeder test.";
        return;
    }

    const std::string suffix = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()
    );
    const std::string postClientId = "seeder-post-" + suffix;
    const std::string illegalClientId = "seeder-illegal-" + suffix;

    auto seed = [](const Json::Value &clients) {
        auto result = std::make_shared<std::promise<std::pair<bool, std::string>>>();
        bootstrap::ClientSeeder::run(
          clients,
          [result](bool ok, const std::string &detail) {
              result->set_value(std::make_pair(ok, detail));
          });
        return result->get_future().get();
    };

    // 1) CONFIDENTIAL + an explicit client_secret_post: seeded with it.
    {
        Json::Value clients(Json::objectValue);
        Json::Value rp;
        rp["client_type"] = "CONFIDENTIAL";
        rp["secret"] = fulla::drogon::utils::generateSecureToken(16);
        rp["redirect_uri"] = "http://127.0.0.1:9999/callback";
        rp["token_endpoint_auth_method"] = "client_secret_post";
        clients[postClientId] = rp;

        const auto seeded = seed(clients);
        CHECK(seeded.first);
        auto row = dbClient->execSqlSync(
          "SELECT client_type, token_endpoint_auth_method FROM oauth2_clients "
          "WHERE client_id = $1",
          postClientId);
        REQUIRE(row.size() == 1);
        CHECK(row[0]["client_type"].as<std::string>() == "CONFIDENTIAL");
        CHECK(row[0]["token_endpoint_auth_method"].as<std::string>() == "client_secret_post");
        dbClient->execSqlSync("DELETE FROM oauth2_clients WHERE client_id = $1", postClientId);
    }

    // 2) PUBLIC + client_secret_post contradicts the type: reported, and the
    //    row is absent (the server itself keeps starting).
    {
        Json::Value clients(Json::objectValue);
        Json::Value bad;
        bad["client_type"] = "PUBLIC";
        bad["token_endpoint_auth_method"] = "client_secret_post";
        clients[illegalClientId] = bad;

        const auto refused = seed(clients);
        CHECK(!refused.first);
        CHECK(refused.second.find("token_endpoint_auth_method") != std::string::npos);
        CHECK(refused.second.find(illegalClientId) != std::string::npos);
        auto row = dbClient->execSqlSync(
          "SELECT 1 FROM oauth2_clients WHERE client_id = $1", illegalClientId);
        CHECK(row.size() == 0);
    }
}
