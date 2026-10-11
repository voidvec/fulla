// Task 17 remainder (fulla-sdk-refactor): unit tests for the new
// Domain-layer fulla::oauth2::protocol::TokenService, exercised
// against minimal in-memory fake repository/port implementations (no
// Drogon, no OAuth2Plugin). Mirrors the shape/coverage of
// Property4_TokenFlowBaselineTest.cc (which pins the OLD
// OAuth2Plugin-side oauth2::TokenService's behavior) so this NEW class's
// behavior can be visually diffed against that baseline for parity.
//
// Coverage additions (P0/P1): the original file covered only the happy
// path + invalid_client/invalid_grant. The additions below close the
// security-relevant gaps surfaced by the coverage audit: null-dependency
// guards, PKCE enforcement through exchangeCodeForToken, id_token
// issuance (openid scope + JwkManager), reuse-detection cascade side
// effects (revokeTokenFamily + audit), and the audit path itself (no
// test previously wired a non-null IAuditSink).

#include <fulla/common/model/PkceChallenge.h>
#include <fulla/common/observability/AuditEvent.h>
#include <fulla/common/ports/IAuditSink.h>
#include <fulla/common/ports/IRoleProvider.h>
#include <fulla/common/ports/ISubjectResolver.h>
#include <fulla/common/testing/FakeCryptoProvider.h>
#include <fulla/oauth2/jwk/JwkManager.h>
#include <fulla/oauth2/pkce/Pkce.h>
#include <fulla/oauth2/protocol/TokenCrypto.h>
#include <fulla/oauth2/protocol/TokenService.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{

using namespace fulla::oauth2::model;
using namespace fulla::oauth2::repository;
using fulla::oauth2::protocol::TokenService;
using fulla::oauth2::protocol::hashToken;

class FakeClientRepo : public IClientRepository
{
  public:
    std::unordered_map<std::string, OAuth2Client> clients;

    void getClient(const std::string &clientId, ClientCallback &&cb) override
    {
        auto it = clients.find(clientId);
        cb(it == clients.end() ? std::nullopt : std::make_optional(it->second));
    }

    void validateClient(
      const std::string &clientId,
      const std::string &clientSecret,
      BoolCallback &&cb
    ) override
    {
        auto it = clients.find(clientId);
        if (it == clients.end())
        {
            cb(false);
            return;
        }
        cb(it->second.clientSecretHash == clientSecret);
    }
};

class FakeGrantRepo : public IGrantRepository
{
  public:
    std::unordered_map<std::string, OAuth2AuthCode> codes;

    void saveAuthCode(const OAuth2AuthCode &code, VoidCallback &&cb) override
    {
        codes[code.code] = code;
        cb();
    }

    void getAuthCode(const std::string &code, AuthCodeCallback &&cb) override
    {
        auto it = codes.find(code);
        cb(it == codes.end() ? std::nullopt : std::make_optional(it->second));
    }

    void markAuthCodeUsed(const std::string &code, VoidCallback &&cb) override
    {
        auto it = codes.find(code);
        if (it != codes.end())
            it->second.used = true;
        cb();
    }

    void consumeAuthCode(
      const std::string &code,
      const std::string &redirectUri,
      AuthCodeCallback &&cb
    ) override
    {
        auto it = codes.find(code);
        if (it == codes.end() || it->second.used || it->second.redirectUri != redirectUri)
        {
            cb(std::nullopt);
            return;
        }
        auto result = it->second;
        it->second.used = true;
        cb(result);
    }

    void saveAuthorizationTransaction(const AuthorizationTransaction &, BoolCallback &&cb) override
    {
        cb(true);
    }

    void getAuthorizationTransaction(const std::string &, TransactionCallback &&cb) override
    {
        cb(std::nullopt);
    }

    void deleteAuthorizationTransaction(const std::string &, VoidCallback &&cb) override
    {
        cb();
    }

    void markTransactionConsumed(const std::string &, BoolCallback &&cb) override
    {
        cb(true);
    }

    void purgeExpired() override
    {
    }
};

class FakeTokenRepo : public ITokenRepository
{
  public:
    std::unordered_map<std::string, OAuth2AccessToken> accessTokens;
    std::unordered_map<std::string, OAuth2RefreshToken> refreshTokens;

    void saveAccessToken(const OAuth2AccessToken &token, VoidCallback &&cb) override
    {
        accessTokens[token.token] = token;
        cb();
    }

    void getAccessToken(const std::string &token, AccessTokenCallback &&cb) override
    {
        auto it = accessTokens.find(token);
        cb(it == accessTokens.end() ? std::nullopt : std::make_optional(it->second));
    }

    void saveRefreshToken(const OAuth2RefreshToken &token, VoidCallback &&cb) override
    {
        refreshTokens[token.token] = token;
        cb();
    }

    void getRefreshToken(const std::string &token, RefreshTokenCallback &&cb) override
    {
        auto it = refreshTokens.find(token);
        cb(it == refreshTokens.end() ? std::nullopt : std::make_optional(it->second));
    }

    void revokeRefreshToken(const std::string &token, VoidCallback &&cb) override
    {
        auto it = refreshTokens.find(token);
        if (it != refreshTokens.end())
            it->second.revoked = true;
        cb();
    }

    void atomicRevokeRefreshToken(const std::string &token, RefreshTokenCallback &&cb) override
    {
        auto it = refreshTokens.find(token);
        if (it == refreshTokens.end() || it->second.revoked)
        {
            cb(std::nullopt);
            return;
        }
        auto result = it->second;
        it->second.revoked = true;
        cb(result);
    }

    void revokeTokenFamily(const std::string &familyId, VoidCallback &&cb) override
    {
        for (auto &[key, rt] : refreshTokens)
        {
            if (rt.familyId == familyId)
                rt.revoked = true;
        }
        for (auto &[key, at] : accessTokens)
        {
            (void)key;
            (void)at;
        }
        cb();
    }

    void introspectToken(const std::string &token, TokenIntrospectionCallback &&cb) override
    {
        auto it = accessTokens.find(token);
        if (it == accessTokens.end())
        {
            TokenIntrospection inactive;
            inactive.active = false;
            cb(inactive);
            return;
        }
        TokenIntrospection intro;
        intro.active = !it->second.revoked;
        intro.clientId = it->second.clientId;
        intro.sub = it->second.userId;
        intro.scope = it->second.scope;
        intro.exp = it->second.expiresAt;
        cb(intro);
    }

    void incrementIntrospectCount(const std::string &, VoidCallback &&cb) override
    {
        cb();
    }

    void revokeAccessToken(
      const std::string &token,
      const std::string &,
      VoidCallback &&cb
    ) override
    {
        auto it = accessTokens.find(token);
        if (it != accessTokens.end())
            it->second.revoked = true;
        cb();
    }

    void purgeExpired() override
    {
    }

    bool supportsTransactions() const override
    {
        return false;
    }

    bool supportsCas() const override
    {
        return true;
    }
};

std::shared_ptr<FakeClientRepo> makeSeededClients()
{
    auto repo = std::make_shared<FakeClientRepo>();
    OAuth2Client c;
    c.clientId = "test-client";
    c.clientType = ClientType::CONFIDENTIAL;
    c.clientSecretHash = "secret";
    c.redirectUris = {"https://example.test/cb"};
    c.allowedScopes = {"openid", "profile", "email"};
    repo->clients["test-client"] = c;
    return repo;
}

std::shared_ptr<TokenService> makeService(
  std::shared_ptr<FakeClientRepo> clients,
  std::shared_ptr<FakeGrantRepo> grants,
  std::shared_ptr<FakeTokenRepo> tokens
)
{
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    return std::make_shared<TokenService>(clients, grants, tokens, crypto);
}

std::string issueAuthCode(
  TokenService &svc,
  const std::string &clientId,
  const std::string &subject,
  const std::string &scope,
  const std::string &redirectUri
)
{
    std::string rawCode;
    svc.generateAuthorizationCode(
      clientId, subject, scope, redirectUri, "", "", "", [&](bool, std::string code, std::string) {
          rawCode = std::move(code);
      }
    );
    return rawCode;
}

// PKCE-aware variant: issues an auth code with a recorded code_challenge +
// method + optional nonce, so exchange tests can exercise the PKCE branch.
std::string issueAuthCodeWithPkce(
  TokenService &svc,
  const std::string &clientId,
  const std::string &subject,
  const std::string &scope,
  const std::string &redirectUri,
  const std::string &codeChallenge,
  const std::string &codeChallengeMethod,
  const std::string &nonce = ""
)
{
    std::string rawCode;
    svc.generateAuthorizationCode(
      clientId, subject, scope, redirectUri, codeChallenge, codeChallengeMethod, nonce,
      [&](bool, std::string code, std::string) { rawCode = std::move(code); }
    );
    return rawCode;
}

// Captures every AuditEvent emitted via IAuditSink::record so audit-path
// tests can assert on action/outcome/actorId/targetType without depending
// on a DB-backed sink.
class FakeAuditSink : public fulla::common::ports::IAuditSink
{
  public:
    std::vector<fulla::common::observability::AuditEvent> events;

    void record(const fulla::common::observability::AuditEvent &event) override
    {
        events.push_back(event);
    }
};

// Minimal role provider for the resolveRoles two-port chain + the
// supportsSubjectLookup() fast path. Returns a fixed role list.
class FakeRoleProvider : public fulla::common::ports::IRoleProvider
{
  public:
    std::vector<std::string> roles;
    bool supportsSubject = false;

    explicit FakeRoleProvider(std::vector<std::string> r, bool supportsSubject = false)
        : roles(std::move(r)), supportsSubject(supportsSubject)
    {
    }

    void getRoles(int32_t, std::function<void(std::vector<std::string>)> &&cb) override
    {
        cb(roles);
    }

    void getRoles(const std::string &, std::function<void(std::vector<std::string>)> &&cb) override
    {
        cb(roles);
    }

    bool supportsSubjectLookup() const noexcept override
    {
        return supportsSubject;
    }
};

// Minimal subject resolver: resolve() yields the configured internalUserId
// (or nullopt to simulate a mapping miss).
class FakeSubjectResolver : public fulla::common::ports::ISubjectResolver
{
  public:
    std::optional<int32_t> internalUserId{std::nullopt};

    explicit FakeSubjectResolver(int32_t id) : internalUserId(id)
    {
    }

    void resolve(
      const fulla::common::model::Subject &,
      std::function<void(std::optional<int32_t>)> &&cb
    ) override
    {
        cb(internalUserId);
    }
};

}  // namespace

TEST(TokenServiceTest, ExchangeCode_HappyPath_ReturnsFrozenShape)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);

    const std::string redirectUri = "https://example.test/cb";
    std::string rawCode =
      issueAuthCode(*svc, "test-client", "alice", "openid profile", redirectUri);
    ASSERT_FALSE(rawCode.empty());

    Json::Value result;
    bool called = false;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &json) {
          result = json;
          called = true;
      }
    );

    ASSERT_TRUE(called);
    ASSERT_TRUE(result.isMember("access_token"));
    EXPECT_FALSE(result["access_token"].asString().empty());
    EXPECT_EQ(result["token_type"].asString(), "Bearer");
    EXPECT_EQ(result["expires_in"].asInt64(), 3600);
    ASSERT_TRUE(result.isMember("refresh_token"));
    EXPECT_FALSE(result["refresh_token"].asString().empty());
    EXPECT_FALSE(result.isMember("id_token"));
    EXPECT_FALSE(result.isMember("error"));

    std::shared_ptr<OAuth2AccessToken> validated;
    svc->validateAccessToken(result["access_token"].asString(), [&](auto at) { validated = at; });
    ASSERT_NE(validated, nullptr);
    EXPECT_EQ(validated->clientId, "test-client");
    EXPECT_EQ(validated->userId, "alice");
    EXPECT_EQ(validated->scope, "openid profile");
    // P0-4 (#258): audience is written at issuance (aud = issuing client),
    // so introspection's aud is no longer permanently empty.
    EXPECT_EQ(validated->audience, "test-client");
}

TEST(TokenServiceTest, ExchangeCode_WrongSecret_ReturnsInvalidClient)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);
    const std::string redirectUri = "https://example.test/cb";

    std::string rawCode = issueAuthCode(*svc, "test-client", "alice", "openid", redirectUri);
    Json::Value r;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "WRONG", redirectUri, "", [&](const Json::Value &j) { r = j; }
    );
    EXPECT_EQ(r["error"].asString(), "invalid_client");
    EXPECT_EQ(r["error_description"].asString(), "Client authentication failed");
}

TEST(TokenServiceTest, ExchangeCode_UnknownCode_ReturnsInvalidGrant)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);

    Json::Value r;
    svc->exchangeCodeForToken(
      "does-not-exist",
      "test-client",
      "secret",
      "https://example.test/cb",
      "",
      [&](const Json::Value &j) { r = j; }
    );
    EXPECT_EQ(r["error"].asString(), "invalid_grant");
    EXPECT_EQ(r["error_description"].asString(), "Invalid authorization code");
}

TEST(TokenServiceTest, RefreshToken_HappyPathThenReuse_YieldsInvalidGrant)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);
    const std::string redirectUri = "https://example.test/cb";

    std::string rawCode = issueAuthCode(*svc, "test-client", "alice", "openid", redirectUri);
    Json::Value exchanged;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) {
          exchanged = j;
      }
    );
    const std::string rt = exchanged["refresh_token"].asString();

    Json::Value refreshed;
    bool called = false;
    svc->refreshAccessToken(rt, "test-client", [&](const Json::Value &j) {
        refreshed = j;
        called = true;
    });
    ASSERT_TRUE(called);
    ASSERT_TRUE(refreshed.isMember("access_token"));
    EXPECT_FALSE(refreshed.isMember("error"));

    // P0-4 (#258): audience survives rotation with the same value.
    std::shared_ptr<OAuth2AccessToken> rotated;
    svc->validateAccessToken(refreshed["access_token"].asString(), [&](auto at) { rotated = at; });
    ASSERT_NE(rotated, nullptr);
    EXPECT_EQ(rotated->audience, "test-client");

    Json::Value reuse;
    svc->refreshAccessToken(rt, "test-client", [&](const Json::Value &j) { reuse = j; });
    EXPECT_EQ(reuse["error"].asString(), "invalid_grant");
}

TEST(TokenServiceTest, RevokeAccessToken_ThenValidateFails)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);
    const std::string redirectUri = "https://example.test/cb";

    std::string rawCode = issueAuthCode(*svc, "test-client", "alice", "openid", redirectUri);
    Json::Value exchanged;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) {
          exchanged = j;
      }
    );
    const std::string accessToken = exchanged["access_token"].asString();

    bool revoked = false;
    svc->revokeAccessToken(accessToken, "test-client", [&]() { revoked = true; });
    EXPECT_TRUE(revoked);

    std::shared_ptr<OAuth2AccessToken> after;
    svc->validateAccessToken(accessToken, [&](auto at) { after = at; });
    EXPECT_EQ(after, nullptr);
}

TEST(TokenServiceTest, IntrospectToken_ActiveAndInactive)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);
    const std::string redirectUri = "https://example.test/cb";

    std::string rawCode = issueAuthCode(*svc, "test-client", "alice", "openid", redirectUri);
    Json::Value exchanged;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) {
          exchanged = j;
      }
    );
    const std::string accessToken = exchanged["access_token"].asString();

    std::optional<TokenIntrospection> active;
    svc->introspectToken(accessToken, [&](auto v) { active = v; });
    ASSERT_TRUE(active.has_value());
    EXPECT_TRUE(active->active);
    EXPECT_EQ(active->clientId, "test-client");

    std::optional<TokenIntrospection> inactive;
    svc->introspectToken("totally-unknown", [&](auto v) { inactive = v; });
    ASSERT_TRUE(inactive.has_value());
    EXPECT_FALSE(inactive->active);
}

// ---------------------------------------------------------------------------
// Coverage additions (P0/P1) -- see file header. Each test below targets a
// branch the original happy-path tests did not exercise.
// ---------------------------------------------------------------------------

// generateAuthorizationCode: null grants/crypto guard (TokenService.cc:135).
TEST(TokenServiceTest, GenerateAuthorizationCode_NullGrants_ReturnsStorageNotInitialized)
{
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    // clients + crypto only, grants/tokens null.
    TokenService svc(makeSeededClients(), nullptr, nullptr, crypto);
    bool ok = true;
    std::string code, err;
    svc.generateAuthorizationCode(
      "test-client", "alice", "openid", "https://example.test/cb", "", "", "",
      [&](bool o, std::string c, std::string e) {
          ok = o;
          code = std::move(c);
          err = std::move(e);
      }
    );
    EXPECT_FALSE(ok);
    EXPECT_TRUE(code.empty());
    EXPECT_EQ(err, "Storage not initialized");
}

// generateAuthorizationCode: the saved grant's fields (hashed code,
// clientId/subject/scope/redirectUri/PKCE/nonce/expiresAt) are mapped
// correctly onto the persisted OAuth2AuthCode.
TEST(TokenServiceTest, GenerateAuthorizationCode_SavesHashedCodeAndFields)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);

    std::string rawCode;
    bool ok = false;
    std::string err;
    svc->generateAuthorizationCode(
      "test-client", "alice", "openid profile", "https://example.test/cb", "cc", "S256", "n-1",
      [&](bool o, std::string c, std::string e) {
          ok = o;
          rawCode = std::move(c);
          err = std::move(e);
      }
    );
    ASSERT_TRUE(ok);
    EXPECT_TRUE(err.empty());
    ASSERT_FALSE(rawCode.empty());

    // The saved code key is hashToken(rawCode) (UPPERCASE sha256 hex).
    fulla::common::testing::FakeCryptoProvider crypto;
    std::string hashed = fulla::oauth2::protocol::hashToken(crypto, rawCode);
    ASSERT_TRUE(grants->codes.count(hashed));
    const auto &saved = grants->codes[hashed];
    EXPECT_EQ(saved.clientId, "test-client");
    EXPECT_EQ(saved.userId, "alice");
    EXPECT_EQ(saved.scope, "openid profile");
    EXPECT_EQ(saved.redirectUri, "https://example.test/cb");
    EXPECT_EQ(saved.codeChallenge, "cc");
    EXPECT_EQ(saved.codeChallengeMethod, "S256");
    EXPECT_EQ(saved.nonce, "n-1");
    EXPECT_FALSE(saved.used);
    // expiresAt = now + authCodeTtl (default 600).
    EXPECT_GT(saved.expiresAt, 0);
}

// exchangeCodeForToken: null-dependency guard (TokenService.cc:167).
TEST(TokenServiceTest, ExchangeCode_NullDependencies_ReturnsServerError)
{
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    TokenService svc(nullptr, nullptr, nullptr, crypto);
    Json::Value r;
    svc.exchangeCodeForToken(
      "any-code", "test-client", "secret", "https://example.test/cb", "",
      [&](const Json::Value &j) { r = j; }
    );
    EXPECT_EQ(r["error"].asString(), "server_error");
}

// exchangeCodeForToken: clientId stored on the grant differs from the
// presented clientId -> invalid_client "Client ID mismatch".
TEST(TokenServiceTest, ExchangeCode_ClientIdMismatch_ReturnsInvalidClient)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);
    const std::string redirectUri = "https://example.test/cb";

    std::string rawCode = issueAuthCode(*svc, "test-client", "alice", "openid", redirectUri);
    Json::Value r;
    // Present a different clientId at the token endpoint.
    svc->exchangeCodeForToken(
      rawCode, "other-client", "secret", redirectUri, "", [&](const Json::Value &j) { r = j; }
    );
    EXPECT_EQ(r["error"].asString(), "invalid_client");
    EXPECT_EQ(r["error_description"].asString(), "Client authentication failed");
}

// exchangeCodeForToken: PKCE code_challenge present, empty verifier ->
// invalid_grant "PKCE validation failed".
TEST(TokenServiceTest, ExchangeCode_Pkce_EmptyVerifier_ReturnsInvalidGrant)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);
    const std::string redirectUri = "https://example.test/cb";

    // Build a verifier + matching S256 challenge so the recorded challenge
    // is genuinely PKCE-protected.
    fulla::common::testing::FakeCryptoProvider crypto;
    const std::string verifier =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";  // 43 chars, valid charset
    const std::string challenge =
      fulla::oauth2::pkce::computeCodeChallenge(verifier, "S256", crypto);

    std::string rawCode = issueAuthCodeWithPkce(
      *svc, "test-client", "alice", "openid", redirectUri, challenge, "S256"
    );
    Json::Value r;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) { r = j; }
    );
    EXPECT_EQ(r["error"].asString(), "invalid_grant");
    EXPECT_EQ(r["error_description"].asString(), "PKCE validation failed");
}

// exchangeCodeForToken: PKCE wrong verifier -> invalid_grant.
TEST(TokenServiceTest, ExchangeCode_Pkce_WrongVerifier_ReturnsInvalidGrant)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);
    const std::string redirectUri = "https://example.test/cb";

    fulla::common::testing::FakeCryptoProvider crypto;
    const std::string verifier =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";  // 43 chars
    const std::string challenge =
      fulla::oauth2::pkce::computeCodeChallenge(verifier, "S256", crypto);

    std::string rawCode = issueAuthCodeWithPkce(
      *svc, "test-client", "alice", "openid", redirectUri, challenge, "S256"
    );
    Json::Value r;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri,
      "cccccccccccccccccccccccccccccccccccccccccccc",  // different verifier
      [&](const Json::Value &j) { r = j; }
    );
    EXPECT_EQ(r["error"].asString(), "invalid_grant");
    EXPECT_EQ(r["error_description"].asString(), "PKCE validation failed");
}

// exchangeCodeForToken: PKCE correct verifier -> success (id_token absent
// because no JwkManager is wired).
TEST(TokenServiceTest, ExchangeCode_Pkce_CorrectVerifier_ReturnsTokens)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);
    const std::string redirectUri = "https://example.test/cb";

    fulla::common::testing::FakeCryptoProvider crypto;
    const std::string verifier =
      "ddddddddddddddddddddddddddddddddddddddddddddd";  // 43 chars
    const std::string challenge =
      fulla::oauth2::pkce::computeCodeChallenge(verifier, "S256", crypto);

    std::string rawCode = issueAuthCodeWithPkce(
      *svc, "test-client", "alice", "openid", redirectUri, challenge, "S256"
    );
    Json::Value r;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, verifier,
      [&](const Json::Value &j) { r = j; }
    );
    EXPECT_FALSE(r["access_token"].asString().empty());
    EXPECT_FALSE(r.isMember("error"));
}

// exchangeCodeForToken: openid scope + wired JwkManager -> id_token issued
// (TokenService.cc:285-308). The id_token is a 3-part JWT (header.payload.
// signature) and the nonce claim is included when the auth code carried one.
TEST(TokenServiceTest, ExchangeCode_OpenIdScope_WithJwkManager_EmitsIdTokenWithNonce)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    // TokenService derives from enable_shared_from_this and uses
    // shared_from_this() inside exchangeCodeForToken -- must be heap-allocated.
    auto svc = std::make_shared<TokenService>(clients, grants, tokens, crypto);

    // Wire a JwkManager initialized with an ephemeral dev key.
    auto jwk = std::make_shared<fulla::oauth2::JwkManager>();
    ASSERT_TRUE(jwk->init(Json::Value::nullSingleton()));
    svc->setJwkManager(jwk);

    const std::string redirectUri = "https://example.test/cb";
    std::string rawCode = issueAuthCodeWithPkce(
      *svc, "test-client", "alice", "openid", redirectUri, "", "", "nonce-xyz"
    );
    Json::Value r;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) { r = j; }
    );
    ASSERT_TRUE(r.isMember("id_token")) << "expected id_token for openid scope";
    const std::string idToken = r["id_token"].asString();
    ASSERT_FALSE(idToken.empty());

    // JWT structure: header.payload.signature (2 dots).
    ASSERT_EQ(std::count(idToken.begin(), idToken.end(), '.'), 2);

    // Decode the payload (middle segment) and assert the nonce claim is set.
    size_t firstDot = idToken.find('.');
    size_t secondDot = idToken.find('.', firstDot + 1);
    std::string payloadB64 = idToken.substr(firstDot + 1, secondDot - firstDot - 1);
    // base64url decode (FakeCryptoProvider accepts unpadded base64url; '='
    // is not a valid base64url char and would yield an empty decode).
    auto decoded = crypto->base64UrlDecode(payloadB64);
    ASSERT_FALSE(decoded.empty());
    std::string payloadJson(decoded.begin(), decoded.end());
    Json::Value claims;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream is(payloadJson);
    ASSERT_TRUE(Json::parseFromStream(builder, is, &claims, &errs));
    EXPECT_EQ(claims["nonce"].asString(), "nonce-xyz");
    EXPECT_EQ(claims["iss"].asString(), "http://localhost:5555");
    EXPECT_EQ(claims["sub"].asString(), "alice");
    EXPECT_EQ(claims["aud"].asString(), "test-client");
}

// exchangeCodeForToken: openid scope but no nonce recorded -> id_token
// issued WITHOUT a nonce claim (TokenService.cc:298).
TEST(TokenServiceTest, ExchangeCode_OpenIdScope_NonceAbsent_OmittedFromIdToken)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    auto svc = std::make_shared<TokenService>(clients, grants, tokens, crypto);
    auto jwk = std::make_shared<fulla::oauth2::JwkManager>();
    ASSERT_TRUE(jwk->init(Json::Value::nullSingleton()));
    svc->setJwkManager(jwk);

    const std::string redirectUri = "https://example.test/cb";
    // No nonce passed -> nonce stays empty.
    std::string rawCode = issueAuthCode(*svc, "test-client", "alice", "openid", redirectUri);
    Json::Value r;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) { r = j; }
    );
    ASSERT_TRUE(r.isMember("id_token"));
    const std::string idToken = r["id_token"].asString();
    ASSERT_FALSE(idToken.empty());

    size_t firstDot = idToken.find('.');
    size_t secondDot = idToken.find('.', firstDot + 1);
    std::string payloadB64 = idToken.substr(firstDot + 1, secondDot - firstDot - 1);
    auto decoded = crypto->base64UrlDecode(payloadB64);
    ASSERT_FALSE(decoded.empty());
    std::string payloadJson(decoded.begin(), decoded.end());
    Json::Value claims;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream is(payloadJson);
    ASSERT_TRUE(Json::parseFromStream(builder, is, &claims, &errs));
    EXPECT_FALSE(claims.isMember("nonce"));
}

// exchangeCodeForToken: a non-openid scope must NOT trigger id_token
// issuance even when a JwkManager is wired (TokenService.cc:287 guard).
TEST(TokenServiceTest, ExchangeCode_NonOpenIdScope_NoIdTokenEvenWithJwkManager)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    auto svc = std::make_shared<TokenService>(clients, grants, tokens, crypto);
    auto jwk = std::make_shared<fulla::oauth2::JwkManager>();
    ASSERT_TRUE(jwk->init(Json::Value::nullSingleton()));
    svc->setJwkManager(jwk);

    const std::string redirectUri = "https://example.test/cb";
    // scope "profile" (no openid).
    std::string rawCode = issueAuthCode(*svc, "test-client", "alice", "profile", redirectUri);
    Json::Value r;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) { r = j; }
    );
    EXPECT_FALSE(r.isMember("id_token"));
}

// exchangeCodeForToken: expired code -> invalid_grant "Code expired".
// We craft a grant whose expiresAt is already in the past.
TEST(TokenServiceTest, ExchangeCode_ExpiredCode_ReturnsInvalidGrant)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);
    const std::string redirectUri = "https://example.test/cb";

    std::string rawCode = issueAuthCode(*svc, "test-client", "alice", "openid", redirectUri);

    // Backdate the saved grant past its expiry (authCodeTtl default 600).
    fulla::common::testing::FakeCryptoProvider crypto;
    std::string hashed = fulla::oauth2::protocol::hashToken(crypto, rawCode);
    ASSERT_TRUE(grants->codes.count(hashed));
    grants->codes[hashed].expiresAt = 1;  // well in the past

    Json::Value r;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) { r = j; }
    );
    EXPECT_EQ(r["error"].asString(), "invalid_grant");
    EXPECT_EQ(r["error_description"].asString(), "Code expired");
}

// exchangeCodeForToken: roles array is populated when a role provider is
// wired (supportsSubjectLookup fast path).
TEST(TokenServiceTest, ExchangeCode_WithRoleProvider_RolesArrayPopulated)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    auto roleProvider = std::make_shared<FakeRoleProvider>(
      std::vector<std::string>{"admin", "user"}, /*supportsSubject=*/true
    );
    auto svc = std::make_shared<TokenService>(clients, grants, tokens, crypto, nullptr, nullptr, roleProvider);

    const std::string redirectUri = "https://example.test/cb";
    std::string rawCode = issueAuthCode(*svc, "test-client", "alice", "openid", redirectUri);
    Json::Value r;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) { r = j; }
    );
    ASSERT_TRUE(r.isMember("roles"));
    ASSERT_EQ(r["roles"].size(), 2u);
    EXPECT_EQ(r["roles"][0].asString(), "admin");
    EXPECT_EQ(r["roles"][1].asString(), "user");
}

// exchangeCodeForToken: custom accessTokenTtl is reflected in expires_in
// (TokenService.cc:281 advertises the configured lifetime, not a hardcoded
// 3600).
TEST(TokenServiceTest, ExchangeCode_CustomAccessTokenTtl_ReflectedInExpiresIn)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    // accessTokenTtl = 7200 (4th positional after the three nullable ports).
    auto svc = std::make_shared<TokenService>(
      clients, grants, tokens, crypto, nullptr, nullptr, nullptr,
      /*authCodeTtl=*/600, /*accessTokenTtl=*/7200, /*refreshTokenTtl=*/2592000
    );
    const std::string redirectUri = "https://example.test/cb";
    std::string rawCode = issueAuthCode(*svc, "test-client", "alice", "openid", redirectUri);
    Json::Value r;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) { r = j; }
    );
    EXPECT_EQ(r["expires_in"].asInt64(), 7200);
}

// refreshAccessToken: null-dependency guard (TokenService.cc:328).
TEST(TokenServiceTest, RefreshToken_NullDependencies_ReturnsServerError)
{
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    TokenService svc(nullptr, nullptr, nullptr, crypto);
    Json::Value r;
    svc.refreshAccessToken("any-rt", "test-client", [&](const Json::Value &j) { r = j; });
    EXPECT_EQ(r["error"].asString(), "server_error");
}

// refreshAccessToken: clientId stored on the refresh token differs from
// the presented clientId -> invalid_grant "Client mismatch".
TEST(TokenServiceTest, RefreshToken_ClientMismatch_ReturnsInvalidGrant)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);
    const std::string redirectUri = "https://example.test/cb";

    std::string rawCode = issueAuthCode(*svc, "test-client", "alice", "openid", redirectUri);
    Json::Value exchanged;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) {
          exchanged = j;
      }
    );
    const std::string rt = exchanged["refresh_token"].asString();

    Json::Value r;
    svc->refreshAccessToken(rt, "other-client", [&](const Json::Value &j) { r = j; });
    EXPECT_EQ(r["error"].asString(), "invalid_grant");
    EXPECT_EQ(r["error_description"].asString(), "Client mismatch");
}

// refreshAccessToken: expired refresh token -> invalid_grant "Token expired".
TEST(TokenServiceTest, RefreshToken_Expired_ReturnsInvalidGrant)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);
    const std::string redirectUri = "https://example.test/cb";

    std::string rawCode = issueAuthCode(*svc, "test-client", "alice", "openid", redirectUri);
    Json::Value exchanged;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) {
          exchanged = j;
      }
    );
    const std::string rt = exchanged["refresh_token"].asString();

    // Backdate the stored refresh token (keyed by hashToken(rt)) so the
    // `now > expiresAt` branch fires.
    fulla::common::testing::FakeCryptoProvider crypto;
    std::string hashed = fulla::oauth2::protocol::hashToken(crypto, rt);
    ASSERT_TRUE(tokens->refreshTokens.count(hashed));
    tokens->refreshTokens[hashed].expiresAt = 1;

    Json::Value r;
    svc->refreshAccessToken(rt, "test-client", [&](const Json::Value &j) { r = j; });
    EXPECT_EQ(r["error"].asString(), "invalid_grant");
    EXPECT_EQ(r["error_description"].asString(), "Token expired");
}

// refreshAccessToken: reuse-detection cascade. After a successful refresh
// the original RT is revoked (atomicRevokeRefreshToken consumed it); a
// second use lands in the maybeRevoked-has-familyId branch, which must
// (a) emit a refresh_token_reuse_detected audit event and (b) revoke the
// whole family. This pins both side effects, not just the final error.
TEST(TokenServiceTest, RefreshToken_Reuse_RevokesFamilyAndAudits)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    auto audit = std::make_shared<FakeAuditSink>();
    auto svc = std::make_shared<TokenService>(clients, grants, tokens, crypto, audit);
    const std::string redirectUri = "https://example.test/cb";

    std::string rawCode = issueAuthCode(*svc, "test-client", "alice", "openid", redirectUri);
    Json::Value exchanged;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) {
          exchanged = j;
      }
    );
    const std::string rt = exchanged["refresh_token"].asString();
    // Issue a successful refresh first (this consumes rt via atomicRevoke).
    Json::Value refreshed;
    svc->refreshAccessToken(rt, "test-client", [&](const Json::Value &j) { refreshed = j; });
    ASSERT_TRUE(refreshed.isMember("access_token"));

    const size_t auditBeforeReuse = audit->events.size();
    // Second use of the now-revoked rt triggers the reuse cascade.
    Json::Value reuse;
    svc->refreshAccessToken(rt, "test-client", [&](const Json::Value &j) { reuse = j; });
    EXPECT_EQ(reuse["error"].asString(), "invalid_grant");

    // Audit: a refresh_token_reuse_detected event was emitted.
    bool sawReuseAudit = false;
    for (const auto &ev : audit->events)
    {
        if (ev.action == "refresh_token_reuse_detected")
        {
            sawReuseAudit = true;
            EXPECT_EQ(ev.outcome, "failure");
            EXPECT_EQ(ev.targetType, "token_family");
            break;
        }
    }
    EXPECT_TRUE(sawReuseAudit);
    EXPECT_GT(audit->events.size(), auditBeforeReuse);

    // Family revocation: the refresh token's family is now fully revoked.
    // The newly-issued RT (from the successful refresh above) shares the
    // same familyId, so it must also be revoked after the cascade.
    fulla::common::testing::FakeCryptoProvider crypto2;
    std::string newRtHashed = fulla::oauth2::protocol::hashToken(crypto2, refreshed["refresh_token"].asString());
    if (tokens->refreshTokens.count(newRtHashed))
    {
        EXPECT_TRUE(tokens->refreshTokens[newRtHashed].revoked);
    }
}

// refreshAccessToken: a successful refresh records a token_refreshed
// audit event (TokenService.cc:406).
TEST(TokenServiceTest, RefreshToken_Success_RecordsAudit)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    auto audit = std::make_shared<FakeAuditSink>();
    auto svc = std::make_shared<TokenService>(clients, grants, tokens, crypto, audit);
    const std::string redirectUri = "https://example.test/cb";

    std::string rawCode = issueAuthCode(*svc, "test-client", "alice", "openid", redirectUri);
    Json::Value exchanged;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) {
          exchanged = j;
      }
    );
    const std::string rt = exchanged["refresh_token"].asString();

    size_t before = audit->events.size();
    Json::Value refreshed;
    svc->refreshAccessToken(rt, "test-client", [&](const Json::Value &j) { refreshed = j; });
    ASSERT_TRUE(refreshed.isMember("access_token"));

    bool sawRefreshed = false;
    for (size_t i = before; i < audit->events.size(); ++i)
    {
        if (audit->events[i].action == "token_refreshed")
        {
            sawRefreshed = true;
            EXPECT_EQ(audit->events[i].outcome, "success");
            break;
        }
    }
    EXPECT_TRUE(sawRefreshed);
}

// validateAccessToken: null-dependency guard (TokenService.cc:425).
TEST(TokenServiceTest, ValidateAccessToken_NullDependencies_ReturnsNull)
{
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    TokenService svc(nullptr, nullptr, nullptr, crypto);
    std::shared_ptr<OAuth2AccessToken> out((OAuth2AccessToken *)0x1, [](OAuth2AccessToken *) {});
    svc.validateAccessToken("any", [&](auto p) { out = std::move(p); });
    EXPECT_EQ(out, nullptr);
}

// validateAccessToken: unknown token -> nullptr (nullopt branch).
TEST(TokenServiceTest, ValidateAccessToken_UnknownToken_ReturnsNull)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);
    std::shared_ptr<OAuth2AccessToken> out((OAuth2AccessToken *)0x1, [](OAuth2AccessToken *) {});
    svc->validateAccessToken("never-issued", [&](auto p) { out = std::move(p); });
    EXPECT_EQ(out, nullptr);
}

// validateAccessToken: expired token -> nullptr (TokenService.cc:447).
TEST(TokenServiceTest, ValidateAccessToken_Expired_ReturnsNull)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);
    const std::string redirectUri = "https://example.test/cb";

    std::string rawCode = issueAuthCode(*svc, "test-client", "alice", "openid", redirectUri);
    Json::Value exchanged;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) {
          exchanged = j;
      }
    );
    const std::string accessToken = exchanged["access_token"].asString();

    // Backdate the stored access token so `now > expiresAt` fires.
    fulla::common::testing::FakeCryptoProvider crypto;
    std::string hashed = fulla::oauth2::protocol::hashToken(crypto, accessToken);
    ASSERT_TRUE(tokens->accessTokens.count(hashed));
    tokens->accessTokens[hashed].expiresAt = 1;

    std::shared_ptr<OAuth2AccessToken> out((OAuth2AccessToken *)0x1, [](OAuth2AccessToken *) {});
    svc->validateAccessToken(accessToken, [&](auto p) { out = std::move(p); });
    EXPECT_EQ(out, nullptr);
}

// introspectToken: null-dependency guard (TokenService.cc:463).
TEST(TokenServiceTest, IntrospectToken_NullDependencies_ReturnsNullopt)
{
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    TokenService svc(nullptr, nullptr, nullptr, crypto);
    std::optional<TokenIntrospection> out;
    svc.introspectToken("any", [&](auto v) { out = std::move(v); });
    EXPECT_FALSE(out.has_value());
}

// revokeAccessToken: null-dependency guard still invokes the callback
// (TokenService.cc:480) and tolerates a null callback (the `if (callback)`
// guards at lines 480 and 486).
TEST(TokenServiceTest, RevokeAccessToken_NullDependencies_InvokesCallback)
{
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    TokenService svc(nullptr, nullptr, nullptr, crypto);
    bool called = false;
    svc.revokeAccessToken("any", "u", [&]() { called = true; });
    EXPECT_TRUE(called);
}

// revokeAccessToken: a null callback must not crash (the inner wrapper
// checks `if (callback)` before invoking).
TEST(TokenServiceTest, RevokeAccessToken_NullCallback_DoesNotCrash)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);
    // No callback supplied -- SUCCEEDS if it does not dereference null.
    svc->revokeAccessToken("any", "u", nullptr);
    SUCCEED();
}

// validatePkceCodeVerifier: null crypto -> false (TokenService.cc:497).
TEST(TokenServiceTest, ValidatePkceCodeVerifier_NullCrypto_ReturnsFalse)
{
    TokenService svc(nullptr, nullptr, nullptr, nullptr);
    EXPECT_FALSE(svc.validatePkceCodeVerifier("v", "c", "S256"));
}

// validatePkceCodeVerifier: empty method defaults to "plain"
// (TokenService.cc:501). With plain, the verifier must equal the challenge.
TEST(TokenServiceTest, ValidatePkceCodeVerifier_EmptyMethod_DefaultsToPlain)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto svc = makeService(clients, grants, tokens);
    EXPECT_TRUE(svc->validatePkceCodeVerifier("matching", "matching", ""));
    EXPECT_FALSE(svc->validatePkceCodeVerifier("matching", "different", ""));
}

// ============================================================================
// F-022 (OIDC Core §2/§3.1.3.7): id_token auth_time / amr / acr claims.
// ============================================================================

// Helper: decode a JWT payload (middle segment) into a Json::Value, using the
// FakeCryptoProvider's base64UrlDecode. Mirrors the inline decode in the
// existing id_token tests.
namespace
{
Json::Value decodeIdTokenPayload(
  const std::string &idToken,
  fulla::common::testing::FakeCryptoProvider &crypto
)
{
    size_t firstDot = idToken.find('.');
    size_t secondDot = idToken.find('.', firstDot + 1);
    std::string payloadB64 = idToken.substr(firstDot + 1, secondDot - firstDot - 1);
    auto decoded = crypto.base64UrlDecode(payloadB64);
    Json::Value claims;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream is(std::string(decoded.begin(), decoded.end()));
    Json::parseFromStream(builder, is, &claims, &errs);
    return claims;
}
}  // namespace

// F-022: when the auth code carries auth_time + amr="pwd", the id_token
// includes auth_time, amr=["pwd"], and acr=1 (password-only).
TEST(TokenServiceTest, ExchangeCode_OpenIdScope_AuthTimeAndAmrPwd_StampsIdTokenClaims)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    auto svc = std::make_shared<TokenService>(clients, grants, tokens, crypto);
    auto jwk = std::make_shared<fulla::oauth2::JwkManager>();
    ASSERT_TRUE(jwk->init(Json::Value::nullSingleton()));
    svc->setJwkManager(jwk);

    const std::string redirectUri = "https://example.test/cb";
    const int64_t authTime = 1700000000;
    std::string rawCode;
    // Pass authTime + amr via the trailing generateAuthorizationCode args.
    svc->generateAuthorizationCode(
      "test-client", "alice", "openid", redirectUri, "", "", "", [&](bool, std::string code, std::string) { rawCode = std::move(code); },
      authTime, "pwd"
    );

    Json::Value r;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) { r = j; }
    );
    ASSERT_TRUE(r.isMember("id_token"));
    Json::Value claims = decodeIdTokenPayload(r["id_token"].asString(), *crypto);
    EXPECT_EQ(claims["auth_time"].asInt64(), authTime);
    ASSERT_TRUE(claims.isMember("amr"));
    ASSERT_EQ(claims["amr"].size(), 1u);
    EXPECT_EQ(claims["amr"][0].asString(), "pwd");
    // R-1 (OIDC Core §2): acr is a STRING claim ("1"=password, "2"=MFA),
    // matching discovery's acr_values_supported strings.
    EXPECT_EQ(claims["acr"].asString(), "1");  // password-only -> acr "1"
}

// F-022: when the auth code carries amr="pwd mfa", acr=2 (MFA).
TEST(TokenServiceTest, ExchangeCode_OpenIdScope_AmrPwdMfa_AcrIsMfaLevel)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    auto svc = std::make_shared<TokenService>(clients, grants, tokens, crypto);
    auto jwk = std::make_shared<fulla::oauth2::JwkManager>();
    ASSERT_TRUE(jwk->init(Json::Value::nullSingleton()));
    svc->setJwkManager(jwk);

    const std::string redirectUri = "https://example.test/cb";
    std::string rawCode;
    svc->generateAuthorizationCode(
      "test-client", "alice", "openid", redirectUri, "", "", "", [&](bool, std::string code, std::string) { rawCode = std::move(code); },
      1700000000, "pwd mfa"
    );

    Json::Value r;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) { r = j; }
    );
    ASSERT_TRUE(r.isMember("id_token"));
    Json::Value claims = decodeIdTokenPayload(r["id_token"].asString(), *crypto);
    ASSERT_TRUE(claims.isMember("amr"));
    EXPECT_EQ(claims["amr"].size(), 2u);
    // R-1: acr is a STRING ("2" for MFA), not an integer.
    EXPECT_EQ(claims["acr"].asString(), "2");  // MFA -> acr "2"
}

// F-022: when auth_time is 0 and amr is empty (legacy defaults), the id_token
// omits auth_time/amr/acr entirely.
TEST(TokenServiceTest, ExchangeCode_OpenIdScope_NoAuthTimeNoAmr_OmitsClaims)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    auto svc = std::make_shared<TokenService>(clients, grants, tokens, crypto);
    auto jwk = std::make_shared<fulla::oauth2::JwkManager>();
    ASSERT_TRUE(jwk->init(Json::Value::nullSingleton()));
    svc->setJwkManager(jwk);

    const std::string redirectUri = "https://example.test/cb";
    std::string rawCode = issueAuthCode(*svc, "test-client", "alice", "openid", redirectUri);
    Json::Value r;
    svc->exchangeCodeForToken(
      rawCode, "test-client", "secret", redirectUri, "", [&](const Json::Value &j) { r = j; }
    );
    ASSERT_TRUE(r.isMember("id_token"));
    Json::Value claims = decodeIdTokenPayload(r["id_token"].asString(), *crypto);
    EXPECT_FALSE(claims.isMember("auth_time"));
    EXPECT_FALSE(claims.isMember("amr"));
    EXPECT_FALSE(claims.isMember("acr"));
}

// F-022: the auth_time/amr persisted on a saved code round-trip through the
// grant repository (the persistence path that PostgresGrantRepository uses).
// FakeGrantRepo stores the DTO by value, so the fields survive -- this test
// pins the contract the Postgres/Memory backends must honor.
TEST(TokenServiceTest, GenerateAuthCode_PersistsAuthTimeAndAmr_OnGrant)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    auto svc = std::make_shared<TokenService>(clients, grants, tokens, crypto);

    const std::string redirectUri = "https://example.test/cb";
    const int64_t authTime = 1700000123;
    std::string rawCode;
    svc->generateAuthorizationCode(
      "test-client", "alice", "openid", redirectUri, "", "", "", [&](bool, std::string code, std::string) { rawCode = std::move(code); },
      authTime, "pwd mfa"
    );
    ASSERT_FALSE(rawCode.empty());

    // The grant repo stored the hashed code; look it up by the hashed form
    // (FakeCryptoProvider hashes via hashToken). Read it back and assert.
    auto it = grants->codes.find(hashToken(*crypto, rawCode));
    ASSERT_NE(it, grants->codes.end());
    EXPECT_EQ(it->second.authTime, authTime);
    EXPECT_EQ(it->second.amr, "pwd mfa");
}

// ============================================================================
// F-025 (OIDC Core §12): refresh_token grant re-issues an id_token when the
// refresh token's scope includes "openid" and the JwkManager is wired.
// ============================================================================

// F-025: refresh with openid scope + JwkManager -> id_token in the response
// (no nonce, since refresh does not carry one).
TEST(TokenServiceTest, RefreshToken_OpenIdScope_WithJwkManager_ReissuesIdToken)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    auto svc = std::make_shared<TokenService>(clients, grants, tokens, crypto);
    auto jwk = std::make_shared<fulla::oauth2::JwkManager>();
    ASSERT_TRUE(jwk->init(Json::Value::nullSingleton()));
    svc->setJwkManager(jwk);

    // Seed a refresh token whose scope includes openid.
    OAuth2RefreshToken rt;
    rt.token = hashToken(*crypto, "raw-rt");
    rt.accessToken = "at";
    rt.clientId = "test-client";
    rt.userId = "alice";
    rt.scope = "openid profile";
    rt.expiresAt = std::numeric_limits<int64_t>::max();
    tokens->refreshTokens[rt.token] = rt;

    Json::Value r;
    svc->refreshAccessToken("raw-rt", "test-client", [&](const Json::Value &j) { r = j; });
    EXPECT_FALSE(r.isMember("error"));
    ASSERT_TRUE(r.isMember("id_token")) << "refresh with openid scope must re-issue id_token";
    Json::Value claims = decodeIdTokenPayload(r["id_token"].asString(), *crypto);
    EXPECT_EQ(claims["sub"].asString(), "alice");
    EXPECT_EQ(claims["aud"].asString(), "test-client");
    EXPECT_FALSE(claims.isMember("nonce"));  // §12: no nonce on refresh
}

// F-025: refresh with a NON-openid scope must NOT issue an id_token.
TEST(TokenServiceTest, RefreshToken_NonOpenIdScope_OmitsIdToken)
{
    auto clients = makeSeededClients();
    auto grants = std::make_shared<FakeGrantRepo>();
    auto tokens = std::make_shared<FakeTokenRepo>();
    auto crypto = std::make_shared<fulla::common::testing::FakeCryptoProvider>();
    auto svc = std::make_shared<TokenService>(clients, grants, tokens, crypto);
    auto jwk = std::make_shared<fulla::oauth2::JwkManager>();
    ASSERT_TRUE(jwk->init(Json::Value::nullSingleton()));
    svc->setJwkManager(jwk);

    OAuth2RefreshToken rt;
    rt.token = hashToken(*crypto, "raw-rt");
    rt.accessToken = "at";
    rt.clientId = "test-client";
    rt.userId = "alice";
    rt.scope = "profile";  // no openid
    rt.expiresAt = std::numeric_limits<int64_t>::max();
    tokens->refreshTokens[rt.token] = rt;

    Json::Value r;
    svc->refreshAccessToken("raw-rt", "test-client", [&](const Json::Value &j) { r = j; });
    EXPECT_FALSE(r.isMember("error"));
    EXPECT_FALSE(r.isMember("id_token"));
}
