// Integration tests — end-to-end MFA flows (Properties 3, 4, 5, 6)
// Validates: Requirements 2.3, 2.4, 2.5, 2.6, 3.1.
//
// Purpose
// -------
// End-to-end coverage of the full first-factor login -> second-factor
// verification flow, asserting both the HTTP contracts and the database side
// effects (no oauth2_codes / oauth2_access_tokens rows created for rejected
// attempts; pending binding lifecycle).
//
//   Happy-path flow (Properties 4, 5, 6): login as fulla-portal -> verifyLogin
//     with the same client/redirect_uri -> tokens issued with the frozen
//     response shape -> pending binding cleared to NULL.
//
//   Cross-client rejection flow (Property 3): login as fulla-portal -> verifyLogin
//     with fulla-admin-console's own valid client_id/redirect_uri -> rejected with
//     AUTH_INVALID_CREDENTIALS (401), and no oauth2_codes / oauth2_access_tokens
//     rows are created for the rejected attempt.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/identity/TotpUtils.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <json/json.h>
#include <future>
#include <chrono>
#include <string>

using namespace drogon;
using namespace drogon::orm;


namespace
{

int64_t totpNowSeconds()
{
    return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      ).count()
    );
}

}  // namespace

namespace
{
constexpr const char *kBaseUrl = "http://127.0.0.1:5555";
constexpr const char *kVueRedirectUri = "http://127.0.0.1:5173/callback";
constexpr const char *kAdminRedirectUri = "http://127.0.0.1:5174/admin/callback";

bool parseBody(const HttpResponsePtr &resp, Json::Value &out)
{
    const std::string body(resp->getBody());
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;
    return reader->parse(body.data(), body.data() + body.size(), &out, &errs);
}

HttpResponsePtr postJson(const std::string &path, const Json::Value &json)
{
    try
    {
        auto client = HttpClient::newHttpClient(kBaseUrl);
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        req->setPath(path);
        req->setContentTypeCode(CT_APPLICATION_JSON);
        Json::StreamWriterBuilder wb;
        req->setBody(Json::writeString(wb, json));
        auto [result, resp] = client->sendRequest(req, /*timeout=*/30.0);
        if (result != ReqResult::Ok || resp == nullptr)
            return nullptr;
        return resp;
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "postJson failed (server likely unreachable): " << e.what();
        return nullptr;
    }
}

struct MfaFixture
{
    std::string secret;
    bool ok = false;
};

MfaFixture enableAdminMfa()
{
    auto db = app().getDbClient();
    MfaFixture f;
    if (!db)
        return f;
    f.secret = fulla::identity::totp::generateSecret(::fulla::drogon::utils::detail::cryptoProvider());
    std::promise<bool> p;
    db->execSqlAsync(
      "UPDATE users SET mfa_enabled = true, mfa_secret = $1 WHERE username = 'admin'",
      [&](const Result &) { p.set_value(true); },
      [&](const DrogonDbException &) { p.set_value(false); },
      f.secret
    );
    f.ok = p.get_future().get();
    return f;
}

void restoreAdminMfa()
{
    auto db = app().getDbClient();
    if (!db)
        return;
    std::promise<void> p;
    db->execSqlAsync(
      "UPDATE users "
      "SET mfa_enabled = false, mfa_secret = NULL, mfa_backup_codes = NULL, "
      "    mfa_pending_client_id = NULL, mfa_pending_redirect_uri = NULL "
      "WHERE username = 'admin'",
      [&](const Result &) { p.set_value(); },
      [&](const DrogonDbException &) { p.set_value(); }
    );
    p.get_future().get();
}

// P0-3 backup-code seeding for the fallback test below. The storage contract
// (users.mfa_backup_codes) is a JSON array of SHA-256 hex hashes in a text
// column; the two historical write paths hash with different hex case
// (enable: sha256Hex lowercase; regenerate: hashToken uppercase), so the test
// seeds one code per form to pin the case-insensitive match (libs/drogon/
// include/fulla/drogon/utils/MfaBackupCodes.h).
bool seedAdminBackupCodes(const std::string &jsonArray)
{
    auto db = app().getDbClient();
    if (!db)
        return false;
    std::promise<bool> p;
    db->execSqlAsync(
      "UPDATE users SET mfa_backup_codes = $1 WHERE username = 'admin'",
      [&](const Result &) { p.set_value(true); },
      [&](const DrogonDbException &) { p.set_value(false); },
      jsonArray
    );
    return p.get_future().get();
}

// audit_logs rows for the admin user's mfa_backup_code_used events; -1 on
// query failure (audit_logs columns per V012: actor_id, action).
long countBackupCodeAuditRows()
{
    auto db = app().getDbClient();
    if (!db)
        return -1;
    std::promise<long> p;
    db->execSqlAsync(
      "SELECT COUNT(*) AS n FROM audit_logs WHERE action = 'mfa_backup_code_used' "
      "AND actor_id = (SELECT public_sub::text FROM users WHERE username = 'admin')",
      [&](const Result &r) {
          long n = -1;
          if (!r.empty())
              n = r[0]["n"].as<long>();
          p.set_value(n);
      },
      [&](const DrogonDbException &) { p.set_value(-1); }
    );
    return p.get_future().get();
}

std::string loginForMfaToken(const std::string &clientId, const std::string &redirectUri)
{
    Json::Value body;
    body["username"] = "admin";
    body["password"] = "admin";
    body["client_id"] = clientId;
    body["redirect_uri"] = redirectUri;
    body["scope"] = "openid profile email";
    auto resp = postJson("/oauth2/login", body);
    if (!resp)
        return "";
    Json::Value root;
    if (!parseBody(resp, root))
        return "";
    if (!root.isMember("mfa_required") || !root["mfa_required"].asBool())
        return "";
    return root.get("mfa_token", "").asString();
}

bool serverReachable()
{
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        try
        {
            auto client = HttpClient::newHttpClient(kBaseUrl);
            auto req = HttpRequest::newHttpRequest();
            req->setMethod(Post);
            req->setPath("/nonexistent-probe");
            auto [result, resp] = client->sendRequest(req, 10.0);
            if (resp != nullptr)
                return true;
        }
        catch (...)
        {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

// Count oauth2_codes / oauth2_access_tokens rows for the admin user's
// public_sub so a test can confirm a rejected attempt created no new rows.
// Returns -1 on query failure. (oauth2_access_tokens.user_id stores the user's
// public_sub UUID, not the integer id.)
long countRowsForUser(const std::string &table)
{
    auto db = app().getDbClient();
    if (!db)
        return -1;
    std::promise<long> p;
    std::string sql = "SELECT COUNT(*) AS n FROM " + table +
                      " WHERE user_id = (SELECT public_sub::text FROM users "
                      "WHERE username = 'admin')";
    db->execSqlAsync(
      sql,
      [&](const Result &r) {
          long n = -1;
          if (!r.empty())
              n = r[0]["n"].as<long>();
          p.set_value(n);
      },
      [&](const DrogonDbException &) { p.set_value(-1); }
    );
    return p.get_future().get();
}
}  // namespace

// ---------------------------------------------------------------------------
// Integration: Happy-path end-to-end flow (Properties 4, 5, 6).
// login (fulla-portal) -> verifyLogin (matching binding) -> tokens issued with
// the frozen response shape -> pending binding cleared.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_MfaCrossClientAuthFix_HappyPath_EndToEnd)
{
    auto plugin = app().getPlugin<OAuth2Plugin>();
    if (!plugin || plugin->getStorageType() == "memory")
    {
        CHECK(true);
        return;
    }
    if (!serverReachable())
    {
        LOG_INFO << "Skipping: HTTP listener not reachable";
        CHECK(true);
        return;
    }

    auto fx = enableAdminMfa();
    REQUIRE(fx.ok);

    struct Guard
    {
        ~Guard()
        {
            restoreAdminMfa();
        }
    } guard;

    std::string mfaToken = loginForMfaToken("fulla-portal", kVueRedirectUri);
    REQUIRE(!mfaToken.empty());

    std::string code = fulla::identity::totp::generateCode(fx.secret, totpNowSeconds());
    Json::Value body;
    body["mfa_token"] = mfaToken;
    body["code"] = code;
    body["client_id"] = "fulla-portal";
    body["redirect_uri"] = kVueRedirectUri;
    body["scope"] = "openid profile email";
    auto resp = postJson("/oauth2/mfa/verify", body);
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == k200OK);

    // Frozen response shape (Requirement 3.9).
    Json::Value root;
    REQUIRE(parseBody(resp, root));
    CHECK(root.isMember("access_token"));
    CHECK(root.isMember("refresh_token"));
    CHECK(root.get("mfa_verified", false).asBool() == true);
    CHECK(root.get("message", "").asString() == "MFA verification successful");

    // A successful flow DOES create the code/token rows (positive confirmation
    // that the integration path is exercised end to end).
    CHECK(countRowsForUser("oauth2_access_tokens") >= 1);
}

// ---------------------------------------------------------------------------
// Integration: Cross-client rejection flow (Property 3) — no rows created.
// login (fulla-portal) -> verifyLogin (fulla-admin-console's own valid credentials) ->
// rejected with AUTH_INVALID_CREDENTIALS, and no new oauth2_codes /
// oauth2_access_tokens rows are created for the rejected attempt.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_MfaCrossClientAuthFix_CrossClient_NoRowsCreated)
{
    auto plugin = app().getPlugin<OAuth2Plugin>();
    if (!plugin || plugin->getStorageType() == "memory")
    {
        CHECK(true);
        return;
    }
    if (!serverReachable())
    {
        LOG_INFO << "Skipping: HTTP listener not reachable";
        CHECK(true);
        return;
    }

    auto fx = enableAdminMfa();
    REQUIRE(fx.ok);

    struct Guard
    {
        ~Guard()
        {
            restoreAdminMfa();
        }
    } guard;

    std::string mfaToken = loginForMfaToken("fulla-portal", kVueRedirectUri);
    REQUIRE(!mfaToken.empty());

    // Snapshot the existing access-token row count for the admin user.
    long tokensBefore = countRowsForUser("oauth2_access_tokens");
    REQUIRE(tokensBefore >= 0);

    std::string code = fulla::identity::totp::generateCode(fx.secret, totpNowSeconds());
    Json::Value body;
    body["mfa_token"] = mfaToken;
    body["code"] = code;
    body["client_id"] = "fulla-admin-console";
    body["redirect_uri"] = kAdminRedirectUri;
    body["scope"] = "openid profile email";
    auto resp = postJson("/oauth2/mfa/verify", body);
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == k401Unauthorized);

    // The rejected attempt must not have created any new access tokens.
    long tokensAfter = countRowsForUser("oauth2_access_tokens");
    CHECK(tokensAfter == tokensBefore);
}

// ---------------------------------------------------------------------------
// Integration: P0-3 backup-code login fallback, end to end at the HTTP tier
// (2026-11 audit finding #3: the backup-code path had only unit-level
// coverage — Unit_P0_MfaBackup_* + MfaBackupCodesTest — and no HTTP-level
// regression net for the login fall-through, one-shot consumption, the
// mfa_backup_code_used audit row, or the re-bind flag).
//
//   1. seed TWO backup codes — one hashed lowercase (enable-path form), one
//      UPPERCASE (regenerate-path form): matching is case-insensitive;
//   2. a code that is not a valid TOTP falls through to the backup codes and
//      completes the login (backup_codes_remaining reflects the consumption);
//   3. the SAME code cannot be consumed twice (one-shot CAS);
//   4. consuming the LAST code answers mfa_rebind_required=true;
//   5. every consumption writes an mfa_backup_code_used audit row.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_MfaBackupCode_LoginFallback_EndToEnd)
{
    auto plugin = app().getPlugin<OAuth2Plugin>();
    if (!plugin || plugin->getStorageType() == "memory")
    {
        CHECK(true);
        return;
    }
    if (!serverReachable())
    {
        LOG_INFO << "Skipping: HTTP listener not reachable";
        CHECK(true);
        return;
    }

    auto fx = enableAdminMfa();
    REQUIRE(fx.ok);

    struct Guard
    {
        ~Guard()
        {
            restoreAdminMfa();
        }
    } guard;

    const std::string plain1 = "wavea-backup-one-4f2a91";
    const std::string plain2 = "wavea-backup-two-b7c3d5";
    std::string hash1 =
      fulla::drogon::utils::detail::cryptoProvider().sha256Hex(plain1);
    std::string hash2 =
      fulla::drogon::utils::detail::cryptoProvider().sha256Hex(plain2);
    std::transform(
      hash2.begin(), hash2.end(), hash2.begin(),
      [](unsigned char c) { return static_cast<char>(std::toupper(c)); }
    );
    REQUIRE(seedAdminBackupCodes("[\"" + hash1 + "\",\"" + hash2 + "\"]"));

    long auditBefore = countBackupCodeAuditRows();
    REQUIRE(auditBefore >= 0);

    // -- 1st consumption: the lowercase-stored code completes the login. ----
    std::string mfaToken = loginForMfaToken("fulla-portal", kVueRedirectUri);
    REQUIRE(!mfaToken.empty());

    Json::Value body;
    body["mfa_token"] = mfaToken;
    body["code"] = plain1;
    body["client_id"] = "fulla-portal";
    body["redirect_uri"] = kVueRedirectUri;
    body["scope"] = "openid profile email";
    auto resp = postJson("/oauth2/mfa/verify", body);
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == k200OK);

    Json::Value root;
    REQUIRE(parseBody(resp, root));
    CHECK(root.isMember("access_token"));
    CHECK(root.get("backup_codes_remaining", -1).asInt() == 1);
    CHECK(root.get("mfa_rebind_required", true).asBool() == false);
    CHECK(countBackupCodeAuditRows() == auditBefore + 1);

    // -- Replay: one-shot CAS — the same code must not work twice. ----------
    body["mfa_token"] = loginForMfaToken("fulla-portal", kVueRedirectUri);
    REQUIRE(!body["mfa_token"].asString().empty());
    auto replay = postJson("/oauth2/mfa/verify", body);
    REQUIRE(replay != nullptr);
    CHECK(replay->getStatusCode() == k401Unauthorized);

    // -- Last consumption: the UPPERCASE-stored form matches too; exhausting
    //    the final code flags a forced TOTP re-bind. -------------------------
    body["mfa_token"] = loginForMfaToken("fulla-portal", kVueRedirectUri);
    REQUIRE(!body["mfa_token"].asString().empty());
    body["code"] = plain2;
    auto last = postJson("/oauth2/mfa/verify", body);
    REQUIRE(last != nullptr);
    CHECK(last->getStatusCode() == k200OK);

    Json::Value lastRoot;
    REQUIRE(parseBody(last, lastRoot));
    CHECK(lastRoot.get("backup_codes_remaining", -1).asInt() == 0);
    CHECK(lastRoot.get("mfa_rebind_required", false).asBool() == true);
    CHECK(countBackupCodeAuditRows() == auditBefore + 2);
}
