#include <fulla/drogon/admin/EmailTemplateAdminService.h>

#include <fulla/drogon/adapters/DrogonAuditSink.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/services/EmailTemplateService.h>
#include <fulla/common/utils/EmailNormalizer.h>
#include <fulla/drogon/utils/EmailService.h>
#include <fulla/storage/postgres/models/AuditLogs.h>
#include <fulla/storage/postgres/models/EmailTemplates.h>
#include <fulla/storage/postgres/models/Users.h>

#include <drogon/drogon.h>
#include <trantor/utils/Date.h>

#include <cctype>
#include <vector>

namespace fulla::drogon::admin
{

namespace
{
using namespace ::drogon::orm;
// Sibling-namespace shorthand: the template engine lives in
// fulla::drogon::services (unqualified lookup cannot cross into it).
using fulla::drogon::services::EmailTemplateService;
using TemplateModel = ::drogon_model::fulla_db::EmailTemplates;
using AuditModel = ::drogon_model::fulla_db::AuditLogs;
using UserModel = ::drogon_model::fulla_db::Users;

void respondError(
  const ::drogon::HttpRequestPtr &req,
  const EmailTemplateAdminService::ResponseCallback &cb,
  std::string code,
  std::string detailForLog = ""
)
{
    ::fulla::common::error::ErrorResponder::respond(
      req,
      [cb](const ::drogon::HttpResponsePtr &r) { (*cb)(r); },
      std::move(code),
      std::move(detailForLog)
    );
}

void audit(
  const ::drogon::HttpRequestPtr &req,
  const char *action,
  const std::string &targetId
)
{
    auto *plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
    if (!plugin)
        return;
    std::string actorId;
    try
    {
        actorId = req->getAttributes()->get<std::string>("userId");
    }
    catch (...)
    {
    }
    ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
      plugin->getAuditSink(),
      action,
      "success",
      req,
      actorId,
      "email_template",
      targetId,
      Json::Value());
}

// Memory-mode guard: getDbClient() ABORTS in Debug builds when no db_clients
// are configured (the M1 pitfall). Memory deployments expose no admin API —
// clean 404 before any DB touch.
bool memoryMode()
{
    auto *p = ::drogon::app().getPlugin<::OAuth2Plugin>();
    return p != nullptr && p->getStorageType() == "memory";
}

std::vector<std::string> configuredTestRecipients()
{
    std::vector<std::string> out;
    auto config = ::drogon::app().getCustomConfig();
    if (config.isMember("email") && config["email"].isMember("test_recipients") &&
        config["email"]["test_recipients"].isArray())
    {
        for (const auto &r : config["email"]["test_recipients"])
            if (r.isString())
                out.push_back(r.asString());
    }
    return out;
}

int testQuotaPerDay()
{
    auto config = ::drogon::app().getCustomConfig();
    if (config.isMember("email") && config["email"].isMember("test_send_quota_per_day") &&
        config["email"]["test_send_quota_per_day"].isInt())
        return config["email"]["test_send_quota_per_day"].asInt();
    return 10;
}

bool validKey(const std::string &key)
{
    if (key.empty() || key.size() > 64)
        return false;
    for (char c : key)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    return true;
}

bool validLocale(const std::string &locale)
{
    // BCP47-light: 2 lower letters, optional "-xxxx" subtag (e.g. en, zh-CN).
    if (locale.empty() || locale.size() > 8)
        return false;
    if (!std::isalpha(static_cast<unsigned char>(locale[0])) ||
        !std::isalpha(static_cast<unsigned char>(locale[1])))
        return false;
    if (locale.size() == 2)
        return true;
    if (locale[2] != '-')
        return false;
    for (std::size_t i = 3; i < locale.size(); ++i)
        if (!std::isalnum(static_cast<unsigned char>(locale[i])))
            return false;
    return true;
}

Json::Value templateRowToJson(const TemplateModel &row)
{
    Json::Value t;
    t["template_key"] = row.getValueOfTemplateKey();
    t["locale"] = row.getValueOfLocale();
    t["subject"] = row.getValueOfSubject();
    t["body_html"] = row.getValueOfBodyHtml();
    t["body_text"] = row.getValueOfBodyText();
    t["updated_at"] = row.getValueOfUpdatedAt().toDbString();
    return t;
}
}  // namespace

void EmailTemplateAdminService::listTemplates(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb
)
{
    if (memoryMode())
    {
        respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "email templates require a database");
        return;
    }
    ::drogon::orm::DbClientPtr db;
    try
    {
        db = ::drogon::app().getDbClient();
    }
    catch (...)
    {
        respondError(req, cb, "DB_CONNECTION_ERROR", "Database unavailable");
        return;
    }

    try
    {
        Mapper<TemplateModel> mapper(db);
        mapper.findBy(
          Criteria(),
          [cb](const std::vector<TemplateModel> &rows) {
              Json::Value json;
              json["status"] = "success";
              Json::Value arr(Json::arrayValue);
              for (const auto &r : rows)
                  arr.append(templateRowToJson(r));
              json["templates"] = arr;
              json["total"] = static_cast<Json::Int64>(rows.size());
              (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
          },
          [req, cb](const DrogonDbException &e) {
              respondError(req, cb, "DB_QUERY_ERROR", std::string("list templates: ") + e.base().what());
          });
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "list templates: Mapper construction failed");
    }
}

void EmailTemplateAdminService::getTemplate(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &key,
  const std::string &locale
)
{
    if (!validKey(key) || !validLocale(locale))
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "invalid template key or locale");
        return;
    }
    if (memoryMode())
    {
        respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "email templates require a database");
        return;
    }
    ::drogon::orm::DbClientPtr db;
    try
    {
        db = ::drogon::app().getDbClient();
    }
    catch (...)
    {
        respondError(req, cb, "DB_CONNECTION_ERROR", "Database unavailable");
        return;
    }

    try
    {
        Mapper<TemplateModel> mapper(db);
        mapper.findBy(
          Criteria(TemplateModel::Cols::_template_key, CompareOperator::EQ, key) &&
            Criteria(TemplateModel::Cols::_locale, CompareOperator::EQ, locale),
          [req, cb, key, locale](const std::vector<TemplateModel> &rows) {
              if (rows.empty())
              {
                  respondError(
                    req, cb, "EMAIL_TEMPLATE_NOT_FOUND", "no row for " + key + "/" + locale);
                  return;
              }
              Json::Value json = templateRowToJson(rows.front());
              json["status"] = "success";
              (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
          },
          [req, cb](const DrogonDbException &e) {
              respondError(req, cb, "DB_QUERY_ERROR", std::string("get template: ") + e.base().what());
          });
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "get template: Mapper construction failed");
    }
}

void EmailTemplateAdminService::upsertTemplate(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &key,
  const std::string &locale
)
{
    if (!validKey(key) || !validLocale(locale))
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "invalid template key or locale");
        return;
    }
    if (memoryMode())
    {
        respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "email templates require a database");
        return;
    }

    auto json = req->getJsonObject();
    if (!json || !json->isMember("subject") || !json->isMember("body_html") ||
        !(*json)["subject"].isString() || !(*json)["body_html"].isString())
    {
        respondError(
          req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "subject and body_html are required");
        return;
    }
    const std::string subject = (*json)["subject"].asString();
    const std::string bodyHtml = (*json)["body_html"].asString();
    std::string bodyText;
    if (json->isMember("body_text") && (*json)["body_text"].isString())
        bodyText = (*json)["body_text"].asString();

    if (subject.empty() || subject.size() > 256)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "subject must be 1..256 chars");
        return;
    }
    if (bodyHtml.empty() || bodyHtml.size() > 65536 || bodyText.size() > 65536)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "bodies must be within length limits");
        return;
    }

    // Save-time static content policy (script/iframe/form, URL schemes,
    // outbound-href domain allowlist).
    std::string rejection = EmailTemplateService::validateTemplateContent(subject, bodyHtml, bodyText);
    if (!rejection.empty())
    {
        respondError(
          req, cb, "EMAIL_TEMPLATE_CONTENT_REJECTED", "template rejected: " + rejection);
        return;
    }

    ::drogon::orm::DbClientPtr db;
    try
    {
        db = ::drogon::app().getDbClient();
    }
    catch (...)
    {
        respondError(req, cb, "DB_CONNECTION_ERROR", "Database unavailable");
        return;
    }

    try
    {
        Mapper<TemplateModel> mapper(db);
        mapper.findBy(
          Criteria(TemplateModel::Cols::_template_key, CompareOperator::EQ, key) &&
            Criteria(TemplateModel::Cols::_locale, CompareOperator::EQ, locale),
          [req, cb, db, key, locale, subject, bodyHtml, bodyText](const std::vector<TemplateModel> &rows) {
              auto respondSaved = [req, cb, key, locale]() {
                  audit(req, "email_template_updated", key + "/" + locale);
                  Json::Value out;
                  out["status"] = "success";
                  out["message"] = "Template saved";
                  out["template_key"] = key;
                  out["locale"] = locale;
                  (*cb)(::drogon::HttpResponse::newHttpJsonResponse(out));
              };
              auto respondDbError = [req, cb](const DrogonDbException &e) {
                  respondError(
                    req, cb, "DB_QUERY_ERROR", std::string("upsert template: ") + e.base().what());
              };
              try
              {
                  Mapper<TemplateModel> writeMapper(db);
                  if (!rows.empty())
                  {
                      TemplateModel row = rows.front();
                      row.setSubject(subject);
                      row.setBodyHtml(bodyHtml);
                      row.setBodyText(bodyText);
                      row.setUpdatedAt(::trantor::Date::now());
                      writeMapper.update(
                        row,
                        [respondSaved](const std::size_t) { respondSaved(); },
                        respondDbError);
                      return;
                  }
                  TemplateModel row;
                  row.setTemplateKey(key);
                  row.setLocale(locale);
                  row.setSubject(subject);
                  row.setBodyHtml(bodyHtml);
                  row.setBodyText(bodyText);
                  row.setUpdatedAt(::trantor::Date::now());
                  writeMapper.insert(
                    row,
                    [respondSaved](const TemplateModel &) { respondSaved(); },
                    respondDbError);
              }
              catch (...)
              {
                  respondError(req, cb, "DB_QUERY_ERROR", "upsert template: Mapper construction failed");
              }
          },
          [req, cb](const DrogonDbException &e) {
              respondError(
                req, cb, "DB_QUERY_ERROR", std::string("upsert lookup: ") + e.base().what());
          });
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "upsert template: Mapper construction failed");
    }
}

void EmailTemplateAdminService::testSend(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &key,
  const std::string &locale
)
{
    if (!validKey(key) || !validLocale(locale))
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "invalid template key or locale");
        return;
    }
    if (memoryMode())
    {
        respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "email templates require a database");
        return;
    }

    auto json = req->getJsonObject();
    std::string to;
    if (json && json->isMember("to") && (*json)["to"].isString())
        to = (*json)["to"].asString();
    if (to.empty())
    {
        respondError(req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "to is required");
        return;
    }

    // The AuthorizationFilter sets "userId" to the OAuth2 subject; be
    // defensive anyway (an unset attribute would throw out of the handler
    // and drop the connection).
    std::string adminSub;
    try
    {
        adminSub = req->getAttributes()->get<std::string>("userId");
    }
    catch (...)
    {
    }

    ::drogon::orm::DbClientPtr db;
    try
    {
        db = ::drogon::app().getDbClient();
    }
    catch (...)
    {
        respondError(req, cb, "DB_CONNECTION_ERROR", "Database unavailable");
        return;
    }

    // Resolve the admin's own address for the whitelist, then gate the
    // recipient, then check the rolling-24h quota, then render + send.
    try
    {
        Mapper<UserModel> userMapper(db);
        userMapper.findOne(
          Criteria(UserModel::Cols::_public_sub, CompareOperator::EQ, adminSub),
          [req, cb, db, key, locale, to, adminSub](const UserModel &admin) {
              const std::string ownEmail = admin.getValueOfEmail();
              const auto allow = configuredTestRecipients();
              bool allowed = (::fulla::common::utils::normalizeEmail(to) == ownEmail);
              if (!allowed)
              {
                  for (const auto &r : allow)
                  {
                      if (::fulla::common::utils::normalizeEmail(to) ==
                          ::fulla::common::utils::normalizeEmail(r))
                      {
                          allowed = true;
                          break;
                      }
                  }
              }
              if (!allowed)
              {
                  respondError(
                    req,
                    cb,
                    "EMAIL_TEST_RECIPIENT_DENIED",
                    "test-send recipient must be your own address or in email.test_recipients");
                  return;
              }

              // Rolling-24h quota (external verification A3: mirror the
              // open_platform inline pattern, createdAt > now-86400 — NOT a
              // calendar day, and NOT the failure-only RateLimiter).
              const int64_t cutoff =
                ::trantor::Date::now().secondsSinceEpoch() - 86400;
              try
              {
                  Mapper<AuditModel> auditMapper(db);
                  auditMapper.count(
                    Criteria(AuditModel::Cols::_action, CompareOperator::EQ,
                             std::string("email_test_sent")) &&
                      Criteria(AuditModel::Cols::_actor_id, CompareOperator::EQ, adminSub) &&
                      Criteria(AuditModel::Cols::_timestamp, CompareOperator::GT,
                               ::trantor::Date(cutoff * 1000000)),
                    [req, cb, db, key, locale, to, adminSub](const std::size_t sent) {
                        if (static_cast<int>(sent) >= testQuotaPerDay())
                        {
                            respondError(
                              req,
                              cb,
                              "EMAIL_TEST_QUOTA_EXCEEDED",
                              "rolling-24h test-send quota reached; try again later");
                            return;
                        }

                        // Render with the full fallback chain; synthetic vars
                        // only (never real user PII).
                        std::map<std::string, std::string> vars = {
                          {"verify_link", "https://example.invalid/verify?token=sample"},
                          {"reset_link", "https://example.invalid/reset?token=sample"},
                          {"org_name", "Example Organization"},
                          {"invite_token", "sample-invitation-token"},
                        };
                        EmailTemplateService::render(
                          key,
                          locale,
                          vars,
                          [req, cb, key, locale, to, adminSub](EmailTemplateService::RenderedEmail email) {
                              if (email.subject.empty())
                              {
                                  respondError(
                                    req, cb, "EMAIL_TEMPLATE_NOT_FOUND",
                                    "no template for " + key + "/" + locale);
                                  return;
                              }
                              // The SMTP path fires its callback on a
                              // detached thread — marshal the response (and
                              // the audit write) back onto the event loop
                              // before touching req/resp.
                              auto loop = ::drogon::app().getLoop();
                              ::fulla::drogon::utils::getEmailService().sendEmail(
                                to,
                                email.subject,
                                email.bodyText,
                                email.bodyHtml,
                                [loop, req, cb, key, locale, to, email](bool ok) {
                                    loop->queueInLoop([req, cb, key, locale, to, email, ok]() {
                                        if (!ok)
                                        {
                                            respondError(
                                              req, cb, "NET_CONNECTION_FAILED",
                                              "test-send delivery failed");
                                            return;
                                        }
                                        audit(req, "email_test_sent", key + "/" + locale);
                                        Json::Value out;
                                        out["status"] = "success";
                                        out["message"] = "Test email sent";
                                        out["recipient"] = to;
                                        out["subject"] = email.subject;
                                        (*cb)(::drogon::HttpResponse::newHttpJsonResponse(out));
                                    });
                                });
                          });
                    },
                    [req, cb](const DrogonDbException &e) {
                        respondError(
                          req, cb, "DB_QUERY_ERROR",
                          std::string("test-send quota count: ") + e.base().what());
                    });
              }
              catch (...)
              {
                  respondError(req, cb, "DB_QUERY_ERROR", "test-send: Mapper construction failed");
              }
          },
          [req, cb](const DrogonDbException &) {
              // Admin user row unresolvable: the self-address leg of the
              // whitelist cannot be established (the configured
              // email.test_recipients leg still works after the admin user
              // row is fixed). Deny rather than guess.
              respondError(
                req, cb, "EMAIL_TEST_RECIPIENT_DENIED", "admin email unresolvable; use email.test_recipients");
          });
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "test-send: Mapper construction failed");
    }
}

}  // namespace fulla::drogon::admin
