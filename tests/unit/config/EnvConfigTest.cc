#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <iostream>
#include <fstream>
#include <json/json.h>
#include <cstdlib>
#include <sstream>

// Helper to parse JSON (replaces deprecated Json::Reader)
static bool parseJsonString(std::istream &stream, Json::Value &json)
{
    Json::CharReaderBuilder builder;
    std::string errs;
    return Json::parseFromStream(builder, stream, &json, &errs);
}

using namespace drogon;

DROGON_TEST(Unit_P1_Config_EnvInjection_Works)
{
    // Only run this verify if we specifically signaled it via ENV
    // This allows normal tests to pass without needing these specific ENVs
    const char *flag = std::getenv("FULLA_ENV_TEST_FLAG");
    if (!flag)
    {
        LOG_INFO << "Skipping EnvInjectionVerify (FULLA_ENV_TEST_FLAG not set)";
        return;
    }

    // Verify by reading the generated runtime config file directly
    // checking app().getCustomConfig() is insufficient as it only returns
    // "custom_config" section
    std::ifstream configFile("test_config_env_runtime.json");
    if (!configFile.is_open())
    {
        FAIL("Could not open test_config_env_runtime.json");
    }

    Json::Value config;
    if (!parseJsonString(configFile, config))
    {
        FAIL("Failed to parse test_config_env_runtime.json");
    }

    // Check DB Name Override
    const char *expectedDbName = std::getenv("FULLA_DB_NAME");
    if (expectedDbName)
    {
        if (config.isMember("db_clients") && config["db_clients"].size() > 0)
        {
            std::string actual = config["db_clients"][0]["dbname"].asString();
            CHECK(actual == expectedDbName);
        }
        else
        {
            FAIL("db_clients missing in generated config");
        }
    }

    // Check Portal Client Secret Override (new primary name, legacy alias)
    const char *expectedSecret = std::getenv("FULLA_PORTAL_CLIENT_SECRET");
    if (!expectedSecret || expectedSecret[0] == 0)
        expectedSecret = std::getenv("FULLA_VUE_CLIENT_SECRET");
    if (expectedSecret)
    {
        bool found = false;
        if (config.isMember("plugins"))
        {
            for (const auto &plugin : config["plugins"])
            {
                if (plugin.get("name", "").asString() == "OAuth2Plugin")
                {
                    std::string actual =
                      plugin["config"]["clients"]["fulla-portal"]["secret"].asString();
                    CHECK(actual == expectedSecret);
                    LOG_INFO << "Verified Client Secret: " << actual;
                    found = true;
                    break;
                }
            }
        }
        CHECK(found == true);
    }
}
