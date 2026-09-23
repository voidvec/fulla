#include <fulla/oauth2/protocol/AuthorizationService.h>

#include <fulla/common/model/Subject.h>
#include <fulla/oauth2/access/ScopeDecisionEngine.h>
#include <fulla/oauth2/model/Client.h>

#include <algorithm>
#include <memory>
#include <unordered_map>

namespace fulla::oauth2::protocol
{

namespace
{
fulla::oauth2::access::ScopeValidationSummary allInvalid(
  const std::vector<std::string> &scopes,
  const std::string &reason
)
{
    fulla::oauth2::access::ScopeValidationSummary summary;
    for (const auto &scope : scopes)
    {
        summary.invalid.push_back(scope);
        summary.invalidReasons.push_back(reason);
    }
    return summary;
}

bool anyRequiresAdminRole(
  const std::vector<std::string> &scopes,
  const std::unordered_set<std::string> &adminScopes)
{
    for (const auto &scope : scopes)
    {
        if (adminScopes.count(scope) > 0)
            return true;
    }
    return false;
}

// #43 §5.5: the safe default admin-scope set, mirroring the V006 seed
// (oauth2_scopes.requires_admin_role = TRUE). Used when no DB-loaded set is
// supplied (memory/dev/test mode). In production (postgres) OAuth2Plugin
// overrides this at startup with the live DB value via setAdminScopes(), so
// the runtime definition is data-driven and cannot drift from the catalog.
// This default exists ONLY so the Tier-2 check is never a silent no-op
// before the DB load completes or when DB is unavailable.
const std::unordered_set<std::string> &defaultAdminScopes()
{
    static const std::unordered_set<std::string> scopes = {
      "admin",        "users:read",  "users:write",  "clients:read",
      "clients:write", "tokens:read", "tokens:write", "roles:read",
      "roles:write",  "audit:read"};
    return scopes;
}
}  // namespace

AuthorizationService::AuthorizationService(
  std::shared_ptr<fulla::oauth2::repository::IClientRepository> clients,
  std::shared_ptr<fulla::oauth2::repository::IConsentRepository> consents,
  std::shared_ptr<fulla::common::ports::ISubjectResolver> subjectResolver,
  std::shared_ptr<fulla::common::ports::IRoleProvider> roleProvider,
  std::unordered_set<std::string> adminScopes
)
    : clients_(std::move(clients)),
      consents_(std::move(consents)),
      subjectResolver_(std::move(subjectResolver)),
      roleProvider_(std::move(roleProvider)),
      adminScopes_(adminScopes.empty() ? defaultAdminScopes() : std::move(adminScopes))
{
}

void AuthorizationService::evaluateScopes(
  const std::string &clientId,
  const std::string &subject,
  const std::vector<std::string> &requestedScopes,
  std::function<void(fulla::oauth2::access::ScopeValidationSummary)> &&callback
)
{
    if (!clients_)
    {
        callback(allInvalid(requestedScopes, "server_error"));
        return;
    }

    clients_->getClient(
      clientId,
      [this, clientId, subject, requestedScopes, callback = std::move(callback)](
        std::optional<fulla::oauth2::model::OAuth2Client> clientOpt
      ) mutable {
          if (!clientOpt)
          {
              callback(allInvalid(requestedScopes, "client_not_found"));
              return;
          }

          auto client = std::make_shared<fulla::oauth2::model::Client>(std::move(*clientOpt));
          bool needsAdminCheck = anyRequiresAdminRole(requestedScopes, adminScopes_);

          // Resolve internalUserId once (used for both the admin-role check
          // and the per-scope consent lookup below).
          auto resolveInternalUserId =
            [this](const std::string &subj, std::function<void(std::optional<int32_t>)> &&cb) {
                if (!subjectResolver_)
                {
                    cb(std::nullopt);
                    return;
                }
                fulla::common::model::Subject subjectValue(subj);
                subjectResolver_->resolve(subjectValue, std::move(cb));
            };

          resolveInternalUserId(
            subject,
            [this,
             client,
             clientId,
             subject,
             requestedScopes,
             needsAdminCheck,
             callback = std::move(callback)](std::optional<int32_t> internalUserId) mutable {
                auto proceedWithAdminFlag = [this,
                                             client,
                                             clientId,
                                             subject,
                                             requestedScopes,
                                             internalUserId,
                                             callback =
                                               std::move(callback)](bool hasAdminRole) mutable {
                    // Fan out consent lookups for every scope that is
                    // syntactically eligible (tiers 1-2 are re-checked
                    // inside evaluateScope itself; prefetching consent for
                    // every requested scope, even ones that will end up
                    // Invalid, is simpler than pre-filtering and is at most
                    // a few redundant lookups).
                    if (!consents_ || !internalUserId)
                    {
                        fulla::oauth2::access::ScopeValidationSummary summary =
                          fulla::oauth2::access::evaluateScopes(
                            requestedScopes, *client, hasAdminRole, [](const std::string &) {
                                return false;
                            },
                            [this](const std::string &s) {
                                return adminScopes_.count(s) > 0;
                            }
                          );
                        callback(std::move(summary));
                        return;
                    }

                    auto consentMap = std::make_shared<std::unordered_map<std::string, bool>>();
                    auto remaining = std::make_shared<size_t>(requestedScopes.size());
                    if (requestedScopes.empty())
                    {
                        callback(
                          fulla::oauth2::access::evaluateScopes(
                            requestedScopes, *client, hasAdminRole, [](const std::string &) {
                                return false;
                            },
                            [this](const std::string &s) {
                                return adminScopes_.count(s) > 0;
                            }
                          )
                        );
                        return;
                    }

                    fulla::oauth2::model::UserRef userRef{*internalUserId};
                    for (const auto &scope : requestedScopes)
                    {
                        // v1.5.0 M2 (design §2.2, R-M2-1): the consent
                        // tier is a UNION -- a scope is consented when a
                        // personal row exists OR any org the user
                        // currently belongs to holds an active org
                        // consent row. The personal lookup stays first
                        // and short-circuits (org memberships are the
                        // minority case, and an unset resolver -- memory
                        // mode / unit tests -- keeps the decision exactly
                        // pre-M2). shared_ptr finisher: both hops funnel
                        // into it exactly once each.
                        auto finisher = std::make_shared<std::function<void(bool)>>(
                          [this, consentMap, remaining, scope, client, hasAdminRole,
                           requestedScopes, callback](bool hasConsent) mutable {
                              try
                              {
                                  (*consentMap)[scope] = hasConsent;
                                  if (--(*remaining) == 0)
                                  {
                                      callback(
                                        fulla::oauth2::access::evaluateScopes(
                                          requestedScopes,
                                          *client,
                                          hasAdminRole,
                                          [consentMap](const std::string &s) {
                                              auto it = consentMap->find(s);
                                              return it != consentMap->end() && it->second;
                                          },
                                          [this](const std::string &s) {
                                              return adminScopes_.count(s) > 0;
                                          }
                                        )
                                      );
                                  }
                              }
                              catch (const std::exception &)
                              {
                                  callback(allInvalid(requestedScopes, "internal_error"));
                              }
                          }
                        );
                        consents_->hasUserConsent(
                          userRef,
                          clientId,
                          scope,
                          [orgResolver = orgConsentResolver_, internalUserId, clientId, scope,
                           finisher](bool hasConsent) mutable {
                              if (hasConsent || !orgResolver)
                              {
                                  (*finisher)(hasConsent);
                                  return;
                              }
                              orgResolver->hasOrgConsentForUser(
                                *internalUserId,
                                clientId,
                                scope,
                                [finisher](bool orgConsent) mutable {
                                    (*finisher)(orgConsent);
                                }
                              );
                          }
                        );
                    }
                };

                if (!needsAdminCheck || !roleProvider_ || !internalUserId)
                {
                    proceedWithAdminFlag(false);
                    return;
                }

                roleProvider_->getRoles(
                  *internalUserId,
                  [proceedWithAdminFlag =
                     std::move(proceedWithAdminFlag)](std::vector<std::string> roles) mutable {
                      bool hasAdmin = std::find(roles.begin(), roles.end(), "admin") != roles.end();
                      proceedWithAdminFlag(hasAdmin);
                  }
                );
            }
          );
      }
    );
}

}  // namespace fulla::oauth2::protocol
