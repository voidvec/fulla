#include <fulla/drogon/adapters/BackchannelLogoutNotifier.h>
#include <fulla/common/observability/AuditActions.h>

#include <fulla/common/observability/AuditEvent.h>
#include <fulla/oauth2/protocol/LogoutToken.h>
#include <fulla/storage/postgres/models/Oauth2AccessTokens.h>
#include <fulla/storage/postgres/models/Oauth2Clients.h>
#include <fulla/storage/postgres/models/Users.h>

#include <drogon/drogon.h>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fulla::drogon::adapters
{

// Namespace-visibility trap (see ClientManagementService.cc's identical note):
// inside fulla::drogon::adapters a bare `drogon::` resolves to
// fulla::drogon first. Use ::drogon:: / ::drogon_model:: globally, then
// bring the orm + model names in for the Mapper/Criteria bodies below.
using namespace ::drogon::orm;
using namespace ::drogon_model::fulla_db;

namespace
{

void recordBackchannelAudit(
  const std::shared_ptr<fulla::common::ports::IAuditSink> &sink,
  const std::string &subject,
  const std::string &clientId,
  const std::string &uri,
  bool ok,
  int httpStatus)
{
    if (!sink)
        return;
    fulla::common::observability::AuditEvent ev;
    ev.actorType = "system";
    ev.actorId = subject;
    ev.action = ::fulla::common::observability::audit_events::kBackchannelLogout;
    ev.targetType = "client";
    ev.targetId = clientId;
    ev.outcome = ok ? "success" : "failure";
    ev.details["logout_uri"] = uri;
    ev.details["http_status"] = httpStatus;
    sink->record(ev);
}

}  // namespace

BackchannelLogoutNotifier::BackchannelLogoutNotifier(
  DbClientPtr dbClient,
  std::shared_ptr<const fulla::oauth2::JwkManager> jwkManager,
  std::string issuer,
  std::shared_ptr<fulla::identity::IOAuthHttpClient> httpClient,
  std::shared_ptr<fulla::common::ports::IAuditSink> auditSink,
  int tokenTtlSeconds)
  : dbClient_(std::move(dbClient)),
    jwkManager_(std::move(jwkManager)),
    issuer_(std::move(issuer)),
    httpClient_(std::move(httpClient)),
    auditSink_(std::move(auditSink)),
    tokenTtlSeconds_(tokenTtlSeconds)
{
}

void BackchannelLogoutNotifier::notify(
  const std::string &userId,
  std::function<void()> &&callback)
{
    auto sharedCb = std::make_shared<std::function<void()>>(std::move(callback));

    // Without a DB or signing key or HTTP transport there is nothing to fan
    // out -- just complete so the logout flow is never wedged by a missing dep.
    if (!dbClient_ || !jwkManager_ || !httpClient_)
    {
        (*sharedCb)();
        return;
    }

    auto self = shared_from_this();
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));

    // #82: tokens carry EITHER subject form in user_id (password-flow rows
    // store the public sub, social-flow rows the internal id -- the dual-key
    // convention UserAdminService/UserSelfServiceController revoke with).
    // The notifier previously matched only the handed form, so an RP holding
    // tokens from the other form never learned about the logout. Resolve the
    // user row to get BOTH keys, then match either; if the row is gone (hard
    // delete), fall back to the single handed form exactly as before.
    auto queryTokens = [self, sharedCb, now, userId](
                        std::optional<std::pair<std::string, std::string>> keys) {
        Criteria active =
          Criteria(Oauth2AccessTokens::Cols::_expires_at, CompareOperator::GT, now) &&
          (Criteria(Oauth2AccessTokens::Cols::_revoked, CompareOperator::EQ, false) ||
           Criteria(Oauth2AccessTokens::Cols::_revoked, CompareOperator::IsNull));
        if (keys)
        {
            // "Active" mirrors TokenManagementService::listTokens -- expires_at
            // > now AND not revoked -- evaluated in C++ (app clock) so the
            // query stays pure-Criteria (no raw SQL).
            active = active &&
                     (Criteria(Oauth2AccessTokens::Cols::_user_id, CompareOperator::EQ, keys->first) ||
                      Criteria(Oauth2AccessTokens::Cols::_user_id, CompareOperator::EQ, keys->second));
        }
        else
        {
            active = active &&
                     Criteria(Oauth2AccessTokens::Cols::_user_id, CompareOperator::EQ, userId);
        }
        try
        {
            Mapper<Oauth2AccessTokens> tokenMapper(self->dbClient_);
            tokenMapper.findBy(
              active,
              [self, sharedCb, userId](const std::vector<Oauth2AccessTokens> &tokenRows) {
                  // Distinct client_ids across the user's active sessions.
                  std::set<std::string> clientIds;
                  for (const auto &row : tokenRows)
                      clientIds.insert(row.getValueOfClientId());

                  if (clientIds.empty())
                  {
                      (*sharedCb)();
                      return;
                  }
                  std::vector<std::string> clientIdVec(clientIds.begin(), clientIds.end());

                  // Query 2: load those clients' backchannel_logout_uri. Own
                  // try-catch -- this Mapper is constructed inside an async cb, so
                  // the outer guard does not reach it (db-operations.md 要求 1).
                  try
                  {
                      Mapper<Oauth2Clients> clientMapper(self->dbClient_);
                      clientMapper.findBy(
                        Criteria(Oauth2Clients::Cols::_client_id, CompareOperator::In, clientIdVec),
                        [self, sharedCb, userId](const std::vector<Oauth2Clients> &clientRows) {
                            std::vector<BackchannelRpTarget> targets;
                            for (const auto &c : clientRows)
                            {
                                const auto &uri = c.getValueOfBackchannelLogoutUri();
                                if (!uri.empty())
                                    targets.push_back({c.getValueOfClientId(), uri});
                            }
                            self->dispatch(
                              userId, std::move(targets),
                              [sharedCb]() { (*sharedCb)(); });
                        },
                        [sharedCb](const DrogonDbException &) {
                            // Client lookup failed -- best-effort: complete without
                            // notifying (a transient DB error must not wedge logout).
                            (*sharedCb)();
                        });
                  }
                  catch (const std::exception &)
                  {
                      (*sharedCb)();
                  }
              },
              [sharedCb](const DrogonDbException &) {
                  // Token lookup failed -- complete (db-operations.md 要求 2: a
                  // failure must reach the callback, not just LOG+return).
                  (*sharedCb)();
              });
        }
        catch (const std::exception &)
        {
            // Mapper construction threw -- complete (same rule as above).
            (*sharedCb)();
        }
    };

    // Resolve the other subject form from the users row (numeric input -> id
    // lookup -> public_sub; otherwise public_sub lookup -> id). std::stol
    // with pos-check + try/catch: an out-of-range numeric string throws and
    // must not escape into the event loop.
    std::optional<int32_t> numericId;
    if (!userId.empty() &&
        std::all_of(userId.begin(), userId.end(), [](char c) { return c >= '0' && c <= '9'; }))
    {
        try
        {
            size_t pos = 0;
            long parsed = std::stol(userId, &pos);
            constexpr long kMaxInt32 = 2147483647L;
            if (pos == userId.size() && parsed > 0 && parsed <= kMaxInt32)
                numericId = static_cast<int32_t>(parsed);
        }
        catch (const std::exception &)
        {
            numericId = std::nullopt;
        }
    }
    try
    {
        Mapper<Users> userMapper(dbClient_);
        userMapper.findOne(
          numericId ? Criteria(Users::Cols::_id, CompareOperator::EQ, *numericId)
                    : Criteria(Users::Cols::_public_sub, CompareOperator::EQ, userId),
          [queryTokens, sharedCb](const Users &user) {
              const std::string publicSub = user.getValueOfPublicSub();
              const std::string internalId = std::to_string(user.getValueOfId());
              if (publicSub.empty() || internalId.empty())
                  queryTokens(std::nullopt);
              else
                  queryTokens(std::make_pair(publicSub, internalId));
          },
          [queryTokens](const DrogonDbException &) {
              // Row gone (hard-deleted user): single-form fallback.
              queryTokens(std::nullopt);
          });
    }
    catch (const std::exception &)
    {
        // Users Mapper construction threw: single-form fallback.
        queryTokens(std::nullopt);
    }
}

void BackchannelLogoutNotifier::dispatch(
  const std::string &subject,
  const std::vector<BackchannelRpTarget> &targets,
  std::function<void()> &&completion)
{
    auto sharedCompletion = std::make_shared<std::function<void()>>(std::move(completion));

    if (!jwkManager_ || !httpClient_)
    {
        (*sharedCompletion)();
        return;
    }

    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const auto auditSink = auditSink_;  // copy the shared_ptr for capture

    for (const auto &target : targets)
    {
        if (target.backchannelLogoutUri.empty())
            continue;

        const auto jti = fulla::oauth2::protocol::generateJti();
        auto claims = fulla::oauth2::protocol::buildLogoutTokenClaims(
          issuer_, subject, target.clientId, now, tokenTtlSeconds_, jti);
        const std::string jwt = jwkManager_->signJwt(claims);
        if (jwt.empty())
        {
            // Signing failed (e.g. key not initialized) -- audit and skip.
            recordBackchannelAudit(
              auditSink, subject, target.clientId, target.backchannelLogoutUri, false, 0);
            continue;
        }

        const std::string clientId = target.clientId;
        const std::string uri = target.backchannelLogoutUri;
        std::vector<std::pair<std::string, std::string>> params;
        params.emplace_back("logout_token", jwt);

        // Fire-and-forget POST. The completion callback captures only shared
        // state (the audit sink copy) -- never this -- so it is safe even if
        // it runs after this call returns (real async transport).
        httpClient_->postForm(
          uri,
          params,
          [auditSink, subject, clientId, uri](fulla::identity::OAuthHttpResult result) {
              const bool ok = result.transportOk && result.statusCode == 200;
              recordBackchannelAudit(auditSink, subject, clientId, uri, ok, result.statusCode);
          });
    }

    // Dispatch is done (POSTs are either completed, for the synchronous fake,
    // or in flight, for the real async transport). Complete now either way --
    // logout must not wait on RP round-trips.
    (*sharedCompletion)();
}

}  // namespace fulla::drogon::adapters
