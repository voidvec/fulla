#pragma once

// M2.5 identity completion (fulla-sdk-refactor, design.md §5.1/§6):
// real (non-placeholder) implementation, replacing the previous
// `#ifdef WITH_WEBAUTHN ... TODO` placeholder. Ports the business logic
// out of libs/drogon/src/controllers/WebAuthnController.cc's
// registerBegin/registerFinish/authenticateBegin/authenticateFinish/
// listCredentials handlers into a framework-independent service,
// following AuthService.h/MfaService.h's established pattern:
// dependencies (repository + ports) are injected through the
// constructor, no Drogon/DB-client/HTTP types appear anywhere in this
// class, and the async std::function<void(...)> &&callback convention is
// used throughout even where an individual step happens to be
// synchronous (challenge generation), for consistency with
// AuthService/MfaService's own async surface.
//
// Shape differences from the controller, and why:
//   - No drogon::HttpRequestPtr/HttpResponsePtr anywhere; methods take
//     plain values in and invoke a callback with a plain result struct
//     or std::optional<...>, mirroring AuthService::validateUser's
//     "optional on failure, populated struct on success" convention.
//   - Session-based challenge storage (req->session()->insert(...)) is
//     Drogon-specific and out of scope for a Domain-layer class. Instead,
//     beginRegistration()/beginAuthentication() generate a challenge and
//     hand it back to the caller as a plain string field on the returned
//     struct -- the caller (future production wiring, e.g. a controller
//     in libs/drogon) is responsible for storing/retrieving it from
//     wherever is appropriate (a Drogon session, same as today, or
//     anything else), exactly mirroring how the controller currently
//     treats req->session() as an external concern this class does not
//     own.
//   - Internal user id vs public_sub: every method here is keyed by the
//     internal auto-increment user id (int64_t), not the public_sub
//     string the controller happens to read out of the "userId" request
//     attribute -- same convention as IWebAuthnRepository.h's header
//     comment and AuthService/MfaService's existing precedent. Resolving
//     public_sub -> internal id is the caller's job.
//   - RP (Relying Party) id/name are constructor parameters instead of
//     being read from drogon::app().getCustomConfig() on every call
//     (mirrors MfaService's issuerName_ constructor parameter for the
//     analogous otpauth:// issuer field).
//
// Explicitly NOT ported (matches an existing, pre-existing simplification
// in the controller, not something introduced or required to be fixed
// here): this class does not perform real WebAuthn/FIDO2 cryptographic
// attestation/assertion verification. The current production controller
// does not do this either -- it trusts the client-submitted
// credential_id/public_key directly at registerFinish, and at
// authenticateFinish it does not verify a signed assertion against the
// stored public_key/challenge at all, only that the credential_id exists.
// finishRegistration()/finishAuthentication() below preserve that exact
// (simplified) behavioral contract; a real FIDO2 verifier would need to
// use the stored public_key and the challenge returned by
// beginAuthentication() to verify a signature, which is future work
// tracked by tasks.md Task 31 (WebAuthn crypto/CBOR dependencies), not
// this task.
//
// #142 UPDATE: real verification landed. The two legacy finish methods are
// RETAINED DECLARED (SDK API baseline) but now FAIL CLOSED -- they never
// store or accept anything unverified. The live contracts are
// finishRegistrationVerified()/finishAuthenticationVerified() plus the
// subject-bound challenge store below; the crypto lives in
// src/webauthn/WebAuthnCrypto.{h,cc} (libcbor + OpenSSL, ES256-only,
// fmt="none"-only) against W3C WebAuthn Level 2 §7.1/§6.1.
//
// Scope boundary (design.md §4.1 rule 2, identity <-> oauth2 互不依赖):
// see IWebAuthnRepository.h's header comment -- this class only handles
// the identity-owned credential state and does not drive "authenticate,
// then issue OAuth2 tokens" orchestration (that crosses the
// identity/oauth2 boundary and belongs to future product-level assembly,
// same as MfaService.h's identical rationale for its
// verifyLoginCode()/token-issuance split).

#include <fulla/common/ports/ICryptoProvider.h>
#include <fulla/identity/IWebAuthnRepository.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fulla::identity
{

/**
 * @brief Result of WebAuthnService::beginRegistration -- the fields the
 * controller's registerBegin currently assembles into a
 * PublicKeyCredentialCreationOptions JSON payload (challenge/rp
 * id+name/timeout). The user.id/name/displayName + pubKeyCredParams +
 * authenticatorSelection fields the controller also emits are static
 * protocol boilerplate / view-construction concerns, not business logic,
 * so they are left to the caller rather than duplicated here.
 */
struct WebAuthnRegistrationChallenge
{
    std::string challenge;  // Base64url random challenge (32 bytes / 256 bits of entropy).
    std::string rpId;
    std::string rpName;
    int timeoutMs = 60000;
};

/**
 * @brief Result of WebAuthnService::beginAuthentication -- the fields the
 * controller's authenticateBegin assembles (challenge/rpId/timeout;
 * allowCredentials is left empty for the discoverable/resident-key flow,
 * same as the controller).
 */
struct WebAuthnAuthenticationChallenge
{
    std::string challenge;
    std::string rpId;
    int timeoutMs = 60000;
};

/**
 * @brief Result of a successful WebAuthnService::finishAuthentication --
 * mirrors the fields the controller's authenticateFinish returns
 * (user_id [public_sub] + the post-increment sign_count).
 */
struct WebAuthnAuthResult
{
    int32_t userId = 0;     // Internal user id (for the caller's own follow-up lookups).
    std::string publicSub;  // users.public_sub -- what the controller today calls "user_id".
    int signCount = 0;      // sign_count AFTER this authentication's increment.
    // #142 PR review: canonical base64url credential id (the stored form),
    // so callers audit/respond with the server-normalized value instead
    // of the client-submitted encoding.
    std::string credentialId;
};

/**
 * @brief WebAuthn / passkey business logic: challenge generation,
 * credential registration, authentication (sign_count bookkeeping), and
 * credential listing. Framework-independent -- persistence and crypto
 * are injected through the constructor.
 */
class WebAuthnService
{
  public:
    /**
     * @brief Construct the service with dependencies.
     * @param repo Persistence for credential records (required).
     * @param crypto Randomness/encoding port for challenge generation
     * (required).
     * @param rpId WebAuthn Relying Party id (defaults to "localhost",
     * matching the controller's own default).
     * @param rpName WebAuthn Relying Party display name (defaults to
     * "OAuth2 Server", matching the controller's own default).
     */
    WebAuthnService(
      std::shared_ptr<IWebAuthnRepository> repo,
      std::shared_ptr<fulla::common::ports::ICryptoProvider> crypto,
      std::string rpId = "localhost",
      std::string rpName = "Fulla"
    );

    /**
     * @brief Begin passkey registration: generate a fresh challenge.
     * @param callback Result with the challenge + RP info, or nullopt if
     * the crypto dependency is missing.
     */
    void beginRegistration(
      std::function<void(std::optional<WebAuthnRegistrationChallenge>)> &&callback
    );

    /**
     * @brief Finish passkey registration: validate and store a new
     * credential.
     * @param userId Internal user id the credential belongs to.
     * @param credentialId Client-submitted credential identifier
     * (required).
     * @param publicKey Client-submitted public key material (required).
     * @param name Human-readable label for the credential; defaults to
     * "Passkey" if empty (mirrors the controller's JSON-parsing default).
     * @param callback Invoked with an empty string on success, or a
     * structured Error_Code (registered in ErrorCatalog) on failure --
     * mirrors AuthService::registerUser's existing contract so callers
     * can forward the value verbatim to ErrorResponder. Possible codes:
     * VALIDATION_MISSING_REQUIRED_FIELD (credentialId/publicKey empty),
     * VALIDATION_CREDENTIAL_ALREADY_REGISTERED (duplicate credentialId),
     * DB_QUERY_ERROR (any other repository failure).
     */
    void finishRegistration(
      int32_t userId,
      const std::string &credentialId,
      const std::string &publicKey,
      const std::string &name,
      std::function<void(const std::string &errorCode)> &&callback
    );

    /**
     * @brief Begin passkey authentication: generate a fresh challenge.
     * @param callback Result with the challenge + RP id, or nullopt if
     * the crypto dependency is missing.
     */
    void beginAuthentication(
      std::function<void(std::optional<WebAuthnAuthenticationChallenge>)> &&callback
    );

    /**
     * @brief Finish passkey authentication: look up the credential and,
     * if found, increment its sign_count.
     * @param credentialId Client-submitted credential identifier.
     * @param callback Result with the owning user's info + new
     * sign_count on success; nullopt if credentialId is empty or does
     * not match any stored credential (mirrors
     * AUTH_INVALID_CREDENTIALS -- collapsed into a single failure signal,
     * same security-conscious convention as AuthService::validateUser).
     */
    void finishAuthentication(
      const std::string &credentialId,
      std::function<void(std::optional<WebAuthnAuthResult>)> &&callback
    );

    /**
     * @brief List all credentials belonging to a user (for the
     * credentials-management endpoint).
     * @param userId Internal user id.
     * @param callback The user's credentials, most recently created
     * first. Empty vector if the user has none (or the repository is
     * missing).
     */
    void listCredentials(
      int32_t userId,
      std::function<void(std::vector<WebAuthnCredentialSummary>)> &&callback
    );

    // ------------------------------------------------------------------
    // #142: real verification surface (additive). The methods above keep
    // their declarations; the two legacy finish methods fail closed now
    // (see their reimplemented bodies).
    // ------------------------------------------------------------------

    /**
     * @brief Strict allowlist of accepted clientDataJSON origins
     * (webauthn.rp_origins config). Verification fails closed while the
     * list is empty.
     */
    void setRpOrigins(std::vector<std::string> origins)
    {
        rpOrigins_ = std::move(origins);
    }

    /// @brief Challenge TTL in seconds (default 300; Drogon sessions have
    /// no per-key TTL so it is enforced in code on every consume).
    void setChallengeTtlSeconds(int64_t seconds)
    {
        challengeTtlSeconds_ = seconds > 0 ? seconds : 300;
    }

    /**
     * @brief Clone-detection observability hook (the domain layer stays
     * logging-free): fired when an assertion's signCount regressed
     * (stored > 0 and new <= stored, WebAuthn L2 §6.1 step 17); the
     * authentication is rejected. Assembly wires this to an audit action.
     */
    void setCloneDetectorNotifier(std::function<void(int32_t userId, const std::string &credentialId)> notifier)
    {
        cloneDetectorNotifier_ = std::move(notifier);
    }

    /**
     * @brief Issue a REGISTRATION challenge bound to the authenticated
     * subject (the Bearer public_sub the filter resolves). The register
     * endpoints run behind OAuth2AuthFilter; session cookies are NOT part
     * of their contract (the user SPA sends no credentials), so the store
     * is an in-process map keyed by subject, last-write-wins; stale
     * entries are rejected on consume AND lazily swept on every issue
     * (no background thread).
     * Single-instance deployments only (same limitation class as the
     * consent_csrf nonce store) -- multi-instance challenge sharing is
     * registered follow-up work.
     * @return The base64url challenge (nullopt if crypto is missing).
     */
    std::optional<std::string> issueRegistrationChallenge(const std::string &subject);

    /**
     * @brief Consume the subject-bound registration challenge. The stored
     * entry is erased UNCONDITIONALLY (match or not) -- a challenge's
     * validity must not be probed repeatedly.
     * @return true iff a live (non-expired) entry existed and matched the
     * presented value.
     */
    bool consumeRegistrationChallenge(const std::string &subject, const std::string &presentedChallenge);

    /**
     * @brief Issue an AUTHENTICATION (assertion) challenge. The flow is
     * anonymous and session-cookie-carried (the cookie contract is
     * documented: callers must send credentials); the caller persists the
     * opaque session value (challenge|issuedAt, TTL enforced on verify).
     */
    struct IssuedSessionChallenge
    {
        std::string challenge;   // base64url, echoed by the client.
        std::string sessionValue;  // opaque "challenge|issuedAtEpochSeconds".
    };
    std::optional<IssuedSessionChallenge> issueAuthenticationChallenge();

    /**
     * @brief Verify a session-carried authentication challenge against the
     * presented value, TTL-enforced. The CALLER erases the session key
     * after this call regardless of the result (unconditional
     * consumption).
     */
    bool verifyAuthenticationChallenge(const std::string &sessionValue, const std::string &presentedChallenge);

    /// Raw registration-attestation inputs (all base64url, browser shape).
    struct RegistrationInput
    {
        std::string id;                 // body id (must equal rawId).
        std::string rawId;
        std::string attestationObject;
        std::string clientDataJSON;
        std::string name;               // optional label.
    };

    /**
     * @brief Verified registration finish (WebAuthn L2 §7.1): consumes the
     * subject-bound challenge, checks clientDataJSON
     * (type=webauthn.create / challenge match / origin allowlist /
     * tokenBinding), parses the attestation object (fmt="none" only),
     * authenticator data (rpIdHash, UP, AT, credIdLen<=1023, ED tolerated)
     * and the COSE ES256 key, requires id==rawId==authData credential id,
     * then stores credential_id/public_key as the canonical base64url of
     * the RAW bytes. The client-submitted public_key field is ignored.
     * @param userId Internal user id the caller resolved for the subject
     * (owner of the new credential).
     * @param subject Authenticated subject the challenge was bound to.
     * @param presentedChallenge Challenge echoed inside clientDataJSON.
     * @return "" on success; WEBAUTHN_CHALLENGE_MISMATCH (challenge/origin
     * gate), WEBAUTHN_INVALID_ATTESTATION (attestation/authData/COSE
     * format or alg), VALIDATION_CREDENTIAL_ALREADY_REGISTERED,
     * DB_QUERY_ERROR, INTERNAL_ERROR.
     */
    void finishRegistrationVerified(
      int32_t userId,
      const std::string &subject,
      const std::string &presentedChallenge,
      const RegistrationInput &input,
      std::function<void(const std::string &errorCode)> &&callback
    );

    /// Raw assertion inputs (all base64url, browser shape).
    struct AssertionInput
    {
        std::string id;                 // credential id (must equal rawId).
        std::string rawId;
        std::string authenticatorData;
        std::string clientDataJSON;
        std::string signature;          // ASN.1 DER ECDSA signature.
        std::string userHandle;         // optional; when present must name
                                        // the credential's owner.
    };

    /**
     * @brief Verified assertion finish (WebAuthn L2 §6.1): clientDataJSON
     * (type=webauthn.get / challenge / origin), userHandle owner check,
     * authenticator data (rpIdHash, UP=1, UV=1 -- begin advertises
     * userVerification=required), ES256 signature over
     * authData || SHA256(clientDataJSON), signCount clone policy
     * (stored>0 && new<=stored -> reject + clone notifier; both zero ->
     * legal skip). Every failure answers nullopt (the caller maps to a
     * generic AUTH_INVALID_CREDENTIALS -- no oracle which check failed).
     */
    void finishAuthenticationVerified(
      const std::string &presentedChallenge,
      const AssertionInput &input,
      std::function<void(std::optional<WebAuthnAuthResult>)> &&callback
    );

  private:
    std::shared_ptr<IWebAuthnRepository> repo_;
    std::shared_ptr<fulla::common::ports::ICryptoProvider> crypto_;
    std::string rpId_;
    std::string rpName_;
    std::vector<std::string> rpOrigins_;
    int64_t challengeTtlSeconds_ = 300;
    std::function<void(int32_t, const std::string &)> cloneDetectorNotifier_;
    // Subject-bound registration challenges (see issueRegistrationChallenge).
    std::mutex challengeMu_;
    struct ChallengeEntry
    {
        std::string challenge;
        int64_t issuedAt = 0;
    };
    std::unordered_map<std::string, ChallengeEntry> registrationChallenges_;
};

}  // namespace fulla::identity
