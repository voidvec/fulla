#pragma once

// Task 9 (design.md §7 / REPOSITORY_MAPPING.md): split of PostgresOAuth2Storage
// into per-aggregate implementation files. This one implements
// ITokenRepository (REPOSITORY_MAPPING.md #7-14, #29-31, plus the token
// slice of #32 deleteExpiredData -> purgeExpired, plus the
// supportsTransactions()/supportsCas() capability flags). It is ADDITIVE:
// PostgresOAuth2Storage / IOAuth2Storage are untouched and remain the
// production path used by OAuth2Plugin.cc today.
#include <fulla/oauth2/repository/ITokenRepository.h>
#include <fulla/storage/postgres/PostgresRepositoryBase.h>

#include <memory>

namespace fulla::storage::postgres
{

// Task 27.5: now implements the NEW Domain-layer interface
// fulla::oauth2::repository::ITokenRepository (+ fulla::oauth2::model::* DTOs) instead of
// the legacy oauth2 one. Types are qualified below (not aliased into this namespace) because the
// legacy oauth2::* DTOs still coexist in IOAuth2Storage.h during this transition.
using ITokenRepositoryBase = ::fulla::oauth2::repository::ITokenRepository;

/**
 * @brief PostgreSQL implementation of ITokenRepository.
 *
 * saveTokenPair() is overridden (not the ITokenRepository default) to use a
 * real DB transaction (drogon::orm::DbClient::newTransaction()), exactly as
 * PostgresOAuth2Storage::saveTokenPair did -- this is the "Postgres override
 * uses a database transaction for real atomicity" case the base interface's
 * doc comment describes.
 *
 * Capability flags (verified against this file's actual implementation, not
 * assumed):
 *  - supportsTransactions() -> true: saveTokenPair() wraps both inserts in a
 *    single drogon::orm::DbClient transaction (newTransaction() +
 *    execSqlAsync chained on the same transPtr), so either both writes
 *    succeed or neither does at the SQL layer.
 *  - supportsCas() -> true: atomicRevokeRefreshToken() and consumeAuthCode's
 *    sibling in PostgresGrantRepository both use a single
 *    "UPDATE ... WHERE <not-yet-revoked/used> ... RETURNING ..." statement,
 *    which is a real compare-and-swap at the database level (the WHERE
 *    clause guards against a second concurrent UPDATE seeing the row as
 *    still eligible).
 */
class PostgresTokenRepository : public ITokenRepositoryBase,
                                public PostgresRepositoryBase,
                                public std::enable_shared_from_this<PostgresTokenRepository>
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
    PostgresTokenRepository() = default;

    void saveAccessToken(
      const ::fulla::oauth2::model::OAuth2AccessToken &token,
      VoidCallback &&cb
    ) override;
    void getAccessToken(const std::string &token, AccessTokenCallback &&cb) override;

    void saveTokenPair(
      const ::fulla::oauth2::model::OAuth2AccessToken &at,
      const ::fulla::oauth2::model::OAuth2RefreshToken &rt,
      SaveResultCallback &&cb
    ) override;

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
};

}  // namespace fulla::storage::postgres
