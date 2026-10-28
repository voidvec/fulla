#pragma once

// M5 Task 29b batch 4 (fulla-sdk-refactor): application-service extraction
// for the role + scope admin domain. Raw SQL inline in RoleScopeAdminController
// is now Mapper<T> + Criteria on the ORM Roles/Oauth2Scopes/UserRoles models
// (per .claude/rules/db-operations.md). The listRoles JOIN+GROUP-BY+COUNT is
// split into separate Mapper queries (JOIN-in-one-query is forbidden).
// Behavior equivalent (Admin API tests must stay green).
//
// Lives in libs/drogon (Adapter/SDK layer, namespace fulla::drogon::admin).

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>
#include <memory>
#include <string>

namespace fulla::drogon::admin
{

/**
 * @brief Application service for role + scope admin CRUD.
 */
class RoleScopeAdminService
{
  public:
    using ResponseCallback =
      std::shared_ptr<std::function<void(const ::drogon::HttpResponsePtr &)>>;

    // Roles
    static void listRoles(const ::drogon::HttpRequestPtr &req, ResponseCallback cb);
    static void createRole(const ::drogon::HttpRequestPtr &req, ResponseCallback cb);
    static void updateRole(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &roleId
    );
    static void deleteRole(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &roleId
    );

    // ------------------------------------------------------------------
    // P1-8 (#285): role->permission administration (delegated
    // administration surface). FULL-REPLACE semantics: the PUT body's
    // permission-name list becomes the role's entire grant set.
    // ------------------------------------------------------------------
    static void getRolePermissions(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &roleId
    );
    static void putRolePermissions(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &roleId
    );

    // Scopes
    static void listScopes(const ::drogon::HttpRequestPtr &req, ResponseCallback cb);
    static void createScope(const ::drogon::HttpRequestPtr &req, ResponseCallback cb);
    static void updateScope(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &scopeId
    );
    static void deleteScope(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &scopeId
    );
};

}  // namespace fulla::drogon::admin
