#include <fulla/common/error/ErrorContext.h>
#include <fulla/common/config/ConfigManager.h>
#include <optional>
#include <string>

namespace fulla::common::error
{

namespace
{

// Test-injectable override. When set, it takes precedence over the build flag /
// environment variable detection. A function-local static avoids static
// initialization order issues and keeps the override process-wide.
std::optional<bool> &detailedErrorsOverride()
{
    static std::optional<bool> override;
    return override;
}

}  // namespace

bool ErrorContext::defaultDetailedErrorsAllowed()
{
#ifdef DEBUG
    return true;
#else
    // Production builds: only enable detailed errors when the operator opts in
    // via the DETAILED_VALIDATION_ERRORS switch. Like the other process-only
    // switches it is not read from a .env file.
    const char *env =
      fulla::common::config::ConfigManager::getProcessEnv("DETAILED_VALIDATION_ERRORS");
    return env && (std::string(env) == "1" || std::string(env) == "true");
#endif
}

bool ErrorContext::detailedErrorsAllowed()
{
    const auto &override = detailedErrorsOverride();
    if (override.has_value())
    {
        return *override;
    }
    return defaultDetailedErrorsAllowed();
}

void ErrorContext::setDetailedErrorsOverride(bool allowed)
{
    detailedErrorsOverride() = allowed;
}

void ErrorContext::clearDetailedErrorsOverride()
{
    detailedErrorsOverride().reset();
}

}  // namespace fulla::common::error
