// Properties 5 & 6 — Fix-check (DB-state verification)
// Validates: Requirements 2.5 (Property 5) and 2.6 (Property 6).
//
// Purpose
// -------
// The exploratory test (Property1) and preservation test (Property7) cover the
// HTTP response contracts. This file covers the *database-side* contracts that
// those HTTP-only tests cannot assert:
//
//   Property 6 (Requirement 2.6): SessionController::login, when it returns
//     mfa_required for an MFA-enabled user, MUST persist the request's
//     client_id/redirect_uri into mfa_pending_client_id/mfa_pending_redirect_uri
//     for that user BEFORE the response is sent.
//
//   Property 5 (Requirement 2.5): MfaController::verifyLogin, on a fully
//     successful verification (matching binding, registered client, whitelisted
//     redirect_uri, correct TOTP), MUST clear mfa_pending_client_id and
//     mfa_pending_redirect_uri back to NULL after issuing tokens, so the binding
//     cannot be replayed by a later, unrelated verification attempt.
//
// Method
// ------
// HTTP loopback (http://127.0.0.1:5555) drives the endpoints; synchronous DB
// queries inspect the users row before/after to assert column state. Guards on
// storage type and server reachability.

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

// Read the pending binding for the admin user. Returns {clientId, redirectUri},
// where each is "" if the column is NULL.
struct PendingBinding
{
    std::string clientId;
    std::string redirectUri;
    bool isNull = true;
};

PendingBinding readPendingBinding()
{
    PendingBinding pb;
    auto db = app().getDbClient();
    if (!db)
        return pb;
    std::promise<bool> p;
    db->execSqlAsync(
      "SELECT mfa_pending_client_id, mfa_pending_redirect_uri FROM users "
      "WHERE username = 'admin'",
      [&](const Result &r) {
          if (!r.empty())
          {
              pb.isNull =
                r[0]["mfa_pending_client_id"].isNull() && r[0]["mfa_pending_redirect_uri"].isNull();
              pb.clientId = r[0]["mfa_pending_client_id"].isNull()
                              ? ""
                              : r[0]["mfa_pending_client_id"].as<std::string>();
              pb.redirectUri = r[0]["mfa_pending_redirect_uri"].isNull()
                                 ? ""
                                 : r[0]["mfa_pending_redirect_uri"].as<std::string>();
              p.set_value(true);
          }
          else
          {
              p.set_value(false);
          }
      },
      [&](const DrogonDbException &) { p.set_value(false); }
    );
    p.get_future().get();
    return pb;
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
}  // namespace

// ---------------------------------------------------------------------------
// Property 6 (Requirement 2.6): SessionController::login persists the pending
// binding when MFA is required, BEFORE the mfa_required response is sent.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_MfaCrossClientAuthFix_Property6_LoginPersistsPendingBinding)
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

    // The login response (mfa_required) is only sent AFTER the UPDATE commits
    // (fail-closed design), so as soon as we have the mfa_token the pending
    // binding MUST be durable in the DB.
    std::string mfaToken = loginForMfaToken("fulla-portal", kVueRedirectUri);
    REQUIRE(!mfaToken.empty());

    PendingBinding pb = readPendingBinding();
    REQUIRE(!pb.isNull);
    CHECK(pb.clientId == "fulla-portal");
    CHECK(pb.redirectUri == kVueRedirectUri);
}

// ---------------------------------------------------------------------------
// Property 6 (Requirement 2.6, boundary): the pending binding reflects the
// LAST login attempt's client_id/redirect_uri. Logging in twice with different
// clients overwrites the binding (the accepted concurrent-login limitation).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_MfaCrossClientAuthFix_Property6_LoginOverwritesPreviousBinding)
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

    // First login as fulla-portal.
    REQUIRE(!loginForMfaToken("fulla-portal", kVueRedirectUri).empty());
    {
        PendingBinding pb = readPendingBinding();
        REQUIRE(!pb.isNull);
        CHECK(pb.clientId == "fulla-portal");
    }

    // Second login with a different client/redirect_uri overwrites the binding.
    REQUIRE(!loginForMfaToken("fulla-portal", "http://127.0.0.1:8080/callback").empty());
    {
        PendingBinding pb = readPendingBinding();
        REQUIRE(!pb.isNull);
        CHECK(pb.clientId == "fulla-portal");
        CHECK(pb.redirectUri == "http://127.0.0.1:8080/callback");
    }
}

// ---------------------------------------------------------------------------
// Property 5 (Requirement 2.5): a fully successful verifyLogin clears the
// pending binding back to NULL, so it cannot be replayed.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_MfaCrossClientAuthFix_Property5_PendingBindingClearedOnSuccess)
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

    // Pre-condition: binding is populated.
    {
        PendingBinding pb = readPendingBinding();
        REQUIRE(!pb.isNull);
    }

    // Successful verification (matching binding).
    std::string code = fulla::identity::totp::generateCode(fx.secret, totpNowSeconds());
    Json::Value body;
    body["mfa_token"] = mfaToken;
    body["code"] = code;
    body["client_id"] = "fulla-portal";
    body["redirect_uri"] = kVueRedirectUri;
    body["scope"] = "openid profile email";
    auto resp = postJson("/oauth2/mfa/verify", body);
    REQUIRE(resp != nullptr);
    REQUIRE(resp->getStatusCode() == k200OK);

    // The clear is best-effort and async, so poll briefly for the DB to settle.
    bool cleared = false;
    for (int i = 0; i < 20; ++i)
    {
        PendingBinding pb = readPendingBinding();
        if (pb.isNull)
        {
            cleared = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(cleared);
}

// ---------------------------------------------------------------------------
// Property 5 (Requirement 2.5, negative): a REJECTED verifyLogin (cross-client)
// MUST NOT clear the pending binding — the binding stays so a legitimate retry
// with the correct client can still succeed.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_MfaCrossClientAuthFix_Property5_RejectedVerifyKeepsBinding)
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

    // Rejected: wrong client (fulla-admin-console) with correct TOTP.
    std::string code = fulla::identity::totp::generateCode(fx.secret, totpNowSeconds());
    Json::Value body;
    body["mfa_token"] = mfaToken;
    body["code"] = code;
    body["client_id"] = "fulla-admin-console";
    body["redirect_uri"] = "http://127.0.0.1:5174/admin/callback";
    body["scope"] = "openid profile email";
    auto resp = postJson("/oauth2/mfa/verify", body);
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == k401Unauthorized);

    // The binding must still be present (fulla-portal) — a later legitimate retry
    // with fulla-portal can still succeed.
    PendingBinding pb = readPendingBinding();
    CHECK(!pb.isNull);
    CHECK(pb.clientId == "fulla-portal");
    CHECK(pb.redirectUri == kVueRedirectUri);
}
