#pragma once

// P1-8 (#285): shared role→permission resolution for the THREE-chain RBAC
// enforcement (ADR-0014 rev.1 execution version).
//
// Consumers:
//   - Chain① (scope tiering): the OAuth2Plugin injects this resolver's
//     output into AuthorizationService's per-request predicate closure —
//     a scope in the admin CLASS passes only when the user's permission
//     set covers it (classification vs determination decoupled, security
//     review H2: a helpdesk grant never de-classifies a scope).
//   - Gate③ (HTTP role gate): AuthorizationFilter resolves the caller's
//     permission set once per request for rbac_rules entries that declare
//     a `permissions` requirement.
//
// Data sources:
//   - Postgres: role_permissions JOIN-less two-hop Criteria query (roles →
//     role ids → permission names; db-operations.md forbids JOIN-in-one).
//   - Memory mode: the compile-time default map below, mirroring the V043
//     seed semantics (admin → the full ten-permission set; user → none;
//     the legacy V005 user-role grant is inert by design — F3).
//
// Cache coherence (documented honestly): single-instance deployments are
// immediately consistent for permission-catalog and role-permission
// writes that route through RolePermissionCache::invalidate(); multi-
// instance drift is a known limitation (P3 shared storage resolves it).

#include <drogon/drogon.h>
#include <fulla/storage/postgres/models/Roles.h>
#include <fulla/storage/postgres/models/Permissions.h>
#include <fulla/storage/postgres/models/RolePermissions.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace fulla::drogon::authz
{

/// Compile-time default role→permission map (memory mode / pre-DB), same
/// shape as defaultAdminScopes() and mirroring the V043 seed.
inline const std::unordered_set<std::string> &defaultPermissionsForRole(const std::string &role)
{
    static const std::unordered_set<std::string> kEmpty;
    static const std::unordered_set<std::string> kAdmin = {
      "users:read", "users:write", "clients:read", "clients:write", "tokens:read",
      "tokens:write", "roles:read", "roles:write", "audit:read", "admin"};
    if (role == "admin")
        return kAdmin;
    return kEmpty;
}

/// Compile-time default admin-class scope set (classification source for
/// memory mode; mirrors oauth2_scopes.requires_admin_role seeds).
inline const std::unordered_set<std::string> &defaultAdminClassScopes()
{
    static const std::unordered_set<std::string> kScopes = {
      "admin",       "users:read",  "users:write",  "clients:read", "clients:write",
      "tokens:read", "tokens:write", "roles:read",  "roles:write",  "audit:read"};
    return kScopes;
}

class RolePermissionResolver
{
  public:
    using PermissionsCallback = std::function<void(std::unordered_set<std::string>)>;

    /// `postgresStorage=false` selects the compile-time default map
    /// (memory deployments hold no role_permissions rows by construction).
    explicit RolePermissionResolver(bool postgresStorage);
    ~RolePermissionResolver();

    /// Resolve the union of permissions held by the given role names.
    /// Invokes `cb` exactly once (empty set on any failure — the callers
    /// treat "cannot resolve" as "no permissions", the conservative side).
    void resolveForRoles(const std::vector<std::string> &roles, PermissionsCallback &&cb);

  private:
    bool postgresStorage_;
};

/// Cache invalidation seams (single-instance coherence): call after any
/// write to permissions / role_permissions (the admin role-permissions PUT
/// and future catalog writes).
void invalidateRolePermissionCacheForRole(const std::string &role);
void invalidateRolePermissionCacheAll();

}  // namespace fulla::drogon::authz
