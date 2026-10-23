#pragma once

// Task 13 (fulla-sdk-refactor, design.md §5.2/§5.3/§6): port interfaces
// for the shared Domain kernel.
//
// IRoleProvider is the port oauth2's scope-tiering policy (design.md §5.3:
// "scope 分层策略（'scope X 需 admin'）") uses to ask "what roles does this
// user have" without oauth2 compiling a dependency on libs/identity. See
// ISubjectResolver.h for the full 方案 A rationale (端口下沉到 common) and
// the async-callback design-consistency note -- both apply identically
// here (identity's future implementation is expected to be backed by the
// existing IRoleRepository::getUserRoles, which is async).
//
// Deliberately keyed by internal user id (int32_t), matching
// ISubjectResolver's resolve() output type and the existing
// IRoleRepository::getUserRoles(int32_t internalUserId, ...) overload --
// so the oauth2-side call sequence is
// "ISubjectResolver::resolve(subject) -> IRoleProvider::getRoles(userId)"
// without an intermediate re-wrapping step.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fulla::common::ports
{

/**
 * @brief Provides the role list assigned to an internal user id.
 *
 * Phase 4.5 (fulla-sdk-refactor): a string-keyed overload was added so the
 * legacy "roles keyed by subject string" semantics (MemoryOAuth2Storage /
 * MemoryRoleRepository's userRoles_ map, populated from the admin_users config)
 * survive the god-facade retirement byte-for-byte. An implementation opts in
 * by overriding supportsSubjectLookup() (returns true) + getRoles(subject).
 * Callers that prefer the subject path (e.g. oauth2::protocol::TokenService)
 * check supportsSubjectLookup() first and fall back to the int32 path
 * (ISubjectResolver::resolve -> getRoles(int32)) otherwise.
 */
class IRoleProvider
{
  public:
    using RolesCallback = std::function<void(std::vector<std::string>)>;

    virtual ~IRoleProvider() = default;

    /**
     * @brief Get the roles assigned to a user by internal user id.
     * Invokes `cb` with the role-name list (empty if the user has no
     * roles or does not exist).
     */
    virtual void getRoles(int32_t internalUserId, RolesCallback &&cb) = 0;

    /**
     * @brief Whether this provider implements the subject-string overload
     * below. Default false (the pure two-port design). Identity-backed
     * providers that key roles by subject string override to return true.
     */
    virtual bool supportsSubjectLookup() const noexcept
    {
        return false;
    }

    /**
     * @brief Get the roles assigned to a user by subject string (the opaque
     * OAuth2 subject). Only invoked when supportsSubjectLookup() returns true.
     * Invokes `cb` with the role-name list.
     */
    virtual void getRoles(const std::string & /*subject*/, RolesCallback &&cb)
    {
        cb({});
    }

    // ------------------------------------------------------------------
    // P1-8 (#285): permission resolution for the three-chain RBAC
    // enforcement (classification vs determination decoupled). Additive
    // virtuals: implementations that don't override keep the historical
    // behavior (no permissions -> admin-class scopes require the literal
    // admin role).
    // ------------------------------------------------------------------

    using PermissionsCallback = std::function<void(std::vector<std::string>)>;

    /// Whether this provider resolves role->permission unions. Default
    /// false (the pre-P1-8 two-port shape).
    virtual bool supportsPermissionLookup() const noexcept
    {
        return false;
    }

    /// Resolve the UNION of permission names held by the given roles.
    /// Only invoked when supportsPermissionLookup() returns true. Invokes
    /// `cb` with the permission-name list (empty on failure — callers
    /// treat unresolved as ungranted, the conservative side).
    virtual void getRolePermissions(
      const std::vector<std::string> & /*roles*/,
      PermissionsCallback &&cb
    )
    {
        cb({});
    }
};

}  // namespace fulla::common::ports
