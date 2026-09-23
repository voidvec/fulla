#include "OrgMemberController.h"
#include "OrgMemberService.h"

#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>

#include <memory>

namespace organization
{
namespace
{
namespace openapi = ::fulla::drogon::observability::openapi;

// /api/me family rules: `profile` scope required, NO impliedBy — a bare
// admin token does not satisfy a self-service route (same comment as
// UserSelfServiceController).
openapi::EndpointInfo orgMemberEp(
  const char *path, const char *method, const char *summary, const char *description
)
{
    openapi::EndpointInfo ep;
    ep.path = path;
    ep.method = method;
    ep.summary = summary;
    ep.description = description;
    ep.tags = {"Organization"};
    ep.requiresAuth = true;
    ep.requiredScopes = {"profile"};
    return ep;
}
}  // namespace

void OrgMemberController::initApiDocs()
{
    static std::once_flag docsOnce;
    std::call_once(docsOnce, [] { initApiDocsImpl(); });
}

void OrgMemberController::initApiDocsImpl()
{
    openapi::OpenApiGenerator::addEndpoint(orgMemberEp(
      "/api/me/organizations", "GET", "List My Organizations",
      "List the current user's organization memberships (with the caller's org role)."));
    openapi::OpenApiGenerator::addEndpoint(orgMemberEp(
      "/api/me/organizations", "POST", "Create Organization (self-service)",
      "Create an organization; the caller becomes its owner. Gated by the "
      "open_platform config (max_orgs_per_user quota, reserved-slug list)."));
    openapi::OpenApiGenerator::addEndpoint(orgMemberEp(
      "/api/me/organizations/{slug}/members", "GET", "List Organization Members",
      "List members of an organization (any member may view)."));
    openapi::OpenApiGenerator::addEndpoint(orgMemberEp(
      "/api/me/organizations/{slug}/members/{userId}", "DELETE", "Remove Organization Member",
      "Remove a member. Owner removes anyone except self; admins remove "
      "members; members may only remove themselves (leave). The owner seat "
      "cannot be removed."));
    openapi::OpenApiGenerator::addEndpoint(orgMemberEp(
      "/api/me/organizations/{slug}/invitations", "POST", "Invite Organization Member",
      "Create a single-use 72h invitation (owner/admin). The token is returned "
      "once and must be delivered out-of-band; email delivery is not part of "
      "v1.4.0."));
    openapi::OpenApiGenerator::addEndpoint(orgMemberEp(
      "/api/me/organizations/{slug}/invitations", "GET", "List Pending Invitations",
      "List pending (unaccepted, unexpired-tracking) invitations (owner/admin)."));
    openapi::OpenApiGenerator::addEndpoint(orgMemberEp(
      "/api/me/organizations/{slug}/invitations/{invitationId}", "DELETE", "Revoke Invitation",
      "Revoke a pending invitation (owner/admin)."));
    openapi::OpenApiGenerator::addEndpoint(orgMemberEp(
      "/api/me/org-invitations/accept", "POST", "Accept Organization Invitation",
      "Accept an invitation by token. The caller's account email must match "
      "the invitation email (normalized)."));
    openapi::OpenApiGenerator::addEndpoint(orgMemberEp(
      "/api/me/organizations/{slug}/consents", "GET", "List Organization Consents",
      "Active organization consents grouped by client, each scope with its "
      "granted_by/granted_at (owner/admin)."));
    openapi::OpenApiGenerator::addEndpoint(orgMemberEp(
      "/api/me/organizations/{slug}/consents/{clientId}", "DELETE",
      "Revoke Organization Consents",
      "Revoke every active consent of the (org, client) pair (owner/admin). "
      "Revocation only affects FUTURE authorizations; issued tokens are not "
      "revoked (O4)."));
}

void OrgMemberController::createOrg(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    OrgMemberService::createOrg(req, sharedCb);
}

void OrgMemberController::listMyOrgs(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    OrgMemberService::listMyOrgs(req, sharedCb);
}

void OrgMemberController::listMembers(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &slug
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    OrgMemberService::listMembers(req, sharedCb, slug);
}

void OrgMemberController::removeMember(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &slug,
  const std::string &userId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    OrgMemberService::removeMember(req, sharedCb, slug, userId);
}

void OrgMemberController::createInvitation(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &slug
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    OrgMemberService::createInvitation(req, sharedCb, slug);
}

void OrgMemberController::listInvitations(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &slug
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    OrgMemberService::listInvitations(req, sharedCb, slug);
}

void OrgMemberController::revokeInvitation(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &slug,
  const std::string &invitationId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    OrgMemberService::revokeInvitation(req, sharedCb, slug, invitationId);
}

void OrgMemberController::acceptInvitation(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    OrgMemberService::acceptInvitation(req, sharedCb);
}

void OrgMemberController::listOrgConsents(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &slug
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    OrgMemberService::listOrgConsents(req, sharedCb, slug);
}

void OrgMemberController::revokeOrgConsents(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &slug,
  const std::string &clientId
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    OrgMemberService::revokeOrgConsents(req, sharedCb, slug, clientId);
}

}  // namespace organization
