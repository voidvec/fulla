#pragma once

// P1-2 (#278): thin HTTP adapter for the email-template admin API
// (EmailTemplateAdminService does the work). Admin-role gating rides the
// AuthorizationFilter rbac_rules (/api/admin/.* -> admin); per-route scope
// requirements are declared via initApiDocs (roles:read / roles:write,
// impliedBy admin).

#include <drogon/HttpController.h>

#include <string>

namespace fulla::drogon::controllers
{

class EmailTemplateAdminController : public ::drogon::HttpController<EmailTemplateAdminController, false>
{
  public:
    static void initApiDocs();

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(
      EmailTemplateAdminController::listTemplates, "/api/admin/email-templates", ::drogon::Get,
      "fulla::drogon::filters::AuthorizationFilter");
    ADD_METHOD_TO(
      EmailTemplateAdminController::getTemplate, "/api/admin/email-templates/{key}/{locale}",
      ::drogon::Get, "fulla::drogon::filters::AuthorizationFilter");
    ADD_METHOD_TO(
      EmailTemplateAdminController::upsertTemplate, "/api/admin/email-templates/{key}/{locale}",
      ::drogon::Put, "fulla::drogon::filters::AuthorizationFilter");
    ADD_METHOD_TO(
      EmailTemplateAdminController::testSend,
      "/api/admin/email-templates/{key}/{locale}/test-send", ::drogon::Post,
      "fulla::drogon::filters::AuthorizationFilter");
    METHOD_LIST_END

    void listTemplates(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void getTemplate(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &key,
      const std::string &locale
    );
    void upsertTemplate(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &key,
      const std::string &locale
    );
    void testSend(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &key,
      const std::string &locale
    );

  private:
    static void initApiDocsImpl();
};

}  // namespace fulla::drogon::controllers
