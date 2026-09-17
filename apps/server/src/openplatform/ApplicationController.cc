#include "ApplicationController.h"
#include "ApplicationService.h"

#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>

#include <memory>

namespace openplatform
{
namespace
{
namespace openapi = ::fulla::drogon::observability::openapi;

openapi::EndpointInfo meAppEp(
  const char *path, const char *method, const char *summary, const char *description
)
{
    openapi::EndpointInfo ep;
    ep.path = path;
    ep.method = method;
    ep.summary = summary;
    ep.description = description;
    ep.tags = {"Applications"};
    ep.requiresAuth = true;
    ep.requiredScopes = {"profile"};
    return ep;
}

openapi::EndpointInfo adminAppEp(
  const char *path, const char *method, const char *summary, const char *description
)
{
    openapi::EndpointInfo ep;
    ep.path = path;
    ep.method = method;
    ep.summary = summary;
    ep.description = description;
    ep.tags = {"Admin", "Applications"};
    ep.requiresAuth = true;
    ep.requiredScopes = {"clients:write"};
    ep.impliedBy = {"admin"};
    return ep;
}
}  // namespace

void ApplicationController::initApiDocs()
{
    static std::once_flag docsOnce;
    std::call_once(docsOnce, [] { initApiDocsImpl(); });
}

void ApplicationController::initApiDocsImpl()
{
    openapi::OpenApiGenerator::addEndpoint(meAppEp(
      "/api/me/applications", "GET", "List My Applications",
      "List the caller's self-registered applications (personal plus org apps "
      "the caller can manage). Secrets are never included."));
    openapi::OpenApiGenerator::addEndpoint(meAppEp(
      "/api/me/applications", "POST", "Register Application (self-service)",
      "Register an OAuth2/OIDC application (RP self-registration). Gated by "
      "the open_platform config: enabled, require_org, per-user/per-org "
      "quotas, a 24h creation rate limit, the self_service scope allowlist, "
      "and the grant-type whitelist. The client secret (CONFIDENTIAL) is "
      "returned exactly once."));
    openapi::OpenApiGenerator::addEndpoint(meAppEp(
      "/api/me/applications/{clientId}", "PATCH", "Update Application",
      "Update name / redirect_uris / allowed_grant_types / scopes of a "
      "self-registered application (personal: creator; org: org owner/admin)."));
    openapi::OpenApiGenerator::addEndpoint(meAppEp(
      "/api/me/applications/{clientId}/rotate-secret", "POST", "Rotate Application Secret",
      "Rotate the client secret of a CONFIDENTIAL self-registered application. "
      "The previous secret is invalidated immediately; the new secret is "
      "returned exactly once."));
    openapi::OpenApiGenerator::addEndpoint(meAppEp(
      "/api/me/applications/{clientId}/transfer", "POST", "Transfer Application",
      "Move the management anchor: {org_slug: \"...\"} to an organization the "
      "caller manages, or {org_slug: null} back to personal. client_id, "
      "existing consents and issued tokens are preserved."));
    openapi::OpenApiGenerator::addEndpoint(meAppEp(
      "/api/me/applications/{clientId}", "DELETE", "Delete Application",
      "Soft-delete a self-registered application; it disappears from every "
      "flow immediately (authorization, token, introspection)."));
    openapi::OpenApiGenerator::addEndpoint(adminAppEp(
      "/api/admin/clients/{clientId}/suspend", "POST", "Suspend Application",
      "Suspend a self-registered application (abuse response): client "
      "validation fails while suspended."));
    openapi::OpenApiGenerator::addEndpoint(adminAppEp(
      "/api/admin/clients/{clientId}/resume", "POST", "Resume Application",
      "Lift a suspension on a self-registered application."));
}

void ApplicationController::list(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    ApplicationService::list(req, sharedCb);
}

void ApplicationController::create(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    ApplicationService::create(req, sharedCb);
}

void ApplicationController::update(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &clientId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    ApplicationService::update(req, sharedCb, clientId);
}

void ApplicationController::rotateSecret(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &clientId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    ApplicationService::rotateSecret(req, sharedCb, clientId);
}

void ApplicationController::transfer(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &clientId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    ApplicationService::transfer(req, sharedCb, clientId);
}

void ApplicationController::remove(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &clientId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    ApplicationService::remove(req, sharedCb, clientId);
}

void ApplicationController::suspend(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &clientId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    ApplicationService::setStatus(req, sharedCb, clientId, true);
}

void ApplicationController::resume(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &clientId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    ApplicationService::setStatus(req, sharedCb, clientId, false);
}

}  // namespace openplatform
