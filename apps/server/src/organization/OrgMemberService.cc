#include "OrgMemberService.h"
#include "../openplatform/OpenPlatformConfig.h"

#include <fulla/common/utils/EmailNormalizer.h>
#include <fulla/drogon/adapters/DrogonAuditSink.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/drogon/utils/EmailService.h>
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
  const ::drogon::orm::DbClientPtr &db,
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
          // Quota: orgs where the caller is owner.
          try
          {
              Mapper<MemberModel>(db).findBy(
                Criteria(MemberModel::Cols::_user_id, CompareOperator::EQ, caller.id) &&
                  Criteria(MemberModel::Cols::_role, CompareOperator::EQ, "owner"),
                [req, cb, db, cfg, slug, name, logoUri, primaryColor, caller](
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
                        Mapper<OrgModel>(db).insert(
                          row,
                          [req, cb, db, slug, caller](const OrgModel &inserted) {
                              MemberModel member;
                              member.setOrganizationId(inserted.getValueOfId());
                              member.setUserId(caller.id);
                              member.setRole("owner");
                              try
                              {
                                  Mapper<MemberModel>(db).insert(
                                    member,
                                    [req, cb, slug](const MemberModel &) {
                                        audit(req, "organization_created", slug);
                                        Json::Value json;
                                        json["slug"] = slug;
                                        json["role"] = "owner";
                                        json["message"] = "Organization created";
                                        auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                                        resp->setStatusCode(::drogon::k201Created);
                                        (*cb)(resp);
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
                        Json::Value json;
                        json["organizations"] = Json::Value(Json::arrayValue);
                        json["total"] = 0;
                        (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
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
                          [req, cb, roleByOrg](const std::vector<OrgModel> &orgs) {
                              Json::Value json;
                              Json::Value arr(Json::arrayValue);
                              for (const auto &org : orgs)
                              {
                                  Json::Value o;
                                  o["id"] = org.getValueOfId();
                                  o["slug"] = org.getValueOfSlug();
                                  o["name"] = org.getValueOfName();
                                  o["logo_uri"] = org.getValueOfLogoUri();
                                  o["primary_color"] = org.getValueOfPrimaryColor();
                                  o["role"] = roleByOrg.count(org.getValueOfId())
                                                ? roleByOrg.at(org.getValueOfId())
                                                : "member";
                                  arr.append(o);
                              }
                              json["organizations"] = arr;
                              json["total"] = static_cast<int>(arr.size());
                              (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
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
                      // invitations per org (config, default 20). count()
                      // yields the single gate value; the insert continues
                      // from its success callback.
                      const int pendingCap =
                        openplatform::OpenPlatformConfig::load().maxPendingInvitationsPerOrg;
                      try
                      {
                          Mapper<InviteModel>(db).count(
                            Criteria(InviteModel::Cols::_organization_id,
                                     CompareOperator::EQ, org.getValueOfId()) &&
                              Criteria(InviteModel::Cols::_accepted_at,
                                       CompareOperator::IsNull),
                            [req, cb, db, org, email, role, caller, pendingCap](
                              const size_t pending) {
                                if (static_cast<int>(pending) >= pendingCap)
                                {
                                    respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT",
                                      "invite: too many pending invitations for this organization");
                                    return;
                                }
                                proceedWithInvitationInsert(req, cb, db, org, email, role, caller);
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
                  });
            });
      });
}

// createInvitation tail: the actual insert, split out so the pending-cap
// count() callback can continue into it (re-opened anonymous namespace to
// match the forward declaration above).
namespace {
void proceedWithInvitationInsert(
  const ::drogon::HttpRequestPtr &req,
  const organization::OrgMemberService::ResponseCallback &cb,
  const ::drogon::orm::DbClientPtr &db,
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
                            [req, cb, org](const InviteModel &inserted) {
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

}  // namespace organization

