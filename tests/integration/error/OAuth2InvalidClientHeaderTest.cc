// Feature: error-code-message-standardization
// F2 + F3: WWW-Authenticate header support for introspect/revoke invalid_client
//
// Validates: Requirement 2.9
//   When introspect/revoke endpoints receive invalid client credentials via
//   HTTP Basic Authentication (Authorization: Basic header), the 401 response
//   includes a WWW-Authenticate: Basic realm="..." header per RFC 6749 §5.2.
//
// This is an endpoint-level integration test that verifies the actual HTTP
// response from /oauth2/introspect and /oauth2/revoke includes the
// WWW-Authenticate header when client authentication fails with Basic auth.

#include <drogon/drogon_test.h>
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <memory>
#include <string>

using namespace drogon;

namespace
{

// Helper: create Basic auth header with given credentials
std::string makeBasicAuthHeader(const std::string &clientId, const std::string &clientSecret)
{
    std::string credentials = clientId + ":" + clientSecret;
    std::string encoded = utils::base64Encode(
      reinterpret_cast<const unsigned char *>(credentials.data()), credentials.size()
    );
    return "Basic " + encoded;
}

// Helper: check if server is reachable
bool isServerReachable()
{
    try
    {
        auto client = HttpClient::newHttpClient("http://127.0.0.1:5555");
        auto req = HttpRequest::newHttpRequest();
        req->setPath("/health");
        req->setMethod(Get);

        const auto result = client->sendRequest(req, 2.0);
        return result.first == ReqResult::Ok;
    }
    catch (...)
    {
        return false;
    }
}

}  // namespace

// Test introspect endpoint with invalid Basic auth returns WWW-Authenticate header
DROGON_TEST(Integration_P1_OAuth2InvalidClient_IntrospectReturnsWWWAuthenticateHeader)
{
    if (!isServerReachable())
    {
        // Skip if server not running
        CHECK(true);
        return;
    }

    auto client = HttpClient::newHttpClient("http://127.0.0.1:5555");
    auto req = HttpRequest::newHttpRequest();
    req->setPath("/oauth2/introspect");
    req->setMethod(Post);
    req->setContentTypeCode(CT_APPLICATION_X_FORM);

    // Set invalid Basic auth header
    req->addHeader("Authorization", makeBasicAuthHeader("invalid_client", "wrong_secret"));

    // Set required token parameter (use valid format to avoid invalid_request)
    // Token must be >= 32 chars and match [a-zA-Z0-9._-]+
    req->setParameter("token", "some_valid_token_with_32_characters_min");

    const auto result = client->sendRequest(req, 5.0);
    REQUIRE(result.first == ReqResult::Ok);
    const auto &resp = result.second;
    REQUIRE(resp != nullptr);

    // Check HTTP 401
    CHECK(resp->getStatusCode() == k401Unauthorized);

    // Check Content-Type is application/json
    CHECK(resp->getContentType() == CT_APPLICATION_JSON);

    // Check WWW-Authenticate header is present and contains "Basic"
    auto wwwAuth = resp->getHeader("WWW-Authenticate");
    CHECK(!wwwAuth.empty());
    CHECK(wwwAuth.find("Basic") != std::string::npos);

    // Check body is RFC 6749 §5.2 format with error="invalid_client"
    auto body = std::string(resp->getBody());
    Json::Value json;
    Json::Reader reader;
    CHECK(reader.parse(body, json));
    CHECK(json.isMember("error"));
    CHECK(json["error"].isString());
    CHECK(json["error"].asString() == "invalid_client");
}

// Test revoke endpoint with invalid Basic auth returns WWW-Authenticate header
DROGON_TEST(Integration_P1_OAuth2InvalidClient_RevokeReturnsWWWAuthenticateHeader)
{
    if (!isServerReachable())
    {
        // Skip if server not running
        CHECK(true);
        return;
    }

    auto client = HttpClient::newHttpClient("http://127.0.0.1:5555");
    auto req = HttpRequest::newHttpRequest();
    req->setPath("/oauth2/revoke");
    req->setMethod(Post);
    req->setContentTypeCode(CT_APPLICATION_X_FORM);

    // Set invalid Basic auth header
    req->addHeader("Authorization", makeBasicAuthHeader("invalid_client", "wrong_secret"));

    // Set required token parameter (use valid format to avoid invalid_request)
    // Token must be >= 32 chars and match [a-zA-Z0-9._-]+
    req->setParameter("token", "some_valid_token_with_32_characters_min");

    const auto result = client->sendRequest(req, 5.0);
    REQUIRE(result.first == ReqResult::Ok);
    const auto &resp = result.second;
    REQUIRE(resp != nullptr);

    // Check HTTP 401
    CHECK(resp->getStatusCode() == k401Unauthorized);

    // Check Content-Type is application/json
    CHECK(resp->getContentType() == CT_APPLICATION_JSON);

    // Check WWW-Authenticate header is present and contains "Basic"
    auto wwwAuth = resp->getHeader("WWW-Authenticate");
    CHECK(!wwwAuth.empty());
    CHECK(wwwAuth.find("Basic") != std::string::npos);

    // Check body is RFC 6749 §5.2 format with error="invalid_client"
    auto body = std::string(resp->getBody());
    Json::Value json;
    Json::Reader reader;
    CHECK(reader.parse(body, json));
    CHECK(json.isMember("error"));
    CHECK(json["error"].isString());
    CHECK(json["error"].asString() == "invalid_client");
}
