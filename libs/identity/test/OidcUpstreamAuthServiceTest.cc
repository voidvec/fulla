// OidcUpstreamAuthServiceTest.cc
//
// P2-1 unit tests for the generic OIDC-in federation service. Everything
// upstream is faked: the transport seams are immediate-callback lambdas and
// the account repository is a minimal in-memory ISocialAccountRepository
// (no DB, no network, no Drogon server) -- same philosophy as
// SocialAuthServiceTest.cc.

#ifdef WITH_SOCIAL

#include <fulla/identity/OidcUpstreamAuthService.h>
#include <fulla/identity/ISocialAccountRepository.h>

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{

using fulla::identity::ISocialAccountRepository;
using fulla::identity::LinkMutationStatus;
using fulla::identity::LinkNewSocialAccountResult;
using fulla::identity::OidcUpstreamAuthService;
using fulla::identity::SocialAccountLookup;
using fulla::identity::SocialLinkStatus;
using fulla::identity::UpstreamProviderRecord;

// Immediate (synchronous) fake of the account repository. Only the states
// the service's flow reaches are modelled: linked / no-mapping /
// create-success.
class ImmediateAccountRepository : public ISocialAccountRepository
{
  public:
    struct Link
    {
        std::string provider;
        std::string subject;
        int32_t userId = 0;
        std::string username;
        std::string publicSub;
    };
    std::vector<Link> links;
    std::atomic<int> creates{0};

    void findLinkedUser(
      const std::string &provider,
      const std::string &subject,
      LookupCallback &&cb) override
    {
        for (const auto &link : links)
        {
            if (link.provider == provider && link.subject == subject)
            {
                SocialAccountLookup lookup;
                lookup.userId = link.userId;
                lookup.username = link.username;
                lookup.publicSub = link.publicSub;
                cb(SocialLinkStatus::Linked, lookup);
                return;
            }
        }
        cb(SocialLinkStatus::NoMapping, SocialAccountLookup{});
    }

    void createLinkedUser(
      const std::string &provider,
      const std::string &subject,
      const std::string &username,
      const std::string & /*email*/,
      CreateCallback &&cb) override
    {
        ++creates;
        LinkNewSocialAccountResult result;
        result.userId = ++lastUserId;
        result.username = "u_" + username;
        result.publicSub = "pub_" + username;
        cb(result);
    }

    // Unused by the login flow.
    void listForUser(int32_t, LinkEntriesCallback &&cb) override
    {
        cb(std::nullopt);
    }
    void insertLink(const std::string &, const std::string &, int32_t, LinkMutationCallback &&cb)
      override
    {
        cb(LinkMutationStatus::Error);
    }
    void deleteLink(const std::string &, int32_t, LinkMutationCallback &&cb) override
    {
        cb(LinkMutationStatus::Error);
    }
    void userHasUsablePassword(int32_t, PasswordUsableCallback &&cb) override
    {
        cb(std::nullopt);
    }

    int32_t lastUserId = 100;
};

// Immediate transport fake: scripted responses keyed by URL substring.
struct FakeTransport
{
    struct Reply
    {
        bool transportOk = true;
        int status = 200;
        std::string body;
    };
    std::map<std::string, Reply> replies;               // url substring -> reply
    std::vector<std::string> getUrls;                   // observed GET urls
    std::vector<std::pair<std::string, std::string>> posts;  // (url, form body)

    OidcUpstreamAuthService::HttpGet get()
    {
        return [this](const std::string &url, const std::string &bearer,
                      OidcUpstreamAuthService::FetchCallback &&cb) {
            getUrls.push_back(url + (bearer.empty() ? "" : " [bearer]"));
            for (const auto &[key, reply] : replies)
            {
                if (url.find(key) != std::string::npos)
                {
                    cb(reply.transportOk, reply.status, reply.body);
                    return;
                }
            }
            cb(false, 0, "");
        };
    }

    OidcUpstreamAuthService::HttpPostForm post()
    {
        return [this](const std::string &url,
                      const std::vector<std::pair<std::string, std::string>> &form,
                      OidcUpstreamAuthService::FetchCallback &&cb) {
            std::string joined;
            for (const auto &[key, value] : form)
            {
                if (!joined.empty())
                    joined += '&';
                joined += key + "=" + value;
            }
            posts.emplace_back(url, joined);
            for (const auto &[key, reply] : replies)
            {
                if (url.find(key) != std::string::npos)
                {
                    cb(reply.transportOk, reply.status, reply.body);
                    return;
                }
            }
            cb(false, 0, "");
        };
    }
};

UpstreamProviderRecord makeRecord()
{
    UpstreamProviderRecord record;
    record.providerKey = "entra-test";
    record.displayName = "Entra Test";
    record.issuer = "https://login.example.com";
    record.clientId = "cid";
    record.clientSecret = "csecret";
    record.scopes = "openid profile email";
    record.enabled = true;
    return record;
}

OidcUpstreamAuthService::OidcEndpoints pinnedEndpoints()
{
    return {"https://login.example.com/authorize",
            "https://login.example.com/token",
            "https://login.example.com/userinfo"};
}

std::function<void(OidcUpstreamAuthService::LoginResult)> captureResult(
  OidcUpstreamAuthService::LoginResult &out,
  bool &done)
{
    return [&out, &done](OidcUpstreamAuthService::LoginResult result) {
        out = std::move(result);
        done = true;
    };
}

}  // namespace

TEST(OidcUpstreamAuthService, AuthorizeUrlCarriesStandardParamsAndEscapes)
{
    auto record = makeRecord();
    const auto url = OidcUpstreamAuthService::buildAuthorizeUrl(
      record, pinnedEndpoints(), "http://localhost:5173/callback/entra-test", "abc123");
    EXPECT_EQ(url,
              "https://login.example.com/authorize?response_type=code"
              "&client_id=cid"
              "&redirect_uri=http%3A%2F%2Flocalhost%3A5173%2Fcallback%2Fentra-test"
              "&scope=openid%20profile%20email"
              "&state=abc123");
}

TEST(OidcUpstreamAuthService, EndpointOverridesShortCircuitDiscovery)
{
    FakeTransport transport;  // no discovery reply scripted -> any fetch fails
    auto repo = std::make_shared<ImmediateAccountRepository>();
    auto service = std::make_shared<OidcUpstreamAuthService>(repo, true, transport.get(), transport.post());
    auto record = makeRecord();
    record.endpointOverrides =
      R"({"authorize":"https://a/authorize","token":"https://a/token","userinfo":"https://a/userinfo"})";

    bool done = false;
    service->resolveEndpoints(record, [&done](bool ok, auto endpoints) {
        EXPECT_TRUE(ok);
        EXPECT_EQ(endpoints.authorize, "https://a/authorize");
        EXPECT_EQ(endpoints.token, "https://a/token");
        EXPECT_EQ(endpoints.userinfo, "https://a/userinfo");
        done = true;
    });
    EXPECT_TRUE(done);
    EXPECT_TRUE(transport.getUrls.empty());  // no discovery fetch happened
}

TEST(OidcUpstreamAuthService, DiscoveryParsesAndCachesPerIssuer)
{
    FakeTransport transport;
    transport.replies["/.well-known/openid-configuration"] = {
      true, 200,
      R"({"issuer":"https://login.example.com",)"
      R"("authorization_endpoint":"https://login.example.com/a",)"
      R"("token_endpoint":"https://login.example.com/t",)"
      R"("userinfo_endpoint":"https://login.example.com/u"})"};

    auto repo = std::make_shared<ImmediateAccountRepository>();
    auto service = std::make_shared<OidcUpstreamAuthService>(repo, true, transport.get(), transport.post());
    auto record = makeRecord();

    int calls = 0;
    service->resolveEndpoints(record, [&](bool ok, auto endpoints) {
        ++calls;
        EXPECT_TRUE(ok);
        EXPECT_EQ(endpoints.authorize, "https://login.example.com/a");
    });
    service->resolveEndpoints(record, [&](bool ok, auto) {
        ++calls;
        EXPECT_TRUE(ok);
    });
    EXPECT_EQ(calls, 2);
    // Discovery fetched exactly once (second call hit the cache).
    EXPECT_EQ(transport.getUrls.size(), 1u);
}

TEST(OidcUpstreamAuthService, DiscoveryIssuerMismatchFails)
{
    FakeTransport transport;
    transport.replies["/.well-known/openid-configuration"] = {
      true, 200,
      R"({"issuer":"https://other.example.com",)"
      R"("authorization_endpoint":"https://other/a",)"
      R"("token_endpoint":"https://other/t",)"
      R"("userinfo_endpoint":"https://other/u"})"};
    auto repo = std::make_shared<ImmediateAccountRepository>();
    auto service = std::make_shared<OidcUpstreamAuthService>(repo, true, transport.get(), transport.post());

    bool done = false;
    service->resolveEndpoints(makeRecord(), [&done](bool ok, auto) {
        EXPECT_FALSE(ok);
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST(OidcUpstreamAuthService, LoginHappyPathCreatesLinkedAccount)
{
    FakeTransport transport;
    transport.replies["/token"] = {true, 200, R"({"access_token":"at123"})"};
    transport.replies["/userinfo"] = {
      true, 200, R"({"sub":"sub-1","email":"u@example.com","preferred_username":"alice","name":"Alice"})"};

    auto repo = std::make_shared<ImmediateAccountRepository>();
    auto service = std::make_shared<OidcUpstreamAuthService>(repo, true, transport.get(), transport.post());

    OidcUpstreamAuthService::LoginResult result;
    bool done = false;
    service->login(makeRecord(), pinnedEndpoints(), "http://cb", "thecode", captureResult(result, done));
    EXPECT_TRUE(done);
    EXPECT_TRUE(result.errorCode.empty()) << result.errorCode;
    EXPECT_TRUE(result.isNewUser);
    EXPECT_EQ(result.username, "u_alice");
    EXPECT_EQ(repo->creates.load(), 1);

    // Token exchange carried the authorization-code grant with
    // client_secret_post; userinfo was fetched with the bearer token.
    ASSERT_EQ(transport.posts.size(), 1u);
    EXPECT_EQ(transport.posts[0].first, "https://login.example.com/token");
    EXPECT_NE(transport.posts[0].second.find("grant_type=authorization_code"), std::string::npos);
    EXPECT_NE(transport.posts[0].second.find("code=thecode"), std::string::npos);
    EXPECT_NE(transport.posts[0].second.find("client_secret=csecret"), std::string::npos);
    ASSERT_EQ(transport.getUrls.size(), 1u);
    EXPECT_NE(transport.getUrls[0].find("[bearer]"), std::string::npos);
}

TEST(OidcUpstreamAuthService, LoginExistingMappingDoesNotCreate)
{
    FakeTransport transport;
    transport.replies["/token"] = {true, 200, R"({"access_token":"at"})"};
    transport.replies["/userinfo"] = {
      true, 200, R"({"sub":"sub-1","preferred_username":"alice"})"};

    auto repo = std::make_shared<ImmediateAccountRepository>();
    ImmediateAccountRepository::Link link;
    link.provider = "entra-test";
    link.subject = "sub-1";
    link.userId = 7;
    link.username = "existing";
    link.publicSub = "pub7";
    repo->links.push_back(link);

    auto service = std::make_shared<OidcUpstreamAuthService>(repo, true, transport.get(), transport.post());
    OidcUpstreamAuthService::LoginResult result;
    bool done = false;
    service->login(makeRecord(), pinnedEndpoints(), "http://cb", "c", captureResult(result, done));
    EXPECT_TRUE(done);
    EXPECT_TRUE(result.errorCode.empty()) << result.errorCode;
    EXPECT_FALSE(result.isNewUser);
    EXPECT_EQ(result.userId, 7);
    EXPECT_EQ(result.publicSub, "pub7");
    EXPECT_EQ(repo->creates.load(), 0);
}

TEST(OidcUpstreamAuthService, CustomClaimsMappingAndSubjectFallback)
{
    FakeTransport transport;
    transport.replies["/token"] = {true, 200, R"({"access_token":"at"})"};
    transport.replies["/userinfo"] = {
      true, 200, R"({"uid":"u-9","mail":"x@example.com","login":"bob"})"};

    auto repo = std::make_shared<ImmediateAccountRepository>();
    auto service = std::make_shared<OidcUpstreamAuthService>(repo, true, transport.get(), transport.post());
    auto record = makeRecord();
    record.claimsMapping = R"({"sub":"uid","email":"mail","username":"login"})";

    OidcUpstreamAuthService::LoginResult result;
    bool done = false;
    service->login(record, pinnedEndpoints(), "http://cb", "c", captureResult(result, done));
    EXPECT_TRUE(done);
    EXPECT_TRUE(result.errorCode.empty()) << result.errorCode;
    EXPECT_EQ(result.username, "u_bob");

    // Missing mapped `sub` is the one hard failure.
    transport.replies["/userinfo"] = {true, 200, R"({"mail":"x@example.com"})"};
    done = false;
    service->login(record, pinnedEndpoints(), "http://cb", "c", captureResult(result, done));
    EXPECT_TRUE(done);
    EXPECT_EQ(result.errorCode, "VALIDATION_INVALID_INPUT");
}

TEST(OidcUpstreamAuthService, NoMappingWithoutAutoCreateRefuses)
{
    FakeTransport transport;
    transport.replies["/token"] = {true, 200, R"({"access_token":"at"})"};
    transport.replies["/userinfo"] = {true, 200, R"({"sub":"sub-2","preferred_username":"n"})"};

    auto repo = std::make_shared<ImmediateAccountRepository>();
    auto service = std::make_shared<OidcUpstreamAuthService>(repo, false, transport.get(), transport.post());
    OidcUpstreamAuthService::LoginResult result;
    bool done = false;
    service->login(makeRecord(), pinnedEndpoints(), "http://cb", "c", captureResult(result, done));
    EXPECT_TRUE(done);
    EXPECT_EQ(result.errorCode, "AUTH_SOCIAL_ACCOUNT_NOT_LINKED");
    EXPECT_EQ(repo->creates.load(), 0);
}

TEST(OidcUpstreamAuthService, UpstreamFailuresMapToNetConnectionFailed)
{
    FakeTransport transport;
    transport.replies["/token"] = {true, 400, R"({"error":"invalid_grant"})"};

    auto repo = std::make_shared<ImmediateAccountRepository>();
    auto service = std::make_shared<OidcUpstreamAuthService>(repo, true, transport.get(), transport.post());
    OidcUpstreamAuthService::LoginResult result;
    bool done = false;
    service->login(makeRecord(), pinnedEndpoints(), "http://cb", "c", captureResult(result, done));
    EXPECT_TRUE(done);
    EXPECT_EQ(result.errorCode, "NET_CONNECTION_FAILED");
}

#endif  // WITH_SOCIAL
