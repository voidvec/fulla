#pragma once

// M5 Task 29a (fulla-sdk-refactor): the audit-log + dashboard routes
// (`/api/admin/logs` + `/api/admin/dashboard*`) carved out of the former
// AdminController (design.md §5.8 / Task 29). These are the final 3 routes
// that remained in AdminController; once moved, AdminController is empty and
// is removed. Verbatim move -- behavior unchanged, Admin API tests must stay
// green. The raw-SQL -> ORM Mapper + business-logic-to-service extraction is
// deferred to Task 29b.

#include <drogon/HttpController.h>

namespace fulla::drogon::controllers
{

class AuditController : public ::drogon::HttpController<AuditController, false>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(
      AuditController::dashboard,
      "/api/admin/dashboard",
      ::drogon::Get,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    // Audit Logs
    ADD_METHOD_TO(
      AuditController::listLogs,
      "/api/admin/logs",
      ::drogon::Get,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    // P1-6 (#283): CSV export of the audit trail.
    ADD_METHOD_TO(
      AuditController::exportLogs,
      "/api/admin/logs/export",
      ::drogon::Get,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    // Dashboard Stats
    ADD_METHOD_TO(
      AuditController::getDashboardStats,
      "/api/admin/dashboard/stats",
      ::drogon::Get,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    METHOD_LIST_END

    void listLogs(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );

    void exportLogs(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );

    void getDashboardStats(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );

    void dashboard(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );

    // #43 resource-scope authorization: explicit, order-independent endpoint
    // + scope-requirement registration (replaces the former file-scope
    // static-init struct, defect 1.1 SIOF). Called from main()/test_main()
    // alongside the other controllers' initApiDocs().
    static void initApiDocs();

  private:
    static void initApiDocsImpl();
};

}  // namespace fulla::drogon::controllers
