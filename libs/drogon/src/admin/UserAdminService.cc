#include <fulla/drogon/admin/UserAdminService.h>
#include <fulla/drogon/validation/RuleSet.h>

#include <fulla/storage/postgres/models/Users.h>
#include <fulla/storage/postgres/models/UserRoles.h>
#include <fulla/storage/postgres/models/Roles.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/drogon/utils/PasswordHasher.h>
#include <fulla/drogon/utils/SuccessionGuard.h>
#include <fulla/drogon/adapters/DrogonAuditSink.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/common/utils/EmailNormalizer.h>

#include <drogon/drogon.h>
#include <trantor/utils/Date.h>

// Wave-2 P1: revoke the Redis-cached user profile/roles on every successful
// user/role write (roles feed userinfo claims AND the admin RBAC gate).
#include "../UserReadCache.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace fulla::drogon::admin
{

namespace
{
void respondError(
  const ::drogon::HttpRequestPtr &req,
  const UserAdminService::ResponseCallback &cb,
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

// Parsed pagination query params (page is 1-based).
struct PaginationParams
{
    int page;
    int perPage;
};

::drogon::orm::DbClientPtr getDbOrRespond(
  const ::drogon::HttpRequestPtr &req,
  const UserAdminService::ResponseCallback &cb
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

// Sentinel epoch (year ~2286) the original disableUser used to mean "locked
// forever". Kept identical so lockedUntil semantics are unchanged.
constexpr int64_t kLockedForeverSentinel = 9999999999;

// Current epoch seconds (locked_until is stored as epoch seconds).
int64_t nowEpochSeconds()
{
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch()
    )
                                  .count());
}

// ASCII-only lowercase fold for the search pattern. unsigned char cast is
// mandatory: std::tolower on a negative char is UB (asserts under MSVC debug).
// Non-ASCII case folding is intentionally not supported (username/email are
// ASCII in practice; matches the JS mock's toLowerCase for ASCII input).
std::string asciiToLower(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// Type-check a JSON member when present; false = present with a wrong type
// (caller responds 400). jsoncpp's asString()/asBool() throw Json::LogicError
// on mismatch — inside an async DB callback that escapes to the Drogon loop
// and aborts the process (issue #53), so every coercion must be gated by
// these checks up front. This also replaces the previous silent-skip-on-
// wrong-type behavior (#59/#60): a malformed request must never look like a
// successful update.
bool jsonMemberHasType(
  const Json::Value &body,
  const char *name,
  bool (*check)(const Json::Value &)
)
{
    return !body.isMember(name) || check(body[name]);
}

bool isStringVal(const Json::Value &v)
{
    return v.isString();
}
bool isBoolVal(const Json::Value &v)
{
    return v.isBool();
}

// Parse page/per_page query params with the same clamping as AuditService.
PaginationParams parsePagination(const ::drogon::HttpRequestPtr &req)
{
    PaginationParams p{1, 50};
    try { p.page = std::stoi(req->getParameter("page")); } catch (...) {}
    try { p.perPage = std::stoi(req->getParameter("per_page")); } catch (...) {}
    if (p.perPage > 100) p.perPage = 100;
    if (p.perPage < 1) p.perPage = 50;
    if (p.page < 1) p.page = 1;
    return p;
}

// Read the validated admin actor's subject from the request attributes
// (persisted by AuthorizationFilter in B1). Falls back to empty string if the
// attribute is absent (e.g. older filter) so audit never throws.
std::string adminActorId(const ::drogon::HttpRequestPtr &req)
{
    try
    {
        return req->getAttributes()->get<std::string>("userId");
    }
    catch (...)
    {
        return "";
    }
}

// Fire-and-forget audit entry. The sink is a no-op in memory storage mode, so
// this is safe to call unconditionally.
void auditFromRequest(
  const ::drogon::HttpRequestPtr &req,
  const std::string &action,
  const std::string &outcome,
  const std::string &targetType,
  const std::string &targetId
)
{
    auto *plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
    if (!plugin)
        return;
    ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
      plugin->getAuditSink(),
      action,
      outcome,
      req,
      adminActorId(req),
      targetType,
      targetId,
      Json::Value{}
    );
}

// Final 200 response for deleteUser (#56): the soft-delete itself succeeded,
// so the status stays 200, but token-revocation outcome is reported honestly
// (tokens_revoked:false + warning + audit "partial" on failure — never a
// silent success).
void finishDeleteResponse(
  const UserAdminService::ResponseCallback &cb,
  const ::drogon::HttpRequestPtr &req,
  int32_t id,
  bool tokensRevoked
)
{
    auditFromRequest(
      req, "user_delete", tokensRevoked ? "success" : "partial", "user", std::to_string(id)
    );
    Json::Value json;
    json["status"] = "success";
    json["message"] = tokensRevoked
                        ? "User deleted successfully"
                        : "User deleted successfully, but token revocation FAILED — revoke manually";
    json["user_id"] = id;
    json["tokens_revoked"] = tokensRevoked;
    if (!tokensRevoked)
    {
        json["warning"] = "Token revocation failed; outstanding tokens may remain valid until expiry";
    }
    (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
}

Json::Value userRowToListJson(const Users &row)
{
    Json::Value user;
    user["id"] = row.getValueOfId();
    user["username"] = row.getValueOfUsername();
    user["email"] = row.getValueOfEmail();
    user["email_verified"] = row.getValueOfEmailVerified();
    user["mfa_enabled"] = row.getValueOfMfaEnabled();
    user["must_change_password"] = row.getValueOfMustChangePassword();
    return user;
}

// Build the final paginated JSON envelope.  userIdToRoleNames is optional
// (empty when no roles were fetched, e.g. for an empty page).
void respondUserListEnvelope(
  const UserAdminService::ResponseCallback &cb,
  const std::vector<Users> &rows,
  const std::unordered_map<int32_t, std::vector<std::string>> &userIdToRoleNames,
  int page,
  int perPage,
  size_t total,
  int totalPages
)
{
    Json::Value json;
    json["status"] = "success";
    json["page"] = page;
    json["per_page"] = perPage;
    json["total"] = static_cast<Json::UInt64>(total);
    json["total_pages"] = totalPages;
    Json::Value users(Json::arrayValue);
    for (const auto &row : rows)
    {
        Json::Value user = userRowToListJson(row);
        Json::Value rolesJson(Json::arrayValue);
        auto it = userIdToRoleNames.find(row.getValueOfId());
        if (it != userIdToRoleNames.end())
        {
            for (const auto &rn : it->second)
            {
                rolesJson.append(rn);
            }
        }
        user["roles"] = rolesJson;
        users.append(user);
    }
    json["users"] = users;
    (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
}

// Given the page rows, fan-out role names (two Mapper queries — no JOIN), then
// respond with the full envelope.  total/totalPages were already computed from
// a prior count() call.
void attachRolesAndRespond(
  const ::drogon::orm::DbClientPtr &db,
  std::vector<Users> rows,
  int page,
  int perPage,
  size_t total,
  int totalPages,
  const UserAdminService::ResponseCallback &cb,
  const ::drogon::HttpRequestPtr &req
)
{
    if (rows.empty())
    {
        respondUserListEnvelope(cb, {}, {}, page, perPage, total, totalPages);
        return;
    }
    std::vector<int32_t> userIds;
    userIds.reserve(rows.size());
    for (const auto &r : rows)
    {
        userIds.push_back(r.getValueOfId());
    }
    try
    {
        Mapper<UserRoles> urMapper(db);
        urMapper.findBy(
          Criteria(UserRoles::Cols::_user_id, CompareOperator::In, userIds),
          [cb, req, db, rows = std::move(rows), page, perPage, total, totalPages](
            const std::vector<UserRoles> &allUserRoles
          ) {
              std::unordered_map<int32_t, std::vector<int32_t>> userIdToRoleIds;
              std::set<int32_t> distinctRoleIds;
              for (const auto &ur : allUserRoles)
              {
                  userIdToRoleIds[ur.getValueOfUserId()].push_back(ur.getValueOfRoleId());
                  distinctRoleIds.insert(ur.getValueOfRoleId());
              }
              if (distinctRoleIds.empty())
              {
                  respondUserListEnvelope(cb, rows, {}, page, perPage, total, totalPages);
                  return;
              }
              std::vector<int32_t> roleIdsVec(distinctRoleIds.begin(), distinctRoleIds.end());
              try
              {
                  Mapper<Roles> rMapper(db);
                  rMapper.findBy(
                    Criteria(Roles::Cols::_id, CompareOperator::In, roleIdsVec),
                    [cb, rows = std::move(rows), userIdToRoleIds = std::move(userIdToRoleIds), page, perPage, total, totalPages](
                      const std::vector<Roles> &allRoles
                    ) {
                        std::unordered_map<int32_t, std::string> roleIdToName;
                        for (const auto &r : allRoles)
                        {
                            roleIdToName[r.getValueOfId()] = r.getValueOfName();
                        }
                        std::unordered_map<int32_t, std::vector<std::string>> userIdToRoleNames;
                        for (const auto &row : rows)
                        {
                            auto it = userIdToRoleIds.find(row.getValueOfId());
                            if (it != userIdToRoleIds.end())
                            {
                                for (int32_t rid : it->second)
                                {
                                    auto rn = roleIdToName.find(rid);
                                    if (rn != roleIdToName.end())
                                    {
                                        userIdToRoleNames[row.getValueOfId()].push_back(rn->second);
                                    }
                                }
                            }
                        }
                        respondUserListEnvelope(cb, rows, userIdToRoleNames, page, perPage, total, totalPages);
                    },
                    [req, cb](const ::drogon::orm::DrogonDbException &e) {
                        respondError(req, cb, "DB_QUERY_ERROR", std::string("Failed to fetch roles: ") + e.base().what());
                    }
                  );
              }
              catch (...)
              {
                  respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct roles Mapper");
              }
          },
          [req, cb](const ::drogon::orm::DrogonDbException &e) {
              respondError(req, cb, "DB_QUERY_ERROR", std::string("Failed to fetch user roles: ") + e.base().what());
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct user-roles Mapper");
    }
}
}  // namespace

void UserAdminService::listUsers(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    auto pagination = parsePagination(req);
    std::string q = req->getParameter("q");
    std::string role = req->getParameter("role");
    std::string locked = req->getParameter("locked");

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    // Build the base filter from search (q) and lock-state (locked) params.
    // locked has no column — it is derived from locked_until vs current time.
    int64_t now = nowEpochSeconds();
    // Always exclude soft-deleted users.
    Criteria baseFilter = Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull);
    if (!q.empty())
    {
        // Escape LIKE wildcards, then case-insensitive prefix match by
        // lower() on both sides (issue #58: plain LIKE is case-sensitive in
        // PostgreSQL and diverged from the e2e mock). Drogon's Criteria
        // interpolates the colName verbatim, so a lower() expression stays
        // within the Criteria API (no raw-SQL exemption). ASCII-only folding
        // (see asciiToLower); note lower(col) cannot use the plain username
        // index — acceptable at admin-list scale.
        std::string escaped;
        for (char c : q)
        {
            if (c == '%' || c == '_') escaped += '\\';
            escaped += c;
        }
        std::string pattern = asciiToLower(escaped) + "%";
        baseFilter = baseFilter &&
                     (Criteria("lower(username)", CompareOperator::Like, pattern) ||
                      Criteria("lower(email)", CompareOperator::Like, pattern));
    }
    if (locked == "true")
    {
        baseFilter = baseFilter && Criteria(Users::Cols::_locked_until, CompareOperator::GT, now);
    }
    else if (locked == "false")
    {
        baseFilter = baseFilter && Criteria(Users::Cols::_locked_until, CompareOperator::LT, now);
    }

    // Count + paginate + fan-out roles, parameterised by the final Criteria.
    auto fetchPage = [cb, req, db, pagination](Criteria filter) {
        try
        {
            Mapper<Users> countMapper(db);
            countMapper.count(
              filter,
              [cb, req, db, pagination, filter](const size_t total) {
                  int totalPages = total > 0
                                     ? (static_cast<int>(total) + pagination.perPage - 1) / pagination.perPage
                                     : 0;
                  try
                  {
                      Mapper<Users> pageMapper(db);
                      pageMapper.paginate(pagination.page, pagination.perPage)
                        .orderBy(Users::Cols::_id, SortOrder::ASC)
                        .findBy(
                          filter,
                          [cb, req, db, pagination, total, totalPages](const std::vector<Users> &rows) {
                              attachRolesAndRespond(
                                db, rows, pagination.page, pagination.perPage, total, totalPages, cb, req
                              );
                          },
                          [req, cb](const ::drogon::orm::DrogonDbException &e) {
                              respondError(
                                req, cb, "DB_QUERY_ERROR", std::string("Failed to fetch users: ") + e.base().what()
                              );
                          }
                        );
                  }
                  catch (...)
                  {
                      respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct page Mapper");
                  }
              },
              [req, cb](const ::drogon::orm::DrogonDbException &e) {
                  respondError(
                    req, cb, "DB_QUERY_ERROR", std::string("Failed to count users: ") + e.base().what()
                  );
              }
            );
        }
        catch (...)
        {
            respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct count Mapper");
        }
    };

    if (role.empty())
    {
        fetchPage(baseFilter);
    }
    else
    {
        // JOIN-forbidden: resolve role name -> roleIds -> userIds first, then
        // intersect with the base filter via Criteria(id, In, userIds).
        try
        {
            Mapper<Roles> roleMapper(db);
            roleMapper.findBy(
              Criteria(Roles::Cols::_name, CompareOperator::EQ, role),
              [cb, req, db, baseFilter, fetchPage, pagination](const std::vector<Roles> &roles) {
                  if (roles.empty())
                  {
                      // Role does not exist → empty result.
                      respondUserListEnvelope(cb, {}, {}, pagination.page, pagination.perPage, 0, 0);
                      return;
                  }
                  std::vector<int32_t> roleIds;
                  roleIds.reserve(roles.size());
                  for (const auto &r : roles)
                  {
                      roleIds.push_back(r.getValueOfId());
                  }
                  try
                  {
                      Mapper<UserRoles> urMapper(db);
                      urMapper.findBy(
                        Criteria(UserRoles::Cols::_role_id, CompareOperator::In, roleIds),
                        [cb, req, db, baseFilter, fetchPage, pagination](
                          const std::vector<UserRoles> &userRoles
                        ) {
                            if (userRoles.empty())
                            {
                                respondUserListEnvelope(cb, {}, {}, pagination.page, pagination.perPage, 0, 0);
                                return;
                            }
                            std::set<int32_t> userIdSet;
                            for (const auto &ur : userRoles)
                            {
                                userIdSet.insert(ur.getValueOfUserId());
                            }
                            std::vector<int32_t> userIds(userIdSet.begin(), userIdSet.end());
                            Criteria finalFilter =
                              baseFilter && Criteria(Users::Cols::_id, CompareOperator::In, userIds);
                            fetchPage(finalFilter);
                        },
                        [req, cb](const ::drogon::orm::DrogonDbException &e) {
                            respondError(
                              req, cb, "DB_QUERY_ERROR",
                              std::string("Failed to fetch user-roles for role filter: ") + e.base().what()
                            );
                        }
                      );
                  }
                  catch (...)
                  {
                      respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct user-roles Mapper");
                  }
              },
              [req, cb](const ::drogon::orm::DrogonDbException &e) {
                  respondError(
                    req, cb, "DB_QUERY_ERROR",
                    std::string("Failed to fetch role for filter: ") + e.base().what()
                  );
              }
            );
        }
        catch (...)
        {
            respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct role Mapper");
        }
    }
}

// Fetch the role NAMES assigned to a user (used by getUser + getUserRoles).
// Two Mapper queries (no JOIN): user_roles by user_id -> role_ids, then roles
// by id IN (...). Returned in role.name ascending order to match the original
// getUserRoles ORDER BY r.name.
void fetchUserRoleNames(
  const ::drogon::orm::DbClientPtr &db,
  int32_t userId,
  std::function<void(std::vector<std::pair<int32_t, std::string>>)> &&onDone,
  const ::drogon::HttpRequestPtr &req,
  const UserAdminService::ResponseCallback &cb,
  const char *errCtx
)
{
    // Every Mapper construction gets its own try/catch (db-operations rule):
    // the outer guard cannot reach constructions inside async callbacks.
    try
    {
        Mapper<UserRoles> urMapper(db);
        urMapper.findBy(
          Criteria(UserRoles::Cols::_user_id, CompareOperator::EQ, userId),
          [db, onDone = std::move(onDone), req, cb, errCtx](const std::vector<UserRoles> &userRoles) {
              if (userRoles.empty())
              {
                  onDone({});
                  return;
              }
              std::vector<int32_t> roleIds;
              roleIds.reserve(userRoles.size());
              for (const auto &ur : userRoles)
              {
                  roleIds.push_back(ur.getValueOfRoleId());
              }
              try
              {
                  Mapper<Roles> rMapper(db);
                  rMapper.findBy(
                    Criteria(Roles::Cols::_id, CompareOperator::In, roleIds),
                    [onDone = std::move(onDone)](const std::vector<Roles> &roles) {
                        std::vector<std::pair<int32_t, std::string>> out;
                        out.reserve(roles.size());
                        for (const auto &r : roles)
                        {
                            out.emplace_back(r.getValueOfId(), r.getValueOfName());
                        }
                        std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
                            return a.second < b.second;
                        });
                        onDone(std::move(out));
                    },
                    [req, cb, errCtx](const ::drogon::orm::DrogonDbException &e) {
                        respondError(
                          req, cb, "DB_QUERY_ERROR", std::string(errCtx) + ": " + e.base().what()
                        );
                    }
                  );
              }
              catch (...)
              {
                  respondError(req, cb, "DB_QUERY_ERROR", std::string(errCtx) + ": roles Mapper failed");
              }
          },
          [req, cb, errCtx](const ::drogon::orm::DrogonDbException &e) {
              respondError(req, cb, "DB_QUERY_ERROR", std::string(errCtx) + ": " + e.base().what());
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", std::string(errCtx) + ": user-roles Mapper failed");
    }
}

void UserAdminService::createUser(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    auto jsonBody = req->getJsonObject();
    if (!jsonBody)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "Invalid JSON body");
        return;
    }
    // users.org_id write surface removed (v1.5.0 org-anchor convergence, design
    // 1.2/V7): the column stays until v2.0 but the admin API no longer accepts
    // it - the sole org truth is organization_members + client_owners. Reject
    // the key on presence (any type) so a caller never believes a deprecated
    // write landed; same "never a silent skip that answers 200" rule as #53.
    if (jsonBody->isMember("org_id"))
    {
        respondError(
          req,
          cb,
          "VALIDATION_INVALID_INPUT",
          "org_id is no longer accepted; organization membership is managed "
          "via the organization APIs"
        );
        return;
    }
    // Up-front type validation (#53): jsoncpp coercions (asString/asBool)
    // throw Json::LogicError on type mismatch and would escape to the event
    // loop; malformed input is a 400, never a crash or a silent default.
    if (!jsonMemberHasType(*jsonBody, "username", isStringVal) ||
        !jsonMemberHasType(*jsonBody, "password", isStringVal) ||
        !jsonMemberHasType(*jsonBody, "email", isStringVal) ||
        !jsonMemberHasType(*jsonBody, "email_verified", isBoolVal) ||
        !jsonMemberHasType(*jsonBody, "mfa_enabled", isBoolVal) ||
        !jsonMemberHasType(*jsonBody, "must_change_password", isBoolVal))
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "One or more fields have an invalid type");
        return;
    }
    if (jsonBody->isMember("roles") && !(*jsonBody)["roles"].isArray())
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "roles must be an array of strings");
        return;
    }
    std::string username = jsonBody->get("username", "").asString();
    std::string password = jsonBody->get("password", "").asString();
    std::string email = jsonBody->get("email", "").asString();
    bool emailVerified = jsonBody->get("email_verified", false).asBool();
    bool mfaEnabled = jsonBody->get("mfa_enabled", false).asBool();
    // #145: opt-in forced password change for admin-created users (the admin
    // knows the initial password, so it can be flagged for replacement at
    // first login). Default false: existing API consumers keep their behavior.
    bool mustChangePassword = jsonBody->get("must_change_password", false).asBool();

    if (username.empty())
    {
        respondError(req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "username is required");
        return;
    }
    if (password.empty())
    {
        respondError(req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "password is required");
        return;
    }
    if (password.length() < fulla::drogon::validation::RuleSet::passwordMinLength())
    {
        respondError(
          req,
          cb,
          "VALIDATION_PASSWORD_TOO_SHORT",
          "createUser: password must be at least " +
            std::to_string(fulla::drogon::validation::RuleSet::passwordMinLength()) + " characters"
        );
        return;
    }

    // Hash password with PBKDF2-SHA256 (salt embedded in the hash string, so
    // the Users.salt column is set to "" — same convention as AuthService).
    std::string passwordHash;
    try
    {
        passwordHash = ::fulla::common::utils::PasswordHasher::hash(password);
    }
    catch (const std::exception &e)
    {
        respondError(req, cb, "INTERNAL_ERROR", std::string("Password hashing failed: ") + e.what());
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    Users row;
    row.setUsername(username);
    row.setPasswordHash(passwordHash);
    row.setSalt("");
    if (!email.empty())
    {
        row.setEmail(::fulla::common::utils::normalizeEmail(email));
    }
    row.setEmailVerified(emailVerified);
    row.setMfaEnabled(mfaEnabled);
    row.setMustChangePassword(mustChangePassword);

    try
    {
        Mapper<Users> mapper(db);
        mapper.insert(
          row,
          [cb, req, db, username](const Users &inserted) {
              int32_t newId = inserted.getValueOfId();

              // Role assignment bookkeeping (#60 item 1): the user row is
              // created (201 is factual), but a role-assignment failure must
              // be observable — roles_failed lists every requested name that
              // did not land (unresolved names + failed inserts), and a
              // non-empty list adds a warning + LOG_ERROR instead of the old
              // silent "success" with a permission-less user.
              auto assigned = std::make_shared<std::vector<std::string>>();
              auto assignedMu = std::make_shared<std::mutex>();
              auto sendCreated =
                [cb, req, newId, inserted, assigned, assignedMu](const std::vector<std::string> &requested) {
                    auditFromRequest(req, "user_create", "success", "user", std::to_string(newId));
                    std::vector<std::string> assignedCopy;
                    {
                        std::lock_guard<std::mutex> lock(*assignedMu);
                        assignedCopy = *assigned;
                    }
                    std::set<std::string> assignedSet(assignedCopy.begin(), assignedCopy.end());
                    Json::Value assignedJson(Json::arrayValue);
                    Json::Value failedJson(Json::arrayValue);
                    for (const auto &n : assignedCopy)
                        assignedJson.append(n);
                    for (const auto &n : requested)
                        if (!assignedSet.count(n))
                            failedJson.append(n);
                    Json::Value json;
                    json["status"] = "success";
                    json["message"] = "User created successfully";
                    json["user"] = userRowToListJson(inserted);
                    json["user"]["created_at"] = inserted.getValueOfCreatedAt().toDbString();
                    json["roles_assigned"] = assignedJson;
                    json["roles_failed"] = failedJson;
                    if (!failedJson.empty())
                    {
                        LOG_ERROR << "createUser: user " << newId
                                  << " created but some roles could not be assigned";
                        json["warning"] = "User created but some roles could not be assigned";
                    }
                    auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                    resp->setStatusCode(::drogon::k201Created);
                    (*cb)(resp);
                };

              // #143: a user created here must also get a (local, <id>)
              // subject mapping — consent's getInternalUserId resolves users
              // exclusively through that table and 500s without a row (same
              // defect class as the self-registration path). Ensured before
              // the 201 goes out; a failure is logged and tolerated (mirrors
              // the roles_failed semantics above — the startup backfill
              // converges the invariant).
              auto respondCreated = [db, newId, sendCreated](const std::vector<std::string> &requested) {
                  try
                  {
                      db->execSqlAsync(
                        "INSERT INTO oauth2_subject_mappings "
                        "(subject, internal_user_id, provider) "
                        "VALUES ($1, $2, 'local') "
                        "ON CONFLICT (provider, subject) DO NOTHING",
                        [sendCreated, requested](const ::drogon::orm::Result &) {
                            sendCreated(requested);
                        },
                        [sendCreated, requested, newId](const ::drogon::orm::DrogonDbException &e) {
                            LOG_ERROR << "createUser: subject mapping insert failed for user "
                                      << newId << ": " << e.base().what();
                            sendCreated(requested);
                        },
                        std::to_string(newId), newId);
                  }
                  catch (...)
                  {
                      LOG_ERROR << "createUser: subject mapping exec setup failed for user "
                                << newId;
                      sendCreated(requested);
                  }
              };

              // Determine which roles to assign.
              std::vector<std::string> roleNames;
              if (req->getJsonObject() && req->getJsonObject()->isMember("roles") &&
                  (*req->getJsonObject())["roles"].isArray())
              {
                  for (const auto &r : (*req->getJsonObject())["roles"])
                  {
                      if (r.isString())
                          roleNames.push_back(r.asString());
                  }
              }
              if (roleNames.empty())
              {
                  roleNames.push_back("user");  // default role
              }

              // Resolve role names → ids, then insert user_roles rows. The
              // response fires when every insert has settled.
              try
              {
                  Mapper<Roles> rMapper(db);
                  rMapper.findBy(
                    Criteria(Roles::Cols::_name, CompareOperator::In, roleNames),
                    [cb, req, db, newId, roleNames, assigned, assignedMu, respondCreated](
                      const std::vector<Roles> &resolved
                    ) {
                        if (resolved.empty())
                        {
                            respondCreated(roleNames);
                            return;
                        }
                        auto remaining = std::make_shared<std::atomic<int>>(static_cast<int>(resolved.size()));
                        for (const auto &r : resolved)
                        {
                            UserRoles ur;
                            ur.setUserId(newId);
                            ur.setRoleId(r.getValueOfId());
                            std::string roleName = r.getValueOfName();
                            try
                            {
                                Mapper<UserRoles> insMapper(db);
                                insMapper.insert(
                                  ur,
                                  [remaining, roleName, assigned, assignedMu, roleNames,
                                   respondCreated](const UserRoles &) {
                                      {
                                          std::lock_guard<std::mutex> lock(*assignedMu);
                                          assigned->push_back(roleName);
                                      }
                                      if (remaining->fetch_sub(1) == 1)
                                          respondCreated(roleNames);
                                  },
                                  [remaining, roleName, roleNames, newId, respondCreated](
                                    const ::drogon::orm::DrogonDbException &e
                                  ) {
                                      LOG_ERROR << "createUser: role '" << roleName
                                                << "' insert failed for user " << newId << ": "
                                                << e.base().what();
                                      if (remaining->fetch_sub(1) == 1)
                                          respondCreated(roleNames);
                                  }
                                );
                            }
                            catch (...)
                            {
                                LOG_ERROR << "createUser: role insert Mapper construction failed";
                                if (remaining->fetch_sub(1) == 1)
                                    respondCreated(roleNames);
                            }
                        }
                    },
                    [req, cb, newId, roleNames, respondCreated](
                      const ::drogon::orm::DrogonDbException &e
                    ) {
                        LOG_ERROR << "createUser: role lookup failed for user " << newId << ": "
                                  << e.base().what();
                        respondCreated(roleNames);
                    }
                  );
              }
              catch (...)
              {
                  LOG_ERROR << "createUser: role Mapper construction failed";
                  respondError(req, cb, "DB_QUERY_ERROR", "Failed to assign roles");
              }
          },
          [req, cb](const ::drogon::orm::DrogonDbException &e) {
              // Distinguish username vs email UNIQUE violations by constraint
              // name (same pattern as AuthService / PostgresIdentityRepository).
              std::string what = e.base().what();
              if (what.find("users_username_key") != std::string::npos)
              {
                  respondError(req, cb, "VALIDATION_USERNAME_TAKEN", "Username already exists");
              }
              else if (what.find("idx_users_email_unique") != std::string::npos)
              {
                  respondError(req, cb, "VALIDATION_EMAIL_TAKEN", "Email already in use");
              }
              else
              {
                  respondError(req, cb, "DB_QUERY_ERROR", std::string("Failed to create user: ") + what);
              }
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct insert Mapper");
    }
}

void UserAdminService::getUser(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &userId
)
{
    int32_t id = 0;
    try
    {
        id = std::stoi(userId);
    }
    catch (...)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "userId must be an integer");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    // Original: a 3-table JOIN (users LEFT JOIN user_roles LEFT JOIN roles) with
    // json_agg. Split: (1) fetch the user, (2) fetch role names via
    // fetchUserRoleNames (itself two queries). Aggregation in-memory.
    try
    {
        Mapper<Users> mapper(db);
        mapper.findOne(
          Criteria(Users::Cols::_id, CompareOperator::EQ, id) &&
            Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull),
          [cb, req, db, id](const Users &row) {
              Json::Value json;
              json["status"] = "success";
              json["id"] = row.getValueOfId();
              json["username"] = row.getValueOfUsername();
              json["email"] = row.getValueOfEmail();
              json["email_verified"] = row.getValueOfEmailVerified();
              json["mfa_enabled"] = row.getValueOfMfaEnabled();
              json["must_change_password"] = row.getValueOfMustChangePassword();
              json["failed_login_count"] = row.getValueOfFailedLoginCount();
              int64_t lockedUntil = row.getValueOfLockedUntil();
              int64_t now = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                                   std::chrono::system_clock::now().time_since_epoch()
              )
                                                   .count());
              json["locked"] = (lockedUntil > now);
              json["locked_until"] = lockedUntil;
              // Nullable accessor (#59): an org-less user serializes as JSON
              // null, never as getValueOfOrgId()'s default 0 (a nonexistent
              // org).
              const std::shared_ptr<int32_t> &orgId = row.getOrgId();
              json["org_id"] = orgId ? Json::Value(*orgId) : Json::Value(Json::nullValue);
              json["created_at"] = row.getValueOfCreatedAt().toDbString();

              fetchUserRoleNames(
                db,
                id,
                [json, cb](std::vector<std::pair<int32_t, std::string>> roleNames) {
                    Json::Value rolesJson(Json::arrayValue);
                    for (const auto &rn : roleNames)
                    {
                        rolesJson.append(rn.second);
                    }
                    Json::Value resp = json;
                    resp["roles"] = rolesJson;
                    (*cb)(::drogon::HttpResponse::newHttpJsonResponse(resp));
                },
                req,
                cb,
                "Failed to fetch user"
              );
          },
          [req, cb](const ::drogon::orm::DrogonDbException &) {
              respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "User not found");
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct findOne Mapper");
    }
}

void UserAdminService::updateUser(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &userId
)
{
    int32_t id = 0;
    try
    {
        id = std::stoi(userId);
    }
    catch (...)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "userId must be an integer");
        return;
    }

    auto jsonBody = req->getJsonObject();
    if (!jsonBody)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "Invalid JSON body");
        return;
    }
    // org_id write surface removed (v1.5.0, design 1.2/V7 - see createUser);
    // presence of the key is a 400 regardless of value type.
    if (jsonBody->isMember("org_id"))
    {
        respondError(
          req,
          cb,
          "VALIDATION_INVALID_INPUT",
          "org_id is no longer accepted; organization membership is managed "
          "via the organization APIs"
        );
        return;
    }
    // Up-front type validation (#53/#59): wrong-typed fields are a 400 —
    // never a crash (jsoncpp coercion inside the DB callback aborts the
    // process) and never a silent skip that still answers 200 "success".
    if (!jsonMemberHasType(*jsonBody, "email", isStringVal) ||
        !jsonMemberHasType(*jsonBody, "email_verified", isBoolVal) ||
        !jsonMemberHasType(*jsonBody, "username", isStringVal) ||
        !jsonMemberHasType(*jsonBody, "mfa_enabled", isBoolVal) ||
        !jsonMemberHasType(*jsonBody, "must_change_password", isBoolVal) ||
        !jsonMemberHasType(*jsonBody, "locked", isBoolVal))
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "One or more fields have an invalid type");
        return;
    }
    bool hasEmail = jsonBody->isMember("email");
    bool hasEmailVerified = jsonBody->isMember("email_verified");
    bool hasUsername = jsonBody->isMember("username");
    bool hasMfaEnabled = jsonBody->isMember("mfa_enabled");
    bool hasMustChangePassword = jsonBody->isMember("must_change_password");
    bool hasLocked = jsonBody->isMember("locked");
    bool locking = hasLocked && (*jsonBody)["locked"].asBool();
    if (
      !hasEmail && !hasEmailVerified && !hasUsername && !hasMfaEnabled && !hasMustChangePassword &&
      !hasLocked
    )
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "No updatable fields provided");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    try
    {
        Mapper<Users> mapper(db);
        mapper.findOne(
          Criteria(Users::Cols::_id, CompareOperator::EQ, id) &&
        Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull),
          [cb, req, jsonBody, hasEmail, hasEmailVerified, hasUsername, hasMfaEnabled,
           hasMustChangePassword, hasLocked, locking, db, id](
            Users row
          ) {
              // Defense-in-depth (db-operations rule 2): the whole callback
              // body is wrapped so no future coercion/JSON access can escape
              // into the Drogon event loop.
              try
              {
                  // Last-admin guard (#60 item 2): locking the only active
                  // admin is a management-plane lockout. NOTE: this lambda is
                  // intentionally NOT mutable (row is copied, not moved) so it
                  // stays callable from const capture contexts (the guard's
                  // async callback captures it by value).
                  auto proceedWithUpdate = [cb, req, jsonBody, hasEmail, hasEmailVerified,
                                            hasUsername, hasMfaEnabled, hasMustChangePassword,
                                            hasLocked, db,
                                            id, row]() {
                      Users rowLocal = row;
                      if (hasEmail)
                      {
                          rowLocal.setEmail(::fulla::common::utils::normalizeEmail((*jsonBody)["email"].asString()));
                      }
                      if (hasEmailVerified)
                      {
                          rowLocal.setEmailVerified((*jsonBody)["email_verified"].asBool());
                      }
                      if (hasUsername)
                      {
                          rowLocal.setUsername((*jsonBody)["username"].asString());
                      }
                      if (hasMfaEnabled)
                      {
                          rowLocal.setMfaEnabled((*jsonBody)["mfa_enabled"].asBool());
                      }
                      if (hasMustChangePassword)
                      {
                          // #145: admin can set/clear the forced-change flag on
                          // an existing account; enforcement starts at the
                          // user's next login (the session marker is minted
                          // there from this column).
                          rowLocal.setMustChangePassword((*jsonBody)["must_change_password"].asBool());
                      }
                      if (hasLocked)
                      {
                          // locked is derived from locked_until: true → forever sentinel,
                          // false → 0 (unlocked, matching enableUser).
                          rowLocal.setLockedUntil((*jsonBody)["locked"].asBool() ? kLockedForeverSentinel : 0);
                      }
                      try
                      {
                          Mapper<Users> updateMapper(db);
                          updateMapper.update(
                            rowLocal,
                            [cb, req, id, publicSub = rowLocal.getValueOfPublicSub()](const size_t) {
                                // Dual key form (UserReadCache contract): password-
                                // flow tokens cache reads under public_sub, github-
                                // flow tokens under the numeric id — DEL both.
                                fulla::drogon::UserCacheInvalidator::instance().invalidateUser(
                                  std::to_string(id), publicSub);
                                auditFromRequest(req, "user_update", "success", "user", std::to_string(id));
                                Json::Value json;
                                json["status"] = "success";
                                json["message"] = "User updated successfully";
                                (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                            },
                            [req, cb](const ::drogon::orm::DrogonDbException &e) {
                                // Distinguish username vs email UNIQUE violations.
                                std::string what = e.base().what();
                                if (what.find("users_username_key") != std::string::npos)
                                {
                                    respondError(req, cb, "VALIDATION_USERNAME_TAKEN", "Username already exists");
                                }
                                else if (what.find("idx_users_email_unique") != std::string::npos)
                                {
                                    respondError(req, cb, "VALIDATION_EMAIL_TAKEN", "Email already in use");
                                }
                                else
                                {
                                    respondError(
                                      req, cb, "DB_QUERY_ERROR",
                                      std::string("Failed to update user: ") + what
                                    );
                                }
                            }
                          );
                      }
                      catch (...)
                      {
                          respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct update Mapper");
                      }
                  };

                  if (!locking)
                  {
                      proceedWithUpdate();
                      return;
                  }
                  isLastActiveAdmin(
                    db,
                    id,
                    [proceedWithUpdate, cb, req](bool lastAdmin) {
                        if (lastAdmin)
                        {
                            respondError(
                              req, cb, "VALIDATION_RESOURCE_CONFLICT",
                              "Cannot lock the last active admin"
                            );
                            return;
                        }
                        proceedWithUpdate();
                    },
                    [cb, req]() {
                        respondError(req, cb, "DB_QUERY_ERROR", "Failed to evaluate last-admin guard");
                    }
                  );
              }
              catch (...)
              {
                  respondError(req, cb, "INTERNAL_ERROR", "Unexpected error while updating user");
              }
          },
          [req, cb](const ::drogon::orm::DrogonDbException &) {
              respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "User not found");
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct findOne Mapper");
    }
}

void UserAdminService::deleteUser(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &userId
)
{
    int32_t id = 0;
    try
    {
        id = std::stoi(userId);
    }
    catch (...)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "userId must be an integer");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    // Soft-delete: set deleted_at to current timestamp. The findOne already
    // excludes deleted users (deleted_at IS NULL filter), so a 404 is returned
    // for already-deleted users.
    try
    {
        Mapper<Users> mapper(db);
        mapper.findOne(
          Criteria(Users::Cols::_id, CompareOperator::EQ, id) &&
            Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull),
          [cb, req, db, id](Users row) {
              // Self-delete guard: prevent an admin from deleting their own account.
              if (row.getValueOfPublicSub() == adminActorId(req))
              {
                  respondError(req, cb, "VALIDATION_INVALID_INPUT", "Cannot delete your own account");
                  return;
              }
              // Last-admin guard (#60 item 2): deleting the only other active
              // admin is a management-plane lockout.
              isLastActiveAdmin(
                db,
                id,
                [cb, req, db, id, row = std::move(row)](bool lastAdmin) mutable {
                    if (lastAdmin)
                    {
                        respondError(
                          req, cb, "VALIDATION_RESOURCE_CONFLICT",
                          "Cannot delete the last active admin"
                        );
                        return;
                    }
                    // v1.5.0 M3 (R-M3-4): auto-effect any pending succession
                    // nomination the deleting owner holds BEFORE the soft
                    // delete; failure aborts. Orgs without a pending
                    // nomination fall to the admin-takeover state (#228 is
                    // this endpoint's sibling fallback).
                    ::fulla::drogon::utils::effectPendingSuccessionsForUser(
                      db,
                      id,
                      req,
                      [cb, req, db, id, row = std::move(row)](
                        bool successionOk, const std::string &detail) mutable {
                          if (!successionOk)
                          {
                              respondError(
                                req, cb, "DB_QUERY_ERROR",
                                "deleteUser: ownership succession failed for " + detail
                              );
                              return;
                          }
                          row.setDeletedAt(::trantor::Date::now());
                    std::string publicSub = row.getValueOfPublicSub();
                    try
                    {
                        Mapper<Users> updateMapper(db);
                        updateMapper.update(
                          row,
                          [cb, req, id, db, publicSub](const size_t) {
                              fulla::drogon::UserCacheInvalidator::instance().invalidateUser(
                                std::to_string(id), publicSub);
                              // Revoke all outstanding tokens — introspection and
                              // refresh copy the subject from stored token rows and
                              // never query users, so existing tokens would otherwise
                              // stay valid until natural expiry.
                              //
                              // Dual key (#56): password-flow tokens store the
                              // public sub in user_id, GitHub-flow tokens store the
                              // internal id — revoke both or social-issued tokens
                              // survive the delete.
                              //
                              // The two UPDATEs are chained (access → refresh) and
                              // the response waits for both: fire-and-forget meant
                              // a swallowed failure left a soft-deleted user's
                              // refresh token rotatable forever with zero log
                              // output. Soft-delete itself already succeeded, so a
                              // revocation failure is still a 200 — but with
                              // tokens_revoked:false + warning + LOG_ERROR +
                              // audit outcome "partial" so ops can see and fix it.
                              auto revokeRefreshThenRespond =
                                [cb, req, id, db, publicSub](bool accessOk) {
                                    try
                                    {
                                        db->execSqlAsync(
                                          "UPDATE oauth2_refresh_tokens SET revoked = true "
                                          "WHERE user_id = $1 OR user_id = $2",
                                          [cb, req, id, accessOk](const ::drogon::orm::Result &) {
                                              finishDeleteResponse(cb, req, id, accessOk && true);
                                          },
                                          [cb, req, id, accessOk](const ::drogon::orm::DrogonDbException &e) {
                                              LOG_ERROR << "deleteUser: refresh-token revocation failed for user "
                                                        << id << ": " << e.base().what();
                                              finishDeleteResponse(cb, req, id, false);
                                          },
                                          publicSub,
                                          std::to_string(id)
                                        );
                                    }
                                    catch (...)
                                    {
                                        LOG_ERROR << "deleteUser: refresh revocation submission threw";
                                        finishDeleteResponse(cb, req, id, false);
                                    }
                                };
                              try
                              {
                                  db->execSqlAsync(
                                    "UPDATE oauth2_access_tokens SET revoked = true "
                                    "WHERE user_id = $1 OR user_id = $2",
                                    [revokeRefreshThenRespond](const ::drogon::orm::Result &) {
                                        revokeRefreshThenRespond(true);
                                    },
                                    [revokeRefreshThenRespond](const ::drogon::orm::DrogonDbException &e) {
                                        LOG_ERROR << "deleteUser: access-token revocation failed: "
                                                  << e.base().what();
                                        revokeRefreshThenRespond(false);
                                    },
                                    publicSub,
                                    std::to_string(id)
                                  );
                              }
                              catch (...)
                              {
                                  LOG_ERROR << "deleteUser: access revocation submission threw";
                                  revokeRefreshThenRespond(false);
                              }
                          },
                          [req, cb](const ::drogon::orm::DrogonDbException &e) {
                              respondError(
                                req, cb, "DB_QUERY_ERROR",
                                std::string("Failed to delete user: ") + e.base().what()
                              );
                          }
                        );
                    }
                    catch (...)
                    {
                        respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct update Mapper");
                    }
                      }
                      );
                },
                [cb, req]() {
                    respondError(req, cb, "DB_QUERY_ERROR", "Failed to evaluate last-admin guard");
                }
              );
          },
          [req, cb](const ::drogon::orm::DrogonDbException &) {
              respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "User not found");
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct findOne Mapper");
    }
}

void UserAdminService::disableUser(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &userId
)
{
    int32_t id = 0;
    try
    {
        id = std::stoi(userId);
    }
    catch (...)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "userId must be an integer");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    try
    {
        Mapper<Users> mapper(db);
        mapper.findOne(
          Criteria(Users::Cols::_id, CompareOperator::EQ, id) &&
            Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull),
          [cb, req, userId, id, db](Users row) {
              // Last-admin guard (#60 item 2): disabling the only other
              // active admin locks out the management plane.
              isLastActiveAdmin(
                db,
                id,
                [cb, req, userId, db, row = std::move(row)](bool lastAdmin) mutable {
                    if (lastAdmin)
                    {
                        respondError(
                          req, cb, "VALIDATION_RESOURCE_CONFLICT",
                          "Cannot disable the last active admin"
                        );
                        return;
                    }
                    row.setLockedUntil(kLockedForeverSentinel);
                    try
                    {
                        Mapper<Users> updateMapper(db);
                        updateMapper.update(
                          row,
                          [cb, userId, publicSub = row.getValueOfPublicSub()](const size_t) {
                              // Dual key form (UserReadCache contract) — see updateUser.
                              fulla::drogon::UserCacheInvalidator::instance().invalidateUser(
                                userId, publicSub);
                              Json::Value json;
                              json["status"] = "success";
                              json["message"] = "User disabled successfully";
                              json["user_id"] = userId;
                              (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                          },
                          [req, cb](const ::drogon::orm::DrogonDbException &e) {
                              respondError(
                                req,
                                cb,
                                "DB_QUERY_ERROR",
                                std::string("Failed to disable user: ") + e.base().what()
                              );
                          }
                        );
                    }
                    catch (...)
                    {
                        respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct update Mapper");
                    }
                },
                [cb, req]() {
                    respondError(req, cb, "DB_QUERY_ERROR", "Failed to evaluate last-admin guard");
                }
              );
          },
          [req, cb](const ::drogon::orm::DrogonDbException &) {
              respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "User not found");
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct findOne Mapper");
    }
}

void UserAdminService::enableUser(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &userId
)
{
    int32_t id = 0;
    try
    {
        id = std::stoi(userId);
    }
    catch (...)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "userId must be an integer");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    try
    {
        Mapper<Users> mapper(db);
        mapper.findOne(
          Criteria(Users::Cols::_id, CompareOperator::EQ, id) &&
            Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull),
          [cb, req, userId, db](Users row) {
              row.setLockedUntil(0);
              row.setFailedLoginCount(0);
              try
              {
                      Mapper<Users> updateMapper(db);
                      updateMapper.update(
                        row,
                        [cb, userId, publicSub = row.getValueOfPublicSub()](const size_t) {
                            // Dual key form (UserReadCache contract) — see updateUser.
                            fulla::drogon::UserCacheInvalidator::instance().invalidateUser(
                              userId, publicSub);
                            Json::Value json;
                            json["status"] = "success";
                            json["message"] = "User enabled successfully";
                        json["user_id"] = userId;
                        (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                    },
                    [req, cb](const ::drogon::orm::DrogonDbException &e) {
                        respondError(
                          req,
                          cb,
                          "DB_QUERY_ERROR",
                          std::string("Failed to enable user: ") + e.base().what()
                        );
                    }
                  );
              }
              catch (...)
              {
                  respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct update Mapper");
              }
          },
          [req, cb](const ::drogon::orm::DrogonDbException &) {
              respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "User not found");
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct findOne Mapper");
    }
}

void UserAdminService::getUserRoles(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &userId
)
{
    int32_t id = 0;
    try
    {
        id = std::stoi(userId);
    }
    catch (...)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "userId must be an integer");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    // Original: SELECT r.* FROM roles JOIN user_roles ... WHERE user_id.
    // Reuse fetchUserRoleNames but also surface description (the original
    // response includes id/name/description). Fetch roles directly via the
    // two-query split: user_roles -> role_ids -> roles IN (...).
    try
    {
        Mapper<UserRoles> urMapper(db);
        urMapper.findBy(
          Criteria(UserRoles::Cols::_user_id, CompareOperator::EQ, id),
          [cb, req, db](const std::vector<UserRoles> &userRoles) {
              if (userRoles.empty())
              {
                  Json::Value json;
                  json["status"] = "success";
                  json["roles"] = Json::Value(Json::arrayValue);
                  (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                  return;
              }
              std::vector<int32_t> roleIds;
              roleIds.reserve(userRoles.size());
              for (const auto &ur : userRoles)
              {
                  roleIds.push_back(ur.getValueOfRoleId());
              }
              try
              {
                  Mapper<Roles> rMapper(db);
                  rMapper.findBy(
                    Criteria(Roles::Cols::_id, CompareOperator::In, roleIds),
                    [cb](const std::vector<Roles> &roles) {
                        std::vector<Roles> sorted = roles;
                        std::sort(sorted.begin(), sorted.end(), [](const Roles &a, const Roles &b) {
                            return a.getValueOfName() < b.getValueOfName();
                        });
                        Json::Value json;
                        json["status"] = "success";
                        Json::Value rolesJson(Json::arrayValue);
                        for (const auto &r : sorted)
                        {
                            Json::Value role;
                            role["id"] = r.getValueOfId();
                            role["name"] = r.getValueOfName();
                            role["description"] = r.getValueOfDescription();
                            rolesJson.append(role);
                        }
                        json["roles"] = rolesJson;
                        (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                    },
                    [req, cb](const ::drogon::orm::DrogonDbException &e) {
                        respondError(
                          req,
                          cb,
                          "DB_QUERY_ERROR",
                          std::string("Failed to fetch user roles: ") + e.base().what()
                        );
                    }
                  );
              }
              catch (...)
              {
                  respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct roles Mapper");
              }
          },
          [req, cb](const ::drogon::orm::DrogonDbException &e) {
              respondError(
                req, cb, "DB_QUERY_ERROR", std::string("Failed to fetch user roles: ") + e.base().what()
              );
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct user-roles Mapper");
    }
}

void UserAdminService::assignUserRoles(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &userId
)
{
    int32_t id = 0;
    try
    {
        id = std::stoi(userId);
    }
    catch (...)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "userId must be an integer");
        return;
    }

    auto jsonBody = req->getJsonObject();
    if (!jsonBody || !jsonBody->isMember("roles") || !(*jsonBody)["roles"].isArray())
    {
        respondError(
          req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "Request body must contain a 'roles' array"
        );
        return;
    }

    std::vector<std::string> roleNames;
    for (const auto &role : (*jsonBody)["roles"])
    {
        if (role.isString())
        {
            roleNames.push_back(role.asString());
        }
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    // Step 1: fetch the user row (404 semantics like updateUser/disableUser,
    // plus the public_sub needed for dual-form cache invalidation). Step 2:
    // clear existing roles for the user (deleteBy). Step 3: resolve each
    // requested role name to its id, then insert user_roles. The original
    // used `INSERT INTO user_roles SELECT $1, id FROM roles WHERE name=$2`
    // (INSERT...SELECT) -- a documented batch pattern, but Mapper::insert can't
    // express it, so resolve names -> ids first (one findBy(In names)) then
    // insert each assignment.
    //
    // Last-admin guard (#60 item 2): assigning a role set that drops 'admin'
    // from the only active admin is a management-plane lockout — reject 409.
    bool keepsAdmin =
      std::find(roleNames.begin(), roleNames.end(), "admin") != roleNames.end();

    auto proceedWithAssign = [cb, req, db, id, roleNames](const std::string &publicSub) {
        // Invalidate under BOTH subject forms (UserReadCache contract — see
        // updateUser), and only at completion points AFTER a write: firing
        // between the deleteBy and the inserts let a concurrent read refill
        // the EMPTY role set and pin it for the roles TTL (120s).
        auto invalidateCache = [id, publicSub]() {
            fulla::drogon::UserCacheInvalidator::instance().invalidateUser(
              std::to_string(id), publicSub);
        };
        try
        {
            Mapper<UserRoles> urMapper(db);
            urMapper.deleteBy(
              Criteria(UserRoles::Cols::_user_id, CompareOperator::EQ, id),
              [cb, req, db, id, roleNames, invalidateCache](const size_t) {
                  if (roleNames.empty())
                  {
                      // Last write done (nothing to re-insert): invalidate now.
                      invalidateCache();
                      Json::Value json;
                      json["status"] = "success";
                      json["message"] = "User roles updated successfully";
                      json["user_id"] = id;
                      json["roles"] = Json::Value(Json::arrayValue);
                      (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                      return;
                  }

                  // Resolve role names -> role rows (single findBy In).
                  try
                  {
                      Mapper<Roles> rMapper(db);
                      rMapper.findBy(
                        Criteria(Roles::Cols::_name, CompareOperator::In, roleNames),
                        [cb, req, db, id, roleNames, invalidateCache](const std::vector<Roles> &resolved) {
                            if (resolved.empty())
                            {
                                // No requested names matched any role -- nothing inserted;
                                // report success with empty assigned set (preserves
                                // original "skip unknown role" behavior).
                                invalidateCache();
                                Json::Value json;
                                json["status"] = "success";
                                json["message"] = "User roles updated successfully";
                                json["user_id"] = id;
                                json["roles"] = Json::Value(Json::arrayValue);
                                (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                                return;
                            }
                            // NOTE: the response lists roles in COMPLETION order
                            // (concurrent inserts), not request order — treat it
                            // as a set.
                            auto remaining =
                              std::make_shared<std::atomic<int>>(static_cast<int>(resolved.size()));
                            auto assigned = std::make_shared<std::vector<std::string>>();
                            auto mu = std::make_shared<std::mutex>();

                            for (const auto &r : resolved)
                            {
                                UserRoles row;
                                row.setUserId(id);
                                row.setRoleId(r.getValueOfId());
                                try
                                {
                                    Mapper<UserRoles> insMapper(db);
                                    insMapper.insert(
                                      row,
                                      [cb, req, remaining, assigned, mu, name = r.getValueOfName(), id,
                                       invalidateCache](
                                          const UserRoles &
                                      ) {
                                          {
                                              std::lock_guard<std::mutex> lock(*mu);
                                              assigned->push_back(name);
                                          }
                                          if (remaining->fetch_sub(1) == 1)
                                          {
                                              // All inserts done: invalidate now.
                                              invalidateCache();
                                              Json::Value json;
                                              json["status"] = "success";
                                              json["message"] = "User roles updated successfully";
                                              json["user_id"] = id;
                                              Json::Value rolesJson(Json::arrayValue);
                                              {
                                                  std::lock_guard<std::mutex> lock(*mu);
                                                  for (const auto &n : *assigned)
                                                  {
                                                      rolesJson.append(n);
                                                  }
                                              }
                                              json["roles"] = rolesJson;
                                              (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                                          }
                                      },
                                      [cb, req, remaining, invalidateCache](const ::drogon::orm::DrogonDbException &e) {
                                          if (remaining->fetch_sub(1) == 1)
                                          {
                                              // Partial state persisted: invalidate so
                                              // the cache does not pin the pre-change set.
                                              invalidateCache();
                                              respondError(
                                                req,
                                                cb,
                                                "DB_QUERY_ERROR",
                                                std::string("Failed to assign some roles: ") + e.base().what()
                                              );
                                          }
                                      }
                                    );
                                }
                                catch (...)
                                {
                                    LOG_ERROR << "assignUserRoles: insert Mapper construction failed";
                                    if (remaining->fetch_sub(1) == 1)
                                    {
                                        invalidateCache();
                                        respondError(
                                          req, cb, "DB_QUERY_ERROR",
                                          "Failed to construct insert Mapper"
                                        );
                                    }
                                }
                            }
                        },
                        [req, cb, invalidateCache](const ::drogon::orm::DrogonDbException &e) {
                            // Roles were already cleared above — invalidate so the
                            // cache does not pin the pre-change role set.
                            invalidateCache();
                            respondError(
                              req,
                              cb,
                              "DB_QUERY_ERROR",
                              std::string("Failed to resolve role names: ") + e.base().what()
                            );
                        }
                      );
                  }
                  catch (...)
                  {
                      // Same as the findBy error above: the clear already happened.
                      invalidateCache();
                      respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct roles Mapper");
                  }
              },
              [req, cb](const ::drogon::orm::DrogonDbException &e) {
                  respondError(
                    req,
                    cb,
                    "DB_QUERY_ERROR",
                    std::string("Failed to clear existing roles: ") + e.base().what()
                  );
              }
            );
        }
        catch (...)
        {
            respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct user-roles Mapper");
        }
    };

    try
    {
        Mapper<Users> usersMapper(db);
        usersMapper.findOne(
          Criteria(Users::Cols::_id, CompareOperator::EQ, id) &&
            Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull),
          [cb, req, db, id, keepsAdmin, proceedWithAssign](Users row) {
              std::string publicSub = row.getValueOfPublicSub();
              if (keepsAdmin)
              {
                  proceedWithAssign(publicSub);
                  return;
              }
              isLastActiveAdmin(
                db,
                id,
                [proceedWithAssign, publicSub, cb, req](bool lastAdmin) {
                    if (lastAdmin)
                    {
                        respondError(
                          req, cb, "VALIDATION_RESOURCE_CONFLICT",
                          "Cannot remove the admin role from the last active admin"
                        );
                        return;
                    }
                    proceedWithAssign(publicSub);
                },
                [cb, req]() {
                    respondError(req, cb, "DB_QUERY_ERROR", "Failed to evaluate last-admin guard");
                }
              );
          },
          [req, cb](const ::drogon::orm::DrogonDbException &) {
              respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "User not found");
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "Failed to construct users Mapper");
    }
}

// Last-active-admin guard (#60 item 2). True = target IS an active admin and
// NO other active admin exists (deleting/disabling/locking/demoting them
// would lock out the management plane). "Active" = not soft-deleted and not
// locked (NULL locked_until counts as unlocked — the column is nullable but
// DEFAULT 0, see design §6.2 note). Three Mapper queries, JOIN-forbidden.
void isLastActiveAdmin(
  const ::drogon::orm::DbClientPtr &db,
  int32_t targetUserId,
  std::function<void(bool)> &&onDone,
  std::function<void()> &&onError
)
{
    try
    {
        Mapper<Roles> roleMapper(db);
        roleMapper.findBy(
          Criteria(Roles::Cols::_name, CompareOperator::EQ, std::string("admin")),
          [db, targetUserId, onDone = std::make_shared<std::function<void(bool)>>(std::move(onDone)),
           onError = std::make_shared<std::function<void()>>(std::move(onError))](
            const std::vector<Roles> &roles
          ) {
              if (roles.empty())
              {
                  // No admin role exists at all — nothing to protect.
                  (*onDone)(false);
                  return;
              }
              std::vector<int32_t> roleIds;
              roleIds.reserve(roles.size());
              for (const auto &r : roles)
                  roleIds.push_back(r.getValueOfId());
              try
              {
                  Mapper<UserRoles> urMapper(db);
                  urMapper.findBy(
                    Criteria(UserRoles::Cols::_role_id, CompareOperator::In, roleIds),
                    [db, targetUserId, onDone, onError](const std::vector<UserRoles> &userRoles) {
                        std::set<int32_t> adminIds;
                        for (const auto &ur : userRoles)
                            adminIds.insert(ur.getValueOfUserId());
                        if (adminIds.find(targetUserId) == adminIds.end())
                        {
                            // Target is not an admin — no restriction.
                            (*onDone)(false);
                            return;
                        }
                        // Any OTHER admin candidate?
                        std::vector<int32_t> others;
                        for (int32_t uid : adminIds)
                            if (uid != targetUserId)
                                others.push_back(uid);
                        if (others.empty())
                        {
                            (*onDone)(true);
                            return;
                        }
                        try
                        {
                            Mapper<Users> usersMapper(db);
                            usersMapper.count(
                              Criteria(Users::Cols::_id, CompareOperator::In, others) &&
                                Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull) &&
                                (Criteria(Users::Cols::_locked_until, CompareOperator::IsNull) ||
                                 Criteria(Users::Cols::_locked_until, CompareOperator::LE, nowEpochSeconds())),
                              [onDone](const size_t otherActiveAdmins) {
                                  (*onDone)(otherActiveAdmins == 0);
                              },
                              [onError](const ::drogon::orm::DrogonDbException &e) {
                                  LOG_ERROR << "isLastActiveAdmin: admin liveness count failed: "
                                            << e.base().what();
                                  (*onError)();
                              }
                            );
                        }
                        catch (...)
                        {
                            LOG_ERROR << "isLastActiveAdmin: users Mapper construction failed";
                            (*onError)();
                        }
                    },
                    [onError](const ::drogon::orm::DrogonDbException &e) {
                        LOG_ERROR << "isLastActiveAdmin: user-roles query failed: " << e.base().what();
                        (*onError)();
                    }
                  );
              }
              catch (...)
              {
                  LOG_ERROR << "isLastActiveAdmin: user-roles Mapper construction failed";
                  (*onError)();
              }
          },
          [onError](const ::drogon::orm::DrogonDbException &e) {
              LOG_ERROR << "isLastActiveAdmin: role query failed: " << e.base().what();
              onError();
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "isLastActiveAdmin: role Mapper construction failed";
        onError();
    }
}

}  // namespace fulla::drogon::admin
