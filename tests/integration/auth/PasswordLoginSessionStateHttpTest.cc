#include <drogon/drogon_test.h>

#include "../../common/HttpTestClient.h"
#include <fulla/identity/TotpUtils.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
namespace http = ::fulla::test::http;
namespace crypto = ::fulla::drogon::utils;
using Params = std::vector<std::pair<std::string, std::string>>;
constexpr const char *kRedirect = "http://127.0.0.1:9987/callback";

int64_t nowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()
    )
      .count();
}

Json::Value jsonOf(const ::drogon::HttpResponsePtr &response, ::drogon::HttpStatusCode status)
{
    Json::Value body;
    if (!response || response->getStatusCode() != status || !http::parseJsonBody(response, body))
        throw std::runtime_error(
          "C1a fixture HTTP response failed: actual=" +
          std::to_string(response ? static_cast<int>(response->getStatusCode()) : 0) +
          " expected=" + std::to_string(static_cast<int>(status))
        );
    return body;
}

std::string queryValue(const std::string &url, const std::string &name)
{
    const auto query = url.find('?');
    if (query == std::string::npos)
        return "";
    std::istringstream stream(url.substr(query + 1));
    std::string item;
    while (std::getline(stream, item, '&'))
    {
        const auto equal = item.find('=');
        if (item.substr(0, equal) == name && equal != std::string::npos)
            return ::drogon::utils::urlDecode(item.substr(equal + 1));
    }
    return "";
}

Json::Value jwtClaims(const std::string &jwt)
{
    const auto first = jwt.find('.');
    const auto last = jwt.rfind('.');
    if (first == std::string::npos || last <= first)
        throw std::runtime_error("C1a fixture missing JWT payload");
    auto payload = jwt.substr(first + 1, last - first - 1);
    std::replace(payload.begin(), payload.end(), '-', '+');
    std::replace(payload.begin(), payload.end(), '_', '/');
    while (payload.size() % 4)
        payload += '=';
    std::istringstream stream(::drogon::utils::base64Decode(payload));
    Json::Value claims;
    Json::CharReaderBuilder builder;
    std::string errors;
    if (!Json::parseFromStream(builder, stream, &claims, &errors))
        throw std::runtime_error("C1a fixture malformed JWT payload");
    return claims;
}

// Explicit cookie jars and single physical requests: no write/grant retries.
class Browser
{
  public:
    std::string bearer;
    std::map<std::string, std::string> cookies;

    const std::string &sessionId() const
    {
        // Both tracked test configurations explicitly select JSESSIONID.
        return cookies.at("JSESSIONID");
    }

    ::drogon::HttpResponsePtr send(
      ::drogon::HttpMethod method,
      const std::string &path,
      const Params &params = {},
      const Json::Value *json = nullptr,
      double timeout = http::fullaTestHttpTimeout()
    )
    {
        auto request = ::drogon::HttpRequest::newHttpRequest();
        request->setMethod(method);
        request->setPath(path);
        if (json)
        {
            request->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
            request->setBody(Json::writeString(Json::StreamWriterBuilder(), *json));
        }
        else if (method == ::drogon::Get)
        {
            for (const auto &param : params)
                request->setParameter(param.first, param.second);
        }
        else
        {
            std::string body;
            for (const auto &param : params)
            {
                if (!body.empty())
                    body += '&';
                body += ::drogon::utils::urlEncodeComponent(param.first) + '=' +
                        ::drogon::utils::urlEncodeComponent(param.second);
            }
            request->setContentTypeCode(::drogon::CT_APPLICATION_X_FORM);
            request->setBody(body);
        }
        if (!bearer.empty())
            request->addHeader("Authorization", "Bearer " + bearer);
        for (const auto &cookie : cookies)
            request->addCookie(cookie.first, cookie.second);
        if (!client_)
            client_ =
              ::drogon::HttpClient::newHttpClient(http::kTestBaseUrl, ::drogon::app().getLoop());
        const auto result = client_->sendRequest(request, timeout);
        if (result.first != ::drogon::ReqResult::Ok || !result.second)
            throw std::runtime_error("C1a fixture HTTP transport failed");
        for (const auto &cookie : result.second->getCookies())
            cookies[cookie.first] = cookie.second.value();
        return result.second;
    }

  private:
    ::drogon::HttpClientPtr client_;
};

struct Flow
{
    std::string verifier = crypto::generateSecureToken(32);
    std::string nonce = ::drogon::utils::getUuid();
    std::string state = "c1a state+" + ::drogon::utils::getUuid();
    std::string scope = "openid profile";

    Params parameters(const std::string &client, const std::string &redirect = kRedirect) const
    {
        return {
          {"client_id", client},
          {"redirect_uri", redirect},
          {"scope", scope},
          {"state", state},
          {"nonce", nonce},
          {"code_challenge", crypto::computeCodeChallenge(verifier, "S256")},
          {"code_challenge_method", "S256"}
        };
    }
};

::drogon::HttpResponsePtr login(
  Browser &browser,
  const std::string &user,
  const std::string &password,
  const std::string &client,
  const Flow &flow,
  bool json = true,
  const std::string &redirect = kRedirect
)
{
    auto params = flow.parameters(client, redirect);
    params.emplace_back("username", user);
    params.emplace_back("password", password);
    return browser.send(::drogon::Post, json ? "/oauth2/login?json=true" : "/oauth2/login", params);
}

Json::Value exchange(
  Browser &browser,
  const std::string &client,
  const std::string &code,
  const Flow &flow,
  const std::string &redirect = kRedirect
)
{
    return jsonOf(
      browser.send(
        ::drogon::Post,
        "/oauth2/token",
        {{"grant_type", "authorization_code"},
         {"client_id", client},
         {"redirect_uri", redirect},
         {"code", code},
         {"code_verifier", flow.verifier}}
      ),
      ::drogon::k200OK
    );
}

struct User
{
    std::string name;
    std::string password = crypto::generateSecureToken(18) + "aA1!";
    std::string id;
};

struct Fixture
{
    Browser admin;
    Browser browser;
    std::array<User, 2> users;
    std::string client;
    bool retired = false;

    ~Fixture()
    {
        if (!retired)
        {
            try
            {
                retire();
            }
            catch (...)
            {
                std::cerr << "[c1a] isolated fixture retirement incomplete\n";
            }
        }
    }

    void setup()
    {
        Flow flow;
        flow.scope = "openid profile admin";
        auto body = jsonOf(
          login(
            admin,
            "admin",
            "admin",
            "fulla-admin-console",
            flow,
            true,
            "http://127.0.0.1:5174/admin/callback"
          ),
          ::drogon::k200OK
        );
        auto tokens = exchange(
          admin,
          "fulla-admin-console",
          body["code"].asString(),
          flow,
          "http://127.0.0.1:5174/admin/callback"
        );
        admin.bearer = tokens["access_token"].asString();
        if (admin.bearer.empty())
            throw std::runtime_error("C1a fixture admin authentication failed");
        for (auto &user : users)
        {
            user.name = "c1a_" + ::drogon::utils::getUuid();
            Json::Value create;
            create["username"] = user.name;
            create["password"] = user.password;
            create["email_verified"] = true;
            body = jsonOf(
              admin.send(::drogon::Post, "/api/admin/users", {}, &create), ::drogon::k201Created
            );
            user.id = body["user"]["id"].asString();
            if (user.id.empty())
                throw std::runtime_error("C1a fixture user identity missing");
        }
        Json::Value create;
        create["name"] = "c1a-" + ::drogon::utils::getUuid();
        create["redirect_uris"] = kRedirect;
        create["client_type"] = "PUBLIC";
        create["allowed_grant_types"] = "authorization_code";
        body = jsonOf(
          admin.send(::drogon::Post, "/api/admin/clients", {}, &create), ::drogon::k201Created
        );
        client = body["client_id"].asString();
        if (client.empty())
            throw std::runtime_error("C1a fixture client identity missing");
        Json::Value scopes;
        scopes["scopes"] = Json::Value(Json::arrayValue);
        scopes["scopes"].append("openid");
        scopes["scopes"].append("profile");
        jsonOf(
          admin.send(::drogon::Put, "/api/admin/clients/" + client + "/scopes", {}, &scopes),
          ::drogon::k200OK
        );
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        for (;;)
        {
            const double remaining =
              std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0)
                throw std::runtime_error("C1a fixture scope commit not visible");
            body = jsonOf(
              admin.send(
                ::drogon::Get,
                "/api/admin/clients/" + client + "/scopes",
                {},
                nullptr,
                (std::min)(remaining, http::fullaTestHttpTimeout())
              ),
              ::drogon::k200OK
            );
            if (!body["scopes"].isArray())
                throw std::runtime_error("C1a fixture scope shape invalid");
            std::set<std::string> actual;
            for (const auto &scope : body["scopes"])
            {
                if (!scope.isString())
                    throw std::runtime_error("C1a fixture scope type invalid");
                actual.insert(scope.asString());
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                throw std::runtime_error("C1a fixture scope commit deadline exceeded");
            if (actual == std::set<std::string>{"openid", "profile"})
                break;
            std::this_thread::sleep_until((std::min)(deadline,
                                                     now + std::chrono::milliseconds(10)));
        }
        std::cout << "[c1a] created 2 isolated users and 1 retained UUID client\n";
    }

    bool retire()
    {
        bool ok = true;
        for (const auto &user : users)
        {
            if (!user.id.empty())
                ok = admin.send(::drogon::Put, "/api/admin/users/" + user.id + "/disable")
                         ->getStatusCode() == ::drogon::k200OK &&
                     ok;
        }
        retired = ok;
        return ok;
    }

    ::drogon::HttpResponsePtr authorize(const Flow &flow, const std::string &prompt = "consent")
    {
        auto params = flow.parameters(client);
        params.emplace_back("response_type", "code");
        params.emplace_back("prompt", prompt);
        return browser.send(::drogon::Get, "/oauth2/authorize", params);
    }

    Json::Value downstream(const Flow &flow, const std::string &expectedSub)
    {
        auto response = authorize(flow);
        if (response->getStatusCode() != ::drogon::k302Found)
            throw std::runtime_error(
              "C1a fixture authorize did not redirect: status=" +
              std::to_string(static_cast<int>(response->getStatusCode()))
            );
        const auto location = response->getHeader("Location");
        const auto subject = queryValue(location, "user_id");
        const auto csrf = queryValue(location, "consent_csrf");
        if (subject != expectedSub || csrf.empty() || queryValue(location, "state") != flow.state)
            throw std::runtime_error("C1a fixture downstream session identity or CSRF mismatch");
        auto params = flow.parameters(client);
        params.emplace_back("user_id", subject);
        params.emplace_back("consent_csrf", csrf);
        params.emplace_back("action", "approve");
        response = browser.send(::drogon::Post, "/oauth2/consent", params);
        if (response->getStatusCode() != ::drogon::k302Found)
            throw std::runtime_error("C1a fixture consent did not redirect");
        const auto code = queryValue(response->getHeader("Location"), "code");
        if (code.empty() || queryValue(response->getHeader("Location"), "state") != flow.state)
            throw std::runtime_error("C1a fixture consent code/state missing");
        return jwtClaims(exchange(browser, client, code, flow)["id_token"].asString());
    }
};
}  // namespace

DROGON_TEST(Integration_P0_PasswordLoginSession_ReloginAndRejectedCredentials)
{
    try
    {
        if (!http::postgresAvailable())
            return;
        Fixture f;
        f.setup();
        f.browser.send(::drogon::Get, "/.well-known/openid-configuration");
        const auto anonymous = f.browser.sessionId();
        REQUIRE(!anonymous.empty());
        std::array<std::string, 2> subjects;
        for (size_t i = 0; i < f.users.size(); ++i)
        {
            const auto prior = f.browser.sessionId();
            const auto start = nowSeconds();
            Flow flow;
            const auto body = jsonOf(
              login(f.browser, f.users[i].name, f.users[i].password, f.client, flow),
              ::drogon::k200OK
            );
            const auto end = nowSeconds();
            REQUIRE(!f.browser.sessionId().empty());
            CHECK(f.browser.sessionId() != prior);
            CHECK(body.size() == 2);
            REQUIRE(body["code"].isString());
            CHECK(queryValue(body["location"].asString(), "code") == body["code"].asString());
            CHECK(queryValue(body["location"].asString(), "state") == flow.state);
            auto tokens = exchange(f.browser, f.client, body["code"].asString(), flow);
            const auto claims = jwtClaims(tokens["id_token"].asString());
            Browser identity;
            identity.bearer = tokens["access_token"].asString();
            const auto info =
              jsonOf(identity.send(::drogon::Get, "/oauth2/userinfo"), ::drogon::k200OK);
            CHECK(info["preferred_username"].asString() == f.users[i].name);
            REQUIRE(claims["sub"].isString());
            subjects[i] = claims["sub"].asString();
            CHECK(subjects[i] == info["sub"].asString());
            CHECK(claims["nonce"].asString() == flow.nonce);
            REQUIRE(claims["amr"].isArray());
            CHECK(claims["amr"].size() == 1);
            CHECK(claims["amr"][0].asString() == "pwd");
            CHECK(claims["auth_time"].asInt64() >= start);
            CHECK(claims["auth_time"].asInt64() <= end);
            if (i)
            {
                CHECK(subjects[0] != subjects[1]);
                const auto downstream = f.downstream(Flow{}, subjects[1]);
                CHECK(downstream["sub"].asString() == subjects[1]);
                CHECK(downstream["amr"] == claims["amr"]);
                CHECK(downstream["auth_time"] == claims["auth_time"]);
            }
        }
        for (int rejected = 0; rejected < 3; ++rejected)
        {
            Flow flow;
            auto params = flow.parameters(f.client);
            params.emplace_back(
              "username", rejected == 1 ? "absent_" + ::drogon::utils::getUuid() : f.users[1].name
            );
            if (rejected != 2)
                params.emplace_back(
                  "password", rejected == 0 ? "incorrect-C1a-password" : f.users[1].password
                );
            const auto cookie = f.browser.sessionId();
            const auto response = f.browser.send(::drogon::Post, "/oauth2/login?json=true", params);
            const auto body = jsonOf(
              response, rejected == 2 ? ::drogon::k400BadRequest : ::drogon::k401Unauthorized
            );
            REQUIRE(body["error"].isObject());
            if (rejected != 2)
                CHECK(body["error"]["code"].asString() == "AUTH_INVALID_CREDENTIALS");
            CHECK(!body.isMember("code"));
            CHECK(f.browser.sessionId() == cookie);
            CHECK(f.downstream(Flow{}, subjects[1])["sub"].asString() == subjects[1]);
        }
        CHECK(f.retire());
    }
    catch (const std::exception &error)
    {
        std::cerr << "[c1a] fixture failed: " << error.what() << "\n";
        CHECK(false);
    }
}

DROGON_TEST(Integration_P0_PasswordLoginSession_ForcedThenMfaThenPasswordOnly)
{
    try
    {
        if (!http::postgresAvailable())
            return;
        Fixture f;
        f.setup();
        Flow initial;
        auto body = jsonOf(
          login(f.browser, f.users[0].name, f.users[0].password, f.client, initial),
          ::drogon::k200OK
        );
        auto tokens = exchange(f.browser, f.client, body["code"].asString(), initial);
        const auto subject = jwtClaims(tokens["id_token"].asString())["sub"].asString();
        Browser mfa;
        mfa.bearer = tokens["access_token"].asString();
        const auto originalIdentity =
          jsonOf(mfa.send(::drogon::Get, "/oauth2/userinfo"), ::drogon::k200OK);
        REQUIRE(originalIdentity["preferred_username"].asString() == f.users[0].name);
        REQUIRE(originalIdentity["sub"].asString() == subject);
        body = jsonOf(mfa.send(::drogon::Post, "/api/me/mfa/setup"), ::drogon::k200OK);
        REQUIRE(body["secret"].isString());
        const auto secret = body["secret"].asString();
        REQUIRE(!secret.empty());
        Json::Value verify;
        verify["code"] = ::fulla::identity::totp::generateCode(secret, nowSeconds());
        jsonOf(mfa.send(::drogon::Post, "/api/me/mfa/verify", {}, &verify), ::drogon::k200OK);
        Json::Value flags;
        flags["must_change_password"] = true;
        flags["email_verified"] = false;
        jsonOf(
          f.admin.send(::drogon::Put, "/api/admin/users/" + f.users[0].id, {}, &flags),
          ::drogon::k200OK
        );
        const auto detail = jsonOf(
          f.admin.send(::drogon::Get, "/api/admin/users/" + f.users[0].id), ::drogon::k200OK
        );
        REQUIRE(detail["mfa_enabled"].asBool());
        REQUIRE(detail["must_change_password"].asBool());
        // Email verification is disabled in the standard fixture: no email-policy claim.
        REQUIRE(!detail["email_verified"].asBool());
        body = jsonOf(
          login(f.browser, f.users[0].name, f.users[0].password, f.client, Flow{}), ::drogon::k200OK
        );
        CHECK(body["password_change_required"].asBool());
        CHECK(!body.isMember("mfa_required"));
        CHECK(!body.isMember("code"));
        auto response = f.authorize(Flow{}, "none");
        REQUIRE(response->getStatusCode() == ::drogon::k302Found);
        CHECK(queryValue(response->getHeader("Location"), "error") == "login_required");
        Flow forced;
        auto params = forced.parameters(f.client);
        params.emplace_back("user_id", subject);
        params.emplace_back("action", "approve");
        params.emplace_back("consent_csrf", "not-consumed-before-forced-gate");
        body = jsonOf(
          f.browser.send(::drogon::Post, "/oauth2/consent", params), ::drogon::k403Forbidden
        );
        CHECK(body["error"]["code"].asString() == "AUTH_PASSWORD_CHANGE_REQUIRED");
        params.emplace_back("mfa_token", subject);
        params.emplace_back("code", ::fulla::identity::totp::generateCode(secret, nowSeconds()));
        body = jsonOf(
          f.browser.send(::drogon::Post, "/oauth2/mfa/verify", params), ::drogon::k403Forbidden
        );
        CHECK(body["error"]["code"].asString() == "AUTH_PASSWORD_CHANGE_REQUIRED");
        response = login(f.browser, f.users[0].name, f.users[0].password, f.client, Flow{}, false);
        REQUIRE(response->getStatusCode() == ::drogon::k302Found);
        CHECK(response->getHeader("Location").find("must_change_password=1") != std::string::npos);
        CHECK(queryValue(response->getHeader("Location"), "code").empty());

        flags["must_change_password"] = false;
        flags["email_verified"] = true;
        jsonOf(
          f.admin.send(::drogon::Put, "/api/admin/users/" + f.users[0].id, {}, &flags),
          ::drogon::k200OK
        );
        Flow secondFactor;
        body = jsonOf(
          login(f.browser, f.users[0].name, f.users[0].password, f.client, secondFactor),
          ::drogon::k200OK
        );
        REQUIRE(body["mfa_required"].asBool());
        REQUIRE(body["mfa_token"].asString() == subject);
        CHECK(!body.isMember("code"));
        response = f.authorize(Flow{}, "none");
        REQUIRE(response->getStatusCode() == ::drogon::k302Found);
        CHECK(queryValue(response->getHeader("Location"), "error") == "login_required");
        auto pending = secondFactor.parameters(f.client);
        pending.emplace_back("user_id", subject);
        pending.emplace_back("action", "approve");
        pending.emplace_back("consent_csrf", "not-consumed-before-mfa-gate");
        const auto refused = jsonOf(
          f.browser.send(::drogon::Post, "/oauth2/consent", pending), ::drogon::k401Unauthorized
        );
        CHECK(refused["error"]["code"].asString() == "AUTH_MFA_REQUIRED");
        params = secondFactor.parameters(f.client);
        params.emplace_back("mfa_token", subject);
        params.emplace_back("code", ::fulla::identity::totp::generateCode(secret, nowSeconds()));
        params.emplace_back("code_verifier", secondFactor.verifier);
        // A legitimate MFA flow establishes the prior claims. This does not test
        // rejected verifiers or sign off the separate #350 PKCE ordering defect.
        tokens =
          jsonOf(f.browser.send(::drogon::Post, "/oauth2/mfa/verify", params), ::drogon::k200OK);
        const auto mfaClaims = jwtClaims(tokens["id_token"].asString());
        REQUIRE(mfaClaims["sub"].asString() == subject);
        REQUIRE(mfaClaims["amr"].isArray());
        REQUIRE(mfaClaims["amr"].size() == 2);
        REQUIRE(mfaClaims["amr"][0].asString() == "pwd");
        REQUIRE(mfaClaims["amr"][1].asString() == "mfa");
        // A second code issued from the same browser proves the SESSION
        // already holds MFA claims, independently of verifyLogin token fields.
        const auto elevated = f.downstream(Flow{}, subject);
        REQUIRE(elevated["sub"].asString() == subject);
        REQUIRE(elevated["amr"] == mfaClaims["amr"]);
        REQUIRE(elevated["auth_time"] == mfaClaims["auth_time"]);
        const auto prior = f.browser.sessionId();
        Flow passwordOnly;
        body = jsonOf(
          login(f.browser, f.users[1].name, f.users[1].password, f.client, passwordOnly),
          ::drogon::k200OK
        );
        REQUIRE(!f.browser.sessionId().empty());
        CHECK(f.browser.sessionId() != prior);
        tokens = exchange(f.browser, f.client, body["code"].asString(), passwordOnly);
        const auto claims = jwtClaims(tokens["id_token"].asString());
        CHECK(claims["sub"].asString() != subject);
        Browser replacementIdentity;
        replacementIdentity.bearer = tokens["access_token"].asString();
        const auto replacement =
          jsonOf(replacementIdentity.send(::drogon::Get, "/oauth2/userinfo"), ::drogon::k200OK);
        REQUIRE(replacement["preferred_username"].asString() == f.users[1].name);
        REQUIRE(replacement["sub"].asString() == claims["sub"].asString());
        REQUIRE(claims["amr"].isArray());
        CHECK(claims["amr"].size() == 1);
        CHECK(claims["amr"][0].asString() == "pwd");
        CHECK(f.downstream(Flow{}, claims["sub"].asString())["amr"] == claims["amr"]);
        CHECK(f.retire());
    }
    catch (const std::exception &error)
    {
        std::cerr << "[c1a] fixture failed: " << error.what() << "\n";
        CHECK(false);
    }
}
