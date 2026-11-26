#pragma once

// M2b Task 17 slice 10 (fulla-sdk-refactor, design.md §6's directory
// layout: "pkce/ jwk/ # PKCE、JWK 签名（经 ICryptoProvider 端口）"):
// relocates JwkManager from OAuth2Plugin/include/oauth2/utils/JwkManager.h
// into libs/oauth2 (Domain layer), now that slices 8-9 removed its two
// blockers (hardcoded DrogonLogger dependency; hardcoded
// fulla::drogon::adapters::OpenSslCryptoProvider dependency for base64url).
//
// Behavior/API is otherwise UNCHANGED from the pre-move class (same
// method signatures, same init-once-then-read-only concurrency contract,
// same OpenSSL RS256 signing implementation) -- this is a pure relocation
// plus a default-logger-fallback change (see the constructor doc below).
// OAuth2Plugin/include/oauth2/utils/JwkManager.h becomes a thin
// compatibility shim (`using JwkManager = fulla::oauth2::JwkManager`)
// so the many existing `oauth2::JwkManager`/`using oauth2::JwkManager`
// call sites across OAuth2Plugin/OAuth2Server do not need to change in
// this slice (design.md's namespace-sync-everywhere pass is explicitly a
// later task, M8 Task 40).

#include <fulla/common/ports/ILogger.h>

#include <string>
#include <json/json.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

namespace fulla::oauth2
{

/**
 * @brief Manages RSA signing keys for OpenID Connect id_token.
 *
 * Loads RSA private keys from a keystore directory (#110-B), a single PEM
 * file / env var (legacy single-key sources), or an ephemeral dev fallback.
 * Signs JWT tokens with the ACTIVE key (RS256); verifies by routing on the
 * JWT header kid across ALL loaded keys, so outstanding tokens signed by a
 * since-retired key keep verifying while its public half is still published.
 * Exposes every loaded public key in JWK format for /.well-known/jwks.json.
 *
 * ── Keystore directory (#110-B rotation) ─────────────────────────────────
 * config "signing_keystore_dir" points at a directory of
 *   <kid>.pem        one RSA private key per file, filename = kid
 *   active_kid       a one-line text file naming the signing kid
 * Rotation procedure (JwkManager is init-once/read-only by contract, so each
 * step restarts the server -- see docs/operate/configuration-guide.md §9):
 *   1. drop the new <kid>.pem in, restart  -> both keys published in JWKS,
 *      old key keeps signing;
 *   2. flip active_kid to the new kid, restart -> new key signs, old still
 *      published for verification;
 *   3. after the max token lifetime has passed, remove the old <kid>.pem,
 *      restart -> old key fully retired.
 *
 * ── Concurrency contract: init-once, then read-only ─────────────────────
 * init() is serialized and atomically publishes one immutable key snapshot.
 * A second call logs an error and is a no-op, preserving the first key set.
 * signJwt(), verifyJwt(), getJwks(), getKeyId(), and isInitialized() load
 * that snapshot and are safe concurrently, including before or during the
 * first init(); until publication they observe the uninitialized state.
 * Production also publishes this object as std::shared_ptr<const JwkManager>
 * after init() (see OAuth2Plugin::initAndStart()). Copies share the immutable
 * snapshot without copying keys; moves transfer it and leave the source
 * uninitialized. Copy/move construction and assignment serialize with init()
 * and assignment on the other object. A reference returned by getKeyId() is
 * invalidated by assignment, like any reference into a replaced value.
 *
 * OpenSSL concurrency assumption: see the comment on signJwt() --
 * concurrent signing is safe only under OpenSSL >= 1.1.0 with threads
 * enabled.
 */
class JwkManager
{
  public:
    /**
     * @brief Construct, optionally injecting the ILogger this class logs
     * through. Defaults to nullptr, in which case log() is a NO-OP (this
     * Domain-layer class has no Drogon-backed fallback to fall back to,
     * unlike the pre-move class -- see this header's own top comment).
     * Production call sites that want JwkManager's diagnostic log lines
     * (key-load source, init-once-violation warnings, sign failures)
     * MUST pass a real ILogger implementation explicitly (e.g.
     * fulla::drogon::adapters::DrogonLogger, from the Adapter layer).
     *
     * Does NOT take ownership: the caller must keep `*logger` alive for
     * at least this object's lifetime.
     */
    explicit JwkManager(fulla::common::ports::ILogger *logger = nullptr) : logger_(logger)
    {
    }

    JwkManager(const JwkManager &other);
    JwkManager &operator=(const JwkManager &other);
    JwkManager(JwkManager &&other);
    JwkManager &operator=(JwkManager &&other);

    ~JwkManager();

    /**
     * @brief Initialize from configuration (call EXACTLY ONCE, at startup).
     *
     * Source precedence (first that yields a loaded key wins):
     *   1. "signing_keystore_dir" -- multi-key rotation directory (#110-B);
     *   2. FULLA_SIGNING_KEY env (inline PEM, single key);
     *   3. FULLA_JWT_KEY_PATH env (PEM file, single key);
     *   4. "signing_key_path" config (PEM file, single key);
     *   5. ephemeral generated key -- DEV ONLY, refused under
     *      FULLA_ENV=production.
     * Single-key sources load with kid = config "kid" (default "key-1");
     * the keystore derives kids from the PEM filenames and the active key
     * from the directory's active_kid file.
     *
     * @param config JSON config with "signing_keystore_dir" /
     * "signing_key_path" / "kid" (or env vars).
     * @return true if at least one key is loaded and the manager is
     * initialized (including the no-op case where it was already
     * initialized); false on first-time initialization failure.
     */
    bool init(const Json::Value &config);

    /**
     * @brief Sign a JWT payload with RS256.
     * @param payload JSON payload (claims).
     * @return Signed JWT string (header.payload.signature), or "" on
     * failure (not initialized, or an OpenSSL signing error).
     */
    std::string signJwt(const Json::Value &payload) const;

    /// Outcome of verifyJwt(): one value per distinct rejection reason so
    /// callers can surface precise diagnostics (logged as Internal_Detail
    /// only; the client always gets the same generic error envelope).
    enum class JwtVerificationResult
    {
        Ok,
        NotInitialized,   ///< no key loaded; fail closed (cannot verify)
        Malformed,        ///< not 3 non-empty segments / bad base64url / bad JSON
        BadAlg,           ///< header alg absent or != RS256 (strict: no "none"/HS256 confusion)
        KidMismatch,      ///< header kid present but != the current key id
        BadSignature,     ///< RS256 signature check failed
        IssuerMismatch,   ///< payload iss != expectedIssuer
        Expired,          ///< payload exp <= nowSecs, or exp absent (fail closed)
        NotYetValid,      ///< #87 M2: payload nbf present and > nowSecs (RFC 7519 §4.1.5)
        MissingSubject,   ///< payload sub absent or empty
        AudienceMismatch  ///< #87 M1: expectedAudience requested but the aud claim does not contain
                          ///< it
    };

    /**
     * @brief Verify one of OUR RS256 JWTs end-to-end: signature + claim policy.
     *
     * The symmetric counterpart of signJwt() (#78: /oauth2/end_session must
     * not trust an id_token_hint's claims before the signature verifies).
     * Checks, in order: structure; header alg strictly RS256; header kid (if
     * present) equals the current kid; RS256 signature over header.payload;
     * payload iss == expectedIssuer; payload exp > nowSecs; payload nbf, if
     * present, <= nowSecs (#87 M2); payload sub non-empty; payload aud, when
     * expectedAudience is non-empty, contains it as the whole string claim or
     * as a string element of the array claim (#87 M1). Absent kid is
     * tolerated (tokens signed before a kid was configured still verify);
     * absent exp fails closed as Expired; absent nbf is fine (optional
     * claim); absent aud with a non-empty expectedAudience fails closed as
     * AudienceMismatch.
     *
     * Concurrency contract: same as signJwt() -- const, read-only after
     * init(), safe to call concurrently.
     *
     * @param jwt             The compact JWT (header.payload.signature).
     * @param expectedIssuer  The OP issuer the iss claim must match.
     * @param nowSecs         Current unix time (seconds) for exp/nbf checks.
     * @param expectedAudience Optional audience the aud claim must contain
     *                        (empty = do not check aud). Distinguishes token
     *                        types (an access token's aud is a resource, not
     *                        the client an id_token carries).
     * @return Ok, or the first failing reason. Never throws.
     */
    JwtVerificationResult verifyJwt(
      const std::string &jwt,
      const std::string &expectedIssuer,
      long long nowSecs,
      const std::string &expectedAudience = {}
    ) const;

    /**
     * @brief verifyJwt + the verified payload in one pass (#87 L2).
     *
     * Avoids the second base64+JSON decode callers otherwise perform on an
     * already-verified token. Returns the payload when verification is Ok,
     * nullopt otherwise; when `rejectionReason` is non-null it always
     * receives the verification outcome (Ok on success). Never throws.
     */
    std::optional<Json::Value> verifyAndDecode(
      const std::string &jwt,
      const std::string &expectedIssuer,
      long long nowSecs,
      const std::string &expectedAudience = {},
      JwtVerificationResult *rejectionReason = nullptr
    ) const;

    /**
     * @brief Get the JWKS (JSON Web Key Set) containing public key(s).
     * @return JSON object with "keys" array.
     */
    Json::Value getJwks() const;

    /// Get the current key ID.
    const std::string &getKeyId() const;

    /// Check if manager is initialized with a valid key.
    bool isInitialized() const;

  private:
    /// One loaded signing key. `pkey` is an EVP_PKEY* kept opaque (void*) so
    /// the public header does not pull OpenSSL in -- same convention as the
    /// pre-#110 single `rsaKey_` member.
    struct KeyEntry
    {
        std::string kid;
        void *pkey = nullptr;  // EVP_PKEY* (owned)
    };

    struct KeySet;

    std::atomic<fulla::common::ports::ILogger *> logger_{nullptr};  // non-owning
    mutable std::mutex initMutex_;
    std::shared_ptr<const KeySet> keySet_;

    /// Find a loaded entry by kid; nullptr when unknown.
    const KeyEntry *findEntry(const KeySet &keySet, const std::string &kid) const;

    bool generateEphemeralKey(KeyEntry &entry);
    /// Parse one PEM into `entry.pkey`; returns false on parse failure
    /// (kid must be set by the caller).
    bool loadPemInto(KeyEntry &entry, const std::string &pemData);
    /// #110-B: load <kid>.pem files + the active_kid marker from `dir`.
    /// Returns false (with keySet untouched) on any structural error.
    bool loadKeystoreDir(KeySet &keySet, const std::string &dir);

    static std::string base64UrlEncode(const unsigned char *data, size_t len);
    static std::string base64UrlEncode(const std::string &data);
    /// Strict base64url decode (no padding, '-'/'_' alphabet). Returns false
    /// on any non-alphabet character or impossible length; output is cleared
    /// on failure. Domain-layer constraint: hand-rolled, no drogon/3rd-party.
    static bool base64UrlDecode(const std::string &input, std::string &output);

    /// n/e components of one key's public half (empty-then-false on failure).
    bool getPublicKeyComponents(const KeyEntry &entry, std::string &n, std::string &e) const;

    /// Verify the RS256 signature of `signingInput` against one key.
    static bool verifyRs256Signature(
      const void *pkey,
      const std::string &signingInput,
      const std::string &signature
    );

    /// Shared verification core of verifyJwt()/verifyAndDecode(): runs the
    /// full check chain; when verifiedPayloadOut is non-null and every check
    /// passes, receives the decoded payload (left untouched on rejection).
    JwtVerificationResult verifyCore(
      const std::string &jwt,
      const std::string &expectedIssuer,
      long long nowSecs,
      const std::string &expectedAudience,
      Json::Value *verifiedPayloadOut
    ) const;

    /// Log through the injected logger_ if set; no-op if nullptr.
    void log(fulla::common::ports::LogLevel level, const std::string &message) const;
};

}  // namespace fulla::oauth2
