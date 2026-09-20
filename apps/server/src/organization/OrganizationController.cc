#include "OrganizationController.h"
#include "OrganizationService.h"
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>

#include <memory>

// M5 Task 29b batch 3 (fulla-sdk-refactor): inline raw-SQL DB access from
// the Task 30 verbatim move is now delegated to OrganizationService (Mapper +
// Criteria, per .claude/rules/db-operations.md). Controller is now a thin HTTP
// adapter. Behavior equivalent (org CRUD routes/tests must stay green).

namespace organization
{
namespace
{
namespace openapi = ::fulla::drogon::observability::openapi;

// #43 resource-scope authorization: org-admin routes are part of the identity
// management family -> guarded by roles:read / roles:write, impliedBy admin.
// (The old docs struct registered dead /api/orgs endpoints that had no
// backing ADD_METHOD_TO routes -- removed.)
openapi::EndpointInfo orgEp(
  const char *path, const char *method, const char *summary, const char *description,
  std::vector<std::string> requiredScopes)
{
    openapi::EndpointInfo ep;
    ep.path = path;
    ep.method = method;
    ep.summary = summary;
    ep.description = description;
    ep.tags = {"Admin", "Organization"};
    ep.requiresAuth = true;
    ep.requiredScopes = std::move(requiredScopes);
    ep.impliedBy = {"admin"};
    return ep;
}
}  // namespace

void OrganizationController::initApiDocs()
{
    static std::once_flag docsOnce;
    std::call_once(docsOnce, [] { initApiDocsImpl(); });
}

void OrganizationController::initApiDocsImpl()
{
    openapi::OpenApiGenerator::addEndpoint(
      orgEp("/api/admin/organizations", "GET", "List Organizations",
            "List all organizations.", {"roles:read"}));
    openapi::OpenApiGenerator::addEndpoint(
      orgEp("/api/admin/organizations", "POST", "Create Organization",
            "Create a new organization.", {"roles:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      orgEp("/api/admin/organizations/{slug}", "GET", "Get Organization",
            "Get details of a specific organization by slug.", {"roles:read"}));
    openapi::OpenApiGenerator::addEndpoint(
      orgEp("/api/admin/organizations/{slug}/transfer-ownership", "POST",
            "Transfer Organization Ownership",
            "Reassign the organization's owner seat to a live user (#221 admin "
            "override). Body: {user_id}. The target is promoted (or added) as "
            "owner; previous owner rows are demoted to admin.",
            {"roles:write"}));
}

void OrganizationController::list(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    OrganizationService::list(req, sharedCb);
}

void OrganizationController::create(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    OrganizationService::create(req, sharedCb);
}

void OrganizationController::getBySlug(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &slug
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    OrganizationService::getBySlug(req, sharedCb, slug);
}


void OrganizationController::transferOwnership(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &slug
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    OrganizationService::transferOwnership(req, sharedCb, slug);
}

}  // namespace organization
