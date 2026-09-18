// tests/integration/oidc/ConsentAuthGateHttpTest.cc
//
// F1 (PR tranche1): POST /oauth2/consent must be session-authenticated,
// user-bound, CSRF-nonce protected, and the deny redirect validated —
// previously the handler trusted the form's user_id with no session check,
// so anyone knowing a user id could approve/deny on their behalf.
#include <drogon/drogon_test.h>
#include <drogon/HttpClient.h>
#include "../../common/HttpTestClient.h"

#include <iostream>
#include <string>

using namespace drogon;

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
}  // namespace

DROGON_TEST(Integration_P1_Consent_NoSession_Returns401)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    auto resp = post("/oauth2/consent",
                     "client_id=fulla-portal&user_id=1&scope=openid&redirect_uri=http%3A%2F%2Flocalhost%3A5173%2Fcallback&state=x&action=approve");
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == k401Unauthorized);
}

DROGON_TEST(Integration_P1_Consent_UserMismatch_Returns403)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    auto cookie = loginCookie("admin", "admin");
    REQUIRE(!cookie.empty());
    // user_id != session["userId"] (admin's internal id)
    auto resp = post("/oauth2/consent",
                     "client_id=fulla-portal&user_id=999999&scope=openid&redirect_uri=http%3A%2F%2Flocalhost%3A5173%2Fcallback&state=x&consent_csrf=anything&action=approve",
                     cookie);
    CHECK(resp->getStatusCode() == k403Forbidden);
}


// Drive authorize (prompt=consent) with the session cookie and extract the
// minted consent_csrf nonce + session user_id from the redirect Location.
// Returns false if any step fails. Session cookies are merged in-place.
bool authorizeMintNonce(const std::string &cookieIn, std::string &csrf,
                        std::string &userId, std::string &cookieOut)
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
    req->setParameter("state", "consentstate01");
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

DROGON_TEST(Integration_P1_Consent_MissingNonce_Returns400)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    auto cookie = loginCookie("admin", "admin");
    REQUIRE(!cookie.empty());
    std::string csrf, userId, cookie2;
    REQUIRE(authorizeMintNonce(cookie, csrf, userId, cookie2));
    // Correct session user + minted slot, but the consent_csrf form field is
    // absent -> Gate 3 rejects with 400 (NOT the gate-2 403 nor a 500).
    auto resp = post(
      "/oauth2/consent",
      "client_id=fulla-portal&user_id=" + userId +
        "&scope=openid&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback&state=consentstate01&action=approve",
      cookie2
    );
    CHECK(resp->getStatusCode() == k400BadRequest);
}

DROGON_TEST(Integration_P1_Consent_DenyUnregisteredUri_Returns400)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    auto cookie = loginCookie("admin", "admin");
    REQUIRE(!cookie.empty());
    std::string csrf, userId, cookie2;
    REQUIRE(authorizeMintNonce(cookie, csrf, userId, cookie2));

    // Gate 4: deny to an unregistered redirect_uri must NOT 302 (open
    // redirect) — 400 envelope instead.
    auto deny = post(
      "/oauth2/consent",
      "client_id=fulla-portal&user_id=" + userId +
        "&scope=openid&redirect_uri=https%3A%2F%2Fevil.example%2Fcb&state=consentstate01&consent_csrf=" +
        csrf + "&action=deny",
      cookie2
    );
    CHECK(deny->getStatusCode() == k400BadRequest);

    // Deny to the REGISTERED redirect_uri -> 302 with error=access_denied.
    // (The nonce above was consumed by the failed deny? No: gate 3 erases the
    // slot only on successful verification — the deny above passed gate 3 and
    // consumed it, so mint a fresh one.)
    std::string csrf2, userId2, cookie3;
    REQUIRE(authorizeMintNonce(cookie, csrf2, userId2, cookie3));
    auto denyOk = post(
      "/oauth2/consent",
      "client_id=fulla-portal&user_id=" + userId2 +
        "&scope=openid&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback&state=consentstate02&consent_csrf=" +
        csrf2 + "&action=deny",
      cookie3
    );
    REQUIRE(denyOk->getStatusCode() == k302Found);
    CHECK(denyOk->getHeader("Location").find("error=access_denied") != std::string::npos);
}

DROGON_TEST(Integration_P1_Consent_NonceRoundTrip_ApprovesAndRejectsReplay)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    auto cookie = loginCookie("admin", "admin");
    REQUIRE(!cookie.empty());

    // authorize (prompt=consent) must 302 to the consent screen carrying a
    // server-minted consent_csrf and the session user_id.
    auto client = HttpClient::newHttpClient("http://127.0.0.1:5555", app().getLoop());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/oauth2/authorize");
    req->setParameter("response_type", "code");
    req->setParameter("client_id", "fulla-portal");
    req->setParameter("redirect_uri", "http://127.0.0.1:5173/callback");
    req->setParameter("scope", "openid");
    req->setParameter("state", "consentstate01");
    req->setParameter("prompt", "consent");
    req->setParameter("code_challenge", "F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po");
    req->setParameter("code_challenge_method", "plain");
    req->addHeader("Cookie", cookie);
    auto [result, resp] = client->sendRequest(req, 30.0);
    REQUIRE(result == ReqResult::Ok);
    REQUIRE(resp->getStatusCode() == k302Found);
    // Merge any cookies the authorize round-trip set (session writes may
    // re-issue) into the outgoing cookie string.
    for (const auto &entry : resp->getCookies())
    {
        std::string name = entry.first + "=";
        if (cookie.find(name) == std::string::npos)
            cookie += "; " + name + entry.second.value();
    }
    auto location = resp->getHeader("Location");
    CHECK(location.find("consent_csrf=") != std::string::npos);
    auto csrfPos = location.find("consent_csrf=");
    auto csrfEnd = location.find('&', csrfPos);
    std::string csrf = location.substr(
      csrfPos + 13, (csrfEnd == std::string::npos ? location.size() : csrfEnd) - csrfPos - 13
    );
    auto uidPos = location.find("user_id=");
    auto uidEnd = location.find('&', uidPos);
    std::string userId = location.substr(
      uidPos + 8, (uidEnd == std::string::npos ? location.size() : uidEnd) - uidPos - 8
    );
    REQUIRE(!csrf.empty());
    REQUIRE(!userId.empty());

    // #143: the admin's (local, <id>) mapping comes from the dev seed (and
    // is converged by the V027 backfill + startup self-heal) — no manual
    // re-seeding workaround here anymore; consent's getInternalUserId must
    // resolve the session user through the mapping table on its own.

    // Approve with the minted nonce -> 302 back to the client with a code.
    auto approve = post(
      "/oauth2/consent",
      "client_id=fulla-portal&user_id=" + userId +
        "&scope=openid&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback" + "&state=consentstate01&code_challenge=F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po&code_challenge_method=plain&consent_csrf=" + csrf +
        "&action=approve",
      cookie
    );
    CHECK(approve->getStatusCode() == k302Found);

    // One-shot: replaying the same nonce must fail.
    auto replay = post(
      "/oauth2/consent",
      "client_id=fulla-portal&user_id=" + userId +
        "&scope=openid&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback" + "&state=consentstate01&consent_csrf=" + csrf +
        "&action=approve",
      cookie
    );
    CHECK(replay->getStatusCode() == k400BadRequest);
}
