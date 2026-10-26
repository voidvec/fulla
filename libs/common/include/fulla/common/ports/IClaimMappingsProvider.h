#pragma once

// P1-3 (#280): the Domain-facing port for claims-mapping publication.
//
// Same additive-setter shape as IOrgContextResolver (v1.4.0 M1's port
// pattern): TokenService consumes it at BOTH id_token issuance sites
// (code path + refresh path) and the userinfo handler consumes it via the
// plugin; an unset provider keeps the pre-P1-3 claim set byte-for-byte.
//
// Security model (deliverables/p1-fix-design-2026-10.md §P1-3 rev.1):
//   * each mapping is (trigger scope, claim name, source path);
//   * a mapped claim is published ONLY when the token carries the trigger
//     scope (the same authorization decision as org_ctx under `org`);
//   * claim_name reserved words / source whitelist / scope=openid are
//     enforced at WRITE time by the admin surface (ClaimMappingValidator);
//     the provider assumes stored rows already passed validation.

#include <functional>
#include <string>
#include <vector>

namespace fulla::common::ports
{

/// One resolved mapping row (enabled only).
struct ClaimMappingEntry
{
    std::string scopeName;
    std::string claimName;
    std::string sourcePath;
    /// "id_token" | "userinfo" | "both"
    std::string includeIn;
};

class IClaimMappingsProvider
{
  public:
    using MappingsCallback = std::function<void(std::vector<ClaimMappingEntry>)>;

    virtual ~IClaimMappingsProvider() = default;

    /// All ENABLED mappings. Invokes `cb` exactly once (empty list on any
    /// failure — mapped claims are claim data; their failure mode is
    /// absence, matching org_ctx).
    virtual void getEnabledMappings(MappingsCallback &&cb) = 0;
};

}  // namespace fulla::common::ports
