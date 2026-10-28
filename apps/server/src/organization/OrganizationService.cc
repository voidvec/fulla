#include "OrganizationService.h"
#include <fulla/drogon/adapters/DrogonAuditSink.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/utils/BrandValidator.h>

#include <fulla/storage/postgres/AdvisoryLock.h>
#include <fulla/storage/postgres/models/OrganizationMembers.h>
#include <fulla/storage/postgres/models/Organizations.h>
#include <fulla/storage/postgres/models/Users.h>
#include <fulla/drogon/error/ErrorResponder.h>

#include <drogon/drogon.h>

#include <limits>
#include <map>
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
  const std::string &targetId,
  const std::string &orgId = ""
)
{
    // Same pattern as OrgMemberService::audit (organization targetType):
    // empty actorId falls back to the filter-set userId attribute inside
    // logFromRequest; orgId feeds audit_logs.org_id (V036).
    auto *plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
    if (plugin)
    {
        ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
          plugin->getAuditSink(), action, "success", req, "", "organization", targetId,
          Json::Value(), orgId
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
      // P1-4 (#281): the admin list shows only live orgs (soft-deleted
      // rows carry the '#del-<id>' placeholder slug).
      Criteria(Organizations::Cols::_deleted_at, CompareOperator::IsNull),
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

// P1-4 create tail (tombstone pre-check passes the row here; defined in
// the anonymous namespace below — same unnamed namespace, internal
// linkage).
namespace
{
void insertAdminOrgRow(
  const ::drogon::HttpRequestPtr &req,
  const organization::OrganizationService::ResponseCallback &cb,
  const ::drogon::orm::DbClientPtr &db,
  ::drogon_model::fulla_db::Organizations row
);
}  // namespace

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

    // P1-1 (#279): brand fields are optional but, when present, validated —
    // the color regex is the tightened ^#[0-9a-fA-F]{3,6}$ (the {3,8} draft
    // form contradicted the VARCHAR(7) column); the logo is the brand-image
    // rule (https + loopback exemption), NOT the redirect rule.
    const std::string colorErr = ::fulla::drogon::utils::validatePrimaryColor(primaryColor);
    if (!colorErr.empty())
    {
        respondError(req, cb, "VALIDATION_FORMAT_ERROR", "create org: " + colorErr);
        return;
    }
    const std::string logoErr = ::fulla::drogon::utils::validateBrandUri(logoUri);
    if (!logoErr.empty())
    {
        respondError(req, cb, "VALIDATION_FORMAT_ERROR", "create org: " + logoErr);
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

    // P1-4 (#281): tombstone pre-check — a soft-deleted org holds the slug
    // as 'slug#del-<id>', so the unique constraint no longer fires for the
    // original spelling. Same-name impersonation guard (audit rows still
    // reference the old id).
    try
    {
        Mapper<Organizations> tombstoneMapper(db);
        tombstoneMapper.findBy(
          Criteria(Organizations::Cols::_slug, CompareOperator::Like, slug + "#del-%"),
          [req, cb, row, db](const std::vector<Organizations> &tombs) mutable {
              if (!tombs.empty())
              {
                  respondError(
                    req, cb, "VALIDATION_RESOURCE_CONFLICT",
                    "create org: this slug was used by a deleted organization and cannot "
                    "be re-registered");
                  return;
              }
              insertAdminOrgRow(req, cb, db, row);
          },
          [req, cb](const DrogonDbException &e) {
              respondError(
                req, cb, "DB_QUERY_ERROR",
                std::string("create org: tombstone check failed: ") + e.base().what());
          });
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "create org: Mapper construction failed");
    }
}

// Admin-create tail (after the P1-4 tombstone pre-check).
namespace
{
void insertAdminOrgRow(
  const ::drogon::HttpRequestPtr &req,
  const organization::OrganizationService::ResponseCallback &cb,
  const ::drogon::orm::DbClientPtr &db,
  ::drogon_model::fulla_db::Organizations row
)
{
    const std::string slug = row.getValueOfSlug();
    try
    {
        Mapper<Organizations> mapper(db);
        mapper.insert(
      row,
      [cb, slug, req](const Organizations &inserted) {
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
          json["name"] = inserted.getValueOfName();
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
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "create org: Mapper construction failed");
    }
}
}  // namespace

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
    const int64_t requestedUserId = (*jsonBody)["user_id"].asInt64();
    // Validate in the int64 domain BEFORE narrowing: a plain static_cast
    // would wrap out-of-range values (e.g. 2^32 + 1 -> 1) onto a different
    // real user (review D1).
    if (requestedUserId <= 0 || requestedUserId > (std::numeric_limits<int32_t>::max)())
    {
        respondError(
          req, cb, "VALIDATION_INVALID_INPUT",
          "transfer ownership: user_id is out of range"
        );
        return;
    }
    const int32_t targetUserId = static_cast<int32_t>(requestedUserId);

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
          Criteria(Organizations::Cols::_slug, CompareOperator::EQ, slug) &&
            Criteria(Organizations::Cols::_deleted_at, CompareOperator::IsNull),
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
                        // after the promote/insert so a mid-flight failure
                        // biases toward "two owners temporarily" (retryable).
                        // Not transactional (V034's comment envisioned a
                        // single-transaction transfer for the full v1.4.0
                        // nominate-and-accept workflow); the promote's
                        // count==0 abort below closes the "target left
                        // mid-flight -> zero owners" window, and the
                        // remaining two-concurrent-transfers race is a
                        // system-admin-only surface documented in the PR.
                        auto demoteOthersAndRespond = [req, cb, db, org, orgId, slug, targetUserId]() {
                            // Documented batch-UPDATE exemption (same pattern
                            // as the open-platform suspend revocation): the
                            // owner set is service-managed and tiny; the
                            // WHERE clause is fully scoped by org + role.
                            db->execSqlAsync(
                              "UPDATE organization_members SET role = 'admin' "
                              "WHERE organization_id = $1 AND role = 'owner' AND user_id <> $2",
                              [req, cb, org, slug, targetUserId](const Result &) {
                                  audit(req, "org_ownership_transferred", slug, std::to_string(org.getValueOfId()));
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
                                        [req, cb, demoteOthersAndRespond](const std::size_t count) {
                                            if (count == 0)
                                            {
                                                // The target left the org
                                                // between findOne and update.
                                                // Demoting now would leave the
                                                // org with zero owners — abort
                                                // instead (review D2).
                                                respondError(
                                                  req, cb, "VALIDATION_RESOURCE_CONFLICT",
                                                  "target left the organization during the "
                                                  "transfer; retry");
                                                return;
                                            }
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
                              [req, cb, db, orgId, targetUserId, demoteOthersAndRespond](
                                const DrogonDbException &e) {
                                  if (dynamic_cast<const UnexpectedRows *>(&e) == nullptr)
                                  {
                                      respondError(
                                        req, cb, "DB_QUERY_ERROR",
                                        std::string("membership lookup failed: ") +
                                          e.base().what());
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
      Criteria(Organizations::Cols::_slug, CompareOperator::EQ, slug) &&
        Criteria(Organizations::Cols::_deleted_at, CompareOperator::IsNull),
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

// ---------------------------------------------------------------------------
// P1-4 (#281): PATCH / DELETE / member management.
// ---------------------------------------------------------------------------

namespace
{
void auditOrg(
  const ::drogon::HttpRequestPtr &req,
  const char *action,
  const std::string &slug
)
{
    auto *plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
    if (!plugin)
        return;
    ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
      plugin->getAuditSink(), action, "success", req, "", "organization", slug);
}

// Shared tail of removeMember: delete the membership row + audit + respond.
void deleteMembership(
  const ::drogon::HttpRequestPtr &req,
  const organization::OrganizationService::ResponseCallback &cb,
  const ::drogon::orm::DbClientPtr &db,
  const std::string &slug,
  int32_t orgId,
  int32_t targetUserId
)
{
    using MemberModel = ::drogon_model::fulla_db::OrganizationMembers;
    try
    {
        Mapper<MemberModel> mapper(db);
        mapper.deleteBy(
          Criteria(MemberModel::Cols::_organization_id, CompareOperator::EQ, orgId) &&
            Criteria(MemberModel::Cols::_user_id, CompareOperator::EQ, targetUserId),
          [req, cb, slug](const std::size_t removed) {
              if (removed == 0)
              {
                  respondError(
                    req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "membership not found");
                  return;
              }
              ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
                ::drogon::app().getPlugin<::OAuth2Plugin>()->getAuditSink(),
                "org_member_removed",
                "success",
                req,
                "",
                "organization",
                slug,
                Json::Value(Json::objectValue));
              Json::Value json;
              json["status"] = "success";
              json["message"] = "Member removed";
              (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
          },
          [req, cb](const DrogonDbException &e) {
              respondError(
                req, cb, "DB_QUERY_ERROR",
                std::string("remove member failed: ") + e.base().what());
          });
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "remove member: Mapper construction failed");
    }
}
}  // namespace

void OrganizationService::updateOrg(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &slug
)
{
    auto jsonBody = req->getJsonObject();
    if (!jsonBody)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "update org: JSON body required");
        return;
    }
    const bool hasName = jsonBody->isMember("name");
    const bool hasLogo = jsonBody->isMember("logo_uri");
    const bool hasColor = jsonBody->isMember("primary_color");
    if (!hasName && !hasLogo && !hasColor)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "update org: no editable field present (name / logo_uri / primary_color)");
        return;
    }
    if (jsonBody->isMember("slug"))
    {
        respondError(
          req, cb, "VALIDATION_INVALID_INPUT",
          "update org: slug is immutable (external reference anchor)");
        return;
    }

    std::string name;
    if (hasName)
    {
        name = (*jsonBody)["name"].asString();
        if (name.empty() || name.size() > 100)
        {
            respondError(req, cb, "VALIDATION_INVALID_INPUT", "update org: name must be 1-100 chars");
            return;
        }
    }
    // P1-1 brand validators (same rules as create).
    if (hasColor)
    {
        const std::string err =
          ::fulla::drogon::utils::validatePrimaryColor((*jsonBody)["primary_color"].asString());
        if (!err.empty())
        {
            respondError(req, cb, "VALIDATION_FORMAT_ERROR", "update org: " + err);
            return;
        }
    }
    if (hasLogo)
    {
        const std::string err =
          ::fulla::drogon::utils::validateBrandUri((*jsonBody)["logo_uri"].asString());
        if (!err.empty())
        {
            respondError(req, cb, "VALIDATION_FORMAT_ERROR", "update org: " + err);
            return;
        }
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    try
    {
        using OrgModel = ::drogon_model::fulla_db::Organizations;
        Mapper<OrgModel> mapper(db);
        mapper.findOne(
          Criteria(OrgModel::Cols::_slug, CompareOperator::EQ, slug) &&
            Criteria(OrgModel::Cols::_deleted_at, CompareOperator::IsNull),
          [req, cb, db, slug, jsonBody, hasName, hasLogo, hasColor](OrgModel row) {
              if (hasName)
                  row.setName((*jsonBody)["name"].asString());
              if (hasLogo)
              {
                  const std::string logo = (*jsonBody)["logo_uri"].asString();
                  if (logo.empty())
                      row.setLogoUriToNull();
                  else
                      row.setLogoUri(logo);
              }
              if (hasColor)
              {
                  const std::string color = (*jsonBody)["primary_color"].asString();
                  if (color.empty())
                      row.setPrimaryColorToNull();
                  else
                      row.setPrimaryColor(color);
              }
              row.setUpdatedAt(::trantor::Date::now());
              try
              {
                  Mapper<OrgModel> updateMapper(db);
                  updateMapper.update(
                    row,
                    [req, cb, slug](const std::size_t) {
                        auditOrg(req, "org_updated", slug);
                        Json::Value json;
                        json["status"] = "success";
                        json["message"] = "Organization updated";
                        (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                    },
                    [req, cb](const DrogonDbException &e) {
                        respondError(
                          req, cb, "DB_QUERY_ERROR",
                          std::string("update org failed: ") + e.base().what());
                    });
              }
              catch (...)
              {
                  respondError(req, cb, "DB_QUERY_ERROR", "update org: Mapper construction failed");
              }
          },
          [req, cb](const DrogonDbException &e) {
              if (dynamic_cast<const UnexpectedRows *>(&e) != nullptr)
                  respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "organization not found");
              else
                  respondError(
                    req, cb, "DB_QUERY_ERROR", std::string("org lookup failed: ") + e.base().what());
          });
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "update org: Mapper construction failed");
    }
}

void OrganizationService::deleteOrg(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &slug
)
{
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    using OrgModel = ::drogon_model::fulla_db::Organizations;

    // Resolve the live org first (outside the transaction — the lock key
    // needs the org id).
    try
    {
        Mapper<OrgModel> mapper(db);
        mapper.findOne(
          Criteria(OrgModel::Cols::_slug, CompareOperator::EQ, slug) &&
            Criteria(OrgModel::Cols::_deleted_at, CompareOperator::IsNull),
          [req, cb, db, slug](const OrgModel &org) {
              const int32_t orgId = org.getValueOfId();

              // The closed cascade, ONE advisory-locked transaction keyed on
              // the org domain (serializes against concurrent member/invite
              // writes on the same org).
              ::fulla::storage::postgres::withAdvisoryXactLock(
                db,
                {"org:" + std::to_string(orgId)},
                [req, cb, db, slug, orgId](
                  const std::shared_ptr<::drogon::orm::Transaction> &txn) {
                    auto respondDeleted = [req, cb, slug, orgId]() {
                        auditOrg(req, "org_deleted", slug);
                        Json::Value json;
                        json["status"] = "success";
                        json["message"] = "Organization deleted";
                        json["id"] = orgId;
                        (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                    };
                    auto respondTxnError = [req, cb](const std::string &what) {
                        respondError(
                          req, cb, "DB_QUERY_ERROR",
                          "delete org cascade failed (transaction rolled back): " + what);
                    };

                    // The multi-table cascade below runs as raw SQL inside the
                    // transaction — db-operations.md exemption: documented
                    // bulk operations (V044 design: cascade UPDATE/DELETE
                    // across four child tables in one atomic step; Mapper
                    // per-table would need four coordinated transactions).
                    txn->execSqlAsync(
                      // 1. Soft-delete marker + slug placeholder rewrite:
                      //    the unique constraint keeps the tombstone from
                      //    colliding and BLOCKS slug reuse (anti-confusion:
                      //    audit_logs.org_id has no FK and still points at
                      //    this id).
                      "UPDATE organizations SET deleted_at = now(), "
                      "slug = slug || '#del-' || id::text, updated_at = now() "
                      "WHERE id = $1 AND deleted_at IS NULL",
                      [txn, respondDeleted, respondTxnError, orgId](const Result &) {
                          txn->execSqlAsync(
                            // 2. Void pending invitations (physical delete —
                            //    FK CASCADE never fires on the soft delete
                            //    above, and an accept within the 72h TTL
                            //    would resurrect membership).
                            "DELETE FROM organization_invitations "
                            "WHERE organization_id = $1 AND accepted_at IS NULL",
                            [txn, respondDeleted, respondTxnError, orgId](const Result &) {
                                txn->execSqlAsync(
                                  // 3. Void pending succession nominations
                                  //    (same resurrection class — a soft-deleted
                                  //    org must not change owner seats).
                                  "DELETE FROM organization_succession_nominations "
                                  "WHERE organization_id = $1 AND accepted_at IS NULL",
                                  [txn, respondDeleted, respondTxnError, orgId](const Result &) {
                                      txn->execSqlAsync(
                                        // 4. Revoke live org consents.
                                        "UPDATE organization_consents SET revoked_at = now() "
                                        "WHERE organization_id = $1 AND revoked_at IS NULL",
                                        [txn, respondDeleted, respondTxnError, orgId](const Result &) {
                                            txn->execSqlAsync(
                                              // 5. Suspend owned clients — the
                                              //    proven V035 channel
                                              //    (PostgresClientRepository
                                              //    rejects suspended clients'
                                              //    credentials) + the P1-4
                                              //    introspection overlay.
                                              "UPDATE oauth2_client_owners SET status = 'suspended' "
                                              "WHERE org_id = $1 AND status = 'active'",
                                              [txn, respondDeleted, respondTxnError, orgId](const Result &) {
                                                  txn->execSqlAsync(
                                                    // 6. Detach memberships.
                                                    "DELETE FROM organization_members "
                                                    "WHERE organization_id = $1",
                                                    // #219 pattern: the success response
                                                    // fires from the COMMIT callback — an
                                                    // inline response would race the commit.
                                                    [txn, respondDeleted, respondTxnError](const Result &) {
                                                        txn->setCommitCallback(
                                                          [respondDeleted, respondTxnError](bool committed) {
                                                              if (committed)
                                                                  respondDeleted();
                                                              else
                                                                  respondTxnError("commit failed");
                                                          });
                                                    },
                                                    [respondTxnError](const DrogonDbException &e) {
                                                        respondTxnError(e.base().what());
                                                    },
                                                    orgId);
                                              },
                                              [respondTxnError](const DrogonDbException &e) {
                                                  respondTxnError(e.base().what());
                                              },
                                              orgId);
                                        },
                                        [respondTxnError](const DrogonDbException &e) {
                                            respondTxnError(e.base().what());
                                        },
                                        orgId);
                                  },
                                  [respondTxnError](const DrogonDbException &e) {
                                      respondTxnError(e.base().what());
                                  },
                                  orgId);
                            },
                            [respondTxnError](const DrogonDbException &e) {
                                respondTxnError(e.base().what());
                            },
                            orgId);
                      },
                      [respondTxnError](const DrogonDbException &e) {
                          respondTxnError(e.base().what());
                      },
                      orgId);
                  },
                  [req, cb](const std::string &lockError) {
                      respondError(
                        req, cb, "DB_QUERY_ERROR",
                        "delete org: lock acquisition failed: " + lockError);
                  });
          },
          [req, cb](const DrogonDbException &e) {
              if (dynamic_cast<const UnexpectedRows *>(&e) != nullptr)
                  respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "organization not found");
              else
                  respondError(
                    req, cb, "DB_QUERY_ERROR", std::string("org lookup failed: ") + e.base().what());
          });
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "delete org: Mapper construction failed");
    }
}

void OrganizationService::listMembers(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &slug
)
{
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    using OrgModel = ::drogon_model::fulla_db::Organizations;
    using MemberModel = ::drogon_model::fulla_db::OrganizationMembers;
    using UserModel = ::drogon_model::fulla_db::Users;

    try
    {
        Mapper<OrgModel> mapper(db);
        mapper.findOne(
          Criteria(OrgModel::Cols::_slug, CompareOperator::EQ, slug) &&
            Criteria(OrgModel::Cols::_deleted_at, CompareOperator::IsNull),
          [req, cb, db](const OrgModel &org) {
              try
              {
                  Mapper<MemberModel>(db).findBy(
                    Criteria(MemberModel::Cols::_organization_id, CompareOperator::EQ,
                             org.getValueOfId()),
                    [req, cb, db](const std::vector<MemberModel> &members) {
                        std::vector<int32_t> userIds;
                        userIds.reserve(members.size());
                        for (const auto &m : members)
                            userIds.push_back(m.getValueOfUserId());
                        auto resolveUsers = [req, cb, members](
                                              const std::vector<::drogon_model::fulla_db::Users>
                                                &users) {
                            std::map<int32_t, std::string> names;
                            for (const auto &u : users)
                                names[u.getValueOfId()] = u.getValueOfUsername();
                            Json::Value json;
                            json["status"] = "success";
                            Json::Value arr(Json::arrayValue);
                            for (const auto &m : members)
                            {
                                Json::Value row;
                                row["user_id"] = m.getValueOfUserId();
                                row["username"] = names.count(m.getValueOfUserId())
                                                    ? names[m.getValueOfUserId()]
                                                    : "";
                                row["role"] = m.getValueOfRole();
                                row["joined_at"] = m.getValueOfCreatedAt().toDbString();
                                arr.append(row);
                            }
                            json["members"] = arr;
                            json["total"] = static_cast<Json::Int64>(members.size());
                            (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                        };
                        if (userIds.empty())
                        {
                            resolveUsers({});
                            return;
                        }
                        try
                        {
                            Mapper<UserModel>(db).findBy(
                              Criteria(UserModel::Cols::_id, CompareOperator::In, userIds),
                              [resolveUsers](const std::vector<UserModel> &users) {
                                  resolveUsers(users);
                              },
                              [req, cb](const DrogonDbException &e) {
                                  respondError(
                                    req, cb, "DB_QUERY_ERROR",
                                    std::string("member user lookup failed: ") + e.base().what());
                              });
                        }
                        catch (...)
                        {
                            respondError(req, cb, "DB_QUERY_ERROR", "list members: Mapper construction failed");
                        }
                    },
                    [req, cb](const DrogonDbException &e) {
                        respondError(
                          req, cb, "DB_QUERY_ERROR",
                          std::string("member lookup failed: ") + e.base().what());
                    });
              }
              catch (...)
              {
                  respondError(req, cb, "DB_QUERY_ERROR", "list members: Mapper construction failed");
              }
          },
          [req, cb](const DrogonDbException &e) {
              if (dynamic_cast<const UnexpectedRows *>(&e) != nullptr)
                  respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "organization not found");
              else
                  respondError(
                    req, cb, "DB_QUERY_ERROR", std::string("org lookup failed: ") + e.base().what());
          });
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "list members: Mapper construction failed");
    }
}

void OrganizationService::removeMember(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &slug,
  const std::string &userId
)
{
    int32_t targetUserId = 0;
    try
    {
        targetUserId = std::stoi(userId);
    }
    catch (...)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "remove member: userId must be an integer");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    using OrgModel = ::drogon_model::fulla_db::Organizations;
    using MemberModel = ::drogon_model::fulla_db::OrganizationMembers;

    try
    {
        Mapper<OrgModel> mapper(db);
        mapper.findOne(
          Criteria(OrgModel::Cols::_slug, CompareOperator::EQ, slug) &&
            Criteria(OrgModel::Cols::_deleted_at, CompareOperator::IsNull),
          [req, cb, db, slug, targetUserId](const OrgModel &org) {
              try
              {
                  Mapper<MemberModel>(db).findOne(
                    Criteria(MemberModel::Cols::_organization_id, CompareOperator::EQ,
                             org.getValueOfId()) &&
                      Criteria(MemberModel::Cols::_user_id, CompareOperator::EQ, targetUserId),
                    [req, cb, db, slug, targetUserId, orgId = org.getValueOfId()](
                      const MemberModel &membership) {
                        if (membership.getValueOfRole() == "owner")
                        {
                            // Count owners — a single-owner org must transfer
                            // the seat first (the org would otherwise be
                            // stranded without any manager).
                            try
                            {
                                Mapper<MemberModel>(db).findBy(
                                  Criteria(MemberModel::Cols::_organization_id, CompareOperator::EQ,
                                           orgId) &&
                                    Criteria(MemberModel::Cols::_role, CompareOperator::EQ,
                                             std::string("owner")),
                                  [req, cb, db, slug, targetUserId, orgId](
                                    const std::vector<MemberModel> &owners) {
                                      if (owners.size() <= 1)
                                      {
                                          respondError(
                                            req, cb, "VALIDATION_RESOURCE_CONFLICT",
                                            "remove member: transfer ownership before removing the last owner");
                                          return;
                                      }
                                      deleteMembership(req, cb, db, slug, orgId, targetUserId);
                                  },
                                  [req, cb](const DrogonDbException &e) {
                                      respondError(
                                        req, cb, "DB_QUERY_ERROR",
                                        std::string("owner count failed: ") + e.base().what());
                                  });
                            }
                            catch (...)
                            {
                                respondError(req, cb, "DB_QUERY_ERROR", "remove member: Mapper construction failed");
                            }
                            return;
                        }
                        deleteMembership(req, cb, db, slug, orgId, targetUserId);
                    },
                    [req, cb](const DrogonDbException &e) {
                        if (dynamic_cast<const UnexpectedRows *>(&e) != nullptr)
                            respondError(
                              req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "membership not found");
                        else
                            respondError(
                              req, cb, "DB_QUERY_ERROR",
                              std::string("membership lookup failed: ") + e.base().what());
                    });
              }
              catch (...)
              {
                  respondError(req, cb, "DB_QUERY_ERROR", "remove member: Mapper construction failed");
              }
          },
          [req, cb](const DrogonDbException &e) {
              if (dynamic_cast<const UnexpectedRows *>(&e) != nullptr)
                  respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "organization not found");
              else
                  respondError(
                    req, cb, "DB_QUERY_ERROR", std::string("org lookup failed: ") + e.base().what());
          });
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "remove member: Mapper construction failed");
    }
}

}  // namespace organization
