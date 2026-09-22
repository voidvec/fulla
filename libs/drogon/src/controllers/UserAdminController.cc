#include <fulla/drogon/controllers/UserAdminController.h>
#include <fulla/drogon/admin/UserAdminService.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>

#include <memory>

// M5 Task 29b batch 5 (fulla-sdk-refactor): inline raw-SQL DB access from
// the Task 29a verbatim move is now delegated to UserAdminService (Mapper +
// Criteria, per .claude/rules/db-operations.md). The getUser 3-table JOIN +
// json_agg and getUserRoles JOIN are split into multiple Mapper queries
// (JOIN-in-one-query forbidden). Controller is now a thin HTTP adapter.

namespace fulla::drogon::controllers
{
namespace
{
namespace openapi = ::fulla::drogon::observability::openapi;

// #43 resource-scope authorization: declare one EndpointInfo with its
// requiredScopes + impliedBy. All user-admin routes are admin-gated; the
// `admin` super-scope (in impliedBy) satisfies any of them.
openapi::EndpointInfo adminEp(
  const char *path,
  const char *method,
  const char *summary,
  const char *description,
  std::vector<std::string> requiredScopes)
{
    openapi::EndpointInfo ep;
    ep.path = path;
    ep.method = method;
    ep.summary = summary;
    ep.description = description;
    ep.tags = {"Admin", "Users"};
    ep.requiresAuth = true;
    ep.requiredScopes = std::move(requiredScopes);
    ep.impliedBy = {"admin"};
    return ep;
}
}  // namespace

void UserAdminController::initApiDocs()
{
    static std::once_flag docsOnce;
    std::call_once(docsOnce, [] { initApiDocsImpl(); });
}

void UserAdminController::initApiDocsImpl()
{
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/users", "GET", "List Users",
              "List users with optional pagination (page, per_page) and filtering "
              "(q for username/email prefix, role, locked). Returns total count.",
              {"users:read"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/users", "POST", "Create User",
              "Create a new user. Requires username and password; email, roles, "
              "mfa_enabled, and email_verified are optional. The deprecated "
              "users.org_id column is read-only since v1.5.0: a body containing "
              "org_id is rejected with 400 (organization membership is managed "
              "via the organization APIs).",
              {"users:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/users/{userId}", "GET", "Get User Detail",
              "Get detailed information about a specific user including roles and account status.",
              {"users:read"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/users/{userId}", "PUT", "Update User",
              "Update user fields: email, email_verified, username, mfa_enabled, "
              "locked. Wrong-typed fields are rejected with 400. The deprecated "
              "users.org_id column is read-only since v1.5.0: a body containing "
              "org_id is rejected with 400 (organization membership is managed "
              "via the organization APIs).",
              {"users:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/users/{userId}", "DELETE", "Delete User",
              "Soft-delete a user account (sets deleted_at). The user is excluded "
              "from all queries and can no longer log in. Outstanding tokens are "
              "revoked before the response (tokens_revoked reported in the body); "
              "409 if the target is the last active admin.",
              {"users:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/users/{userId}/disable", "PUT", "Disable User",
              "Disable a specific user account.", {"users:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/users/{userId}/enable", "POST", "Enable User",
              "Enable a disabled user account by resetting lockout state.", {"users:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/users/{userId}/roles", "GET", "Get User Roles",
              "Get the roles assigned to a specific user.", {"users:read"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/users/{userId}/roles", "PUT", "Assign User Roles",
              "Assign roles to a specific user.", {"users:write"}));
}

using UserService = ::fulla::drogon::admin::UserAdminService;

void UserAdminController::listUsers(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    UserService::listUsers(req, sharedCb);
}

void UserAdminController::createUser(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    UserService::createUser(req, sharedCb);
}

void UserAdminController::disableUser(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &userId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    UserService::disableUser(req, sharedCb, userId);
}

void UserAdminController::assignUserRoles(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &userId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    UserService::assignUserRoles(req, sharedCb, userId);
}

void UserAdminController::getUser(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &userId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    UserService::getUser(req, sharedCb, userId);
}

void UserAdminController::updateUser(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &userId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    UserService::updateUser(req, sharedCb, userId);
}

void UserAdminController::deleteUser(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &userId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    UserService::deleteUser(req, sharedCb, userId);
}

void UserAdminController::enableUser(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &userId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    UserService::enableUser(req, sharedCb, userId);
}

void UserAdminController::getUserRoles(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &userId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    UserService::getUserRoles(req, sharedCb, userId);
}

}  // namespace fulla::drogon::controllers
