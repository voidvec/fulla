#pragma once

// Task 17 slice 3 (fulla-sdk-refactor, design.md §6/§7): ports the M1
// repository interface oauth2::ITokenRepository (OAuth2Plugin/include/
// oauth2/storage/ITokenRepository.h) into fulla::oauth2::repository.
// See IClientRepository.h's header comment for the general
// decoupling-from-OAuth2Plugin rationale. Method shapes, docs, the
// saveTokenPair default-body (sequential, non-transactional) and the
// supportsTransactions()/supportsCas() capability-flag contract are
// carried over unchanged.

#include <fulla/oauth2/model/Dto.h>
#include <fulla/oauth2/repository/RefreshTokenResult.h>

#include <functional>
#include <optional>
#include <string>

namespace fulla::oauth2::repository
{

/**
 * @brief Repository for access tokens, refresh tokens, and their lifecycle
 * (introspection, revocation) -- the "token" aggregate.
 *
 * SDK consumers MUST implement this interface (design.md §7.1).
 *
 * Contract preserved from IOAuth2Storage (design.md §7.2):
 *  - saveTokenPair / revokeTokenFamily transactional atomicity is a
 *    per-implementation contract: the default implementation below runs
 *    sequentially (non-transactional); a Postgres override is expected to
 *    wrap both writes in a DB transaction.
 *  - supportsTransactions() / supportsCas() let the tiered contract test
 *    suite (Task 12) select which implementations run the atomicity/CAS
 *    tier. Every implementation MUST answer truthfully.
 */
class ITokenRepository
{
  public:
    virtual ~ITokenRepository() = default;

    using AccessTokenCallback =
      std::function<void(std::optional<fulla::oauth2::model::OAuth2AccessToken>)>;
    using RefreshTokenCallback =
      std::function<void(std::optional<fulla::oauth2::model::OAuth2RefreshToken>)>;
    using VoidCallback = std::function<void()>;
    using TokenIntrospectionCallback =
      std::function<void(std::optional<fulla::oauth2::model::TokenIntrospection>)>;
    /// Completion callback for saveTokenPair: @p ok is true only if BOTH
    /// tokens were durably persisted. Callers MUST treat ok == false as
    /// "no usable token was issued" and surface an error to the end user
    /// (returning 200 + tokens that were never stored is a silent-failure
    /// defect: subsequent introspection/refresh lookups all miss).
    using SaveResultCallback = std::function<void(bool ok)>;
    using RefreshResultCallback = std::function<void(RefreshTokenResult)>;

    /// Authoritative refresh lookup, including used tokens. Implementations
    /// must distinguish storage failure from absence and validate the client
    /// before reporting reuse. Legacy implementations fail closed until migrated.
    virtual void inspectRefreshToken(
      const std::string &,
      const std::string &,
      RefreshResultCallback &&cb
    )
    {
        cb({RefreshTokenStatus::StorageError, std::nullopt});
    }

    /// Consume the expected old RT and persist the new pair atomically.
    /// Revalidate bindings and expiry under the same family lock as revocation.
    /// Rotated is delivered only after confirmed commit. A StorageError can
    /// represent an unknown COMMIT outcome; callers must never return tokens.
    virtual void rotateRefreshToken(
      const model::OAuth2RefreshToken &,
      const model::OAuth2AccessToken &,
      const model::OAuth2RefreshToken &,
      RefreshResultCallback &&cb
    )
    {
        cb({RefreshTokenStatus::StorageError, std::nullopt});
    }

    /// Revoke both token kinds under the rotation family lock, confirming commit.
    virtual void revokeTokenFamilyChecked(const std::string &, SaveResultCallback &&cb)
    {
        cb(false);
    }

    // ========== Access Token Operations ==========

    /// Save a new access token. Original: IOAuth2Storage::saveAccessToken.
    virtual void saveAccessToken(
      const fulla::oauth2::model::OAuth2AccessToken &token,
      VoidCallback &&cb
    ) = 0;

    /// Get access token by token value. Original: IOAuth2Storage::getAccessToken.
    virtual void getAccessToken(const std::string &token, AccessTokenCallback &&cb) = 0;

    // ========== Token Pair Operations (Transactional) ==========

    /**
     * @brief Save access token + refresh token as an atomic pair.
     * Default implementation calls saveAccessToken then saveRefreshToken
     * sequentially (non-transactional), matching IOAuth2Storage's default.
     * A PostgreSQL-backed implementation should override this to use a
     * database transaction for real atomicity.
     *
     * Error reporting: the callback receives false when the pair was NOT
     * fully persisted (transaction timeout, INSERT failure, commit
     * failure, missing backend client). Implementations that override
     * this MUST report false on every failure path -- invoking the
     * callback with true after a failed write is exactly the
     * silent-failure defect this signature exists to prevent. The default
     * body reports true because saveAccessToken/saveRefreshToken carry no
     * error channel and the Memory/Redis backends never fail them.
     *
     * Original: IOAuth2Storage::saveTokenPair.
     */
    virtual void saveTokenPair(
      const fulla::oauth2::model::OAuth2AccessToken &at,
      const fulla::oauth2::model::OAuth2RefreshToken &rt,
      SaveResultCallback &&cb
    )
    {
        // Default: sequential (non-transactional) for Memory/Redis.
        saveAccessToken(at, [this, rt, cb = std::move(cb)]() mutable {
            saveRefreshToken(rt, [cb = std::move(cb)]() { cb(true); });
        });
    }

    // ========== Refresh Token Operations ==========

    /// Save a new refresh token. Original: IOAuth2Storage::saveRefreshToken.
    virtual void saveRefreshToken(
      const fulla::oauth2::model::OAuth2RefreshToken &token,
      VoidCallback &&cb
    ) = 0;

    /// Get refresh token by token value. Original: IOAuth2Storage::getRefreshToken.
    virtual void getRefreshToken(const std::string &token, RefreshTokenCallback &&cb) = 0;

    /// Revoke a refresh token. Original: IOAuth2Storage::revokeRefreshToken.
    virtual void revokeRefreshToken(const std::string &token, VoidCallback &&cb) = 0;

    /**
     * @brief Atomically revoke a refresh token (CAS operation).
     * Only revokes if the token is currently NOT revoked. Returns the token
     * data if successfully revoked, nullopt if already revoked. Used for
     * refresh token rotation to detect reuse.
     *
     * Original: IOAuth2Storage::atomicRevokeRefreshToken.
     */
    virtual void atomicRevokeRefreshToken(const std::string &token, RefreshTokenCallback &&cb) = 0;

    /**
     * @brief Revoke all tokens in a refresh token family (cascade
     * revocation). Original: IOAuth2Storage::revokeTokenFamily.
     */
    virtual void revokeTokenFamily(const std::string &familyId, VoidCallback &&cb) = 0;

    // ========== P1: Token Introspection (RFC 7662) ==========

    /// Introspect token metadata for RFC 7662 compliance.
    /// Original: IOAuth2Storage::introspectToken.
    virtual void introspectToken(const std::string &token, TokenIntrospectionCallback &&cb) = 0;

    /// Increment introspection count for monitoring.
    /// Original: IOAuth2Storage::incrementIntrospectCount.
    virtual void incrementIntrospectCount(const std::string &token, VoidCallback &&cb) = 0;

    // ========== P1: Token Revocation (RFC 7009) ==========

    /// Revoke an access token with audit trail. Original: IOAuth2Storage::revokeAccessToken.
    virtual void revokeAccessToken(
      const std::string &token,
      const std::string &revokedBy,
      VoidCallback &&cb
    ) = 0;

    // ========== Cleanup ==========

    /**
     * @brief Purge expired access tokens and refresh tokens.
     * Required pure-virtual method. Intended to be invoked by a future
     * product-level CleanupService.
     */
    virtual void purgeExpired() = 0;

    // ========== Capability flags (F5 contract tiering) ==========

    /**
     * @brief Whether this implementation provides true transactional
     * atomicity for saveTokenPair (and any other multi-write operation
     * that claims atomicity). Used by the tiered contract test suite
     * (Task 12) to decide whether to run the atomicity tier. MUST answer
     * truthfully.
     */
    virtual bool supportsTransactions() const = 0;

    /**
     * @brief Whether this implementation provides a true compare-and-swap
     * guarantee for atomicRevokeRefreshToken. Used by the tiered contract
     * test suite (Task 12) to decide whether to run the CAS tier. MUST
     * answer truthfully.
     */
    virtual bool supportsCas() const = 0;
};

}  // namespace fulla::oauth2::repository
