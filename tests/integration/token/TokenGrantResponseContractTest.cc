#include <drogon/drogon_test.h>

#include "../../common/HttpTestClient.h"

#include <array>
#include <string>
#include <utility>

namespace
{
// Codes and refresh tokens are single-use credentials. Never replay a POST
// after a transport timeout: the server may already have consumed it.
::drogon::HttpResponsePtr postFormOnce(
  const std::string &path,
  const std::string &body,
  const std::string &authorization = ""
)
{
    auto client = ::fulla::test::http::sharedTestHttpClient();
    auto req = ::drogon::HttpRequest::newHttpRequest();
    req->setMethod(::drogon::Post);
    req->setPath(path);
    req->setContentTypeCode(::drogon::CT_APPLICATION_X_FORM);
    req->setBody(body);
    if (!authorization.empty())
        req->addHeader("Authorization", authorization);
    auto [result, response] = client->sendRequest(req, ::fulla::test::http::fullaTestHttpTimeout());
    return result == ::drogon::ReqResult::Ok ? response : nullptr;
}
}  // namespace

DROGON_TEST(Integration_P1_TokenGrantResponse_HttpContract)
{
    if (!::fulla::test::http::postgresAvailable())
        return;
    auto plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
    REQUIRE(plugin != nullptr);

    const auto verifier = ::fulla::drogon::utils::generateSecureToken(32);
    const auto challenge = ::fulla::drogon::utils::computeCodeChallenge(verifier, "S256");
    auto login = postFormOnce(
      "/oauth2/login",
      "username=admin&password=admin&client_id=fulla-portal"
      "&redirect_uri=http://127.0.0.1:5173/callback&scope=openid+profile"
      "&state=token-response-contract&json=true&code_challenge_method=S256"
      "&code_challenge=" +
        challenge
    );
    REQUIRE(login != nullptr);
    REQUIRE(login->getStatusCode() == ::drogon::k200OK);
    Json::Value loginJson;
    REQUIRE(::fulla::test::http::parseJsonBody(login, loginJson));
    REQUIRE(loginJson["code"].isString());
    REQUIRE(!loginJson["code"].asString().empty());

    const std::string exchangeForm =
      "grant_type=authorization_code&client_id=fulla-portal"
      "&redirect_uri=http://127.0.0.1:5173/callback&code_verifier=" +
      verifier + "&code=" + loginJson["code"].asString();
    auto exchange = postFormOnce("/oauth2/token", exchangeForm);
    REQUIRE(exchange != nullptr);
    REQUIRE(exchange->getStatusCode() == ::drogon::k200OK);
    Json::Value issued;
    REQUIRE(::fulla::test::http::parseJsonBody(exchange, issued));
    REQUIRE(issued["refresh_token"].isString());
    REQUIRE(!issued["refresh_token"].asString().empty());
    CHECK(issued["id_token"].isString());
    CHECK(!issued["id_token"].asString().empty());

    auto refresh = postFormOnce(
      "/oauth2/token",
      "grant_type=refresh_token&client_id=fulla-portal&refresh_token=" +
        issued["refresh_token"].asString()
    );
    REQUIRE(refresh != nullptr);
    REQUIRE(refresh->getStatusCode() == ::drogon::k200OK);
    Json::Value rotated;
    REQUIRE(::fulla::test::http::parseJsonBody(refresh, rotated));

    for (const auto &response : {exchange, refresh})
    {
        Json::Value json;
        REQUIRE(::fulla::test::http::parseJsonBody(response, json));
        CHECK(json.isObject());
        CHECK(!json.isMember("error"));
        CHECK(json["access_token"].isString());
        CHECK(!json["access_token"].asString().empty());
        CHECK(json["refresh_token"].isString());
        CHECK(!json["refresh_token"].asString().empty());
        CHECK(json["token_type"].asString() == "Bearer");
        CHECK(json["expires_in"].isIntegral());
        CHECK(json["expires_in"].asInt64() == plugin->getAccessTokenTtl());
        CHECK(!json.isMember("scope"));
        CHECK(json["id_token"].isString());
        CHECK(!json["id_token"].asString().empty());
        CHECK(response->getHeader("content-type").find("application/json") == 0);
        CHECK(response->getHeader("cache-control") == "no-store");
        CHECK(response->getHeader("pragma") == "no-cache");
        CHECK(response->getHeader("www-authenticate").empty());
    }
    CHECK(rotated["access_token"].asString() != issued["access_token"].asString());
    CHECK(rotated["refresh_token"].asString() != issued["refresh_token"].asString());
    CHECK(issued["roles"].isArray());
    CHECK(!rotated.isMember("roles"));

    // A consumed code and a nonexistent RT both pass valid client validation
    // and exercise the grant completion callbacks, rather than entry errors.
    const std::array<std::pair<std::string, std::string>, 2> invalidForms{
      {{exchangeForm, "Invalid authorization code"},
       {"grant_type=refresh_token&client_id=fulla-portal&refresh_token=" +
          ::fulla::drogon::utils::generateSecureToken(32),
        "Invalid or revoked refresh token"}}
    };
    for (const auto &[form, description] : invalidForms)
    {
        auto response = postFormOnce("/oauth2/token", form);
        REQUIRE(response != nullptr);
        CHECK(response->getStatusCode() == ::drogon::k400BadRequest);
        Json::Value json;
        REQUIRE(::fulla::test::http::parseJsonBody(response, json));
        Json::Value expected;
        expected["error"] = "invalid_grant";
        expected["error_description"] = description;
        CHECK(json == expected);
        CHECK(response->getHeader("content-type").find("application/json") == 0);
        CHECK(response->getHeader("cache-control").empty());
        CHECK(response->getHeader("pragma").empty());
        CHECK(response->getHeader("www-authenticate").empty());
    }

    // The existing public error responder has different headers. Preserve
    // this distinction when extracting direct grant-result presentation.
    const std::string unknownClient = "missing-" + ::drogon::utils::getUuid();
    auto unauthorized = postFormOnce(
      "/oauth2/token",
      "grant_type=refresh_token&refresh_token=" + ::fulla::drogon::utils::generateSecureToken(32),
      "Basic " + ::drogon::utils::base64Encode(unknownClient + ":unused-secret")
    );
    REQUIRE(unauthorized != nullptr);
    CHECK(unauthorized->getStatusCode() == ::drogon::k401Unauthorized);
    Json::Value unauthorizedJson;
    REQUIRE(::fulla::test::http::parseJsonBody(unauthorized, unauthorizedJson));
    Json::Value expectedUnauthorized;
    expectedUnauthorized["error"] = "invalid_client";
    expectedUnauthorized["error_description"] = "Unknown client_id";
    CHECK(unauthorizedJson == expectedUnauthorized);
    CHECK(unauthorized->getHeader("content-type").find("application/json") == 0);
    CHECK(unauthorized->getHeader("cache-control") == "no-store");
    CHECK(unauthorized->getHeader("pragma") == "no-cache");
    CHECK(unauthorized->getHeader("www-authenticate").find("Basic ") == 0);
}
