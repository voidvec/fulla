#pragma once

// M3 Task 25 (fulla-sdk-refactor, design.md §15 item 5 / evaluation
// L1/B1): relocated from OAuth2Plugin/include/oauth2/observability/openapi/
// OpenApiGenerator.h into fulla::drogon::observability::openapi.
//
// Deviation from the literal design.md wording (documented here and in
// PROGRESS.md): design.md's B1 decision said this class belongs in
// `apps/server` because it does not use Drogon route introspection
// (verified true -- it only calls LOG_INFO/LOG_ERROR from <drogon/drogon.h>
// and manually-constructed EndpointInfo/Json::Value, nothing
// Drogon-route-specific). That assessment was made before Task 20
// relocated all 15+ controllers that call OpenApiGenerator::addEndpoint()
// (their static-init EndpointDocs structs) into libs/drogon. Moving this
// class into apps/server (OAuth2Server, the top of the dependency graph)
// would make libs/drogon depend on the executable that depends on
// libs/drogon -- a circular/backwards dependency. Moving it into
// libs/drogon instead achieves the same underlying goal (out of
// OAuth2Plugin's pseudo-domain layer) while matching where its actual
// callers live; apps/server (main.cc's bootstrap::setupOpenApi(), see
// OAuth2Server/bootstrap/OpenApiSetup.cc) still owns configuring the
// server URL and writing the generated spec to disk -- only the endpoint
// registry itself moved.

#include <string>
#include <vector>
#include <map>
#include <json/json.h>

namespace fulla::drogon::observability::openapi
{

// Parameter types supported by OpenAPI
enum class ParameterType
{
    STRING,
    INTEGER,
    NUMBER,
    BOOLEAN,
    ARRAY,
    OBJECT
};

// Parameter locations
enum class ParameterLocation
{
    QUERY,
    HEADER,
    PATH,
    COOKIE
};

// Enhanced parameter information
struct ParameterInfo
{
    std::string name;
    std::string description;
    ParameterType type = ParameterType::STRING;
    ParameterLocation location = ParameterLocation::QUERY;
    bool required = true;
    std::string defaultValue;  // Optional default value
    std::string enumValues;    // Comma-separated enum values (optional)
    std::string format;        // OpenAPI format (e.g., "int64", "email", "uuid")
};

// Authentication style of an endpoint -- selects the OpenAPI security scheme
// emitted when requiresAuth == true.
enum class AuthType
{
    Bearer,            // Authorization: Bearer <user access token>
    ClientCredentials  // OAuth2 client authentication (RFC 6749 §2.3 /
                       // RFC 7662 / RFC 7009): client_id + client_secret via
                       // HTTP Basic or POST body parameters
};

struct EndpointInfo
{
    std::string path;
    std::string method;
    std::string summary;
    std::string description;
    std::vector<std::string> tags;
    std::vector<ParameterInfo> parameters;  // Changed to use ParameterInfo
    std::map<int, std::string> responses;
    std::map<int, Json::Value> responseExamples;  // NEW: Response examples
    bool requiresAuth;
    // Only meaningful when requiresAuth == true. Defaults to Bearer so
    // pre-existing registrations keep their generated output unchanged.
    AuthType authType = AuthType::Bearer;

    // Resource-scope authorization model (#43): the OAuth2 scopes a token
    // MUST carry (per ScopeMatch semantics, see authz/ScopeResolver.h) to
    // access this endpoint. Empty = no scope requirement at this layer
    // (the endpoint may still require a valid token + RBAC role). Consumed
    // by ResourceScopeRegistry at startup and emitted as the OpenAPI
    // x-required-scopes extension.
    std::vector<std::string> requiredScopes;
    // Super-scopes whose presence on the token satisfies this endpoint's
    // requirement even when the exact requiredScopes are absent (e.g. an
    // "admin" super-scope satisfies "users:read"). Per-requirement, NOT a
    // global graph -- avoids the hardcoded implication list.
    std::vector<std::string> impliedBy;
    // Optional explicit body contract for JSON/form operations.
    Json::Value requestBody;
};

class OpenApiGenerator
{
  public:
    static void addEndpoint(const EndpointInfo &endpoint);
    static Json::Value generateOpenApiSpec();
    static bool writeToFile(const std::string &outputPath);
    static void setApiInfo(
      const std::string &title,
      const std::string &version,
      const std::string &description
    );

    // Server configuration
    static void setServerConfig(const std::string &url, const std::string &description = "");

    // Helper function to convert ParameterType to string
    static std::string parameterTypeToString(ParameterType type);

    // Helper function to convert ParameterLocation to string
    static std::string parameterLocationToString(ParameterLocation location);

    // Read-only access to the registered endpoint set. Used by
    // ResourceScopeRegistry (#43) to build the (path, method) -> scopes
    // matrix at startup from the same single source that drives OpenAPI.
    static const std::vector<EndpointInfo> &endpoints();

  private:
    static std::vector<EndpointInfo> &getEndpoints();
    static Json::Value &getApiInfo();
    static bool &getInitialized();
    static Json::Value &getServerConfig();

    static Json::Value generatePathItem(const EndpointInfo &endpoint);
    static Json::Value generateParameter(const ParameterInfo &param);
    static Json::Value generateSchema();
};

}  // namespace fulla::drogon::observability::openapi
