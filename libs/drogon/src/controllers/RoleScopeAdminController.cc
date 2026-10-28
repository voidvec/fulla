#include <fulla/drogon/controllers/RoleScopeAdminController.h>
#include <fulla/drogon/admin/RoleScopeAdminService.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>
#include <fulla/drogon/authz/ResourceScopeRegistry.h>

#include <memory>

// M5 Task 29b batch 4 (fulla-sdk-refactor): inline raw-SQL DB access from
// the Task 29a verbatim move is now delegated to RoleScopeAdminService (Mapper
// + Criteria, per .claude/rules/db-operations.md). The listRoles JOIN+GROUP-BY
// +COUNT is split into separate Mapper queries (JOIN-in-one-query forbidden).
// Controller is now a thin HTTP adapter. Behavior equivalent (Admin API tests
// must stay green).

namespace fulla::drogon::controllers
{

namespace
{
namespace openapi = ::fulla::drogon::observability::openapi;

// #43 resource-scope authorization: declare one EndpointInfo with its
// requiredScopes + impliedBy. All role/scope-admin routes are admin-gated; the
// `admin` super-scope (in impliedBy) satisfies any of them. `tags` is a
// parameter because this controller mixes the Scopes and Roles tag groups.
openapi::EndpointInfo adminEp(
  const char *path,
  const char *method,
  const char *summary,
  const char *description,
  std::vector<std::string> tags,
  std::vector<std::string> requiredScopes)
{
    openapi::EndpointInfo ep;
    ep.path = path;
    ep.method = method;
    ep.summary = summary;
    ep.description = description;
    ep.tags = std::move(tags);
    ep.requiresAuth = true;
    ep.requiredScopes = std::move(requiredScopes);
    ep.impliedBy = {"admin"};
    return ep;
}
}  // namespace

void RoleScopeAdminController::initApiDocs()
{
    static std::once_flag docsOnce;
    std::call_once(docsOnce, [] { initApiDocsImpl(); });
}

void RoleScopeAdminController::initApiDocsImpl()
{
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/scopes", "GET", "List Scopes",
              "Get a list of all available scopes.", {"Admin", "Scopes"}, {"roles:read"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/scopes", "POST", "Create Scope",
              "Create a new OAuth2 scope.", {"Admin", "Scopes"}, {"roles:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/scopes/{scopeId}", "PUT", "Update Scope",
              "Update a scope's properties.", {"Admin", "Scopes"}, {"roles:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/scopes/{scopeId}", "DELETE", "Delete Scope",
              "Delete a scope. Built-in scopes cannot be deleted.", {"Admin", "Scopes"},
              {"roles:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/roles", "GET", "List Roles",
              "Get a list of all roles with user counts.", {"Admin", "Roles"}, {"roles:read"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/roles", "POST", "Create Role",
              "Create a new role. Built-in roles cannot be duplicated.", {"Admin", "Roles"},
              {"roles:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/roles/{roleId}", "PUT", "Update Role",
              "Update a role's description.", {"Admin", "Roles"}, {"roles:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/roles/{roleId}/permissions", "GET", "List Role Permissions",
              "P1-8 (#285): the role's permission set (delegated administration).",
              {"Admin", "Roles"}, {"roles:read"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/roles/{roleId}/permissions", "PUT", "Replace Role Permissions",
              "P1-8 (#285): FULL-REPLACE the role's permission set. Every listed "
              "name must exist in the permissions catalog; unknown names fail "
              "the whole replace. Audited as role_permissions_updated; the "
              "role-permission cache entry is invalidated on commit.",
              {"Admin", "Roles"}, {"roles:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/roles/{roleId}", "DELETE", "Delete Role",
              "Delete a role. Built-in roles (admin, user) cannot be deleted.", {"Admin", "Roles"},
              {"roles:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/claim-mappings", "GET", "List Claim Mappings",
              "P1-3 (#280): all claim mappings (id_token/userinfo claim publication).",
              {"Admin", "Scopes"}, {"roles:read"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/claim-mappings", "POST", "Create Claim Mapping",
              "P1-3 (#280): map a source (custom_attributes.<key> or user.display_name) "
              "to a published claim under a trigger scope. Red lines: claim_name may "
              "not be reserved; scope_name may not be openid (CLAIM_MAPPING_* codes).",
              {"Admin", "Scopes"}, {"roles:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/claim-mappings/{mappingId}", "PUT", "Update Claim Mapping",
              "P1-3 (#280): update a mapping (full body).",
              {"Admin", "Scopes"}, {"roles:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/claim-mappings/{mappingId}", "DELETE", "Delete Claim Mapping",
              "P1-3 (#280): delete a mapping.",
              {"Admin", "Scopes"}, {"roles:write"}));

    // #43 discovery endpoint.
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/scopes/resources", "GET", "Scope-Resource Matrix",
              "Discovery: the (path, method) -> required-scopes authorization matrix.",
              {"Admin", "Scopes"}, {"roles:read"}));
}

using RoleScopeService = ::fulla::drogon::admin::RoleScopeAdminService;

void RoleScopeAdminController::listRoles(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    RoleScopeService::listRoles(req, sharedCb);
}

void RoleScopeAdminController::createRole(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    RoleScopeService::createRole(req, sharedCb);
}

void RoleScopeAdminController::updateRole(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &roleId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    RoleScopeService::updateRole(req, sharedCb, roleId);
}

void RoleScopeAdminController::deleteRole(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &roleId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    RoleScopeService::deleteRole(req, sharedCb, roleId);
}

// P1-8 (#285): role->permission administration adapters.
void RoleScopeAdminController::getRolePermissions(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &roleId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    RoleScopeService::getRolePermissions(req, sharedCb, roleId);
}

void RoleScopeAdminController::putRolePermissions(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &roleId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    RoleScopeService::putRolePermissions(req, sharedCb, roleId);
}


// ---------------------------------------------------------------------------
// P1-3 (#280): claim-mapping CRUD adapters.
// ---------------------------------------------------------------------------

void RoleScopeAdminController::listClaimMappings(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    RoleScopeService::listClaimMappings(req, sharedCb);
}

void RoleScopeAdminController::createClaimMapping(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    RoleScopeService::createClaimMapping(req, sharedCb);
}

void RoleScopeAdminController::updateClaimMapping(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &mappingId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    RoleScopeService::updateClaimMapping(req, sharedCb, mappingId);
}

void RoleScopeAdminController::deleteClaimMapping(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &mappingId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    RoleScopeService::deleteClaimMapping(req, sharedCb, mappingId);
}

void RoleScopeAdminController::listScopes(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    RoleScopeService::listScopes(req, sharedCb);
}

void RoleScopeAdminController::createScope(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    RoleScopeService::createScope(req, sharedCb);
}

void RoleScopeAdminController::updateScope(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &scopeId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    RoleScopeService::updateScope(req, sharedCb, scopeId);
}

void RoleScopeAdminController::deleteScope(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &scopeId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    RoleScopeService::deleteScope(req, sharedCb, scopeId);
}

void RoleScopeAdminController::scopeResources(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    (void)req;  // discovery endpoint needs no request data
    // #43 discovery: return the (path, method) -> required-scopes matrix from
    // the central ResourceScopeRegistry. Read-only; no DB access needed.
    Json::Value resources(Json::arrayValue);
    for (const auto &entry : fulla::drogon::authz::ResourceScopeRegistry::snapshot())
    {
        Json::Value e;
        e["path"] = entry.path;
        e["method"] = entry.method;
        Json::Value scopes(Json::arrayValue);
        for (const auto &s : entry.requirement.scopes)
            scopes.append(s);
        e["required_scopes"] = scopes;
        Json::Value implied(Json::arrayValue);
        for (const auto &s : entry.requirement.impliedBy)
            implied.append(s);
        e["implied_by"] = implied;
        resources.append(e);
    }
    Json::Value body;
    body["resources"] = resources;
    auto resp = ::drogon::HttpResponse::newHttpJsonResponse(body);
    callback(resp);
}

}  // namespace fulla::drogon::controllers
