#pragma once

// #42 Phase 2 (postgres-redis-cache-design.md §5.2/§5.4): a Redis-backed
// cache decorator that wraps an ITokenRepository (the Postgres impl in
// production) and serves the two hottest, invalidation-safe token reads from
// Redis: getAccessToken (bearer validation hot path) and introspectToken (RFC
// 7662). All other methods are pure pass-through to the wrapped impl.
//
// Correctness constraints (design §5.4 + PR #47 subagent review):
//  - C1: getAccessToken consults the token:revoked:{hash} negative cache on
//    EVERY return (hit + before fill), so a revoked token is never served as
//    valid. Without this the cache would serve a revoked token for up to the
//    60s TTL — a security hole on the bearer-validation path.
//  - N2: introspectToken caches its result ONLY when result.active AND a
//    token:access:{hash} entry exists (the EXISTS-based discriminator).
//    PostgresTokenRepository::introspectToken falls through to refresh_tokens
//    on an access-token miss; a blind cache would persist refresh-token
//    introspections that revokeRefreshToken never invalidates. The access
//    cache key can only be populated by getAccessToken/saveAccessToken, which
//    are access-token-only — so its presence proves the token is an access
//    token.
//  - C7: the absolute fill budget starts before the read; it is capped by
//    remaining lifetime and the configured maximum (default 60s). Lua checks
//    revocation and deadline atomically with EXAT, including delayed fills.
//  - revokeAccessToken sets token:revoked:{hash} (at least the TTL cap) BEFORE
//    DEL-ing token:access/token:introspect, so a re-population race is
//    corrected on the next read (negative-cache-before-DEL, §5.4).
//  - G1: revokeTokenFamily is a pure pass-through (TTL-bounded convergence,
//    §10.5); family-revoked access tokens may be served stale for ≤60s.
//  - Soft-fail + atomic once-guard + IMetrics, mirroring Phase 1's
//    RedisCachedClientRepository.
#include <fulla/common/ports/IMetrics.h>
#include <fulla/oauth2/repository/ITokenRepository.h>

#include <drogon/nosql/RedisClient.h>

#include <atomic>
#include <memory>
#include <string>

namespace fulla::storage::redis
{

using RedisCachedTokenRepositoryBase = ::fulla::oauth2::repository::ITokenRepository;

/**
 * @brief Redis L2 cache decorator for ITokenRepository (design #42 Phase 2).
 *
 * Caches getAccessToken + introspectToken (access-token-only, via the N2
 * discriminator). Initial errors/misses use the authoritative repository;
 * guarded-fill failures and the five-second local deadline reject the read.
 * Completion fires once and releases the callback even if Redis queues forever.
 */
class RedisCachedTokenRepository : public RedisCachedTokenRepositoryBase,
                                   public std::enable_shared_from_this<RedisCachedTokenRepository>
{
  public:
    void inspectRefreshToken(
      const std::string &token,
      const std::string &clientId,
      RefreshResultCallback &&cb
    ) override
    {
        impl_->inspectRefreshToken(token, clientId, std::move(cb));
    }

    void rotateRefreshToken(
      const ::fulla::oauth2::model::OAuth2RefreshToken &expected,
      const ::fulla::oauth2::model::OAuth2AccessToken &at,
      const ::fulla::oauth2::model::OAuth2RefreshToken &rt,
      RefreshResultCallback &&cb
    ) override
    {
        impl_->rotateRefreshToken(expected, at, rt, std::move(cb));
    }

    void revokeTokenFamilyChecked(const std::string &familyId, SaveResultCallback &&cb) override
    {
        impl_->revokeTokenFamilyChecked(familyId, std::move(cb));
    }
    RedisCachedTokenRepository(
      std::shared_ptr<RedisCachedTokenRepositoryBase> impl,
      ::drogon::nosql::RedisClientPtr redisClient,
      std::shared_ptr<::fulla::common::ports::IMetrics> metrics = nullptr,
      int accessTokenMaxTtlSeconds = 60
    );

    // ========== Cached read paths (Phase 2) ==========
    void getAccessToken(const std::string &token, AccessTokenCallback &&cb) override;
    void introspectToken(const std::string &token, TokenIntrospectionCallback &&cb) override;

    // ========== Invalidation hooks ==========
    void revokeAccessToken(
      const std::string &token,
      const std::string &revokedBy,
      VoidCallback &&cb
    ) override;

    // ========== Cache warming (optional) ==========
    void saveAccessToken(
      const ::fulla::oauth2::model::OAuth2AccessToken &token,
      VoidCallback &&cb
    ) override;

    // ========== Pass-through (not cached per §5.2) ==========
    void saveTokenPair(
      const ::fulla::oauth2::model::OAuth2AccessToken &at,
      const ::fulla::oauth2::model::OAuth2RefreshToken &rt,
      SaveResultCallback &&cb
    ) override
    {
        impl_->saveTokenPair(at, rt, std::move(cb));
    }
    void saveRefreshToken(
      const ::fulla::oauth2::model::OAuth2RefreshToken &token,
      VoidCallback &&cb
    ) override
    {
        impl_->saveRefreshToken(token, std::move(cb));
    }
    void getRefreshToken(const std::string &token, RefreshTokenCallback &&cb) override
    {
        impl_->getRefreshToken(token, std::move(cb));
    }
    void revokeRefreshToken(const std::string &token, VoidCallback &&cb) override
    {
        impl_->revokeRefreshToken(token, std::move(cb));
    }
    void atomicRevokeRefreshToken(const std::string &token, RefreshTokenCallback &&cb) override
    {
        impl_->atomicRevokeRefreshToken(token, std::move(cb));
    }
    void revokeTokenFamily(const std::string &familyId, VoidCallback &&cb) override
    {
        // G1 (§10.5): pure pass-through. Family-revoked access tokens remain
        // cacheable for ≤ the 60s access-token TTL cap. The locked decision
        // accepts this bounded convergence window rather than maintaining a
        // familyId → {hash} index.
        impl_->revokeTokenFamily(familyId, std::move(cb));
    }
    void incrementIntrospectCount(const std::string &token, VoidCallback &&cb) override
    {
        // §5.4: not cached — the count is best-effort read-modify-write.
        impl_->incrementIntrospectCount(token, std::move(cb));
    }
    void purgeExpired() override
    {
        impl_->purgeExpired();
    }
    bool supportsTransactions() const override
    {
        return impl_->supportsTransactions();
    }
    bool supportsCas() const override
    {
        return impl_->supportsCas();
    }

  private:
    std::shared_ptr<RedisCachedTokenRepositoryBase> impl_;
    ::drogon::nosql::RedisClientPtr redisClient_;
    std::shared_ptr<::fulla::common::ports::IMetrics> metrics_;
    int accessTokenMaxTtlSeconds_;

    // Emit a cache counter (hit/miss/error) if metrics_ is wired.
    void emitMetric(const char *outcome) const;
};

}  // namespace fulla::storage::redis
