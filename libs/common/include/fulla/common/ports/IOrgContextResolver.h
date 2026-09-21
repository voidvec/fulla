#pragma once

// v1.5.0 M1 (real-tenant design §2.1 item 3): port for the org context
// behind an access token's org binding. TokenService (Domain, libs/oauth2)
// needs the ACTIVE organization's name + the user's roles IN that org to
// build the org_ctx claim for id_tokens -- but membership/name live behind
// storage, which the Domain layer must not compile against. Same port-
//下沉 rationale as IRoleProvider/ISubjectResolver (design.md §5.2/§5.3):
// the interface lives in common, the implementation is wired by the
// composition root (OAuth2Plugin).
//
// Keyed by the OAuth2 subject string (like IRoleProvider's subject
// overload): callers hold the subject, and the subject -> internal id hop
// is the adapter's problem, keeping TokenService's call sites single-hop.
//
// Real-time semantics (design §2.1 item 4 / O7): implementations MUST
// re-check CURRENT membership, not a snapshot taken at issuance, so a user
// removed from the org stops getting org_ctx from userinfo immediately.
// A missing membership resolves to nullopt (no org_ctx), never an error.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fulla::common::ports
{

/// The org-scoped slice of the org_ctx claim: the active org's display
/// name and the user's CURRENT roles within that org.
struct OrgContextInfo
{
    std::string orgName;
    std::vector<std::string> roles;
};

class IOrgContextResolver
{
  public:
    using OrgContextCallback = std::function<void(std::optional<OrgContextInfo>)>;

    virtual ~IOrgContextResolver() = default;

    /**
     * @brief Resolve the org context of `subject` in `orgId`.
     * Invokes `cb` with the org name + current membership roles, or
     * nullopt when the user is not currently a member of the org (or
     * anything along the lookup fails -- org_ctx is claim data, its
     * failure mode is absence, never a failed request).
     */
    virtual void resolveOrgContext(
      const std::string &subject,
      int32_t orgId,
      OrgContextCallback &&cb
    ) = 0;
};

}  // namespace fulla::common::ports
