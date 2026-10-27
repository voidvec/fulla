// B1 (OIDC Back-Channel Logout 1.0): unit tests for
// BackchannelLogoutNotifier::dispatch (AC D1-D6). These exercise the fan-out
// logic (sign + POST + audit) WITHOUT a database -- dispatch takes a
// pre-resolved RP target list. notify()'s DB lookup path is covered by the
// end-to-end integration test (tests/integration/controllers/).
//
// The notifier is constructed with a real (ephemeral-key) JwkManager so the
// signed logout_token is a genuine RS256 JWT, a FakeOAuthHttpClient
// (synchronous, scripted queue + call recording) and a FakeAuditSink. gtest
// (not DROGON_TEST) so the binary runs with no event loop.

#include <fulla/drogon/adapters/BackchannelLogoutNotifier.h>
#include <fulla/common/testing/FakeAuditSink.h>
#include <fulla/identity/IOAuthHttpClient.h>
#include <fulla/identity/testing/FakeOAuthHttpClient.h>
#include <fulla/oauth2/jwk/JwkManager.h>
#include <fulla/oauth2/protocol/LogoutToken.h>

#include <gtest/gtest.h>

#include <json/json.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace
{
using fulla::common::testing::FakeAuditSink;
using fulla::drogon::adapters::BackchannelLogoutNotifier;
using fulla::drogon::adapters::BackchannelRpTarget;
using fulla::identity::testing::FakeOAuthHttpClient;
using fulla::identity::testing::okJson;
using fulla::identity::testing::transportFailure;
using fulla::oauth2::JwkManager;
using fulla::oauth2::protocol::kBackchannelLogoutEventUrn;

// ---- small JWT helpers -----------------------------------------------------

std::string base64urlDecode(const std::string &s)
{
    int8_t table[256];
    for (int i = 0; i < 256; ++i)
        table[i] = -1;
    const char *alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (int i = 0; i < 64; ++i)
        table[static_cast<unsigned char>(alpha[i])] = static_cast<int8_t>(i);

    int val = 0, bits = 0;
    std::string out;
    for (char c : s)
    {
        const auto v = table[static_cast<unsigned char>(c)];
        if (v < 0)
            continue;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<char>((val >> bits) & 0xff));
        }
    }
    return out;
}

std::vector<std::string> splitJwt(const std::string &jwt)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true)
    {
        auto dot = jwt.find('.', start);
        if (dot == std::string::npos)
        {
            parts.push_back(jwt.substr(start));
            break;
        }
        parts.push_back(jwt.substr(start, dot - start));
        start = dot + 1;
    }
    return parts;
}

Json::Value decodeJwtPart(const std::string &part)
{
    Json::Value root;
    const auto bytes = base64urlDecode(part);
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    JSONCPP_STRING errs;
    reader->parse(bytes.data(), bytes.data() + bytes.size(), &root, &errs);
    return root;
}

std::string paramValue(
  const std::vector<std::pair<std::string, std::string>> &params,
  const std::string &key)
{
    for (const auto &p : params)
        if (p.first == key)
            return p.second;
    return "";
}

// ---- shared fixture (ephemeral key) ---------------------------------------

class BackchannelLogoutNotifierTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        jwk_ = std::make_shared<JwkManager>();
        ASSERT_TRUE(jwk_->init(Json::Value(Json::objectValue)));
        http_ = std::make_shared<FakeOAuthHttpClient>();
        audit_ = std::make_shared<FakeAuditSink>();
        notifier_ = std::make_shared<BackchannelLogoutNotifier>(
          /*dbClient=*/nullptr, jwk_, "https://op.example.com", http_, audit_);
        completions_ = 0;
    }

    void runDispatch(const std::string &subject, std::vector<BackchannelRpTarget> targets)
    {
        notifier_->dispatch(subject, std::move(targets), [this] { ++completions_; });
    }

    std::shared_ptr<JwkManager> jwk_;
    std::shared_ptr<FakeOAuthHttpClient> http_;
    std::shared_ptr<FakeAuditSink> audit_;
    std::shared_ptr<BackchannelLogoutNotifier> notifier_;
    int completions_ = 0;
};

// D1: each of N clients receives exactly one POST, each logout_token carries
// the matching aud (clientId) and the shared sub (subject); all audited success.
TEST_F(BackchannelLogoutNotifierTest, FansOutPerClientWithMatchingAudAndSub)
{
    for (int i = 0; i < 3; ++i)
        http_->postFormResponses.push_back(okJson(Json::Value(Json::objectValue)));

    runDispatch("user-1",
                {{"c1", "https://rp1.example.com/backchannel"},
                 {"c2", "https://rp2.example.com/backchannel"},
                 {"c3", "https://rp3.example.com/backchannel"}});

    ASSERT_EQ(http_->postFormCalls.size(), 3u);
    EXPECT_EQ(completions_, 1);
    const std::string ids[3] = {"c1", "c2", "c3"};
    const std::string uris[3] = {
      "https://rp1.example.com/backchannel",
      "https://rp2.example.com/backchannel",
      "https://rp3.example.com/backchannel"};
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_EQ(http_->postFormCalls[i].url, uris[i]);
        const auto jwt = paramValue(http_->postFormCalls[i].params, "logout_token");
        ASSERT_FALSE(jwt.empty());
        const auto payload = decodeJwtPart(splitJwt(jwt)[1]);
        EXPECT_EQ(payload["sub"].asString(), "user-1");
        EXPECT_EQ(payload["aud"].asString(), ids[i]);
        EXPECT_EQ(payload["iss"].asString(), "https://op.example.com");
        ASSERT_TRUE(payload.isMember("events"));
        EXPECT_TRUE(payload["events"].isMember(kBackchannelLogoutEventUrn));
        EXPECT_FALSE(payload.isMember("nonce"));
    }
    EXPECT_EQ(audit_->count(), 3u);
    EXPECT_TRUE(audit_->hasEventWithAction("backchannel_logout"));
}

// D2: a client with an empty URI is skipped (no POST, no audit).
TEST_F(BackchannelLogoutNotifierTest, SkipsClientsWithEmptyUri)
{
    http_->postFormResponses.push_back(okJson(Json::Value(Json::objectValue)));

    runDispatch("user-1",
                {{"c-with-uri", "https://rp.example.com/backchannel"}, {"c-no-uri", ""}});

    ASSERT_EQ(http_->postFormCalls.size(), 1u);
    EXPECT_EQ(http_->postFormCalls[0].url, "https://rp.example.com/backchannel");
    EXPECT_EQ(completions_, 1);
    EXPECT_EQ(audit_->count(), 1u);
}

// D3: a transport failure for one RP is audited as failure and does NOT abort
// the fan-out (the other RP still receives its POST).
TEST_F(BackchannelLogoutNotifierTest, TransportFailureAuditedAndOthersStillNotified)
{
    http_->postFormResponses.push_back(transportFailure());
    http_->postFormResponses.push_back(okJson(Json::Value(Json::objectValue)));

    runDispatch("user-1",
                {{"c-fail", "https://rp1.example.com/backchannel"},
                 {"c-ok", "https://rp2.example.com/backchannel"}});

    ASSERT_EQ(http_->postFormCalls.size(), 2u);
    EXPECT_EQ(completions_, 1);
    ASSERT_EQ(audit_->count(), 2u);
    EXPECT_EQ(audit_->events()[0].outcome, "failure");
    EXPECT_EQ(audit_->events()[1].outcome, "success");
}

// D4: HTTP 200 -> success; non-200 (4xx/5xx) -> failure.
TEST_F(BackchannelLogoutNotifierTest, Non200IsFailureAnd200IsSuccess)
{
    http_->postFormResponses.push_back(okJson(Json::Value(Json::objectValue), 200));
    http_->postFormResponses.push_back(okJson(Json::Value(Json::objectValue), 503));

    runDispatch("user-1",
                {{"c-ok", "https://rp1.example.com/backchannel"},
                 {"c-err", "https://rp2.example.com/backchannel"}});

    ASSERT_EQ(audit_->count(), 2u);
    EXPECT_EQ(audit_->events()[0].outcome, "success");
    EXPECT_EQ(audit_->events()[1].outcome, "failure");
}

// D6: with no targets, dispatch completes (once) and posts nothing; with
// targets, completion fires only after the (synchronous fake) fan-out recorded
// every call -- i.e. completion strictly follows dispatch.
TEST_F(BackchannelLogoutNotifierTest, CompletionInvokedAfterDispatchAndSkipsEmpty)
{
    runDispatch("user-1", {});
    EXPECT_EQ(completions_, 1);
    EXPECT_TRUE(http_->postFormCalls.empty());
}

// D5: the logout_token is a genuine RS256 JWT whose signature verifies against
// the JwkManager's signing key, and whose header kid matches getKeyId().
TEST(BackchannelLogoutNotifierSignatureTest, LogoutTokenVerifiesWithSigningKey)
{
    // Generate a known RSA keypair, feed its PEM to JwkManager via the
    // FULLA_SIGNING_KEY env var (the production key-loading branch), and load
    // the same PEM in the test to verify signatures.
    auto generatePem = []() -> std::string {
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        EVP_PKEY_keygen_init(ctx);
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
        EVP_PKEY *pkey = nullptr;
        EVP_PKEY_keygen(ctx, &pkey);
        EVP_PKEY_CTX_free(ctx);
        BIO *bio = BIO_new(BIO_s_mem());
        PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        char *data = nullptr;
        long len = BIO_get_mem_data(bio, &data);
        std::string pem(data, static_cast<size_t>(len));
        BIO_free(bio);
        EVP_PKEY_free(pkey);
        return pem;
    };

    struct EnvGuard
    {
        std::string name, old;
        bool had = false;
        explicit EnvGuard(const std::string &n, const std::string &v) : name(n)
        {
            if (const char *cur = std::getenv(n.c_str()))
            {
                had = true;
                old = cur;
            }
#ifdef _WIN32
            _putenv_s(n.c_str(), v.c_str());
#else
            setenv(n.c_str(), v.c_str(), 1);
#endif
        }
        ~EnvGuard()
        {
#ifdef _WIN32
            _putenv_s(name.c_str(), had ? old.c_str() : "");
#else
            if (had)
                setenv(name.c_str(), old.c_str(), 1);
            else
                unsetenv(name.c_str());
#endif
        }
    };

    const std::string pem = generatePem();
    EnvGuard guard("FULLA_SIGNING_KEY", pem);

    auto jwk = std::make_shared<JwkManager>();
    ASSERT_TRUE(jwk->init(Json::Value(Json::objectValue)));
    ASSERT_FALSE(jwk->getKeyId().empty());

    // Load the signing (private) key; EVP_DigestVerify* uses its public half.
    BIO *kbio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(kbio, nullptr, nullptr, nullptr);
    BIO_free(kbio);
    ASSERT_NE(pkey, nullptr);

    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->postFormResponses.push_back(okJson(Json::Value(Json::objectValue)));
    auto notifier = std::make_shared<BackchannelLogoutNotifier>(
      nullptr, jwk, "https://op.example.com", http);
    notifier->dispatch("subj-9", {{"client-z", "https://rp.example.com/backchannel"}}, [] {});

    ASSERT_EQ(http->postFormCalls.size(), 1u);
    const auto jwt = paramValue(http->postFormCalls[0].params, "logout_token");
    ASSERT_FALSE(jwt.empty());

    const auto parts = splitJwt(jwt);
    ASSERT_EQ(parts.size(), 3u);
    const auto header = decodeJwtPart(parts[0]);
    EXPECT_EQ(header["alg"].asString(), "RS256");
    EXPECT_EQ(header["kid"].asString(), jwk->getKeyId());

    // Verify RS256 over headerB64.payloadB64 with the signing key.
    const std::string signingInput = parts[0] + "." + parts[1];
    const auto sig = base64urlDecode(parts[2]);
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    ASSERT_NE(mdctx, nullptr);
    int ok = EVP_DigestVerifyInit(mdctx, nullptr, EVP_sha256(), nullptr, pkey);
    if (ok == 1)
        ok = EVP_DigestVerifyUpdate(mdctx, signingInput.data(), signingInput.size());
    const int verified =
      (ok == 1)
        ? EVP_DigestVerifyFinal(mdctx, reinterpret_cast<const unsigned char *>(sig.data()), sig.size())
        : 0;
    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);

    EXPECT_EQ(verified, 1);
}

}  // namespace

// Own main(), compiled as an object (not GTest::Main's .lib). Historically
// this dodged an MSVC link trap: the transitive Boost umbrella linked
// Boost::test_exec_monitor (a main() that calls test_main), which MSVC could
// resolve before GTest::Main. Since drogon/*:with_boost=False removed Boost
// from the graph the trap is gone; the pattern is kept. See the matching
// note in this target's CMakeLists.txt.
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
