#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>
#include <drogon/drogon.h>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <system_error>
#include <sstream>
#include <algorithm>

namespace fulla::drogon::observability::openapi
{

std::vector<EndpointInfo> &OpenApiGenerator::getEndpoints()
{
    static std::vector<EndpointInfo> endpoints;
    return endpoints;
}

Json::Value &OpenApiGenerator::getApiInfo()
{
    static Json::Value apiInfo;
    return apiInfo;
}

bool &OpenApiGenerator::getInitialized()
{
    static bool initialized = false;
    return initialized;
}

Json::Value &OpenApiGenerator::getServerConfig()
{
    static Json::Value serverConfig;
    return serverConfig;
}

void OpenApiGenerator::setApiInfo(
  const std::string &title,
  const std::string &version,
  const std::string &description
)
{
    auto &apiInfo = getApiInfo();
    apiInfo["title"] = title;
    apiInfo["version"] = version;
    apiInfo["description"] = description;
    getInitialized() = true;
}

void OpenApiGenerator::setServerConfig(const std::string &url, const std::string &description)
{
    auto &serverConfig = getServerConfig();
    serverConfig["url"] = url;
    serverConfig["description"] = description;
}

void OpenApiGenerator::addEndpoint(const EndpointInfo &endpoint)
{
    getEndpoints().push_back(endpoint);
}

const std::vector<EndpointInfo> &OpenApiGenerator::endpoints()
{
    return getEndpoints();
}

Json::Value OpenApiGenerator::generateOpenApiSpec()
{
    Json::Value spec;
    spec["openapi"] = "3.0.0";

    // Info section
    if (!getInitialized())
    {
        setApiInfo(
          "OAuth2 Authorization Server API",
#ifdef FULLA_VERSION_TEXT
          FULLA_VERSION_TEXT,
#else
          "1.0.1",
#endif
          "OAuth2.0 authorization server with token management"
        );
    }
    spec["info"] = getApiInfo();

    // Servers
    Json::Value servers(Json::arrayValue);
    auto &serverConfig = getServerConfig();
    if (!serverConfig.empty() && serverConfig.isMember("url"))
    {
        // Use configured server
        servers.append(serverConfig);
    }
    else
    {
        // Default to relative path for environment flexibility
        Json::Value server;
        server["url"] = "/";
        server["description"] = "Default server (relative path)";
        servers.append(server);
    }
    spec["servers"] = servers;

    // Paths
    Json::Value paths;
    for (const auto &endpoint : getEndpoints())
    {
        std::string pathKey = endpoint.path;
        Json::Value pathItem = generatePathItem(endpoint);

        std::string mLower = endpoint.method;
        std::transform(mLower.begin(), mLower.end(), mLower.begin(), [](unsigned char c) {
            return std::tolower(c);
        });

        if (paths.isMember(pathKey))
        {
            paths[pathKey][mLower] = pathItem[mLower];
        }
        else
        {
            paths[pathKey] = pathItem;
        }
    }
    spec["paths"] = paths;

    // Components/schemas
    spec["components"]["schemas"] = generateSchema();

    // Security Schemes
    Json::Value securitySchemes;
    Json::Value bearerAuth;
    bearerAuth["type"] = "http";
    bearerAuth["scheme"] = "bearer";
    securitySchemes["bearerAuth"] = bearerAuth;
    // OAuth2 client authentication (RFC 6749 §2.3): used by RFC 7662
    // introspection and RFC 7009 revocation -- these authenticate the
    // CLIENT (client_id/client_secret via HTTP Basic or POST body), NOT a
    // user bearer token. Declaring bearerAuth for them misleads SDK
    // consumers.
    Json::Value clientCredentialsAuth;
    clientCredentialsAuth["type"] = "http";
    clientCredentialsAuth["scheme"] = "basic";
    clientCredentialsAuth["description"] =
      "OAuth2 client authentication (RFC 6749 §2.3): client_id and "
      "client_secret via HTTP Basic Authorization header or POST body "
      "parameters.";
    securitySchemes["clientCredentialsAuth"] = clientCredentialsAuth;
    spec["components"]["securitySchemes"] = securitySchemes;

    return spec;
}

Json::Value OpenApiGenerator::generatePathItem(const EndpointInfo &endpoint)
{
    Json::Value pathItem;
    pathItem["summary"] = endpoint.summary;
    pathItem["description"] = endpoint.description;

    std::string safePath = endpoint.path.substr(1);
    std::replace(safePath.begin(), safePath.end(), '/', '_');

    std::string methodLower = endpoint.method;
    std::transform(
      methodLower.begin(), methodLower.end(), methodLower.begin(), [](unsigned char c) {
          return std::tolower(c);
      }
    );

    pathItem["operationId"] = methodLower + "_" + safePath;

    // Tags
    Json::Value tags(Json::arrayValue);
    for (const auto &tag : endpoint.tags)
    {
        tags.append(tag);
    }
    pathItem["tags"] = tags;

    // Parameters
    if (!endpoint.parameters.empty())
    {
        Json::Value parameters(Json::arrayValue);
        for (const auto &param : endpoint.parameters)
        {
            parameters.append(generateParameter(param));
        }
        pathItem["parameters"] = parameters;
    }

    if (!endpoint.requestBody.isNull())
        pathItem["requestBody"] = endpoint.requestBody;

    // Responses
    Json::Value responses;
    for (const auto &[code, desc] : endpoint.responses)
    {
        Json::Value response;
        response["description"] = desc;

        // Add response example if available
        if (endpoint.responseExamples.find(code) != endpoint.responseExamples.end())
        {
            response["content"]["application/json"]["example"] = endpoint.responseExamples.at(code);
        }
        else if (code == 200)
        {
            // Default schema for 200 responses
            response["content"]["application/json"]["schema"]["type"] = "object";
        }

        responses[std::to_string(code)] = response;
    }
    pathItem["responses"] = responses;

    // Security
    if (endpoint.requiresAuth)
    {
        // Scheme selection: user-token endpoints use bearerAuth; RFC
        // 7662/7009-style client-authenticated endpoints (introspect,
        // revoke) use clientCredentialsAuth.
        //
        // OpenAPI 3.0 §securityRequirement: the `security` field MUST be an
        // ARRAY of Security Requirement Objects, each `{ schemeName: [scopes] }`
        // -- an empty scope list for scope-less HTTP schemes. (Issue #41: the
        // previous code emitted an object { scheme: [scheme] }, which is
        // malformed and broke Swagger UI / SDK codegen.)
        const char *schemeName = (endpoint.authType == AuthType::ClientCredentials)
                                   ? "clientCredentialsAuth"
                                   : "bearerAuth";
        Json::Value securityReq(Json::objectValue);
        securityReq[schemeName] = Json::Value(Json::arrayValue);  // empty scope list
        Json::Value securityArr(Json::arrayValue);
        securityArr.append(securityReq);
        pathItem["security"] = securityArr;
    }

    // Resource-scope authorization model (#43): emit the concrete scope
    // requirement as a vendor extension. The securitySchemes above use
    // `type: http` (Bearer/Basic), for which OpenAPI 3.0 gives the scope
    // array in a Security Requirement Object no defined semantics -- only
    // `type: oauth2` schemes interpret scopes. Rather than mislead SDK
    // consumers by populating the (semantically inert) bearerAuth scope
    // list, the authoritative per-operation scope set is carried here so
    // codegen/tooling that understands the extension can bind it (R3).
    if (!endpoint.requiredScopes.empty())
    {
        Json::Value reqScopes(Json::arrayValue);
        for (const auto &s : endpoint.requiredScopes)
        {
            reqScopes.append(s);
        }
        pathItem["x-required-scopes"] = reqScopes;
        if (!endpoint.impliedBy.empty())
        {
            Json::Value implied(Json::arrayValue);
            for (const auto &s : endpoint.impliedBy)
            {
                implied.append(s);
            }
            pathItem["x-scope-implied-by"] = implied;
        }
    }

    Json::Value result;
    result[methodLower] = pathItem;
    return result;
}

Json::Value OpenApiGenerator::generateSchema()
{
    Json::Value schemas;

    // Error schema
    Json::Value errorSchema;
    errorSchema["type"] = "object";
    Json::Value errorProps;
    errorProps["code"]["type"] = "integer";
    errorProps["category"]["type"] = "string";
    errorProps["message"]["type"] = "string";
    errorProps["details"]["type"] = "string";
    errorProps["request_id"]["type"] = "string";
    errorSchema["properties"] = errorProps;
    schemas["Error"] = errorSchema;

    // Token response schema
    Json::Value tokenSchema;
    tokenSchema["type"] = "object";
    Json::Value tokenProps;
    tokenProps["access_token"]["type"] = "string";
    tokenProps["refresh_token"]["type"] = "string";
    tokenProps["expires_in"]["type"] = "integer";
    tokenProps["token_type"]["type"] = "string";
    tokenSchema["properties"] = tokenProps;
    schemas["TokenResponse"] = tokenSchema;

    return schemas;
}

std::string OpenApiGenerator::parameterTypeToString(ParameterType type)
{
    switch (type)
    {
        case ParameterType::STRING:
            return "string";
        case ParameterType::INTEGER:
            return "integer";
        case ParameterType::NUMBER:
            return "number";
        case ParameterType::BOOLEAN:
            return "boolean";
        case ParameterType::ARRAY:
            return "array";
        case ParameterType::OBJECT:
            return "object";
        default:
            return "string";
    }
}

std::string OpenApiGenerator::parameterLocationToString(ParameterLocation location)
{
    switch (location)
    {
        case ParameterLocation::QUERY:
            return "query";
        case ParameterLocation::HEADER:
            return "header";
        case ParameterLocation::PATH:
            return "path";
        case ParameterLocation::COOKIE:
            return "cookie";
        default:
            return "query";
    }
}

Json::Value OpenApiGenerator::generateParameter(const ParameterInfo &param)
{
    Json::Value parameter;
    parameter["name"] = param.name;
    parameter["in"] = parameterLocationToString(param.location);
    parameter["description"] = param.description;
    parameter["required"] = param.required;

    // Set schema based on parameter type
    parameter["schema"]["type"] = parameterTypeToString(param.type);

    // Add format if specified
    if (!param.format.empty())
    {
        parameter["schema"]["format"] = param.format;
    }

    // Add default value if specified
    if (!param.defaultValue.empty())
    {
        if (param.type == ParameterType::BOOLEAN)
        {
            parameter["schema"]["default"] = (param.defaultValue == "true");
        }
        else if (param.type == ParameterType::INTEGER || param.type == ParameterType::NUMBER)
        {
            // Try to parse as number
            try
            {
                if (param.type == ParameterType::INTEGER)
                {
                    parameter["schema"]["default"] = std::stoi(param.defaultValue);
                }
                else
                {
                    parameter["schema"]["default"] = std::stod(param.defaultValue);
                }
            }
            catch (...)
            {
                // If parsing fails, store as string
                parameter["schema"]["default"] = param.defaultValue;
            }
        }
        else
        {
            parameter["schema"]["default"] = param.defaultValue;
        }
    }

    // Add enum values if specified
    if (!param.enumValues.empty())
    {
        Json::Value enumArray(Json::arrayValue);
        std::stringstream ss(param.enumValues);
        std::string value;
        while (std::getline(ss, value, ','))
        {
            // Trim whitespace
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            enumArray.append(value);
        }
        parameter["schema"]["enum"] = enumArray;
    }

    return parameter;
}

bool OpenApiGenerator::writeToFile(const std::string &outputPath)
{
    try
    {
        // Create directory if it doesn't exist
        std::filesystem::path filePath(outputPath);
        std::filesystem::path dirPath = filePath.parent_path();

        if (!dirPath.empty() && !std::filesystem::exists(dirPath))
        {
            std::filesystem::create_directories(dirPath);
            LOG_INFO << "Created directory: " << dirPath.string();
        }

        Json::Value spec = generateOpenApiSpec();

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());

        std::ofstream outputFile(outputPath);
        if (!outputFile.is_open())
        {
            LOG_ERROR << "Failed to open file for writing: " << outputPath;
            return false;
        }

        writer->write(spec, &outputFile);
        outputFile.close();

        LOG_INFO << "OpenAPI specification written to: " << outputPath;
        return true;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "Error writing OpenAPI spec: " << e.what();
        return false;
    }
}

}  // namespace fulla::drogon::observability::openapi
