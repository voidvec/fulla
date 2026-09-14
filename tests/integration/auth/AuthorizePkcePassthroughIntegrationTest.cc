// tests/integration/auth/AuthorizePkcePassthroughIntegrationTest.cc
//
// P0 #1 (评审问题点有效性分析报告, review finding 1): /oauth2/authorize's
// silent re-authorization path (logged-in session + recorded consent) used to
// read only response_type/client_id/redirect_uri/scope/state and silently
// dropped code_challenge/code_challenge_method/nonce, so codes issued on that
// path had an empty stored challenge -- and TokenService's PKCE check is
// conditional on a stored challenge (RFC 7636 §4.4), so PKCE was bypassed
// entirely for returning users.
//
// This test drives the REAL direct-issue path over live HTTP (Postgres mode):
//   1. seed consent for admin x fulla-portal x openid (so authorize skips the
//      consent screen), log in via POST /oauth2/login to obtain the session
//      cookie;
//   2. GET /oauth2/authorize with a plain code_challenge + session cookie ->
//      must 302 straight to the client redirect_uri (NOT /login or /consent)
//      with a code;
//   3. exchanging that code WITHOUT code_verifier must fail invalid_grant
//      (proves the challenge was persisted with the code);
//   4. a second authorize round + exchange WITH the correct verifier must
//      succeed.
//
// Known residual gap (documented, out of P0 scope): oauth2_codes has no nonce
// column, so nonce -- while now threaded through the HTTP layers on every
// path -- is not persisted by the Postgres grant repository (pre-existing
// schema limitation affecting login/consent paths equally). This test
// therefore asserts the PKCE round-trip only.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <drogon/HttpClient.h>
#include <drogon/utils/Utilities.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <json/json.h>

#include <chrono>
#include <future>
#include <string>
#include <thread>

using namespace drogon;
using namespace drogon::orm;

namespace
{
constexpr const char *kBaseUrl = "http://127.0.0.1:5555";
constexpr const char *kRedirectUri = "http://127.0.0.1:5173/callback";
constexpr const char *kState = "pkce_e2e_state_01";
// 43-char PKCE verifier; with method "plain" the challenge is the verifier
// itself (RFC 7636 §4.2), which keeps the test free of base64url/sha256
// plumbing while still exercising the storage round-trip.
constexpr const char *kVerifier = "abcdefghijklmnopqrstuvwxyzABCDEF0123456789~";

bool parseBody(const HttpResponsePtr &resp, Json::Value &out)
{
    const std::string body(resp->getBody());
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;
    return reader->parse(body.data(), body.data() + body.size(), &out, &errs);
}

HttpResponsePtr sendReq(const HttpRequestPtr &req)
{
    try
    {
        auto client = HttpClient::newHttpClient(kBaseUrl);
        auto [result, resp] = client->sendRequest(req, /*timeout=*/30.0);
        if (result != ReqResult::Ok || resp == nullptr)
            return nullptr;
        return resp;
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "sendReq failed (server likely unreachable): " << e.what();
        return nullptr;
    }
}

bool serverReachable()
{
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        req->setPath("/nonexistent-probe");
        if (sendReq(req) != nullptr)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

// Insert the consent row the direct-issue branch needs (admin x fulla-portal x
// openid). Returns the number of rows actually inserted (0 = pre-existing),
// -1 on error -- so the guard only deletes what this test created.
long ensureAdminConsent()
{
    auto db = app().getDbClient();
    if (!db)
        return -1;
    std::promise<long> p;
    db->execSqlAsync(
      "INSERT INTO oauth2_user_consents (internal_user_id, client_id, scope_name) "
      "SELECT u.id, 'fulla-portal', 'openid' FROM users u WHERE u.username = 'admin' "
      "ON CONFLICT (internal_user_id, client_id, scope_name) DO NOTHING",
      [&](const Result &r) { p.set_value(static_cast<long>(r.affectedRows())); },
      [&](const DrogonDbException &) { p.set_value(-1); }
    );
    return p.get_future().get();
}

void removeAdminConsent()
{
    auto db = app().getDbClient();
    if (!db)
        return;
    std::promise<void> p;
    db->execSqlAsync(
      "DELETE FROM oauth2_user_consents WHERE client_id = 'fulla-portal' "
      "AND scope_name = 'openid' AND internal_user_id = "
      "(SELECT id FROM users WHERE username = 'admin')",
      [&](const Result &) { p.set_value(); },
      [&](const DrogonDbException &) { p.set_value(); }
    );
    p.get_future().get();
}

// POST /oauth2/login as the seeded admin user; returns the JSESSIONID session
// cookie value ("" on failure). json=true makes the handler answer JSON
// instead of a 302 so the cookie is the only thing we need off the response.
std::string loginAndGetSessionCookie()
{
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Post);
    req->setPath("/oauth2/login");
    req->setContentTypeCode(CT_APPLICATION_X_FORM);
    req->setBody(
      std::string("username=admin&password=admin&client_id=fulla-portal") +
      "&redirect_uri=" + utils::urlEncodeComponent(kRedirectUri) + "&scope=openid&state=" + kState +
      "&response_type=code&json=true"
    );
    auto resp = sendReq(req);
    if (!resp)
        return "";
    return resp->getCookie("JSESSIONID").value();
}

HttpResponsePtr callAuthorize(const std::string &sessionCookie)
{
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/oauth2/authorize");
    req->setParameter("response_type", "code");
    req->setParameter("client_id", "fulla-portal");
    req->setParameter("redirect_uri", kRedirectUri);
    req->setParameter("scope", "openid");
    req->setParameter("state", kState);
    req->setParameter("code_challenge", kVerifier);
    req->setParameter("code_challenge_method", "plain");
    req->setParameter("nonce", "pkce_e2e_nonce_01");
    req->addCookie("JSESSIONID", sessionCookie);
    return sendReq(req);
}

std::string extractCode(const std::string &location)
{
    auto pos = location.find("code=");
    if (pos == std::string::npos)
        return "";
    auto end = location.find('&', pos);
    return location.substr(pos + 5, end == std::string::npos ? std::string::npos : end - pos - 5);
}

HttpResponsePtr exchangeToken(const std::string &code, const std::string &verifier)
{
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Post);
    req->setPath("/oauth2/token");
    req->setContentTypeCode(CT_APPLICATION_X_FORM);
    std::string body = "grant_type=authorization_code&client_id=fulla-portal&code=" + code +
                       "&redirect_uri=" + utils::urlEncodeComponent(kRedirectUri);
    if (!verifier.empty())
        body += "&code_verifier=" + std::string(verifier);
    req->setBody(body);
    return sendReq(req);
}
}  // namespace

DROGON_TEST(Integration_P0_AuthorizeDirectIssue_PkcePassthrough_TokenEnforcesVerifier)
{
    auto plugin = app().getPlugin<OAuth2Plugin>();
    if (!plugin || plugin->getStorageType() == "memory")
    {
        // Fixture (admin user / fulla-portal / consent rows) is Postgres seed
        // data; memory mode has no equivalent direct-issue fixture.
        CHECK(true);
        return;
    }
    if (!serverReachable())
    {
        LOG_INFO << "Skipping: HTTP listener not reachable on " << kBaseUrl;
        CHECK(true);
        return;
    }

    long inserted = ensureAdminConsent();
    REQUIRE(inserted >= 0);

    struct Guard
    {
        bool doDelete;

        ~Guard()
        {
            if (doDelete)
                removeAdminConsent();
        }
    } guard{inserted > 0};

    std::string session = loginAndGetSessionCookie();
    REQUIRE(!session.empty());

    // Round A: the direct-issue branch must go straight to the client
    // redirect_uri (not /login, not /consent) and the issued code must carry
    // the challenge -- proven by the verifier-less exchange being rejected.
    auto respA = callAuthorize(session);
    REQUIRE(respA != nullptr);
    CHECK(respA->getStatusCode() == k302Found);
    const std::string locA = respA->getHeader("location");
    CHECK(locA.rfind(kRedirectUri, 0) == 0);
    const std::string codeA = extractCode(locA);
    REQUIRE(!codeA.empty());

    auto noVerifier = exchangeToken(codeA, "");
    REQUIRE(noVerifier != nullptr);
    Json::Value errBody;
    REQUIRE(parseBody(noVerifier, errBody));
    // Before the fix the stored challenge was empty, so this exchange
    // SUCCEEDED (PKCE silently skipped). invalid_grant proves persistence.
    CHECK(errBody["error"].asString() == "invalid_grant");
    CHECK(noVerifier->getStatusCode() == k400BadRequest);

    // Round B: with the correct verifier the exchange succeeds end to end.
    auto respB = callAuthorize(session);
    REQUIRE(respB != nullptr);
    CHECK(respB->getStatusCode() == k302Found);
    const std::string codeB = extractCode(respB->getHeader("location"));
    REQUIRE(!codeB.empty());

    auto withVerifier = exchangeToken(codeB, kVerifier);
    REQUIRE(withVerifier != nullptr);
    Json::Value okBody;
    REQUIRE(parseBody(withVerifier, okBody));
    CHECK(okBody.isMember("access_token"));
    CHECK(okBody["token_type"].asString() == "Bearer");
}
