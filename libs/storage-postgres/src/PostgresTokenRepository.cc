#include <fulla/storage/postgres/PostgresTokenRepository.h>
#include <drogon/drogon.h>

#include <fulla/storage/postgres/models/Oauth2AccessTokens.h>
#include <fulla/storage/postgres/models/Oauth2RefreshTokens.h>

#include <chrono>
#include <ctime>

namespace fulla::storage::postgres
{

// Task 27.5: callback + DTO aliases for the new base interface; safe at namespace scope here (this
// .cc does not include IOAuth2Storage.h, so no oauth2::* clash).
using OAuth2AccessToken = ::fulla::oauth2::model::OAuth2AccessToken;
using OAuth2RefreshToken = ::fulla::oauth2::model::OAuth2RefreshToken;
using TokenIntrospection = ::fulla::oauth2::model::TokenIntrospection;
using VoidCallback = ITokenRepositoryBase::VoidCallback;
using SaveResultCallback = ITokenRepositoryBase::SaveResultCallback;
using AccessTokenCallback = ITokenRepositoryBase::AccessTokenCallback;
using RefreshTokenCallback = ITokenRepositoryBase::RefreshTokenCallback;
using TokenIntrospectionCallback = ITokenRepositoryBase::TokenIntrospectionCallback;

using namespace ::drogon::orm;
using namespace drogon_model::fulla_db;

void PostgresTokenRepository::saveAccessToken(const OAuth2AccessToken &token, VoidCallback &&cb)
{
    if (!dbClientMaster_)
    {
        if (cb)
            cb();
        return;
    }
    auto sharedCb = std::make_shared<VoidCallback>(std::move(cb));
    try
    {
        Mapper<Oauth2AccessTokens> mapper(dbClientMaster_);
        Oauth2AccessTokens newToken;
        newToken.setToken(token.token);
        newToken.setClientId(token.clientId);
        newToken.setUserId(token.userId);
        newToken.setScope(token.scope);
        newToken.setExpiresAt(token.expiresAt);
        newToken.setRevoked(token.revoked);
        // F-016: persist the issuer stamped at issuance (previously never
        // written, so every row carried the schema's hardcoded default).
        newToken.setIssuer(token.issuer);
        // v1.5.0 M1 (design §2.1 item 4): the org binding inherited from
        // the code. V036 column; absent -> NULL.
        if (token.orgId.has_value())
        {
            newToken.setOrgId(*token.orgId);
        }

        mapper.insert(
          newToken,
          [sharedCb](const Oauth2AccessTokens &) {
              if (*sharedCb)
                  (*sharedCb)();
          },
          [sharedCb](const DrogonDbException &e) {
              LOG_ERROR << "saveAccessToken Error: " << e.base().what();
              if (*sharedCb)
                  (*sharedCb)();
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "saveAccessToken Exception: " << e.what();
        if (*sharedCb)
            (*sharedCb)();
    }
    catch (...)
    {
        LOG_ERROR << "saveAccessToken Unknown Exception";
        if (*sharedCb)
            (*sharedCb)();
    }
}

void PostgresTokenRepository::saveTokenPair(
  const OAuth2AccessToken &at,
  const OAuth2RefreshToken &rt,
  SaveResultCallback &&cb
)
{
    if (!dbClientMaster_)
    {
        // Failure: no backend to persist into. Reporting false (not true,
        // and not silently dropping the callback) is exactly the
        // ITokenRepository::SaveResultCallback contract.
        if (cb)
            cb(false);
        return;
    }
    auto sharedCb = std::make_shared<SaveResultCallback>(std::move(cb));

    // Exemption (db-operations.md §3): Documented batch operation.
    // saveTokenPair inserts both access and refresh tokens within a single
    // database transaction.  The commit callback ensures the caller is
    // notified only after COMMIT (not merely after INSERT), which is
    // critical for correctness (see inline comment for the race-condition
    // analysis).  Splitting into two Mapper::insert calls would lose
    // transactional atomicity.

    // Bug fix: the caller's callback must not fire until the transaction
    // has actually been COMMITted. Drogon only issues "commit" to Postgres
    // when the last shared_ptr reference to the Transaction object is
    // released (see drogon::orm::TransactionImpl::~TransactionImpl), which
    // happens asynchronously, some time after the second INSERT's own
    // result callback returns. The previous implementation called the
    // caller's callback directly from that second INSERT's success
    // callback -- i.e. before COMMIT was even sent. A second connection
    // (e.g. this repository's own dbClientReader_, or a contract test using
    // a separate client) reading immediately afterwards could observe
    // pre-commit state under Postgres MVCC and see neither row. This was
    // intermittent (a race between the async COMMIT round trip and a
    // subsequent read on a different connection), which is why it only
    // reproduced under some CI runners/timings and not deterministically
    // in every environment.
    //
    // Fix: install a commit callback on the transaction and fire the
    // caller's callback from there (guaranteed to run only after COMMIT
    // completes). The insert error paths still invoke the caller's
    // callback directly, because Drogon automatically rolls back on error
    // and its own documentation states the commit callback "will never be
    // executed" if the transaction is rolled back.
    auto invoked = std::make_shared<bool>(false);
    // ok must be true ONLY when the transaction actually committed. Every
    // failure path below reports false so callers (token endpoint,
    // GitHub/social login) can surface a real error instead of handing
    // out tokens that were never persisted.
    auto invokeOnce = [sharedCb, invoked](bool ok) {
        if (!*invoked)
        {
            *invoked = true;
            if (*sharedCb)
                (*sharedCb)(ok);
        }
    };

    // Deadlock fix: the transaction must be acquired asynchronously.
    // saveTokenPair is reached from inside DB result callbacks (e.g. the
    // token endpoint's device-code flow issues tokens from within
    // PgConnection::handleRead on a DbLoop thread). The blocking
    // newTransaction() overload waits on a future that is only fulfilled by
    // the DB event loops themselves, so calling it there stalls the loop it
    // runs on; with every DbLoop blocked this way (two concurrent
    // redemptions), BEGIN can never be sent and the whole process
    // deadlocks. newTransactionAsync() delivers the transaction via
    // callback without ever blocking the calling loop.
    dbClientMaster_->newTransactionAsync(
      [invokeOnce, at, rt](const std::shared_ptr<Transaction> &transPtr) {
          if (!transPtr)
          {
              LOG_ERROR << "saveTokenPair: failed to acquire transaction (timeout)";
              invokeOnce(false);
              return;
          }

          // Use a transaction to ensure both tokens are saved atomically
          transPtr->setCommitCallback([invokeOnce](bool committed) {
              if (!committed)
                  LOG_ERROR << "saveTokenPair: transaction commit failed";
              invokeOnce(committed);
          });

          auto refreshInsertErrorCb = [invokeOnce](const DrogonDbException &e) {
              LOG_ERROR << "saveTokenPair (refresh) failed: " << e.base().what();
              invokeOnce(false);
          };

          // v1.5.0 M1: the refresh insert branches on family_id (pre-
          // existing) AND org_id (new) -- raw SQL placeholders are a
          // compile-time arg list, so each combination gets its own
          // statement. Kept in one lambda so the access insert below has a
          // single continuation regardless.
          auto insertRefresh = [transPtr, rt, refreshInsertErrorCb]() {
              const bool hasFamily = !rt.familyId.empty();
              const bool hasOrg = rt.orgId.has_value();
              if (hasFamily && hasOrg)
              {
                  transPtr->execSqlAsync(
                    "INSERT INTO oauth2_refresh_tokens "
                    "(token, access_token, client_id, user_id, scope, expires_at, revoked, "
                    "family_id, org_id) "
                    "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)",
                    [](const ::drogon::orm::Result &) {},
                    refreshInsertErrorCb,
                    rt.token,
                    rt.accessToken,
                    rt.clientId,
                    rt.userId,
                    rt.scope,
                    rt.expiresAt,
                    rt.revoked,
                    rt.familyId,
                    *rt.orgId
                  );
              }
              else if (hasFamily)
              {
                  transPtr->execSqlAsync(
                    "INSERT INTO oauth2_refresh_tokens "
                    "(token, access_token, client_id, user_id, scope, expires_at, revoked, "
                    "family_id) "
                    "VALUES ($1, $2, $3, $4, $5, $6, $7, $8)",
                    [](const ::drogon::orm::Result &) {},
                    refreshInsertErrorCb,
                    rt.token,
                    rt.accessToken,
                    rt.clientId,
                    rt.userId,
                    rt.scope,
                    rt.expiresAt,
                    rt.revoked,
                    rt.familyId
                  );
              }
              else if (hasOrg)
              {
                  transPtr->execSqlAsync(
                    "INSERT INTO oauth2_refresh_tokens "
                    "(token, access_token, client_id, user_id, scope, expires_at, revoked, "
                    "org_id) "
                    "VALUES ($1, $2, $3, $4, $5, $6, $7, $8)",
                    [](const ::drogon::orm::Result &) {},
                    refreshInsertErrorCb,
                    rt.token,
                    rt.accessToken,
                    rt.clientId,
                    rt.userId,
                    rt.scope,
                    rt.expiresAt,
                    rt.revoked,
                    *rt.orgId
                  );
              }
              else
              {
                  transPtr->execSqlAsync(
                    "INSERT INTO oauth2_refresh_tokens "
                    "(token, access_token, client_id, user_id, scope, expires_at, revoked) "
                    "VALUES ($1, $2, $3, $4, $5, $6, $7)",
                    [](const ::drogon::orm::Result &) {},
                    refreshInsertErrorCb,
                    rt.token,
                    rt.accessToken,
                    rt.clientId,
                    rt.userId,
                    rt.scope,
                    rt.expiresAt,
                    rt.revoked
                  );
              }
          };

          auto accessInsertErrorCb = [invokeOnce](const DrogonDbException &e) {
              LOG_ERROR << "saveTokenPair (access) failed: " << e.base().what();
              invokeOnce(false);
          };

          if (at.orgId.has_value())
          {
              transPtr->execSqlAsync(
                "INSERT INTO oauth2_access_tokens (token, client_id, user_id, scope, "
                "expires_at, revoked, issuer, org_id) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8)",
                [insertRefresh](const ::drogon::orm::Result &) {
                    // Access token saved, now save refresh token. The
                    // caller's callback fires from the commit callback above,
                    // not here.
                    insertRefresh();
                },
                accessInsertErrorCb,
                at.token,
                at.clientId,
                at.userId,
                at.scope,
                at.expiresAt,
                at.revoked,
                at.issuer,
                *at.orgId
              );
          }
          else
          {
              transPtr->execSqlAsync(
                "INSERT INTO oauth2_access_tokens (token, client_id, user_id, scope, "
                "expires_at, revoked, issuer) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7)",
                [insertRefresh](const ::drogon::orm::Result &) {
                    // Access token saved, now save refresh token. The
                    // caller's callback fires from the commit callback above,
                    // not here.
                    insertRefresh();
                },
                accessInsertErrorCb,
                at.token,
                at.clientId,
                at.userId,
                at.scope,
                at.expiresAt,
                at.revoked,
                at.issuer  // F-016: write the issuance-time issuer, not the schema default
              );
          }
      }
    );
}

void PostgresTokenRepository::getAccessToken(const std::string &token, AccessTokenCallback &&cb)
{
    if (!dbClientReader_)
    {
        cb(std::nullopt);
        return;
    }
    auto sharedCb = std::make_shared<AccessTokenCallback>(std::move(cb));
    try
    {
        Mapper<Oauth2AccessTokens> mapper(dbClientReader_);
        mapper.findOne(
          Criteria(Oauth2AccessTokens::Cols::_token, CompareOperator::EQ, token),
          [sharedCb](const Oauth2AccessTokens &row) {
              OAuth2AccessToken t;
              t.token = row.getValueOfToken();
              t.clientId = row.getValueOfClientId();
              t.userId = row.getValueOfUserId();
              t.scope = row.getValueOfScope();
              t.expiresAt = row.getValueOfExpiresAt();
              t.revoked = row.getValueOfRevoked();
              // v1.5.0 M1: org binding round-trips (NULL -> nullopt).
              t.orgId = row.getOrgId() ? std::optional<int32_t>(*row.getOrgId())
                                       : std::nullopt;
              (*sharedCb)(t);
          },
          [sharedCb](const DrogonDbException &e) {
              LOG_WARN << "getAccessToken not found/error: " << e.base().what();
              (*sharedCb)(std::nullopt);
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "getAccessToken Exception: " << e.what();
        (*sharedCb)(std::nullopt);
    }
    catch (...)
    {
        LOG_ERROR << "getAccessToken Unknown Exception";
        (*sharedCb)(std::nullopt);
    }
}

void PostgresTokenRepository::saveRefreshToken(const OAuth2RefreshToken &token, VoidCallback &&cb)
{
    if (!dbClientMaster_)
    {
        if (cb)
            cb();
        return;
    }
    auto sharedCb = std::make_shared<VoidCallback>(std::move(cb));
    try
    {
        Mapper<Oauth2RefreshTokens> mapper(dbClientMaster_);
        Oauth2RefreshTokens newToken;
        newToken.setToken(token.token);
        newToken.setAccessToken(token.accessToken);
        newToken.setClientId(token.clientId);
        newToken.setUserId(token.userId);
        newToken.setScope(token.scope);
        newToken.setExpiresAt(token.expiresAt);
        newToken.setRevoked(token.revoked);
        if (!token.familyId.empty())
            newToken.setFamilyId(token.familyId);

        mapper.insert(
          newToken,
          [sharedCb](const Oauth2RefreshTokens &) {
              if (*sharedCb)
                  (*sharedCb)();
          },
          [sharedCb](const DrogonDbException &e) {
              LOG_ERROR << "saveRefreshToken Error: " << e.base().what();
              if (*sharedCb)
                  (*sharedCb)();
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "saveRefreshToken Exception: " << e.what();
        if (*sharedCb)
            (*sharedCb)();
    }
    catch (...)
    {
        LOG_ERROR << "saveRefreshToken Unknown Exception";
        if (*sharedCb)
            (*sharedCb)();
    }
}

void PostgresTokenRepository::getRefreshToken(const std::string &token, RefreshTokenCallback &&cb)
{
    if (!dbClientReader_)
    {
        cb(std::nullopt);
        return;
    }
    auto sharedCb = std::make_shared<RefreshTokenCallback>(std::move(cb));
    try
    {
        Mapper<Oauth2RefreshTokens> mapper(dbClientReader_);
        mapper.findOne(
          Criteria(Oauth2RefreshTokens::Cols::_token, CompareOperator::EQ, token),
          [sharedCb](const Oauth2RefreshTokens &row) {
              OAuth2RefreshToken t;
              t.token = row.getValueOfToken();
              t.accessToken = row.getValueOfAccessToken();
              t.clientId = row.getValueOfClientId();
              t.userId = row.getValueOfUserId();
              t.scope = row.getValueOfScope();
              t.expiresAt = row.getValueOfExpiresAt();
              t.revoked = row.getValueOfRevoked();
              t.familyId = row.getValueOfFamilyId();
              // v1.5.0 M1: org binding round-trips (NULL -> nullopt).
              t.orgId = row.getOrgId() ? std::optional<int32_t>(*row.getOrgId())
                                       : std::nullopt;
              (*sharedCb)(t);
          },
          [sharedCb](const DrogonDbException &e) {
              LOG_WARN << "getRefreshToken not found/error: " << e.base().what();
              (*sharedCb)(std::nullopt);
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "getRefreshToken Exception: " << e.what();
        (*sharedCb)(std::nullopt);
    }
    catch (...)
    {
        LOG_ERROR << "getRefreshToken Unknown Exception";
        (*sharedCb)(std::nullopt);
    }
}

void PostgresTokenRepository::revokeRefreshToken(const std::string &token, VoidCallback &&cb)
{
    if (!dbClientMaster_)
    {
        if (cb)
            cb();
        return;
    }
    auto sharedCb = std::make_shared<VoidCallback>(std::move(cb));
    try
    {
        Mapper<Oauth2RefreshTokens> mapper(dbClientMaster_);
        Oauth2RefreshTokens updateObj;
        updateObj.setToken(token);
        updateObj.setRevoked(true);

        mapper.update(
          updateObj,
          [sharedCb, token](const size_t count) {
              LOG_INFO << "Revoked refresh token: " << token << ", affected rows: " << count;
              if (*sharedCb)
                  (*sharedCb)();
          },
          [sharedCb, token](const DrogonDbException &e) {
              LOG_ERROR << "Failed to revoke refresh token: " << token
                        << ", error: " << e.base().what();
              if (*sharedCb)
                  (*sharedCb)();
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "revokeRefreshToken Exception: " << e.what();
        if (*sharedCb)
            (*sharedCb)();
    }
    catch (...)
    {
        LOG_ERROR << "revokeRefreshToken Unknown Exception";
        if (*sharedCb)
            (*sharedCb)();
    }
}

void PostgresTokenRepository::atomicRevokeRefreshToken(
  const std::string &token,
  RefreshTokenCallback &&cb
)
{
    if (!dbClientMaster_)
    {
        cb(std::nullopt);
        return;
    }
    auto sharedCb = std::make_shared<RefreshTokenCallback>(std::move(cb));

    // Atomic CAS: UPDATE ... WHERE revoked=false RETURNING *
    dbClientMaster_->execSqlAsync(
        "UPDATE oauth2_refresh_tokens SET revoked = true "
        "WHERE token = $1 AND revoked = false "
        "RETURNING token, access_token, client_id, user_id, scope, expires_at, family_id, "
        "org_id",
      [sharedCb](const ::drogon::orm::Result &r) {
          if (r.empty())
          {
              // Already revoked or not found -> reuse detected
              (*sharedCb)(std::nullopt);
              return;
          }
          auto row = r[0];
          OAuth2RefreshToken rt;
          rt.token = row["token"].as<std::string>();
          rt.accessToken = row["access_token"].as<std::string>();
          rt.clientId = row["client_id"].as<std::string>();
          rt.userId = row["user_id"].as<std::string>();
          rt.scope = row["scope"].isNull() ? "" : row["scope"].as<std::string>();
          rt.expiresAt = row["expires_at"].as<int64_t>();
          rt.familyId = row["family_id"].isNull() ? "" : row["family_id"].as<std::string>();
          // v1.5.0 M1: the org binding must survive the rotation read.
          rt.orgId =
            row["org_id"].isNull() ? std::nullopt
                                   : std::optional<int32_t>(row["org_id"].as<int32_t>());
          rt.revoked = true;
          (*sharedCb)(rt);
      },
      [sharedCb](const DrogonDbException &e) {
          LOG_ERROR << "atomicRevokeRefreshToken error: " << e.base().what();
          (*sharedCb)(std::nullopt);
      },
      token
    );
}

void PostgresTokenRepository::revokeTokenFamily(const std::string &familyId, VoidCallback &&cb)
{
    if (!dbClientMaster_ || familyId.empty())
    {
        if (cb)
            cb();
        return;
    }
    auto sharedCb = std::make_shared<VoidCallback>(std::move(cb));

    // Exemption (db-operations.md §3): Documented batch operation.
    // Security-critical cascade revoke of an entire token family in 2 queries
    // (refresh tokens + access tokens via sub-select).  Splitting into
    // individual Mapper::update calls would be both incorrect (non-atomic)
    // and inefficient (N+M round-trips vs 2).

    // Revoke all refresh tokens in the family
    dbClientMaster_->execSqlAsync(
      "UPDATE oauth2_refresh_tokens SET revoked = true WHERE family_id = $1",
      [sharedCb, familyId, self = shared_from_this(), this](const ::drogon::orm::Result &) {
          // Also revoke all associated access tokens
          dbClientMaster_->execSqlAsync(
            "UPDATE oauth2_access_tokens SET revoked = true "
            "WHERE token IN (SELECT access_token FROM oauth2_refresh_tokens WHERE family_id = $1)",
            [sharedCb, familyId](const ::drogon::orm::Result &) {
                LOG_WARN << "[SECURITY] Token family cascade-revoked: " << familyId;
                if (*sharedCb)
                    (*sharedCb)();
            },
            [sharedCb](const DrogonDbException &e) {
                LOG_ERROR << "revokeTokenFamily (access tokens) error: " << e.base().what();
                if (*sharedCb)
                    (*sharedCb)();
            },
            familyId
          );
      },
      [sharedCb](const DrogonDbException &e) {
          LOG_ERROR << "revokeTokenFamily error: " << e.base().what();
          if (*sharedCb)
              (*sharedCb)();
      },
      familyId
    );
}

// ========== P1: Token Introspection (RFC 7662) ==========

void PostgresTokenRepository::introspectToken(
  const std::string &token,
  TokenIntrospectionCallback &&cb
)
{
    if (!dbClientReader_)
    {
        TokenIntrospection introspection;
        introspection.active = false;
        cb(introspection);
        return;
    }

    auto sharedCb = std::make_shared<TokenIntrospectionCallback>(std::move(cb));
    int64_t now = std::time(nullptr);

    // 查询 access tokens 表
    Mapper<Oauth2AccessTokens> atMapper(dbClientReader_);
    atMapper.findOne(
      Criteria(Oauth2AccessTokens::Cols::_token, CompareOperator::EQ, token),
      [sharedCb, now, token, self = shared_from_this()](
        const Oauth2AccessTokens &accessToken
      ) {
          // 检查是否吊销或过期
          bool revoked = accessToken.getValueOfRevoked();
          int64_t expiresAt = accessToken.getValueOfExpiresAt();

          if (revoked || expiresAt < now)
          {
              TokenIntrospection introspection;
              introspection.active = false;
              (*sharedCb)(introspection);
              return;
          }

          TokenIntrospection introspection;
          introspection.active = true;
          introspection.clientId = accessToken.getValueOfClientId();
          introspection.tokenType = "Bearer";
          introspection.exp = expiresAt;
          introspection.iat = accessToken.getValueOfIssuedAt();
          introspection.iss = accessToken.getValueOfIssuer();
          introspection.aud = accessToken.getValueOfAudience();
          introspection.nbf = accessToken.getValueOfNotBefore();
          introspection.sub = accessToken.getValueOfUserId();
          introspection.scope = accessToken.getValueOfScope();
          // v1.5.0 M1: expose the token's org binding (design §2.1 item 4).
          introspection.orgId =
            accessToken.getOrgId() ? std::optional<int32_t>(*accessToken.getOrgId())
                                   : std::nullopt;
          (*sharedCb)(introspection);
      },
      [sharedCb, now, token, self = shared_from_this(), this](const DrogonDbException &) {
          // 未在 access tokens 中找到，尝试 refresh tokens
          Mapper<Oauth2RefreshTokens> rtMapper(dbClientReader_);
          rtMapper.findOne(
            Criteria(Oauth2RefreshTokens::Cols::_token, CompareOperator::EQ, token),
            [sharedCb, now](const Oauth2RefreshTokens &refreshToken) {
                bool revoked = refreshToken.getValueOfRevoked();
                int64_t expiresAt = refreshToken.getValueOfExpiresAt();

                if (revoked || expiresAt < now)
                {
                    TokenIntrospection introspection;
                    introspection.active = false;
                    (*sharedCb)(introspection);
                    return;
                }

                TokenIntrospection introspection;
                introspection.active = true;
                introspection.clientId = refreshToken.getValueOfClientId();
                introspection.tokenType = "Bearer";
                introspection.exp = expiresAt;
                // P2-7 audit fix: a refresh token has no iat/nbf on its row;
                // the previous response fabricated iat=now/nbf=now. Omit
                // both (RFC 7662: claims the server has no knowledge of are
                // simply absent).
                introspection.iat = 0;
                introspection.iss = "";
                introspection.aud = "";
                introspection.nbf = 0;
                introspection.sub = refreshToken.getValueOfUserId();
                introspection.scope = refreshToken.getValueOfScope();
                // v1.5.0 M1: org binding on the refresh branch too.
                introspection.orgId =
                  refreshToken.getOrgId() ? std::optional<int32_t>(*refreshToken.getOrgId())
                                          : std::nullopt;
                (*sharedCb)(introspection);
            },
            [sharedCb](const DrogonDbException &) {
                TokenIntrospection introspection;
                introspection.active = false;
                (*sharedCb)(introspection);
            }
          );
      }
    );
}

void PostgresTokenRepository::incrementIntrospectCount(const std::string &token, VoidCallback &&cb)
{
    if (!dbClientMaster_)
    {
        cb();
        return;
    }

    auto sharedCb = std::make_shared<VoidCallback>(std::move(cb));

    Mapper<Oauth2AccessTokens> mapper(dbClientMaster_);
    mapper.findOne(
      Criteria(Oauth2AccessTokens::Cols::_token, CompareOperator::EQ, token),
      [sharedCb, token, self = shared_from_this()](const Oauth2AccessTokens &found) {
          Oauth2AccessTokens updated;
          updated.setToken(token);
          updated.setIntrospectCount(found.getValueOfIntrospectCount() + 1);
          Mapper<Oauth2AccessTokens>(self->dbClientMaster_)
            .update(
              updated,
              [sharedCb](const size_t) { (*sharedCb)(); },
              [sharedCb](const DrogonDbException &e) {
                  LOG_ERROR << "incrementIntrospectCount update failed: " << e.base().what();
                  (*sharedCb)();
              }
            );
      },
      [sharedCb](const DrogonDbException &e) {
          LOG_ERROR << "incrementIntrospectCount find failed: " << e.base().what();
          (*sharedCb)();
      }
    );
}

// ========== P1: Token Revocation (RFC 7009) ==========

void PostgresTokenRepository::revokeAccessToken(
  const std::string &token,
  const std::string &revokedBy,
  VoidCallback &&cb
)
{
    if (!dbClientMaster_)
    {
        cb();
        return;
    }

    auto sharedCb = std::make_shared<VoidCallback>(std::move(cb));
    int64_t now = std::time(nullptr);

    // Find the access token first
    Mapper<Oauth2AccessTokens> mapper(dbClientMaster_);
    mapper.findOne(
      Criteria(Oauth2AccessTokens::Cols::_token, CompareOperator::EQ, token),
      [sharedCb, now, revokedBy, token, self = shared_from_this()](
        const Oauth2AccessTokens & /*found*/
      ) {
          Oauth2AccessTokens updated;
          updated.setToken(token);
          updated.setRevoked(true);
          updated.setRevokedAt(now);
          updated.setRevokedBy(revokedBy);

          Mapper<Oauth2AccessTokens>(self->dbClientMaster_)
            .update(
              updated,
              [sharedCb, now, revokedBy, token, self](const size_t) {
                  // Also terminate the refresh-token FAMILY behind this
                  // access token. The incoming value is the ACCESS token's
                  // hash; oauth2_refresh_tokens stores it in the
                  // access_token column (token holds the RT's own hash), so
                  // the join must be on _access_token — matching on the
                  // token column never hits (P0-2 audit fix). Going one step
                  // further and cascading through family_id closes the
                  // logout semantics: after a refresh rotation the client's
                  // current RT belongs to the same family as the (possibly
                  // stale) AT it presents at logout, and revoking only the
                  // exact paired row would leave the rotated RT usable for
                  // up to 30 days (RFC 7009 2.1 permits — and logout
                  // semantics expect — related-token invalidation).
                  Mapper<Oauth2RefreshTokens> rtMapper(self->dbClientMaster_);
                  rtMapper.findOne(
                    Criteria(Oauth2RefreshTokens::Cols::_access_token, CompareOperator::EQ, token),
                    [sharedCb, self](const Oauth2RefreshTokens &rt) {
                        const auto familyId = rt.getFamilyId();
                        if (familyId && !familyId->empty())
                        {
                            self->revokeTokenFamily(
                              *familyId, [sharedCb]() { (*sharedCb)(); });
                            return;
                        }
                        // Pre-family rows (no family_id): revoke just the pair.
                        Oauth2RefreshTokens rtUpdated;
                        rtUpdated.setToken(rt.getValueOfToken());
                        rtUpdated.setRevoked(true);
                        Mapper<Oauth2RefreshTokens>(self->dbClientMaster_)
                          .update(
                            rtUpdated,
                            [sharedCb](const size_t) { (*sharedCb)(); },
                            [sharedCb](const DrogonDbException &) { (*sharedCb)(); }
                          );
                    },
                    [sharedCb](const DrogonDbException &) {
                        LOG_INFO << "Token revoked successfully (access token only)";
                        (*sharedCb)();
                    }
                  );
              },
              [sharedCb, token, self](const DrogonDbException &e) {
                  LOG_ERROR << "Access token revocation audit failed: " << e.base().what();
                  // Fallback: simple revoked=true without audit columns
                  Oauth2AccessTokens simpleUpdate;
                  simpleUpdate.setToken(token);
                  simpleUpdate.setRevoked(true);

                  Mapper<Oauth2AccessTokens>(self->dbClientMaster_)
                    .update(
                      simpleUpdate,
                      [sharedCb, token, self](const size_t) {
                          // P0-2 audit fix: same _access_token join as the
                          // audited path above (token column never matches
                          // an access-token hash).
                          Mapper<Oauth2RefreshTokens> rtMapper(self->dbClientMaster_);
                          rtMapper.findOne(
                            Criteria(Oauth2RefreshTokens::Cols::_access_token, CompareOperator::EQ, token),
                            [sharedCb, self](const Oauth2RefreshTokens &rt) {
                                Oauth2RefreshTokens rtSimple;
                                rtSimple.setToken(rt.getValueOfToken());
                                rtSimple.setRevoked(true);
                                Mapper<Oauth2RefreshTokens>(self->dbClientMaster_)
                                  .update(
                                    rtSimple,
                                    [sharedCb](const size_t) { (*sharedCb)(); },
                                    [sharedCb](const DrogonDbException &) { (*sharedCb)(); }
                                  );
                            },
                            [sharedCb](const DrogonDbException &) { (*sharedCb)(); }
                          );
                      },
                      [sharedCb](const DrogonDbException &) { (*sharedCb)(); }
                    );
              }
            );
      },
      [sharedCb](const DrogonDbException &) {
          // Token not found, nothing to revoke
          (*sharedCb)();
      }
    );
}

// ========== Cleanup ==========

void PostgresTokenRepository::purgeExpired()
{
    // Token-side slice of the original PostgresOAuth2Storage::deleteExpiredData():
    // access/refresh token sweeps + the archive_expired_tokens() call. The
    // auth-code sweep lives in PostgresGrantRepository::purgeExpired() instead
    // (see REPOSITORY_MAPPING.md #32 decision table).
    if (!dbClientMaster_)
        return;

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now().time_since_epoch()
    )
                 .count();

    try
    {
        // Access Tokens
        Mapper<Oauth2AccessTokens> atMapper(dbClientMaster_);
        atMapper.deleteBy(
          Criteria(Oauth2AccessTokens::Cols::_expires_at, CompareOperator::LT, now),
          [](const size_t count) {
              if (count > 0)
                  LOG_INFO << "Cleaned " << count << " expired access tokens";
          },
          [](const DrogonDbException &e) {
              LOG_ERROR << "Cleanup AccessTokens Error: " << e.base().what();
          }
        );

        // Refresh Tokens
        Mapper<Oauth2RefreshTokens> rtMapper(dbClientMaster_);
        rtMapper.deleteBy(
          Criteria(Oauth2RefreshTokens::Cols::_expires_at, CompareOperator::LT, now),
          [](const size_t count) {
              if (count > 0)
                  LOG_INFO << "Cleaned " << count << " expired refresh tokens";
          },
          [](const DrogonDbException &e) {
              LOG_ERROR << "Cleanup RefreshTokens Error: " << e.base().what();
          }
        );

        // Archive old tokens (older than 30 days)
        dbClientMaster_->execSqlAsync(
          "SELECT archive_expired_tokens(30)",
          [](const ::drogon::orm::Result &r) {
              if (!r.empty() && r[0][0].as<int>() > 0)
              {
                  LOG_INFO << "Archived " << r[0][0].as<int>() << " expired tokens";
              }
          },
          [](const DrogonDbException &e) {
              LOG_WARN << "Token archival skipped (function may not exist): " << e.base().what();
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "PostgresTokenRepository::purgeExpired Exception: " << e.what();
    }
    catch (...)
    {
        LOG_ERROR << "PostgresTokenRepository::purgeExpired Unknown Exception";
    }
}

}  // namespace fulla::storage::postgres
