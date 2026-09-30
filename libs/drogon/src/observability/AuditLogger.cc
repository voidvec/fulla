#include <fulla/drogon/observability/AuditLogger.h>
#include <drogon/drogon.h>
#include <fulla/drogon/adapters/DrogonLogger.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/storage/postgres/models/AuditLogs.h>

namespace fulla::drogon::observability
{

namespace
{
fulla::common::ports::ILogger &logger()
{
    static fulla::drogon::adapters::DrogonLogger instance;
    return instance;
}
}  // namespace

void AuditLogger::log(const fulla::common::observability::AuditEvent &event)
{
    // Skip if storage type is memory
    auto plugin = ::drogon::app().getPlugin<OAuth2Plugin>();
    if (plugin && plugin->getStorageType() == "memory")
    {
        return;
    }
    // Async write to database via ORM Mapper - fire and forget (don't block main flow)
    try
    {
        auto db = ::drogon::app().getDbClient();
        if (!db)
        {
            logger().log(
              fulla::common::ports::LogLevel::Warn,
              "AuditLogger: No DB client, logging to console only"
            );
            logger().log(
              fulla::common::ports::LogLevel::Info,
              "[AUDIT] " + event.action + " " + event.outcome + " actor=" + event.actorType + ":" +
                event.actorId + " target=" + event.targetType + ":" + event.targetId
            );
            return;
        }

        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        std::string detailsStr =
          event.details.isNull() ? "{}" : Json::writeString(writer, event.details);

        drogon_model::fulla_db::AuditLogs auditLog;
        auditLog.setActorType(event.actorType);
        auditLog.setActorId(event.actorId);
        auditLog.setAction(event.action);
        auditLog.setTargetType(event.targetType);
        auditLog.setTargetId(event.targetId);
        auditLog.setOutcome(event.outcome);
        auditLog.setIp(event.ip);
        auditLog.setUserAgent(event.userAgent);
        auditLog.setRequestId(event.requestId);
        auditLog.setDetails(detailsStr);
        // V036 org dimension: org-scoped actions carry the numeric org id
        // (string in AuditEvent; stoi is safe — writers pass std::to_string
        // of an int32). Empty/non-numeric leaves the column NULL.
        if (!event.orgId.empty())
        {
            try
            {
                auditLog.setOrgId(std::stoi(event.orgId));
            }
            catch (...)
            {
            }
        }

        auto sharedCb =
          std::make_shared<std::function<void(const ::drogon::orm::DrogonDbException &)>>(
            [action = event.action](const ::drogon::orm::DrogonDbException &e) {
                logger().log(
                  fulla::common::ports::LogLevel::Warn,
                  "AuditLogger: Mapper insert FAILED: " + std::string(e.base().what()) +
                    " (action=" + action + ")"
                );
            }
          );

        LOG_DEBUG << "[AuditLogger] Starting Mapper::insert for action=" << event.action;

        ::drogon::orm::Mapper<drogon_model::fulla_db::AuditLogs> mapper(db);
        mapper.insert(
          auditLog,
          [action = event.action](const drogon_model::fulla_db::AuditLogs &) {
              LOG_DEBUG << "[AuditLogger] Mapper::insert OK for action=" << action;
          },
          [sharedCb](const ::drogon::orm::DrogonDbException &e) { (*sharedCb)(e); }
        );
    }
    catch (const std::exception &e)
    {
        logger().log(
          fulla::common::ports::LogLevel::Warn,
          "AuditLogger: Exception: " + std::string(e.what())
        );
    }
}

}  // namespace fulla::drogon::observability
