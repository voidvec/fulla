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

    /// POST /api/admin/organizations/{slug}/transfer-ownership — #221 admin
    /// override: reassign the org's single owner seat to a live user (system
    /// admin surface; the self-service nominate-and-accept workflow is
    /// v1.5.0 scope). Target may be a non-member (membership is created) or
    /// an existing member (role promoted). Any previous owner row is demoted
    /// to 'admin' so the single-owner invariant holds after the call.
    static void transferOwnership(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &slug
    );
};

}  // namespace organization
