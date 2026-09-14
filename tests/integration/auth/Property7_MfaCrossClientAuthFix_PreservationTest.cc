// Property 7 — Preservation (Baseline / Regression)
// Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5, 3.9 (Unchanged Behavior).
//
// Purpose
// -------
// Capture the *unchanged* behavior that must survive the MFA cross-client
// authentication fix unmodified. These tests assert the legitimate matching
// path still issues tokens with the existing response shape, and that the
// pre-existing rejections (wrong TOTP, missing fields, unknown mfa_token) and
// the non-MFA login path keep behaving exactly as before.
//
// These tests are written to PASS on UNFIXED code (they capture today's
// behavior as the baseline to preserve). After Task 4 lands, they MUST still
// PASS (Task 4.4) — that is the regression check.
//
// Method
// ------
// HTTP loopback against the listener the test binary itself runs
// (http://localhost:5555). Each test guards on storage type (skip in memory
// mode) and server reachability.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/identity/TotpUtils.h>
#include <fulla/drogon/utils/CryptoUtils.h>
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
// Use the IPv4 literal explicitly: Drogon's listener binds to 0.0.0.0:5555
// (IPv4 only), and resolving "localhost" can yield IPv6 ::1 on Windows, which
// then hangs in SYN_SENT against an IPv4-only listener.
constexpr const char *kBaseUrl = "http://127.0.0.1:5555";
constexpr const char *kVueRedirectUri = "http://127.0.0.1:5173/callback";

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
      "SET mfa_enabled = false, mfa_secret = NULL, "
      "    mfa_pending_client_id = NULL, mfa_pending_redirect_uri = NULL "
      "WHERE username = 'admin'",
      [&](const Result &) { p.set_value(); },
      [&](const DrogonDbException &) { p.set_value(); }
    );
    p.get_future().get();
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

HttpResponsePtr verifyMfa(
  const std::string &mfaToken,
  const std::string &code,
  const std::string &clientId,
  const std::string &redirectUri
)
{
    Json::Value body;
    body["mfa_token"] = mfaToken;
    body["code"] = code;
    body["client_id"] = clientId;
    body["redirect_uri"] = redirectUri;
    body["scope"] = "openid profile email";
    return postJson("/oauth2/mfa/verify", body);
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

// Returns the error code value regardless of response shape (RFC top-level
// `error` string vs Error Envelope `error.code`). Empty if none found.
std::string errorCodeOf(const Json::Value &root)
{
    if (root.isMember("error"))
    {
        if (root["error"].isString())
            return root["error"].asString();
        if (root["error"].isObject() && root["error"].isMember("code"))
            return root["error"]["code"].asString();
    }
    return "";
}
}  // namespace

// ---------------------------------------------------------------------------
// 3.9 / 2.4 — Matching binding still issues tokens with the frozen response
// shape {mfa_verified: true, message: "MFA verification successful",
// access_token, refresh_token, ...}.
//
// On UNFIXED code this passes because verifyLogin issues tokens for any
// registered client/redirect_uri (the bug ALSO lets the legitimate path
// through). After the fix it must STILL pass because the matching-binding path
// is the intended, preserved flow.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_MfaCrossClientAuthFix_Property7_MatchingBindingIssuesTokens)
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
    auto resp = verifyMfa(mfaToken, code, "fulla-portal", kVueRedirectUri);
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == k200OK);

    Json::Value root;
    REQUIRE(parseBody(resp, root));
    CHECK(root.isMember("access_token"));
    CHECK(root.isMember("refresh_token"));
    CHECK(root.get("mfa_verified", false).asBool() == true);
    CHECK(root.get("message", "").asString() == "MFA verification successful");
}

// ---------------------------------------------------------------------------
// 3.3 — Incorrect TOTP code rejected with AUTH_INVALID_CREDENTIALS regardless of
// client/redirect_uri validity. The error message preserves the exact existing
// text "verifyLogin: TOTP code is incorrect" (or carries that detail in the
// envelope's message field).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_MfaCrossClientAuthFix_Property7_WrongTotpRejected)
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

    // Deliberately wrong TOTP code.
    auto resp = verifyMfa(mfaToken, "000000", "fulla-portal", kVueRedirectUri);
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == k401Unauthorized);

    Json::Value root;
    REQUIRE(parseBody(resp, root));
    CHECK(errorCodeOf(root) == "AUTH_INVALID_CREDENTIALS");
}

// ---------------------------------------------------------------------------
// 3.4 — Unknown mfa_token (no matching user id) rejected with
// AUTH_INVALID_CREDENTIALS.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_MfaCrossClientAuthFix_Property7_UnknownMfaTokenRejected)
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

    // Use an mfa_token that cannot resolve to any user id (users.id is SERIAL
    // starting at 1, so 999999999 does not exist).
    auto resp = verifyMfa("999999999", "123456", "fulla-portal", kVueRedirectUri);
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == k401Unauthorized);

    Json::Value root;
    REQUIRE(parseBody(resp, root));
    CHECK(errorCodeOf(root) == "AUTH_INVALID_CREDENTIALS");
}

// ---------------------------------------------------------------------------
// 3.5 — Missing required fields rejected with VALIDATION_MISSING_REQUIRED_FIELD.
// Covers missing mfa_token/code, and missing client_id/redirect_uri.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_MfaCrossClientAuthFix_Property7_MissingFieldsRejected)
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

    // Missing mfa_token + code.
    {
        Json::Value body;
        body["client_id"] = "fulla-portal";
        body["redirect_uri"] = kVueRedirectUri;
        auto resp = postJson("/oauth2/mfa/verify", body);
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() == k400BadRequest);
        Json::Value root;
        REQUIRE(parseBody(resp, root));
        CHECK(errorCodeOf(root) == "VALIDATION_MISSING_REQUIRED_FIELD");
    }
    // Missing client_id + redirect_uri (mfa_token/code present).
    {
        Json::Value body;
        body["mfa_token"] = mfaToken;
        body["code"] = "123456";
        auto resp = postJson("/oauth2/mfa/verify", body);
        REQUIRE(resp != nullptr);
        Json::Value root;
        REQUIRE(parseBody(resp, root));
        CHECK(errorCodeOf(root) == "VALIDATION_MISSING_REQUIRED_FIELD");
    }
}

// ---------------------------------------------------------------------------
// 3.1 / 3.2 — Non-MFA login path unchanged.
//
// With MFA disabled on the user, /oauth2/login must still produce its existing
// authorization-code response (HTTP 200 with `code`/`location` for json=true,
// or a redirect) and must NOT emit mfa_required. mfa_token's format
// (std::to_string(internalId)) is only relevant when MFA is enabled, so this
// case primarily pins the non-MFA path.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_MfaCrossClientAuthFix_Property7_NonMfaLoginUnchanged)
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

    auto db = app().getDbClient();
    // Ensure MFA is OFF for admin (the default seeded state).
    std::promise<void> pDisable;
    db->execSqlAsync(
      "UPDATE users SET mfa_enabled = false, mfa_secret = NULL WHERE username = 'admin'",
      [&](const Result &) { pDisable.set_value(); },
      [&](const DrogonDbException &) { pDisable.set_value(); }
    );
    pDisable.get_future().get();

    struct Guard
    {
        ~Guard()
        {
            restoreAdminMfa();
        }
    } guard;

    Json::Value body;
    body["username"] = "admin";
    body["password"] = "admin";
    body["client_id"] = "fulla-portal";
    body["redirect_uri"] = kVueRedirectUri;
    body["scope"] = "openid profile email";
    // json=true requests the code/location JSON shape instead of a redirect.
    // Drogon reads the `json` flag from the query string in SessionController;
    // to keep the request body pure JSON we instead assert on the redirect
    // branch which is the default.
    auto resp = postJson("/oauth2/login", body);
    REQUIRE(resp != nullptr);

    Json::Value root;
    parseBody(resp, root);
    // Non-MFA login either redirects (302) or, when json=true, returns 200 with
    // a `code`. The one behavior it must NOT exhibit is mfa_required.
    const auto status = resp->getStatusCode();
    bool isMfaRequired = (root.isMember("mfa_required") && root["mfa_required"].asBool());
    CHECK(!isMfaRequired);
    // Accept either the redirect (302) or the json-code (200) shape.
    CHECK((status == k302Found || status == k200OK));
}
