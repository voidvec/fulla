#include <fulla/storage/postgres/PostgresTokenRepository.h>
#include <drogon/drogon.h>
#include <atomic>

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
        // v1.4.0 M1 (design §2.1 item 4): the org binding inherited from
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

namespace
{
using RefreshResult = ::fulla::oauth2::repository::RefreshTokenResult;
using RefreshStatus = ::fulla::oauth2::repository::RefreshTokenStatus;
using RefreshCallback = ITokenRepositoryBase::RefreshResultCallback;

// The shared quota-lock helper predates synchronous-throw guards in async
// continuations. Keep token failure completion local without changing the
// behavior of unrelated quota/consent callers.
void withTokenFamilyLock(
  const DbClientPtr &db,
  const std::string &key,
  std::function<void(const std::shared_ptr<Transaction> &)> locked,
  std::function<void()> failed
)
{
    if (!db)
    {
        failed();
        return;
    }
    auto onLocked = std::make_shared<decltype(locked)>(std::move(locked));
    auto onError = std::make_shared<decltype(failed)>(std::move(failed));
    try
    {
        db->newTransactionAsync([key, onLocked, onError](const auto &txn) {
            if (!txn)
            {
                (*onError)();
                return;
            }
            auto fail = [txn, onError]() {
                txn->rollback();
                (*onError)();
            };
            try
            {
                // Advisory-lock concurrency primitive; parameter-bound key.
                txn->execSqlAsync(
                  "SELECT pg_advisory_xact_lock(hashtext($1))",
                  [txn, onLocked, fail](const Result &) {
                      try
                      {
                          (*onLocked)(txn);
                      }
                      catch (const std::exception &)
                      {
                          fail();
                      }
                      catch (...)
                      {
                          fail();
                      }
                  },
                  [fail](const DrogonDbException &) { fail(); },
                  key
                );
            }
            catch (const std::exception &)
            {
                fail();
            }
            catch (...)
            {
                fail();
            }
        });
    }
    catch (const std::exception &)
    {
        (*onError)();
    }
    catch (...)
    {
        (*onError)();
    }
}

OAuth2RefreshToken refreshDto(const Oauth2RefreshTokens &row)
{
    OAuth2RefreshToken rt;
    rt.token = row.getValueOfToken();
    rt.accessToken = row.getValueOfAccessToken();
    rt.clientId = row.getValueOfClientId();
    rt.userId = row.getValueOfUserId();
    rt.scope = row.getValueOfScope();
    rt.expiresAt = row.getValueOfExpiresAt();
    rt.revoked = row.getValueOfRevoked();
    rt.familyId = row.getValueOfFamilyId();
    if (row.getOrgId())
        rt.orgId = *row.getOrgId();
    return rt;
}

void inspectRefresh(
  const DbClientPtr &db,
  const std::string &token,
  const std::string &clientId,
  RefreshCallback cb
)
{
    auto sharedCb = std::make_shared<RefreshCallback>(std::move(cb));
    if (!db)
    {
        (*sharedCb)({RefreshStatus::StorageError, std::nullopt});
        return;
    }
    try
    {
        Mapper<Oauth2RefreshTokens> mapper(db);
        mapper.findBy(
          Criteria(Oauth2RefreshTokens::Cols::_token, token),
          [sharedCb, clientId](const std::vector<Oauth2RefreshTokens> &rows) {
              if (rows.empty())
              {
                  (*sharedCb)({RefreshStatus::NotFound, std::nullopt});
                  return;
              }
              auto rt = refreshDto(rows.front());
              if (rt.clientId != clientId)
                  (*sharedCb)({RefreshStatus::ClientMismatch, std::nullopt});
              else if (!rt.revoked && rt.expiresAt <= std::time(nullptr))
                  (*sharedCb)({RefreshStatus::Expired, std::nullopt});
              else
                  (*sharedCb)(
                    {rt.revoked ? RefreshStatus::AlreadyUsed : RefreshStatus::Ready, std::move(rt)}
                  );
          },
          [sharedCb](const DrogonDbException &) {
              LOG_ERROR << "Refresh lookup failed";
              (*sharedCb)({RefreshStatus::StorageError, std::nullopt});
          }
        );
    }
    catch (const std::exception &)
    {
        (*sharedCb)({RefreshStatus::StorageError, std::nullopt});
    }
    catch (...)
    {
        (*sharedCb)({RefreshStatus::StorageError, std::nullopt});
    }
}

// Both issuance and rotation share the same transactional pair insert. A
// synchronous Mapper failure also needs rollback, unlike a SQL error which
// Drogon rolls back automatically. Completion belongs to the commit callback.
void insertPair(
  const std::shared_ptr<Transaction> &txn,
  const OAuth2AccessToken &at,
  const OAuth2RefreshToken &rt,
  const std::function<void()> &failed
)
{
    auto fail = [txn, failed]() {
        txn->rollback();
        failed();
    };
    try
    {
        Mapper<Oauth2AccessTokens> mapper(txn);
        Oauth2AccessTokens row;
        row.setToken(at.token);
        row.setClientId(at.clientId);
        row.setUserId(at.userId);
        row.setScope(at.scope);
        row.setExpiresAt(at.expiresAt);
        row.setRevoked(at.revoked);
        row.setIssuer(at.issuer);
        row.setAudience(at.audience);
        if (at.issuedAt)
            row.setIssuedAt(at.issuedAt);
        if (at.notBefore)
            row.setNotBefore(at.notBefore);
        if (at.orgId)
            row.setOrgId(*at.orgId);
        mapper.insert(
          row,
          [txn, rt, fail](const Oauth2AccessTokens &) {
              try
              {
                  Mapper<Oauth2RefreshTokens> refresh(txn);
                  Oauth2RefreshTokens next;
                  next.setToken(rt.token);
                  next.setAccessToken(rt.accessToken);
                  next.setClientId(rt.clientId);
                  next.setUserId(rt.userId);
                  next.setScope(rt.scope);
                  next.setExpiresAt(rt.expiresAt);
                  next.setRevoked(rt.revoked);
                  if (!rt.familyId.empty())
                      next.setFamilyId(rt.familyId);
                  if (rt.orgId)
                      next.setOrgId(*rt.orgId);
                  refresh.insert(
                    next,
                    [](const Oauth2RefreshTokens &) {},
                    [fail](const DrogonDbException &) { fail(); }
                  );
              }
              catch (const std::exception &)
              {
                  fail();
              }
              catch (...)
              {
                  fail();
              }
          },
          [fail](const DrogonDbException &) { fail(); }
        );
    }
    catch (const std::exception &)
    {
        fail();
    }
    catch (...)
    {
        fail();
    }
}
}  // namespace

void PostgresTokenRepository::saveTokenPair(
  const OAuth2AccessToken &at,
  const OAuth2RefreshToken &rt,
  SaveResultCallback &&cb
)
{
    auto callback = std::make_shared<SaveResultCallback>(std::move(cb));
    auto fired = std::make_shared<std::atomic<bool>>(false);
    auto finish = [callback, fired](bool ok) {
        if (!fired->exchange(true))
        {
            auto deliver = std::move(*callback);
            if (deliver)
                deliver(ok);
        }
    };
    if (!dbClientMaster_)
    {
        finish(false);
        return;
    }
    try
    {
        dbClientMaster_->newTransactionAsync([finish, at, rt](const auto &txn) {
            if (!txn)
            {
                finish(false);
                return;
            }
            txn->setCommitCallback([finish](bool ok) { finish(ok); });
            insertPair(txn, at, rt, [finish]() { finish(false); });
        });
    }
    catch (const std::exception &)
    {
        finish(false);
    }
    catch (...)
    {
        finish(false);
    }
}

void PostgresTokenRepository::inspectRefreshToken(
  const std::string &token,
  const std::string &clientId,
  RefreshResultCallback &&cb
)
{
    inspectRefresh(dbClientMaster_, token, clientId, std::move(cb));
}

void PostgresTokenRepository::rotateRefreshToken(
  const OAuth2RefreshToken &expected,
  const OAuth2AccessToken &at,
  const OAuth2RefreshToken &rt,
  RefreshResultCallback &&cb
)
{
    auto callback = std::make_shared<RefreshResultCallback>(std::move(cb));
    auto fired = std::make_shared<std::atomic<bool>>(false);
    auto finish = [callback, fired](RefreshResult result) {
        if (!fired->exchange(true))
        {
            auto deliver = std::move(*callback);
            if (deliver)
                deliver(std::move(result));
        }
    };
    auto self = shared_from_this();
    // Derive the lock domain from authoritative metadata, never the new pair.
    inspectRefresh(
      dbClientMaster_,
      expected.token,
      expected.clientId,
      [self, expected, at, rt, finish](RefreshResult first) {
          if (first.status != RefreshStatus::Ready || !first.token)
          {
              finish(std::move(first));
              return;
          }
          const auto family = first.token->familyId;
          const auto key =
            family.empty() ? "refresh-token:" + expected.token : "refresh-family:" + family;
          withTokenFamilyLock(
            self->dbClientMaster_,
            key,
            [expected, at, rt, family, finish](const auto &txn) {
                inspectRefresh(
                  txn,
                  expected.token,
                  expected.clientId,
                  [txn, expected, at, rt, family, finish](RefreshResult current) {
                      try
                      {
                          if (current.status != RefreshStatus::Ready || !current.token)
                          {
                              finish(std::move(current));
                              return;
                          }
                          const auto &old = *current.token;
                          if (
                            old.familyId != family || old.familyId != expected.familyId ||
                            old.userId != expected.userId || old.scope != expected.scope ||
                            old.orgId != expected.orgId || at.clientId != old.clientId ||
                            rt.clientId != old.clientId || at.scope != old.scope ||
                            rt.scope != old.scope || at.orgId != old.orgId ||
                            rt.orgId != old.orgId || rt.familyId != old.familyId ||
                            at.userId != rt.userId || rt.accessToken != at.token ||
                            at.token.empty() || rt.token.empty() || at.revoked || rt.revoked
                          )
                          {
                              finish({RefreshStatus::StorageError, std::nullopt});
                              return;
                          }
                          txn->setCommitCallback([finish](bool ok) {
                              finish(
                                {ok ? RefreshStatus::Rotated : RefreshStatus::StorageError,
                                 std::nullopt}
                              );
                          });
                          // CAS is guarded in SQL as well as under the family lock.
                          // UPDATE RETURNING is the documented raw-SQL exemption.
                          txn->execSqlAsync(
                            "UPDATE oauth2_refresh_tokens SET revoked=true "
                            "WHERE token=$1 AND client_id=$2 AND revoked=false "
                            "AND expires_at > EXTRACT(EPOCH FROM statement_timestamp()) "
                            "RETURNING token",
                            [txn, at, rt, expected, finish](const Result &rows) {
                                if (rows.empty())
                                {
                                    inspectRefresh(
                                      txn,
                                      expected.token,
                                      expected.clientId,
                                      [txn, finish](RefreshResult state) {
                                          txn->rollback();
                                          if (state.status == RefreshStatus::Ready)
                                              state = {RefreshStatus::StorageError, std::nullopt};
                                          finish(std::move(state));
                                      }
                                    );
                                    return;
                                }
                                insertPair(txn, at, rt, [finish]() {
                                    finish({RefreshStatus::StorageError, std::nullopt});
                                });
                            },
                            [finish](const DrogonDbException &) {
                                finish({RefreshStatus::StorageError, std::nullopt});
                            },
                            expected.token,
                            expected.clientId
                          );
                      }
                      catch (const std::exception &)
                      {
                          txn->rollback();
                          finish({RefreshStatus::StorageError, std::nullopt});
                      }
                      catch (...)
                      {
                          txn->rollback();
                          finish({RefreshStatus::StorageError, std::nullopt});
                      }
                  }
                );
            },
            [finish]() { finish({RefreshStatus::StorageError, std::nullopt}); }
          );
      }
    );
}

void PostgresTokenRepository::getAccessToken(const std::string &token, AccessTokenCallback &&cb)
{
    if (!dbClientMaster_)
    {
        cb(std::nullopt);
        return;
    }
    auto sharedCb = std::make_shared<AccessTokenCallback>(std::move(cb));
    try
    {
        Mapper<Oauth2AccessTokens> mapper(dbClientMaster_);
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
              // v1.4.0 M1: org binding round-trips (NULL -> nullopt).
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
              // v1.4.0 M1: org binding round-trips (NULL -> nullopt).
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
          // v1.4.0 M1: the org binding must survive the rotation read.
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

void PostgresTokenRepository::revokeTokenFamilyChecked(
  const std::string &familyId,
  SaveResultCallback &&cb
)
{
    auto callback = std::make_shared<SaveResultCallback>(std::move(cb));
    auto fired = std::make_shared<std::atomic<bool>>(false);
    auto finish = [callback, fired](bool ok) {
        if (!fired->exchange(true))
        {
            auto deliver = std::move(*callback);
            if (deliver)
                deliver(ok);
        }
    };
    if (!dbClientMaster_ || familyId.empty())
    {
        finish(false);
        return;
    }
    withTokenFamilyLock(
      dbClientMaster_,
      "refresh-family:" + familyId,
      [familyId, finish](const auto &txn) {
          txn->setCommitCallback([finish](bool ok) { finish(ok); });
          // Documented batch-operation exemption: one family spans two
          // token tables; both updates share this locked transaction.
          txn->execSqlAsync(
            "UPDATE oauth2_refresh_tokens SET revoked=true WHERE family_id=$1",
            [txn, familyId, finish](const Result &) {
                try
                {
                    txn->execSqlAsync(
                      "UPDATE oauth2_access_tokens SET revoked=true WHERE token IN "
                      "(SELECT access_token FROM oauth2_refresh_tokens WHERE family_id=$1)",
                      [](const Result &) {},
                      [finish](const DrogonDbException &) { finish(false); },
                      familyId
                    );
                }
                catch (const std::exception &)
                {
                    txn->rollback();
                    finish(false);
                }
                catch (...)
                {
                    txn->rollback();
                    finish(false);
                }
            },
            [finish](const DrogonDbException &) { finish(false); },
            familyId
          );
      },
      [finish]() { finish(false); }
    );
}

void PostgresTokenRepository::revokeTokenFamily(const std::string &familyId, VoidCallback &&cb)
{
    revokeTokenFamilyChecked(familyId, [cb = std::move(cb)](bool ok) {
        if (!ok)
            LOG_ERROR << "Token family revocation failed";
        if (cb)
            cb();
    });
}

// ========== P1: Token Introspection (RFC 7662) ==========

void PostgresTokenRepository::introspectToken(
  const std::string &token,
  TokenIntrospectionCallback &&cb
)
{
    auto callback = std::make_shared<TokenIntrospectionCallback>(std::move(cb));
    auto fired = std::make_shared<std::atomic<bool>>(false);
    auto finish = [callback, fired](TokenIntrospection result) {
        if (!fired->exchange(true))
        {
            auto deliver = std::move(*callback);
            if (deliver)
                deliver(std::move(result));
        }
    };
    auto inactive = [finish]() {
        TokenIntrospection result;
        result.active = false;
        finish(std::move(result));
    };
    // Security reads cannot refill L2 from a lagging reader after revocation.
    const auto db = dbClientMaster_;
    if (!db)
    {
        inactive();
        return;
    }
    auto readRefresh = [db, token, finish, inactive]() {
        try
        {
            Mapper<Oauth2RefreshTokens> mapper(db);
            mapper.findBy(
              Criteria(Oauth2RefreshTokens::Cols::_token, token),
              [finish, inactive](const std::vector<Oauth2RefreshTokens> &rows) {
                  if (
                    rows.empty() || rows.front().getValueOfRevoked() ||
                    rows.front().getValueOfExpiresAt() <= std::time(nullptr)
                  )
                  {
                      inactive();
                      return;
                  }
                  const auto &row = rows.front();
                  TokenIntrospection result;
                  result.active = true;
                  result.clientId = row.getValueOfClientId();
                  result.tokenType = "Bearer";
                  result.exp = row.getValueOfExpiresAt();
                  result.sub = row.getValueOfUserId();
                  result.scope = row.getValueOfScope();
                  if (row.getOrgId())
                      result.orgId = *row.getOrgId();
                  finish(std::move(result));
              },
              [inactive](const DrogonDbException &) { inactive(); }
            );
        }
        catch (const std::exception &)
        {
            inactive();
        }
        catch (...)
        {
            inactive();
        }
    };
    try
    {
        Mapper<Oauth2AccessTokens> mapper(db);
        mapper.findBy(
          Criteria(Oauth2AccessTokens::Cols::_token, token),
          [finish, inactive, readRefresh](const std::vector<Oauth2AccessTokens> &rows) {
              if (rows.empty())
              {
                  readRefresh();
                  return;
              }
              const auto &row = rows.front();
              if (row.getValueOfRevoked() || row.getValueOfExpiresAt() <= std::time(nullptr))
              {
                  inactive();
                  return;
              }
              TokenIntrospection result;
              result.active = true;
              result.clientId = row.getValueOfClientId();
              result.tokenType = "Bearer";
              result.exp = row.getValueOfExpiresAt();
              result.iat = row.getValueOfIssuedAt();
              result.iss = row.getValueOfIssuer();
              result.aud = row.getValueOfAudience();
              result.nbf = row.getValueOfNotBefore();
              result.sub = row.getValueOfUserId();
              result.scope = row.getValueOfScope();
              if (row.getOrgId())
                  result.orgId = *row.getOrgId();
              finish(std::move(result));
          },
          [inactive](const DrogonDbException &) { inactive(); }
        );
    }
    catch (const std::exception &)
    {
        inactive();
    }
    catch (...)
    {
        inactive();
    }
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

        // Documented batch-operation exemption: the cross-token predicate
        // preserves family revocation reachability when RT TTL is shorter
        // than AT TTL. Expired RTs remain unusable and are reclaimed once
        // the associated AT is absent, expired or revoked. IS NOT TRUE also
        // preserves nullable legacy AT flags, matching the DTO default.
        dbClientMaster_->execSqlAsync(
          "DELETE FROM oauth2_refresh_tokens AS expired_rt "
          "WHERE expired_rt.expires_at < $1 AND NOT EXISTS ("
          "SELECT 1 FROM oauth2_access_tokens AS live_at "
          "WHERE live_at.token=expired_rt.access_token "
          "AND live_at.expires_at > $1 AND live_at.revoked IS NOT TRUE)",
          [](const ::drogon::orm::Result &result) {
              if (result.affectedRows() > 0)
                  LOG_INFO << "Cleaned " << result.affectedRows() << " expired refresh tokens";
          },
          [](const DrogonDbException &e) {
              LOG_ERROR << "Cleanup RefreshTokens Error: " << e.base().what();
          },
          now
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
