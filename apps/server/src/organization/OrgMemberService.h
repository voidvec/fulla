#pragma once

// v1.4.0 organization mainline (profile-orgs design doc §B): self-service
// organization membership — create/join/invite — product-level, same home as
// the admin OrganizationController (organization CRUD is a product concern
// per design.md §5.4).
//
// Model recap (V034):
//   organization_members    M:N user<->org, org-scoped role
//                           owner|admin|member (orthogonal to global RBAC).
//   organization_invitations single-use, 72h-expiring, lowercase-normalized
//                           email, one PENDING invite per (org, email).
//
// Permission rules (ratified 2026-09-17):
//   owner: manage everything except cannot remove/leave the org's single
//          owner seat (ownership transfer is out of scope for v1.4.0);
//   admin: manage members/invites below owner/admin level;
//   member: read-only + may leave (remove self).

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>
#include <memory>
#include <string>

namespace organization
{

class OrgMemberService
{
  public:
    using ResponseCallback =
      std::shared_ptr<std::function<void(const ::drogon::HttpResponsePtr &)>>;

    /// POST /api/me/organizations — self-serve org creation; creator becomes
    /// owner. Quota: open_platform.max_orgs_per_user (default 3).
    static void createOrg(const ::drogon::HttpRequestPtr &req, ResponseCallback cb);

    /// GET /api/me/organizations — memberships of the current user.
    static void listMyOrgs(const ::drogon::HttpRequestPtr &req, ResponseCallback cb);

    /// GET /api/me/organizations/{slug}/members — any member can view.
    static void listMembers(
      const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &slug
    );

    /// DELETE /api/me/organizations/{slug}/members/{userId} — role rules per
    /// class comment; the owner seat is irremovable.
    static void removeMember(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &slug,
      const std::string &userIdStr
    );

    /// POST /api/me/organizations/{slug}/invitations — owner/admin; body
    /// {email, role}; 72h expiry; returns the invite token once.
    static void createInvitation(
      const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &slug
    );

    /// GET /api/me/organizations/{slug}/invitations — pending only (owner/admin).
    static void listInvitations(
      const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &slug
    );

    /// DELETE /api/me/organizations/{slug}/invitations/{invitationId} — revoke
    /// a pending invite (owner/admin).
    static void revokeInvitation(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &slug,
      const std::string &invitationId
    );

    /// POST /api/me/organizations/invitations/accept — body {token}; requires
    /// the caller's verified-bound email to match the invite email.
    static void acceptInvitation(const ::drogon::HttpRequestPtr &req, ResponseCallback cb);
};

}  // namespace organization
