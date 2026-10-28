#include <fulla/drogon/controllers/EmailTemplateAdminController.h>
#include <fulla/drogon/admin/EmailTemplateAdminService.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>

#include <memory>

namespace fulla::drogon::controllers
{

namespace
{
namespace openapi = ::fulla::drogon::observability::openapi;

// #43 resource-scope authorization: same shape as RoleScopeAdminController's
// adminEp — read routes need roles:read, mutations roles:write; the `admin`
// super-scope satisfies either via impliedBy.
openapi::EndpointInfo templateEp(
  const char *path,
  const char *method,
  const char *summary,
  const char *description,
  std::vector<std::string> requiredScopes
)
{
    openapi::EndpointInfo ep;
    ep.path = path;
    ep.method = method;
    ep.summary = summary;
    ep.description = description;
    ep.tags = {"Admin", "Email"};
    ep.requiresAuth = true;
    ep.requiredScopes = std::move(requiredScopes);
    ep.impliedBy = {"admin"};
    return ep;
}
}  // namespace

void EmailTemplateAdminController::initApiDocs()
{
    static std::once_flag docsOnce;
    std::call_once(docsOnce, [] { initApiDocsImpl(); });
}

void EmailTemplateAdminController::initApiDocsImpl()
{
    openapi::OpenApiGenerator::addEndpoint(
      templateEp("/api/admin/email-templates", "GET", "List Email Templates",
                 "List all email templates (template_key x locale).",
                 {"roles:read"}));
    openapi::OpenApiGenerator::addEndpoint(
      templateEp("/api/admin/email-templates/{key}/{locale}", "GET", "Get Email Template",
                 "Fetch one template by key and locale.",
                 {"roles:read"}));
    openapi::OpenApiGenerator::addEndpoint(
      templateEp("/api/admin/email-templates/{key}/{locale}", "PUT", "Upsert Email Template",
                 "Create or update a template. Content policy: no script/iframe/form tags, "
                 "outbound links only to configured email.allowed_link_domains.",
                 {"roles:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      templateEp("/api/admin/email-templates/{key}/{locale}/test-send", "POST",
                 "Test-send Email Template",
                 "Render a template with synthetic variables and send it to a whitelisted "
                 "recipient (admin's own address or email.test_recipients; rolling-24h quota).",
                 {"roles:write"}));
}

using TemplateAdminService = ::fulla::drogon::admin::EmailTemplateAdminService;

void EmailTemplateAdminController::listTemplates(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    TemplateAdminService::listTemplates(req, sharedCb);
}

void EmailTemplateAdminController::getTemplate(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &key,
  const std::string &locale
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    TemplateAdminService::getTemplate(req, sharedCb, key, locale);
}

void EmailTemplateAdminController::upsertTemplate(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &key,
  const std::string &locale
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    TemplateAdminService::upsertTemplate(req, sharedCb, key, locale);
}

void EmailTemplateAdminController::testSend(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &key,
  const std::string &locale
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    TemplateAdminService::testSend(req, sharedCb, key, locale);
}

}  // namespace fulla::drogon::controllers
