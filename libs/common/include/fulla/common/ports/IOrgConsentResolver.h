#pragma once

// v1.5.0 M2 (real-tenant design §2.2, ruling R-M2-1): port for the
// organization-consent half of the authorize-time consent UNION.
// AuthorizationService (Domain, libs/oauth2) decides per scope
//   consented = personal consent EXISTS
//              OR any org the user is CURRENTLY a member of holds an
//                 active (org, client, scope) organization_consents row
// but the org tables live behind storage, which the Domain layer must
// not compile against. Same port-sinking rationale as
// IRoleProvider/ISubjectResolver/IOrgContextResolver (M1): the
// interface lives in common, the implementation is wired by the
// composition root (OAuth2Plugin) -- StorageOrgConsentResolver over the
// shared OrgConsentRepository.
//
// Keyed by internal user id (AuthorizationService already resolved it
// before the consent fan-out; unlike IOrgContextResolver there is no
// subject hop to hide here).
//
// Failure semantics: implementations MUST resolve lookup failures to
// false (the union's failure mode is "prompt the user", mirroring
// IConsentRepository's hasUserConsent error path) and re-check CURRENT
// membership (a removed member's org consents stop counting
// immediately -- same real-time semantics as O7).

#include <cstdint>
#include <functional>
#include <string>

namespace fulla::common::ports
{

class IOrgConsentResolver
{
  public:
    using BoolCallback = std::function<void(bool)>;

    virtual ~IOrgConsentResolver() = default;

    /**
     * @brief Does any org the user currently belongs to hold an active
     * consent row for (clientId, scope)? Invokes `cb` with the answer;
     * false also covers "no memberships", "no rows" and "lookup
     * failed".
     */
    virtual void hasOrgConsentForUser(
      int32_t internalUserId,
      const std::string &clientId,
      const std::string &scope,
      BoolCallback &&cb
    ) = 0;
};

}  // namespace fulla::common::ports
