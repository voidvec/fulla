#pragma once

// P1-2 (#278): application service for the email-template admin API.
//
// Backs EmailTemplateAdminController:
//   GET  /api/admin/email-templates              — list all (key x locale)
//   GET  /api/admin/email-templates/{key}/{locale}
//   PUT  /api/admin/email-templates/{key}/{locale}     (content-policy gate)
//   POST /api/admin/email-templates/{key}/{locale}/test-send
//
// Storage: Postgres-only. Memory deployments get a clean 404 (the
// getDbClient() Debug-assert hazard — guarded before any DB touch).
//
// test-send protections (design §P1-2 item 6, external verification A3):
//   * recipient whitelist: the admin's own address or email.test_recipients;
//   * rolling-24h per-admin quota (count of audit rows action=
//     'email_test_sent' created within now-86400) — modeled on the
//     open_platform inline implementation (ApplicationService), NOT the
//     failure-only RateLimiter (recordSuccess clears its bucket, useless
//     for always-succeeding sends);
//   * sample vars are synthetic (no real user PII).

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>
#include <memory>
#include <string>

namespace fulla::drogon::admin
{

class EmailTemplateAdminService
{
  public:
    using ResponseCallback =
      std::shared_ptr<std::function<void(const ::drogon::HttpResponsePtr &)>>;

    static void listTemplates(const ::drogon::HttpRequestPtr &req, ResponseCallback cb);

    static void getTemplate(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &key,
      const std::string &locale
    );

    static void upsertTemplate(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &key,
      const std::string &locale
    );

    static void testSend(
      const ::drogon::HttpRequestPtr &req,
      ResponseCallback cb,
      const std::string &key,
      const std::string &locale
    );
};

}  // namespace fulla::drogon::admin
