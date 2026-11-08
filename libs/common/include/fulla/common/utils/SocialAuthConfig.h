#pragma once

#include <json/json.h>
#include <algorithm>
#include <cctype>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>

namespace fulla::common::utils
{

inline bool socialCredentialConfigured(const std::string &value)
{
    return !value.empty() && value.rfind("YOUR_", 0) != 0 && value.rfind("OVERRIDE_", 0) != 0;
}

inline std::string socialRedirectUri(const Json::Value &config, const std::string &provider)
{
    auto frontend = config["frontend"].get("url", "http://localhost:5173").asString();
    while (!frontend.empty() && frontend.back() == '/')
        frontend.pop_back();
    const auto overrideUri = config["external_auth"][provider].get("redirect_uri", "").asString();
    return overrideUri.empty() ? frontend + "/callback/" + provider : overrideUri;
}

inline std::pair<std::string, std::string> socialOriginAndPath(const std::string &uri)
{
    static const std::regex pattern(
      R"(^(https?)://(\[[0-9a-fA-F:]+\]|[a-zA-Z0-9.-]+)(:[0-9]{1,5})?(/[^?#]*)?(\?[^#]*)?$)");
    std::smatch match;
    if (!std::regex_match(uri, match, pattern) ||
        std::any_of(uri.begin(), uri.end(), [](unsigned char c) { return c <= 32 || c == 127 || c == '\\'; }))
        throw std::invalid_argument("Invalid social redirect URI");
    auto host = match[2].str();
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    auto port = match[3].str();
    if (!port.empty())
    {
        const auto number = std::stoi(port.substr(1));
        if (number == 0 || number > 65535)
            throw std::invalid_argument("Invalid social redirect port");
        port = ((match[1] == "http" && number == 80) || (match[1] == "https" && number == 443))
                 ? "" : ":" + std::to_string(number);
    }
    auto path = match[4].str().empty() ? "/" : match[4].str();
    // Require unambiguous callback paths across proxies, browsers and routers.
    if (path.find('%') != std::string::npos || path.find("//") != std::string::npos ||
        std::regex_search(path, std::regex(R"((^|/)\.{1,2}(/|$))")))
        throw std::invalid_argument("Social callback path must be canonical");
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return {match[1].str() + "://" + host + port, path};
}

}  // namespace fulla::common::utils
