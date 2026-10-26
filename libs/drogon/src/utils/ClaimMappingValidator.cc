#include <fulla/drogon/utils/ClaimMappingValidator.h>

#include <json/json.h>

#include <cctype>
#include <functional>
#include <sstream>
#include <regex>
#include <set>

namespace fulla::drogon::utils
{

namespace
{
// The reserved-word blacklist (design §P1-3 rev.1): every standard claim
// the server emits today on id_token or userinfo, plus the OIDC reserved
// set — a mapping may never shadow any of them.
const std::set<std::string> &reservedClaimNames()
{
    static const std::set<std::string> kReserved = {
      "sub", "iss", "aud", "exp", "iat", "nonce", "auth_time", "acr", "amr",
      "org_ctx", "email", "email_verified", "name", "preferred_username",
      "family_name", "given_name", "nickname", "picture", "website",
      "phone_number", "phone_number_verified", "username", "roles"};
    return kReserved;
}

const std::regex &customAttributeKeyRegex()
{
    static const std::regex kKey("^[a-z][a-z0-9_]{0,63}$");
    return kKey;
}

bool isValidIncludeIn(const std::string &v)
{
    return v == "id_token" || v == "userinfo" || v == "both";
}
}  // namespace

std::string validateClaimMapping(
  const std::string &scopeName,
  const std::string &claimName,
  const std::string &sourcePath,
  const std::string &includeIn
)
{
    if (scopeName.empty() || scopeName.size() > 64)
        return "scope_name must be 1..64 chars";
    if (scopeName == "openid")
        return "scope_name 'openid' is forbidden (always granted; a mapping on "
               "it would be zero-authorization publication)";
    if (claimName.empty() || claimName.size() > 64)
        return "claim_name must be 1..64 chars";
    if (!std::regex_match(claimName, std::regex("^[a-z][a-z0-9_]{0,63}$")))
        return "claim_name must match ^[a-z][a-z0-9_]{0,63}$";
    if (reservedClaimNames().count(claimName) > 0)
        return "claim_name '" + claimName + "' is reserved";
    if (!isValidIncludeIn(includeIn))
        return "include_in must be one of id_token | userinfo | both";

    // Source whitelist: custom_attributes.<key> with the self-service key
    // grammar, or the single built-in user.display_name.
    if (sourcePath == "user.display_name")
        return "";
    static const std::string kPrefix = "custom_attributes.";
    if (sourcePath.rfind(kPrefix, 0) == 0)
    {
        std::string key = sourcePath.substr(kPrefix.size());
        if (std::regex_match(key, customAttributeKeyRegex()))
            return "";
    }
    return "source_path must be 'user.display_name' or 'custom_attributes.<key>' "
           "with the self-service key grammar";
}

namespace
{
bool nodeDepthOk(const Json::Value &v, int depth, int &maxSeen)
{
    if (depth > maxSeen)
        maxSeen = depth;
    if (v.isObject())
    {
        for (const auto &k : v.getMemberNames())
        {
            if (!nodeDepthOk(v[k], depth + 1, maxSeen))
                return false;
        }
    }
    return maxSeen <= 2;  // root=0, one nesting level=1, leaf objects=2
}
}  // namespace

std::string validateCustomAttributes(const std::string &jsonDocument)
{
    if (jsonDocument.size() > 8192)
        return "custom attributes exceed the 8 KiB limit";
    Json::CharReaderBuilder rb;
    std::string errs;
    Json::Value root;
    std::istringstream in(jsonDocument);
    if (!Json::parseFromStream(rb, in, &root, &errs))
        return "custom attributes must be a JSON object";
    if (!root.isObject())
        return "custom attributes must be a JSON object";

    // Depth: root counts as 0; values may nest ONE object level
    // ({"prefs": {"theme": "dark"}}). Arrays are containers too.
    int maxSeen = 0;
    if (!nodeDepthOk(root, 0, maxSeen))
        return "custom attributes depth must be <= 2";

    std::function<bool(const Json::Value &)> keysOk = [&](const Json::Value &v) -> bool {
        if (v.isObject())
        {
            for (const auto &k : v.getMemberNames())
            {
                if (k == "__proto__" || k == "constructor" || k == "prototype")
                    return false;
                if (!std::regex_match(k, customAttributeKeyRegex()))
                    return false;
                if (!keysOk(v[k]))
                    return false;
            }
        }
        else if (v.isArray())
        {
            for (const auto &e : v)
                if (!keysOk(e))
                    return false;
        }
        return true;
    };
    if (!keysOk(root))
        return "custom attribute keys must match ^[a-z][a-z0-9_]{0,63}$ and must "
               "not be __proto__/constructor/prototype";
    return "";
}

bool resolveClaimSourceValue(
  const std::string &sourcePath,
  const std::string &userJson,
  std::string &outValue
)
{
    outValue.clear();
    Json::CharReaderBuilder rb;
    Json::Value root;
    std::istringstream in(userJson);
    std::string errs;
    if (!Json::parseFromStream(rb, in, &root, &errs) || !root.isObject())
        return false;

    Json::Value node;
    if (sourcePath == "user.display_name")
    {
        node = root["display_name"];
    }
    else
    {
        static const std::string kPrefix = "custom_attributes.";
        if (sourcePath.rfind(kPrefix, 0) != 0)
            return false;
        std::string key = sourcePath.substr(kPrefix.size());
        const Json::Value &attrs = root["custom_attributes"];
        if (!attrs.isObject() || !attrs.isMember(key))
            return false;
        node = attrs[key];
    }
    // Scalars only — objects/arrays never publish (structured claims are a
    // follow-up; publishing raw JSON invites schema confusion).
    if (node.isString())
    {
        outValue = node.asString();
        return !outValue.empty();
    }
    if (node.isIntegral())
    {
        outValue = std::to_string(node.asInt64());
        return true;
    }
    if (node.isNumeric())
    {
        outValue = std::to_string(node.asDouble());
        return true;
    }
    if (node.isBool())
    {
        outValue = node.asBool() ? "true" : "false";
        return true;
    }
    return false;
}

}  // namespace fulla::drogon::utils
