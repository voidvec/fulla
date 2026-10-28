#pragma once

// M2b Task 17 slice 12 (fulla-sdk-refactor): Adapter-side default
// implementation of fulla::common::ports::IRoleProvider.
//
// Phase 4.5: overrides the subject-string overload (supportsSubjectLookup()
// = true), preserving the legacy "roles keyed by subject string" semantics
// (MemoryRoleRepository userRoles_ populated from admin_users config) through
// the new IRoleProvider port.
//
// Phase 1.5d (Task 39): now backed by fulla::identity::IRoleRepository
// (the NEW identity interface; was the legacy ::oauth2::IRoleRepository).
// Forwards getRoles(...) to roleRepo->getRoles(...). Placed under
// OAuth2Plugin/include/oauth2/adapters/ alongside the other Adapter-layer
// port implementations.
//
// P1-8 (#285): also implements the additive permission path
// (supportsPermissionLookup + getRolePermissions) via the shared
// RolePermissionResolver — Postgres-backed under DB storage, the
// compile-time default map (V043 seed mirror) under memory storage. The
// plugin injects the resolver; a null resolver keeps the historical
// no-permission behavior.

#include <fulla/common/ports/IRoleProvider.h>
#include <fulla/drogon/authz/RolePermissionResolver.h>
#include <fulla/identity/IRoleRepository.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace fulla::drogon::adapters
{

class StorageRoleProvider : public fulla::common::ports::IRoleProvider
{
  public:
    explicit StorageRoleProvider(std::shared_ptr<fulla::identity::IRoleRepository> roleRepo)
        : roleRepo_(std::move(roleRepo))
    {
    }

    // P1-8: with a resolver, the provider also answers permission unions.
    StorageRoleProvider(
      std::shared_ptr<fulla::identity::IRoleRepository> roleRepo,
      std::shared_ptr<fulla::drogon::authz::RolePermissionResolver> permissionResolver
    )
        : roleRepo_(std::move(roleRepo)), permissionResolver_(std::move(permissionResolver))
    {
    }

    // Phase 4.5: subject-string path -- preferred by
    // oauth2::protocol::TokenService. Byte-equivalent to the legacy
    // storage_->getUserRoles(subjectString).
    bool supportsSubjectLookup() const noexcept override
    {
        return true;
    }

    void getRoles(const std::string &subject, RolesCallback &&cb) override
    {
        if (!roleRepo_)
        {
            cb({});
            return;
        }
        roleRepo_->getRoles(subject, std::move(cb));
    }

    void getRoles(int32_t internalUserId, RolesCallback &&cb) override
    {
        if (!roleRepo_)
        {
            cb({});
            return;
        }
        roleRepo_->getRoles(internalUserId, std::move(cb));
    }

    // P1-8 (#285): permission path.
    bool supportsPermissionLookup() const noexcept override
    {
        return permissionResolver_ != nullptr;
    }

    void getRolePermissions(
      const std::vector<std::string> &roles,
      PermissionsCallback &&cb
    ) override
    {
        if (!permissionResolver_)
        {
            cb({});
            return;
        }
        permissionResolver_->resolveForRoles(
          roles,
          [cb = std::move(cb)](const std::unordered_set<std::string> &perms) mutable {
              std::vector<std::string> out(perms.begin(), perms.end());
              cb(std::move(out));
          });
    }

  private:
    std::shared_ptr<fulla::identity::IRoleRepository> roleRepo_;
    std::shared_ptr<fulla::drogon::authz::RolePermissionResolver> permissionResolver_;
};

}  // namespace fulla::drogon::adapters
