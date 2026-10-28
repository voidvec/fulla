#include <fulla/drogon/admin/AuditService.h>

#include <fulla/storage/postgres/models/AuditLogs.h>
#include <fulla/storage/postgres/models/Users.h>
#include <fulla/storage/postgres/models/Oauth2Clients.h>
#include <fulla/storage/postgres/models/Oauth2AccessTokens.h>
#include <fulla/drogon/adapters/DrogonAuditSink.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>

#include <drogon/drogon.h>
#include <trantor/utils/Date.h>

#include <algorithm>
#include <chrono>
#include <sstream>

namespace fulla::drogon::admin
{

namespace
{
void respondError(
  const ::drogon::HttpRequestPtr &req,
  const AuditService::ResponseCallback &cb,
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

using namespace ::drogon::orm;
using namespace ::drogon_model::fulla_db;

::drogon::orm::DbClientPtr getDbOrRespond(
  const ::drogon::HttpRequestPtr &req,
  const AuditService::ResponseCallback &cb
)
{
    try
    {
        return ::drogon::app().getDbClient();
    }
    catch (...)
    {
        respondError(req, cb, "DB_CONNECTION_ERROR", "Database unavailable");
        return nullptr;
    }
}

Json::Value logRowToJson(const AuditLogs &row)
{
    Json::Value log;
    log["id"] = row.getValueOfId();
    log["timestamp"] = row.getValueOfTimestamp().toDbString();
    log["actor_type"] = row.getValueOfActorType();
    log["actor_id"] = row.getValueOfActorId();
    log["action"] = row.getValueOfAction();
    log["target_type"] = row.getValueOfTargetType();
    log["target_id"] = row.getValueOfTargetId();
    log["outcome"] = row.getValueOfOutcome();
    log["ip"] = row.getValueOfIp();
    return log;
}
}  // namespace

void AuditService::listLogs(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    int page = 1;
    int perPage = 50;
    std::string action = req->getParameter("action");
    std::string outcome = req->getParameter("outcome");
    std::string actorId = req->getParameter("actor_id");

    try
    {
        page = std::stoi(req->getParameter("page"));
    }
    catch (...)
    {
    }
    try
    {
        perPage = std::stoi(req->getParameter("per_page"));
    }
    catch (...)
    {
    }
    if (perPage > 100)
        perPage = 100;
    if (perPage < 1)
        perPage = 50;
    if (page < 1)
        page = 1;

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    // Build the optional filters as a compound Criteria (the previous
    // implementation built the clause then ran an unfiltered query -- A-LOG-004;
    // this version actually applies the filters).
    Criteria filter;
    if (!action.empty())
    {
        filter = filter && Criteria(AuditLogs::Cols::_action, CompareOperator::EQ, action);
    }
    if (!outcome.empty())
    {
        filter = filter && Criteria(AuditLogs::Cols::_outcome, CompareOperator::EQ, outcome);
    }
    if (!actorId.empty())
    {
        filter = filter && Criteria(AuditLogs::Cols::_actor_id, CompareOperator::EQ, actorId);
    }

    // #146: count-then-page two-step (same shape as
    // TokenManagementService::listTokens). `total` must be the number of rows
    // matching the filter set -- the previous implementation echoed back the
    // current page's row count, so any client paging via `total` mis-computed.
    // Each Mapper construction gets its own try/catch (db-operations rule:
    // an outer guard cannot protect constructions inside async callbacks).
    try
    {
        Mapper<AuditLogs> countMapper(db);
        countMapper.count(
          filter,
          [cb, req, page, perPage, filter, db](const size_t total) {
              try
              {
                  Mapper<AuditLogs> dataMapper(db);
                  dataMapper.paginate(page, perPage)
                    .orderBy(AuditLogs::Cols::_timestamp, SortOrder::DESC)
                    .findBy(
                      filter,
                      [cb, page, perPage, total](const std::vector<AuditLogs> &rows) {
                          Json::Value json;
                          json["status"] = "success";
                          json["page"] = page;
                          json["per_page"] = perPage;
                          json["total"] = static_cast<Json::UInt64>(total);
                          Json::Value logs(Json::arrayValue);
                          for (const auto &row : rows)
                          {
                              logs.append(logRowToJson(row));
                          }
                          json["logs"] = logs;
                          (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                      },
                      [req, cb](const ::drogon::orm::DrogonDbException &e) {
                          respondError(
                            req,
                            cb,
                            "DB_QUERY_ERROR",
                            std::string("Failed to fetch audit logs: ") + e.base().what()
                          );
                      }
                    );
              }
              catch (const std::exception &e)
              {
                  respondError(
                    req,
                    cb,
                    "DB_QUERY_ERROR",
                    std::string("Failed to fetch audit logs: ") + e.what()
                  );
              }
          },
          [req, cb](const ::drogon::orm::DrogonDbException &e) {
              respondError(
                req,
                cb,
                "DB_QUERY_ERROR",
                std::string("Failed to count audit logs: ") + e.base().what()
              );
          }
        );
    }
    catch (const std::exception &e)
    {
        respondError(
          req,
          cb,
          "DB_QUERY_ERROR",
          std::string("Failed to count audit logs: ") + e.what()
        );
    }
}

void AuditService::getDashboardStats(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    // Original: a single compound query with 5 (SELECT COUNT(*)) subqueries.
    // Split into 5 sequential Mapper::count() calls (one per stat). The
    // active-token and logs/failures "today" filters use the same conditions
    // (now/dayAgo evaluated in C++ for the epoch comparisons, matching the
    // other admin services' approach).
    int64_t now = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::system_clock::now().time_since_epoch()
    )
                                         .count());
    int64_t dayAgo = now - 86400;

    // Stats accumulator shared across the count chain.
    struct Stats
    {
        size_t totalUsers = 0;
        size_t totalClients = 0;
        size_t activeTokens = 0;
        size_t logsToday = 0;
        size_t failuresToday = 0;
    };

    auto stats = std::make_shared<Stats>();

    // Chain: totalUsers -> totalClients -> activeTokens -> logsToday ->
    // failuresToday -> respond.
    Mapper<Users>(db).count(
      Criteria(),
      [cb, req, db, stats, dayAgo, now](const size_t n) {
          stats->totalUsers = n;
          Mapper<Oauth2Clients>(db).count(
            Criteria(),
            [cb, req, db, stats, dayAgo, now](const size_t n2) {
                stats->totalClients = n2;
                Criteria activeTok =
                  Criteria(Oauth2AccessTokens::Cols::_expires_at, CompareOperator::GT, now) &&
                  (Criteria(Oauth2AccessTokens::Cols::_revoked, CompareOperator::EQ, false) ||
                   Criteria(Oauth2AccessTokens::Cols::_revoked, CompareOperator::IsNull));
                Mapper<Oauth2AccessTokens>(db).count(
                  activeTok,
                  [cb, req, db, stats, dayAgo](const size_t n3) {
                      stats->activeTokens = n3;
                      // AuditLogs.timestamp is a trantor::Date; the original
                      // used to_timestamp($epoch) on the DB side. Compare
                      // against a Date built from the same epoch (microseconds).
                      Criteria logsToday = Criteria(
                        AuditLogs::Cols::_timestamp,
                        CompareOperator::GT,
                        ::trantor::Date(dayAgo * 1000000)
                      );
                      Mapper<AuditLogs>(db).count(
                        logsToday,
                        [cb, req, db, stats](const size_t n4) {
                            stats->logsToday = n4;
                            Criteria failuresToday =
                              Criteria(AuditLogs::Cols::_outcome, CompareOperator::EQ, "failure");
                            Mapper<AuditLogs>(db).count(
                              failuresToday,
                              [cb, stats](const size_t n5) {
                                  stats->failuresToday = n5;
                                  Json::Value json;
                                  json["status"] = "success";
                                  json["total_users"] =
                                    static_cast<Json::UInt64>(stats->totalUsers);
                                  json["total_clients"] =
                                    static_cast<Json::UInt64>(stats->totalClients);
                                  json["active_tokens"] =
                                    static_cast<Json::UInt64>(stats->activeTokens);
                                  json["logs_today"] = static_cast<Json::UInt64>(stats->logsToday);
                                  json["failures_today"] =
                                    static_cast<Json::UInt64>(stats->failuresToday);
                                  (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                              },
                              [req, cb](const ::drogon::orm::DrogonDbException &e) {
                                  respondError(
                                    req,
                                    cb,
                                    "DB_QUERY_ERROR",
                                    std::string("Failed to fetch dashboard stats: ") +
                                      e.base().what()
                                  );
                              }
                            );
                        },
                        [req, cb](const ::drogon::orm::DrogonDbException &e) {
                            respondError(
                              req,
                              cb,
                              "DB_QUERY_ERROR",
                              std::string("Failed to fetch dashboard stats: ") + e.base().what()
                            );
                        }
                      );
                  },
                  [req, cb](const ::drogon::orm::DrogonDbException &e) {
                      respondError(
                        req,
                        cb,
                        "DB_QUERY_ERROR",
                        std::string("Failed to fetch dashboard stats: ") + e.base().what()
                      );
                  }
                );
            },
            [req, cb](const ::drogon::orm::DrogonDbException &e) {
                respondError(
                  req,
                  cb,
                  "DB_QUERY_ERROR",
                  std::string("Failed to fetch dashboard stats: ") + e.base().what()
                );
            }
          );
      },
      [req, cb](const ::drogon::orm::DrogonDbException &e) {
          respondError(
            req,
            cb,
            "DB_QUERY_ERROR",
            std::string("Failed to fetch dashboard stats: ") + e.base().what()
          );
      }
    );
}

void AuditService::dashboard(ResponseCallback cb)
{
    Json::Value json;
    json["message"] = "Welcome to Admin Dashboard";
    json["status"] = "success";
    (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
}

// ---------------------------------------------------------------------------
// P1-6 (#283): CSV export + formula-injection hardening
// ---------------------------------------------------------------------------

std::string AuditService::csvCell(const std::string &value)
{
    // OWASP CSV injection: spreadsheet editors evaluate cells that START with
    // = + - @ (formulas) or a tab/CR (DDE-class payloads) — the audit trail
    // carries fully user-controlled user_agent/ip/details, so every cell is
    // prefixed before quoting. The apostrophe renders literally in editors
    // and neutralizes the formula.
    std::string cell = value;
    if (!cell.empty())
    {
        const char c = cell[0];
        if (c == '=' || c == '+' || c == '-' || c == '@' || c == '\t' || c == '\r')
            cell.insert(cell.begin(), '\'');
    }
    // RFC 4180 quoting only when needed (keeps the common row compact).
    bool needsQuotes = false;
    for (char c : cell)
    {
        if (c == ',' || c == '"' || c == '\n' || c == '\r')
        {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes)
        return cell;
    std::string out;
    out.reserve(cell.size() + 2);
    out.push_back('"');
    for (char c : cell)
    {
        if (c == '"')
            out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

namespace
{
std::string auditRowToCsvLine(const AuditLogs &row)
{
    // Column order matches the export header (the full audit row surface).
    std::ostringstream line;
    line << row.getValueOfId() << ','
         << AuditService::csvCell(row.getValueOfTimestamp().toDbString()) << ','
         << AuditService::csvCell(row.getValueOfActorType()) << ','
         << AuditService::csvCell(row.getValueOfActorId()) << ','
         << AuditService::csvCell(row.getValueOfAction()) << ','
         << AuditService::csvCell(row.getValueOfTargetType()) << ','
         << AuditService::csvCell(row.getValueOfTargetId()) << ','
         << AuditService::csvCell(row.getValueOfOutcome()) << ','
         << AuditService::csvCell(row.getValueOfIp()) << ','
         << AuditService::csvCell(row.getValueOfUserAgent()) << ','
         << AuditService::csvCell(row.getValueOfRequestId()) << ','
         << (row.getOrgId() == nullptr ? ""
                                       : std::to_string(row.getValueOfOrgId()))
         << ',' << AuditService::csvCell(row.getValueOfDetails()) << "\r\n";
    return line.str();
}
}  // namespace

void AuditService::exportLogs(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    // Memory-mode guard: no audit store, no export (getDbClient() would
    // ABORT in Debug builds — the M1 pitfall).
    if (auto *p = ::drogon::app().getPlugin<::OAuth2Plugin>();
        p != nullptr && p->getStorageType() == "memory")
    {
        respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "audit export requires a database");
        return;
    }

    // Filters mirror listLogs plus the export-specific controls.
    std::string action = req->getParameter("action");
    std::string outcome = req->getParameter("outcome");
    std::string actorId = req->getParameter("actor_id");
    std::string fromStr = req->getParameter("from");
    std::string toStr = req->getParameter("to");
    std::string allStr = req->getParameter("all");
    std::string limitStr = req->getParameter("limit");

    int64_t from = 0;
    int64_t to = 0;
    auto parseInt64 = [](const std::string &s, int64_t &out) {
        try
        {
            std::size_t pos = 0;
            long long v = std::stoll(s, &pos);
            if (pos != s.size())
                return false;
            out = v;
            return true;
        }
        catch (...)
        {
            return false;
        }
    };

    const bool hasFrom = !fromStr.empty() && parseInt64(fromStr, from);
    const bool hasTo = !toStr.empty() && parseInt64(toStr, to);
    const bool all = (allStr == "true" || allStr == "1");

    // Unbounded-export DoS guard: a window is mandatory unless the caller
    // explicitly opts into the full table with all=true.
    if (!all && !(hasFrom && hasTo))
    {
        respondError(
          req,
          cb,
          "AUDIT_EXPORT_WINDOW_REQUIRED",
          "export requires from/to (epoch seconds) or an explicit all=true");
        return;
    }
    if (hasFrom && hasTo && from >= to)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "from must be strictly before to");
        return;
    }

    // Row cap: config default, per-request limit may only lower it.
    const int64_t configCap = [] {
        auto config = ::drogon::app().getCustomConfig();
        if (config.isMember("audit") && config["audit"].isMember("export_max_rows") &&
            config["audit"]["export_max_rows"].isInt64())
            return config["audit"]["export_max_rows"].asInt64();
        return static_cast<int64_t>(500000);
    }();
    int64_t cap = configCap;
    if (!limitStr.empty())
    {
        int64_t limit = 0;
        if (!parseInt64(limitStr, limit) || limit < 1)
        {
            respondError(req, cb, "VALIDATION_INVALID_INPUT", "limit must be a positive integer");
            return;
        }
        cap = (std::min)(limit, configCap);
    }

    Criteria filter;
    if (!action.empty())
        filter = filter && Criteria(AuditLogs::Cols::_action, CompareOperator::EQ, action);
    if (!outcome.empty())
        filter = filter && Criteria(AuditLogs::Cols::_outcome, CompareOperator::EQ, outcome);
    if (!actorId.empty())
        filter = filter && Criteria(AuditLogs::Cols::_actor_id, CompareOperator::EQ, actorId);
    if (hasFrom)
        filter = filter && Criteria(AuditLogs::Cols::_timestamp, CompareOperator::GE,
                                    ::trantor::Date(from * 1000000));
    if (hasTo)
        filter = filter && Criteria(AuditLogs::Cols::_timestamp, CompareOperator::LE,
                                    ::trantor::Date(to * 1000000));

    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    // Count first: over-cap exports are rejected with a narrowing hint
    // instead of streaming an unbounded payload.
    try
    {
        Mapper<AuditLogs> countMapper(db);
        countMapper.count(
          filter,
          [cb, req, filter, db, cap, from, to, all](const size_t total) {
              if (static_cast<int64_t>(total) > cap)
              {
                  respondError(
                    req,
                    cb,
                    "AUDIT_EXPORT_TOO_LARGE",
                    "export of " + std::to_string(total) +
                      " rows exceeds the cap of " + std::to_string(cap) +
                      "; narrow the time window or lower `limit`");
                  return;
              }
              try
              {
                  Mapper<AuditLogs> dataMapper(db);
                  dataMapper.orderBy(AuditLogs::Cols::_timestamp, SortOrder::ASC)
                    .findBy(
                      filter,
                      [cb, req, total, from, to, all](const std::vector<AuditLogs> &rows) {
                          // Self-audit BEFORE the (potentially large) body is
                          // streamed back.
                          if (auto *plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
                              plugin != nullptr)
                          {
                              Json::Value details;
                              details["rows"] = static_cast<Json::UInt64>(total);
                              details["windowed"] = !all;
                              if (!all)
                              {
                                  details["from"] = static_cast<Json::Int64>(from);
                                  details["to"] = static_cast<Json::Int64>(to);
                              }
                              ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
                                plugin->getAuditSink(),
                                "audit_exported",
                                "success",
                                req,
                                "",
                                "audit_log",
                                "export",
                                details);
                          }

                          std::ostringstream csv;
                          csv << "id,timestamp,actor_type,actor_id,action,target_type,"
                                 "target_id,outcome,ip,user_agent,request_id,org_id,details\r\n";
                          for (const auto &row : rows)
                              csv << auditRowToCsvLine(row);

                          auto resp = ::drogon::HttpResponse::newHttpResponse();
                          resp->setContentTypeString("text/csv; charset=utf-8");
                          resp->addHeader(
                            "Content-Disposition",
                            all ? "attachment; filename=\"audit_logs_all.csv\""
                                : "attachment; filename=\"audit_logs_" + std::to_string(from) +
                                    "_" + std::to_string(to) + ".csv\"");
                          resp->setBody(csv.str());
                          (*cb)(resp);
                      },
                      [req, cb](const ::drogon::orm::DrogonDbException &e) {
                          respondError(
                            req,
                            cb,
                            "DB_QUERY_ERROR",
                            std::string("export fetch failed: ") + e.base().what());
                      });
              }
              catch (const std::exception &e)
              {
                  respondError(
                    req, cb, "DB_QUERY_ERROR", std::string("export fetch failed: ") + e.what());
              }
          },
          [req, cb](const ::drogon::orm::DrogonDbException &e) {
              respondError(
                req, cb, "DB_QUERY_ERROR", std::string("export count failed: ") + e.base().what());
          });
    }
    catch (const std::exception &e)
    {
        respondError(req, cb, "DB_QUERY_ERROR", std::string("export count failed: ") + e.what());
    }
}

}  // namespace fulla::drogon::admin
