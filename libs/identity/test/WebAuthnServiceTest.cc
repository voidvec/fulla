// M2.5 identity completion (fulla-sdk-refactor): unit tests for
// fulla::identity::WebAuthnService, exercised against a minimal
// in-memory fake IWebAuthnRepository (no DB/no Drogon).

#include <fulla/common/testing/FakeCryptoProvider.h>
#include <fulla/identity/IWebAuthnRepository.h>
#include <fulla/identity/WebAuthnService.h>
// Shared test double (promoted from this file's former anonymous-namespace
// fake): FakeWebAuthnRepository + StoredCredential. See
// libs/identity/include/fulla/identity/testing/.
#include <fulla/identity/testing/FakeWebAuthnRepository.h>

#include <chrono>
#include <thread>
#include <json/json.h>

#include <gtest/gtest.h>

#include <memory>

using namespace fulla::identity;
using fulla::common::testing::FakeCryptoProvider;
using fulla::identity::testing::FakeWebAuthnRepository;

namespace
{

std::shared_ptr<WebAuthnService> makeService(std::shared_ptr<FakeWebAuthnRepository> repo)
{
    auto crypto = std::make_shared<FakeCryptoProvider>();
    return std::make_shared<WebAuthnService>(repo, crypto);
}

std::pair<std::shared_ptr<WebAuthnService>, std::shared_ptr<FakeCryptoProvider>>
makeServiceWithCrypto(std::shared_ptr<FakeWebAuthnRepository> repo)
{
    auto crypto = std::make_shared<FakeCryptoProvider>();
    return {std::make_shared<WebAuthnService>(repo, crypto), crypto};
}

}  // namespace

TEST(WebAuthnServiceTest, BeginRegistration_GeneratesChallengeAndRpInfo)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    auto svc = makeService(repo);

    std::optional<WebAuthnRegistrationChallenge> result;
    svc->beginRegistration([&](auto r) { result = std::move(r); });

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->challenge.empty());
    EXPECT_EQ(result->rpId, "localhost");
    EXPECT_EQ(result->rpName, "Fulla");
    EXPECT_EQ(result->timeoutMs, 60000);
}

TEST(WebAuthnServiceTest, BeginRegistration_TwoCallsProduceDifferentChallenges)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    auto svc = makeService(repo);

    std::optional<WebAuthnRegistrationChallenge> r1;
    std::optional<WebAuthnRegistrationChallenge> r2;
    svc->beginRegistration([&](auto r) { r1 = std::move(r); });
    svc->beginRegistration([&](auto r) { r2 = std::move(r); });

    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_NE(r1->challenge, r2->challenge);
}

// #142: the legacy unverified registration contract FAILS CLOSED — no
// input shape stores anything. (The verified contract lives in
// finishRegistrationVerified; the HTTP tests cover its positive path with
// real crypto material, and the crypto layer has its own 32-case suite.)
TEST(WebAuthnServiceTest, FinishRegistration_LegacyContract_FailsClosed)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    auto svc = makeService(repo);

    std::string errorCode = "unset";
    svc->finishRegistration(42, "cred-1", "pubkey-1", "My Passkey", [&](const std::string &code) {
        errorCode = code;
    });

    EXPECT_EQ(errorCode, "WEBAUTHN_INVALID_ATTESTATION");
    EXPECT_TRUE(repo->credentials.empty());
}

// #142: verified registration finish — the challenge gate fires BEFORE any
// parsing (WEBAUTHN_CHALLENGE_MISMATCH, and the subject-bound entry is
// consumed unconditionally), and a malformed attestation body never stores
// anything (WEBAUTHN_INVALID_ATTESTATION).
TEST(WebAuthnServiceTest, FinishRegistrationVerified_NoChallenge_ReturnsMismatch)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    auto [svc, crypto] = makeServiceWithCrypto(repo);
    svc->setRpOrigins({"https://example.test"});

    WebAuthnService::RegistrationInput input;
    input.id = "aa";
    input.rawId = "aa";
    input.attestationObject = "aa";  // garbage once the challenge gate passes
    input.name = "Laptop";

    std::string errorCode;
    svc->finishRegistrationVerified(
      42, "subject-1", "not-issued", input,
      [&](const std::string &code) { errorCode = code; });

    EXPECT_EQ(errorCode, "WEBAUTHN_CHALLENGE_MISMATCH");
    EXPECT_TRUE(repo->credentials.empty());

    // A second attempt carries the freshly issued challenge in a
    // well-formed clientDataJSON, so the challenge gate PASSES and the
    // garbage attestation object is what fails.
    auto challenge = svc->issueRegistrationChallenge("subject-1");
    ASSERT_TRUE(challenge.has_value());
    Json::Value clientData;
    clientData["type"] = "webauthn.create";
    clientData["challenge"] = *challenge;
    clientData["origin"] = "https://example.test";
    Json::StreamWriterBuilder w;
    input.clientDataJSON = crypto->base64UrlEncode(Json::writeString(w, clientData));
    std::string second;
    svc->finishRegistrationVerified(
      42, "subject-1", *challenge, input,
      [&](const std::string &code) { second = code; });
    EXPECT_EQ(second, "WEBAUTHN_INVALID_ATTESTATION");
    EXPECT_TRUE(repo->credentials.empty());
}

// #142: subject-bound challenge store semantics — match consumes, mismatch
// ALSO consumes (unconditional), TTL expiry rejects.
// PR review (Low): the CAS interface default (used by in-memory fakes)
// forwards to plain updateSignCount -- single-threaded tests observe the
// non-atomic fallback; the PG override is exercised end-to-end by
// WebAuthnHttpTest's assertion chain.
TEST(WebAuthnServiceTest, UpdateSignCountIfCurrent_DefaultForwardsToUpdate)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    repo->credentials["cred-1"] = {42, "pk", "Passkey", 3};
    bool called = false;
    repo->updateSignCountIfCurrent("cred-1", 3, 4, [&](bool ok) {
        called = true;
        EXPECT_TRUE(ok);
    });
    EXPECT_TRUE(called);
    EXPECT_EQ(repo->credentials["cred-1"].signCount, 4);
}

TEST(WebAuthnServiceTest, RegistrationChallengeStore_UnconditionalConsumptionAndTtl)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    auto svc = makeService(repo);

    auto c1 = svc->issueRegistrationChallenge("subject-A");
    auto c2 = svc->issueRegistrationChallenge("subject-B");
    ASSERT_TRUE(c1.has_value());
    ASSERT_TRUE(c2.has_value());
    EXPECT_NE(*c1, *c2);

    // Wrong subject does not see subject-A's challenge.
    EXPECT_FALSE(svc->consumeRegistrationChallenge("subject-B", *c1));
    // subject-A's entry is still live and matches.
    EXPECT_TRUE(svc->consumeRegistrationChallenge("subject-A", *c1));
    // ...but was consumed by the successful match.
    EXPECT_FALSE(svc->consumeRegistrationChallenge("subject-A", *c1));
    // subject-B's failed match consumed it too (unconditional).
    EXPECT_FALSE(svc->consumeRegistrationChallenge("subject-B", *c2));

    // TTL: a 1-second window expires.
    svc->setChallengeTtlSeconds(1);
    auto c3 = svc->issueRegistrationChallenge("subject-C");
    ASSERT_TRUE(c3.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    EXPECT_FALSE(svc->consumeRegistrationChallenge("subject-C", *c3));
}

// #142: session-carried authentication challenge helper — value shape,
// TTL, and mismatch behavior.
TEST(WebAuthnServiceTest, AuthenticationChallengeSession_HelperSemantics)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    auto svc = makeService(repo);

    auto issued = svc->issueAuthenticationChallenge();
    ASSERT_TRUE(issued.has_value());
    EXPECT_FALSE(issued->challenge.empty());
    EXPECT_NE(issued->sessionValue.find(issued->challenge), std::string::npos);
    EXPECT_TRUE(svc->verifyAuthenticationChallenge(issued->sessionValue, issued->challenge));
    EXPECT_FALSE(svc->verifyAuthenticationChallenge(issued->sessionValue, "other"));
    EXPECT_FALSE(svc->verifyAuthenticationChallenge("garbage", issued->challenge));

    svc->setChallengeTtlSeconds(1);
    auto shortLived = svc->issueAuthenticationChallenge();
    ASSERT_TRUE(shortLived.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    EXPECT_FALSE(svc->verifyAuthenticationChallenge(shortLived->sessionValue, shortLived->challenge));
}

TEST(WebAuthnServiceTest, BeginAuthentication_GeneratesChallengeAndRpId)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    auto svc = makeService(repo);

    std::optional<WebAuthnAuthenticationChallenge> result;
    svc->beginAuthentication([&](auto r) { result = std::move(r); });

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->challenge.empty());
    EXPECT_EQ(result->rpId, "localhost");
    EXPECT_EQ(result->timeoutMs, 60000);
}

TEST(WebAuthnServiceTest, FinishAuthentication_UnknownCredential_ReturnsNullopt)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    auto svc = makeService(repo);

    std::optional<WebAuthnAuthResult> result;
    svc->finishAuthentication("no-such-cred", [&](auto r) { result = std::move(r); });

    EXPECT_FALSE(result.has_value());
}

TEST(WebAuthnServiceTest, FinishAuthentication_EmptyCredentialId_ReturnsNullopt)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    auto svc = makeService(repo);

    std::optional<WebAuthnAuthResult> result;
    svc->finishAuthentication("", [&](auto r) { result = std::move(r); });

    EXPECT_FALSE(result.has_value());
}

// #142: the credential_id-only authentication contract (knowing the id
// was proof of possession) FAILS CLOSED even for a stored credential.
TEST(WebAuthnServiceTest, FinishAuthentication_LegacyContract_FailsClosedEvenForKnownCredential)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    auto svc = makeService(repo);

    repo->credentials["cred-1"] = {42, "pubkey-1", "Passkey", 0};

    std::optional<WebAuthnAuthResult> result;
    svc->finishAuthentication("cred-1", [&](auto r) { result = std::move(r); });

    EXPECT_FALSE(result.has_value());
    // ...and nothing was touched.
    EXPECT_EQ(repo->credentials["cred-1"].signCount, 0);
}

TEST(WebAuthnServiceTest, ListCredentials_ReturnsOnlyMatchingUsersCredentials)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    auto svc = makeService(repo);

    repo->credentials["cred-1"] = {42, "pubkey-1", "Laptop", 0};
    repo->credentials["cred-2"] = {42, "pubkey-2", "Phone", 0};
    repo->credentials["cred-3"] = {99, "pubkey-3", "Other User", 0};

    std::vector<WebAuthnCredentialSummary> result;
    svc->listCredentials(42, [&](auto r) { result = std::move(r); });

    ASSERT_EQ(result.size(), 2u);
    for (const auto &summary : result)
    {
        EXPECT_TRUE(summary.credentialId == "cred-1" || summary.credentialId == "cred-2");
        EXPECT_EQ(summary.signCount, 0);
    }
}

TEST(WebAuthnServiceTest, ListCredentials_NoCredentials_ReturnsEmpty)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    auto svc = makeService(repo);

    std::vector<WebAuthnCredentialSummary> result;
    svc->listCredentials(7, [&](auto r) { result = std::move(r); });

    EXPECT_TRUE(result.empty());
}

TEST(WebAuthnServiceTest, ListCredentials_ReflectsSignCountAfterAuthentication)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    auto svc = makeService(repo);

    repo->credentials["cred-1"] = {42, "pubkey-1", "Laptop", 0};
    repo->credentials["cred-1"].signCount = 1;  // simulate a past authentication

    std::vector<WebAuthnCredentialSummary> result;
    svc->listCredentials(42, [&](auto r) { result = std::move(r); });

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].signCount, 1);
}

// ---------------------------------------------------------------------------
// Coverage additions (P1): null-dependency guards for every method
// (WebAuthnService.cc:45,66,109,126,168), the StoreCredentialOutcome::Error
// -> DB_QUERY_ERROR branch (cc:96-99), and custom rpId/rpName propagation.
// Null-guard tests construct WebAuthnService directly with nullptr deps.
// ---------------------------------------------------------------------------

// beginRegistration: null crypto guard -> nullopt (cc:45).
TEST(WebAuthnServiceTest, BeginRegistration_NullCrypto_ReturnsNullopt)
{
    WebAuthnService svc(nullptr, nullptr);
    std::optional<WebAuthnRegistrationChallenge> result = WebAuthnRegistrationChallenge{};
    svc.beginRegistration([&](auto r) { result = std::move(r); });
    EXPECT_FALSE(result.has_value());
}

// beginAuthentication: null crypto guard -> nullopt (cc:109).
TEST(WebAuthnServiceTest, BeginAuthentication_NullCrypto_ReturnsNullopt)
{
    WebAuthnService svc(nullptr, nullptr);
    std::optional<WebAuthnAuthenticationChallenge> result = WebAuthnAuthenticationChallenge{};
    svc.beginAuthentication([&](auto r) { result = std::move(r); });
    EXPECT_FALSE(result.has_value());
}

// #142: the legacy finishRegistration no longer touches the repository at
// all (null repo or store errors are unreachable — it fails closed before
// them), so both former guards now observe the closed contract.
TEST(WebAuthnServiceTest, FinishRegistration_NullRepo_FailsClosed)
{
    WebAuthnService svc(nullptr, nullptr);
    std::string err = "none";
    svc.finishRegistration(42, "cred-1", "pubkey-1", "Laptop", [&](const std::string &e) { err = e; });
    EXPECT_EQ(err, "WEBAUTHN_INVALID_ATTESTATION");
}

TEST(WebAuthnServiceTest, FinishRegistration_StoreError_UnreachableUnderClosedContract)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    repo->forceStoreError = true;
    auto svc = makeService(repo);
    std::string err = "none";
    svc->finishRegistration(42, "cred-1", "pubkey-1", "Laptop", [&](const std::string &e) { err = e; });
    EXPECT_EQ(err, "WEBAUTHN_INVALID_ATTESTATION");
    EXPECT_TRUE(repo->credentials.empty());
}

// finishAuthentication: null repo guard -> nullopt (cc:126).
TEST(WebAuthnServiceTest, FinishAuthentication_NullRepo_ReturnsNullopt)
{
    WebAuthnService svc(nullptr, nullptr);
    std::optional<WebAuthnAuthResult> result = WebAuthnAuthResult{};
    svc.finishAuthentication("cred-1", [&](auto r) { result = std::move(r); });
    EXPECT_FALSE(result.has_value());
}

// listCredentials: null repo guard -> empty vector (cc:168).
TEST(WebAuthnServiceTest, ListCredentials_NullRepo_ReturnsEmpty)
{
    WebAuthnService svc(nullptr, nullptr);
    std::vector<WebAuthnCredentialSummary> result;
    result.push_back({});  // sentinel so emptiness is observable
    svc.listCredentials(42, [&](auto r) { result = std::move(r); });
    EXPECT_TRUE(result.empty());
}

// Constructor: custom rpId/rpName propagate into begin-registration and
// begin-authentication challenges (cc:28-39,52-54,116-117). The existing
// tests only exercise the default values.
TEST(WebAuthnServiceTest, Constructor_CustomRpIdAndRpName_PropagatedToChallenges)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    auto crypto = std::make_shared<FakeCryptoProvider>();
    WebAuthnService svc(repo, crypto, "auth.example.com", "Custom Issuer");

    std::optional<WebAuthnRegistrationChallenge> reg;
    svc.beginRegistration([&](auto r) { reg = std::move(r); });
    ASSERT_TRUE(reg.has_value());
    EXPECT_EQ(reg->rpId, "auth.example.com");
    EXPECT_EQ(reg->rpName, "Custom Issuer");

    std::optional<WebAuthnAuthenticationChallenge> auth;
    svc.beginAuthentication([&](auto r) { auth = std::move(r); });
    ASSERT_TRUE(auth.has_value());
    EXPECT_EQ(auth->rpId, "auth.example.com");
}

// ---------------------------------------------------------------------------
// Coverage additions (P3): beginAuthentication produces a different
// challenge on each call (mirror of the existing registration test).
// ---------------------------------------------------------------------------

// beginAuthentication: two consecutive calls yield distinct challenges
// (WebAuthnService.cc:105-119, mirrors BeginRegistration_TwoCallsProduceDifferentChallenges).
TEST(WebAuthnServiceTest, BeginAuthentication_TwoCallsProduceDifferentChallenges)
{
    auto repo = std::make_shared<FakeWebAuthnRepository>();
    auto svc = makeService(repo);

    std::optional<WebAuthnAuthenticationChallenge> first;
    svc->beginAuthentication([&](auto r) { first = std::move(r); });
    std::optional<WebAuthnAuthenticationChallenge> second;
    svc->beginAuthentication([&](auto r) { second = std::move(r); });

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(first->challenge.empty());
    EXPECT_NE(first->challenge, second->challenge);
}
