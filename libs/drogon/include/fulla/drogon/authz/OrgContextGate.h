#pragma once

// v1.5.0 M1 (real-tenant design §2.1 item 5, ruling O1): the authorize-time
// validity gate for the `org_id` parameter. The parameter is a client-
// supplied HINT selecting the organization context of this authorization;
// it is accepted only when
//
//   1. the referenced org exists (integer id or slug, normalized to id),
//   2. the authorizing user is a CURRENT member of that org, and
//   3. the client is that org's application (oauth2_client_owners.org_id)
//      OR the org holds an active org consent row for the client under any
//      scope (#236, design §2.2 -- third-party apps granted at org level).
//
// plus the §2.5 org MFA policy: an org flagged require_mfa only lends its
// context to an MFA-elevated session (amr contains "mfa").
//
// Anti-enumeration (O1): EVERY failure cause -- unknown org, non-member,
// unrelated client, no consent row, even a DB error -- produces the SAME
// decision (Invalid); the caller renders ONE uniform inline error page with
// no redirect, so the endpoint cannot be used as a membership/provenance
// oracle (#229's authorize-side face).
//
// Storage reads go through the shared ClientOwnersRepository (#222) and
// OrgConsentRepository (M2); the data access lives in storage, the POLICY
// lives here.

#include <cstdint>
#include <functional>
#include <string>

namespace fulla::drogon::authz
{

struct OrgContextDecision
{
    enum class Kind
    {
        None,         // no org_id parameter: proceed without org context
        Proceed,      // org context accepted; orgId/orgName are set
        Invalid,      // uniform rejection (see the header comment)
        MfaRequired,  // org accepted but require_mfa vs a non-MFA session
        StorageError  // DB unavailable (memory mode / no client): uniform
                      // rejection, logged -- fail-closed, same response shape
    };
    Kind kind = Kind::None;
    int32_t orgId = 0;
    std::string orgName;
};

class OrgContextGate
{
  public:
    using DecisionCallback = std::function<void(const OrgContextDecision &)>;

    /// Validate the org context of an authorization request.
    /// `orgRef` is the raw org_id parameter ("" -> Kind::None);
    /// `internalUserId` is the user's internal id (membership key);
    /// `amr` is the session's current amr value ("" when absent).
    static void validate(
      const std::string &orgRef,
      const std::string &clientId,
      int32_t internalUserId,
      const std::string &amr,
      DecisionCallback &&cb
    );
};

}  // namespace fulla::drogon::authz
