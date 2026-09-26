#include "OrgMemberService.h"
#include "../openplatform/OpenPlatformConfig.h"

#include <fulla/common/utils/EmailNormalizer.h>
#include <fulla/drogon/adapters/DrogonAuditSink.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/drogon/utils/EmailService.h>
#include <fulla/storage/postgres/AdvisoryLock.h>
#include <fulla/storage/postgres/ClientOwnersRepository.h>
#include <fulla/storage/postgres/OrgConsentRepository.h>
#include <fulla/storage/postgres/OrgConsentRequestRepository.h>
#include <fulla/storage/postgres/OrgSuccessionRepository.h>
#include <fulla/storage/postgres/models/Oauth2ClientScopes.h>
#include <fulla/storage/postgres/models/OrganizationConsents.h>
#include <fulla/storage/postgres/models/OrganizationInvitations.h>
#include <fulla/storage/postgres/models/OrganizationMembers.h>
#include <fulla/storage/postgres/models/Organizations.h>
#include <fulla/storage/postgres/models/Users.h>

#include <drogon/drogon.h>

#include <map>
#include <regex>
#include <set>

// v1.4.0 organization membership service. Product-level (same rationale as
// OrganizationService). All DB access is Mapper + Criteria per
// .claude/rules/db-operations.md; every Mapper<...> constructor sits in its
// own try/catch and every failure path routes through (*sharedCb) — no
// swallowed errors, no [this]/[&] captures (all state travels by value in
// the lambda captures).

namespace organization
{

using ResponseCallback = OrgMemberService::ResponseCallback;

namespace
{
using namespace ::drogon::orm;
using UserModel = ::drogon_model::fulla_db::Users;
using OrgModel = ::drogon_model::fulla_db::Organizations;
using MemberModel = ::drogon_model::fulla_db::OrganizationMembers;
using InviteModel = ::drogon_model::fulla_db::OrganizationInvitations;
using OrgConsentModel = ::drogon_model::fulla_db::OrganizationConsents;
using SuccessionModel = ::drogon_model::fulla_db::OrganizationSuccessionNominations;
using ::fulla::storage::postgres::withAdvisoryXactLock;

constexpr int64_t kInviteTtlSeconds = 72 * 3600;

void respondError(
  const ::drogon::HttpRequestPtr &req,
  const ResponseCallback &cb,
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
  const ::drogon::HttpRequestPtr &req, const ResponseCallback &cb
)
{
    try
    {
        return ::drogon::app().getDbClient();
    }
    catch (...)
    {
        respondError(req, cb, "DB_CONNECTION_ERROR", "org members: database unavailable");
        return nullptr;
    }
}

struct ResolvedUser
{
    int32_t id;
    std::string email;
    std::string username;
    bool emailVerified = false;  // review M7: invitation acceptance requires it
};

using ResolvedCallback = std::function<void(bool found, const ResolvedUser &user)>;

void resolveCaller(
  const DbClientPtr &db,
  const std::string &userIdAttr,
  const ::drogon::HttpRequestPtr &req,
  const ResponseCallback &cb,
  ResolvedCallback &&onResolved
)
{
    bool isNumeric = false;
    int32_t numericId = 0;
    try
    {
        size_t pos = 0;
        int parsed = std::stoi(userIdAttr, &pos);
        isNumeric = (pos == userIdAttr.length());
        if (isNumeric)
            numericId = parsed;
    }
    catch (...)
    {
        isNumeric = false;
    }

    try
    {
        Mapper<UserModel> mapper(db);
        mapper.findBy(
          (isNumeric ? Criteria(UserModel::Cols::_id, CompareOperator::EQ, numericId)
                     : Criteria(UserModel::Cols::_public_sub, CompareOperator::EQ, userIdAttr)) &&
            Criteria(UserModel::Cols::_deleted_at, CompareOperator::IsNull),
          [cb, req, onResolved = std::move(onResolved)](const std::vector<UserModel> &users) {
              if (users.empty())
              {
                  respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "user not found");
                  return;
              }
              ResolvedUser u;
              u.id = users[0].getValueOfId();
              u.email = users[0].getValueOfEmail();
              u.username = users[0].getValueOfUsername();
              try
              {
                  u.emailVerified = users[0].getValueOfEmailVerified();
              }
              catch (...)
              {
              }
              onResolved(true, u);
          },
          [req, cb](const DrogonDbException &e) {
              respondError(req, cb, "DB_QUERY_ERROR", std::string("user lookup failed: ") + e.base().what());
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "user lookup: Mapper construction failed");
    }
}

using OrgCallback = std::function<void(bool found, const OrgModel &org)>;
using MembershipCallback = std::function<void(bool member, std::string role)>;

void findOrgBySlug(
  const DbClientPtr &db,
  const std::string &slug,
  const ::drogon::HttpRequestPtr &req,
  const ResponseCallback &cb,
  OrgCallback &&onFound
)
{
    try
    {
        Mapper<OrgModel> mapper(db);
        mapper.findOne(
          Criteria(OrgModel::Cols::_slug, CompareOperator::EQ, slug),
          [cb, req, onFound = std::move(onFound)](const OrgModel &org) {
              onFound(true, org);
          },
          [req, cb](const DrogonDbException &e) {
              LOG_WARN << "findOrgBySlug: not found or DB error: " << e.base().what();
              respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "organization not found");
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "org lookup: Mapper construction failed");
    }
}

void getMembership(
  const DbClientPtr &db,
  int32_t orgId,
  int32_t userId,
  const ::drogon::HttpRequestPtr &req,
  const ResponseCallback &cb,
  MembershipCallback &&onCheck
)
{
    try
    {
        Mapper<MemberModel> mapper(db);
        mapper.findBy(
          Criteria(MemberModel::Cols::_organization_id, CompareOperator::EQ, orgId) &&
            Criteria(MemberModel::Cols::_user_id, CompareOperator::EQ, userId),
          [cb, req, onCheck = std::move(onCheck)](const std::vector<MemberModel> &rows) {
              if (rows.empty())
                  onCheck(false, "");
              else
                  onCheck(true, rows[0].getValueOfRole());
          },
          [req, cb](const DrogonDbException &e) {
              respondError(req, cb, "DB_QUERY_ERROR", std::string("membership lookup failed: ") + e.base().what());
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "membership lookup: Mapper construction failed");
    }
}

bool isManagerRole(const std::string &role)
{
    return role == "owner" || role == "admin";
}

// createInvitation tail helper (defined below the public methods; forward
// declaration because the pending-cap count() callback calls it).
void proceedWithInvitationInsert(
  const ::drogon::HttpRequestPtr &req,
  const organization::OrgMemberService::ResponseCallback &cb,
  const std::shared_ptr<::drogon::orm::Transaction> &db,
  const ::drogon_model::fulla_db::Organizations &org,
  const std::string &email,
  const std::string &role,
  const ResolvedUser &caller
);

void audit(
  const ::drogon::HttpRequestPtr &req,
  const char *action,
  const std::string &targetId
)
{
    auto *plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
    if (plugin)
    {
        ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
          plugin->getAuditSink(), action, "success", req, "", "organization", targetId
        );
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// POST /api/me/organizations  {slug, name, logo_uri?, primary_color?}
// ---------------------------------------------------------------------------
void OrgMemberService::createOrg(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    // Review C4: self-service org creation rides the SAME fail-closed master
    // switch as application registration (open_platform.enabled) — the
    // config block documents both as off until an operator enables it.
    if (!openplatform::OpenPlatformConfig::load().enabled)
    {
        respondError(
          req, cb, "AUTHZ_ACCESS_DENIED",
          "organization self-service is disabled on this deployment"
        );
        return;
    }

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

    // Same shape rules as the admin endpoint (OrganizationService::create).
    std::regex slugPattern("^[a-z0-9][a-z0-9-]{1,48}[a-z0-9]$");
    if (!std::regex_match(slug, slugPattern))
    {
        respondError(
          req, cb, "VALIDATION_FORMAT_ERROR",
          "create org: slug must be 3-50 chars, lowercase alphanumeric + hyphens"
        );
        return;
    }
    // Reserved slugs (ratified 2026-09-17): never allow org slugs that would
    // shadow product routes/identifiers.
    static const std::set<std::string> kReservedSlugs = {
        "admin", "api", "me", "oauth2", "callback", "login", "logout",
        "register", "settings", "apps", "organizations", "new", "org"
    };
    if (kReservedSlugs.count(slug) != 0)
    {
        respondError(req, cb, "VALIDATION_FORMAT_ERROR", "create org: slug is reserved");
        return;
    }
    if (name.empty() || name.size() > 100)
    {
        respondError(req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "create org: name is required (<=100 chars)");
        return;
    }

    const auto cfg = openplatform::OpenPlatformConfig::load();

    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, cfg, slug, name, logoUri, primaryColor](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          // #219 (R-M2-5): the quota count and BOTH inserts run inside one
          // transaction holding the per-user advisory lock
          // (quota:user:<id>) -- concurrent createOrg calls serialize, so
          // max_orgs_per_user cannot be exceeded at the boundary. Side
          // effect: org row + owner membership are now atomic (previously
          // two independent statements).
          withAdvisoryXactLock(
            db,
            {"quota:user:" + std::to_string(caller.id)},
            [req, cb, cfg, slug, name, logoUri, primaryColor, caller](
              const std::shared_ptr<::drogon::orm::Transaction> &txn) {
              // Quota: orgs where the caller is owner.
              try
              {
                  Mapper<MemberModel>(txn).findBy(
                    Criteria(MemberModel::Cols::_user_id, CompareOperator::EQ, caller.id) &&
                      Criteria(MemberModel::Cols::_role, CompareOperator::EQ, "owner"),
                    [req, cb, txn, cfg, slug, name, logoUri, primaryColor, caller](
                      const std::vector<MemberModel> &owned) {
                      if (static_cast<int>(owned.size()) >= cfg.maxOrgsPerUser)
                      {
                          respondError(
                            req, cb, "VALIDATION_RESOURCE_CONFLICT",
                            "create org: organization quota exceeded (" +
                              std::to_string(cfg.maxOrgsPerUser) + ")"
                          );
                          return;
                      }
                      OrgModel row;
                      row.setSlug(slug);
                      row.setName(name);
                      row.setLogoUri(logoUri);
                      row.setPrimaryColor(primaryColor);
                      try
                      {
                          Mapper<OrgModel>(txn).insert(
                            row,
                            [req, cb, txn, slug, caller](const OrgModel &inserted) {
                                MemberModel member;
                                member.setOrganizationId(inserted.getValueOfId());
                                member.setUserId(caller.id);
                                member.setRole("owner");
                                try
                                {
                                    Mapper<MemberModel>(txn).insert(
                                      member,
                                      [req, cb, txn, slug](const MemberModel &) {
                                          // #219: the 201 fires from the COMMIT
                                          // callback (an inline response races
                                          // the commit; an immediate follow-up
                                          // read could miss the new org).
                                          txn->setCommitCallback(
                                            [req, cb, slug](bool committed) {
                                                if (!committed)
                                                {
                                                    respondError(
                                                      req, cb, "DB_QUERY_ERROR",
                                                      "create org: commit failed");
                                                    return;
                                                }
                                                audit(req, "organization_created", slug);
                                                Json::Value json;
                                                json["slug"] = slug;
                                                json["role"] = "owner";
                                                json["message"] = "Organization created";
                                                auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                                                resp->setStatusCode(::drogon::k201Created);
                                                (*cb)(resp);
                                            });
                                      },
                                      [req, cb](const DrogonDbException &e) {
                                          respondError(req, cb, "DB_QUERY_ERROR",
                                            std::string("create org: owner membership insert failed: ") + e.base().what());
                                      }
                                    );
                                }
                                catch (...)
                                {
                                    respondError(req, cb, "DB_QUERY_ERROR", "create org: Mapper construction failed");
                                }
                            },
                            [req, cb](const DrogonDbException &e) {
                                respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT",
                                  std::string("create org: slug already exists or DB error: ") + e.base().what());
                            }
                          );
                      }
                      catch (...)
                      {
                          respondError(req, cb, "DB_QUERY_ERROR", "create org: Mapper construction failed");
                      }
                    },
                    [req, cb](const DrogonDbException &e) {
                        respondError(req, cb, "DB_QUERY_ERROR", std::string("quota check failed: ") + e.base().what());
                    }
                  );
              }
              catch (...)
              {
                  respondError(req, cb, "DB_QUERY_ERROR", "quota check: Mapper construction failed");
              }
            },
            [req, cb](const std::string &err) {
                respondError(req, cb, "DB_QUERY_ERROR",
                  "create org: quota serialization failed: " + err);
            });
      });
}

// ---------------------------------------------------------------------------
// GET /api/me/organizations
// ---------------------------------------------------------------------------
void OrgMemberService::listMyOrgs(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          try
          {
              Mapper<MemberModel>(db).findBy(
                Criteria(MemberModel::Cols::_user_id, CompareOperator::EQ, caller.id),
                [req, cb, db, caller](const std::vector<MemberModel> &memberships) {
                    if (memberships.empty())
                    {
                        // Not a member anywhere -- but they may still be a
                        // pending succession NOMINEE (a nominee need not be a
                        // member; M3's discovery surface must not dead-end
                        // here).
                        auto repos = std::make_shared<
                          ::fulla::storage::postgres::OrgSuccessionRepository>(db);
                        repos->findPendingWithOrgForNominee(
                          caller.id,
                          [req, cb](
                            const std::vector<
                              ::fulla::storage::postgres::OrgSuccessionRepository::
                                NomineeNomination> &forYou) {
                              Json::Value json;
                              json["organizations"] = Json::Value(Json::arrayValue);
                              json["total"] = 0;
                              Json::Value forYouArr(Json::arrayValue);
                              for (const auto &n : forYou)
                              {
                                  Json::Value fy;
                                  fy["organization_id"] = n.org.getValueOfId();
                                  fy["slug"] = n.org.getValueOfSlug();
                                  fy["name"] = n.org.getValueOfName();
                                  fy["created_at"] =
                                    n.nomination.getValueOfCreatedAt().toDbStringLocal();
                                  forYouArr.append(fy);
                              }
                              json["pending_succession_nominations"] = forYouArr;
                              (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                          }
                        );
                        return;
                    }
                    std::vector<int32_t> orgIds;
                    std::map<int32_t, std::string> roleByOrg;
                    for (const auto &m : memberships)
                    {
                        orgIds.push_back(m.getValueOfOrganizationId());
                        roleByOrg[m.getValueOfOrganizationId()] = m.getValueOfRole();
                    }
                    try
                    {
                        Mapper<OrgModel>(db).findBy(
                          Criteria(OrgModel::Cols::_id, CompareOperator::In, orgIds),
                          [req, cb, db, roleByOrg, caller](const std::vector<OrgModel> &orgs) {
                              // v1.5.0 M3 (R-M3-3 visibility): owner/admin
                              // entries carry the org's pending succession
                              // nomination; the caller's own pending
                              // nominations (they may be a NON-member
                              // nominee -- the accept banner's discovery
                              // surface) ride as a sibling array.
                              std::vector<int32_t> managerOrgIds;
                              for (const auto &kv : roleByOrg)
                              {
                                  if (isManagerRole(kv.second))
                                      managerOrgIds.push_back(kv.first);
                              }
                              auto repos = std::make_shared<
                                ::fulla::storage::postgres::OrgSuccessionRepository>(db);
                              repos->findPendingForOrgs(
                                managerOrgIds,
                                [req, cb, roleByOrg, caller, orgs, repos](
                                  const std::vector<SuccessionModel> &pendingRows) {
                                    // Store BY VALUE: the rows vector is
                                    // destroyed when this callback returns,
                                    // and the continuation below dereferences
                                    // the entries two async hops later
                                    // (pointers would dangle -- review
                                    // finding 3).
                                    std::map<int32_t, SuccessionModel> pendingByOrg;
                                    for (const auto &n : pendingRows)
                                        pendingByOrg[n.getValueOfOrganizationId()] = n;
                                    repos->findPendingWithOrgForNominee(
                                      caller.id,
                                      [req, cb, roleByOrg, orgs, pendingByOrg](
                                        const std::vector<
                                          ::fulla::storage::postgres::OrgSuccessionRepository::
                                            NomineeNomination> &forYou) {
                                          Json::Value json;
                                          Json::Value arr(Json::arrayValue);
                                          for (const auto &org : orgs)
                                          {
                                              Json::Value o;
                                              o["id"] = org.getValueOfId();
                                              o["slug"] = org.getValueOfSlug();
                                              o["name"] = org.getValueOfName();
                                              o["logo_uri"] = org.getValueOfLogoUri();
                                              o["primary_color"] =
                                                org.getValueOfPrimaryColor();
                                              o["role"] = roleByOrg.count(org.getValueOfId())
                                                            ? roleByOrg.at(org.getValueOfId())
                                                            : "member";
                                              Json::Value nomination =
                                                Json::Value(Json::nullValue);
                                              const bool manager =
                                                roleByOrg.count(org.getValueOfId()) > 0 &&
                                                isManagerRole(
                                                  roleByOrg.at(org.getValueOfId())
                                                );
                                              if (manager)
                                              {
                                                  auto it =
                                                    pendingByOrg.find(org.getValueOfId());
                                                  if (it != pendingByOrg.end())
                                                  {
                                                      nomination =
                                                        Json::Value(Json::objectValue);
                                                      nomination["nominee_user_id"] =
                                                        it->second.getValueOfNomineeUserId();
                                                      nomination["nominated_by"] =
                                                        it->second.getValueOfNominatedBy();
                                                      nomination["created_at"] =
                                                        it->second.getValueOfCreatedAt()
                                                          .toDbStringLocal();
                                                  }
                                              }
                                              o["successor_nomination"] = nomination;
                                              arr.append(o);
                                          }
                                          json["organizations"] = arr;
                                          json["total"] = static_cast<int>(arr.size());
                                          Json::Value forYouArr(Json::arrayValue);
                                          for (const auto &n : forYou)
                                          {
                                              Json::Value fy;
                                              fy["organization_id"] =
                                                n.org.getValueOfId();
                                              fy["slug"] = n.org.getValueOfSlug();
                                              fy["name"] = n.org.getValueOfName();
                                              fy["created_at"] =
                                                n.nomination.getValueOfCreatedAt()
                                                  .toDbStringLocal();
                                              forYouArr.append(fy);
                                          }
                                          json["pending_succession_nominations"] = forYouArr;
                                          (*cb)(
                                            ::drogon::HttpResponse::newHttpJsonResponse(json)
                                          );
                                      }
                                    );
                                }
                              );
                          },
                          [req, cb](const DrogonDbException &e) {
                              respondError(req, cb, "DB_QUERY_ERROR",
                                std::string("org lookup failed: ") + e.base().what());
                          }
                        );
                    }
                    catch (...)
                    {
                        respondError(req, cb, "DB_QUERY_ERROR", "org lookup: Mapper construction failed");
                    }
                },
                [req, cb](const DrogonDbException &e) {
                    respondError(req, cb, "DB_QUERY_ERROR", std::string("memberships lookup failed: ") + e.base().what());
                }
              );
          }
          catch (...)
          {
              respondError(req, cb, "DB_QUERY_ERROR", "memberships lookup: Mapper construction failed");
          }
      });
}

// ---------------------------------------------------------------------------
// GET /api/me/organizations/{slug}/members
// ---------------------------------------------------------------------------
void OrgMemberService::listMembers(
  const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &slug
)
{
    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, slug](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          findOrgBySlug(db, slug, req, cb,
            [req, cb, db, caller](bool, const OrgModel &org) {
                getMembership(db, org.getValueOfId(), caller.id, req, cb,
                  [req, cb, db, org](bool isMember, const std::string &) {
                      if (!isMember)
                      {
                          respondError(req, cb, "AUTHZ_ACCESS_DENIED", "not a member of this organization");
                          return;
                      }
                      try
                      {
                          Mapper<MemberModel>(db).findBy(
                            Criteria(MemberModel::Cols::_organization_id, CompareOperator::EQ, org.getValueOfId()),
                            [req, cb, db](const std::vector<MemberModel> &rows) {
                                if (rows.empty())
                                {
                                    Json::Value json;
                                    json["members"] = Json::Value(Json::arrayValue);
                                    json["total"] = 0;
                                    (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                                    return;
                                }
                                std::vector<int32_t> userIds;
                                for (const auto &r : rows)
                                    userIds.push_back(r.getValueOfUserId());
                                try
                                {
                                    Mapper<UserModel>(db).findBy(
                                      Criteria(UserModel::Cols::_id, CompareOperator::In, userIds) &&
                                        Criteria(UserModel::Cols::_deleted_at,
                                                 CompareOperator::IsNull),
                                      [req, cb, rows](const std::vector<UserModel> &users) {
                                          std::map<int32_t, const UserModel *> byId;
                                          for (const auto &u : users)
                                              byId[u.getValueOfId()] = &u;
                                          Json::Value json;
                                          Json::Value arr(Json::arrayValue);
                                          for (const auto &r : rows)
                                          {
                                              Json::Value m;
                                              m["user_id"] = r.getValueOfUserId();
                                              m["role"] = r.getValueOfRole();
                                              auto it = byId.find(r.getValueOfUserId());
                                              if (it != byId.end())
                                              {
                                                  m["username"] = it->second->getValueOfUsername();
                                                  m["display_name"] = it->second->getValueOfDisplayName();
                                              }
                                              arr.append(m);
                                          }
                                          json["members"] = arr;
                                          json["total"] = static_cast<int>(arr.size());
                                          (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                                      },
                                      [req, cb](const DrogonDbException &e) {
                                          respondError(req, cb, "DB_QUERY_ERROR",
                                            std::string("member users lookup failed: ") + e.base().what());
                                      }
                                    );
                                }
                                catch (...)
                                {
                                    respondError(req, cb, "DB_QUERY_ERROR", "member users lookup: Mapper construction failed");
                                }
                            },
                            [req, cb](const DrogonDbException &e) {
                                respondError(req, cb, "DB_QUERY_ERROR",
                                  std::string("members lookup failed: ") + e.base().what());
                            }
                          );
                      }
                      catch (...)
                      {
                          respondError(req, cb, "DB_QUERY_ERROR", "members lookup: Mapper construction failed");
                      }
                  });
            });
      });
}

// ---------------------------------------------------------------------------
// DELETE /api/me/organizations/{slug}/members/{userId}
// Permission matrix: owner removes anyone except self (single-owner rule);
// admin removes members only; member removes self only (leave).
// ---------------------------------------------------------------------------
void OrgMemberService::removeMember(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &slug,
  const std::string &userIdStr
)
{
    int32_t targetUserId = 0;
    try
    {
        size_t pos = 0;
        targetUserId = std::stoi(userIdStr, &pos);
        if (pos != userIdStr.length())
            throw std::invalid_argument("trailing");
    }
    catch (...)
    {
        respondError(req, cb, "VALIDATION_FORMAT_ERROR", "remove member: invalid user id");
        return;
    }

    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, slug, targetUserId](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          findOrgBySlug(db, slug, req, cb,
            [req, cb, db, caller, targetUserId](bool, const OrgModel &org) {
                getMembership(db, org.getValueOfId(), caller.id, req, cb,
                  [req, cb, db, org, caller, targetUserId](bool isMember, const std::string &role) {
                      if (!isMember)
                      {
                          respondError(req, cb, "AUTHZ_ACCESS_DENIED", "not a member of this organization");
                          return;
                      }
                      const bool selfRemoval = (caller.id == targetUserId);
                      bool allowed = false;
                      if (role == "owner")
                          allowed = !selfRemoval;  // owner seat is irremovable
                      else if (role == "admin")
                          allowed = true;  // admins may remove anyone (owner protected below)
                      else
                          allowed = selfRemoval;  // member may only leave
                      if (!allowed)
                      {
                          respondError(req, cb, "AUTHZ_ACCESS_DENIED", "insufficient role for this removal");
                          return;
                      }
                      getMembership(db, org.getValueOfId(), targetUserId, req, cb,
                        [req, cb, db, org, role, selfRemoval, targetUserId](
                          bool targetIsMember, const std::string &targetRole) {
                            if (!targetIsMember)
                            {
                                respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "target is not a member");
                                return;
                            }
                            if (targetRole == "owner")
                            {
                                respondError(req, cb, "AUTHZ_ACCESS_DENIED",
                                  "the organization owner cannot be removed (transfer is out of scope)");
                                return;
                            }
                            if (role == "admin" && targetRole == "admin" && !selfRemoval)
                            {
                                respondError(req, cb, "AUTHZ_ACCESS_DENIED",
                                  "admins can only remove members");
                                return;
                            }
                            try
                            {
                                Mapper<MemberModel>(db).deleteBy(
                                  Criteria(MemberModel::Cols::_organization_id, CompareOperator::EQ, org.getValueOfId()) &&
                                    Criteria(MemberModel::Cols::_user_id, CompareOperator::EQ, targetUserId),
                                  [req, cb, org, selfRemoval](const std::size_t count) {
                                      if (count == 0)
                                      {
                                          respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "membership not found");
                                          return;
                                      }
                                      audit(req, selfRemoval ? "org_member_left" : "org_member_removed",
                                        org.getValueOfSlug());
                                      Json::Value json;
                                      json["message"] = "Member removed";
                                      (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                                  },
                                  [req, cb](const DrogonDbException &e) {
                                      respondError(req, cb, "DB_QUERY_ERROR",
                                        std::string("member removal failed: ") + e.base().what());
                                  }
                                );
                            }
                            catch (...)
                            {
                                respondError(req, cb, "DB_QUERY_ERROR", "member removal: Mapper construction failed");
                            }
                        });
                  });
            });
      });
}

// ---------------------------------------------------------------------------
// POST /api/me/organizations/{slug}/invitations  {email, role?}
// Owner/admin only. Email normalized (V031 semantics); one PENDING invite
// per (org, email); token returned ONCE in the response (no email delivery
// in v1.4.0 — share out-of-band or accept via the portal token flow).
// ---------------------------------------------------------------------------
void OrgMemberService::createInvitation(
  const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &slug
)
{
    auto jsonBody = req->getJsonObject();
    if (!jsonBody)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "invite: JSON body required");
        return;
    }
    std::string email = ::fulla::common::utils::normalizeEmail((*jsonBody).get("email", "").asString());
    std::string role = (*jsonBody).get("role", "member").asString();
    if (email.empty() || email.find('@') == std::string::npos)
    {
        respondError(req, cb, "VALIDATION_FORMAT_ERROR", "invite: a valid email is required");
        return;
    }
    if (role != "member" && role != "admin")
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "invite: role must be member or admin");
        return;
    }

    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, slug, email, role](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          findOrgBySlug(db, slug, req, cb,
            [req, cb, db, caller, email, role](bool, const OrgModel &org) {
                getMembership(db, org.getValueOfId(), caller.id, req, cb,
                  [req, cb, db, org, email, role, caller](bool isMember, const std::string &memberRole) {
                      if (!isManagerRole(memberRole) || !isMember)
                      {
                          respondError(req, cb, "AUTHZ_ACCESS_DENIED", "owner or admin role required");
                          return;
                      }
                      // Review M6: bound the mail relay — at most N pending
                      // invitations per org (config, default 20). #219
                      // (R-M2-5): the count gate and the insert run inside
                      // one transaction holding the per-org advisory lock
                      // (quota:org:<id>), so concurrent invites cannot push
                      // past the cap at the boundary.
                      const int pendingCap =
                        openplatform::OpenPlatformConfig::load().maxPendingInvitationsPerOrg;
                      withAdvisoryXactLock(
                        db,
                        {"quota:org:" + std::to_string(org.getValueOfId())},
                        [req, cb, org, email, role, caller, pendingCap](
                          const std::shared_ptr<::drogon::orm::Transaction> &txn) {
                          try
                          {
                              Mapper<InviteModel>(txn).count(
                                Criteria(InviteModel::Cols::_organization_id,
                                         CompareOperator::EQ, org.getValueOfId()) &&
                                  Criteria(InviteModel::Cols::_accepted_at,
                                           CompareOperator::IsNull),
                                [req, cb, txn, org, email, role, caller, pendingCap](
                                  const size_t pending) {
                                    if (static_cast<int>(pending) >= pendingCap)
                                    {
                                        respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT",
                                          "invite: too many pending invitations for this organization");
                                        return;
                                    }
                                    proceedWithInvitationInsert(req, cb, txn, org, email, role, caller);
                                },
                                [req, cb](const DrogonDbException &e) {
                                    respondError(req, cb, "DB_QUERY_ERROR",
                                      std::string("invite: pending count failed: ") + e.base().what());
                                });
                          }
                          catch (...)
                          {
                              respondError(req, cb, "DB_QUERY_ERROR",
                                "invite: count Mapper construction failed");
                          }
                        },
                        [req, cb](const std::string &err) {
                            respondError(req, cb, "DB_QUERY_ERROR",
                              "invite: cap serialization failed: " + err);
                        });
                  });
            });
      });
}

// createInvitation tail: the actual insert, split out so the pending-cap
// count() callback can continue into it (re-opened anonymous namespace to
// match the forward declaration above). Runs inside the #219 advisory
// transaction; the 201 + email delivery fire from the COMMIT callback.
namespace {
void proceedWithInvitationInsert(
  const ::drogon::HttpRequestPtr &req,
  const organization::OrgMemberService::ResponseCallback &cb,
  const std::shared_ptr<::drogon::orm::Transaction> &db,
  const ::drogon_model::fulla_db::Organizations &org,
  const std::string &email,
  const std::string &role,
  const ResolvedUser &caller)
{
                      InviteModel invite;
                      invite.setOrganizationId(org.getValueOfId());
                      invite.setEmail(email);
                      invite.setRole(role);
                      invite.setToken(::fulla::drogon::utils::generateSecureToken());
                      invite.setInvitedBy(caller.id);
                      const int64_t now = ::trantor::Date::now().secondsSinceEpoch();
                      invite.setExpiresAt(::trantor::Date((now + kInviteTtlSeconds) * 1000000));
                      try
                      {
                          Mapper<InviteModel>(db).insert(
                            invite,
                            [req, cb, db, org](const InviteModel &inserted) {
                                // #219: the 201 + email fire from the COMMIT
                                // callback (an inline response races the
                                // commit; the email must reference a durable
                                // invitation row).
                                db->setCommitCallback(
                                  [req, cb, org, inserted](bool committed) {
                                      if (!committed)
                                      {
                                          respondError(req, cb, "DB_QUERY_ERROR",
                                            "invite: commit failed");
                                          return;
                                      }
                                      audit(req, "org_invitation_created", org.getValueOfSlug());
                                      // Fire-and-forget delivery (same pattern as
                                      // EmailVerificationService): with SMTP
                                      // configured the invitee gets the token by
                                      // mail; in Console mode (no SMTP) it just
                                      // logs — the admin UI response still carries
                                      // the token for out-of-band delivery.
                                      const std::string mailBody =
                                        "You have been invited to join the organization \"" +
                                        org.getValueOfName() +
                                        "\" on Fulla.\n\n"
                                        "Invitation token (valid for 72 hours, single use):\n  " +
                                        inserted.getValueOfToken() +
                                        "\n\n"
                                        "Sign in to the portal, open My Organizations, and paste "
                                        "the token under \"Accept an invitation\". If you did not "
                                        "expect this invitation you can ignore this email.";
                                      ::fulla::drogon::utils::getEmailService().sendEmail(
                                        inserted.getValueOfEmail(),
                                        "Fulla Organization Invitation",
                                        mailBody,
                                        [](bool ok) {
                                            if (!ok)
                                            {
                                                LOG_WARN << "org invitation email delivery failed";
                                            }
                                        });
                                      Json::Value json;
                                      json["id"] = inserted.getValueOfId();
                                      json["email"] = inserted.getValueOfEmail();
                                      json["role"] = inserted.getValueOfRole();
                                      json["token"] = inserted.getValueOfToken();
                                      json["expires_at"] = inserted.getValueOfExpiresAt().toDbStringLocal();
                                      json["message"] = "Invitation created; deliver the token out-of-band";
                                      auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                                      resp->setStatusCode(::drogon::k201Created);
                                      (*cb)(resp);
                                  });
                            },
                            [req, cb](const DrogonDbException &e) {
                                respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT",
                                  std::string("invite: a pending invitation for this email already exists or DB error: ") +
                                    e.base().what());
                            }
                          );
                      }
                      catch (...)
                      {
                          respondError(req, cb, "DB_QUERY_ERROR", "invite: Mapper construction failed");
                      }
}  // namespace
}  // closes proceedWithInvitationInsert

// ---------------------------------------------------------------------------
// GET /api/me/organizations/{slug}/invitations — pending only (owner/admin).
// ---------------------------------------------------------------------------
void OrgMemberService::listInvitations(
  const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &slug
)
{
    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, slug](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          findOrgBySlug(db, slug, req, cb,
            [req, cb, db, caller](bool, const OrgModel &org) {
                getMembership(db, org.getValueOfId(), caller.id, req, cb,
                  [req, cb, db, org](bool isMember, const std::string &memberRole) {
                      if (!isManagerRole(memberRole) || !isMember)
                      {
                          respondError(req, cb, "AUTHZ_ACCESS_DENIED", "owner or admin role required");
                          return;
                      }
                      try
                      {
                          Mapper<InviteModel>(db).findBy(
                            Criteria(InviteModel::Cols::_organization_id, CompareOperator::EQ, org.getValueOfId()) &&
                              Criteria(InviteModel::Cols::_accepted_at, CompareOperator::IsNull),
                            [req, cb](const std::vector<InviteModel> &rows) {
                                Json::Value json;
                                Json::Value arr(Json::arrayValue);
                                for (const auto &r : rows)
                                {
                                    Json::Value i;
                                    i["id"] = r.getValueOfId();
                                    i["email"] = r.getValueOfEmail();
                                    i["role"] = r.getValueOfRole();
                                    i["expires_at"] = r.getValueOfExpiresAt().toDbStringLocal();
                                    arr.append(i);
                                }
                                json["invitations"] = arr;
                                json["total"] = static_cast<int>(arr.size());
                                (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                            },
                            [req, cb](const DrogonDbException &e) {
                                respondError(req, cb, "DB_QUERY_ERROR",
                                  std::string("invitations lookup failed: ") + e.base().what());
                            }
                          );
                      }
                      catch (...)
                      {
                          respondError(req, cb, "DB_QUERY_ERROR", "invitations lookup: Mapper construction failed");
                      }
                  });
            });
      });
}

// ---------------------------------------------------------------------------
// DELETE /api/me/organizations/{slug}/invitations/{invitationId}
// ---------------------------------------------------------------------------
void OrgMemberService::revokeInvitation(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &slug,
  const std::string &invitationId
)
{
    int32_t inviteId = 0;
    try
    {
        size_t pos = 0;
        inviteId = std::stoi(invitationId, &pos);
        if (pos != invitationId.length())
            throw std::invalid_argument("trailing");
    }
    catch (...)
    {
        respondError(req, cb, "VALIDATION_FORMAT_ERROR", "revoke invite: invalid invitation id");
        return;
    }

    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, slug, inviteId](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          findOrgBySlug(db, slug, req, cb,
            [req, cb, db, caller, inviteId](bool, const OrgModel &org) {
                getMembership(db, org.getValueOfId(), caller.id, req, cb,
                  [req, cb, db, org, inviteId](bool isMember, const std::string &memberRole) {
                      if (!isManagerRole(memberRole) || !isMember)
                      {
                          respondError(req, cb, "AUTHZ_ACCESS_DENIED", "owner or admin role required");
                          return;
                      }
                      try
                      {
                          Mapper<InviteModel>(db).deleteBy(
                            Criteria(InviteModel::Cols::_id, CompareOperator::EQ, inviteId) &&
                              Criteria(InviteModel::Cols::_organization_id, CompareOperator::EQ, org.getValueOfId()),
                            [req, cb, org](const std::size_t count) {
                                if (count == 0)
                                {
                                    respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "invitation not found");
                                    return;
                                }
                                audit(req, "org_invitation_revoked", org.getValueOfSlug());
                                Json::Value json;
                                json["message"] = "Invitation revoked";
                                (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                            },
                            [req, cb](const DrogonDbException &e) {
                                respondError(req, cb, "DB_QUERY_ERROR",
                                  std::string("invite revoke failed: ") + e.base().what());
                            }
                          );
                      }
                      catch (...)
                      {
                          respondError(req, cb, "DB_QUERY_ERROR", "invite revoke: Mapper construction failed");
                      }
                  });
            });
      });
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// GET /api/me/organizations/{slug}/consents — v1.5.0 M2 (R-M2-4): active
// org consents grouped by client, each scope with its own granted_by /
// granted_at (rows of one client may carry different grantors after a
// partial re-grant). Owner/admin only. Queries live in the shared
// OrgConsentRepository (storage-postgres; arch-guard R4 keeps this
// service Mapper-free here).
// ---------------------------------------------------------------------------
void OrgMemberService::listOrgConsents(
  const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &slug
)
{
    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, slug](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          findOrgBySlug(db, slug, req, cb,
            [req, cb, db, caller](bool, const OrgModel &org) {
                getMembership(db, org.getValueOfId(), caller.id, req, cb,
                  [req, cb, db, org](bool isMember, const std::string &memberRole) {
                      if (!isManagerRole(memberRole) || !isMember)
                      {
                          respondError(req, cb, "AUTHZ_ACCESS_DENIED", "owner or admin role required");
                          return;
                      }
                      auto repo = std::make_shared<
                        ::fulla::storage::postgres::OrgConsentRepository>(db);
                      repo->listActiveByOrg(
                        org.getValueOfId(),
                        [repo, req, cb, org](const std::vector<OrgConsentModel> &rows) {
                            // Group by client (std::map: ordered by client_id).
                            std::map<std::string, Json::Value> byClient;
                            for (const auto &r : rows)
                            {
                                const std::string cid = r.getValueOfClientId();
                                if (byClient.find(cid) == byClient.end())
                                {
                                    Json::Value group;
                                    group["client_id"] = cid;
                                    group["scopes"] = Json::Value(Json::arrayValue);
                                    byClient[cid] = group;
                                }
                                Json::Value sc;
                                sc["scope"] = r.getValueOfScopeName();
                                sc["granted_by"] = r.getValueOfGrantedBy();
                                sc["granted_at"] = r.getValueOfGrantedAt().toDbStringLocal();
                                byClient[cid]["scopes"].append(sc);
                            }
                            Json::Value json;
                            Json::Value arr(Json::arrayValue);
                            for (const auto &kv : byClient)
                                arr.append(kv.second);
                            json["slug"] = org.getValueOfSlug();
                            json["consents"] = arr;
                            json["total"] = static_cast<int>(arr.size());
                            (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                        }
                      );
                  });
            });
      });
}

// ---------------------------------------------------------------------------
// DELETE /api/me/organizations/{slug}/consents/{clientId} — R-M2-4: revoke
// the whole (org, client) pair (every active row, revoked_at = now; never
// a physical delete -- history lives on for the audit trail). O4: only
// future authorizations are affected; issued tokens are NOT revoked.
// 404 when no active rows remain (the removeMember count==0 convention).
// ---------------------------------------------------------------------------
void OrgMemberService::revokeOrgConsents(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &slug,
  const std::string &clientId
)
{
    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, slug, clientId](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          findOrgBySlug(db, slug, req, cb,
            [req, cb, db, caller, clientId](bool, const OrgModel &org) {
                getMembership(db, org.getValueOfId(), caller.id, req, cb,
                  [req, cb, db, org, clientId](
                    bool isMember, const std::string &memberRole) {
                      if (!isManagerRole(memberRole) || !isMember)
                      {
                          respondError(req, cb, "AUTHZ_ACCESS_DENIED", "owner or admin role required");
                          return;
                      }
                      auto repo = std::make_shared<
                        ::fulla::storage::postgres::OrgConsentRepository>(db);
                      repo->revokeClientConsents(
                        org.getValueOfId(),
                        clientId,
                        [repo, req, cb, org, clientId](const size_t revoked) {
                            if (revoked == 0)
                            {
                                respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND",
                                  "no active organization consents for this client");
                                return;
                            }
                            audit(req, "org_consent_revoked",
                              org.getValueOfSlug() + ":" + clientId);
                            Json::Value json;
                            json["slug"] = org.getValueOfSlug();
                            json["client_id"] = clientId;
                            json["revoked"] = static_cast<Json::Int64>(revoked);
                            json["message"] =
                              "Organization consents revoked (future authorizations only)";
                            (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                        }
                      );
                  });
            });
      });
}

// ---------------------------------------------------------------------------
// POST /api/me/organizations/{slug}/successor-nomination {user_id} —
// v1.5.0 M3 (R-M3-3): the owner nominates a successor (any LIVE user --
// may be a non-member; a soft-deleted target would recreate the #221
// deadlock). Overwrites a previous pending nomination (idempotent).
// ---------------------------------------------------------------------------
void OrgMemberService::nominateSuccessor(
  const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &slug
)
{
    auto jsonBody = req->getJsonObject();
    if (!jsonBody || !(*jsonBody).isMember("user_id") || !(*jsonBody)["user_id"].isIntegral())
    {
        respondError(
          req, cb, "VALIDATION_INVALID_INPUT",
          "nominate successor: JSON body with integer user_id required"
        );
        return;
    }
    const int64_t requestedUserId = (*jsonBody)["user_id"].asInt64();
    // Range-check before narrowing (a static_cast would wrap 2^32+1 onto
    // a different real user -- the #228 review's D1 lesson).
    if (requestedUserId <= 0 || requestedUserId > (std::numeric_limits<int32_t>::max)())
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "nominate successor: user_id is out of range");
        return;
    }
    const int32_t targetUserId = static_cast<int32_t>(requestedUserId);

    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, slug, targetUserId](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          findOrgBySlug(db, slug, req, cb,
            [req, cb, db, caller, targetUserId](bool, const OrgModel &org) {
                getMembership(db, org.getValueOfId(), caller.id, req, cb,
                  [req, cb, db, org, caller, targetUserId](
                    bool isMember, const std::string &memberRole) {
                      if (!isMember || memberRole != "owner")
                      {
                          respondError(req, cb, "AUTHZ_ACCESS_DENIED",
                            "only the organization owner may nominate a successor");
                          return;
                      }
                      // Self-nomination re-creates the #221 freeze: the
                      // delete-time auto-effect would pass the seat to the
                      // deleting owner's own (about-to--soft-delete) account
                      // (review finding 5).
                      if (targetUserId == caller.id)
                      {
                          respondError(req, cb, "VALIDATION_INVALID_INPUT",
                            "nominate successor: the owner cannot nominate themselves");
                          return;
                      }
                      auto repos = std::make_shared<
                        ::fulla::storage::postgres::OrgSuccessionRepository>(db);
                      repos->isLiveUser(
                        targetUserId,
                        [repos, req, cb, org, caller, targetUserId](bool live) {
                            if (!live)
                            {
                                respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND",
                                  "nominate successor: user not found");
                                return;
                            }
                            repos->upsertNomination(
                              org.getValueOfId(),
                              targetUserId,
                              caller.id,
                              [req, cb, org, targetUserId](bool ok) {
                                  if (!ok)
                                  {
                                      respondError(req, cb, "DB_QUERY_ERROR",
                                        "nominate successor: write failed");
                                      return;
                                  }
                                  audit(req, "org_successor_nominated",
                                    org.getValueOfSlug() + ":" + std::to_string(targetUserId));
                                  Json::Value json;
                                  json["slug"] = org.getValueOfSlug();
                                  json["nominee_user_id"] = targetUserId;
                                  json["message"] =
                                    "Successor nominated; the nominee must accept to take the seat";
                                  (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                              }
                            );
                        }
                      );
                  });
            });
      });
}

// ---------------------------------------------------------------------------
// DELETE /api/me/organizations/{slug}/successor-nomination — the owner
// withdraws the pending nomination. 404 when none is pending (the
// family's count==0 convention).
// ---------------------------------------------------------------------------
void OrgMemberService::withdrawSuccessionNomination(
  const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &slug
)
{
    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, slug](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          findOrgBySlug(db, slug, req, cb,
            [req, cb, db, caller](bool, const OrgModel &org) {
                getMembership(db, org.getValueOfId(), caller.id, req, cb,
                  [req, cb, db, org](bool isMember, const std::string &memberRole) {
                      if (!isMember || memberRole != "owner")
                      {
                          respondError(req, cb, "AUTHZ_ACCESS_DENIED",
                            "only the organization owner may withdraw a nomination");
                          return;
                      }
                      auto repos = std::make_shared<
                        ::fulla::storage::postgres::OrgSuccessionRepository>(db);
                      repos->withdrawPending(
                        org.getValueOfId(),
                        [req, cb, org](const size_t withdrawn) {
                            if (withdrawn == 0)
                            {
                                respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND",
                                  "no pending successor nomination");
                                return;
                            }
                            audit(req, "org_successor_nomination_withdrawn",
                              org.getValueOfSlug());
                            Json::Value json;
                            json["slug"] = org.getValueOfSlug();
                            json["message"] = "Successor nomination withdrawn";
                            (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                        }
                      );
                  });
            });
      });
}

// ---------------------------------------------------------------------------
// POST /api/me/organizations/{slug}/successor-nomination/accept — only
// the pending nominee. The seat swap is ONE transaction (R-M3-3): the
// nominee's membership becomes owner, every other owner row demotes to
// admin, the nomination row is marked accepted; any failure rolls the
// whole thing back (no #228-style fail-open -- two-step confirmation
// makes atomicity the right bias). False from the swap maps to 409
// (the common cause is the optimistic guard: a concurrent acceptance).
// ---------------------------------------------------------------------------
void OrgMemberService::acceptSuccession(
  const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &slug
)
{
    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, slug](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          findOrgBySlug(db, slug, req, cb,
            [req, cb, db, caller](bool, const OrgModel &org) {
                auto repos = std::make_shared<
                  ::fulla::storage::postgres::OrgSuccessionRepository>(db);
                repos->findPending(
                  org.getValueOfId(),
                  [repos, req, cb, org, caller](
                    const std::optional<SuccessionModel> &pending) {
                      if (!pending.has_value())
                      {
                          respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND",
                            "no pending successor nomination");
                          return;
                      }
                      if (pending->getValueOfNomineeUserId() != caller.id)
                      {
                          respondError(req, cb, "AUTHZ_ACCESS_DENIED",
                            "only the nominated successor may accept");
                          return;
                      }
                      repos->effectSuccession(
                        org.getValueOfId(),
                        caller.id,
                        [req, cb, org, caller](bool ok) {
                            if (!ok)
                            {
                                respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT",
                                  "succession no longer pending or the swap failed; retry");
                                return;
                            }
                            audit(req, "org_successor_accepted", org.getValueOfSlug());
                            Json::Value json;
                            json["slug"] = org.getValueOfSlug();
                            json["owner_user_id"] = caller.id;
                            json["message"] = "Ownership transferred";
                            (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                        }
                      );
                  }
                );
            });
      });
}

// POST /api/me/organizations/invitations/accept  {token}
// Requires the caller's email to equal the invite email (normalized). Single
// use; expired invites are rejected. Adding the member and marking the
// invite accepted are two statements (no interactive txn in the callback
// API); a duplicate accept is idempotent at the membership UNIQUE constraint
// and reports conflict.
// ---------------------------------------------------------------------------
void OrgMemberService::acceptInvitation(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    auto jsonBody = req->getJsonObject();
    if (!jsonBody)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "accept invite: JSON body required");
        return;
    }
    std::string token = (*jsonBody).get("token", "").asString();
    if (token.empty())
    {
        respondError(req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "accept invite: token is required");
        return;
    }

    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, token](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          const std::string callerEmail = ::fulla::common::utils::normalizeEmail(caller.email);
          if (callerEmail.empty() || callerEmail.find('@') == std::string::npos)
          {
              respondError(req, cb, "VALIDATION_INVALID_INPUT",
                "accept invite: your account has no email address to match the invitation");
              return;
          }
          // Review M7: matching the email is not enough when the account has
          // never verified it — otherwise a leaked accept token can be
          // redeemed by any account that merely CLAIMS the address.
          if (!caller.emailVerified)
          {
              respondError(req, cb, "AUTHZ_ACCESS_DENIED",
                "accept invite: verify your email address before accepting an invitation");
              return;
          }
          try
          {
              Mapper<InviteModel>(db).findOne(
                Criteria(InviteModel::Cols::_token, CompareOperator::EQ, token),
                [req, cb, db, caller, callerEmail](const InviteModel &invite) {
                    // Default-constructed trantor::Date has epoch 0; the only
                    // way accepted_at is "set" is a real timestamp.
                    if (invite.getValueOfAcceptedAt().secondsSinceEpoch() != 0)
                    {
                        respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT",
                          "accept invite: invitation already accepted");
                        return;
                    }
                    if (invite.getValueOfEmail() != callerEmail)
                    {
                        respondError(req, cb, "AUTHZ_ACCESS_DENIED",
                          "accept invite: the invitation was issued to a different email address");
                        return;
                    }
                    const int64_t now = ::trantor::Date::now().secondsSinceEpoch();
                    if (invite.getValueOfExpiresAt().secondsSinceEpoch() < now)
                    {
                        respondError(req, cb, "VALIDATION_INVALID_INPUT", "accept invite: invitation expired");
                        return;
                    }
                          MemberModel member;
                          member.setOrganizationId(invite.getValueOfOrganizationId());
                          member.setUserId(caller.id);
                          member.setRole(invite.getValueOfRole() == "admin" ? "admin" : "member");
                          try
                          {
                              Mapper<MemberModel>(db).insert(
                                member,
                                [req, cb, db, invite](const MemberModel &) {
                                    InviteModel accepted = invite;
                                    accepted.setAcceptedAt(::trantor::Date::now());
                                    try
                                    {
                                        Mapper<InviteModel>(db).update(
                                          accepted,
                                          [req, cb, invite](const std::size_t) {
                                              Json::Value json;
                                              json["organization_id"] = invite.getValueOfOrganizationId();
                                              json["role"] = invite.getValueOfRole();
                                              json["message"] = "Invitation accepted";
                                              (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                                          },
                                          [req, cb](const DrogonDbException &e) {
                                              respondError(req, cb, "DB_QUERY_ERROR",
                                                std::string("accept invite: stamp failed: ") + e.base().what());
                                          }
                                        );
                                    }
                                    catch (...)
                                    {
                                        respondError(req, cb, "DB_QUERY_ERROR", "accept invite: Mapper construction failed");
                                    }
                                },
                          [req, cb](const DrogonDbException &e) {
                              respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT",
                                std::string("accept invite: already a member or DB error: ") + e.base().what());
                          }
                        );
                    }
                    catch (...)
                    {
                        respondError(req, cb, "DB_QUERY_ERROR", "accept invite: Mapper construction failed");
                    }
                },
                [req, cb](const DrogonDbException &e) {
                    LOG_WARN << "accept invite: token lookup failed: " << e.base().what();
                    respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "accept invite: invalid token");
                }
              );
          }
          catch (...)
          {
              respondError(req, cb, "DB_QUERY_ERROR", "accept invite: Mapper construction failed");
          }
      });
}

// ---------------------------------------------------------------------------
// #236 entry half (plan B): member-files-manager-approves org consent
// requests. Rulings B2-B8 of .zcode/plans/issues-batch-2/
// 04-design-B-entry.md. All queries live in OrgConsentRequestRepository
// (arch-guard R4); every hop is the resolveCaller -> findOrgBySlug ->
// getMembership family chain; each failure path responds exactly once.
// ---------------------------------------------------------------------------

namespace
{
using RequestModel = ::drogon_model::fulla_db::OrganizationConsentRequests;
using ::fulla::storage::postgres::LookupStatus;
using ::fulla::storage::postgres::OwnerRowLookup;

Json::Value requestToJson(const RequestModel &row)
{
    Json::Value json;
    json["id"] = static_cast<Json::Int64>(row.getValueOfId());
    json["organization_id"] = row.getValueOfOrganizationId();
    json["client_id"] = row.getValueOfClientId();
    json["requested_by"] = row.getValueOfRequestedBy();
    json["requested_at"] = row.getValueOfRequestedAt().toDbStringLocal();
    json["status"] = row.getValueOfStatus();
    return json;
}

// Path-id parser: non-numeric or non-positive ids are a client error; the
// callers respond 400 before touching storage (request ids are SERIAL).
bool parseRequestId(const std::string &raw, int32_t &out)
{
    if (raw.empty())
        return false;
    for (char c : raw)
        if (c < '0' || c > '9')
            return false;
    try
    {
        std::size_t pos = 0;
        const long long parsed = std::stoll(raw, &pos);
        // The id column is SERIAL (int4): a wider bind param is rejected by
        // the PG driver ("incorrect binary data in bind parameter"), so
        // narrow here behind an explicit range check.
        if (pos != raw.size() || parsed <= 0 ||
            parsed > (std::numeric_limits<int32_t>::max)())
            return false;
        out = static_cast<int32_t>(parsed);
        return true;
    }
    catch (...)
    {
        return false;
    }
}
}  // namespace

void OrgMemberService::fileConsentRequest(
  const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &slug
)
{
    auto jsonBody = req->getJsonObject();
    if (!jsonBody || !(*jsonBody).isMember("client_id") ||
        !(*jsonBody)["client_id"].isString() ||
        (*jsonBody)["client_id"].asString().empty())
    {
        respondError(
          req, cb, "VALIDATION_INVALID_INPUT",
          "file consent request: JSON body with client_id required"
        );
        return;
    }
    const std::string clientId = (*jsonBody)["client_id"].asString();

    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, slug, clientId](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          findOrgBySlug(db, slug, req, cb,
            [req, cb, db, caller, clientId](bool, const OrgModel &org) {
                getMembership(db, org.getValueOfId(), caller.id, req, cb,
                  [req, cb, db, org, caller, clientId](
                    bool isMember, const std::string &) {
                      if (!isMember)
                      {
                          respondError(req, cb, "AUTHZ_ACCESS_DENIED",
                            "not a member of this organization");
                          return;
                      }
                      // B5: one owner-row read answers "client exists"
                      // (unknown / admin-seeded -> the #223 uniform 404)
                      // and "already org-owned" (409).
                      auto repo = std::make_shared<
                        ::fulla::storage::postgres::OrgConsentRequestRepository>(db);
                      ::fulla::storage::postgres::ClientOwnersRepository(db)
                        .findOwnerRow(
                          clientId,
                          [repo, req, cb, db, org, caller, clientId](
                            const OwnerRowLookup &o) {
                              if (o.status != LookupStatus::Found)
                              {
                                  respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND",
                                    "application not found");
                                  return;
                              }
                              if (o.row.getOrgId() != nullptr &&
                                  *o.row.getOrgId() == org.getValueOfId())
                              {
                                  respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT",
                                    "application is already owned by the organization");
                                  return;
                              }
                              ::fulla::storage::postgres::OrgConsentRepository(db)
                                .hasActiveConsentForOrg(
                                  org.getValueOfId(),
                                  clientId,
                                  [repo, req, cb, db, org, caller, clientId](
                                    bool hasActive) {
                                      // fail-open by design: a lookup
                                      // failure resolves false, worst case
                                      // one harmless doomed-successfully
                                      // request.
                                      if (hasActive)
                                      {
                                          respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT",
                                            "organization already holds an active consent for this application");
                                          return;
                                      }
                                      repo->fileRequest(
                                        org.getValueOfId(),
                                        clientId,
                                        caller.id,
                                        [repo, req, cb, db, org, caller, clientId](
                                          std::size_t affected) {
                                            repo->findPending(
                                              org.getValueOfId(),
                                              clientId,
                                              caller.id,
                                              [repo, req, cb, org, caller, clientId, affected](
                                                bool foundRow, const RequestModel &row) {
                                                  if (!foundRow)
                                                  {
                                                      // affected==0 AND the
                                                      // pending row is gone:
                                                      // either a real DB
                                                      // failure on the insert
                                                      // or the row vanished
                                                      // concurrently -- the
                                                      // caller retries.
                                                      respondError(req, cb, "DB_QUERY_ERROR",
                                                        "consent request insert failed");
                                                      return;
                                                  }
                                                  if (affected == 0)
                                                  {
                                                      // B3 idempotent re-file.
                                                      Json::Value json = requestToJson(row);
                                                      json["message"] =
                                                        "An identical request is already pending";
                                                      (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                                                      return;
                                                  }
                                                  audit(req, "org_consent_request_filed",
                                                    org.getValueOfSlug() + ":" + clientId);
                                                  Json::Value json = requestToJson(row);
                                                  json["message"] =
                                                    "Consent request filed; awaiting a manager decision";
                                                  (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                                              }
                                            );
                                      }
                                    );
                                  }
                                );
                          }
                        );
                  });
            });
      });
}

void OrgMemberService::listConsentRequests(
  const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &slug
)
{
    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, slug](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          findOrgBySlug(db, slug, req, cb,
            [req, cb, db, caller](bool, const OrgModel &org) {
                getMembership(db, org.getValueOfId(), caller.id, req, cb,
                  [req, cb, db, org](bool isMember, const std::string &memberRole) {
                      // Family-folded manager gate (same text as the
                      // consents endpoints): non-member and non-manager
                      // are indistinguishable here.
                      if (!isManagerRole(memberRole) || !isMember)
                      {
                          respondError(req, cb, "AUTHZ_ACCESS_DENIED", "owner or admin role required");
                          return;
                      }
                      auto repo = std::make_shared<
                        ::fulla::storage::postgres::OrgConsentRequestRepository>(db);
                      repo->listPendingByOrg(
                        org.getValueOfId(),
                        [repo, req, cb, org](const std::vector<RequestModel> &rows) {
                            // Hop 2 (no JOIN rule): requester usernames by
                            // Criteria::In over the distinct requester ids
                            // (the query lives in the repository -- R4).
                            std::vector<int32_t> ids;
                            for (const auto &r : rows)
                                ids.push_back(r.getValueOfRequestedBy());
                            repo->findUsernames(
                              ids,
                              [repo, req, cb, org, rows](
                                const std::map<int32_t, std::string> &names) {
                                  Json::Value json;
                                  json["slug"] = org.getValueOfSlug();
                                  Json::Value arr(Json::arrayValue);
                                  for (const auto &r : rows)
                                  {
                                      Json::Value item = requestToJson(r);
                                      auto it = names.find(r.getValueOfRequestedBy());
                                      item["requester_username"] =
                                        it != names.end() ? it->second : "";
                                      arr.append(item);
                                  }
                                  json["requests"] = arr;
                                  json["total"] = static_cast<int>(arr.size());
                                  (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                              }
                            );
                        }
                      );
                  });
            });
      });
}

void OrgMemberService::approveConsentRequest(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &slug,
  const std::string &requestIdStr
)
{
    int32_t requestId = 0;
    if (!parseRequestId(requestIdStr, requestId))
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT",
          "approve consent request: numeric request id required");
        return;
    }

    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, slug, requestId](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          findOrgBySlug(db, slug, req, cb,
            [req, cb, db, caller, requestId](bool, const OrgModel &org) {
                getMembership(db, org.getValueOfId(), caller.id, req, cb,
                  [req, cb, db, org, caller, requestId](
                    bool isMember, const std::string &memberRole) {
                      if (!isManagerRole(memberRole) || !isMember)
                      {
                          respondError(req, cb, "AUTHZ_ACCESS_DENIED", "owner or admin role required");
                          return;
                      }
                      auto repo = std::make_shared<
                        ::fulla::storage::postgres::OrgConsentRequestRepository>(db);
                      repo->findById(
                        requestId,
                        [repo, req, cb, db, org, caller, requestId](
                          bool foundRow, const RequestModel &row) {
                            // Anti-enumeration: a request of another org
                            // (or a nonexistent id) is the same 404.
                            if (!foundRow ||
                                row.getValueOfOrganizationId() != org.getValueOfId())
                            {
                                respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND",
                                  "consent request not found");
                                return;
                            }
                            if (row.getValueOfStatus() == "rejected")
                            {
                                respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT",
                                  "consent request was rejected");
                                return;
                            }
                            // pending -> normal approval; approved ->
                            // B4 idempotent self-heal (re-run the consent
                            // writes; saveConsent is an upsert).
                            // B2: read the client's registered scope set
                            // at approval time.
                            repo->findClientScopes(
                              row.getValueOfClientId(),
                              [repo, req, cb, db, org, caller, row](
                                const std::vector<std::string> &scopes) {
                                  auto consentRepo = std::make_shared<
                                    ::fulla::storage::postgres::OrgConsentRepository>(db);
                                  if (scopes.empty())
                                  {
                                      // Degenerate (registered, review):
                                      // nothing to authorize -- still mark
                                      // decided so the workflow completes.
                                      repo->decidePending(
                                        row.getValueOfId(), "approved", caller.id, "",
                                        [repo, req, cb, db, org, caller, row, scopes, consentRepo](
                                          std::size_t) {
                                              repo->resolveOtherPending(
                                                org.getValueOfId(), row.getValueOfClientId(),
                                                row.getValueOfId(), caller.id,
                                                [repo, req, cb, org, row, scopes](
                                                  std::size_t) {
                                                      audit(req, "org_consent_request_approved",
                                                        org.getValueOfSlug() + ":" + row.getValueOfClientId());
                                                      Json::Value json;
                                                      json["id"] = static_cast<Json::Int64>(row.getValueOfId());
                                                      json["client_id"] = row.getValueOfClientId();
                                                      json["status"] = "approved";
                                                      json["scopes"] = Json::Value(Json::arrayValue);
                                                      json["message"] =
                                                        "Consent request approved (no registered scopes)";
                                                      (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                                                  }
                                              );
                                        }
                                      );
                                      return;
                                  }
                                  // M2 mirror (SessionController consent
                                  // write): first scope error-gated, the
                                  // rest fire-and-forget; the repo shared
                                  // ptr keeps the heap copy alive.
                                  consentRepo->saveConsent(
                                    org.getValueOfId(),
                                    caller.id,
                                    row.getValueOfClientId(),
                                    scopes[0],
                                    [repo, consentRepo, req, cb, db, org, caller, row, scopes](
                                      bool success) {
                                          if (!success)
                                          {
                                              respondError(req, cb, "INTERNAL_ERROR",
                                                "failed to save organization consent for scope: " + scopes[0]);
                                              return;
                                          }
                                          for (std::size_t i = 1; i < scopes.size(); ++i)
                                          {
                                              consentRepo->saveConsent(
                                                org.getValueOfId(), caller.id,
                                                row.getValueOfClientId(), scopes[i],
                                                [](bool) {});
                                          }
                                          repo->decidePending(
                                            row.getValueOfId(), "approved", caller.id, "",
                                            [repo, consentRepo, req, cb, org, caller, row, scopes](
                                              std::size_t) {
                                                  repo->resolveOtherPending(
                                                    org.getValueOfId(), row.getValueOfClientId(),
                                                    row.getValueOfId(), caller.id,
                                                    [repo, consentRepo, req, cb, org, caller, row, scopes](
                                                      std::size_t) {
                                                          audit(req, "org_consent_request_approved",
                                                            org.getValueOfSlug() + ":" + row.getValueOfClientId());
                                                          Json::Value json;
                                                          json["id"] = static_cast<Json::Int64>(row.getValueOfId());
                                                          json["client_id"] = row.getValueOfClientId();
                                                          json["status"] = "approved";
                                                          Json::Value scopeArr(Json::arrayValue);
                                                          for (const auto &s : scopes)
                                                              scopeArr.append(s);
                                                          json["scopes"] = scopeArr;
                                                          json["message"] =
                                                            "Consent request approved; organization consent rows written";
                                                          (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                                                      }
                                                  );
                                            }
                                          );
                                      }
                                  );
                              }
                            );
                        }
                      );
                  });
            });
      });
}

void OrgMemberService::rejectConsentRequest(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &slug,
  const std::string &requestIdStr
)
{
    int32_t requestId = 0;
    if (!parseRequestId(requestIdStr, requestId))
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT",
          "reject consent request: numeric request id required");
        return;
    }
    std::string reason;
    if (auto jsonBody = req->getJsonObject();
        jsonBody && (*jsonBody).isMember("reason") && (*jsonBody)["reason"].isString())
    {
        reason = (*jsonBody)["reason"].asString();
    }

    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, slug, requestId, reason](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          findOrgBySlug(db, slug, req, cb,
            [req, cb, db, caller, requestId, reason](bool, const OrgModel &org) {
                getMembership(db, org.getValueOfId(), caller.id, req, cb,
                  [req, cb, db, org, caller, requestId, reason](
                    bool isMember, const std::string &memberRole) {
                      if (!isManagerRole(memberRole) || !isMember)
                      {
                          respondError(req, cb, "AUTHZ_ACCESS_DENIED", "owner or admin role required");
                          return;
                      }
                      auto repo = std::make_shared<
                        ::fulla::storage::postgres::OrgConsentRequestRepository>(db);
                      repo->findById(
                        requestId,
                        [repo, req, cb, db, org, caller, requestId, reason](
                          bool foundRow, const RequestModel &row) {
                              if (!foundRow ||
                                  row.getValueOfOrganizationId() != org.getValueOfId())
                              {
                                  respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND",
                                    "consent request not found");
                                  return;
                              }
                              if (row.getValueOfStatus() != "pending")
                              {
                                  respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT",
                                    "consent request is already decided");
                                  return;
                              }
                              repo->decidePending(
                                row.getValueOfId(), "rejected", caller.id, reason,
                                [repo, req, cb, org, row](std::size_t flipped) {
                                    if (flipped == 0)
                                    {
                                        // Lost a decide race.
                                        respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT",
                                          "consent request is already decided");
                                        return;
                                    }
                                    audit(req, "org_consent_request_rejected",
                                      org.getValueOfSlug() + ":" + row.getValueOfClientId());
                                    Json::Value json;
                                    json["id"] = static_cast<Json::Int64>(row.getValueOfId());
                                    json["client_id"] = row.getValueOfClientId();
                                    json["status"] = "rejected";
                                    json["message"] =
                                      "Consent request rejected";
                                    (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                                }
                              );
                          }
                      );
                  });
            });
      });
}

void OrgMemberService::withdrawConsentRequest(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &slug,
  const std::string &requestIdStr
)
{
    int32_t requestId = 0;
    if (!parseRequestId(requestIdStr, requestId))
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT",
          "withdraw consent request: numeric request id required");
        return;
    }

    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, slug, requestId](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          findOrgBySlug(db, slug, req, cb,
            [req, cb, db, caller, requestId](bool, const OrgModel &org) {
                getMembership(db, org.getValueOfId(), caller.id, req, cb,
                  [req, cb, db, org, caller, requestId](
                    bool isMember, const std::string &) {
                      if (!isMember)
                      {
                          respondError(req, cb, "AUTHZ_ACCESS_DENIED",
                            "not a member of this organization");
                          return;
                      }
                      auto repo = std::make_shared<
                        ::fulla::storage::postgres::OrgConsentRequestRepository>(db);
                      repo->withdrawOwnPending(
                        requestId,
                        caller.id,
                        [repo, req, cb, org, requestId](std::size_t deleted) {
                            // B7 anti-enumeration: not yours / not
                            // pending / nonexistent are the same 404.
                            if (deleted == 0)
                            {
                                respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND",
                                  "consent request not found");
                                return;
                            }
                            audit(req, "org_consent_request_withdrawn",
                              org.getValueOfSlug());
                            Json::Value json;
                            json["id"] = static_cast<Json::Int64>(requestId);
                            json["status"] = "withdrawn";
                            json["message"] = "Consent request withdrawn";
                            (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                        }
                      );
                  });
            });
      });
}

}  // namespace organization

