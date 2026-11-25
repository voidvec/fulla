#include <drogon/drogon_test.h>
#include <fulla/drogon/utils/PortalUrl.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include "../../common/HttpTestClient.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
namespace http = ::fulla::test::http;
namespace crypto = ::fulla::drogon::utils;
using Params = std::vector<std::pair<std::string, std::string>>;
constexpr const char *kCallback = "http://127.0.0.1:9986/callback";
constexpr const char *kQueryCallback = "http://127.0.0.1:9986/callback?tenant=alpha%2Bbeta&mode=opaque";

void ensure(bool condition, const char *stage)
{
    if (!condition)
        throw std::runtime_error(stage);
}

std::vector<std::string> states()
{
    return {"opaque-safe", "opaque&value", "opaque=value", "opaque+value",
            "opaque value", "opaque%value", "opaque?value", "opaque#value",
            "opaque state+&=value%26?#", "opaque%26value", "opaque%2526value",
            "opaque&code=injected&state=other&client_id=other&error=injected",
            "opaque&amp;marker", "opaque&quot;marker",
            "opaque\"><script data-opaque-injected='yes'>x</script>",
            "opaque'<>value", "opaque-\xE4\xB8\xAD\xE6\x96\x87"};
}

Params queryPairs(const std::string &url)
{
    Params result;
    const auto query = url.find('?');
    const auto fragment = url.find('#');
    if (query == std::string::npos || (fragment != std::string::npos && fragment < query))
        return result;
    const auto end = fragment == std::string::npos ? url.size() : fragment;
    size_t start = query + 1;
    while (start < end)
    {
        auto next = url.find('&', start);
        if (next == std::string::npos || next > end)
            next = end;
        const auto item = url.substr(start, next - start);
        const auto equal = item.find('=');
        result.emplace_back(::drogon::utils::urlDecode(item.substr(0, equal)),
                            equal == std::string::npos ? "" :
                              ::drogon::utils::urlDecode(item.substr(equal + 1)));
        start = next + 1;
    }
    return result;
}

std::string one(const Params &pairs, const std::string &key)
{
    std::string value;
    size_t count = 0;
    for (const auto &pair : pairs)
        if (pair.first == key)
        {
            value = pair.second;
            ++count;
        }
    ensure(count == 1, "query field missing or duplicated");
    return value;
}

bool absent(const Params &pairs, const std::string &key)
{
    return std::none_of(pairs.begin(), pairs.end(),
                        [&key](const auto &pair) { return pair.first == key; });
}

void set(Params &params, const std::string &key, const std::string &value)
{
    for (auto &pair : params)
        if (pair.first == key)
        {
            pair.second = value;
            return;
        }
    params.emplace_back(key, value);
}

class Browser
{
  public:
    std::string bearer;
    std::map<std::string, std::string> cookies;

    ::drogon::HttpResponsePtr send(::drogon::HttpMethod method, const std::string &path,
                                  const Params &params = {}, bool jsonBody = false,
                                  const std::string &basic = "", double timeout = 10.0,
                                  const Json::Value *object = nullptr)
    {
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(method);
        req->setPath(path);
        if (method == ::drogon::Get)
        {
            for (const auto &pair : params)
                req->setParameter(pair.first, pair.second);
        }
        else if (jsonBody)
        {
            Json::Value body(Json::objectValue);
            for (const auto &pair : params)
                body[pair.first] = pair.second;
            if (object)
                body = *object;
            req->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
            req->setBody(Json::writeString(Json::StreamWriterBuilder(), body));
        }
        else
        {
            std::string body;
            for (const auto &pair : params)
            {
                if (!body.empty())
                    body += '&';
                body += ::drogon::utils::urlEncodeComponent(pair.first) + '=' +
                        ::drogon::utils::urlEncodeComponent(pair.second);
            }
            req->setContentTypeCode(::drogon::CT_APPLICATION_X_FORM);
            req->setBody(body);
        }
        if (!basic.empty())
            req->addHeader("Authorization", "Basic " + basic);
        else if (!bearer.empty())
            req->addHeader("Authorization", "Bearer " + bearer);
        for (const auto &cookie : cookies)
            req->addCookie(cookie.first, cookie.second);
        if (!client_)
            client_ = ::drogon::HttpClient::newHttpClient(http::kTestBaseUrl, ::drogon::app().getLoop());
        // A credential, code or write request is physically sent only once.
        const auto result = client_->sendRequest(req, timeout);
        ensure(result.first == ::drogon::ReqResult::Ok && result.second != nullptr,
               "opaque-state HTTP transport failed");
        for (const auto &cookie : result.second->getCookies())
            cookies[cookie.first] = cookie.second.value();
        return result.second;
    }

    ::drogon::HttpResponsePtr sendJson(::drogon::HttpMethod method, const std::string &path,
                                      const Json::Value &body)
    {
        return send(method, path, {}, true, "", 10.0, &body);
    }

  private:
    ::drogon::HttpClientPtr client_;
};

Json::Value jsonOf(const ::drogon::HttpResponsePtr &response, ::drogon::HttpStatusCode status)
{
    ensure(response && response->getStatusCode() == status, "opaque-state HTTP status mismatch");
    Json::Value body;
    ensure(http::parseJsonBody(response, body), "opaque-state JSON response malformed");
    return body;
}

struct Flow
{
    std::string state = "opaque-safe";
    std::string scope = "openid profile";
    std::string verifier = crypto::generateSecureToken(32);
    std::string nonce = "opaque+nonce&value=%26";
    std::string redirect = kQueryCallback;

    Params params(const std::string &client) const
    {
        return {{"client_id", client}, {"redirect_uri", redirect}, {"scope", scope},
                {"state", state}, {"response_type", "code"}, {"nonce", nonce},
                {"code_challenge", crypto::computeCodeChallenge(verifier, "S256")},
                {"code_challenge_method", "S256"}};
    }
};

std::string locationOf(const ::drogon::HttpResponsePtr &response, bool jsonMode,
                       const std::string &field)
{
    if (!jsonMode)
    {
        ensure(response->getStatusCode() == ::drogon::k302Found, "expected OAuth redirect");
        return response->getHeader("Location");
    }
    const auto body = jsonOf(response, ::drogon::k200OK);
    ensure(body[field].isString() && body["location"].isString(), "OAuth JSON envelope changed");
    const auto location = body["location"].asString();
    ensure(one(queryPairs(location), field) == body[field].asString(),
           "OAuth JSON field differs from location");
    return location;
}

std::string checkCallback(const std::string &location, const Flow &flow,
                          const std::string &field, const std::string &error = "")
{
    ensure(location.find('#') == std::string::npos && location.find('\r') == std::string::npos &&
             location.find('\n') == std::string::npos, "callback gained a fragment or raw newline");
    ensure(location.substr(0, location.find('?')) == kCallback, "callback base changed");
    const auto pairs = queryPairs(location);
    ensure(flow.state.empty() ? absent(pairs, "state") : one(pairs, "state") == flow.state,
           "opaque callback state changed");
    const size_t omittedState = flow.state.empty() ? 1u : 0u;
    if (flow.redirect == kQueryCallback)
    {
        ensure(one(pairs, "tenant") == "alpha+beta" && one(pairs, "mode") == "opaque",
               "registered callback query changed");
        ensure(pairs.size() == (field == "code" ? 4u : 5u) - omittedState, "callback gained extra parameters");
    }
    else
        ensure(pairs.size() == (field == "code" ? 2u : 3u) - omittedState, "callback gained extra parameters");
    ensure(absent(pairs, "client_id"), "state injected client_id");
    const auto value = one(pairs, field);
    ensure(!value.empty(), "OAuth result is empty");
    ensure(absent(pairs, field == "code" ? "error" : "code"), "state injected OAuth result");
    if (!error.empty())
        ensure(value == error, "OAuth error semantics changed");
    return value;
}

struct Client
{
    std::string id;
    std::string secret;
};

class Fixture
{
  public:
    Browser admin;
    std::string user = "opaque_" + ::drogon::utils::getUuid();
    std::string password = crypto::generateSecureToken(18) + "aA1!";
    std::string userId;
    Client clients[2];

    ~Fixture()
    {
        if (!retirementAttempted_)
            retire();
    }

    void setup()
    {
        Flow adminFlow;
        adminFlow.redirect = "http://127.0.0.1:5174/admin/callback";
        adminFlow.scope = "openid profile admin";
        auto params = adminFlow.params("fulla-admin-console");
        params.emplace_back("username", "admin");
        params.emplace_back("password", "admin");
        auto body = jsonOf(admin.send(::drogon::Post, "/oauth2/login?json=true", params),
                           ::drogon::k200OK);
        body = jsonOf(admin.send(::drogon::Post, "/oauth2/token",
                                {{"grant_type", "authorization_code"}, {"client_id", "fulla-admin-console"},
                                 {"redirect_uri", adminFlow.redirect}, {"code", body["code"].asString()},
                                 {"code_verifier", adminFlow.verifier}}), ::drogon::k200OK);
        admin.bearer = body["access_token"].asString();
        ensure(!admin.bearer.empty(), "opaque-state admin authentication failed");
        Json::Value create;
        create["username"] = user;
        create["password"] = password;
        create["email_verified"] = true;
        create["mfa_enabled"] = false;
        create["must_change_password"] = false;
        body = jsonOf(admin.sendJson(::drogon::Post, "/api/admin/users", create),
                      ::drogon::k201Created);
        userId = body["user"]["id"].asString();
        ensure(!userId.empty(), "opaque-state fixture user identity missing");
        for (auto &client : clients)
        {
            body = jsonOf(admin.send(::drogon::Post, "/api/admin/clients",
                         {{"name", "opaque-" + ::drogon::utils::getUuid()},
                          {"redirect_uris", std::string(kCallback) + "," + kQueryCallback},
                          {"client_type", "CONFIDENTIAL"},
                          {"token_endpoint_auth_method", "client_secret_basic"},
                          {"allowed_grant_types", "authorization_code"}}, true), ::drogon::k201Created);
            client.id = body["client_id"].asString();
            client.secret = body["client_secret"].asString();
            ensure(!client.id.empty() && !client.secret.empty(), "opaque-state confidential client missing");
            setScopes(client);
        }
        std::cout << "[opaque-state] created 1 isolated user and 2 retained UUID clients\n";
    }

    ::drogon::HttpResponsePtr login(Browser &browser, const Client &client, const Flow &flow,
                                    bool jsonMode, bool jsonBody = false, bool pkce = true)
    {
        auto params = flow.params(client.id);
        params.emplace_back("username", user);
        params.emplace_back("password", password);
        if (!pkce)
            set(params, "code_challenge", "");
        return browser.send(::drogon::Post, jsonMode ? "/oauth2/login?json=true" : "/oauth2/login",
                            params, jsonBody);
    }

    ::drogon::HttpResponsePtr authorize(Browser &browser, const Client &client,
                                        const Flow &flow, const std::string &prompt = "")
    {
        auto params = flow.params(client.id);
        if (!prompt.empty())
            params.emplace_back("prompt", prompt);
        return browser.send(::drogon::Get, "/oauth2/authorize", params);
    }

    ::drogon::HttpResponsePtr token(Browser &browser, const Client &client,
                                    const std::string &code, const std::string &verifier,
                                    const std::string &redirect)
    {
        return browser.send(::drogon::Post, "/oauth2/token",
                            {{"grant_type", "authorization_code"}, {"client_id", client.id},
                             {"redirect_uri", redirect}, {"code", code}, {"code_verifier", verifier}},
                            false, ::drogon::utils::base64Encode(client.id + ":" + client.secret));
    }

    void exchange(Browser &browser, const Client &client, const Flow &flow, const std::string &code)
    {
        const auto body = jsonOf(token(browser, client, code, flow.verifier, flow.redirect),
                                 ::drogon::k200OK);
        ensure(body["access_token"].isString() && !body["access_token"].asString().empty(),
               "opaque-state authorization code exchange failed");
    }

    bool retire() noexcept
    {
        retirementAttempted_ = true;
        if (userId.empty())
            return true;
        try
        {
            auto response = admin.send(::drogon::Put, "/api/admin/users/" + userId + "/disable");
            if (response->getStatusCode() != ::drogon::k200OK)
                return false;
            const auto body = jsonOf(admin.send(::drogon::Get, "/api/admin/users/" + userId),
                                      ::drogon::k200OK);
            const auto &record = body.isMember("user") ? body["user"] : body;
            const bool disabled = record.isMember("locked") && record["locked"].asBool() &&
                                  record["locked_until"].asInt64() > 0;
            std::cout << "[opaque-state] fixture user retirement " << (disabled ? "passed" : "FAILED") << '\n';
            return disabled;
        }
        catch (...)
        {
            std::cerr << "[opaque-state] fixture user retirement FAILED\n";
            return false;
        }
    }

  private:
    bool retirementAttempted_ = false;
    void setScopes(const Client &client);
};

void Fixture::setScopes(const Client &client)
{
    // Scope payload and response shape are independent of URL query encoding.
    Json::Value scopes;
    scopes["scopes"] = Json::Value(Json::arrayValue);
    scopes["scopes"].append("openid");
    scopes["scopes"].append("profile");
    jsonOf(admin.sendJson(::drogon::Put, "/api/admin/clients/" + client.id + "/scopes", scopes),
           ::drogon::k200OK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;)
    {
        const auto remaining = std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
        ensure(remaining > 0, "scope update visibility deadline exceeded");
        const auto body = jsonOf(admin.send(::drogon::Get, "/api/admin/clients/" + client.id + "/scopes",
                                            {}, false, "", (std::min)(remaining, 10.0)), ::drogon::k200OK);
        std::set<std::string> actual;
        for (const auto &scope : body["scopes"])
            actual.insert(scope.asString());
        if (actual == std::set<std::string>{"openid", "profile"})
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void writeFile(const std::filesystem::path &path, const std::string &value)
{
    std::ofstream file(path, std::ios::binary);
    file.write(value.data(), static_cast<std::streamsize>(value.size()));
    file.flush();
    ensure(file.good(), "opaque-state fixture write failed");
    file.close();
    ensure(!file.fail(), "opaque-state fixture close failed");
}
}  // namespace

DROGON_TEST(Integration_P1_OpaqueState_LoginHtmlRoundTrip)
{
    try
    {
        Browser browser;
        Json::Value artifacts(Json::arrayValue);
        const char *directory = std::getenv("FULLA_OPAQUE_STATE_EVIDENCE_DIR");
        for (const auto &state : states())
        {
            Params fields{{"client_id", "opaque-client&\"marker"},
                          {"redirect_uri", kQueryCallback}, {"scope", "openid profile"},
                          {"response_type", "code"}, {"state", state},
                          {"code_challenge", "opaque+challenge&value=%26"},
                          {"code_challenge_method", "S256"}, {"nonce", "opaque&quot;nonce"}};
            const auto response = browser.send(::drogon::Get, "/login", fields);
            REQUIRE(response->getStatusCode() == ::drogon::k200OK);
            const std::string html(response->getBody());
            CHECK(html.find("<form") != std::string::npos);
            CHECK(html.find("<script data-opaque-injected") == std::string::npos);
            Json::Value fixture;
            for (const auto &field : fields)
                fixture["fields"][field.first] = field.second;
            const auto config = ::drogon::app().getCustomConfig();
            fixture["register_url"] = config.isMember("frontend") ?
                config["frontend"].get("url", "http://localhost:5173").asString() +
                config["frontend"].get("register_path", "/register").asString() :
                "http://localhost:5173/register";
            fixture["file"] = "login-" + std::to_string(artifacts.size()) + ".html";
            const auto synthetic = std::string(kQueryCallback) + "&code=opaque-test-code&state=" +
                                   ::drogon::utils::urlEncodeComponent(state);
            CHECK(one(queryPairs(synthetic), "state") == state);
            fixture["query_url"] = synthetic;
            if (directory)
                writeFile(std::filesystem::path(directory) / fixture["file"].asString(), html);
            artifacts.append(fixture);
        }
        if (directory)
            writeFile(std::filesystem::path(directory) / "fixtures.json",
                      Json::writeString(Json::StreamWriterBuilder(), artifacts));
    }
    catch (const std::exception &error)
    {
        std::cerr << "[opaque-state] " << error.what() << '\n';
        CHECK(false);
    }
}

DROGON_TEST(Integration_P1_OpaqueState_OAuthHttpRoundTrip)
{
    if (!http::postgresAvailable())
    {
        std::cout << "[opaque-state] PG-dependent OAuth matrix not exercised in memory mode\n";
        return;
    }
    try
    {
        Fixture fixture;
        fixture.setup();
        const auto &client = fixture.clients[0];
        const auto &other = fixture.clients[1];
        {
            Browser browser;
            Flow flow;
            flow.redirect = kCallback;
            flow.state = "opaque state+&=value%26?#";
            const auto response = fixture.login(browser, client, flow, true);
            fixture.exchange(browser, client, flow,
                checkCallback(locationOf(response, true, "code"), flow, "code"));
            CHECK(true);
        }
        for (const auto &state : states())
        {
            for (int mode = 0; mode < 4; ++mode)
            {
                Browser browser;
                Flow flow;
                flow.state = state;
                flow.scope = mode % 2 ? "" : "openid profile";
                flow.redirect = mode % 2 ? kCallback : kQueryCallback;
                const bool jsonMode = mode < 2;
                const auto response = fixture.login(browser, client, flow, jsonMode);
                const auto code = checkCallback(locationOf(response, jsonMode, "code"), flow, "code");
                fixture.exchange(browser, client, flow, code);
                CHECK(true);
            }
            Browser browser;
            Flow flow;
            flow.state = state;
            for (const auto &scope : {std::string("openid profile"), std::string("")})
            {
                flow.scope = scope;
                const auto response = fixture.login(browser, client, flow, true, true);
                fixture.exchange(browser, client, flow,
                    checkCallback(locationOf(response, true, "code"), flow, "code"));
                CHECK(true);
            }
            flow.scope = "openid profile";
            Browser anonymous;
            auto response = fixture.authorize(anonymous, client, flow);
            ensure(response->getStatusCode() == ::drogon::k302Found, "authorize login forward rejected opaque state");
            auto forward = queryPairs(response->getHeader("Location"));
            ensure(one(forward, "state") == state && one(forward, "redirect_uri") == flow.redirect &&
                     one(forward, "nonce") == flow.nonce && one(forward, "client_id") == client.id &&
                     one(forward, "scope") == flow.scope && one(forward, "response_type") == "code" &&
                     one(forward, "code_challenge") == crypto::computeCodeChallenge(flow.verifier, "S256") &&
                     one(forward, "code_challenge_method") == "S256", "authorize login forward changed fields");
            ensure(forward.size() == queryPairs(crypto::authorizeLoginUrl()).size() + 8,
                   "authorize login forward gained fields");
            CHECK(true);
            Browser silent;
            response = fixture.authorize(silent, client, flow, "none");
            checkCallback(locationOf(response, false, "error"), flow, "error", "login_required");
            CHECK(true);
            for (bool jsonMode : {false, true})
            {
                response = fixture.login(browser, client, flow, jsonMode, false, false);
                checkCallback(locationOf(response, jsonMode, "error"), flow, "error", "invalid_request");
                CHECK(true);
                for (const auto &action : {std::string("deny"), std::string("approve")})
                {
                    response = fixture.authorize(browser, other, flow, "consent");
                    ensure(response->getStatusCode() == ::drogon::k302Found, "authorize consent forward failed");
                    forward = queryPairs(response->getHeader("Location"));
                    ensure(one(forward, "state") == state && one(forward, "redirect_uri") == flow.redirect &&
                             one(forward, "nonce") == flow.nonce && one(forward, "client_id") == other.id &&
                             one(forward, "scope") == flow.scope &&
                             one(forward, "code_challenge") == crypto::computeCodeChallenge(flow.verifier, "S256") &&
                             one(forward, "code_challenge_method") == "S256",
                           "authorize consent forward changed fields");
                    ensure(forward.size() == queryPairs(crypto::authorizeConsentUrl()).size() + 9,
                           "authorize consent forward gained fields");
                    Params params;
                    for (const auto &key : {"client_id", "redirect_uri", "scope", "state", "nonce",
                                            "code_challenge", "code_challenge_method", "user_id", "consent_csrf"})
                        params.emplace_back(key, one(forward, key));
                    params.emplace_back("action", action);
                    response = browser.send(::drogon::Post,
                        jsonMode ? "/oauth2/consent?json=true" : "/oauth2/consent", params);
                    const std::string field = action == "approve" ? "code" : "error";
                    const auto value = checkCallback(locationOf(response, jsonMode, field), flow, field,
                                                     action == "deny" ? "access_denied" : "");
                    if (action == "approve")
                        fixture.exchange(browser, other, flow, value);
                    CHECK(true);
                }
            }
            // Prior consent must also preserve state on the direct code path.
            response = fixture.authorize(browser, other, flow);
            fixture.exchange(browser, other, flow,
                checkCallback(locationOf(response, false, "code"), flow, "code"));
            CHECK(true);
            Browser logout;
            response = logout.send(::drogon::Get, "/oauth2/end_session",
                {{"client_id", client.id}, {"post_logout_redirect_uri", flow.redirect}, {"state", state}});
            ensure(response->getStatusCode() == ::drogon::k302Found, "end-session redirect failed");
            const auto logoutPairs = queryPairs(response->getHeader("Location"));
            ensure(one(logoutPairs, "state") == state && one(logoutPairs, "tenant") == "alpha+beta" &&
                     logoutPairs.size() == 3u, "end-session state/query changed");
            CHECK(true);
        }
        // Only authorize requires 8-512 bytes. The other endpoints retain
        // optional/short state and omit an empty state on their callbacks.
        for (const auto &optionalState : {std::string(""), std::string("s")})
        {
            for (bool jsonMode : {false, true})
            {
                Browser optionalBrowser;
                Flow optional;
                optional.state = optionalState;
                for (const auto &scope : {std::string("openid profile"), std::string("")})
                {
                    optional.scope = scope;
                    const auto response = fixture.login(optionalBrowser, client, optional, jsonMode);
                    fixture.exchange(optionalBrowser, client, optional,
                        checkCallback(locationOf(response, jsonMode, "code"), optional, "code"));
                    CHECK(true);
                }
                optional.scope = "openid profile";
                Flow authorizeFlow = optional;
                authorizeFlow.state = "opaque-valid";
                auto response = fixture.authorize(optionalBrowser, other, authorizeFlow, "consent");
                ensure(response->getStatusCode() == ::drogon::k302Found, "optional state consent setup failed");
                const auto forwarded = queryPairs(response->getHeader("Location"));
                Params params;
                for (const auto &key : {"client_id", "redirect_uri", "scope", "state", "nonce",
                                        "code_challenge", "code_challenge_method", "user_id", "consent_csrf"})
                    params.emplace_back(key, one(forwarded, key));
                set(params, "state", optionalState);
                params.emplace_back("action", "approve");
                response = optionalBrowser.send(::drogon::Post,
                    jsonMode ? "/oauth2/consent?json=true" : "/oauth2/consent", params);
                fixture.exchange(optionalBrowser, other, optional,
                    checkCallback(locationOf(response, jsonMode, "code"), optional, "code"));
                CHECK(true);
                Browser logout;
                response = logout.send(jsonMode ? ::drogon::Post : ::drogon::Get, "/oauth2/end_session",
                    {{"client_id", client.id}, {"post_logout_redirect_uri", optional.redirect}, {"state", optionalState}});
                ensure(response->getStatusCode() == ::drogon::k302Found, "optional logout state rejected");
                const auto pairs = queryPairs(response->getHeader("Location"));
                const bool preserved = optionalState.empty() ? absent(pairs, "state") :
                                       one(pairs, "state") == optionalState;
                CHECK(preserved);
            }
        }
        Flow flow;
        for (size_t length : {size_t{7}, size_t{8}, size_t{512}, size_t{513}, size_t{0}})
        {
            Browser browser;
            flow.state = std::string(length, 's');
            const auto response = fixture.authorize(browser, client, flow);
            CHECK(response->getStatusCode() == ((length == 8 || length == 512) ?
                ::drogon::k302Found : ::drogon::k400BadRequest));
        }
        flow.state = "opaque-safe";
        for (int invalid = 0; invalid < 3; ++invalid)
        {
            Browser browser;
            const auto response = fixture.login(browser, client, flow, true);
            const auto code = checkCallback(locationOf(response, true, "code"), flow, "code");
            const auto rejected = fixture.token(browser, invalid == 2 ? other : client, code,
                invalid == 0 ? crypto::generateSecureToken(32) : flow.verifier,
                invalid == 1 ? "http://127.0.0.1:9986/unregistered" : flow.redirect);
            const auto body = jsonOf(rejected, ::drogon::k400BadRequest);
            CHECK(body["error"].asString() == "invalid_grant");
        }
        Browser browser;
        Flow invalid;
        invalid.redirect = "http://127.0.0.1:9986/unregistered";
        CHECK(fixture.authorize(browser, client, invalid)->getStatusCode() == ::drogon::k400BadRequest);
        const auto invalidLogout = browser.send(::drogon::Get, "/oauth2/end_session",
            {{"client_id", client.id}, {"post_logout_redirect_uri", invalid.redirect}, {"state", invalid.state}});
        CHECK(invalidLogout->getStatusCode() == ::drogon::k400BadRequest);
        CHECK(browser.send(::drogon::Get, "/oauth2/end_session")->getStatusCode() == ::drogon::k200OK);
        CHECK(fixture.retire());
    }
    catch (const std::exception &error)
    {
        std::cerr << "[opaque-state] " << error.what() << '\n';
        CHECK(false);
    }
}

DROGON_TEST(Integration_P1_OpaqueState_AuthorizeDelimiterRoundTrip)
{
    if (!http::postgresAvailable())
    {
        std::cout << "[opaque-state] PG-dependent seeded authorize case not exercised in memory mode\n";
        return;
    }
    try
    {
        Browser browser;
        Flow flow;
        flow.state = "opaque state+&=value%26?#";
        flow.scope = "openid";
        flow.redirect = "http://127.0.0.1:5174/admin/callback";
        const auto response = browser.send(::drogon::Get, "/oauth2/authorize",
                                           flow.params("fulla-admin-console"));
        REQUIRE(response->getStatusCode() == ::drogon::k302Found);
        const auto pairs = queryPairs(response->getHeader("Location"));
        CHECK(one(pairs, "state") == flow.state);
        CHECK(one(pairs, "redirect_uri") == flow.redirect);
        CHECK(one(pairs, "client_id") == "fulla-admin-console");
    }
    catch (const std::exception &error)
    {
        std::cerr << "[opaque-state] " << error.what() << '\n';
        CHECK(false);
    }
}
