// tests/integration/controllers/SocialLoginHttpTest.cc
//
// Mock-based HTTP integration tests for the Social OAuth controllers
// (libs/drogon/src/controllers/{Google,WeChat,GitHub}Controller.cc). These
// controllers were previously 5.9-16.3% covered (only incidental startup
// hits). Each `/api/{provider}/login` route delegates to an injected
// XxxAuthService; tests/common/SocialMockFixture.h installs a real service
// backed by FakeOAuthHttpClient (and FakeSocialAccountRepository for GitHub)
// onto the controller singleton via DrClassMap::getSingleInstance, so the
// controller's injected path runs WITHOUT any real outbound network.
//
// Layer note: these COMPLEMENT (not duplicate) libs/identity/test/
// SocialAuthServiceTest.cc -- that file tests the SERVICE result structs;
// these tests cover the CONTROLLER layer (request body parsing, the
// `if(service_)` injection branch, response re-filtering into JSON, and the
// ErrorResponder error-envelope path with HTTP status codes).
//
// Storage: these run in EVERY CI leg (memory-mode included). Since #70 the
// injected Google/WeChat services carry a FakeSocialAccountRepository and run
// the full account-linking flow; token issuance goes through
// SocialTokenIssuer -> plugin->saveTokenPair (MemoryTokenRepository in memory
// mode), so the happy paths stay DB-free and testable everywhere.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include "HttpTestClient.h"
#include "SocialMockFixture.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

using fulla::test::http::parseJsonBody;
using fulla::test::http::sendPostForm;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;
using fulla::test::social::injectGitHubFake;
using fulla::test::social::injectGoogleFake;
using fulla::test::social::injectWeChatFake;
using fulla::test::http::loginAsAdminWithScope;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendDelete;
using fulla::test::http::sendGet;
using fulla::test::http::sendPostJson;
using fulla::test::http::kTestBaseUrl;
using fulla::test::social::injectSocialLinkFake;

// Server-reachability guard (these routes need no DB, but the in-process
// server must be up). No postgresAvailable() guard: the mock-injected paths
// run under memory mode too.
#define SOCIAL_SKIP_GUARD                                    \
    do                                                       \
    {                                                        \
        if (!serverReachable())                              \
        {                                                    \
            CHECK(true);                                     \
            return;                                          \
        }                                                    \
    } while (0)

// ===========================================================================
// Google (/api/google/login)
// ===========================================================================

// #70 happy path: fake token exchange + userinfo + account auto-create in the
// FakeSocialAccountRepository -> first-party token pair (access/refresh),
// same shape as GitHub's.
DROGON_TEST(Integration_P0_GoogleLogin_FakeExchange_ReturnsTokens)
{
    SOCIAL_SKIP_GUARD;

    auto h = injectGoogleFake();
    Json::Value tokenBody;
    tokenBody["access_token"] = "gtok-test";
    h.http->postFormResponses.push_back(
      fulla::identity::testing::okJson(tokenBody));
    Json::Value userBody;
    userBody["sub"] = "g-123";
    userBody["name"] = "Test User";
    userBody["email"] = "test@example.com";
    userBody["picture"] = "https://example.test/pic.png";
    h.http->getResponses.push_back(fulla::identity::testing::okJson(userBody));

    auto resp = sendPostForm("/api/google/login", "code=test-auth-code");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body.isMember("access_token"));
    CHECK(body.isMember("refresh_token"));
    CHECK(body["token_type"].asString() == "Bearer");
    CHECK(body.isMember("expires_in"));
    // The account was auto-created and linked in the fake repo.
    CHECK(h.accountRepo->linked.count(
            fulla::identity::testing::FakeSocialAccountRepository::key("google", "g-123")) == 1);
}

// #70: auto-create disabled -> 403 AUTH_SOCIAL_ACCOUNT_NOT_LINKED, and no
// account was created (no side effects).
DROGON_TEST(Integration_P0_GoogleLogin_AutoCreateDisabled_Returns403)
{
    SOCIAL_SKIP_GUARD;

    auto h = injectGoogleFake();
    h.service->setAutoCreate(false);
    Json::Value tokenBody;
    tokenBody["access_token"] = "gtok-test";
    h.http->postFormResponses.push_back(
      fulla::identity::testing::okJson(tokenBody));
    Json::Value userBody;
    userBody["sub"] = "g-403x";
    userBody["email"] = "x@example.test";
    h.http->getResponses.push_back(fulla::identity::testing::okJson(userBody));

    auto resp = sendPostForm("/api/google/login", "code=test-auth-code");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k403Forbidden));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(!body.isMember("access_token"));
    CHECK(h.accountRepo->linked.empty());
}

// Missing code -> 400 VALIDATION_MISSING_REQUIRED_FIELD (controller's own
// validation, before the service is called). The controller's body parser
// scans for the literal "code=" substring, so the body must NOT contain that
// substring anywhere (e.g. "state=xyz" is safe; "notcode=xyz" is NOT, because
// it contains "code=").
DROGON_TEST(Integration_P1_GoogleLogin_MissingCode_Returns400)
{
    SOCIAL_SKIP_GUARD;

    injectGoogleFake();  // install fake (not actually hit on this path)
    auto resp = sendPostForm("/api/google/login", "state=xyz&other=val");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k400BadRequest));
}

// Transport failure (token-exchange HTTP call fails) -> service returns
// NET_CONNECTION_FAILED -> controller responds with the NETWORK error envelope
// (HTTP 502, NETWORK-category default).
DROGON_TEST(Integration_P1_GoogleLogin_TransportFailure_Returns502)
{
    SOCIAL_SKIP_GUARD;

    auto h = injectGoogleFake();
    h.http->postFormResponses.push_back(fulla::identity::testing::transportFailure());

    auto resp = sendPostForm("/api/google/login", "code=test-auth-code");
    REQUIRE(resp != nullptr);
    // NET_CONNECTION_FAILED is NETWORK-category -> 502 (ErrorTypes.cc default).
    CHECK(statusIs(resp, drogon::k502BadGateway));
}

// ===========================================================================
// WeChat (/api/wechat/login)
// ===========================================================================

// #70 happy path: fake exchange + userinfo + auto-create -> token pair.
DROGON_TEST(Integration_P0_WeChatLogin_FakeExchange_ReturnsTokens)
{
    SOCIAL_SKIP_GUARD;

    auto h = injectWeChatFake();
    Json::Value tokenBody;
    tokenBody["access_token"] = "wtok-test";
    tokenBody["openid"] = "wx-openid-1";
    h.http->getResponses.push_back(fulla::identity::testing::okJson(tokenBody));
    Json::Value userBody;
    userBody["openid"] = "wx-openid-1";
    userBody["nickname"] = "WX User";
    h.http->getResponses.push_back(fulla::identity::testing::okJson(userBody));

    auto resp = sendPostForm("/api/wechat/login", "code=wx-auth-code");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body.isMember("access_token"));
    CHECK(body.isMember("refresh_token"));
    CHECK(h.accountRepo->linked.count(
            fulla::identity::testing::FakeSocialAccountRepository::key("wechat", "wx-openid-1")) == 1);
}

// #70: an already-linked WeChat identity (existing user) -> tokens, no
// account creation (the linked map stays at the single pre-seeded entry).
DROGON_TEST(Integration_P0_WeChatLogin_LinkedUser_ReturnsTokens)
{
    SOCIAL_SKIP_GUARD;

    auto h = injectWeChatFake();
    fulla::identity::SocialAccountLookup seeded;
    seeded.userId = 4242;
    seeded.username = "wx_existing";
    seeded.publicSub = "sub-4242";
    h.accountRepo->linked[
      fulla::identity::testing::FakeSocialAccountRepository::key("wechat", "wx-linked-1")] = seeded;
    Json::Value tokenBody;
    tokenBody["access_token"] = "wtok-2";
    tokenBody["openid"] = "wx-linked-1";
    h.http->getResponses.push_back(fulla::identity::testing::okJson(tokenBody));
    Json::Value userBody;
    userBody["openid"] = "wx-linked-1";
    h.http->getResponses.push_back(fulla::identity::testing::okJson(userBody));

    auto resp = sendPostForm("/api/wechat/login", "code=wx-auth-code");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body.isMember("access_token"));
    CHECK(h.accountRepo->linked.size() == 1);
}

// #70: derived username collision -> ONE retry with a random suffix, then
// success (the repository logs the conflict; the user is not lost to a bare
// DB_QUERY_ERROR).
DROGON_TEST(Integration_P0_GoogleLogin_UsernameCollision_RetriesWithSuffix)
{
    SOCIAL_SKIP_GUARD;

    auto h = injectGoogleFake();
    // "google_g_collide1" is derived from sub "g-collide1" (first 12 chars);
    // the first create attempt fails on the conflicting username, the retry
    // (suffixed) succeeds.
    h.accountRepo->conflictingUsernames.insert("google_g-collide1");
    Json::Value tokenBody;
    tokenBody["access_token"] = "gtok-c";
    h.http->postFormResponses.push_back(fulla::identity::testing::okJson(tokenBody));
    Json::Value userBody;
    userBody["sub"] = "g-collide1";
    userBody["email"] = "c@example.test";
    h.http->getResponses.push_back(fulla::identity::testing::okJson(userBody));

    auto resp = sendPostForm("/api/google/login", "code=test-auth-code");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body.isMember("access_token"));
    // Exactly one linked account was created (under the suffixed name).
    CHECK(h.accountRepo->linked.size() == 1);
}

// Missing code -> 400. Body must not contain the "code=" substring (same

// parser quirk as Google's missing-code case above).

DROGON_TEST(Integration_P1_WeChatLogin_MissingCode_Returns400)
{
    SOCIAL_SKIP_GUARD;

    injectWeChatFake();
    auto resp = sendPostForm("/api/wechat/login", "state=xyz&other=val");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k400BadRequest));
}

// ===========================================================================
// GitHub (/api/github/login)
// ===========================================================================
//
// GitHubController::issueTokensForUser now routes token issuance through
// OAuth2Plugin::saveTokenPair (the storage abstraction) instead of calling
// drogon::app().getDbClient() directly. The old direct path crashed the
// process under memory storage via an uncatchable getDbClient() assert
// (review B1), so the GitHub happy-path was previously untestable. With the
// saveTokenPair fix, the happy-path runs in BOTH memory and Postgres modes
// (saveTokenPair -> MemoryTokenRepository in memory mode, which is DB-free).

// Happy path: fake token exchange + userinfo, FakeSocialAccountRepository
// pre-seeded with a linked user, controller mints tokens via
// plugin->saveTokenPair and returns {access_token, refresh_token, token_type,
// expires_in}. Runs in memory mode (the GitHub blocker is resolved).
DROGON_TEST(Integration_P0_GitHubLogin_FakeExchange_ReturnsTokens)
{
    SOCIAL_SKIP_GUARD;

    auto h = injectGitHubFake();
    // Token exchange response.
    Json::Value tokenBody;
    tokenBody["access_token"] = "gh-tok-test";
    tokenBody["token_type"] = "bearer";
    tokenBody["scope"] = "user";
    h.http->postFormResponses.push_back(fulla::identity::testing::okJson(tokenBody));
    // Userinfo response.
    Json::Value userBody;
    userBody["id"] = 12345;
    userBody["login"] = "gh-test-user";
    userBody["email"] = "gh@example.com";
    userBody["name"] = "GH Test";
    h.http->getResponses.push_back(fulla::identity::testing::okJson(userBody));

    auto resp = sendPostForm("/api/github/login", "code=gh-auth-code");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body.isMember("access_token"));
    CHECK(body.isMember("refresh_token"));
    CHECK(body["token_type"].asString() == "Bearer");
    CHECK(body.isMember("expires_in"));
}

// Missing code -> 400 (controller validation, service not called).
DROGON_TEST(Integration_P1_GitHubLogin_MissingCode_Returns400)
{
    SOCIAL_SKIP_GUARD;

    injectGitHubFake();
    auto resp = sendPostForm("/api/github/login", "state=xyz");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k400BadRequest));
}

// Transport failure on token exchange -> service returns NET_CONNECTION_FAILED
// -> controller error envelope (502). Safe under memory mode because the
// service returns an error, never reaching issueTokensForUser.
DROGON_TEST(Integration_P1_GitHubLogin_TransportFailure_Returns502)
{
    SOCIAL_SKIP_GUARD;

    auto h = injectGitHubFake();
    h.http->postFormResponses.push_back(fulla::identity::testing::transportFailure());

    auto resp = sendPostForm("/api/github/login", "code=gh-auth-code");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k502BadGateway));
}

// #54 (V024 soft-delete contract): a mapping that resolves to a
// soft-deleted/locked user answers AccountUnavailable -> generic 401 with NO
// access_token issued (previously tokens were minted unconditionally). Error
// path only, so it is safe under memory mode.
DROGON_TEST(Integration_P0_GitHubLogin_DeletedLinkedUser_Rejected401)
{
    SOCIAL_SKIP_GUARD;

    auto h = injectGitHubFake();
    // Mark (github, "12345") as linked-but-unavailable (soft-deleted/locked).
    h.accountRepo->unavailableKeys.insert(
      fulla::identity::testing::FakeSocialAccountRepository::key("github", "12345")
    );
    Json::Value tokenBody;
    tokenBody["access_token"] = "gh-tok-test";
    tokenBody["token_type"] = "bearer";
    h.http->postFormResponses.push_back(fulla::identity::testing::okJson(tokenBody));
    Json::Value userBody;
    userBody["id"] = 12345;
    userBody["login"] = "gh-test-user";
    userBody["email"] = "gh@example.com";
    h.http->getResponses.push_back(fulla::identity::testing::okJson(userBody));

    auto resp = sendPostForm("/api/github/login", "code=gh-auth-code");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k401Unauthorized));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(!body.isMember("access_token"));
}

// #54: a RepositoryError from the account lookup (DB outage) must surface as
// a 5xx DB error — the old optional<nullopt> contract sent it down the
// account-CREATION branch instead (PR-review finding 3).
DROGON_TEST(Integration_P0_GitHubLogin_RepoError_Returns5xx_NoCreation)
{
    SOCIAL_SKIP_GUARD;

    auto h = injectGitHubFake();
    h.accountRepo->failFind = true;
    Json::Value tokenBody;
    tokenBody["access_token"] = "gh-tok-test";
    tokenBody["token_type"] = "bearer";
    h.http->postFormResponses.push_back(fulla::identity::testing::okJson(tokenBody));
    Json::Value userBody;
    userBody["id"] = 99999;
    userBody["login"] = "gh-test-user";
    userBody["email"] = "gh@example.com";
    h.http->getResponses.push_back(fulla::identity::testing::okJson(userBody));

    auto resp = sendPostForm("/api/github/login", "code=gh-auth-code");
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() >= 500);
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(!body.isMember("access_token"));
    // Critically: the outage must NOT have created a linked account.
    CHECK(h.accountRepo->linked.empty());
}


// #54 (review F1) + #70 (retry): a derived username held by an existing row
// (active or soft-deleted) is NEVER adopted (ON CONFLICT DO NOTHING — no
// account takeover); the login retries once with a random suffix and
// succeeds on the fresh name.
DROGON_TEST(Integration_P0_GitHubLogin_ConflictingUsername_RetriesWithoutAdoption)
{
    SOCIAL_SKIP_GUARD;

    auto h = injectGitHubFake();
    // No mapping for the subject, but "gh_gh-test-user" (the derived
    // username) already exists.
    h.accountRepo->conflictingUsernames.insert("gh_gh-test-user");
    Json::Value tokenBody;
    tokenBody["access_token"] = "gh-tok-test";
    tokenBody["token_type"] = "bearer";
    h.http->postFormResponses.push_back(fulla::identity::testing::okJson(tokenBody));
    Json::Value userBody;
    userBody["id"] = 54321;
    userBody["login"] = "gh-test-user";
    userBody["email"] = "gh@example.com";
    h.http->getResponses.push_back(fulla::identity::testing::okJson(userBody));

    auto resp = sendPostForm("/api/github/login", "code=gh-auth-code");
    REQUIRE(resp != nullptr);
    // #70: the suffixed retry succeeds — the CONFLICTING account itself is
    // still never adopted (no takeover), a NEW account is created instead.
    CHECK(statusIs(resp, drogon::k200OK));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body.isMember("access_token"));
    // One linked account exists, under a suffixed name (not the victim's).
    CHECK(h.accountRepo->linked.size() == 1);
    const auto &created = h.accountRepo->linked.begin()->second;
    CHECK(created.username != "gh_gh-test-user");
    CHECK(created.username.rfind("gh_gh-test-user_", 0) == 0);
}

// ===========================================================================
// #69: social-issued tokens must be stored HASHED so the hash-based lookup
// paths (Bearer validation, introspection, refresh grant, family revoke)
// can find them. Before the fix every authenticated call with a social token
// 401'd. The fake-driven happy path runs in every storage mode.
// ===========================================================================

namespace
{
// Drives the fake GitHub login (no mapping -> find-or-create in the fake
// repo) and returns the issued token pair on success.
std::optional<std::pair<std::string, std::string>> fakeGitHubLoginForTokens(
  const std::shared_ptr<fulla::identity::testing::FakeOAuthHttpClient> &http,
  int64_t githubId
)
{
    Json::Value tokenBody;
    tokenBody["access_token"] = "gh-tok-" + std::to_string(githubId);
    tokenBody["token_type"] = "bearer";
    tokenBody["scope"] = "user";
    http->postFormResponses.push_back(fulla::identity::testing::okJson(tokenBody));
    Json::Value userBody;
    userBody["id"] = githubId;
    userBody["login"] = "gh-user-" + std::to_string(githubId);
    userBody["email"] = "gh" + std::to_string(githubId) + "@example.test";
    http->getResponses.push_back(fulla::identity::testing::okJson(userBody));

    auto resp = sendPostForm("/api/github/login", "code=gh-auth-code");
    if (!resp || resp->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    Json::Value body;
    if (!parseJsonBody(resp, body))
        return std::nullopt;
    const std::string access = body.get("access_token", "").asString();
    const std::string refresh = body.get("refresh_token", "").asString();
    if (access.empty() || refresh.empty())
        return std::nullopt;
    return std::make_pair(access, refresh);
}
}  // namespace

// #69 core: a GitHub-issued refresh token is found by the refresh grant.
// TokenService resolves the presented refresh token via hashToken before the
// repository lookup — exactly the lookup that missed when the raw value was
// stored (pre-fix: invalid_grant). Runs in every storage mode: fulla-portal is
// a PUBLIC client available in both the memory seed and the PG seed, and the
// refresh grant accepts an empty secret for it.
DROGON_TEST(Integration_P0_GitHubLogin_IssuedToken_RefreshGrantWorks)
{
    SOCIAL_SKIP_GUARD;

    auto h = injectGitHubFake();
    auto tokens = fakeGitHubLoginForTokens(h.http, 60690);
    REQUIRE(tokens.has_value());

    auto refreshed = sendPostForm(
      "/oauth2/token",
      "grant_type=refresh_token&client_id=fulla-portal&client_secret=&refresh_token=" + tokens->second
    );
    REQUIRE(refreshed != nullptr);
    CHECK(statusIs(refreshed, drogon::k200OK));
    Json::Value refreshedBody;
    REQUIRE(parseJsonBody(refreshed, refreshedBody));
    CHECK(refreshedBody.get("access_token", "").asString().empty() == false);
    CHECK(refreshedBody.get("refresh_token", "").asString().empty() == false);
}

// #69 (PG-backed, #70-subject-updated): a GitHub-issued token whose user_id
// is the PLATFORM SUBJECT of a REAL user (pre-linked mapping) works across the
// authenticated surface: introspection (active:true), userinfo, the
// /api/me/social/links list, and the refresh grant. Introspection
// authenticates as the seeded CONFIDENTIAL backend-svc (client_secret_basic,
// HTTP Basic) — the endpoint requires a non-empty secret, so the PUBLIC
// fulla-admin-console cannot call it.
DROGON_TEST(Integration_P0_GitHubLogin_IssuedToken_AuthenticatedEndpointsWork)
{
    SOCIAL_SKIP_GUARD;
    if (!postgresAvailable())
    {
        CHECK(true);
        return;  // SKIP in memory mode: needs backend-svc + a real users row.
    }

    // Admin's internal id: resolved through the admin users list (same
    // pattern as UserAdminHardeningTest's last-admin checks).
    auto adminToken = loginAsAdminWithScope("openid profile admin");
    REQUIRE(adminToken.has_value());
    auto listResp = sendGet("/api/admin/users?q=admin", *adminToken);
    REQUIRE(listResp != nullptr);
    Json::Value listBody;
    REQUIRE(parseJsonBody(listResp, listBody));
    int32_t adminId = -1;
    for (const auto &u : listBody["users"])
    {
        if (u.get("username", "").asString() == "admin")
            adminId = static_cast<int32_t>(u.get("id", -1).asInt64());
    }
    REQUIRE(adminId > 0);

    // Pre-link (github, "60691") -> admin's internal id in the FAKE repo,
    // mapped to the admin row's REAL public_sub (#70: token rows store the
    // platform subject), so the issued tokens resolve on every authenticated
    // endpoint exactly like a password-flow token.
    auto db = ::drogon::app().getDbClient();
    REQUIRE(db != nullptr);
    auto adminRows = db->execSqlSync("SELECT public_sub FROM users WHERE id = $1", adminId);
    REQUIRE(adminRows.size() == 1);
    const std::string adminPublicSub = adminRows[0]["public_sub"].as<std::string>();
    REQUIRE(!adminPublicSub.empty());

    auto h = injectGitHubFake();
    h.accountRepo->userPublicSubs[adminId] = adminPublicSub;
    h.accountRepo->insertLink(
      "github",
      "60691",
      adminId,
      [](fulla::identity::LinkMutationStatus) {}
    );

    auto tokens = fakeGitHubLoginForTokens(h.http, 60691);
    REQUIRE(tokens.has_value());

    // Introspection via backend-svc HTTP Basic: the social access token must
    // answer active:true (the hash lookup that missed pre-fix). Mirrors
    // TokenIssuedAtIntrospectionTest's request construction (Basic header +
    // form body; F-017 forbids a body secret for client_secret_basic).
    ::drogon::HttpResponsePtr intro;
    try
    {
        auto client = ::drogon::HttpClient::newHttpClient(
          kTestBaseUrl, ::drogon::app().getLoop()
        );
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Post);
        req->setPath("/oauth2/introspect");
        req->setContentTypeCode(::drogon::CT_APPLICATION_X_FORM);
        req->addHeader(
          "Authorization", "Basic " + ::drogon::utils::base64Encode("backend-svc:test-secret")
        );
        req->setBody("token=" + tokens->first + "&client_id=backend-svc");
        auto [result, r] = client->sendRequest(req, 30.0);
        REQUIRE(result == ::drogon::ReqResult::Ok);
        intro = r;
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "introspect request failed: " << e.what();
    }
    REQUIRE(intro != nullptr);
    CHECK(statusIs(intro, drogon::k200OK));
    Json::Value introBody;
    REQUIRE(parseJsonBody(intro, introBody));
    CHECK(introBody.get("active", false).asBool());

    // Bearer-authenticated userinfo with the SOCIAL token: the auth filter's
    // hash lookup must find it (pre-fix: 401 on every authenticated call).
    auto socialInfo = sendGet("/oauth2/userinfo", tokens->first);
    REQUIRE(socialInfo != nullptr);
    CHECK(statusIs(socialInfo, drogon::k200OK));

    // The PR #68 numeric-dispatch branch: social tokens carry the internal id
    // as user_id; /api/me/social/links resolves it and serves the list.
    auto links = sendGet("/api/me/social/links", tokens->first);
    REQUIRE(links != nullptr);
    CHECK(statusIs(links, drogon::k200OK));

    // #75 closure: drive the FULL link -> list -> unlink lifecycle with the
    // SOCIAL token itself, so the numeric-dispatch branch is exercised for
    // the mutations too (previously every SocialLink test authenticated via
    // the admin's public_sub token). injectSocialLinkFake installs the
    // SocialLinkService fakes the mutation routes run against.
    auto h2 = injectSocialLinkFake();
    Json::Value linkTokenBody;
    linkTokenBody["access_token"] = "ghtok-75";
    h2.http->postFormResponses.push_back(fulla::identity::testing::okJson(linkTokenBody));
    Json::Value linkUserBody;
    linkUserBody["id"] = 60692;
    linkUserBody["login"] = "gh-user-60692";
    h2.http->getResponses.push_back(fulla::identity::testing::okJson(linkUserBody));
    // #71: the link POST must carry the one-time state minted for THIS user
    // (adminId) and provider; the handle's memory store mints synchronously.
    Json::Value codeJson;
    codeJson["code"] = "c-75";
    codeJson["state"] = h2.mintState(adminId, "github");
    auto linkResp = sendPostJson("/api/me/social/links/github", codeJson, tokens->first);
    REQUIRE(linkResp != nullptr);
    CHECK(statusIs(linkResp, drogon::k200OK));

    auto listAfter = sendGet("/api/me/social/links", tokens->first);
    REQUIRE(listAfter != nullptr);
    REQUIRE(statusIs(listAfter, drogon::k200OK));
    Json::Value socialListBody;
    REQUIRE(parseJsonBody(listAfter, socialListBody));
    CHECK(socialListBody["total"].asInt() == 1);
    CHECK(socialListBody["social_links"][0]["provider"].asString() == "github");

    // Unlink guard: mark the numeric id password-usable (the fake's
    // userHasUsablePassword set; the real PG admin does have a password).
    h2.accountRepo->usersWithUsablePassword.insert(adminId);
    auto unlinkResp = sendDelete("/api/me/social/links/github", tokens->first);
    REQUIRE(unlinkResp != nullptr);
    CHECK(statusIs(unlinkResp, drogon::k200OK));
}
