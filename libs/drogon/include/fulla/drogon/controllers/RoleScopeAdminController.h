#pragma once

// M5 Task 29a (fulla-sdk-refactor): the role + scope management routes
// (`/api/admin/roles*` + `/api/admin/scopes*`) carved out of the former
// AdminController (design.md §5.8 / Task 29). Verbatim move -- behavior
// unchanged, Admin API tests must stay green. The raw-SQL -> ORM Mapper +
// business-logic-to-service extraction is deferred to Task 29b.

#include <drogon/HttpController.h>

namespace fulla::drogon::controllers
{

class RoleScopeAdminController : public ::drogon::HttpController<RoleScopeAdminController, false>
{
  public:
    METHOD_LIST_BEGIN
    // Roles
    ADD_METHOD_TO(
      RoleScopeAdminController::listRoles,
      "/api/admin/roles",
      ::drogon::Get,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    ADD_METHOD_TO(
      RoleScopeAdminController::createRole,
      "/api/admin/roles",
      ::drogon::Post,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    ADD_METHOD_TO(
      RoleScopeAdminController::updateRole,
      "/api/admin/roles/{roleId}",
      ::drogon::Put,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    ADD_METHOD_TO(
      RoleScopeAdminController::deleteRole,
      "/api/admin/roles/{roleId}",
      ::drogon::Delete,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    // P1-8 (#285): role->permission administration (delegated
    // administration surface; full-replace semantics).
    ADD_METHOD_TO(
      RoleScopeAdminController::getRolePermissions,
      "/api/admin/roles/{roleId}/permissions",
      ::drogon::Get,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    ADD_METHOD_TO(
      RoleScopeAdminController::putRolePermissions,
      "/api/admin/roles/{roleId}/permissions",
      ::drogon::Put,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    // Scopes
    ADD_METHOD_TO(
      RoleScopeAdminController::listScopes,
      "/api/admin/scopes",
      ::drogon::Get,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    ADD_METHOD_TO(
      RoleScopeAdminController::createScope,
      "/api/admin/scopes",
      ::drogon::Post,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    ADD_METHOD_TO(
      RoleScopeAdminController::updateScope,
      "/api/admin/scopes/{scopeId}",
      ::drogon::Put,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    ADD_METHOD_TO(
      RoleScopeAdminController::deleteScope,
      "/api/admin/scopes/{scopeId}",
      ::drogon::Delete,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    // #43 discovery: the scope -> resource authorization matrix.
    // Declared BEFORE /api/admin/scopes/{scopeId} would match it as a path
    // param; Drogon matches static segments before parameterized ones, but
    // explicit ordering documents intent.
    ADD_METHOD_TO(
      RoleScopeAdminController::scopeResources,
      "/api/admin/scopes/resources",
      ::drogon::Get,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    // P1-3 (#280): claim-mapping CRUD.
    ADD_METHOD_TO(
      RoleScopeAdminController::listClaimMappings,
      "/api/admin/claim-mappings",
      ::drogon::Get,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    ADD_METHOD_TO(
      RoleScopeAdminController::createClaimMapping,
      "/api/admin/claim-mappings",
      ::drogon::Post,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    ADD_METHOD_TO(
      RoleScopeAdminController::updateClaimMapping,
      "/api/admin/claim-mappings/{mappingId}",
      ::drogon::Put,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    ADD_METHOD_TO(
      RoleScopeAdminController::deleteClaimMapping,
      "/api/admin/claim-mappings/{mappingId}",
      ::drogon::Delete,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    METHOD_LIST_END

    void listRoles(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );

    void createRole(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );

    void updateRole(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &roleId
    );

    void getRolePermissions(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &roleId
    );
    void putRolePermissions(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &roleId
    );

    void deleteRole(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &roleId
    );

    void listScopes(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );

    void createScope(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );

    void updateScope(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &scopeId
    );

    void listClaimMappings(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void createClaimMapping(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void updateClaimMapping(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &mappingId
    );
    void deleteClaimMapping(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &mappingId
    );

    void deleteScope(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &scopeId
    );

    /// #43 discovery: return the (path, method) -> required-scopes matrix
    /// from ResourceScopeRegistry::snapshot(). Read-only admin view.
    void scopeResources(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );

    // #43 resource-scope authorization: explicit, order-independent endpoint
    // + scope-requirement registration (replaces the former file-scope
    // static-init struct, defect 1.1 SIOF). Called from main()/test_main()
    // alongside the other controllers' initApiDocs().
    static void initApiDocs();

  private:
    static void initApiDocsImpl();
};

}  // namespace fulla::drogon::controllers
