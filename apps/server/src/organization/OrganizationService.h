#pragma once

// M5 Task 29b batch 3 (fulla-sdk-refactor): application-service extraction
// for Organization management (product-level, namespace `organization`). The
// raw `db->execSqlAsync("SELECT/INSERT ...")` calls inline in
// OrganizationController are now Mapper<T> + Criteria operations on the ORM
// Organizations model (per .claude/rules/db-operations.md). Behavior is
// equivalent to the pre-29b controller.

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>
#include <memory>
#include <string>

namespace organization
{

/**
 * @brief Application service for Organization CRUD.
 *
 * Thin DB-access wrapper; the controller parses the request + dispatches here.
 * Lives at product level (matches the controller -- Organization is a product
 * concern per design.md §5.4, not part of the reusable SDK).
 */
class OrganizationService
{
  public:
    using ResponseCallback =
      std::shared_ptr<std::function<void(const ::drogon::HttpResponsePtr &)>>;

    static void list(const ::drogon::HttpRequestPtr &req, ResponseCallback cb);
    static void create(const ::drogon::HttpRequestPtr &req, ResponseCallback cb);
    static void getBySlug(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &slug
    );

    /// PATCH /api/admin/organizations/{slug} — P1-4 (#281): edit
    /// name/logo_uri/primary_color (slug immutable — external reference
    /// anchor). Reuses the P1-1 brand validators. Audited org_updated.
    static void updateOrg(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &slug
    );

    /// DELETE /api/admin/organizations/{slug} — P1-4 (#281): soft delete
    /// with the CLOSED cascade (one advisory-locked transaction): deleted_at
    /// + slug placeholder rewrite (slug || '#del-' || id — no slug reuse),
    /// pending invitations and succession nominations voided (physical
    /// delete — FK CASCADE never fires on soft delete, and a 72h accept
    /// would resurrect membership), org consents revoked_at, owned clients
    /// suspended (blocks new client credentials auth), memberships
    /// detached. Accept paths re-verify org-not-deleted via the
    /// findOrgBySlug filter. Audited org_deleted.
    static void deleteOrg(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &slug
    );

    /// GET /api/admin/organizations/{slug}/members — P1-4 (#281).
    static void listMembers(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &slug
    );

    /// DELETE /api/admin/organizations/{slug}/members/{userId} — P1-4
    /// (#281): admin removal (the owner seat must be vacated via
    /// transfer-ownership first; removing the last owner is rejected).
    static void removeMember(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &slug,
      const std::string &userId
    );

    /// POST /api/admin/organizations/{slug}/transfer-ownership — #221 admin
    /// override: reassign the org's single owner seat to a live user (system
    /// admin surface; the self-service nominate-and-accept workflow is
    /// v1.4.0 scope). Target may be a non-member (membership is created) or
    /// an existing member (role promoted). Any previous owner row is demoted
    /// to 'admin' so the single-owner invariant holds after the call.
    static void transferOwnership(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &slug
    );
};

}  // namespace organization
