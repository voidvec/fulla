// M2b Task 17 slice 10 (fulla-sdk-refactor): basic unit tests for the
// relocated fulla::oauth2::JwkManager. Full concurrency/preservation
// coverage remains in tests/ (Property4_JwkBaselineTest.cc/
// CategoryB_JwkManagerRaceTest.cc) -- these are just Domain-layer smoke
// tests confirming the class works standalone (no Drogon, no injected
// logger required).
//
// Coverage additions (P1): the original tests only exercised the ephemeral
// fallback path. The additions below cover the production key-loading
// branches (FULLA_SIGNING_KEY / FULLA_JWT_KEY_PATH / config
// signing_key_path), the kid override, the logger forwarding path, and
// the JWKS use/e fields.

#include <fulla/common/testing/FakeLogger.h>
#include <fulla/oauth2/jwk/JwkManager.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <string>

namespace
{

using fulla::common::testing::FakeLogger;
using fulla::oauth2::JwkManager;

// RAII guard: save an env var, set it for the test body, restore it on
// destruction. Keeps env-var-mutating tests isolated from one another and
// from the host environment.
class EnvVarGuard
{
  public:
    explicit EnvVarGuard(const std::string &name, std::string value)
        : name_(name), hadValue_(std::getenv(name.c_str()) != nullptr),
          oldValue_(hadValue_ ? std::getenv(name.c_str()) : std::string{})
    {
        set(std::move(value));
    }

    ~EnvVarGuard()
    {
        if (hadValue_)
            set(oldValue_);
        else
            unset();
    }

  private:
    void set(const std::string &v)
    {
#ifdef _WIN32
        _putenv_s(name_.c_str(), v.c_str());
#else
        setenv(name_.c_str(), v.c_str(), 1);
#endif
    }
    void unset()
    {
#ifdef _WIN32
        _putenv_s(name_.c_str(), "");
#else
        unsetenv(name_.c_str());
#endif
    }

    std::string name_;
    bool hadValue_;
    std::string oldValue_;
};

// Generate a fresh RSA private key (PEM) for this test run. Used to feed
// a known-valid PEM into the env-var / file-path branches.
std::string generateTestPem()
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EXPECT_NE(ctx, nullptr);
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
    EVP_PKEY *pkey = nullptr;
    EVP_PKEY_keygen(ctx, &pkey);
    EVP_PKEY_CTX_free(ctx);
    EXPECT_NE(pkey, nullptr);

    BIO *bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    EVP_PKEY_free(pkey);

    char *data;
    long len = BIO_get_mem_data(bio, &data);
    std::string pem(data, static_cast<size_t>(len));
    BIO_free(bio);
    return pem;
}

// Write a PEM to a temp file and return its path.
std::string writeTempPem(const std::string &pem, const std::string &suffix)
{
    std::string path = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : "/tmp") +
                       "/jwktest_" + suffix + ".pem";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << pem;
    f.close();
    return path;
}

TEST(JwkManagerTest, InitWithoutLogger_GeneratesEphemeralKey)
{
    JwkManager jwk;  // no logger injected -- log() must be a safe no-op
    EXPECT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    EXPECT_TRUE(jwk.isInitialized());
    EXPECT_FALSE(jwk.getKeyId().empty());
}

TEST(JwkManagerTest, SignJwt_BeforeInit_ReturnsEmptyString)
{
    JwkManager jwk;
    EXPECT_EQ(jwk.signJwt(Json::Value(Json::objectValue)), "");
}

TEST(JwkManagerTest, SignJwt_AfterInit_ProducesThreePartToken)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));

    Json::Value claims;
    claims["sub"] = "alice";
    std::string jwt = jwk.signJwt(claims);

    ASSERT_FALSE(jwt.empty());
    EXPECT_EQ(std::count(jwt.begin(), jwt.end(), '.'), 2);
}

TEST(JwkManagerTest, GetJwks_BeforeInit_ReturnsEmptyKeysArray)
{
    JwkManager jwk;
    Json::Value jwks = jwk.getJwks();
    ASSERT_TRUE(jwks.isMember("keys"));
    EXPECT_EQ(jwks["keys"].size(), 0u);
}

TEST(JwkManagerTest, GetJwks_AfterInit_ContainsOneRsaKey)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));

    Json::Value jwks = jwk.getJwks();
    ASSERT_EQ(jwks["keys"].size(), 1u);
    EXPECT_EQ(jwks["keys"][0]["kty"].asString(), "RSA");
    EXPECT_EQ(jwks["keys"][0]["alg"].asString(), "RS256");
    EXPECT_EQ(jwks["keys"][0]["kid"].asString(), jwk.getKeyId());
    EXPECT_FALSE(jwks["keys"][0]["n"].asString().empty());
}

TEST(JwkManagerTest, InitCalledTwice_SecondCallIsNoOpAndReturnsTrue)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    const std::string firstKid = jwk.getKeyId();

    EXPECT_TRUE(jwk.init(Json::Value(Json::objectValue)));  // no-op, not a failure
    EXPECT_EQ(jwk.getKeyId(), firstKid);                    // key unchanged
}

// ---------------------------------------------------------------------------
// Coverage additions (P1): production key-loading branches + logger + kid.
// ---------------------------------------------------------------------------

// init: FULLA_SIGNING_KEY env with a valid PEM -> loads from env, kid
// defaults to "key-1" (JwkManager.cc:48).
TEST(JwkManagerTest, Init_FromOauth2SigningKeyEnv_LoadsPem)
{
    const std::string pem = generateTestPem();
    EnvVarGuard guard("FULLA_SIGNING_KEY", pem);
    JwkManager jwk;
    Json::Value config(Json::objectValue);
    EXPECT_TRUE(jwk.init(config));
    EXPECT_TRUE(jwk.isInitialized());
    EXPECT_EQ(jwk.getKeyId(), "key-1");  // default kid for the PEM path
    // A JWT signed with the loaded key must be a 3-part token.
    EXPECT_FALSE(jwk.signJwt(Json::Value(Json::objectValue)).empty());
}

// init: FULLA_SIGNING_KEY env with an invalid PEM -> falls through to the
// ephemeral fallback (JwkManager.cc:46 returns false, falls through).
TEST(JwkManagerTest, Init_FromOauth2SigningKeyEnv_InvalidPem_FallsThroughToEphemeral)
{
    EnvVarGuard guard("FULLA_SIGNING_KEY", "not-a-valid-pem");
    FakeLogger logger;
    JwkManager jwk(&logger);
    EXPECT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    EXPECT_TRUE(jwk.isInitialized());
    EXPECT_EQ(jwk.getKeyId(), "ephemeral-dev-key");
    // The invalid-PEM parse failure was logged.
    EXPECT_TRUE(logger.hasMessageContaining("Failed to parse PEM"));
}

// init: FULLA_JWT_KEY_PATH env pointing at a readable file with a valid
// PEM -> loads from the file (JwkManager.cc:58-76).
TEST(JwkManagerTest, Init_FromOauth2JwtKeyPathEnv_ReadsFileAndLoads)
{
    const std::string pem = generateTestPem();
    const std::string path = writeTempPem(pem, "jwtkeypath_ok");
    EnvVarGuard guard("FULLA_JWT_KEY_PATH", path);
    JwkManager jwk;
    EXPECT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    EXPECT_TRUE(jwk.isInitialized());
    EXPECT_EQ(jwk.getKeyId(), "key-1");
}

// init: FULLA_JWT_KEY_PATH env pointing at a non-existent file -> warns
// and falls through to ephemeral (JwkManager.cc:78-82).
TEST(JwkManagerTest, Init_FromOauth2JwtKeyPathEnv_UnreadableFile_WarnsAndFallsThrough)
{
    EnvVarGuard guard("FULLA_JWT_KEY_PATH", "/nonexistent/path/key.pem");
    FakeLogger logger;
    JwkManager jwk(&logger);
    EXPECT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    EXPECT_EQ(jwk.getKeyId(), "ephemeral-dev-key");
    EXPECT_TRUE(logger.hasMessageContaining("Failed to load key from FULLA_JWT_KEY_PATH"));
}

// init: config["signing_key_path"] with a readable valid-PEM file -> loads
// (JwkManager.cc:84-101).
TEST(JwkManagerTest, Init_FromConfigSigningKeyPath_LoadsPem)
{
    const std::string pem = generateTestPem();
    const std::string path = writeTempPem(pem, "configpath_ok");
    Json::Value config(Json::objectValue);
    config["signing_key_path"] = path;
    JwkManager jwk;
    EXPECT_TRUE(jwk.init(config));
    EXPECT_TRUE(jwk.isInitialized());
    EXPECT_EQ(jwk.getKeyId(), "key-1");
}

// init: config["signing_key_path"] pointing at an unreadable file -> warns
// and falls through (JwkManager.cc:103-105).
TEST(JwkManagerTest, Init_FromConfigSigningKeyPath_InvalidPem_WarnsAndFallsThrough)
{
    const std::string path = writeTempPem("garbage-not-a-pem", "configpath_bad");
    Json::Value config(Json::objectValue);
    config["signing_key_path"] = path;
    FakeLogger logger;
    JwkManager jwk(&logger);
    EXPECT_TRUE(jwk.init(config));
    EXPECT_EQ(jwk.getKeyId(), "ephemeral-dev-key");
    EXPECT_TRUE(logger.hasMessageContaining("Failed to parse PEM"));
}

// init: config["kid"] override is reflected in getKeyId and the JWKS.
TEST(JwkManagerTest, Init_KidFromConfig_IsReflectedInKeyIdAndJwks)
{
    const std::string pem = generateTestPem();
    EnvVarGuard guard("FULLA_SIGNING_KEY", pem);
    Json::Value config(Json::objectValue);
    config["kid"] = "my-custom-kid";
    JwkManager jwk;
    EXPECT_TRUE(jwk.init(config));
    EXPECT_EQ(jwk.getKeyId(), "my-custom-kid");
    Json::Value jwks = jwk.getJwks();
    ASSERT_EQ(jwks["keys"].size(), 1u);
    EXPECT_EQ(jwks["keys"][0]["kid"].asString(), "my-custom-kid");
}

// init: ephemeral fallback sets kid to "ephemeral-dev-key"
// (JwkManager.cc:114).
TEST(JwkManagerTest, Init_EphemeralKey_KidIsEphemeralDevKey)
{
    JwkManager jwk;
    EXPECT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    EXPECT_EQ(jwk.getKeyId(), "ephemeral-dev-key");
}

// init: a second init() with a logger injected forwards the
// init-once-violation warning through ILogger (the logger-truthy branch of
// log(), JwkManager.cc:16-19).
TEST(JwkManagerTest, Init_WithLogger_ForwardsLogLines)
{
    FakeLogger logger;
    JwkManager jwk(&logger);
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    size_t afterFirst = logger.entries().size();
    EXPECT_TRUE(jwk.init(Json::Value(Json::objectValue)));  // no-op
    // The init-once-violation warning was forwarded.
    bool sawInitOnce = false;
    for (size_t i = afterFirst; i < logger.entries().size(); ++i)
    {
        if (logger.entries()[i].message.find("init() called more than once") != std::string::npos)
        {
            sawInitOnce = true;
            break;
        }
    }
    EXPECT_TRUE(sawInitOnce);
}

// getJwks: after init, the JWKS key carries use == "sig" and a non-empty
// "e" exponent (the original test only checked kty/alg/kid/n).
TEST(JwkManagerTest, GetJwks_AfterInit_AssertsUseAndEFields)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    Json::Value jwks = jwk.getJwks();
    ASSERT_EQ(jwks["keys"].size(), 1u);
    EXPECT_EQ(jwks["keys"][0]["use"].asString(), "sig");
    EXPECT_FALSE(jwks["keys"][0]["e"].asString().empty());
}

// ---------------------------------------------------------------------------
// #78: verifyJwt() matrix -- signature + claim-policy verification for
// /oauth2/end_session's id_token_hint gate. One test per rejection reason,
// plus the happy-path roundtrip and the exp boundary.
// ---------------------------------------------------------------------------

namespace
{
const char *kTestIssuer = "https://auth.example.test";

// Explicit-base overload (declared first so the 1-arg form can delegate): the exact-boundary expiry test must pin exp to a
// NOW captured by the test itself -- a fresh std::time(nullptr) here races
// the wall clock across a second boundary and flips the result to Ok
// (1-second flake caught by PR #176's CI on a loaded Windows runner).
Json::Value validClaims(long long expOffsetSecs, long long baseSecs)
{
    Json::Value claims;
    claims["iss"] = kTestIssuer;
    claims["sub"] = "user-123";
    claims["exp"] = static_cast<Json::Int64>(baseSecs + expOffsetSecs);
    return claims;
}

Json::Value validClaims(long long expOffsetSecs)
{
    return validClaims(expOffsetSecs, std::time(nullptr));
}

// Minimal base64url encoder (test-side only) so adversarial headers (alg=none,
// HS256) can be hand-crafted without going through signJwt.
std::string b64Url(const std::string &raw)
{
    static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    unsigned int buffer = 0;
    int bits = 0;
    for (unsigned char c : raw)
    {
        buffer = (buffer << 8) | c;
        bits += 8;
        while (bits >= 6)
        {
            bits -= 6;
            out += alphabet[(buffer >> bits) & 0x3F];
        }
    }
    if (bits > 0)
        out += alphabet[(buffer << (6 - bits)) & 0x3F];
    return out;
}
}  // namespace

TEST(JwkManagerTest, VerifyJwt_SignedBySameManager_ReturnsOk)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    const std::string jwt = jwk.signJwt(validClaims(600));
    ASSERT_FALSE(jwt.empty());
    EXPECT_EQ(
      jwk.verifyJwt(jwt, kTestIssuer, std::time(nullptr)),
      JwkManager::JwtVerificationResult::Ok
    );
}

TEST(JwkManagerTest, VerifyJwt_TamperedPayload_IsBadSignature)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    std::string jwt = jwk.signJwt(validClaims(600));
    // Flip one payload-segment character (base64url alphabet-safe swap).
    const size_t payloadStart = jwt.find('.') + 1;
    jwt[payloadStart] = (jwt[payloadStart] == 'A' ? 'B' : 'A');
    EXPECT_EQ(
      jwk.verifyJwt(jwt, kTestIssuer, std::time(nullptr)),
      JwkManager::JwtVerificationResult::BadSignature
    );
}

TEST(JwkManagerTest, VerifyJwt_TamperedSignature_IsBadSignature)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    std::string jwt = jwk.signJwt(validClaims(600));
    const size_t sigStart = jwt.rfind('.') + 1;
    jwt[sigStart] = (jwt[sigStart] == 'A' ? 'B' : 'A');
    EXPECT_EQ(
      jwk.verifyJwt(jwt, kTestIssuer, std::time(nullptr)),
      JwkManager::JwtVerificationResult::BadSignature
    );
}

TEST(JwkManagerTest, VerifyJwt_AlgNone_IsBadAlg)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    // alg=none with a non-empty (decodable) signature segment: must be
    // rejected by the alg policy, never reach the signature check.
    const std::string header = b64Url(R"({"alg":"none","typ":"JWT"})");
    const std::string payload = b64Url(R"({"iss":")" + std::string(kTestIssuer) +
                                      R"(","sub":"u","exp":9999999999})");
    EXPECT_EQ(
      jwk.verifyJwt(header + "." + payload + ".AAAA", kTestIssuer, std::time(nullptr)),
      JwkManager::JwtVerificationResult::BadAlg
    );
}

TEST(JwkManagerTest, VerifyJwt_AlgHs256_IsBadAlg)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    const std::string header = b64Url(R"({"alg":"HS256","typ":"JWT"})");
    const std::string payload = b64Url(R"({"iss":")" + std::string(kTestIssuer) +
                                      R"(","sub":"u","exp":9999999999})");
    EXPECT_EQ(
      jwk.verifyJwt(header + "." + payload + ".AAAA", kTestIssuer, std::time(nullptr)),
      JwkManager::JwtVerificationResult::BadAlg
    );
}

TEST(JwkManagerTest, VerifyJwt_SignedByOtherKey_IsKidMismatch)
{
    JwkManager verifier;
    ASSERT_TRUE(verifier.init(Json::Value(Json::objectValue)));
    // The ephemeral path hardcodes kid "ephemeral-dev-key" (and would collide
    // with the verifier's), so load the forger's key from a PEM with an
    // explicit distinct kid via the env-var branch.
    const std::string pem = generateTestPem();
    EnvVarGuard guard("FULLA_SIGNING_KEY", pem);
    JwkManager forger;
    Json::Value forgerConfig(Json::objectValue);
    forgerConfig["kid"] = "forger-kid";
    ASSERT_TRUE(forger.init(forgerConfig));
    ASSERT_EQ(forger.getKeyId(), "forger-kid");
    // The forged header carries a kid the verifier never published -> the
    // kid check fires before the signature check (an attacker's correctly
    // signed token from an unknown key is still rejected).
    const std::string jwt = forger.signJwt(validClaims(600));
    EXPECT_EQ(
      verifier.verifyJwt(jwt, kTestIssuer, std::time(nullptr)),
      JwkManager::JwtVerificationResult::KidMismatch
    );
}

TEST(JwkManagerTest, VerifyJwt_WrongIssuer_IsIssuerMismatch)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    const std::string jwt = jwk.signJwt(validClaims(600));
    EXPECT_EQ(
      jwk.verifyJwt(jwt, "https://someone-else.test", std::time(nullptr)),
      JwkManager::JwtVerificationResult::IssuerMismatch
    );
}

TEST(JwkManagerTest, VerifyJwt_Expired_IsExpired_IncludingExactBoundary)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    const long long now = std::time(nullptr);
    const std::string clearlyExpired = jwk.signJwt(validClaims(-10, now));
    EXPECT_EQ(
      jwk.verifyJwt(clearlyExpired, kTestIssuer, now),
      JwkManager::JwtVerificationResult::Expired
    );
    // exp == now is already expired (strict <= rejection). Both the token's
    // exp and the verification instant pin to the SAME `now` (see the
    // validClaims overload note).
    const std::string boundary = jwk.signJwt(validClaims(0, now));
    EXPECT_EQ(
      jwk.verifyJwt(boundary, kTestIssuer, now),
      JwkManager::JwtVerificationResult::Expired
    );
}

TEST(JwkManagerTest, VerifyJwt_MissingSubject_IsMissingSubject)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    Json::Value claims = validClaims(600);
    claims.removeMember("sub");
    const std::string jwt = jwk.signJwt(claims);
    EXPECT_EQ(
      jwk.verifyJwt(jwt, kTestIssuer, std::time(nullptr)),
      JwkManager::JwtVerificationResult::MissingSubject
    );
}

TEST(JwkManagerTest, VerifyJwt_MissingExp_IsExpired)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    Json::Value claims = validClaims(600);
    claims.removeMember("exp");
    const std::string jwt = jwk.signJwt(claims);
    // Absent exp fails closed through the exp check ("not provably
    // unexpired"), not the structural Malformed branch.
    EXPECT_EQ(
      jwk.verifyJwt(jwt, kTestIssuer, std::time(nullptr)),
      JwkManager::JwtVerificationResult::Expired
    );
}

TEST(JwkManagerTest, VerifyJwt_StructurallyInvalidInputs_AreMalformed)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    const long long now = std::time(nullptr);
    EXPECT_EQ(jwk.verifyJwt("", kTestIssuer, now), JwkManager::JwtVerificationResult::Malformed);
    EXPECT_EQ(
      jwk.verifyJwt("garbage", kTestIssuer, now), JwkManager::JwtVerificationResult::Malformed
    );
    EXPECT_EQ(
      jwk.verifyJwt("only.two", kTestIssuer, now), JwkManager::JwtVerificationResult::Malformed
    );
    // Empty signature segment.
    const std::string jwt = jwk.signJwt(validClaims(600));
    EXPECT_EQ(
      jwk.verifyJwt(jwt.substr(0, jwt.rfind('.') + 1), kTestIssuer, now),
      JwkManager::JwtVerificationResult::Malformed
    );
}

TEST(JwkManagerTest, VerifyJwt_NotInitialized_FailsClosed)
{
    JwkManager jwk;  // never init()'d
    JwkManager signer;
    ASSERT_TRUE(signer.init(Json::Value(Json::objectValue)));
    const std::string jwt = signer.signJwt(validClaims(600));
    EXPECT_EQ(
      jwk.verifyJwt(jwt, kTestIssuer, std::time(nullptr)),
      JwkManager::JwtVerificationResult::NotInitialized
    );
}

// ---------------------------------------------------------------------------
// #87 M2: nbf (RFC 7519 §4.1.5) -- optional claim, but when present must be
// numeric with nbf <= nowSecs (no leeway, mirroring the exp policy).
// ---------------------------------------------------------------------------

TEST(JwkManagerTest, VerifyJwt_NbfInTheFuture_IsNotYetValid)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    const long long now = std::time(nullptr);
    Json::Value claims = validClaims(600);
    claims["nbf"] = static_cast<Json::Int64>(now + 30);
    EXPECT_EQ(
      jwk.verifyJwt(jwk.signJwt(claims), kTestIssuer, now),
      JwkManager::JwtVerificationResult::NotYetValid
    );
    // Boundary: nbf == now is already valid (strict <= acceptance).
    claims["nbf"] = static_cast<Json::Int64>(now);
    EXPECT_EQ(
      jwk.verifyJwt(jwk.signJwt(claims), kTestIssuer, now),
      JwkManager::JwtVerificationResult::Ok
    );
}

TEST(JwkManagerTest, VerifyJwt_NbfInThePast_OrAbsent_IsOk)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    const long long now = std::time(nullptr);
    Json::Value claims = validClaims(600);
    claims["nbf"] = static_cast<Json::Int64>(now - 10);
    EXPECT_EQ(
      jwk.verifyJwt(jwk.signJwt(claims), kTestIssuer, now),
      JwkManager::JwtVerificationResult::Ok
    );
    Json::Value noNbf = validClaims(600);
    EXPECT_EQ(
      jwk.verifyJwt(jwk.signJwt(noNbf), kTestIssuer, now),
      JwkManager::JwtVerificationResult::Ok
    );
}

TEST(JwkManagerTest, VerifyJwt_NonNumericNbf_IsMalformed)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    Json::Value claims = validClaims(600);
    claims["nbf"] = "soon-ish";  // unevaluable -> fail closed
    EXPECT_EQ(
      jwk.verifyJwt(jwk.signJwt(claims), kTestIssuer, std::time(nullptr)),
      JwkManager::JwtVerificationResult::Malformed
    );
}

// ---------------------------------------------------------------------------
// #87 M1: optional expectedAudience pinning. aud as a string or as an array
// of strings (RFC 7519 §4.1.3); absent aud with a pin -> AudienceMismatch.
// ---------------------------------------------------------------------------

TEST(JwkManagerTest, VerifyJwt_ExpectedAudience_StringAndArrayForms)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    const long long now = std::time(nullptr);

    Json::Value stringAud = validClaims(600);
    stringAud["aud"] = "fulla-portal";
    const std::string stringJwt = jwk.signJwt(stringAud);
    EXPECT_EQ(
      jwk.verifyJwt(stringJwt, kTestIssuer, now, "fulla-portal"),
      JwkManager::JwtVerificationResult::Ok
    );
    EXPECT_EQ(
      jwk.verifyJwt(stringJwt, kTestIssuer, now, "other-client"),
      JwkManager::JwtVerificationResult::AudienceMismatch
    );

    Json::Value arrayAud = validClaims(600);
    arrayAud["aud"] = Json::Value(Json::arrayValue);
    arrayAud["aud"].append("some-api");
    arrayAud["aud"].append("fulla-portal");
    const std::string arrayJwt = jwk.signJwt(arrayAud);
    EXPECT_EQ(
      jwk.verifyJwt(arrayJwt, kTestIssuer, now, "fulla-portal"),
      JwkManager::JwtVerificationResult::Ok
    );
    EXPECT_EQ(
      jwk.verifyJwt(arrayJwt, kTestIssuer, now, "some-api"),
      JwkManager::JwtVerificationResult::Ok
    );
    EXPECT_EQ(
      jwk.verifyJwt(arrayJwt, kTestIssuer, now, "absent-client"),
      JwkManager::JwtVerificationResult::AudienceMismatch
    );

    // Absent aud fails closed when an audience is pinned.
    const std::string noAudJwt = jwk.signJwt(validClaims(600));
    EXPECT_EQ(
      jwk.verifyJwt(noAudJwt, kTestIssuer, now, "fulla-portal"),
      JwkManager::JwtVerificationResult::AudienceMismatch
    );
    // ...but is accepted when no pin is requested (back-compat).
    EXPECT_EQ(
      jwk.verifyJwt(noAudJwt, kTestIssuer, now),
      JwkManager::JwtVerificationResult::Ok
    );
}

// ---------------------------------------------------------------------------
// #87 L1: strict base64url -- non-canonical trailing zero bits (RFC 4648
// §3.5) are rejected. Exercised via the header (decoded before the signature
// check) and the signature segment (decoded before EVP verify): aliasing the
// final symbol's unused low bits keeps the *bytes* identical under a lenient
// decoder, so the rejection proves the tail check fires.
// ---------------------------------------------------------------------------

TEST(JwkManagerTest, VerifyJwt_NonCanonicalBase64Tail_IsMalformed)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    const long long now = std::time(nullptr);
    const std::string jwt = jwk.signJwt(validClaims(600));
    const size_t firstDot = jwt.find('.');
    const size_t secondDot = jwt.find('.', firstDot + 1);

    // Alias the final symbol's unused low bits. Canonical encodings always
    // leave them zero, so value|1 yields a different symbol that leniently
    // decodes to the SAME bytes -- only a strict decoder rejects it. |1 stays
    // <= 61, so the aliased value is always inside the alphabet.
    auto aliasTail = [](std::string segment) {
        static const char alphabet[] =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        const size_t remainder = segment.size() % 4;
        EXPECT_TRUE(remainder == 2 || remainder == 3);
        const char last = segment.back();
        const int value = (last >= 'A' && last <= 'Z')   ? last - 'A'
                          : (last >= 'a' && last <= 'z') ? last - 'a' + 26
                          : (last >= '0' && last <= '9') ? last - '0' + 52
                          : (last == '-')                ? 62
                                                         : 63;
        segment.back() = alphabet[value | 1];
        return segment;
    };

    // 256-byte RS256 signature -> 342 symbols -> a 2-symbol tail group, so
    // the alias is guaranteed to sit in the tail. The strict decode rejects
    // it before EVP ever sees the bytes.
    const std::string aliasedSignature = aliasTail(jwt.substr(secondDot + 1));
    EXPECT_EQ(
      jwk.verifyJwt(jwt.substr(0, secondDot + 1) + aliasedSignature, kTestIssuer, now),
      JwkManager::JwtVerificationResult::Malformed
    );

    // Header path: craft a header whose JSON length leaves a tail group
    // ({"alg":"RS256","x":12} is 22 bytes -> 30 symbols -> tail of 2). The
    // header is decoded before the signature check, so the aliased tail is
    // rejected as Malformed even though the (unmatching) signature would
    // fail anyway.
    const std::string craftedHeader = b64Url("{\"alg\":\"RS256\",\"x\":12}");
    const std::string payloadAndSig = jwt.substr(firstDot);
    EXPECT_EQ(
      jwk.verifyJwt(aliasTail(craftedHeader) + payloadAndSig, kTestIssuer, now),
      JwkManager::JwtVerificationResult::Malformed
    );
    // Control: the canonical crafted header decodes fine and fails LATER, at
    // the signature check (signed over signJwt's own header).
    EXPECT_EQ(
      jwk.verifyJwt(craftedHeader + payloadAndSig, kTestIssuer, now),
      JwkManager::JwtVerificationResult::BadSignature
    );
    // And the untouched round-trip still verifies.
    EXPECT_EQ(jwk.verifyJwt(jwt, kTestIssuer, now), JwkManager::JwtVerificationResult::Ok);
}

// ---------------------------------------------------------------------------
// #87 L2: verifyAndDecode returns the verified payload in one pass.
// ---------------------------------------------------------------------------

TEST(JwkManagerTest, VerifyAndDecode_Ok_ReturnsPayload_And_Rejection_SetsReason)
{
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    const long long now = std::time(nullptr);

    Json::Value claims = validClaims(600);
    claims["aud"] = "fulla-portal";
    claims["custom"] = "carry-me";
    const std::string jwt = jwk.signJwt(claims);

    JwkManager::JwtVerificationResult reason = JwkManager::JwtVerificationResult::Ok;
    auto payload = jwk.verifyAndDecode(jwt, kTestIssuer, now, "fulla-portal", &reason);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(reason, JwkManager::JwtVerificationResult::Ok);
    EXPECT_EQ((*payload)["sub"].asString(), "user-123");
    EXPECT_EQ((*payload)["custom"].asString(), "carry-me");

    reason = JwkManager::JwtVerificationResult::Ok;
    auto rejected = jwk.verifyAndDecode(jwt, kTestIssuer, now, "wrong-client", &reason);
    EXPECT_FALSE(rejected.has_value());
    EXPECT_EQ(reason, JwkManager::JwtVerificationResult::AudienceMismatch);
}

// ---------------------------------------------------------------------------
// #87 addendum: the ephemeral fallback path honors a configured kid.
// ---------------------------------------------------------------------------

TEST(JwkManagerTest, Init_EphemeralKey_RespectsConfiguredKid)
{
    JwkManager jwk;
    Json::Value config(Json::objectValue);
    config["kid"] = "custom-ephemeral-kid";
    ASSERT_TRUE(jwk.init(config));
    EXPECT_EQ(jwk.getKeyId(), "custom-ephemeral-kid");
    // Absent kid keeps the historical ephemeral marker.
    JwkManager jwk2;
    ASSERT_TRUE(jwk2.init(Json::Value(Json::objectValue)));
    EXPECT_EQ(jwk2.getKeyId(), "ephemeral-dev-key");
}

// #102: the ephemeral fallback is DEV ONLY -- under FULLA_ENV=production with
// no key source configured, init() must fail (the caller refuses to start)
// instead of silently signing with a per-boot random key.
TEST(JwkManagerTest, Init_ProductionEnv_NoKeySource_RefusesEphemeral)
{
    EnvVarGuard envGuard("FULLA_ENV", "production");
    EnvVarGuard keyGuard("FULLA_SIGNING_KEY", "");
    EnvVarGuard keyPathGuard("FULLA_JWT_KEY_PATH", "");
    JwkManager jwk;
    EXPECT_FALSE(jwk.init(Json::Value(Json::objectValue)));
    EXPECT_FALSE(jwk.isInitialized());
}

// Control: any non-production value keeps the dev fallback alive (the
// quick-start path with no key configured must not regress).
TEST(JwkManagerTest, Init_DevelopmentEnv_NoKeySource_EphemeralOk)
{
    EnvVarGuard envGuard("FULLA_ENV", "development");
    EnvVarGuard keyGuard("FULLA_SIGNING_KEY", "");
    EnvVarGuard keyPathGuard("FULLA_JWT_KEY_PATH", "");
    JwkManager jwk;
    EXPECT_TRUE(jwk.init(Json::Value(Json::objectValue)));
    EXPECT_TRUE(jwk.isInitialized());
}


// ---------------------------------------------------------------------------
// #110-B keystore directory: multi-key load, active-kid signing, verify
// routing across keys, JWKS publication of every key, rotation semantics
// (retired key no longer verifies), and the structural failure modes.
// ---------------------------------------------------------------------------

// Build a keystore dir with the given kids (fresh PEMs) + active_kid marker;
// returns the dir path.
// Local issuer for the keystore tests (kTestIssuer lives in the nested
// anonymous namespace above and is not visible here).
constexpr const char *kStoreIssuer = "https://auth.example.test";

// Write a keystore dir from EXPLICIT (kid, pem) pairs -- the multi-manager
// rotation tests must share one key material across directories (a fresh
// generateTestPem() per dir would give same-named kids DIFFERENT keys).
std::string writeKeystoreDir(
  const std::vector<std::pair<std::string, std::string>> &entries,
  const std::string &activeKid
)
{
    namespace fs = std::filesystem;
    static int counter = 100;
    const std::string dir = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : "/tmp") +
                            "/jwkstore_" + std::to_string(++counter);
    std::error_code ec;
    fs::remove_all(fs::path(dir), ec);  // stale pems from earlier runs poison the load
    fs::create_directories(dir);
    for (const auto &[kid, pem] : entries)
    {
        std::ofstream f(fs::path(dir) / (kid + ".pem"), std::ios::binary | std::ios::trunc);
        f << pem;
    }
    std::ofstream active(fs::path(dir) / "active_kid", std::ios::trunc);
    active << activeKid << '\n';
    return dir;
}

std::string makeKeystoreDir(const std::vector<std::string> &kids, const std::string &activeKid)
{
    namespace fs = std::filesystem;
    static int counter = 0;
    const std::string dir = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : "/tmp") +
                            "/jwkstore_" + std::to_string(++counter);
    std::error_code ec;
    fs::remove_all(fs::path(dir), ec);  // stale pems from earlier runs poison the load
    fs::create_directories(dir);
    for (const auto &kid : kids)
    {
        std::ofstream f(fs::path(dir) / (kid + ".pem"), std::ios::binary | std::ios::trunc);
        f << generateTestPem();
    }
    std::ofstream active(fs::path(dir) / "active_kid", std::ios::trunc);
    active << activeKid << "\n";
    return dir;
}

Json::Value keystoreConfig(const std::string &dir)
{
    Json::Value config(Json::objectValue);
    config["signing_keystore_dir"] = dir;
    return config;
}

TEST(JwkManagerTest, Keystore_LoadsAllKeysAndActiveKid)
{
    const std::string dir = makeKeystoreDir({"k2025", "k2026"}, "k2026");
    JwkManager jwk;
    EXPECT_TRUE(jwk.init(keystoreConfig(dir)));
    EXPECT_EQ(jwk.getKeyId(), "k2026");

    Json::Value jwks = jwk.getJwks();
    ASSERT_TRUE(jwks["keys"].isArray());
    ASSERT_EQ(jwks["keys"].size(), 2u);
    // Sorted by filename: k2025 first, k2026 second.
    EXPECT_EQ(jwks["keys"][0]["kid"].asString(), "k2025");
    EXPECT_EQ(jwks["keys"][1]["kid"].asString(), "k2026");
}

TEST(JwkManagerTest, Keystore_SignsWithActiveKidAndVerifies)
{
    const std::string dir = makeKeystoreDir({"k2025", "k2026"}, "k2026");
    JwkManager jwk;
    ASSERT_TRUE(jwk.init(keystoreConfig(dir)));

    Json::Value claims;
    claims["iss"] = kStoreIssuer;
    claims["sub"] = "user-1";
    claims["exp"] = static_cast<Json::Int64>(std::time(nullptr) + 300);
    const std::string jwt = jwk.signJwt(claims);
    ASSERT_FALSE(jwt.empty());

    // Round-trips on the same (both-keys) manager.
    EXPECT_EQ(
      jwk.verifyJwt(jwt, kStoreIssuer, std::time(nullptr)),
      JwkManager::JwtVerificationResult::Ok
    );

    // Kid routing proves the ACTIVE key signed it: a manager holding ONLY
    // k2025 rejects the k2026-kid token with KidMismatch (not BadSignature).
    const std::string dirOldOnly = makeKeystoreDir({"k2025"}, "k2025");
    JwkManager oldOnly;
    ASSERT_TRUE(oldOnly.init(keystoreConfig(dirOldOnly)));
    EXPECT_EQ(
      oldOnly.verifyJwt(jwt, kStoreIssuer, std::time(nullptr)),
      JwkManager::JwtVerificationResult::KidMismatch
    );
}

TEST(JwkManagerTest, Keystore_RetiredKeyStopsVerifyingAfterRemoval)
{
    // ONE key material shared across the three rotation-stage directories.
    const std::string pemOld = generateTestPem();
    const std::string pemNew = generateTestPem();

    // Rotation step 1+2 state: both keys loaded, OLD one still signing.
    const std::string dirBoth =
      writeKeystoreDir({{"k2025", pemOld}, {"k2026", pemNew}}, "k2025");
    JwkManager signing;
    ASSERT_TRUE(signing.init(keystoreConfig(dirBoth)));

    Json::Value claims;
    claims["iss"] = kStoreIssuer;
    claims["sub"] = "user-1";
    claims["exp"] = static_cast<Json::Int64>(std::time(nullptr) + 300);
    const std::string oldKeyToken = signing.signJwt(claims);  // signed by k2025 (active)
    ASSERT_FALSE(oldKeyToken.empty());

    // While k2025 is still published, a manager that loaded BOTH keys
    // verifies it (grace window) -- same key material, active flipped.
    {
        const std::string dirVerify =
          writeKeystoreDir({{"k2025", pemOld}, {"k2026", pemNew}}, "k2026");
        JwkManager verifier;
        ASSERT_TRUE(verifier.init(keystoreConfig(dirVerify)));
        EXPECT_EQ(
          verifier.verifyJwt(oldKeyToken, kStoreIssuer, std::time(nullptr)),
          JwkManager::JwtVerificationResult::Ok
        );
    }

    // Rotation step 3: k2025 removed from the keystore -> its tokens now
    // fail with KidMismatch (unknown kid), never BadSignature confusion.
    {
        const std::string dirRetired = writeKeystoreDir({{"k2026", pemNew}}, "k2026");
        JwkManager retired;
        ASSERT_TRUE(retired.init(keystoreConfig(dirRetired)));
        EXPECT_EQ(
          retired.verifyJwt(oldKeyToken, kStoreIssuer, std::time(nullptr)),
          JwkManager::JwtVerificationResult::KidMismatch
        );
    }
}

TEST(JwkManagerTest, Keystore_ActiveKidMissingFromDir_FailsInit)
{
    const std::string dir = makeKeystoreDir({"k2026"}, "k2026");
    // Marker names a kid with no pem.
    std::ofstream active(std::filesystem::path(dir) / "active_kid", std::ios::trunc);
    active << "k9999\n";

    JwkManager jwk;
    EXPECT_FALSE(jwk.init(keystoreConfig(dir)));
    EXPECT_FALSE(jwk.isInitialized());
}

TEST(JwkManagerTest, Keystore_NoActiveKidMarker_FailsInit)
{
    namespace fs = std::filesystem;
    const std::string dir = makeKeystoreDir({"k2026"}, "k2026");
    fs::remove(fs::path(dir) / "active_kid");

    JwkManager jwk;
    EXPECT_FALSE(jwk.init(keystoreConfig(dir)));
}

TEST(JwkManagerTest, Keystore_BrokenDirDoesNotFallBackToEnvKey)
{
    // A configured-but-invalid keystore must be a HARD failure (init false),
    // never a silent fallthrough to FULLA_SIGNING_KEY: half-configured
    // rotation state signing with an unexpected key is the worst outcome.
    const std::string dir = makeKeystoreDir({"k2026"}, "k2026");
    std::filesystem::remove(std::filesystem::path(dir) / "active_kid");

    EnvVarGuard env("FULLA_SIGNING_KEY", generateTestPem());
    JwkManager jwk;
    EXPECT_FALSE(jwk.init(keystoreConfig(dir)));
}

TEST(JwkManagerTest, Keystore_TakesPrecedenceOverEnvKey)
{
    // With a VALID keystore configured, the env key is ignored (keystore is
    // authoritative) -- the active kid proves which source loaded.
    const std::string dir = makeKeystoreDir({"kk"}, "kk");
    EnvVarGuard env("FULLA_SIGNING_KEY", generateTestPem());
    JwkManager jwk;
    EXPECT_TRUE(jwk.init(keystoreConfig(dir)));
    EXPECT_EQ(jwk.getKeyId(), "kk");
    Json::Value jwks = jwk.getJwks();
    ASSERT_EQ(jwks["keys"].size(), 1u);
}

}  // namespace
