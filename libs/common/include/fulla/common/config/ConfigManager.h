#pragma once

#include <string>
#include <type_traits>
#include <json/json.h>
#include <fulla/common/config/ConfigTypes.h>

namespace fulla::common::config
{

class ConfigManager
{
  public:
    // Load config file with environment variable overrides
    static bool load(const std::string &configPath, Json::Value &config);

    // Type-safe configuration access
    template <typename T>
    static T get(const Json::Value &config, const std::string &path, const T &defaultValue = T{});

    // Configuration validation
    static bool validate(const Json::Value &config, std::string &errorMessage);

    // Apply environment variable overrides
    static void applyEnvOverrides(Json::Value &config, const std::vector<EnvOverride> &rules);

    /**
     * @brief Get environment variable with .env file priority
     * Searches .env file first, then system environment.
     * @return value string or nullptr if not found
     */
    static const char *getEnv(const char *name);

    /**
     * @brief Get an environment variable from the PROCESS environment only.
     *
     * The .env file is NOT consulted. Use this for the few keys that must be
     * real environment variables (signing keys, bootstrap credentials, ...):
     * loadDotEnv() only parses the file into memory and never exports it, so
     * such a key used to be invisible to a raw getenv() -- with nothing
     * saying so. Every process-environment read in production code goes
     * through this accessor (arch-guard rule R5), which reports a key that
     * was put into .env anyway (the report names the key, never the value).
     * @return value string or nullptr if not set in the process environment
     */
    static const char *getProcessEnv(const char *name);

    /**
     * @brief Test seam: replace the in-memory .env table.
     *
     * Production code never calls this. Tests use it to put values into the
     * table without touching the working directory. These seams, like the
     * underlying table, require serial test execution with no in-flight requests;
     * returned strings are value copies, not stable c_str() pointers.
     */
    static void setDotEnvForTesting(
      const std::vector<std::pair<std::string, std::string>> &entries
    );

    static std::vector<std::pair<std::string, std::string>> getDotEnvForTesting();

  private:
    // Parse JSON path and return pointer to node
    static Json::Value *getJsonPointer(Json::Value &root, const std::string &path);

    // Parse integer from string
    static int parseInt(const std::string &str);
};

// Template implementations
template <typename T>
T ConfigManager::get(const Json::Value &config, const std::string &path, const T &defaultValue)
{
    Json::Value *ptr = getJsonPointer(const_cast<Json::Value &>(config), path);
    if (!ptr || ptr->isNull())
    {
        return defaultValue;
    }

    if constexpr (std::is_same_v<T, std::string>)
    {
        return ptr->asString();
    }
    else if constexpr (std::is_integral_v<T>)
    {
        return ptr->asInt();
    }
    else if constexpr (std::is_floating_point_v<T>)
    {
        return ptr->asDouble();
    }
    else if constexpr (std::is_same_v<T, bool>)
    {
        return ptr->asBool();
    }
    else
    {
        return defaultValue;
    }
}

}  // namespace fulla::common::config
