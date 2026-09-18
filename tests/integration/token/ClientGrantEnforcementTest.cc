// tests/integration/token/ClientGrantEnforcementTest.cc
//
// #220 (RFC 6749 §3.2.1): the token endpoint enforces the client's
// registered allowed_grant_types before dispatching any grant:
//   - grant not in a non-empty registered list -> 400 unauthorized_client
//   - empty list (NULL column / legacy row) -> unrestricted (grandfathered)
//   - refresh_token is implied by an authorization_code or device_code
//     registration (those flows mint refresh tokens unconditionally, so a
//     literal-list rejection would brick legitimately issued tokens)
//
// Fixtures:
//   - `backend-svc` (seed: CONFIDENTIAL, client_credentials-only grant list)
//     drives the rejection cases against real seed data.
//   - Throwaway SQL clients drive the grandfather and implication cases
//     (INSERT/DELETE around the assertions; the endpoint-test server runs
//     without the Redis client cache, so rows are visible immediately --
//     same pattern as ensureBackendSvcScopes in ClientCredentialsScope-
//     ValidationTest.cc).
//   - Memory-storage coverage of the new DTO field is asserted plugin-level
//     in the last test (the HTTP gate needs the running server = Postgres).

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <drogon/HttpClient.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <string>
#include <thread>

using namespace drogon;
using namespace drogon::orm;

namespace
{
constexpr const char *kBaseUrl = "http://127.0.0.1:5555";

bool parseBody(const HttpResponsePtr &resp, Json::Value &out)
{
    const std::string body(resp->getBody());
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;
    return reader->parse(body.data(), body.data() + body.size(), &out, &errs);
}

// POST /oauth2/token with client_secret_post credentials (the throwaway
// fixtures below are seeded with token_endpoint_auth_method=client_secret_post
// so the helper stays self-contained; backend-svc uses Basic and gets it from
// the caller).
HttpResponsePtr postToken(const std::string &body, const std::string &authHeader = "")
{
    try
    {
        auto client = HttpClient::newHttpClient(kBaseUrl);
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        req->setPath("/oauth2/token");
        req->setContentTypeCode(CT_APPLICATION_X_FORM);
        if (!authHeader.empty())
            req->addHeader("Authorization", authHeader);
        req->setBody(body);
        auto [result, resp] = client->sendRequest(req, /*timeout=*/30.0);
        if (result != ReqResult::Ok || resp == nullptr)
            return nullptr;
        return resp;
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "postToken failed (server likely unreachable): " << e.what();
        return nullptr;
    }
}

HttpResponsePtr postBackendSvc(const std::string &body)
{
    return postToken(
      body, "Basic " + ::drogon::utils::base64Encode("backend-svc:test-secret")
    );
}

bool serverReachable()
{
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        if (postBackendSvc("grant_type=client_credentials") != nullptr)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

bool isPostgresFixture()
{
    auto plugin = app().getPlugin<OAuth2Plugin>();
    return plugin && plugin->getStorageType() != "memory";
}

// Insert a throwaway CONFIDENTIAL client with the given grant list (raw
// string; pass "" for NULL = legacy row). Secret is always "test-secret"
// stored as lowercase sha256(secret+salt) -- the exact computation the
// Postgres validateClient path performs. One scope grant (tokens:read,
// exists in the V023 seed) is added so the client_credentials positive
// case has a non-empty default scope set. Cleanup happens in removeFixture
// (oauth2_client_scopes FK-cascades on client deletion, V006).
bool seedFixture(const std::string &clientId, const std::string &grantListOrNull)
{
    auto db = app().getDbClient();
    if (!db)
        return false;
    const std::string salt = "gsalt";
    std::string hash = ::drogon::utils::getSha256("test-secret" + salt);
    std::transform(hash.begin(), hash.end(), hash.begin(), ::tolower);
    std::promise<bool> p;
    auto db2 = app().getDbClient();
    db->execSqlAsync(
      "INSERT INTO oauth2_clients (client_id, client_type, client_secret, salt, name, "
      "redirect_uris, allowed_grant_types, token_endpoint_auth_method) "
      "VALUES ($1, 'CONFIDENTIAL', $2, $3, 'Grant Enforcement Fixture', "
      "'http://localhost/cb', $4, 'client_secret_post') "
      "ON CONFLICT (client_id) DO UPDATE SET client_secret = EXCLUDED.client_secret, "
      "salt = EXCLUDED.salt, allowed_grant_types = EXCLUDED.allowed_grant_types",
      [db2, clientId, &p](const Result &) {
          // Same non-blocking chain as ensureBackendSvcScopes: the inner
          // insert's callbacks settle the promise; nothing blocks a DB thread.
          db2->execSqlAsync(
            "INSERT INTO oauth2_client_scopes (client_id, scope_name) "
            "VALUES ($1, 'tokens:read') ON CONFLICT (client_id, scope_name) DO NOTHING",
            [&p](const Result &) { p.set_value(true); },
            [&p](const DrogonDbException &e) {
                LOG_ERROR << "seedFixture scope grant: " << e.base().what();
                p.set_value(false);
            },
            clientId
          );
      },
      [&p](const DrogonDbException &e) {
          LOG_ERROR << "seedFixture: " << e.base().what();
          p.set_value(false);
      },
      clientId,
      hash,
      salt,
      grantListOrNull
    );
    return p.get_future().get();
}

void removeFixture(const std::string &clientId)
{
    auto db = app().getDbClient();
    if (!db)
        return;
    std::promise<bool> p;
    db->execSqlAsync(
      "DELETE FROM oauth2_clients WHERE client_id = $1",
      [&p](const Result &) { p.set_value(true); },
      [&p](const DrogonDbException &e) {
          LOG_ERROR << "removeFixture: " << e.base().what();
          p.set_value(false);
      },
      clientId
    );
    p.get_future().get();
}

std::string fixtureBody(const std::string &grantType)
{
    return "grant_type=" + grantType +
           "&client_id=grant-enforce-fx&client_secret=test-secret";
}

// Format-valid dummy token values (TOKEN_PATTERN ^[a-zA-Z0-9._-]+$,
// TOKEN_MIN_LEN 32): the RuleSet::oauth2Token validation gate rejects
// anything shorter before the grant-enforcement gate can run.
const char *kDummyToken =
  "bogus-token-0123456789-0123456789-0123456789";
}  // namespace

DROGON_TEST(Integration_P1_Token_GrantEnforcement_RejectsUnregisteredGrant)
{
    if (!isPostgresFixture())
    {
        CHECK(true);
        return;
    }
    if (!serverReachable())
    {
        LOG_INFO << "Skipping: HTTP listener not reachable on " << kBaseUrl;
        CHECK(true);
        return;
    }

    // backend-svc is seeded with allowed_grant_types='client_credentials'.
    // The gate fires BEFORE code/token lookup, so even dummy grant payloads
    // (format-valid per RuleSet::oauth2Token) must surface
    // unauthorized_client, not invalid_grant.
    {
        auto resp = postBackendSvc(
          "grant_type=authorization_code&code=BOGUSCODE012345678901234567890123456789&redirect_uri=http://x/cb"
        );
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() == k400BadRequest);
        Json::Value body;
        REQUIRE(parseBody(resp, body));
        CHECK(body["error"].asString() == "unauthorized_client");
    }
    {
        // client_credentials-only list: refresh is NOT implied (only
        // authorization_code / device_code imply it) and CC never mints
        // refresh tokens, so the rejection is the correct semantic.
        auto resp = postBackendSvc(
          std::string("grant_type=refresh_token&refresh_token=") + kDummyToken
        );
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() == k400BadRequest);
        Json::Value body;
        REQUIRE(parseBody(resp, body));
        CHECK(body["error"].asString() == "unauthorized_client");
    }
    {
        auto resp = postBackendSvc(
          "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code&device_code=bogus"
        );
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() == k400BadRequest);
        Json::Value body;
        REQUIRE(parseBody(resp, body));
        CHECK(body["error"].asString() == "unauthorized_client");
    }

    // Sanity: the registered grant itself still works.
    {
        auto resp = postBackendSvc("grant_type=client_credentials&scope=tokens:read");
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() == k200OK);
    }
}

DROGON_TEST(Integration_P1_Token_GrantEnforcement_EmptyListGrandfathered)
{
    if (!isPostgresFixture() || !serverReachable())
    {
        CHECK(true);
        return;
    }
    REQUIRE(seedFixture("grant-enforce-fx", ""));  // NULL list = legacy row

    // Unrestricted: a grant the flow logic itself supports goes through the
    // gate and fails only at the (absent) token lookup, proving the gate
    // passed.
    {
        auto resp = postToken(fixtureBody(std::string("refresh_token&refresh_token=") + kDummyToken));
        REQUIRE(resp != nullptr);
        Json::Value body;
        REQUIRE(parseBody(resp, body));
        CHECK(body["error"].asString() != "unauthorized_client");
    }
    // client_credentials actually succeeds end-to-end for the fixture.
    {
        auto resp = postToken(fixtureBody("client_credentials"));
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() == k200OK);
    }
    removeFixture("grant-enforce-fx");
}

DROGON_TEST(Integration_P1_Token_GrantEnforcement_RefreshImpliedByAuthCode)
{
    if (!isPostgresFixture() || !serverReachable())
    {
        CHECK(true);
        return;
    }
    // List registers authorization_code only; refresh must pass the gate
    // (auth-code flows mint refresh tokens unconditionally).
    REQUIRE(seedFixture("grant-enforce-fx", "authorization_code"));
    {
        auto resp = postToken(fixtureBody(std::string("refresh_token&refresh_token=") + kDummyToken));
        REQUIRE(resp != nullptr);
        Json::Value body;
        REQUIRE(parseBody(resp, body));
        CHECK(body["error"].asString() != "unauthorized_client");
    }
    // But an unrelated grant (client_credentials) is still rejected.
    {
        auto resp = postToken(fixtureBody("client_credentials"));
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() == k400BadRequest);
        Json::Value body;
        REQUIRE(parseBody(resp, body));
        CHECK(body["error"].asString() == "unauthorized_client");
    }
    removeFixture("grant-enforce-fx");
}

DROGON_TEST(Integration_P1_Token_GrantEnforcement_MemoryParsesGrantList)
{
    // Memory-storage coverage for the new DTO field (the HTTP gate above
    // needs the running Postgres server; the plugin-level read here proves
    // the memory repository honors both config spellings).
    auto plugin = std::make_shared<OAuth2Plugin>();
    Json::Value config;
    config["storage_type"] = "memory";

    Json::Value commaForm;
    commaForm["type"] = "CONFIDENTIAL";
    commaForm["secret"] = "s1";
    commaForm["allowed_grant_types"] = "authorization_code,refresh_token";
    config["clients"]["comma-client"] = commaForm;

    Json::Value arrayForm;
    arrayForm["type"] = "PUBLIC";
    arrayForm["secret"] = "s2";
    Json::Value grants(Json::arrayValue);
    grants.append("client_credentials");
    arrayForm["allowed_grant_types"] = grants;
    config["clients"]["array-client"] = arrayForm;

    Json::Value bareForm;
    bareForm["type"] = "CONFIDENTIAL";
    bareForm["secret"] = "s3";
    config["clients"]["bare-client"] = bareForm;

    plugin->initAndStart(config);

    {
        std::promise<bool> p;
        auto f = p.get_future();
        plugin->getClient(
          "comma-client",
          [&p](std::optional<fulla::oauth2::model::OAuth2Client> client) {
              if (client && client->allowedGrantTypes.size() == 2 &&
                  client->allowedGrantTypes[0] == "authorization_code" &&
                  client->allowedGrantTypes[1] == "refresh_token")
                  p.set_value(true);
              else
                  p.set_value(false);
          }
        );
        CHECK(f.get());
    }
    {
        std::promise<bool> p;
        auto f = p.get_future();
        plugin->getClient(
          "array-client",
          [&p](std::optional<fulla::oauth2::model::OAuth2Client> client) {
              if (client && client->allowedGrantTypes.size() == 1 &&
                  client->allowedGrantTypes[0] == "client_credentials")
                  p.set_value(true);
              else
                  p.set_value(false);
          }
        );
        CHECK(f.get());
    }
    // Absent key -> empty list (unrestricted legacy semantics).
    {
        std::promise<bool> p;
        auto f = p.get_future();
        plugin->getClient(
          "bare-client",
          [&p](std::optional<fulla::oauth2::model::OAuth2Client> client) {
              p.set_value(client.has_value() && client->allowedGrantTypes.empty());
          }
        );
        CHECK(f.get());
    }
}
