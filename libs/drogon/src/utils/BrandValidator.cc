#include <fulla/drogon/utils/BrandValidator.h>

#include <cctype>
#include <string>

namespace fulla::drogon::utils
{

namespace
{
std::string lowerAscii(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

bool isLoopbackHost(const std::string &host)
{
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]" || host == "::1";
}
}  // namespace

std::string validateBrandUri(const std::string &uri)
{
    if (uri.empty())
        return "";
    if (uri.size() > 512)
        return "brand URI exceeds 512 characters";

    if (uri.find('#') != std::string::npos)
        return "brand URI must not contain a fragment";

    const std::string lower = lowerAscii(uri);
    if (lower.rfind("https://", 0) != 0)
    {
        // http:// is tolerated ONLY for loopback hosts (dev deployments);
        // this is a brand-image rule, NOT the redirect registration rule,
        // and deliberately does not inherit allow_http_redirect_uri.
        if (lower.rfind("http://", 0) != 0)
            return "brand URI scheme must be https";
    }

    const std::size_t schemeEnd = uri.find("://");
    if (schemeEnd == std::string::npos)
        return "brand URI is not absolute";
    const std::size_t hostStart = schemeEnd + 3;
    const std::size_t pathStart = uri.find_first_of("/?#", hostStart);
    const std::string authority =
      uri.substr(hostStart, pathStart == std::string::npos ? std::string::npos
                                                           : pathStart - hostStart);
    if (authority.empty())
        return "brand URI has no host";

    const std::size_t at = authority.find('@');
    if (at != std::string::npos)
        return "brand URI must not contain userinfo";

    const std::size_t colon = authority.find(':');
    std::string host;
    if (!authority.empty() && authority[0] == '[')
    {
        // Bracketed IPv6 literal: the port (if any) follows ']' — a naive
        // first-colon split would truncate "[::1]" to "[". Keep the closing
        // bracket in the host (substr is end-exclusive).
        const std::size_t close = authority.find(']');
        host = lowerAscii(
          authority.substr(0, close == std::string::npos ? authority.size() : close + 1));
    }
    else
    {
        host = lowerAscii(colon == std::string::npos ? authority : authority.substr(0, colon));
    }

    if (lower.rfind("http://", 0) == 0 && !isLoopbackHost(host))
        return "http brand URIs are only allowed for loopback hosts";

    return "";
}

std::string validatePrimaryColor(const std::string &color)
{
    if (color.empty())
        return "";
    if (color.size() < 4 || color.size() > 7 || color[0] != '#')
        return "primary_color must match ^#[0-9a-fA-F]{3,6}$";
    for (std::size_t i = 1; i < color.size(); ++i)
    {
        const char c = color[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex)
            return "primary_color must match ^#[0-9a-fA-F]{3,6}$";
    }
    return "";
}

}  // namespace fulla::drogon::utils
