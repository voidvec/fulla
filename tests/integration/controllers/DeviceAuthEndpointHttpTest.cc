// tests/integration/controllers/DeviceAuthEndpointHttpTest.cc
//
// HTTP integration tests for the device-authorization endpoints
// (libs/drogon/src/controllers/DeviceAuthController.cc, 162 LOC, 16% covered).
//
// /oauth2/device_authorization is the OAuth2 Device Flow (RFC 8628) start
// endpoint: a device posts its client_id (+optional scope), the server issues
// a device_code + user_code + verification_uri. It requires NO user auth (the
// device is unauthenticated at this point); only a valid client_id is needed.
// The existing tests/integration/token/DeviceCode* tests cover parts of the
// downstream token exchange; this file covers the device_authorization route
// itself + its validation branches.
//
// Route map (DeviceAuthController.h):
//   POST /oauth2/device_authorization -> deviceAuthorization (no auth)
//   POST /oauth2/device/approve       -> approveDevice (admin-gated; covered
//                                         by the two tests at the bottom)

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <json/json.h>

#include "HttpTestClient.h"

#include <chrono>
#include <future>
#include <string>

using fulla::test::http::loginAsAdmin;
using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendPostForm;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define DEVICEAUTH_SKIP_GUARD                                  \
    do                                                         \
    {                                                          \
        if (!postgresAvailable() || !serverReachable())        \
        {                                                      \
            CHECK(true);                                       \
            return;                                            \
        }                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// device_authorization happy path: POST with the seeded fulla-admin-console
// client_id returns 200 with the RFC 8628 device_code/user_code/
// verification_uri fields. Covers the plugin->createDeviceCode -> success
// branch. The fulla-admin-console client is PUBLIC and seeded, so no client secret
// is needed.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_DeviceAuth_AdminConsoleClient_ReturnsDeviceCode)
{
    DEVICEAUTH_SKIP_GUARD;

    auto resp = sendPostForm(
      "/oauth2/device_authorization",
      "client_id=fulla-admin-console&scope=openid profile admin");
    REQUIRE(resp != nullptr);
    // RFC 8628 §3.2: success is 200 (the device started the flow). Some servers
    // return 201; accept both. Assert the body shape regardless of which.
    const auto code = resp->getStatusCode();
    CHECK((code == drogon::k200OK || code == drogon::k201Created));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body.isMember("device_code"));
    CHECK(body["device_code"].isString());
    CHECK(body.isMember("user_code"));
    CHECK(body["user_code"].isString());
    CHECK(body.isMember("verification_uri"));
    // #146: the default verification_uri must point at the REAL approval page
    // (admin console /admin/devices — the old default /oauth2/device had no
    // page behind it), and verification_uri_complete carries the user_code
    // (RFC 8628 §3.3.1) so the approval page can prefill.
    CHECK(body["verification_uri"].asString().find("/admin/devices") != std::string::npos);
    CHECK(body.isMember("verification_uri_complete"));
    CHECK(
      body["verification_uri_complete"].asString().find("user_code=") != std::string::npos
    );
}

// ---------------------------------------------------------------------------
// device_authorization missing-client_id branch: POST with no client_id
// returns 400 invalid_request ("client_id is required"). Covers the early
// validation rejection.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_DeviceAuth_MissingClientId_Returns400)
{
    DEVICEAUTH_SKIP_GUARD;

    auto resp = sendPostForm("/oauth2/device_authorization", "scope=openid");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k400BadRequest));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body.isMember("error"));
    CHECK(body["error"].asString() == "invalid_request");
}

// ---------------------------------------------------------------------------
// device_authorization unknown-client branch: POST with a client_id that does
// not exist returns an error (invalid_client or invalid_request, depending on
// the exact rejection path). Asserts a 4xx with an RFC 6749 error body.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_DeviceAuth_UnknownClientId_Returns4xxError)
{
    DEVICEAUTH_SKIP_GUARD;

    auto resp = sendPostForm(
      "/oauth2/device_authorization",
      "client_id=nonexistent-client-xyz&scope=openid");
    REQUIRE(resp != nullptr);
    const auto code = resp->getStatusCode();
    CHECK((code == drogon::k400BadRequest || code == drogon::k401Unauthorized));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body.isMember("error"));
}

// ---------------------------------------------------------------------------
// Helpers shared by the /oauth2/device/approve tests below (admin-gated leg
// of RFC 8628). Seeding mirrors DeviceCodeRaceConditionTest: direct SQL on
// oauth2_device_codes + the idempotent PUBLIC device-flow client.
// ---------------------------------------------------------------------------
namespace
{
constexpr const char *kApprovePubClient = "p1-test-pub-device";

bool approveExecSql(const std::string &sql)
{
    auto db = drogon::app().getDbClient();
    if (!db)
        return false;
    // Sync-from-async bridge; the migrations/tests elsewhere already rely on
    // the test DB client being alive for the process lifetime.
    std::promise<bool> p;
    db->execSqlAsync(
      sql,
      [&](const drogon::orm::Result &) { p.set_value(true); },
      [&](const drogon::orm::DrogonDbException &e) {
          LOG_ERROR << "execSql failed: " << e.base().what() << " :: " << sql;
          p.set_value(false);
      }
    );
    return p.get_future().get();
}

bool approveEnsurePubDeviceClient()
{
    return approveExecSql(
      "INSERT INTO oauth2_clients "
      "(client_id, client_type, client_secret, salt, name, redirect_uris, "
      "allowed_grant_types) VALUES ('" +
      std::string(kApprovePubClient) +
      "', 'PUBLIC', '', '', 'P1 test public device client', '', "
      "'urn:ietf:params:oauth:grant-type:device_code') "
      "ON CONFLICT (client_id) DO NOTHING"
    );
}

// Seeds a PENDING device code and returns its user_code via the out-param.
bool approveSeedPendingCode(const std::string &rawDeviceCode, const std::string &userCode)
{
    const std::string hash = fulla::drogon::utils::hashToken(rawDeviceCode);
    const int64_t expiresAt = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch()
                              )
                                .count() +
                              3600;
    if (!approveExecSql("DELETE FROM oauth2_device_codes WHERE device_code_hash = '" + hash + "'"))
        return false;
    return approveExecSql(
      "INSERT INTO oauth2_device_codes "
      "(device_code_hash, user_code, client_id, scope, status, expires_at) VALUES ('" +
      hash + "', '" + userCode + "', '" + kApprovePubClient + "', 'read', 'pending', " +
      std::to_string(expiresAt) + ")"
    );
}
}  // namespace

// ---------------------------------------------------------------------------
// approve + unknown user_code: must return 400 VALIDATION_DEVICE_CODE_INVALID
// and — critically — the server must STILL BE ALIVE afterwards. This is the
// regression test for the 2026-09-08 crash where the same callback was
// std::move'd into both Mapper::findOne lambdas; the loser held an empty
// std::function and calling it aborted the event loop (bad_function_call),
// killing the process on every approval miss.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_DeviceAuth_ApproveUnknownUserCode_Returns400_ServerSurvives)
{
    DEVICEAUTH_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto resp = sendPostForm(
      "/oauth2/device/approve",
      "user_code=NOSUCH77&user_id=1",
      *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k400BadRequest));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body["error"]["code"].asString() == "VALIDATION_DEVICE_CODE_INVALID");

    // Server liveness: a subsequent request must still be served. Before the
    // fix this (and any other request after the approval miss) got nothing —
    // the process was gone.
    auto followUp = sendPostForm(
      "/oauth2/device_authorization",
      "client_id=fulla-admin-console&scope=openid");
    REQUIRE(followUp != nullptr);
    CHECK(
      (followUp->getStatusCode() == drogon::k200OK ||
       followUp->getStatusCode() == drogon::k201Created)
    );
}

// ---------------------------------------------------------------------------
// approve happy path, closed through the token endpoint (browser-e2e scenario
// C3): seed a pending device_code -> admin approves via
// /oauth2/device/approve -> the device redeems device_code at /oauth2/token
// (device_code grant) and gets an access token.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_DeviceAuth_ApprovePendingCode_ThenDeviceRedeemsToken)
{
    DEVICEAUTH_SKIP_GUARD;

    REQUIRE(approveEnsurePubDeviceClient());

    const std::string deviceCode = "p0-approve-device-code-fixed";
    // user_code is VARCHAR(8) in the schema — keep the marker 8 chars.
    const std::string userCode = "APRVTS01";
    REQUIRE(approveSeedPendingCode(deviceCode, userCode));

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto approveResp = sendPostForm(
      "/oauth2/device/approve",
      "user_code=" + userCode + "&user_id=1",
      *token);
    REQUIRE(approveResp != nullptr);
    CHECK(statusIs(approveResp, drogon::k200OK));
    Json::Value approveBody;
    REQUIRE(parseJsonBody(approveResp, approveBody));
    CHECK(approveBody["status"].asString() == "approved");
    CHECK(approveBody["user_code"].asString() == userCode);

    // Device leg: poll-style single redemption of the approved device_code.
    auto tokenResp = sendPostForm(
      "/oauth2/token",
      "grant_type=urn:ietf:params:oauth:grant-type:device_code"
      "&device_code=" +
        deviceCode + "&client_id=" + kApprovePubClient);
    REQUIRE(tokenResp != nullptr);
    CHECK(statusIs(tokenResp, drogon::k200OK));
    Json::Value tokenBody;
    REQUIRE(parseJsonBody(tokenResp, tokenBody));
    CHECK(tokenBody.isMember("access_token"));
    CHECK(tokenBody.isMember("refresh_token"));

    // Teardown (PR #180 review M8): remove the seeded row so row-count
    // sensitive assertions elsewhere see no residue. The redeemed tokens
    // stay, consistent with the other device-code tests (the shared
    // p1-test-pub-device client must also survive — DeviceCodeRaceCondition
    // tests reuse it via their own idempotent ensure()).
    CHECK(approveExecSql(
      "DELETE FROM oauth2_device_codes WHERE user_code = '" + userCode + "'"
    ));
}
