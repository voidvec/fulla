#include <drogon/drogon_test.h>

#include "../../common/HttpTestClient.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>

namespace
{
// Every credential POST is sent once, even if the response is lost.
::drogon::HttpResponsePtr requestOnce(
  ::drogon::HttpMethod method,
  const std::string &path,
  const std::string &body,
  ::drogon::ContentType type,
  const std::string &authorization = "",
  double timeout = ::fulla::test::http::fullaTestHttpTimeout()
)
{
    auto request = ::drogon::HttpRequest::newHttpRequest();
    request->setMethod(method);
    request->setPath(path);
    request->setContentTypeCode(type);
    request->setBody(body);
    if (!authorization.empty())
        request->addHeader("Authorization", authorization);
    auto client = ::fulla::test::http::sharedTestHttpClient();
    auto [result, response] = client->sendRequest(request, timeout);
    return result == ::drogon::ReqResult::Ok ? response : nullptr;
}

::drogon::HttpResponsePtr formOnce(
  const std::string &path,
  const std::string &form,
  const std::string &authorization = ""
)
{
    return requestOnce(::drogon::Post, path, form, ::drogon::CT_APPLICATION_X_FORM, authorization);
}

::drogon::HttpResponsePtr jsonOnce(
  ::drogon::HttpMethod method,
  const std::string &path,
  const Json::Value &json,
  const std::string &bearer
)
{
    Json::StreamWriterBuilder builder;
    return requestOnce(
      method,
      path,
      Json::writeString(builder, json),
      ::drogon::CT_APPLICATION_JSON,
      "Bearer " + bearer
    );
}

std::set<std::string> scopeSet(const std::string &scopes)
{
    std::istringstream stream(scopes);
    std::set<std::string> result;
    std::string scope;
    while (stream >> scope)
        result.insert(scope);
    return result;
}

// The existing PUT response precedes transaction commit. Poll only this
// read endpoint, under one total deadline, before sending any grant POST.
bool scopesVisible(
  const std::string &clientId,
  const std::set<std::string> &expected,
  const std::string &bearer
)
{
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(3);
    size_t attempts = 0;
    for (;;)
    {
        const double remaining =
          std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0.0)
            return false;
        ++attempts;
        auto response = requestOnce(
          ::drogon::Get,
          "/api/admin/clients/" + clientId + "/scopes",
          "",
          ::drogon::CT_APPLICATION_JSON,
          "Bearer " + bearer,
          (std::min)(remaining, ::fulla::test::http::fullaTestHttpTimeout())
        );
        if (!response || response->getStatusCode() != ::drogon::k200OK)
            return false;
        Json::Value body;
        if (!::fulla::test::http::parseJsonBody(response, body) || !body["scopes"].isArray())
            return false;
        std::set<std::string> actual;
        for (const auto &scope : body["scopes"])
        {
            if (!scope.isString())
                return false;
            actual.insert(scope.asString());
        }
        if (actual == expected)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return false;
            std::cout << "[payload-contract] scopes ready attempts=" << attempts << " elapsed="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count()
                      << "ms\n";
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

Json::Value idTokenClaims(const std::string &token)
{
    const auto first = token.find('.');
    const auto last = token.rfind('.');
    if (first == std::string::npos || last <= first)
        return {};
    auto payload = token.substr(first + 1, last - first - 1);
    std::replace(payload.begin(), payload.end(), '-', '+');
    std::replace(payload.begin(), payload.end(), '_', '/');
    while (payload.size() % 4 != 0)
        payload += '=';
    const auto decoded = ::drogon::utils::base64Decode(payload);
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value result;
    std::string errors;
    if (!reader->parse(decoded.data(), decoded.data() + decoded.size(), &result, &errors))
        return {};
    return result;
}
}  // namespace

DROGON_TEST(Integration_P1_TokenSuccessPayload_HttpContract)
{
    if (!::fulla::test::http::postgresAvailable())
        return;
    auto plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
    REQUIRE(plugin != nullptr);

    const auto verifier = ::fulla::drogon::utils::generateSecureToken(32);
    const auto challenge = ::fulla::drogon::utils::computeCodeChallenge(verifier, "S256");
    auto login = formOnce(
      "/oauth2/login",
      "username=admin&password=admin&client_id=fulla-admin-console"
      "&redirect_uri=http://127.0.0.1:5174/admin/callback&scope=openid+profile+admin"
      "&state=success-payload-contract&json=true&code_challenge_method=S256&code_challenge=" +
        challenge
    );
    REQUIRE(login != nullptr);
    REQUIRE(login->getStatusCode() == ::drogon::k200OK);
    Json::Value body;
    REQUIRE(::fulla::test::http::parseJsonBody(login, body));
    REQUIRE(body["code"].isString());
    REQUIRE(!body["code"].asString().empty());
    auto adminExchange = formOnce(
      "/oauth2/token",
      "grant_type=authorization_code&client_id=fulla-admin-console"
      "&redirect_uri=http://127.0.0.1:5174/admin/callback&code_verifier=" +
        verifier + "&code=" + body["code"].asString()
    );
    REQUIRE(adminExchange != nullptr);
    REQUIRE(adminExchange->getStatusCode() == ::drogon::k200OK);
    REQUIRE(::fulla::test::http::parseJsonBody(adminExchange, body));
    REQUIRE(body["access_token"].isString());
    const std::string adminToken = body["access_token"].asString();
    REQUIRE(!adminToken.empty());
    auto userInfo = requestOnce(
      ::drogon::Get, "/oauth2/userinfo", "", ::drogon::CT_APPLICATION_JSON, "Bearer " + adminToken
    );
    REQUIRE(userInfo != nullptr);
    REQUIRE(userInfo->getStatusCode() == ::drogon::k200OK);
    REQUIRE(::fulla::test::http::parseJsonBody(userInfo, body));
    REQUIRE(body["sub"].isString());
    const auto subject = body["sub"].asString();
    REQUIRE(!subject.empty());

    std::array<std::string, 2> clientIds;
    std::array<std::string, 2> secrets;
    const std::array<std::set<std::string>, 2> registeredScopes{
      std::set<std::string>{"tokens:read", "clients:read"},
      std::set<std::string>{"openid", "profile"}
    };
    for (size_t i = 0; i < clientIds.size(); ++i)
    {
        Json::Value create;
        create["name"] = "success-payload-" + ::drogon::utils::getUuid();
        create["redirect_uris"] = "http://127.0.0.1:9999/callback";
        create["client_type"] = i == 0 ? "CONFIDENTIAL" : "PUBLIC";
        create["allowed_grant_types"] =
          i == 0 ? "client_credentials" : "urn:ietf:params:oauth:grant-type:device_code";
        auto response = jsonOnce(::drogon::Post, "/api/admin/clients", create, adminToken);
        REQUIRE(response != nullptr);
        REQUIRE(response->getStatusCode() == ::drogon::k201Created);
        REQUIRE(::fulla::test::http::parseJsonBody(response, body));
        REQUIRE(body["client_id"].isString());
        REQUIRE(body["client_secret"].isString());
        clientIds[i] = body["client_id"].asString();
        secrets[i] = body["client_secret"].asString();
        REQUIRE(!clientIds[i].empty());
        REQUIRE(
          body["token_endpoint_auth_method"].asString() == (i == 0 ? "client_secret_basic" : "none")
        );
        Json::Value scopes;
        scopes["scopes"] = Json::Value(Json::arrayValue);
        for (const auto &scope : registeredScopes[i])
            scopes["scopes"].append(scope);
        response = jsonOnce(
          ::drogon::Put, "/api/admin/clients/" + clientIds[i] + "/scopes", scopes, adminToken
        );
        REQUIRE(response != nullptr);
        REQUIRE(response->getStatusCode() == ::drogon::k200OK);
        REQUIRE(scopesVisible(clientIds[i], registeredScopes[i], adminToken));
    }

    const auto basic = "Basic " + ::drogon::utils::base64Encode(clientIds[0] + ":" + secrets[0]);
    for (bool explicitScope : {true, false})
    {
        auto response = formOnce(
          "/oauth2/token",
          "grant_type=client_credentials" + std::string(explicitScope ? "&scope=tokens:read" : ""),
          basic
        );
        REQUIRE(response != nullptr);
        REQUIRE(response->getStatusCode() == ::drogon::k200OK);
        REQUIRE(::fulla::test::http::parseJsonBody(response, body));
        REQUIRE(body.size() == 4U);
        REQUIRE(body["access_token"].isString());
        REQUIRE(!body["access_token"].asString().empty());
        REQUIRE(body["token_type"].asString() == "Bearer");
        REQUIRE(body["expires_in"].isIntegral());
        REQUIRE(body["expires_in"].asInt64() == plugin->getAccessTokenTtl());
        REQUIRE(body["scope"].isString());
        if (explicitScope)
            REQUIRE(body["scope"].asString() == "tokens:read");
        else
        {
            const auto actual = scopeSet(body["scope"].asString());
            REQUIRE(actual.size() == registeredScopes[0].size());
            for (const auto &scope : registeredScopes[0])
                REQUIRE(actual.count(scope) == 1U);
        }
        REQUIRE(!body.isMember("refresh_token"));
        REQUIRE(!body.isMember("id_token"));
        REQUIRE(!body.isMember("roles"));
        REQUIRE(response->getHeader("content-type").find("application/json") == 0);
        REQUIRE(response->getHeader("cache-control") == "no-store");
        REQUIRE(response->getHeader("pragma") == "no-cache");
        REQUIRE(response->getHeader("www-authenticate").empty());
    }
    auto rejected = formOnce("/oauth2/token", "grant_type=client_credentials&scope=admin", basic);
    REQUIRE(rejected != nullptr);
    REQUIRE(rejected->getStatusCode() == ::drogon::k400BadRequest);
    REQUIRE(::fulla::test::http::parseJsonBody(rejected, body));
    REQUIRE(body["error"].asString() == "invalid_scope");
    REQUIRE(!body.isMember("access_token"));
    REQUIRE(!body.isMember("refresh_token"));
    REQUIRE(rejected->getHeader("cache-control").empty());
    REQUIRE(rejected->getHeader("pragma").empty());

    const std::array<std::string, 3> deviceScopes{"", "profile", "openid profile"};
    for (const auto &scope : deviceScopes)
    {
        auto response = formOnce(
          "/oauth2/device_authorization",
          "client_id=" + clientIds[1] + "&scope=" + ::drogon::utils::urlEncode(scope)
        );
        REQUIRE(response != nullptr);
        REQUIRE(response->getStatusCode() == ::drogon::k200OK);
        REQUIRE(::fulla::test::http::parseJsonBody(response, body));
        REQUIRE(body["device_code"].isString());
        REQUIRE(body["user_code"].isString());
        const auto deviceCode = body["device_code"].asString();
        const auto userCode = body["user_code"].asString();
        const std::string redeem =
          "grant_type=urn:ietf:params:oauth:grant-type:device_code&client_id=" + clientIds[1] +
          "&device_code=" + deviceCode;
        if (scope.empty())
        {
            REQUIRE(body["interval"].isInt());
            const int interval = body["interval"].asInt();
            REQUIRE(interval > 1);
            const auto start = std::chrono::steady_clock::now();
            auto pending = formOnce("/oauth2/token", redeem);
            auto slow = formOnce("/oauth2/token", redeem);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            const auto elapsedMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            std::cout << "[payload-contract] poll interval=" << interval
                      << "s elapsed=" << elapsedMs << "ms\n";
            // The server compares whole application seconds. Reserve one
            // second so crossing a clock boundary cannot invalidate this test.
            REQUIRE(elapsedMs < static_cast<int64_t>(interval - 1) * 1000);
            REQUIRE(pending != nullptr);
            REQUIRE(slow != nullptr);
            REQUIRE(pending->getStatusCode() == ::drogon::k400BadRequest);
            REQUIRE(::fulla::test::http::parseJsonBody(pending, body));
            REQUIRE(body["error"].asString() == "authorization_pending");
            REQUIRE(!body.isMember("access_token"));
            REQUIRE(slow->getStatusCode() == ::drogon::k400BadRequest);
            REQUIRE(::fulla::test::http::parseJsonBody(slow, body));
            REQUIRE(body["error"].asString() == "slow_down");
            REQUIRE(body["interval"].asInt() == interval + 5);
            REQUIRE(!body.isMember("refresh_token"));
        }
        auto approved = formOnce(
          "/oauth2/device/approve",
          "user_code=" + userCode + "&user_id=" + subject,
          "Bearer " + adminToken
        );
        REQUIRE(approved != nullptr);
        REQUIRE(approved->getStatusCode() == ::drogon::k200OK);
        REQUIRE(::fulla::test::http::parseJsonBody(approved, body));
        REQUIRE(body["status"].asString() == "approved");
        response = formOnce("/oauth2/token", redeem);
        REQUIRE(response != nullptr);
        REQUIRE(response->getStatusCode() == ::drogon::k200OK);
        REQUIRE(::fulla::test::http::parseJsonBody(response, body));
        const bool openid = scope == "openid profile";
        REQUIRE(body.size() == (scope.empty() ? 4U : openid ? 6U : 5U));
        REQUIRE(body["access_token"].isString());
        REQUIRE(!body["access_token"].asString().empty());
        REQUIRE(body["refresh_token"].isString());
        REQUIRE(!body["refresh_token"].asString().empty());
        REQUIRE(body["token_type"].asString() == "Bearer");
        REQUIRE(body["expires_in"].isIntegral());
        REQUIRE(body["expires_in"].asInt64() == plugin->getAccessTokenTtl());
        REQUIRE(!body.isMember("roles"));
        if (scope.empty())
            REQUIRE(!body.isMember("scope"));
        else
            REQUIRE(body["scope"].asString() == scope);
        if (openid)
        {
            REQUIRE(body["id_token"].isString());
            REQUIRE(!body["id_token"].asString().empty());
            const auto claims = idTokenClaims(body["id_token"].asString());
            REQUIRE(claims.isObject());
            REQUIRE(claims["sub"].asString() == subject);
            REQUIRE(claims["aud"].asString() == clientIds[1]);
            REQUIRE(claims["iss"].asString() == plugin->getIssuer());
            REQUIRE(
              claims["exp"].asInt64() - claims["iat"].asInt64() == plugin->getAccessTokenTtl()
            );
        }
        else
            REQUIRE(!body.isMember("id_token"));
        REQUIRE(response->getHeader("content-type").find("application/json") == 0);
        REQUIRE(response->getHeader("cache-control") == "no-store");
        REQUIRE(response->getHeader("pragma") == "no-cache");
        REQUIRE(response->getHeader("www-authenticate").empty());

        auto replay = formOnce("/oauth2/token", redeem);
        REQUIRE(replay != nullptr);
        REQUIRE(replay->getStatusCode() == ::drogon::k400BadRequest);
        REQUIRE(::fulla::test::http::parseJsonBody(replay, body));
        REQUIRE(body["error"].asString() == "invalid_grant");
        REQUIRE(!body.isMember("access_token"));
        REQUIRE(!body.isMember("refresh_token"));
    }
    // The admin API hard-deletes clients; issued AT/RT/device rows retain
    // foreign-key references. Keep this run's two UUID clients and finite
    // associated rows for the isolated functional fixture's lifecycle.
    // Repeated runs add data until fixture retirement; do not purge shared
    // records or use the separate S07 canonical capacity fixture here.
}
