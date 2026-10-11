// Task 17 remainder (fulla-sdk-refactor): unit tests for the new
// Domain-layer fulla::oauth2::protocol::ClientService, ported onto
// IClientRepository instead of the old god interface.

#include <fulla/oauth2/protocol/ClientService.h>

#include <gtest/gtest.h>

#include <unordered_map>

namespace
{

using namespace fulla::oauth2::model;
using namespace fulla::oauth2::repository;
using fulla::oauth2::protocol::ClientService;

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
        cb(it != clients.end() && it->second.clientSecretHash == clientSecret);
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
    c.allowedScopes = {"openid", "profile"};
    repo->clients["test-client"] = c;
    return repo;
}

}  // namespace

TEST(ClientServiceTest, ValidateClient_CorrectSecret_ReturnsTrue)
{
    auto repo = makeSeededClients();
    ClientService svc(repo);

    bool valid = false;
    svc.validateClient("test-client", "secret", [&](bool v) { valid = v; });
    EXPECT_TRUE(valid);
}

TEST(ClientServiceTest, ValidateClient_WrongSecret_ReturnsFalse)
{
    auto repo = makeSeededClients();
    ClientService svc(repo);

    bool valid = true;
    svc.validateClient("test-client", "wrong", [&](bool v) { valid = v; });
    EXPECT_FALSE(valid);
}

TEST(ClientServiceTest, ValidateRedirectUri_Registered_ReturnsTrue)
{
    auto repo = makeSeededClients();
    ClientService svc(repo);

    bool valid = false;
    svc.validateRedirectUri("test-client", "https://example.test/cb", [&](bool v) { valid = v; });
    EXPECT_TRUE(valid);
}

TEST(ClientServiceTest, ValidateRedirectUri_Unregistered_ReturnsFalse)
{
    auto repo = makeSeededClients();
    ClientService svc(repo);

    bool valid = true;
    svc.validateRedirectUri("test-client", "https://evil.test/cb", [&](bool v) { valid = v; });
    EXPECT_FALSE(valid);
}

// P0-7b (#261): empty post_logout_redirect_uris = legacy fallback — the
// LOGIN redirect_uris decide (pre-v1.5.0 behavior preserved).
TEST(ClientServiceTest, ValidatePostLogoutRedirectUri_EmptyWhitelist_FallsBackToLoginUris)
{
    auto repo = makeSeededClients();
    ClientService svc(repo);

    bool loginUriValid = false;
    svc.validatePostLogoutRedirectUri(
      "test-client", "https://example.test/cb", [&](bool v) { loginUriValid = v; }
    );
    EXPECT_TRUE(loginUriValid);

    bool otherUriValid = true;
    svc.validatePostLogoutRedirectUri(
      "test-client", "https://evil.test/cb", [&](bool v) { otherUriValid = v; }
    );
    EXPECT_FALSE(otherUriValid);
}

// P0-7b: a non-empty dedicated whitelist is AUTHORITATIVE — a URI valid for
// LOGIN but not registered for logout must be rejected, and a logout-only
// URI (not a login redirect) must be accepted.
TEST(ClientServiceTest, ValidatePostLogoutRedirectUri_WhitelistIsAuthoritative)
{
    auto repo = makeSeededClients();
    OAuth2Client &c = repo->clients["test-client"];
    c.postLogoutRedirectUris = {"https://example.test/logged-out"};
    ClientService svc(repo);

    bool dedicatedValid = false;
    svc.validatePostLogoutRedirectUri(
      "test-client", "https://example.test/logged-out", [&](bool v) { dedicatedValid = v; }
    );
    EXPECT_TRUE(dedicatedValid);

    // Registered for LOGIN but NOT for logout -> rejected once the
    // dedicated whitelist exists.
    bool loginOnlyValid = true;
    svc.validatePostLogoutRedirectUri(
      "test-client", "https://example.test/cb", [&](bool v) { loginOnlyValid = v; }
    );
    EXPECT_FALSE(loginOnlyValid);
}

TEST(ClientServiceTest, ValidateClientScopes_AllAllowed_ReturnsTrue)
{
    auto repo = makeSeededClients();
    ClientService svc(repo);

    bool valid = false;
    std::string msg;
    svc.validateClientScopes("test-client", {"openid", "profile"}, [&](bool v, std::string m) {
        valid = v;
        msg = std::move(m);
    });
    EXPECT_TRUE(valid);
    EXPECT_TRUE(msg.empty());
}

TEST(ClientServiceTest, ValidateClientScopes_SomeDisallowed_ReturnsFalseWithMessage)
{
    auto repo = makeSeededClients();
    ClientService svc(repo);

    bool valid = true;
    std::string msg;
    svc.validateClientScopes("test-client", {"openid", "admin"}, [&](bool v, std::string m) {
        valid = v;
        msg = std::move(m);
    });
    EXPECT_FALSE(valid);
    EXPECT_NE(msg.find("admin"), std::string::npos);
}

TEST(ClientServiceTest, ValidateClientScopes_UnknownClient_ReturnsClientNotFound)
{
    auto repo = makeSeededClients();
    ClientService svc(repo);

    bool valid = true;
    std::string msg;
    svc.validateClientScopes("nonexistent", {"openid"}, [&](bool v, std::string m) {
        valid = v;
        msg = std::move(m);
    });
    EXPECT_FALSE(valid);
    EXPECT_EQ(msg, "Client not found");
}

// ---------------------------------------------------------------------------
// Coverage additions (P1): null-repository guards and the comma-joining
// loop for multiple disallowed scopes (ClientService.cc:82-85), neither of
// which the original happy-path tests exercised.
// ---------------------------------------------------------------------------

// validateClient: null clients_ guard -> callback(false).
TEST(ClientServiceTest, ValidateClient_NullRepo_ReturnsFalse)
{
    ClientService svc(nullptr);
    bool valid = true;
    svc.validateClient("test-client", "secret", [&](bool v) { valid = v; });
    EXPECT_FALSE(valid);
}

// validateRedirectUri: null clients_ guard -> callback(false).
TEST(ClientServiceTest, ValidateRedirectUri_NullRepo_ReturnsFalse)
{
    ClientService svc(nullptr);
    bool valid = true;
    svc.validateRedirectUri("test-client", "https://example.test/cb", [&](bool v) { valid = v; });
    EXPECT_FALSE(valid);
}

// validateRedirectUri: client-not-found branch -> callback(false).
TEST(ClientServiceTest, ValidateRedirectUri_UnknownClient_ReturnsFalse)
{
    auto repo = makeSeededClients();
    ClientService svc(repo);
    bool valid = true;
    svc.validateRedirectUri("nonexistent", "https://example.test/cb", [&](bool v) { valid = v; });
    EXPECT_FALSE(valid);
}

// validateClientScopes: null clients_ guard -> callback(false, "Storage not
// initialized"). Pins the literal message the controllers surface.
TEST(ClientServiceTest, ValidateClientScopes_NullRepo_ReturnsStorageNotInitialized)
{
    ClientService svc(nullptr);
    bool valid = true;
    std::string msg;
    svc.validateClientScopes("test-client", {"openid"}, [&](bool v, std::string m) {
        valid = v;
        msg = std::move(m);
    });
    EXPECT_FALSE(valid);
    EXPECT_EQ(msg, "Storage not initialized");
}

// validateClientScopes: multiple disallowed scopes -> comma-joined message
// listing every offender (ClientService.cc:82-85). The single-disallowed
// test above does not exercise the loop's accumulation branch.
TEST(ClientServiceTest, ValidateClientScopes_MultipleDisallowed_ListsAllInMessage)
{
    auto repo = makeSeededClients();
    ClientService svc(repo);

    bool valid = true;
    std::string msg;
    svc.validateClientScopes(
      "test-client", {"admin", "write", "delete"}, [&](bool v, std::string m) {
          valid = v;
          msg = std::move(m);
      }
    );
    EXPECT_FALSE(valid);
    EXPECT_NE(msg.find("admin"), std::string::npos);
    EXPECT_NE(msg.find("write"), std::string::npos);
    EXPECT_NE(msg.find("delete"), std::string::npos);
    // Comma-join separator is present.
    EXPECT_NE(msg.find(", "), std::string::npos);
}

// validateClientScopes: empty requestedScopes -> vacuously valid (no
// iterations, no invalid scopes).
TEST(ClientServiceTest, ValidateClientScopes_EmptyRequestedScopes_ReturnsTrue)
{
    auto repo = makeSeededClients();
    ClientService svc(repo);

    bool valid = false;
    std::string msg = "unchanged";
    svc.validateClientScopes("test-client", {}, [&](bool v, std::string m) {
        valid = v;
        msg = std::move(m);
    });
    EXPECT_TRUE(valid);
    EXPECT_TRUE(msg.empty());
}
