#pragma once

// Task 10 (design.md §7 / REPOSITORY_MAPPING.md): split of
// MemoryOAuth2Storage into per-aggregate implementation files, mirroring the
// Task 9 Postgres split. This one implements ITokenRepository
// (REPOSITORY_MAPPING.md #7-14, #29-31, plus the token slice of #32
// deleteExpiredData -> purgeExpired, plus the
// supportsTransactions()/supportsCas() capability flags). It is ADDITIVE:
// MemoryOAuth2Storage / IOAuth2Storage are untouched and remain the
// production path used by OAuth2Plugin.cc and existing tests today.
//
// State ownership (see MemoryClientRepository.h header comment for the
// general rationale): this class owns `accessTokens_` and `refreshTokens_`
// -- the two maps every ITokenRepository method touches -- and its own
// private recursive_mutex.
#include <fulla/oauth2/repository/ITokenRepository.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fulla::storage::memory
{

// Task 27.5 (fulla-sdk-refactor): now implements the NEW Domain-layer
// interface fulla::oauth2::repository::ITokenRepository (+ the
// fulla::oauth2::model::* DTOs) instead of the legacy oauth2 one from
// IOAuth2Storage.h. Old/new DTOs are field-identical. Types are fully
// qualified (not aliased) because the legacy oauth2::OAuth2AccessToken /
// oauth2::OAuth2RefreshToken / oauth2::TokenIntrospection still coexist in
// IOAuth2Storage.h during this transition (see MemoryClientRepository.h).
using ITokenRepositoryBase = ::fulla::oauth2::repository::ITokenRepository;

/**
 * @brief In-memory implementation of ITokenRepository.
 *
 * saveTokenPair() is intentionally NOT overridden here; it uses
 * ITokenRepository's default (sequential saveAccessToken() then
 * saveRefreshToken()), exactly as MemoryOAuth2Storage never overrode
 * IOAuth2Storage::saveTokenPair either.
 *
 * Capability flags (Task 10's actual judgment call for this backend --
 * verified against this file's real locking behavior, not assumed):
 *
 *  - supportsTransactions() -> true. This is the one place Memory's answer
 *    differs from a naive "it just uses the default sequential
 *    implementation, so it must be false" read. The reason: both
 *    saveAccessToken() and saveRefreshToken() below take the SAME
 *    std::recursive_mutex (mutex_) via std::lock_guard, and -- critically --
 *    each method invokes its `cb` callback SYNCHRONOUSLY, INLINE, BEFORE
 *    the lock_guard goes out of scope (i.e. before the surrounding
 *    function returns and releases the lock). Because ITokenRepository's
 *    default saveTokenPair() calls saveAccessToken(at, continuation), and
 *    that continuation (which calls saveRefreshToken()) is invoked as `cb`
 *    from INSIDE saveAccessToken() while its lock_guard is still alive, the
 *    nested saveRefreshToken() call re-enters the SAME mutex recursively
 *    (legal because it's a recursive_mutex) WITHOUT ever releasing it in
 *    between. The net effect: from the moment saveAccessToken() takes the
 *    lock to the moment saveRefreshToken() releases it, the lock is held
 *    continuously by one thread. No other thread can ever observe a state
 *    where the access token exists but the refresh token doesn't (or
 *    vice-versa) -- which is exactly the externally-observable atomicity
 *    guarantee the capability flag promises. This is a genuine
 *    (non-rollback, but Memory writes can't fail anyway) atomicity
 *    guarantee, not a "尽力而为" approximation -- so true is the truthful
 *    answer here, unlike Redis (RedisTokenRepository declares false)
 *    where saveRefreshToken() is a no-op and there is no such lock-holding
 *    continuation.
 *
 *  - supportsCas() -> true. atomicRevokeRefreshToken() performs its
 *    find-check-revoked-then-mark-revoked sequence entirely inside one
 *    std::lock_guard scope on the same mutex_ -- no other thread can
 *    interleave between the "is it already revoked" check and the "mark it
 *    revoked" write. This is a real, in-process compare-and-swap, not an
 *    approximation.
 */
class MemoryTokenRepository : public ITokenRepositoryBase
{
  public:
    void inspectRefreshToken(
      const std::string &token,
      const std::string &clientId,
      RefreshResultCallback &&cb
    ) override;
    void rotateRefreshToken(
      const ::fulla::oauth2::model::OAuth2RefreshToken &expected,
      const ::fulla::oauth2::model::OAuth2AccessToken &at,
      const ::fulla::oauth2::model::OAuth2RefreshToken &rt,
      RefreshResultCallback &&cb
    ) override;
    void revokeTokenFamilyChecked(const std::string &familyId, SaveResultCallback &&cb) override;
    void saveAccessToken(
      const ::fulla::oauth2::model::OAuth2AccessToken &token,
      VoidCallback &&cb
    ) override;
    void getAccessToken(const std::string &token, AccessTokenCallback &&cb) override;

    void saveRefreshToken(
      const ::fulla::oauth2::model::OAuth2RefreshToken &token,
      VoidCallback &&cb
    ) override;
    void getRefreshToken(const std::string &token, RefreshTokenCallback &&cb) override;
    void revokeRefreshToken(const std::string &token, VoidCallback &&cb) override;
    void atomicRevokeRefreshToken(const std::string &token, RefreshTokenCallback &&cb) override;
    void revokeTokenFamily(const std::string &familyId, VoidCallback &&cb) override;

    void introspectToken(const std::string &token, TokenIntrospectionCallback &&cb) override;
    void incrementIntrospectCount(const std::string &token, VoidCallback &&cb) override;
    void revokeAccessToken(
      const std::string &token,
      const std::string &revokedBy,
      VoidCallback &&cb
    ) override;

    void purgeExpired() override;

    bool supportsTransactions() const override
    {
        return true;
    }

    bool supportsCas() const override
    {
        return true;
    }

  private:
    std::recursive_mutex mutex_;
    std::unordered_map<std::string, ::fulla::oauth2::model::OAuth2AccessToken> accessTokens_;
    std::unordered_map<std::string, ::fulla::oauth2::model::OAuth2RefreshToken> refreshTokens_;

    int64_t getCurrentTimestamp() const;
};

}  // namespace fulla::storage::memory
