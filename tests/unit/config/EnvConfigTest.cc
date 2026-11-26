#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <iostream>
#include <fstream>
#include <json/json.h>
#include <fulla/common/config/ConfigManager.h>
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
    const char *expectedDbName = fulla::common::config::ConfigManager::getEnv("FULLA_DB_NAME");
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

    const char *expectedRedisPortValue =
      fulla::common::config::ConfigManager::getEnv("FULLA_REDIS_PORT");
    int expectedRedisPort = 0;
    if (expectedRedisPortValue)
    {
        try
        {
            expectedRedisPort = std::stoi(expectedRedisPortValue);
        }
        catch (...)
        {
            expectedRedisPort = 0;
        }
    }
    if (expectedRedisPortValue)
    {
        REQUIRE(config["redis_clients"].isArray());
        if (config["redis_clients"].empty())
        {
            bool memoryStorage = false;
            if (config["plugins"].isArray())
            {
                for (const auto &plugin : config["plugins"])
                {
                    if (plugin.get("name", "").asString() == "OAuth2Plugin" &&
                        plugin["config"].get("storage_type", "").asString() == "memory")
                    {
                        memoryStorage = true;
                        break;
                    }
                }
            }
            CHECK(memoryStorage);
        }
        else
        {
            CHECK(config["redis_clients"][0]["port"].asInt() == expectedRedisPort);
        }
    }

    // Check Portal Client Secret Override (new primary name, legacy alias)
    const char *expectedSecret =
      fulla::common::config::ConfigManager::getEnv("FULLA_PORTAL_CLIENT_SECRET");
    if (!expectedSecret || expectedSecret[0] == 0)
        expectedSecret = fulla::common::config::ConfigManager::getEnv("FULLA_VUE_CLIENT_SECRET");
    if (expectedSecret && expectedSecret[0] != 0)
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
                    found = true;
                    break;
                }
            }
        }
        CHECK(found == true);
    }
}
