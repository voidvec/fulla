#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <future>
#include <chrono>

DROGON_TEST(Integration_P0_RefreshToken_NormalRotation)
{
    // Test normal refresh: old RT revoked, new RT works
    auto plugin = std::make_shared<OAuth2Plugin>();
    Json::Value config;
    config["storage_type"] = "memory";

    Json::Value clientConfig;
    clientConfig["type"] = "PUBLIC";
    clientConfig["secret"] = "test-secret";
    clientConfig["redirect_uri"] = "http://localhost/cb";
    config["clients"]["test-client"] = clientConfig;

    plugin->initAndStart(config);

    // Generate initial tokens via auth code flow
    std::string authCode;
    {
        std::promise<std::string> p;
        auto f = p.get_future();
        plugin->generateAuthorizationCode(
          "test-client",
          "user1",
          "openid",
          "http://localhost/cb",
          "",
          "",
          "",  // nonce
          [&](bool success, std::string code, std::string /*error*/) {
              if (success)
                  p.set_value(code);
              else
                  p.set_value("");
          }
        );
        if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
        {
            throw std::runtime_error("TIMEOUT generating auth code");
        }
        authCode = f.get();
        CHECK(authCode.length() > 0);
    }

    // Exchange code for tokens
    Json::Value tokenResult;
    {
        std::promise<Json::Value> p;
        auto f = p.get_future();
        plugin->exchangeCodeForToken(
          authCode, "test-client", "", "http://localhost/cb", "", [&](const Json::Value &result) {
              p.set_value(result);
          }
        );
        if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
        {
            throw std::runtime_error("TIMEOUT exchanging code");
        }
        tokenResult = f.get();
        CHECK(tokenResult.isMember("refresh_token"));
    }

    std::string rt1 = tokenResult["refresh_token"].asString();
    CHECK(rt1.length() > 0);

    // Refresh once - should succeed
    Json::Value refresh1;
    {
        std::promise<Json::Value> p;
        auto f = p.get_future();
        plugin->refreshAccessToken(rt1, "test-client", [&](const Json::Value &r) {
            p.set_value(r);
        });
        if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
        {
            throw std::runtime_error("TIMEOUT refreshing token");
        }
        refresh1 = f.get();
        CHECK(!refresh1.isMember("error"));
        CHECK(refresh1.isMember("refresh_token"));
    }

    std::string rt2 = refresh1["refresh_token"].asString();
    CHECK(rt2 != rt1);  // New RT issued (rotation)

    // A second normal rotation succeeds before any reuse is observed.
    std::string rt3;
    {
        std::promise<Json::Value> p;
        auto f = p.get_future();
        plugin->refreshAccessToken(rt2, "test-client", [&](const Json::Value &r) {
            p.set_value(r);
        });
        if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
        {
            throw std::runtime_error("TIMEOUT refreshing with rt2");
        }
        auto refresh2 = f.get();
        CHECK(!refresh2.isMember("error"));
        CHECK(refresh2.isMember("access_token"));
        CHECK(refresh2.isMember("refresh_token"));
        rt3 = refresh2["refresh_token"].asString();
        CHECK(!rt3.empty());
        CHECK(rt3 != rt2);  // Rotated again
    }

    // Use old RT again - should fail (reuse detection)
    {
        std::promise<Json::Value> p;
        auto f = p.get_future();
        plugin->refreshAccessToken(rt1, "test-client", [&](const Json::Value &r) {
            p.set_value(r);
        });
        if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
        {
            throw std::runtime_error("TIMEOUT on reuse detection");
        }
        auto reuseResult = f.get();
        CHECK(reuseResult.isMember("error"));
        CHECK(reuseResult["error"].asString() == "invalid_grant");
    }

    // Strict reuse detection revokes even the latest, previously unused RT.
    {
        std::promise<Json::Value> p;
        auto f = p.get_future();
        plugin->refreshAccessToken(rt3, "test-client", [&](const Json::Value &r) {
            p.set_value(r);
        });
        if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            throw std::runtime_error("TIMEOUT refreshing revoked family");
        auto rejected = f.get();
        CHECK(rejected["error"].asString() == "invalid_grant");
        CHECK(!rejected.isMember("access_token"));
        CHECK(!rejected.isMember("refresh_token"));
    }
}
