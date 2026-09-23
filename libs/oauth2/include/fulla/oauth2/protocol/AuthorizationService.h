#pragma once

// Task 17 remainder (fulla-sdk-refactor, design.md §6/§8 "protocol/"):
// AuthorizationService did not exist anywhere in the pre-migration
// codebase as a distinct class -- the consent/scope decision orchestration
// design.md calls for was spread across ClientService::validateClientScopes
// (Tier 1: client allowlist), IdentityService::validateUserRolesForScopes
// + scopeRequiresAdminRole (Tier 2: admin role), and
// OAuth2StandardController::checkUserConsentAndProceed's own recursive
// plugin->hasUserConsent(...) walk (Tier 3: per-scope consent) -- three
// separate call sites in two services plus a controller, with no single
// place owning the combined decision.
//
// This class is that single place: it drives
// fulla::oauth2::access::evaluateScopes (the pure decision engine,
// Task 17 slice 7) by resolving the facts that engine needs (the Client
// aggregate via IClientRepository, the admin-role flag via
// ISubjectResolver+IRoleProvider, and per-scope consent via
// IConsentRepository) and returns a single ScopeValidationSummary the
// caller can act on (proceed / redirect to consent / reject) instead of
// threading three separate async calls together itself.
//
// NOT YET WIRED INTO PRODUCTION: OAuth2StandardController's consent flow
// (OAuth2Plugin/libs/drogon) continues to use its own inline
// plugin->validateClientScopes/validateUserRolesForScopes/hasUserConsent
// call chain. Wiring this class in is Task 24 (apps/server assembly),
// deferred until libs/identity's remaining services are filled in (see
// PROGRESS.md). This class is additive and independently unit-tested.

#include <fulla/common/ports/IOrgConsentResolver.h>
#include <fulla/common/ports/IRoleProvider.h>
#include <fulla/common/ports/ISubjectResolver.h>
#include <fulla/oauth2/access/ScopeDecision.h>
#include <fulla/oauth2/repository/IClientRepository.h>
#include <fulla/oauth2/repository/IConsentRepository.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace fulla::oauth2::protocol
{

class AuthorizationService
{
  public:
    AuthorizationService(
      std::shared_ptr<fulla::oauth2::repository::IClientRepository> clients,
      std::shared_ptr<fulla::oauth2::repository::IConsentRepository> consents = nullptr,
      std::shared_ptr<fulla::common::ports::ISubjectResolver> subjectResolver = nullptr,
      std::shared_ptr<fulla::common::ports::IRoleProvider> roleProvider = nullptr,
      // #43 §5.5: the set of scope names that require the admin role
      // (loaded from oauth2_scopes.requires_admin_role at startup by
      // OAuth2Plugin). Drives the engine's Tier-2 check via a predicate.
      std::unordered_set<std::string> adminScopes = {}
    );

    /**
     * @brief Evaluate every scope in `requestedScopes` for `clientId` +
     * `subject` (client allowlist -> admin role -> user consent, in that
     * tier order -- matches the pre-existing production sequence). The
     * consent tier's `internalUserId` (needed by IConsentRepository, which
     * is keyed by UserRef) is resolved via subjectResolver_ internally;
     * if subjectResolver_ is unset, or the subject does not resolve, or
     * consents_ is unset, every scope that passes tiers 1-2 is reported
     * as ConsentRequired (the safe default: cannot confirm consent, so do
     * not assume it).
     *
     * @param clientId Client identifier.
     * @param subject OAuth2 subject (e.g. "local:alice").
     * @param requestedScopes The scopes being requested.
     * @param callback Invoked with the aggregated decision. If `clientId`
     * does not resolve to a known client, every scope is reported Invalid
     * with reason "client_not_found".
     */
    void evaluateScopes(
      const std::string &clientId,
      const std::string &subject,
      const std::vector<std::string> &requestedScopes,
      std::function<void(fulla::oauth2::access::ScopeValidationSummary)> &&callback
    );

    /// #43 §5.5: override the admin-scope set at runtime (e.g. after an
    /// async DB load in OAuth2Plugin). The constructor supplies a safe
    /// default; this replaces it with the live DB value so the Tier-2 check
    /// is fully data-driven in production.
    void setAdminScopes(std::unordered_set<std::string> adminScopes)
    {
        adminScopes_ = std::move(adminScopes);
    }

    /// v1.5.0 M2 (design §2.2, R-M2-1): inject the org-consent half of
    /// the consent UNION. Unset (or a scope with a personal consent
    /// already recorded -- the common case short-circuits) keeps the
    /// decision exactly as before this milestone. Setter-injected like
    /// TokenService::setOrgContextResolver (M1): the composition root
    /// wires it at plugin init; memory-mode deployments pass nothing.
    void setOrgConsentResolver(std::shared_ptr<fulla::common::ports::IOrgConsentResolver> resolver)
    {
        orgConsentResolver_ = std::move(resolver);
    }

  private:
    std::shared_ptr<fulla::oauth2::repository::IClientRepository> clients_;
    std::shared_ptr<fulla::oauth2::repository::IConsentRepository> consents_;
    std::shared_ptr<fulla::common::ports::ISubjectResolver> subjectResolver_;
    std::shared_ptr<fulla::common::ports::IRoleProvider> roleProvider_;
    std::unordered_set<std::string> adminScopes_;  // #43 §5.5
    std::shared_ptr<fulla::common::ports::IOrgConsentResolver> orgConsentResolver_;  // M2
};

}  // namespace fulla::oauth2::protocol
