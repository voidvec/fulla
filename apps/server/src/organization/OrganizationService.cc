#include "OrganizationService.h"
#include <fulla/drogon/adapters/DrogonAuditSink.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>

#include <fulla/storage/postgres/models/OrganizationMembers.h>
#include <fulla/storage/postgres/models/Organizations.h>
#include <fulla/storage/postgres/models/Users.h>
#include <fulla/drogon/adapters/DrogonAuditSink.h>
#include <fulla/drogon/error/ErrorResponder.h>

#include <drogon/drogon.h>

#include <regex>

namespace organization
{

namespace
{
void respondError(
  const ::drogon::HttpRequestPtr &req,
  const OrganizationService::ResponseCallback &cb,
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

::drogon::orm::DbClientPtr getDbOrRespond(
  const ::drogon::HttpRequestPtr &req,
  const OrganizationService::ResponseCallback &cb
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

void audit(
  const ::drogon::HttpRequestPtr &req,
  const char *action,
  const std::string &targetId
)
{
    // Same pattern as OrgMemberService::audit (organization targetType).
    auto *plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
    if (plugin)
    {
        ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
          plugin->getAuditSink(), action, "success", req, "", "organization", targetId
        );
    }
}

Json::Value orgRowToJson(const ::drogon_model::fulla_db::Organizations &row)
{
    Json::Value org;
    org["id"] = row.getValueOfId();
    org["slug"] = row.getValueOfSlug();
    org["name"] = row.getValueOfName();
    org["logo_uri"] = row.getValueOfLogoUri();
    org["primary_color"] = row.getValueOfPrimaryColor();
    org["issuer_override"] = row.getValueOfIssuerOverride();
    return org;
}
}  // namespace

using namespace ::drogon::orm;
using namespace ::drogon_model::fulla_db;
using MemberModel = ::drogon_model::fulla_db::OrganizationMembers;

void OrganizationService::list(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }
    // No `created_at` is emitted in the original response (it selected the
    // column but never serialized it); orgRowToJson preserves that exactly.
    Mapper<Organizations> mapper(db);
    mapper.findBy(
      Criteria(),
      [cb](const std::vector<Organizations> &rows) {
          Json::Value json;
          Json::Value orgs(Json::arrayValue);
          for (const auto &row : rows)
          {
              orgs.append(orgRowToJson(row));
          }
          json["organizations"] = orgs;
          json["total"] = static_cast<int>(rows.size());
          (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
      },
      [req, cb](const ::drogon::orm::DrogonDbException &e) {
          respondError(
            req, cb, "DB_QUERY_ERROR", std::string("list organizations failed: ") + e.base().what()
          );
      }
    );
}

void OrganizationService::create(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    auto jsonBody = req->getJsonObject();
    if (!jsonBody)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "create org: JSON body required");
        return;
    }

    std::string slug = (*jsonBody).get("slug", "").asString();
    std::string name = (*jsonBody).get("name", "").asString();
    std::string logoUri = (*jsonBody).get("logo_uri", "").asString();
    std::string primaryColor = (*jsonBody).get("primary_color", "").asString();
    std::string issuerOverride = (*jsonBody).get("issuer_override", "").asString();

    std::regex slugPattern("^[a-z0-9][a-z0-9-]{1,48}[a-z0-9]$");
    if (!std::regex_match(slug, slugPattern))
    {
        respondError(
          req,
          cb,
          "VALIDATION_FORMAT_ERROR",
          "create org: slug must be 3-50 chars, lowercase alphanumeric + hyphens"
        );
        return;
    }

    if (name.empty())
    {
        respondError(req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "create org: name is required");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    Organizations row;
    row.setSlug(slug);
    row.setName(name);
    row.setLogoUri(logoUri);
    row.setPrimaryColor(primaryColor);
    row.setIssuerOverride(issuerOverride);

    Mapper<Organizations> mapper(db);
    mapper.insert(
      row,
      [cb, slug, name, req](const Organizations &inserted) {
          ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
            ::drogon::app().getPlugin<::OAuth2Plugin>()->getAuditSink(),
            "organization_created",
            "success",
            req,
            "",
            "organization",
            slug
          );
          Json::Value json;
          json["id"] = inserted.getValueOfId();
          json["slug"] = slug;
          json["name"] = name;
          json["message"] = "Organization created";
          auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
          resp->setStatusCode(::drogon::k201Created);
          (*cb)(resp);
      },
      [req, cb](const ::drogon::orm::DrogonDbException &e) {
          // Original mapped any DB error (incl. unique-violation on slug) to
          // RESOURCE_CONFLICT; preserved.
          respondError(
            req,
            cb,
            "VALIDATION_RESOURCE_CONFLICT",
            std::string("create org: slug already exists or DB error: ") + e.base().what()
          );
      }
    );
}

// ---------------------------------------------------------------------------
// POST /api/admin/organizations/{slug}/transfer-ownership  (#221 admin override)
// ---------------------------------------------------------------------------
void OrganizationService::transferOwnership(
  const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &slug
)
{
    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    auto jsonBody = req->getJsonObject();
    if (!jsonBody || !(*jsonBody).isMember("user_id") || !(*jsonBody)["user_id"].isIntegral())
    {
        respondError(
          req, cb, "VALIDATION_INVALID_INPUT",
          "transfer ownership: JSON body with integer user_id required"
        );
        return;
    }
    const int32_t targetUserId = static_cast<int32_t>((*jsonBody)["user_id"].asInt64());
    if (targetUserId <= 0)
    {
        respondError(
          req, cb, "VALIDATION_INVALID_INPUT", "transfer ownership: user_id must be positive"
        );
        return;
    }

    // UnexpectedRows (no such org/user) -> uniform 404; real DB failures
    // surface as DB_QUERY_ERROR (fail visibly — same split as the
    // PostgresClientRepository owners-lookup C3 pattern).
    auto orgNotFound = [req, cb](const DrogonDbException &e) {
        if (dynamic_cast<const UnexpectedRows *>(&e) != nullptr)
            respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "organization not found");
        else
            respondError(
              req, cb, "DB_QUERY_ERROR", std::string("org lookup failed: ") + e.base().what());
    };
    auto userNotFound = [req, cb](const DrogonDbException &e) {
        if (dynamic_cast<const UnexpectedRows *>(&e) != nullptr)
            respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "user not found");
        else
            respondError(
              req, cb, "DB_QUERY_ERROR", std::string("user lookup failed: ") + e.base().what());
    };

    try
    {
        Mapper<Organizations> orgMapper(db);
        orgMapper.findOne(
          Criteria(Organizations::Cols::_slug, CompareOperator::EQ, slug),
          [req, cb, db, slug, targetUserId, userNotFound](const Organizations &org) {
              const int32_t orgId = org.getValueOfId();
              // #221: the target must be a LIVE user — transferring the seat
              // back to a soft-deleted user would recreate the very deadlock
              // this endpoint resolves.
              try
              {
                  Mapper<Users> userMapper(db);
                  userMapper.findOne(
                    Criteria(Users::Cols::_id, CompareOperator::EQ, targetUserId) &&
                      Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull),
                    [req, cb, db, org, orgId, slug, targetUserId, userNotFound](const Users &) {
                        // Step 2 (runs after the upsert below): demote any
                        // OTHER owner rows to 'admin' and respond. Ordered
                        // after the promote/insert so the fail-open direction
                        // is "two owners temporarily" (retryable), never
                        // "zero owners".
                        auto demoteOthersAndRespond = [req, cb, db, org, orgId, slug, targetUserId]() {
                            // Documented batch-UPDATE exemption (same pattern
                            // as the open-platform suspend revocation): the
                            // owner set is service-managed and tiny; the
                            // WHERE clause is fully scoped by org + role.
                            db->execSqlAsync(
                              "UPDATE organization_members SET role = 'admin' "
                              "WHERE organization_id = $1 AND role = 'owner' AND user_id <> $2",
                              [req, cb, org, slug, targetUserId](const Result &) {
                                  audit(req, "org_ownership_transferred", slug);
                                  Json::Value json;
                                  json["slug"] = slug;
                                  json["organization_id"] = org.getValueOfId();
                                  json["owner_user_id"] = targetUserId;
                                  (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                              },
                              [req, cb, slug](const DrogonDbException &e) {
                                  // The target IS already owner at this point;
                                  // failing loudly lets the admin retry
                                  // idempotently instead of silently leaving
                                  // two owners.
                                  respondError(
                                    req, cb, "DB_QUERY_ERROR",
                                    std::string("demote previous owners failed for ") + slug +
                                      " (target is already owner; retry is idempotent): " +
                                      e.base().what());
                              },
                              orgId, targetUserId);
                        };
                        // Step 1: upsert the target's membership as owner
                        // (promote an existing member, or create the seat —
                        // the admin override must also resolve orgs where
                        // nobody is left to invite).
                        try
                        {
                            Mapper<MemberModel> memberMapper(db);
                            memberMapper.findOne(
                              Criteria(MemberModel::Cols::_organization_id, CompareOperator::EQ,
                                       orgId) &&
                                Criteria(MemberModel::Cols::_user_id, CompareOperator::EQ,
                                         targetUserId),
                              [req, cb, db, orgId, targetUserId, demoteOthersAndRespond](
                                const MemberModel &row) {
                                  MemberModel updated = row;
                                  updated.setRole("owner");
                                  try
                                  {
                                      Mapper<MemberModel>(db).update(
                                        updated,
                                        [demoteOthersAndRespond](const std::size_t) {
                                            demoteOthersAndRespond();
                                        },
                                        [req, cb](const DrogonDbException &e) {
                                            respondError(
                                              req, cb, "DB_QUERY_ERROR",
                                              std::string("promote membership failed: ") +
                                                e.base().what());
                                        });
                                  }
                                  catch (...)
                                  {
                                      respondError(
                                        req, cb, "DB_QUERY_ERROR",
                                        "membership promote: Mapper construction failed");
                                  }
                              },
                              [req, cb, db, orgId, targetUserId, demoteOthersAndRespond,
                               userNotFound](const DrogonDbException &e) {
                                  if (dynamic_cast<const UnexpectedRows *>(&e) == nullptr)
                                  {
                                      userNotFound(e);  // real DB error (same split)
                                      return;
                                  }
                                  MemberModel m;
                                  m.setOrganizationId(orgId);
                                  m.setUserId(targetUserId);
                                  m.setRole("owner");
                                  try
                                  {
                                      Mapper<MemberModel>(db).insert(
                                        m,
                                        [demoteOthersAndRespond](const MemberModel &) {
                                            demoteOthersAndRespond();
                                        },
                                        [req, cb](const DrogonDbException &e) {
                                            respondError(
                                              req, cb, "DB_QUERY_ERROR",
                                              std::string("create owner membership failed: ") +
                                                e.base().what());
                                        });
                                  }
                                  catch (...)
                                  {
                                      respondError(
                                        req, cb, "DB_QUERY_ERROR",
                                        "membership insert: Mapper construction failed");
                                  }
                              });
                        }
                        catch (...)
                        {
                            respondError(
                              req, cb, "DB_QUERY_ERROR",
                              "membership lookup: Mapper construction failed");
                        }
                    },
                    userNotFound);
              }
              catch (...)
              {
                  respondError(
                    req, cb, "DB_QUERY_ERROR", "user lookup: Mapper construction failed");
              }
          },
          orgNotFound);
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "org lookup: Mapper construction failed");
    }
}

void OrganizationService::getBySlug(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &slug
)
{
    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    Mapper<Organizations> mapper(db);
    mapper.findOne(
      Criteria(Organizations::Cols::_slug, CompareOperator::EQ, slug),
      [cb](const Organizations &row) {
          Json::Value json = orgRowToJson(row);
          (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
      },
      [req, cb](const ::drogon::orm::DrogonDbException &e) {
          // NoRowsException -> not found (original empty-result branch).
          respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "get org: organization not found");
          (void)e;
      }
    );
}

}  // namespace organization
