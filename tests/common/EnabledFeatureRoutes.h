#pragma once

#include <sstream>
#include <string>

namespace fulla::test::features
{
inline bool pathWithin(const std::string &path, const std::string &prefix)
{
    return path == prefix ||
           (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 &&
            path[prefix.size()] == '/');
}

inline bool routeEnabled(const std::string &path)
{
    (void)path;
#ifndef WITH_SOCIAL
    for (const char *prefix : {"/api/google", "/api/wechat", "/api/github", "/api/me/social/links"})
    {
        if (pathWithin(path, prefix))
            return false;
    }
#endif
#ifndef WITH_WEBAUTHN
    if (pathWithin(path, "/api/me/webauthn") || pathWithin(path, "/oauth2/webauthn"))
        return false;
#endif
    return true;
}

// Apply only to the frozen EXPECTED set. Filtering actual registrations
// would hide a disabled controller accidentally linked into the product.
inline std::string expectedRoutes(const std::string &baseline)
{
#if defined(WITH_SOCIAL) && defined(WITH_WEBAUTHN)
    return baseline;
#else
    std::istringstream input(baseline);
    std::string line;
    std::string result;
    while (std::getline(input, line))
    {
        const auto space = line.find(' ');
        if (space == std::string::npos || routeEnabled(line.substr(space + 1)))
            result += line + '\n';
    }
    return result;
#endif
}
}  // namespace fulla::test::features
