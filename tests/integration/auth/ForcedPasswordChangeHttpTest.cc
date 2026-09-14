// tests/integration/auth/ForcedPasswordChangeHttpTest.cc
//
// #145: accounts flagged must_change_password (bootstrap admin, admin-created
// users) must not receive authorization codes: login answers
// password_change_required, authorize redirects to the change page
// (login_required under prompt=none), consent answers 403, and the dedicated
// session-authenticated POST /oauth2/password/change is the working path
// (old_password verified, flag cleared, normal login resumes).
#include <drogon/drogon_test.h>
#include <drogon/HttpClient.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/drogon/utils/PasswordHasher.h>
#include <json/json.h>

#include <future>
#include <string>

#include "../../common/HttpTestClient.h"

using namespace drogon;
using namespace drogon::orm;

namespace
{

const char *kTestUser = "forced_pwd_user_1";

// Throwaway credentials minted at runtime for the dedicated test user (fresh
// per run; nothing reusable leaves the process).
const std::string &oldPassword()
{
    static const std::string v = ::fulla::drogon::utils::generateSecureToken(12) + "aA1!";
    return v;
}

const std::string &newPassword()
{
    static const std::string v = ::fulla::drogon::utils::generateSecureToken(12) + "bB2!";
    return v;
}

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

bool execSqlBind(const std::string &sql, const std::string &param)
{
    auto db = app().getDbClient();
    if (!db)
        return false;
    std::promise<bool> p;
    db->execSqlAsync(
      sql,
      [&](const Result &) { p.set_value(true); },
      [&](const DrogonDbException &) { p.set_value(false); },
      param
    );
    return p.get_future().get();
}

// Idempotent: (re)create the dedicated test user with the flag set and the
// run's old password.
bool ensureFlaggedUser()
{
    const std::string hash = fulla::common::utils::PasswordHasher::hash(oldPassword());
    if (!execSqlBind(
          "INSERT INTO users (username, password_hash, salt, email, must_change_password) "
          "VALUES ('" +
            std::string(kTestUser) + "', $1, '', 'forced@example.com', true) "
            "ON CONFLICT (username) DO NOTHING",
          hash
        ))
        return false;
    return execSqlBind(
      "UPDATE users SET must_change_password = true, password_hash = $1 WHERE username = '" +
        std::string(kTestUser) + "'",
      hash
    );
}

bool userFlag(const std::string &username, bool &flag)
{
    auto db = app().getDbClient();
    if (!db)
        return false;
    std::promise<bool> p;
    db->execSqlAsync(
      "SELECT must_change_password FROM users WHERE username = $1",
      [&](const Result &rows) {
          if (rows.empty())
          {
              p.set_value(false);
              return;
          }
          flag = rows[0]["must_change_password"].as<bool>();
          p.set_value(true);
      },
      [&](const DrogonDbException &) { p.set_value(false); },
      username
    );
    return p.get_future().get();
}

std::string userInternalId(const std::string &username)
{
    auto db = app().getDbClient();
    if (!db)
        return "";
    std::promise<std::string> p;
    db->execSqlAsync(
      "SELECT id FROM users WHERE username = $1",
      [&](const Result &rows) {
          p.set_value(rows.empty() ? "" : std::to_string(rows[0]["id"].as<int32_t>()));
      },
      [&](const DrogonDbException &) { p.set_value(""); },
      username
    );
    return p.get_future().get();
}

// Login form for the dedicated test user (fulla-portal, PKCE S256).
HttpResponsePtr loginUser()
{
    return fulla::test::http::sendPostForm(
      "/oauth2/login?json=true",
      std::string("username=") + kTestUser + "&password=" + oldPassword() +
        "&client_id=fulla-portal&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback"
        "&scope=openid&state=pwdstate001&code_challenge=F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po&code_challenge_method=S256"
    );
}

std::string cookieOf(const HttpResponsePtr &resp)
{
    std::string cookie;
    for (const auto &entry : resp->getCookies())
    {
        if (!cookie.empty())
            cookie += "; ";
        cookie += entry.first + "=" + entry.second.value();
    }
    return cookie;
}

HttpResponsePtr authorize(
  const std::string &cookie,
  const std::string &prompt = "",
  const std::string &clientId = "fulla-portal",
  const std::string &redirectUri = "http://127.0.0.1:5173/callback"
)
{
    auto client = HttpClient::newHttpClient("http://127.0.0.1:5555", app().getLoop());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/oauth2/authorize");
    req->setParameter("response_type", "code");
    req->setParameter("client_id", clientId);
    req->setParameter("redirect_uri", redirectUri);
    req->setParameter("scope", "openid");
    req->setParameter("state", "pwdstate001");
    req->setParameter("code_challenge", "F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po");
    req->setParameter("code_challenge_method", "plain");
    if (!prompt.empty())
        req->setParameter("prompt", prompt);
    req->addHeader("Cookie", cookie);
    auto [result, resp] = client->sendRequest(req, 30.0);
    if (result != ReqResult::Ok)
        return nullptr;
    return resp;
}

HttpResponsePtr postChange(const std::string &cookie, const std::string &oldPwd, const std::string &newPwd)
{
    auto client = HttpClient::newHttpClient("http://127.0.0.1:5555", app().getLoop());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Post);
    req->setPath("/oauth2/password/change");
    Json::Value body;
    body["old_password"] = oldPwd;
    body["new_password"] = newPwd;
    Json::StreamWriterBuilder wb;
    req->setBody(Json::writeString(wb, body));
    req->setContentTypeCode(CT_APPLICATION_JSON);
    req->addHeader("Cookie", cookie);
    auto [result, resp] = client->sendRequest(req, 30.0);
    if (result != ReqResult::Ok)
        return nullptr;
    return resp;
}

}  // namespace

DROGON_TEST(Integration_P0_ForcedPasswordChange_LoginReturnsFlag_NoCode)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    REQUIRE(ensureFlaggedUser());

    auto resp = loginUser();
    REQUIRE(resp != nullptr);
    REQUIRE(resp->getStatusCode() == k200OK);
    Json::Value body;
    REQUIRE(fulla::test::http::parseJsonBody(resp, body));
    CHECK(body.get("password_change_required", false).asBool() == true);
    CHECK(!body.isMember("code"));
}

DROGON_TEST(Integration_P1_ForcedPasswordChange_Authorize_BlockedAndPromptNone)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    REQUIRE(ensureFlaggedUser());

    auto login = loginUser();
    REQUIRE(login != nullptr);
    REQUIRE(login->getStatusCode() == k200OK);
    const std::string cookie = cookieOf(login);
    REQUIRE(!cookie.empty());

    // Silent authorize: no code — routed to the frontend change-password page.
    auto silent = authorize(cookie);
    REQUIRE(silent != nullptr);
    CHECK(silent->getStatusCode() == k302Found);
    CHECK(silent->getHeader("Location").find("must_change_password=1") != std::string::npos);
    CHECK(silent->getHeader("Location").find("code=") == std::string::npos);

    // PR #157 review MAJOR 4: the redirect must follow the ORIGINATING
    // portal — fulla-portal goes to the user portal (frontend.url), the
    // fulla-admin-console client goes to the admin console (admin_console.url +
    // /admin/login), never a hardcoded origin.
    CHECK(
      silent->getHeader("Location").find("http://localhost:5173/login?must_change_password=1") !=
      std::string::npos
    );
    auto adminSilent = authorize(cookie, "", "fulla-admin-console", "http://127.0.0.1:5174/admin/callback");
    REQUIRE(adminSilent != nullptr);
    CHECK(adminSilent->getStatusCode() == k302Found);
    CHECK(
      adminSilent->getHeader("Location").find(
        "http://localhost:5174/admin/login?must_change_password=1"
      ) != std::string::npos
    );

    // prompt=none: OIDC Core §3.1.2.1 — error redirect, never UI.
    auto promptNone = authorize(cookie, "none");
    REQUIRE(promptNone != nullptr);
    CHECK(promptNone->getStatusCode() == k302Found);
    auto location = promptNone->getHeader("Location");
    CHECK(location.find("http://127.0.0.1:5173/callback") != std::string::npos);
    CHECK(location.find("error=login_required") != std::string::npos);
}

DROGON_TEST(Integration_P1_ForcedPasswordChange_Consent_Returns403)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    REQUIRE(ensureFlaggedUser());

    auto login = loginUser();
    REQUIRE(login != nullptr);
    REQUIRE(login->getStatusCode() == k200OK);
    const std::string cookie = cookieOf(login);
    REQUIRE(!cookie.empty());
    const std::string userId = userInternalId(kTestUser);
    REQUIRE(!userId.empty());

    auto client = HttpClient::newHttpClient("http://127.0.0.1:5555", app().getLoop());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Post);
    req->setPath("/oauth2/consent");
    req->setBody(
      "client_id=fulla-portal&user_id=" + userId +
      "&scope=openid&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback&state=pwdstate001&consent_csrf=anything&action=approve"
    );
    req->setContentTypeCode(CT_APPLICATION_X_FORM);
    req->addHeader("Cookie", cookie);
    auto [result, resp] = client->sendRequest(req, 30.0);
    REQUIRE(result == ReqResult::Ok);
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == k403Forbidden);
    Json::Value errBody;
    REQUIRE(fulla::test::http::parseJsonBody(resp, errBody));
    CHECK(errBody["error"]["code"].asString() == "AUTH_PASSWORD_CHANGE_REQUIRED");
}

DROGON_TEST(Integration_P0_ForcedPasswordChange_ChangeEndpoint_ValidationAndSuccess)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    REQUIRE(ensureFlaggedUser());

    auto login = loginUser();
    REQUIRE(login != nullptr);
    REQUIRE(login->getStatusCode() == k200OK);
    const std::string cookie = cookieOf(login);
    REQUIRE(!cookie.empty());

    // Wrong old password -> 401 AUTH_INVALID_CREDENTIALS, flag stays set.
    const std::string wrongOld = ::fulla::drogon::utils::generateSecureToken(12) + "cC3!";
    auto wrongOldResp = postChange(cookie, wrongOld, newPassword());
    REQUIRE(wrongOldResp != nullptr);
    CHECK(wrongOldResp->getStatusCode() == k401Unauthorized);
    bool flag = false;
    REQUIRE(userFlag(kTestUser, flag));
    CHECK(flag == true);

    // New password below the configured minimum -> 400 VALIDATION_PASSWORD_TOO_SHORT.
    auto tooShort = postChange(cookie, oldPassword(), "short");
    REQUIRE(tooShort != nullptr);
    CHECK(tooShort->getStatusCode() == k400BadRequest);
    Json::Value shortBody;
    REQUIRE(fulla::test::http::parseJsonBody(tooShort, shortBody));
    CHECK(shortBody["error"]["code"].asString() == "VALIDATION_PASSWORD_TOO_SHORT");

    // Correct old password -> 200, flag cleared in the DB, session marker gone.
    auto ok = postChange(cookie, oldPassword(), newPassword());
    REQUIRE(ok != nullptr);
    CHECK(ok->getStatusCode() == k200OK);
    REQUIRE(userFlag(kTestUser, flag));
    CHECK(flag == false);

    // After the change the endpoint refuses the (now marker-less) session.
    auto again = postChange(cookie, newPassword(), oldPassword());
    REQUIRE(again != nullptr);
    CHECK(again->getStatusCode() == k401Unauthorized);

    // PR #157 review (MAJOR 1): the successful change must DEMOTE the session
    // to anonymous — authorize with the pre-change cookie is refused (routed
    // to the login screen, no code), enforcing the promised re-login. Without
    // this, a flagged MFA-enabled account could authorize single-factor from
    // the leftover first-factor session.
    auto silent = authorize(cookie);
    REQUIRE(silent != nullptr);
    CHECK(silent->getStatusCode() == k302Found);
    CHECK(silent->getHeader("Location").find("/login") != std::string::npos);
    CHECK(silent->getHeader("Location").find("code=") == std::string::npos);

    // Re-login with the NEW password: no password_change_required, and the
    // normal code issuance path works again.
    auto relogin = fulla::test::http::sendPostForm(
      "/oauth2/login?json=true",
      std::string("username=") + kTestUser + "&password=" + newPassword() +
        "&client_id=fulla-portal&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback"
        "&scope=openid&state=pwdstate002&code_challenge=F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po&code_challenge_method=S256"
    );
    REQUIRE(relogin != nullptr);
    REQUIRE(relogin->getStatusCode() == k200OK);
    Json::Value body;
    REQUIRE(fulla::test::http::parseJsonBody(relogin, body));
    CHECK(!body.isMember("password_change_required"));
    CHECK(!body.get("code", "").asString().empty());
}

DROGON_TEST(Integration_P1_ForcedPasswordChange_ChangeEndpoint_RequiresMarker)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    // The seeded admin is NOT flagged: its session must not reach the
    // endpoint (401 AUTH_SESSION_REQUIRED), keeping the surface minimal.
    auto resp = fulla::test::http::sendPostForm(
      "/oauth2/login?json=true",
      "username=admin&password=admin"
      "&client_id=fulla-admin-console&redirect_uri=http%3A%2F%2F127.0.0.1%3A5174%2Fadmin%2Fcallback"
      "&scope=openid&state=g1&code_challenge=F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po&code_challenge_method=S256"
    );
    REQUIRE(resp != nullptr);
    REQUIRE(resp->getStatusCode() == k200OK);
    const std::string cookie = cookieOf(resp);
    REQUIRE(!cookie.empty());

    auto change = postChange(cookie, "admin", "SomeNewPass1!");
    REQUIRE(change != nullptr);
    CHECK(change->getStatusCode() == k401Unauthorized);
    Json::Value errBody;
    REQUIRE(fulla::test::http::parseJsonBody(change, errBody));
    CHECK(errBody["error"]["code"].asString() == "AUTH_SESSION_REQUIRED");
}

// Admin create-user API: must_change_password is opt-in, round-trips through
// the detail endpoint, can be cleared via update, and a flagged created user
// is actually gated at login.
DROGON_TEST(Integration_P1_AdminUser_MustChangePassword_FieldRoundTrip)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }
    auto token = fulla::test::http::loginAsAdmin();
    REQUIRE(token.has_value());

    // Runtime-minted password for the throwaway admin-created user.
    const std::string createdPassword = ::fulla::drogon::utils::generateSecureToken(12) + "dD4!";
    // Unique per run: the username unique constraint spans soft-deleted rows,
    // so re-runs against a non-reset DB would 409 on a fixed name.
    const std::string createdUser =
      "forced_pwd_admin_" + ::fulla::drogon::utils::generateSecureToken(6);
    Json::Value create;
    create["username"] = createdUser;
    create["password"] = createdPassword;
    // No email: the column carries a unique constraint, and a fixed address
    // would 409 on re-runs against a non-reset DB.
    create["must_change_password"] = true;
    auto createResp = fulla::test::http::sendPostJson("/api/admin/users", create, *token);
    REQUIRE(createResp != nullptr);
    REQUIRE(createResp->getStatusCode() == k201Created);
    Json::Value created;
    REQUIRE(fulla::test::http::parseJsonBody(createResp, created));
    REQUIRE(created.isMember("user"));
    CHECK(created["user"].get("must_change_password", false).asBool() == true);
    const std::string newId = created["user"].get("id", "").asString();
    REQUIRE(!newId.empty());

    // Flagged created user is gated at login.
    auto login = fulla::test::http::sendPostForm(
      "/oauth2/login?json=true",
      "username=" + createdUser + "&password=" + createdPassword +
        "&client_id=fulla-portal&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback"
        "&scope=openid&state=pwdstate003&code_challenge=F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po&code_challenge_method=S256"
    );
    REQUIRE(login != nullptr);
    REQUIRE(login->getStatusCode() == k200OK);
    Json::Value loginBody;
    REQUIRE(fulla::test::http::parseJsonBody(login, loginBody));
    CHECK(loginBody.get("password_change_required", false).asBool() == true);

    // Update clears the flag; the detail endpoint reflects it.
    Json::Value update;
    update["must_change_password"] = false;
    auto updateResp = fulla::test::http::sendPutJson(
      "/api/admin/users/" + newId, update, *token
    );
    REQUIRE(updateResp != nullptr);
    REQUIRE(updateResp->getStatusCode() == k200OK);

    auto detailResp = fulla::test::http::sendGet("/api/admin/users/" + newId, *token);
    REQUIRE(detailResp != nullptr);
    REQUIRE(detailResp->getStatusCode() == k200OK);
    Json::Value detail;
    REQUIRE(fulla::test::http::parseJsonBody(detailResp, detail));
    CHECK(detail.get("must_change_password", true).asBool() == false);
}

// PR #157 review MAJOR 3: the lockout counters must have a READ side — a
// locked account is refused here (even with the CORRECT old password) just
// like the login path refuses it; otherwise the wrong-password increments
// fill locked_until while the endpoint keeps accepting attempts.
DROGON_TEST(Integration_P1_ForcedPasswordChange_LockedAccount_RefusedWith401)
{
    if (!fulla::test::http::postgresAvailable())
    {
        CHECK(true);
        return;
    }

    // RAII: whatever REQUIRE aborts mid-case, the shared row is unlocked for
    // the rest of the suite.
    struct UnlockGuard
    {
        ~UnlockGuard()
        {
            execSql(
              std::string("UPDATE users SET locked_until = 0, failed_login_count = 0 WHERE "
                          "username = '") +
              kTestUser + "'"
            );
        }
    } unlockGuard;

    // Establish the flagged session FIRST (login itself checks the lock).
    REQUIRE(ensureFlaggedUser());
    auto login = loginUser();
    REQUIRE(login != nullptr);
    REQUIRE(login->getStatusCode() == k200OK);
    const std::string cookie = cookieOf(login);
    REQUIRE(!cookie.empty());

    // Seed a live lock directly (5 wrong logins would also work but is slower).
    const int64_t future = static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      ).count()
    ) + 60;
    REQUIRE(execSql(
      std::string("UPDATE users SET locked_until = ") + std::to_string(future) +
      " WHERE username = '" + kTestUser + "'"
    ));

    // Correct old password + live lock -> 401, and the flag stays set.
    auto change = postChange(cookie, oldPassword(), newPassword());
    REQUIRE(change != nullptr);
    CHECK(change->getStatusCode() == k401Unauthorized);
    Json::Value errBody;
    REQUIRE(fulla::test::http::parseJsonBody(change, errBody));
    CHECK(errBody["error"]["code"].asString() == "AUTH_INVALID_CREDENTIALS");

    bool flag = true;
    REQUIRE(userFlag(kTestUser, flag));
    CHECK(flag == true);
}
