// tests/integration/admin/AdminRbacEnforcementHttpTest.cc
//
// P1-8 (#285): the delegated-administration red line, end to end through
// all THREE gates:
//   gate③ (AuthorizationFilter rbac_rules permissions semantics),
//   the #43 scope gate, and chain① (AuthorizationService's permission-
//   aware scope tiering at authorize time).
//
// Red line: a fresh "helpdesk" role holding ONLY the users:read permission
// can list users, cannot write users (its token cannot even carry
// users:write), and cannot touch other admin resource families. The
// negative line: granting the USER role a permission never collapses the
// catch-all role gate for paths without a specific permissions rule.
//
// Storage: Postgres-only (role admin APIs need the DB).

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/drogon/authz/RolePermissionResolver.h>

#include "HttpTestClient.h"

#include <chrono>
#include <future>
#include <optional>
#include <string>

using fulla::test::http::loginAsAdmin;
using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendGet;
using fulla::test::http::sendPostForm;
using fulla::test::http::sendPostFormWithAuth;
using fulla::test::http::sendPostJson;
using fulla::test::http::sendPutJson;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define RBAC_E2E_SKIP_GUARD                                    \
    do                                                         \
    {                                                          \
        if (!postgresAvailable() || !serverReachable())        \
        {                                                      \
            CHECK(true);                                       \
            return;                                            \
        }                                                      \
    } while (0)

namespace
{
std::string uniqueSuffix()
{
    // Nanosecond tail: millisecond suffixes collided between back-to-back
    // runs (role-name 409s).
    return std::to_string(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count() % 1000000000000ULL);
}

bool createVerifiedUser(const std::string &username, const std::string &password)
{
    Json::Value body;
    body["username"] = username;
    body["password"] = password;
    body["email"] = username + "@qa.example";
    auto resp = fulla::test::http::sendPostJson("/api/register", body);
    if (resp == nullptr || resp->getStatusCode() >= 300)
        return false;
    auto db = ::drogon::app().getDbClient();
    std::promise<bool> done;
    db->execSqlAsync(
      "UPDATE users SET email_verified = true WHERE username = $1",
      [&done](const ::drogon::orm::Result &) { done.set_value(true); },
      [&done](const ::drogon::orm::DrogonDbException &) { done.set_value(false); },
      username);
    return done.get_future().get();
}

// PKCE authorization-code login for an arbitrary client (the delegated
// console): returns the access token + the granted scope string.
std::optional<std::pair<std::string, std::string>> loginForClient(
  const std::string &username,
  const std::string &password,
  const std::string &clientId,
  const std::string &scope
)
{
    const std::string codeVerifier = ::fulla::drogon::utils::generateSecureToken(32);
    const std::string codeChallenge =
      ::fulla::drogon::utils::computeCodeChallenge(codeVerifier, "S256");
    const std::string loginForm =
      "username=" + username + "&password=" + password + "&client_id=" + clientId +
      "&redirect_uri=http://127.0.0.1:5174/admin/callback&scope=" +
      ::drogon::utils::urlEncode(scope) +
      "&state=rbac-e2e-check&code_challenge=" + codeChallenge + "&code_challenge_method=S256";
    auto loginResp = fulla::test::http::sendPostForm("/oauth2/login?json=true", loginForm);
    if (loginResp == nullptr || loginResp->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    Json::Value loginJson;
    if (!parseJsonBody(loginResp, loginJson))
        return std::nullopt;
    const std::string code = loginJson.get("code", "").asString();
    if (code.empty())
        return std::nullopt;
    const std::string tokenForm =
      "grant_type=authorization_code&code=" + code +
      "&redirect_uri=http://127.0.0.1:5174/admin/callback&client_id=" + clientId +
      "&code_verifier=" + codeVerifier;
    auto tokenResp = fulla::test::http::sendPostForm("/oauth2/token", tokenForm);
    if (tokenResp == nullptr || tokenResp->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    Json::Value tokenJson;
    if (!parseJsonBody(tokenResp, tokenJson))
        return std::nullopt;
    const std::string access = tokenJson.get("access_token", "").asString();
    const std::string granted = tokenJson.get("scope", "").asString();
    if (access.empty())
        return std::nullopt;
    return std::make_pair(access, granted);
}
}  // namespace

DROGON_TEST(Integration_P1_Rbac_HelpdeskRedLine_AllThreeGates)
{
    RBAC_E2E_SKIP_GUARD;

    auto adminToken = loginAsAdmin();
    REQUIRE(adminToken.has_value());

    const std::string suffix = uniqueSuffix();
    const std::string roleName = "qa-helpdesk-" + suffix;

    // 1. Create the helpdesk role + grant ONLY users:read.
    {
        Json::Value role;
        role["name"] = roleName;
        role["description"] = "P1-8 e2e helpdesk";
        auto resp = sendPostJson("/api/admin/roles", role, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k201Created));
    }
    std::string roleId;
    {
        auto resp = sendGet("/api/admin/roles", *adminToken);
        REQUIRE(resp != nullptr);
        Json::Value body;
        REQUIRE(parseJsonBody(resp, body));
        for (const auto &r : body["roles"])
        {
            if (r["name"].asString() == roleName)
                roleId = std::to_string(r["id"].asInt64());
        }
        REQUIRE(!roleId.empty());
    }
    {
        Json::Value perms;
        Json::Value arr(Json::arrayValue);
        arr.append("users:read");
        perms["permissions"] = arr;
        auto resp = sendPutJson("/api/admin/roles/" + roleId + "/permissions", perms, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
    }
    // Read-back.
    {
        auto resp = sendGet("/api/admin/roles/" + roleId + "/permissions", *adminToken);
        REQUIRE(resp != nullptr);
        Json::Value body;
        REQUIRE(parseJsonBody(resp, body));
        CHECK(body["total"].asInt64() == 1);
        CHECK(body["permissions"][0].asString() == "users:read");
    }

    // 2. A delegated-console client allowing the resource scopes.
    std::string consoleClientId;
    {
        Json::Value client;
        client["name"] = "QA Helpdesk Console " + suffix;
        client["client_type"] = "PUBLIC";
        client["redirect_uris"] = "http://127.0.0.1:5174/admin/callback";
        auto resp = sendPostJson("/api/admin/clients", client, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k201Created));
        Json::Value body;
        REQUIRE(parseJsonBody(resp, body));
        consoleClientId = body["client_id"].asString();
        REQUIRE(!consoleClientId.empty());
        // Grant the client the resource scopes it may relay.
        auto db = ::drogon::app().getDbClient();
        auto f = db->execSqlAsyncFuture(
          "INSERT INTO oauth2_client_scopes (client_id, scope_name) "
          "SELECT $1, s FROM unnest(ARRAY['openid','users:read','users:write','roles:read']) AS s "
          "ON CONFLICT DO NOTHING",
          consoleClientId);
        f.get();
    }

    // 3. Helpdesk user + role assignment.
    const std::string hdUser = "qa_hd_" + suffix;
    const std::string hdPass = "qa-hd-Zx9-" + suffix;
    REQUIRE(createVerifiedUser(hdUser, hdPass));
    {
        auto db = ::drogon::app().getDbClient();
        auto f = db->execSqlAsyncFuture(
          "INSERT INTO user_roles (user_id, role_id) "
          "SELECT u.id, r.id FROM users u, roles r "
          "WHERE u.username = $1 AND r.name = $2 ON CONFLICT DO NOTHING",
          hdUser, roleName);
        f.get();
    }

    // 4. Login requesting three admin-class scopes: chain① must issue ONLY
    // users:read (the helpdesk permission covers exactly that).
    auto login = loginForClient(hdUser, hdPass, consoleClientId,
                                "openid users:read users:write roles:read");
    REQUIRE(login.has_value());
    const std::string &hdToken = login->first;
    // The token response omits `scope` when it equals the request; the
    // EFFECTIVE scope is read back via introspection (backend-svc Basic).
    std::string granted;
    {
        const std::string svcCred = "backend-svc:test-secret";
        const std::string svcBasic =
          "Basic " + ::drogon::utils::base64Encode(
                       reinterpret_cast<const unsigned char *>(svcCred.data()), svcCred.size());
        auto resp = fulla::test::http::sendPostFormWithAuth(
          "/oauth2/introspect", "token=" + hdToken, svcBasic);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
        Json::Value body;
        REQUIRE(parseJsonBody(resp, body));
        granted = body.get("scope", "").asString();
    }
    CHECK(granted.find("users:read") != std::string::npos);
    CHECK(granted.find("users:write") == std::string::npos);
    CHECK(granted.find("roles:read") == std::string::npos);

    // 5. Red line legs.
    // (a) CAN list users: gate③ users.* permissions rule + scope gate.
    {
        auto resp = sendGet("/api/admin/users?per_page=1", hdToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k200OK));
    }
    // (b) CANNOT write users: the scope gate rejects (token has no
    // users:write — chain① already refused to issue it).
    {
        Json::Value body;
        body["full_name"] = "Nope";
        auto resp = sendPutJson("/api/admin/users/1", body, hdToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k403Forbidden));
    }
    // (c) CANNOT touch other resource families: the catch-all gate③ rule
    // requires the admin role.
    {
        auto resp = sendGet("/api/admin/roles", hdToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k403Forbidden));
        auto resp2 = sendGet("/api/admin/clients", hdToken);
        REQUIRE(resp2 != nullptr);
        CHECK(statusIs(resp2, ::drogon::k403Forbidden));
    }
}

DROGON_TEST(Integration_P1_Rbac_NegativeLine_UserRoleGrantNoGateCollapse)
{
    RBAC_E2E_SKIP_GUARD;

    auto adminToken = loginAsAdmin();
    REQUIRE(adminToken.has_value());

    const std::string suffix = uniqueSuffix();

    // A client + a plain (user-role) user.
    std::string clientId;
    {
        Json::Value client;
        client["name"] = "QA NegLine Console " + suffix;
        client["client_type"] = "PUBLIC";
        client["redirect_uris"] = "http://127.0.0.1:5174/admin/callback";
        auto resp = sendPostJson("/api/admin/clients", client, *adminToken);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k201Created));
        Json::Value body;
        REQUIRE(parseJsonBody(resp, body));
        clientId = body["client_id"].asString();
        auto db = ::drogon::app().getDbClient();
        auto f = db->execSqlAsyncFuture(
          "INSERT INTO oauth2_client_scopes (client_id, scope_name) "
          "SELECT $1, s FROM unnest(ARRAY['openid','users:read','roles:read']) AS s "
          "ON CONFLICT DO NOTHING",
          clientId);
        f.get();
    }

    const std::string user = "qa_neg_" + suffix;
    const std::string pass = "qa-neg-Zx9-" + suffix;
    REQUIRE(createVerifiedUser(user, pass));

    // Baseline: the user-role user CANNOT get users:read (admin-class,
    // user role holds no permission).
    {
        auto login = loginForClient(user, pass, clientId, "openid users:read");
        REQUIRE(login.has_value());
        // The catch-all blocks any /api/admin path for them regardless of
        // what scope the token ended up carrying.
        auto resp = sendGet("/api/admin/users?per_page=1", login->first);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, ::drogon::k403Forbidden));
    }

    // Grant the USER role users:read (an operator action). Now the user CAN
    // obtain the scope (chain①) and passes the users.* gate③ rule — that is
    // delegated administration working — but the CATCH-ALL family
    // (/api/admin/roles) still denies: no gate collapse.
    {
        auto db = ::drogon::app().getDbClient();
        auto f = db->execSqlAsyncFuture(
          "INSERT INTO role_permissions (role_id, permission_id) "
          "SELECT r.id, p.id FROM roles r, permissions p "
          "WHERE r.name = 'user' AND p.name = 'users:read' ON CONFLICT DO NOTHING");
        f.get();
        // Cache coherence: the resolver caches per role — invalidate.
        // (The admin PUT endpoint does this; direct DB writes in tests must
        // do it manually or use a fresh role name. The user role is cached
        // by earlier tests in this binary, so invalidate via the seam.)
        ::fulla::drogon::authz::invalidateRolePermissionCacheForRole("user");

        auto login = loginForClient(user, pass, clientId, "openid users:read roles:read");
        REQUIRE(login.has_value());
        // The granted token's effective scope: users:read yes; roles:read
        // no (user role's permission set covers ONLY users:read —
        // classification vs determination decoupled). Verified through the
        // API behavior legs below (roles:read would pass the roles-list
        // scope gate only if issued).

        // users.* path: allowed via the specific permissions rule.
        auto usersResp = sendGet("/api/admin/users?per_page=1", login->first);
        REQUIRE(usersResp != nullptr);
        CHECK(statusIs(usersResp, ::drogon::k200OK));
        // Catch-all family: still denied for a non-admin role.
        auto rolesResp = sendGet("/api/admin/roles", login->first);
        REQUIRE(rolesResp != nullptr);
        CHECK(statusIs(rolesResp, ::drogon::k403Forbidden));

        // Cleanup: the user-role grant is GLOBAL state shared with every
        // later test in the binary — restore the pristine shape (no
        // permission) and invalidate the cache entry.
        {
            // distinct name: an outer `db` local exists (C4456 under the CI
            // -Werror legs)
            auto cleanupDb = ::drogon::app().getDbClient();
            auto cleanupFuture = cleanupDb->execSqlAsyncFuture(
              "DELETE FROM role_permissions rp USING roles r, permissions p "
              "WHERE rp.role_id = r.id AND rp.permission_id = p.id "
              "AND r.name = 'user' AND p.name = 'users:read'");
            cleanupFuture.get();
            ::fulla::drogon::authz::invalidateRolePermissionCacheForRole("user");
        }
    }
}
