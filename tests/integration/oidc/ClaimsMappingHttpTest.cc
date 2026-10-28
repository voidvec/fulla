// tests/integration/oidc/ClaimsMappingHttpTest.cc
//
// P1-3 (#280): HTTP integration tests for the claims-mapping publication
// chain — the three issuance surfaces (id_token code path, id_token REFRESH
// path, userinfo), the write-time red lines through the admin CRUD, the
// self-service custom-attributes document, and the D5 legacy `username`
// removal.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <fulla/drogon/utils/CryptoUtils.h>

#include "HttpTestClient.h"

#include <chrono>
#include <sstream>
#include <future>
#include <optional>
#include <string>

using fulla::test::http::loginAsAdmin;
using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendDelete;
using fulla::test::http::sendGet;
using fulla::test::http::sendPostForm;
using fulla::test::http::sendPostJson;
using fulla::test::http::sendPutJson;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define CLAIMS_MAP_SKIP_GUARD                                   \
    do                                                          \
    {                                                           \
        if (!postgresAvailable() || !serverReachable())         \
        {                                                       \
            CHECK(true);                                        \
            return;                                             \
        }                                                       \
    } while (0)

namespace
{
std::string uniqueSuffix()
{
    return std::to_string(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count() % 1000000000000ULL);
}

// Decode a JWT payload (no verification — these tests issued the token
// themselves and only inspect claims).
Json::Value jwtPayload(const std::string &token)
{
    Json::Value out;
    const std::size_t dot1 = token.find('.');
    const std::size_t dot2 = token.find('.', dot1 + 1);
    if (dot1 == std::string::npos || dot2 == std::string::npos)
        return out;
    std::string seg = token.substr(dot1 + 1, dot2 - dot1 - 1);
    // base64url -> base64, pad.
    for (char &c : seg)
    {
        if (c == '-')
            c = '+';
        else if (c == '_')
            c = '/';
    }
    while (seg.size() % 4 != 0)
        seg.push_back('=');
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream in(::drogon::utils::base64Decode(seg));
    Json::parseFromStream(rb, in, &out, &errs);
    return out;
}
}  // namespace

// ---------------------------------------------------------------------------
// Write-time red lines through the admin CRUD.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_ClaimsMapping_RedLines_AdminCRUD)
{
    CLAIMS_MAP_SKIP_GUARD;

auto adminToken = loginAsAdmin();
    REQUIRE(adminToken.has_value());

    // Reserved claim rejected.
    {
        Json::Value m;
        m["scope_name"] = "profile";
        m["claim_name"] = "email";
        m["source_path"] = "custom_attributes.tenant";
        auto resp = sendPostJson("/api/admin/claim-mappings", m, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k400BadRequest));
        Json::Value err;
        REQUIRE(parseJsonBody(resp, err));
        CHECK(err["error"]["code"].asString() == "CLAIM_MAPPING_RESERVED_CLAIM");
    }
    // openid trigger rejected.
    {
        Json::Value m;
        m["scope_name"] = "openid";
        m["claim_name"] = "tenant_id";
        m["source_path"] = "custom_attributes.tenant";
        auto resp = sendPostJson("/api/admin/claim-mappings", m, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k400BadRequest));
        Json::Value err;
        REQUIRE(parseJsonBody(resp, err));
        CHECK(err["error"]["code"].asString() == "CLAIM_MAPPING_FORBIDDEN_SCOPE");
    }
    // Non-whitelisted source rejected.
    {
        Json::Value m;
        m["scope_name"] = "profile";
        m["claim_name"] = "tenant_id";
        m["source_path"] = "org.role";
        auto resp = sendPostJson("/api/admin/claim-mappings", m, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k400BadRequest));
        Json::Value err;
        REQUIRE(parseJsonBody(resp, err));
        CHECK(err["error"]["code"].asString() == "CLAIM_MAPPING_INVALID_SOURCE");
    }
    // List works.
    {
        auto resp = sendGet("/api/admin/claim-mappings", *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
    }
}

// ---------------------------------------------------------------------------
// Publication red line through the FULL chain: a mapping under the
// `qa_p13_<suffix>` scope publishes on all three surfaces ONLY when the
// token carries the trigger scope; refresh re-evaluates; userinfo merges.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_ClaimsMapping_PublicationChain)
{
    CLAIMS_MAP_SKIP_GUARD;

    auto adminToken = loginAsAdmin();
    REQUIRE(adminToken.has_value());

    const std::string suffix = uniqueSuffix();
    const std::string triggerScope = "qa_p13_" + suffix;  // non-admin, client-scoped
    const std::string claim = "tenant_id_" + suffix.substr(0, 6);

    // Seed the trigger scope on the client + a mapping under it.
    // 1) Create the scope via admin scopes API.
    {
        Json::Value s;
        s["name"] = triggerScope;
        s["description"] = "P1-3 e2e trigger";
        auto resp = sendPostJson("/api/admin/scopes", s, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k201Created));
    }
    // 2) Client: fresh PUBLIC test console allowed openid+profile+trigger.
    std::string clientId;
    {
        Json::Value c;
        c["name"] = "P13 Console " + suffix;
        c["client_type"] = "PUBLIC";
        c["redirect_uris"] = "http://127.0.0.1:5174/admin/callback";
        // The refresh leg of the test needs the refresh_token grant.
        c["allowed_grant_types"] = "authorization_code,refresh_token";
        auto resp = sendPostJson("/api/admin/clients", c, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k201Created));
        Json::Value body;
        REQUIRE(parseJsonBody(resp, body));
        clientId = body["client_id"].asString();
        auto db = ::drogon::app().getDbClient();
        auto f = db->execSqlAsyncFuture(
          "INSERT INTO oauth2_client_scopes (client_id, scope_name) "
          "SELECT $1, s FROM unnest(ARRAY['openid','profile',$2::text]) AS s "
          "ON CONFLICT DO NOTHING",
          clientId, triggerScope);
        f.get();
    }
    // 3) User with a tenant attribute.
    const std::string user = "qa_p13_" + suffix;
    const std::string pass = "qa-p13-Zx9-" + suffix;
    {
        Json::Value reg;
        reg["username"] = user;
        reg["password"] = pass;
        reg["email"] = user + "@qa.example";
        auto resp = sendPostJson("/api/register", reg);
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() < 300);
        auto db = ::drogon::app().getDbClient();
        auto f1 = db->execSqlAsyncFuture(
          "UPDATE users SET email_verified = true WHERE username = $1", user);
        f1.get();
        auto f2 = db->execSqlAsyncFuture(
          "UPDATE users SET custom_attributes = $2::jsonb WHERE username = $1",
          user,
          "{\"tenant\":\"acme-" + suffix + "\"}");
        f2.get();
    }
    // 4) Login (PKCE) requesting openid + profile + trigger.
    const std::string codeVerifier = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string codeChallenge =
      ::fulla::drogon::utils::computeCodeChallenge(codeVerifier, "S256");
    std::string accessA;
    std::string refreshA;
    std::string idTokenA;
    {
        const std::string form =
          "username=" + user + "&password=" + pass + "&client_id=" + clientId +
          "&redirect_uri=http://127.0.0.1:5174/admin/callback"
          "&scope=openid%20profile%20" + triggerScope +
          "&state=p13&code_challenge=" + codeChallenge + "&code_challenge_method=S256";
        auto loginResp = sendPostForm("/oauth2/login?json=true", form);
        REQUIRE(loginResp != nullptr);
        CHECK(statusIs(loginResp, ::drogon::k200OK));
        Json::Value lj;
        REQUIRE(parseJsonBody(loginResp, lj));
        const std::string code = lj["code"].asString();
        REQUIRE(!code.empty());
        const std::string tf =
          "grant_type=authorization_code&code=" + code +
          "&redirect_uri=http://127.0.0.1:5174/admin/callback&client_id=" + clientId +
          "&code_verifier=" + codeVerifier;
        auto tokResp = sendPostForm("/oauth2/token", tf);
        REQUIRE(tokResp != nullptr);
        CHECK(statusIs(tokResp, ::drogon::k200OK));
        Json::Value tj;
        REQUIRE(parseJsonBody(tokResp, tj));
        accessA = tj["access_token"].asString();
        refreshA = tj["refresh_token"].asString();
        idTokenA = tj["id_token"].asString();
        REQUIRE(accessA.size() > 0);
        REQUIRE(refreshA.size() > 0);
    }

    // 5) BEFORE any mapping exists: no tenant claim anywhere.
    {
        REQUIRE(!idTokenA.empty());
        Json::Value payload = jwtPayload(idTokenA);
        CHECK(payload.isMember(claim) == false);
        auto ui = sendGet("/oauth2/userinfo", accessA);
        REQUIRE(ui != nullptr);
        CHECK(statusIs(ui, ::drogon::k200OK));
        Json::Value uiBody;
        REQUIRE(parseJsonBody(ui, uiBody));
        CHECK(uiBody.isMember(claim) == false);
        // D5: the legacy username dual-emission is GONE.
        CHECK(uiBody.isMember("username") == false);
        CHECK(uiBody.isMember("preferred_username") == true);
    }

    // 6) Create the mapping (include_in both).
    {
        Json::Value m;
        m["scope_name"] = triggerScope;
        m["claim_name"] = claim;
        m["source_path"] = "custom_attributes.tenant";
        m["include_in"] = "both";
        auto resp = sendPostJson("/api/admin/claim-mappings", m, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k201Created));
    }

    // 7) Userinfo (existing token) now carries the mapped claim.
    {
        auto ui = sendGet("/oauth2/userinfo", accessA);
        REQUIRE(ui != nullptr);
        CHECK(statusIs(ui, ::drogon::k200OK));
        Json::Value uiBody;
        REQUIRE(parseJsonBody(ui, uiBody));
        CHECK(uiBody[claim].asString() == "acme-" + suffix);
    }

    // 8) REFRESH: the rotation-issued id_token carries the claim (mapping
    // re-evaluated at refresh).
    {
        const std::string rf = "grant_type=refresh_token&refresh_token=" + refreshA +
                                "&client_id=" + clientId;
        auto resp = sendPostForm("/oauth2/token", rf);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
        Json::Value tj;
        REQUIRE(parseJsonBody(resp, tj));
        const std::string idTok = tj["id_token"].asString();
        REQUIRE(!idTok.empty());
        Json::Value payload = jwtPayload(idTok);
        CHECK(payload[claim].asString() == "acme-" + suffix);
        refreshA = tj["refresh_token"].asString();
    }

    // 9) Disable the mapping -> the NEXT refresh drops the claim (O7).
    std::string mappingId;
    {
        auto list = sendGet("/api/admin/claim-mappings", *adminToken);
        REQUIRE(list != nullptr);
        Json::Value lb;
        REQUIRE(parseJsonBody(list, lb));
        for (const auto &m : lb["mappings"])
        {
            if (m["claim_name"].asString() == claim)
                mappingId = std::to_string(m["id"].asInt64());
        }
        REQUIRE(!mappingId.empty());
        Json::Value upd;
        upd["scope_name"] = triggerScope;
        upd["claim_name"] = claim;
        upd["source_path"] = "custom_attributes.tenant";
        upd["include_in"] = "both";
        upd["enabled"] = false;
        auto resp = sendPutJson("/api/admin/claim-mappings/" + mappingId, upd, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
    }
    {
        const std::string rf = "grant_type=refresh_token&refresh_token=" + refreshA +
                                "&client_id=" + clientId;
        auto resp = sendPostForm("/oauth2/token", rf);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
        Json::Value tj;
        REQUIRE(parseJsonBody(resp, tj));
        const std::string idTok = tj["id_token"].asString();
        REQUIRE(!idTok.empty());
        Json::Value payload = jwtPayload(idTok);
        CHECK(payload.isMember(claim) == false);
    }
}

// ---------------------------------------------------------------------------
// Self-service custom-attributes document: happy path + red lines.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_ClaimsMapping_SelfServiceAttributes)
{
    CLAIMS_MAP_SKIP_GUARD;

    auto adminToken = loginAsAdmin();
    REQUIRE(adminToken.has_value());

    const std::string suffix = uniqueSuffix();
    const std::string user = "qa_attr_" + suffix;
    const std::string pass = "qa-attr-Zx9-" + suffix;
    {
        Json::Value reg;
        reg["username"] = user;
        reg["password"] = pass;
        reg["email"] = user + "@qa.example";
        auto resp = sendPostJson("/api/register", reg);
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() < 300);
        auto db = ::drogon::app().getDbClient();
        auto f = db->execSqlAsyncFuture(
          "UPDATE users SET email_verified = true WHERE username = $1", user);
        f.get();
    }

    // Login as the user (admin-console PKCE recipe).
    const std::string codeVerifier = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string codeChallenge =
      ::fulla::drogon::utils::computeCodeChallenge(codeVerifier, "S256");
    std::string token;
    {
        const std::string form =
          "username=" + user + "&password=" + pass +
          "&client_id=fulla-admin-console"
          "&redirect_uri=http://127.0.0.1:5174/admin/callback&scope=openid%20profile"
          "&state=a&code_challenge=" + codeChallenge + "&code_challenge_method=S256";
        auto loginResp = sendPostForm("/oauth2/login?json=true", form);
        REQUIRE(loginResp != nullptr);
        CHECK(statusIs(loginResp, ::drogon::k200OK));
        Json::Value lj;
        REQUIRE(parseJsonBody(loginResp, lj));
        const std::string code = lj["code"].asString();
        REQUIRE(!code.empty());
        const std::string tf =
          "grant_type=authorization_code&code=" + code +
          "&redirect_uri=http://127.0.0.1:5174/admin/callback&client_id=fulla-admin-console"
          "&client_secret=&code_verifier=" + codeVerifier;
        auto tokResp = sendPostForm("/oauth2/token", tf);
        REQUIRE(tokResp != nullptr);
        CHECK(statusIs(tokResp, ::drogon::k200OK));
        Json::Value tj;
        REQUIRE(parseJsonBody(tokResp, tj));
        token = tj["access_token"].asString();
        REQUIRE(!token.empty());
    }

    // PUT a valid document.
    {
        Json::Value doc;
        doc["tenant"] = "acme";
        doc["prefs"]["theme"] = "dark";
        auto resp = sendPutJson("/api/me/profile/custom-attributes", doc, token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
    }
    // GET reads it back.
    {
        auto resp = sendGet("/api/me/profile/custom-attributes", token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
        Json::Value body;
        REQUIRE(parseJsonBody(resp, body));
        CHECK(body["custom_attributes"]["tenant"].asString() == "acme");
    }
    // Red lines: __proto__ / bad keys / depth.
    {
        Json::Value doc;
        doc["__proto__"] = "x";
        auto resp = sendPutJson("/api/me/profile/custom-attributes", doc, token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k400BadRequest));
        Json::Value err;
        REQUIRE(parseJsonBody(resp, err));
        CHECK(err["error"]["code"].asString() == "CUSTOM_ATTRIBUTES_INVALID");
    }
    {
        Json::Value doc;
        doc["BadKey"] = 1;
        auto resp = sendPutJson("/api/me/profile/custom-attributes", doc, token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k400BadRequest));
    }
}
