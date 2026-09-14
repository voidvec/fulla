// tests/integration/oidc/ConsentMultiFlowHttpTest.cc
//
// #144: (a) the consent CSRF nonce lives in a bounded multi-slot list, so two
// concurrent authorize flows on one session no longer overwrite each other's
// slot (the single-slot regression made the first consent page permanently
// un-submittable); (b) a session paused at the MFA challenge (amr="pwd",
// mfa_pending marker) must NOT pass the consent/authorize gates as a fully
// authenticated session.
#include <drogon/drogon_test.h>
#include <drogon/HttpClient.h>
#include <fulla/identity/TotpUtils.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/drogon/utils/PasswordHasher.h>
#include <json/json.h>

#include <future>
#include <sstream>
#include <string>

#include "../../common/HttpTestClient.h"

using namespace drogon;
using namespace drogon::orm;

namespace
{

HttpResponsePtr post(const std::string &path, const std::string &body, const std::string &cookie = "")
{
    auto client = HttpClient::newHttpClient("http://127.0.0.1:5555", app().getLoop());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Post);
    req->setPath(path);
    req->setBody(body);
    req->setContentTypeCode(::drogon::CT_APPLICATION_X_FORM);
    if (!cookie.empty())
        req->addHeader("Cookie", cookie);
    auto [result, resp] = client->sendRequest(req, 30.0);
    if (result != ReqResult::Ok)
        return nullptr;
    return resp;
}

std::string loginCookie(const std::string &username, const std::string &password)
{
    auto resp = fulla::test::http::sendPostForm(
      "/oauth2/login?json=true",
      "username=" + username + "&password=" + password +
        "&client_id=fulla-admin-console&redirect_uri=http%3A%2F%2F127.0.0.1%3A5174%2Fadmin%2Fcallback"
        "&scope=openid&state=g1&code_challenge=F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po&code_challenge_method=S256"
    );
    if (!resp || resp->getStatusCode() != k200OK)
        return "";
    std::string cookie;
    for (const auto &entry : resp->getCookies())
    {
        if (!cookie.empty())
            cookie += "; ";
        cookie += entry.first + "=" + entry.second.value();
    }
    return cookie;
}

// authorize (prompt=consent) with a per-flow state; extracts the minted
// consent_csrf and user_id. Returns false on any failure.
bool authorizeMintNonce(
  const std::string &cookieIn,
  const std::string &state,
  std::string &csrf,
  std::string &userId,
  std::string &cookieOut
)
{
    cookieOut = cookieIn;
    auto client = HttpClient::newHttpClient("http://127.0.0.1:5555", app().getLoop());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/oauth2/authorize");
    req->setParameter("response_type", "code");
    req->setParameter("client_id", "fulla-portal");
    req->setParameter("redirect_uri", "http://127.0.0.1:5173/callback");
    req->setParameter("scope", "openid");
    req->setParameter("state", state);
    req->setParameter("prompt", "consent");
    req->setParameter("code_challenge", "F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po");
    req->setParameter("code_challenge_method", "plain");
    req->addHeader("Cookie", cookieOut);
    auto [result, resp] = client->sendRequest(req, 30.0);
    if (result != ReqResult::Ok || resp->getStatusCode() != k302Found)
        return false;
    for (const auto &entry : resp->getCookies())
    {
        std::string name = entry.first + "=";
        if (cookieOut.find(name) == std::string::npos)
            cookieOut += "; " + name + entry.second.value();
    }
    auto location = resp->getHeader("Location");
    auto csrfPos = location.find("consent_csrf=");
    auto uidPos = location.find("user_id=");
    if (csrfPos == std::string::npos || uidPos == std::string::npos)
        return false;
    auto csrfEnd = location.find('&', csrfPos);
    csrf = location.substr(csrfPos + 13,
                           (csrfEnd == std::string::npos ? location.size() : csrfEnd) - csrfPos - 13);
    auto uidEnd = location.find('&', uidPos);
    userId = location.substr(uidPos + 8,
                             (uidEnd == std::string::npos ? location.size() : uidEnd) - uidPos - 8);
    return !csrf.empty() && !userId.empty();
}

HttpResponsePtr authorize(const std::string &cookie, const std::string &extraParam = "")
{
    auto client = HttpClient::newHttpClient("http://127.0.0.1:5555", app().getLoop());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/oauth2/authorize");
    req->setParameter("response_type", "code");
    req->setParameter("client_id", "fulla-portal");
    req->setParameter("redirect_uri", "http://127.0.0.1:5173/callback");
    req->setParameter("scope", "openid");
    req->setParameter("state", "silentstate01");
    req->setParameter("code_challenge", "F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po");
    req->setParameter("code_challenge_method", "plain");
    if (!extraParam.empty())
        req->setParameter("prompt", extraParam);
    req->addHeader("Cookie", cookie);
    auto [result, resp] = client->sendRequest(req, 30.0);
    if (result != ReqResult::Ok)
        return nullptr;
    return resp;
}

// Sync SQL helper (future/promise pattern used by the MfaCrossClient tests).
bool execSql(const std::string &sql)
{
    auto db = app().getDbClient();
    if (!db)
        return false;
    std::promise<bool> p;
    db->execSqlAsync(
      sql,
      [&](const Result &) { p.set_value(true); },
      [&](const DrogonDbException &) { p.set_value(false); }
    );
    return p.get_future().get();
}

// PR #157 review MINOR 7: RAII restore for tests that flip the shared admin
// row's MFA state. A REQUIRE failure aborts the case mid-flow; the guard's
// destructor still runs, so the seeded admin can never leak to the rest of
// the suite with mfa_enabled=true (which would cascade login failures).
struct AdminMfaRestore
{
    bool armed = true;
    explicit AdminMfaRestore() = default;
    AdminMfaRestore(const AdminMfaRestore &) = delete;
    AdminMfaRestore &operator=(const AdminMfaRestore &) = delete;
    ~AdminMfaRestore()
    {
        if (armed)
            execSql("UPDATE users SET mfa_enabled = false, mfa_secret = NULL WHERE username = 'admin'");
    }
};


// Internal id of the seeded admin (Gate 2 needs user_id = session["userId"]).
std::string adminInternalId()
{
    auto db = app().getDbClient();
    if (!db)
        return "";
    std::promise<std::string> p;
    db->execSqlAsync(
      "SELECT id FROM users WHERE username = 'admin'",
      [&](const Result &rows) {
          p.set_value(rows.empty() ? "" : std::to_string(rows[0]["id"].as<int32_t>()));
      },
      [&](const DrogonDbException &) { p.set_value(""); }
    );
    return p.get_future().get();
}

}  // namespace

// Two concurrent authorize flows on one session (same client, two tabs ->
// two states): BOTH minted nonces must remain submittable. The single-slot
// implementation made the first flow's consent page fail with a hard 400.
DROGON_TEST(Integration_P1_ConsentMultiFlow_TwoConcurrentNonces_BothApprove)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    auto cookie = loginCookie("admin", "admin");
    REQUIRE(!cookie.empty());

    std::string csrf1, csrf2, userId, cookieA, cookieB;
    REQUIRE(authorizeMintNonce(cookie, "multiflowstate1", csrf1, userId, cookieA));
    REQUIRE(authorizeMintNonce(cookie, "multiflowstate2", csrf2, userId, cookieB));
    REQUIRE(!csrf1.empty());
    REQUIRE(csrf1 != csrf2);

    // First flow's consent still validates after the second mint.
    auto approve1 = post(
      "/oauth2/consent",
      "client_id=fulla-portal&user_id=" + userId +
        "&scope=openid&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback&state=multiflowstate1&consent_csrf=" +
        csrf1 + "&action=approve",
      cookieA
    );
    REQUIRE(approve1 != nullptr);
    CHECK(approve1->getStatusCode() == k302Found);
    CHECK(approve1->getHeader("Location").find("code=") != std::string::npos);

    // Second flow's consent validates too (its slot was not overwritten).
    auto approve2 = post(
      "/oauth2/consent",
      "client_id=fulla-portal&user_id=" + userId +
        "&scope=openid&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback&state=multiflowstate2&consent_csrf=" +
        csrf2 + "&action=approve",
      cookieB
    );
    REQUIRE(approve2 != nullptr);
    CHECK(approve2->getStatusCode() == k302Found);
    CHECK(approve2->getHeader("Location").find("code=") != std::string::npos);
}

// The slot list is bounded (kMaxSlots = 5): the 6th concurrent mint evicts
// the oldest entry, so THAT consent fails with the standard expired/mismatch
// 400 (not a silent success and not a 500).
DROGON_TEST(Integration_P1_ConsentMultiFlow_NonceCapEvictsOldest)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    auto cookie = loginCookie("admin", "admin");
    REQUIRE(!cookie.empty());

    std::string csrfOldest, csrfNewest, userId, cookieOldest, cookieNewest;
    REQUIRE(authorizeMintNonce(cookie, "capstate01", csrfOldest, userId, cookieOldest));
    for (int i = 2; i <= 5; ++i)
    {
        std::string csrf, uid, cookieTmp;
        REQUIRE(authorizeMintNonce(cookie, "capstate0" + std::to_string(i), csrf, uid, cookieTmp));
    }
    REQUIRE(authorizeMintNonce(cookie, "capstate06", csrfNewest, userId, cookieNewest));

    // The 6th mint evicted the oldest -> its nonce is no longer consumable.
    auto evicted = post(
      "/oauth2/consent",
      "client_id=fulla-portal&user_id=" + userId +
        "&scope=openid&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback&state=capstate01&consent_csrf=" +
        csrfOldest + "&action=approve",
      cookieOldest
    );
    REQUIRE(evicted != nullptr);
    CHECK(evicted->getStatusCode() == k400BadRequest);

    // The newest nonce is still live.
    auto live = post(
      "/oauth2/consent",
      "client_id=fulla-portal&user_id=" + userId +
        "&scope=openid&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback&state=capstate06&consent_csrf=" +
        csrfNewest + "&action=approve",
      cookieNewest
    );
    REQUIRE(live != nullptr);
    CHECK(live->getStatusCode() == k302Found);
}

// A session paused at the MFA challenge (amr="pwd", mfa_pending marker) must
// be refused by the consent gate with 401 AUTH_MFA_REQUIRED — passing Gate 1
// (login writes userId/sub before the MFA decision) but granting consent
// would mint a code for an MFA-required account without the second factor.
DROGON_TEST(Integration_P1_MfaPending_Consent_Returns401)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    REQUIRE(execSql("UPDATE users SET mfa_enabled = true, mfa_secret = 'JBSWY3DPEHPK3PXP' WHERE username = 'admin'"));
    AdminMfaRestore mfaRestore;

    // First factor only -> mfa_required response, session is MFA-pending.
    auto resp = fulla::test::http::sendPostForm(
      "/oauth2/login?json=true",
      "username=admin&password=admin"
      "&client_id=fulla-admin-console&redirect_uri=http%3A%2F%2F127.0.0.1%3A5174%2Fadmin%2Fcallback"
      "&scope=openid&state=g1&code_challenge=F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po&code_challenge_method=S256"
    );
    REQUIRE(resp != nullptr);
    REQUIRE(resp->getStatusCode() == k200OK);
    Json::Value body;
    REQUIRE(fulla::test::http::parseJsonBody(resp, body));
    CHECK(body.get("mfa_required", false).asBool() == true);

    std::string cookie;
    for (const auto &entry : resp->getCookies())
    {
        if (!cookie.empty())
            cookie += "; ";
        cookie += entry.first + "=" + entry.second.value();
    }
    REQUIRE(!cookie.empty());

    // Restore the seed state BEFORE asserting: the mfa_pending SESSION marker
    // intentionally survives until the next login, and the DB flag must not
    // leak into other tests.

    const std::string userId = adminInternalId();
    REQUIRE(!userId.empty());

    auto consent = post(
      "/oauth2/consent",
      "client_id=fulla-portal&user_id=" + userId +
        "&scope=openid&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback&state=g1&consent_csrf=anything&action=approve",
      cookie
    );
    REQUIRE(consent != nullptr);
    CHECK(consent->getStatusCode() == k401Unauthorized);
    Json::Value errBody;
    REQUIRE(fulla::test::http::parseJsonBody(consent, errBody));
    CHECK(errBody["error"]["code"].asString() == "AUTH_MFA_REQUIRED");
}

// The authorize endpoint must treat an MFA-pending session as NOT logged in:
// silent authorize redirects to the login screen, and prompt=none answers
// with error=login_required (OIDC Core §3.1.2.1: no UI may be shown).
DROGON_TEST(Integration_P1_MfaPending_Authorize_BlockedAndPromptNone_LoginRequired)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    REQUIRE(execSql("UPDATE users SET mfa_enabled = true, mfa_secret = 'JBSWY3DPEHPK3PXP' WHERE username = 'admin'"));
    AdminMfaRestore mfaRestore;

    auto resp = fulla::test::http::sendPostForm(
      "/oauth2/login?json=true",
      "username=admin&password=admin"
      "&client_id=fulla-admin-console&redirect_uri=http%3A%2F%2F127.0.0.1%3A5174%2Fadmin%2Fcallback"
      "&scope=openid&state=g1&code_challenge=F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po&code_challenge_method=S256"
    );
    REQUIRE(resp != nullptr);
    REQUIRE(resp->getStatusCode() == k200OK);

    std::string cookie;
    for (const auto &entry : resp->getCookies())
    {
        if (!cookie.empty())
            cookie += "; ";
        cookie += entry.first + "=" + entry.second.value();
    }
    REQUIRE(!cookie.empty());


    // Silent authorize: the MFA-pending session must NOT receive a code —
    // it is routed to the login screen like an anonymous session.
    auto silent = authorize(cookie);
    REQUIRE(silent != nullptr);
    CHECK(silent->getStatusCode() == k302Found);
    CHECK(silent->getHeader("Location").find("/login") != std::string::npos);
    CHECK(silent->getHeader("Location").find("code=") == std::string::npos);

    // prompt=none: error redirect to the client with login_required, never UI.
    auto promptNone = authorize(cookie, "none");
    REQUIRE(promptNone != nullptr);
    CHECK(promptNone->getStatusCode() == k302Found);
    auto location = promptNone->getHeader("Location");
    CHECK(location.find("http://127.0.0.1:5173/callback") != std::string::npos);
    CHECK(location.find("error=login_required") != std::string::npos);
}

// A FAILED TOTP verification must not leave the session MFA-elevated: the
// amr/auth_time session writes happen only after the code actually verifies
// (#144 companion fix — the pre-fix code wrote amr="pwd mfa" before
// verification). Combined with the mfa_pending gate the failed-verify session
// is still refused at authorize.
DROGON_TEST(Integration_P1_MfaWrongCode_SessionNotElevated)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    REQUIRE(execSql("UPDATE users SET mfa_enabled = true, mfa_secret = 'JBSWY3DPEHPK3PXP' WHERE username = 'admin'"));
    AdminMfaRestore mfaRestore;

    auto resp = fulla::test::http::sendPostForm(
      "/oauth2/login?json=true",
      "username=admin&password=admin"
      "&client_id=fulla-admin-console&redirect_uri=http%3A%2F%2F127.0.0.1%3A5174%2Fadmin%2Fcallback"
      "&scope=openid&state=g1&code_challenge=F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po&code_challenge_method=S256"
    );
    REQUIRE(resp != nullptr);
    REQUIRE(resp->getStatusCode() == k200OK);
    Json::Value body;
    REQUIRE(fulla::test::http::parseJsonBody(resp, body));
    const std::string mfaToken = body.get("mfa_token", "").asString();
    REQUIRE(!mfaToken.empty());

    std::string cookie;
    for (const auto &entry : resp->getCookies())
    {
        if (!cookie.empty())
            cookie += "; ";
        cookie += entry.first + "=" + entry.second.value();
    }
    REQUIRE(!cookie.empty());

    // Wrong TOTP code -> 401; the session must not become MFA-elevated.
    auto wrong = post(
      "/oauth2/mfa/verify",
      "mfa_token=" + mfaToken + "&code=000000"
      "&client_id=fulla-admin-console&redirect_uri=http%3A%2F%2F127.0.0.1%3A5174%2Fadmin%2Fcallback"
      "&scope=openid",
      cookie
    );
    REQUIRE(wrong != nullptr);
    CHECK(wrong->getStatusCode() == k401Unauthorized);


    // The session stays MFA-pending (never elevated) -> authorize is refused.
    auto silent = authorize(cookie);
    REQUIRE(silent != nullptr);
    CHECK(silent->getStatusCode() == k302Found);
    CHECK(silent->getHeader("Location").find("/login") != std::string::npos);
    CHECK(silent->getHeader("Location").find("code=") == std::string::npos);
}

// PR #157 review MINOR 8: a DIRECT assertion that a failed TOTP attempt does
// not leave amr elevated. The refusal-based cases above are also guaranteed
// by the mfa_pending gate alone, so they would pass even with the pre-fix
// (verify-time) amr write. This case inspects the actual id_token amr claim:
//
//   1. enable MFA, login (mfa_required), submit a WRONG TOTP code
//   2. disable MFA (row flag), re-login password-only with a known PKCE pair
//   3. exchange the issued code and decode the id_token payload
//
// Pre-fix (amr written before verification + bare-insert re-login) the
// stale "pwd mfa" survives step 2's insert and the id_token carries
// amr=["pwd","mfa"]; post-fix it must be exactly ["pwd"].
DROGON_TEST(Integration_P0_MfaWrongCodeThenRelogin_IdTokenAmrStaysPwd)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    REQUIRE(execSql("UPDATE users SET mfa_enabled = true, mfa_secret = 'JBSWY3DPEHPK3PXP' WHERE username = 'admin'"));
    AdminMfaRestore mfaRestore;

    // Step 1: first factor + wrong TOTP.
    auto login1 = fulla::test::http::sendPostForm(
      "/oauth2/login?json=true",
      "username=admin&password=admin"
      "&client_id=fulla-admin-console&redirect_uri=http%3A%2F%2F127.0.0.1%3A5174%2Fadmin%2Fcallback"
      "&scope=openid&state=g1&code_challenge=F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po&code_challenge_method=S256"
    );
    REQUIRE(login1 != nullptr);
    REQUIRE(login1->getStatusCode() == k200OK);
    Json::Value body1;
    REQUIRE(fulla::test::http::parseJsonBody(login1, body1));
    const std::string mfaToken = body1.get("mfa_token", "").asString();
    REQUIRE(!mfaToken.empty());
    std::string cookie1;
    for (const auto &entry : login1->getCookies())
    {
        if (!cookie1.empty())
            cookie1 += "; ";
        cookie1 += entry.first + "=" + entry.second.value();
    }
    auto wrong = post(
      "/oauth2/mfa/verify",
      "mfa_token=" + mfaToken + "&code=000000"
      "&client_id=fulla-admin-console&redirect_uri=http%3A%2F%2F127.0.0.1%3A5174%2Fadmin%2Fcallback"
      "&scope=openid",
      cookie1
    );
    REQUIRE(wrong != nullptr);
    CHECK(wrong->getStatusCode() == k401Unauthorized);

    // Step 2: MFA administratively disabled, password-only re-login with a
    // known PKCE pair (RFC 7636: verifier -> S256 challenge).
    REQUIRE(execSql("UPDATE users SET mfa_enabled = false, mfa_secret = NULL WHERE username = 'admin'"));
    const std::string verifier = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string challenge = ::fulla::drogon::utils::computeCodeChallenge(verifier, "S256");
    auto login2 = fulla::test::http::sendPostForm(
      "/oauth2/login?json=true",
      "username=admin&password=admin"
      "&client_id=fulla-portal&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback"
      "&scope=openid&state=g2&code_challenge=" + challenge + "&code_challenge_method=S256"
    );
    REQUIRE(login2 != nullptr);
    REQUIRE(login2->getStatusCode() == k200OK);
    Json::Value body2;
    REQUIRE(fulla::test::http::parseJsonBody(login2, body2));
    const std::string code = body2.get("code", "").asString();
    REQUIRE(!code.empty());

    // Step 3: exchange and decode the id_token payload.
    auto tokenResp = fulla::test::http::sendPostForm(
      "/oauth2/token",
      "grant_type=authorization_code&code=" + code +
        "&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback&client_id=fulla-portal&code_verifier=" +
        verifier
    );
    REQUIRE(tokenResp != nullptr);
    REQUIRE(tokenResp->getStatusCode() == k200OK);
    Json::Value tokens;
    REQUIRE(fulla::test::http::parseJsonBody(tokenResp, tokens));
    const std::string idToken = tokens.get("id_token", "").asString();
    REQUIRE(!idToken.empty());

    auto payloadPos = idToken.find('.');
    auto signaturePos = idToken.find('.', payloadPos + 1);
    REQUIRE(payloadPos != std::string::npos);
    REQUIRE(signaturePos != std::string::npos);
    std::string b64 = idToken.substr(payloadPos + 1, signaturePos - payloadPos - 1);
    // base64url -> base64 (+/ with padding) for drogon::utils::base64Decode.
    for (auto &c : b64)
    {
        if (c == '-')
            c = '+';
        else if (c == '_')
            c = '/';
    }
    while (b64.size() % 4 != 0)
        b64 += '=';
    const std::string decoded = ::drogon::utils::base64Decode(b64);
    Json::Value claims;
    {
        Json::CharReaderBuilder reader;
        std::istringstream stream(decoded);
        std::string errors;
        REQUIRE(Json::parseFromStream(reader, stream, &claims, &errors));
    }
    // amr must reflect ONLY the methods actually performed (OIDC Core
    // §2 / RFC 8176): a single password factor, no "mfa".
    REQUIRE(claims.isMember("amr"));
    std::string amrJoined;
    if (claims["amr"].isArray())
    {
        for (const auto &v : claims["amr"])
            amrJoined += v.asString() + " ";
    }
    else
    {
        amrJoined = claims["amr"].asString();
    }
    CHECK(amrJoined.find("mfa") == std::string::npos);
    CHECK(amrJoined.find("pwd") != std::string::npos);
}
