#pragma once

// v1.4.0 open platform (v1.4.0-open-platform-design.md): self-service OAuth2
// client registration for end users — product-level concern (same rationale
// as the organization module; design.md §5.4). The protocol itself is NOT
// new: a registered "application" is a plain oauth2_clients row (RP
// registration); this service adds ownership, governance and quota checks
// around creating/maintaining one.
//
// Gates (OpenPlatformConfig, fail-closed): enabled=false disables creation;
// require_org restricts creation to org owner/admins; per-creator/per-org
// quotas; 24h rolling creation rate limit. Scope selection is restricted to
// oauth2_scopes rows flagged self_service (V035; FALSE by default).
//
// Ownership model (ratified 2026-09-17): creator_user_id is an immutable
// audit anchor; org_id (nullable) is the management anchor. Personal app
// (org_id NULL) is managed by the creator; an org app is managed by that
// org's owner/admin members. Secrets are shown exactly once (create/rotate).

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>
#include <memory>
#include <string>

namespace openplatform
{

class ApplicationService
{
  public:
    using ResponseCallback =
      std::shared_ptr<std::function<void(const ::drogon::HttpResponsePtr &)>>;

    /// GET /api/me/applications — personal apps + org apps I can manage.
    static void list(const ::drogon::HttpRequestPtr &req, ResponseCallback cb);

    /// POST /api/me/applications — create (gated); CONFIDENTIAL secret is
    /// returned exactly once.
    static void create(const ::drogon::HttpRequestPtr &req, ResponseCallback cb);

    /// PATCH /api/me/applications/{clientId} — name / redirect_uris / scopes
    /// / allowed_grant_types (whitelist subset; status is NOT editable here).
    static void update(
      const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &clientId
    );

    /// POST /api/me/applications/{clientId}/rotate-secret — CONFIDENTIAL only;
    /// old secret invalidated immediately.
    static void rotateSecret(
      const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &clientId
    );

    /// POST /api/me/applications/{clientId}/transfer — body {org_slug: "..."}
    /// or {org_slug: null} (transfer to personal). client_id/consents/tokens
    /// are preserved.
    static void transfer(
      const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &clientId
    );

    /// DELETE /api/me/applications/{clientId} — soft delete (V035
    /// oauth2_clients.deleted_at); the client disappears from every flow.
    static void remove(
      const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &clientId
    );

    /// POST /api/admin/clients/{clientId}/suspend|resume — governance
    /// (AuthorizationFilter-guarded routes); self-registered clients only.
    static void setStatus(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &clientId,
      bool suspended
    );
};

}  // namespace openplatform
