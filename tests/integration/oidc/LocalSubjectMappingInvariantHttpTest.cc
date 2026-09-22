// tests/integration/oidc/LocalSubjectMappingInvariantHttpTest.cc
//
// #143: users created via self-registration (/api/register) and the admin
// API (POST /api/admin/users) previously got NO (local, <id>) row in
// oauth2_subject_mappings. Consent's getInternalUserId resolves users
// exclusively through that table, so those users 500'd on
// authorize -> consent. Fixed with a three-layer defense (creation paths
// write the mapping + V027 one-time backfill + startup self-heal); these
// tests pin each layer:
//   1. self-register -> login -> authorize(prompt=consent) -> approve -> 302 code
//   2. admin createUser -> that user's consent chain -> 302 code
//   3. V027 SQL rerun semantics: heals a mapping-less user, idempotent,
//      and internal_user_id stays consistent (points at the right user)
//   4. AdminBootstrapper::backfillLocalSubjectMappings heals a gap
#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>

#include "HttpTestClient.h"
#include "bootstrap/AdminBootstrapper.h"

#include <chrono>
#include <future>
#include <string>

using fulla::test::http::loginAsAdmin;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendPostForm;
using fulla::test::http::sendPostJson;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define SUBJECT_MAPPING_SKIP_GUARD                                     \
    do                                                                 \
    {                                                                  \
        if (!postgresAvailable() || !serverReachable())                \
        {                                                              \
            CHECK(true);                                               \
            return;                                                    \
        }                                                              \
    } while (0)

namespace
{
std::string uniqueSuffix()
{
    return std::to_string(
      std::chrono::high_resolution_clock::now().time_since_epoch().count() % 1000000
    );
}

// Session-cookie login (the consent flow is session-authenticated). Mirrors
// the ConsentAuthGateHttpTest helper; returns "" on failure.
std::string loginCookie(const std::string &username, const std::string &password)
{
    auto resp = sendPostForm(
      "/oauth2/login?json=true",
      "username=" + username + "&password=" + password +
        "&client_id=fulla-portal&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback"
        "&scope=openid&state=p0143&code_challenge=F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po&code_challenge_method=S256"
    );
    if (!resp || !statusIs(resp, drogon::k200OK))
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

// authorize with prompt=consent using the session cookie; on 302, extracts
// the minted consent_csrf + user_id from the redirect and merges any new
// cookies. Returns false on any failure.
bool authorizeMintNonce(
  const std::string &cookieIn,
  std::string &csrf,
  std::string &userId,
  std::string &cookieOut
)
{
    cookieOut = cookieIn;
    try
    {
        auto client =
          ::drogon::HttpClient::newHttpClient("http://127.0.0.1:5555", ::drogon::app().getLoop());
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Get);
        req->setPath("/oauth2/authorize");
        req->setParameter("response_type", "code");
        req->setParameter("client_id", "fulla-portal");
        req->setParameter("redirect_uri", "http://127.0.0.1:5173/callback");
        req->setParameter("scope", "openid");
        req->setParameter("state", "p0143state");
        req->setParameter("prompt", "consent");
        req->setParameter("code_challenge", "F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po");
        req->setParameter("code_challenge_method", "plain");
        req->addHeader("Cookie", cookieOut);
        auto [result, resp] = client->sendRequest(req, 30.0);
        if (result != ::drogon::ReqResult::Ok || resp == nullptr ||
            resp->getStatusCode() != ::drogon::k302Found)
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
        csrf = location.substr(
          csrfPos + 13, (csrfEnd == std::string::npos ? location.size() : csrfEnd) - csrfPos - 13
        );
        auto uidEnd = location.find('&', uidPos);
        userId = location.substr(
          uidPos + 8, (uidEnd == std::string::npos ? location.size() : uidEnd) - uidPos - 8
        );
        return !csrf.empty() && !userId.empty();
    }
    catch (...)
    {
        return false;
    }
}

// The consent POST (approve) with the session cookie + minted nonce.
::drogon::HttpResponsePtr postWithCookie(
  const std::string &path,
  const std::string &body,
  const std::string &cookie
)
{
    try
    {
        auto client =
          ::drogon::HttpClient::newHttpClient("http://127.0.0.1:5555", ::drogon::app().getLoop());
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Post);
        req->setPath(path);
        req->setBody(body);
        req->setContentTypeCode(::drogon::CT_APPLICATION_X_FORM);
        req->addHeader("Cookie", cookie);
        auto [result, resp] = client->sendRequest(req, 30.0);
        if (result != ::drogon::ReqResult::Ok)
            return nullptr;
        return resp;
    }
    catch (...)
    {
        return nullptr;
    }
}

// Full consent chain for an already-session-authenticated user:
// authorize(prompt=consent) -> approve -> expect 302 with an auth code.
// Returns the approve response (caller asserts), or nullptr on setup failure.
::drogon::HttpResponsePtr consentChainForSession(const std::string &cookie)
{
    std::string csrf, userId, cookie2;
    if (!authorizeMintNonce(cookie, csrf, userId, cookie2))
        return nullptr;
    return postWithCookie(
      "/oauth2/consent",
      "client_id=fulla-portal&user_id=" + userId +
        "&scope=openid&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback&state=p0143state&code_challenge=F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po&code_challenge_method=plain&consent_csrf=" +
        csrf + "&action=approve",
      cookie2
    );
}

// Direct-DB: how many (local, <id>) mapping rows point at the wrong user or
// are missing entirely (the #143 acceptance query, tightened to also catch
// mis-pointed rows).
long mappingGapCount()
{
    auto db = ::drogon::app().getDbClient();
    if (!db)
        return -1;
    auto rows = db->execSqlSync(
      "SELECT count(*) AS n FROM users u WHERE NOT EXISTS ("
      "SELECT 1 FROM oauth2_subject_mappings m "
      "WHERE m.provider = 'local' AND m.subject = u.id::text "
      "AND m.internal_user_id = u.id)"
    );
    return rows.empty() ? -1 : rows[0]["n"].as<long>();
}
}  // namespace

// ---------------------------------------------------------------------------
// Layer A1: self-registration -> login -> authorize -> consent approve must
// 302 with a code (previously 500 — no (local, <id>) mapping was written).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_SubjectMapping_SelfRegister_ConsentFlow_Returns302WithCode)
{
    SUBJECT_MAPPING_SKIP_GUARD;

    const std::string suffix = uniqueSuffix();
    const std::string uname = "p0143reg_" + suffix;
    auto reg = sendPostForm(
      "/api/register", "username=" + uname + "&password=Passw0rd!143&email=p0143reg_" + suffix + "@example.test"
    );
    REQUIRE(reg != nullptr);
    REQUIRE(statusIs(reg, drogon::k200OK));

    auto cookie = loginCookie(uname, "Passw0rd!143");
    REQUIRE(!cookie.empty());

    auto approve = consentChainForSession(cookie);
    REQUIRE(approve != nullptr);
    CHECK(approve->getStatusCode() == ::drogon::k302Found);
    CHECK(approve->getHeader("Location").find("code=") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Layer A2: a user created through the admin API gets the same consent
// chain (UserAdminService::createUser previously wrote no mapping either).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_SubjectMapping_AdminCreateUser_ConsentFlow_Returns302WithCode)
{
    SUBJECT_MAPPING_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    const std::string suffix = uniqueSuffix();
    const std::string uname = "p0143adm_" + suffix;
    Json::Value body;
    body["username"] = uname;
    body["password"] = "Passw0rd!143";
    body["email"] = "p0143adm_" + suffix + "@example.test";
    body["email_verified"] = false;
    body["mfa_enabled"] = false;
    auto created = sendPostJson("/api/admin/users", body, *token);
    REQUIRE(created != nullptr);
    REQUIRE(statusIs(created, drogon::k201Created));

    auto cookie = loginCookie(uname, "Passw0rd!143");
    REQUIRE(!cookie.empty());

    auto approve = consentChainForSession(cookie);
    REQUIRE(approve != nullptr);
    CHECK(approve->getStatusCode() == ::drogon::k302Found);
    CHECK(approve->getHeader("Location").find("code=") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Layer B (V027 rerun semantics): re-running the migration's SQL text heals
// a mapping-less user, is idempotent, and keeps internal_user_id consistent.
// SchemaSetup applies all migrations atomically (a "pre-migration" state
// cannot be simulated), so the rerun-idempotency semantics are what a
// production upgrade relies on and what is pinned here.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_SubjectMapping_V027Backfill_IdempotentAndConsistent)
{
    SUBJECT_MAPPING_SKIP_GUARD;

    auto db = ::drogon::app().getDbClient();
    REQUIRE(db != nullptr);

    // A user row with NO mapping — the pre-V027 state for admin-created or
    // self-registered users (password hash value irrelevant; never logged in).
    const std::string uname = "p0143back_" + uniqueSuffix();
    auto inserted = db->execSqlSync(
      "INSERT INTO users (username, password_hash, salt, email) "
      "VALUES ($1, 'x', '', $2) RETURNING id",
      uname, uname + "@example.test"
    );
    REQUIRE(inserted.size() == 1);
    const int32_t uid = inserted[0]["id"].as<int32_t>();

    // The exact V027 statement text (single statement, ON CONFLICT guard).
    const char *kV027Sql =
      "INSERT INTO oauth2_subject_mappings (subject, internal_user_id, provider) "
      "SELECT u.id::text, u.id, 'local' FROM users u "
      "ON CONFLICT (provider, subject) DO NOTHING";

    db->execSqlSync(kV027Sql);
    auto rows = db->execSqlSync(
      "SELECT internal_user_id FROM oauth2_subject_mappings "
      "WHERE provider = 'local' AND subject = $1",
      std::to_string(uid)
    );
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]["internal_user_id"].as<int32_t>() == uid);

    // Idempotent: a second run neither duplicates nor mis-points.
    db->execSqlSync(kV027Sql);
    rows = db->execSqlSync(
      "SELECT count(*) AS n FROM oauth2_subject_mappings "
      "WHERE provider = 'local' AND subject = $1",
      std::to_string(uid)
    );
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]["n"].as<long>() == 1);

    // The tightened acceptance query: no user may lack an id-consistent row.
    CHECK(mappingGapCount() == 0);
}

// ---------------------------------------------------------------------------
// Layer C (startup self-heal): AdminBootstrapper::backfillLocalSubjectMappings
// (the exact pass main.cc schedules at boot) heals a mapping-less user.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_SubjectMapping_StartupBackfill_HealsMissingMapping)
{
    SUBJECT_MAPPING_SKIP_GUARD;

    auto db = ::drogon::app().getDbClient();
    REQUIRE(db != nullptr);

    const std::string uname = "p0143heal_" + uniqueSuffix();
    auto inserted = db->execSqlSync(
      "INSERT INTO users (username, password_hash, salt, email) "
      "VALUES ($1, 'x', '', $2) RETURNING id",
      uname, uname + "@example.test"
    );
    REQUIRE(inserted.size() == 1);
    const int32_t uid = inserted[0]["id"].as<int32_t>();

    auto done = std::make_shared<std::promise<bool>>();
    bootstrap::AdminBootstrapper::backfillLocalSubjectMappings(
      [done](bool ok, const std::string &detail) {
          if (!ok)
              LOG_WARN << "backfill test: " << detail;
          done->set_value(ok);
      }
    );
    CHECK(done->get_future().get());

    auto rows = db->execSqlSync(
      "SELECT internal_user_id FROM oauth2_subject_mappings "
      "WHERE provider = 'local' AND subject = $1",
      std::to_string(uid)
    );
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]["internal_user_id"].as<int32_t>() == uid);
}
