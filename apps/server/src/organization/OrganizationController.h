#pragma once

// M5 Task 30 (fulla-sdk-refactor): Organization management relocated from
// libs/drogon (fulla::drogon::controllers) into the product app
// (apps/server/src/organization/, namespace `organization`). Per design.md
// §5.4, Organization management is a PRODUCT-level concern (multi-tenant org
// CRUD), NOT part of the reusable protocol-engine/identity SDK, so it does
// not belong in libs/drogon. Verbatim move -- behavior unchanged (the org
// CRUD tests / routes must stay equivalent). The raw-SQL -> ORM Mapper +
// business-logic-to-service extraction remains deferred (same debt as the
// other admin controllers, Task 29b).
//
// This controller still depends on the SDK's public surface (OpenApiGenerator,
// ErrorResponder, AuditLogger), which the product app links via
// fulla::drogon / oauth2 -- correct dependency direction (product -> SDK).

#include <drogon/HttpController.h>

namespace organization
{

class OrganizationController : public ::drogon::HttpController<OrganizationController, false>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(
      OrganizationController::list,
      "/api/admin/organizations",
      ::drogon::Get,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    ADD_METHOD_TO(
      OrganizationController::create,
      "/api/admin/organizations",
      ::drogon::Post,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    ADD_METHOD_TO(
      OrganizationController::getBySlug,
      "/api/admin/organizations/{slug}",
      ::drogon::Get,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    // #221 admin override: reassign an org's owner seat (minimal slice; the
    // self-service nominate-and-accept workflow is v1.5.0 scope).
    ADD_METHOD_TO(
      OrganizationController::transferOwnership,
      "/api/admin/organizations/{slug}/transfer-ownership",
      ::drogon::Post,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    METHOD_LIST_END

    void list(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void create(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void getBySlug(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug
    );

    void transferOwnership(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug
    );

    /// #43 resource-scope authorization: declare org-admin routes' scope
    /// requirements (same initApiDocs pattern as the libs/drogon controllers).
    static void initApiDocs();

  private:
    static void initApiDocsImpl();
};

}  // namespace organization
