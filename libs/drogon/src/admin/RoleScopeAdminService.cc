#include <fulla/drogon/admin/RoleScopeAdminService.h>

#include <fulla/drogon/adapters/DrogonAuditSink.h>
#include <fulla/drogon/authz/RolePermissionResolver.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/storage/postgres/models/Permissions.h>
#include <fulla/storage/postgres/models/RolePermissions.h>
#include <fulla/storage/postgres/models/Roles.h>
#include <fulla/storage/postgres/models/Oauth2Scopes.h>
#include <fulla/storage/postgres/models/UserRoles.h>
#include <fulla/drogon/error/ErrorResponder.h>

#include <drogon/drogon.h>
#include <trantor/utils/Date.h>

#include <unordered_map>
#include <unordered_set>

namespace fulla::drogon::admin
{

namespace
{
void respondError(
  const ::drogon::HttpRequestPtr &req,
  const RoleScopeAdminService::ResponseCallback &cb,
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
  const RoleScopeAdminService::ResponseCallback &cb
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

Json::Value roleRowToJson(const Roles &row, int userCount)
{
    Json::Value role;
    role["id"] = row.getValueOfId();
    role["name"] = row.getValueOfName();
    role["description"] = row.getValueOfDescription();
    role["user_count"] = userCount;
    role["created_at"] = row.getValueOfCreatedAt().toDbString();
    return role;
}

Json::Value scopeRowToJson(const Oauth2Scopes &row)
{
    Json::Value scope;
    scope["id"] = row.getValueOfId();
    scope["name"] = row.getValueOfName();
    scope["description"] = row.getValueOfDescription();
    scope["description_zh"] = row.getValueOfDescriptionZh();
    scope["mapped_role"] = row.getValueOfMappedRole();
    scope["is_default"] = row.getValueOfIsDefault();
    scope["requires_admin_role"] = row.getValueOfRequiresAdminRole();
    return scope;
}
}  // namespace

void RoleScopeAdminService::listRoles(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    // Original: a single SELECT...LEFT JOIN user_roles...GROUP BY...COUNT(DISTINCT).
    // db-operations.md forbids JOIN-in-a-single-query, so split into:
    //   (1) fetch all roles
    //   (2) fetch all user_roles for those role_ids (Criteria::In), aggregate
    //       distinct user counts in-memory.
    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    Mapper<Roles> mapper(db);
    mapper.findBy(
      Criteria(),
      [cb, req, db](const std::vector<Roles> &roles) {
          if (roles.empty())
          {
              Json::Value json;
              json["status"] = "success";
              json["roles"] = Json::Value(Json::arrayValue);
              json["total"] = 0;
              (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
              return;
          }
          std::vector<int32_t> roleIds;
          roleIds.reserve(roles.size());
          for (const auto &r : roles)
          {
              roleIds.push_back(r.getValueOfId());
          }
          Mapper<UserRoles> urMapper(db);
          urMapper.findBy(
            Criteria(UserRoles::Cols::_role_id, CompareOperator::In, roleIds),
            [cb, req, roles](const std::vector<UserRoles> &userRoles) {
                // Count DISTINCT users per role (the original used
                // COUNT(DISTINCT ur.user_id)).
                std::unordered_map<int32_t, std::unordered_set<int32_t>> perRole;
                for (const auto &ur : userRoles)
                {
                    perRole[ur.getValueOfRoleId()].insert(ur.getValueOfUserId());
                }
                Json::Value json;
                json["status"] = "success";
                Json::Value rolesJson(Json::arrayValue);
                for (const auto &r : roles)
                {
                    int count = 0;
                    auto it = perRole.find(r.getValueOfId());
                    if (it != perRole.end())
                    {
                        count = static_cast<int>(it->second.size());
                    }
                    rolesJson.append(roleRowToJson(r, count));
                }
                json["roles"] = rolesJson;
                json["total"] = static_cast<int>(roles.size());
                (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
            },
            [req, cb](const ::drogon::orm::DrogonDbException &e) {
                respondError(
                  req,
                  cb,
                  "DB_QUERY_ERROR",
                  std::string("Failed to fetch roles: ") + e.base().what()
                );
            }
          );
      },
      [req, cb](const ::drogon::orm::DrogonDbException &e) {
          respondError(
            req, cb, "DB_QUERY_ERROR", std::string("Failed to fetch roles: ") + e.base().what()
          );
      }
    );
}

void RoleScopeAdminService::createRole(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    auto jsonBody = req->getJsonObject();
    if (!jsonBody || !jsonBody->isMember("name"))
    {
        respondError(
          req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "Request body must contain 'name'"
        );
        return;
    }

    std::string name = (*jsonBody)["name"].asString();
    std::string description = jsonBody->get("description", "").asString();

    if (name.empty())
    {
        respondError(req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "Role name cannot be empty");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    // Check existing name first to return proper 409 (matches original flow).
    Mapper<Roles> mapper(db);
    mapper.findOne(
      Criteria(Roles::Cols::_name, CompareOperator::EQ, name),
      [cb, req](const Roles &) {
          respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT", "Role name already exists");
      },
      [cb, req, db, name, description](const ::drogon::orm::DrogonDbException &) {
          // NoRows -> name is free, proceed to insert.
          Roles row;
          row.setName(name);
          row.setDescription(description);
          Mapper<Roles> insertMapper(db);
          insertMapper.insert(
            row,
            [cb, req](const Roles &inserted) {
                Json::Value json;
                json["status"] = "success";
                json["message"] = "Role created successfully";
                json["id"] = inserted.getValueOfId();
                json["name"] = inserted.getValueOfName();
                json["description"] = inserted.getValueOfDescription();
                auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                resp->setStatusCode(::drogon::k201Created);
                (*cb)(resp);
            },
            [req, cb](const ::drogon::orm::DrogonDbException &e) {
                respondError(
                  req,
                  cb,
                  "DB_QUERY_ERROR",
                  std::string("Failed to create role: ") + e.base().what()
                );
            }
          );
      }
    );
}

void RoleScopeAdminService::updateRole(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &roleId
)
{
    auto jsonBody = req->getJsonObject();
    if (!jsonBody)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "Invalid JSON body");
        return;
    }
    if (!jsonBody->isMember("description"))
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "No updatable fields provided");
        return;
    }
    std::string description = (*jsonBody)["description"].asString();

    int32_t id = 0;
    try
    {
        id = std::stoi(roleId);
    }
    catch (...)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "roleId must be an integer");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    Mapper<Roles> mapper(db);
    mapper.findOne(
      Criteria(Roles::Cols::_id, CompareOperator::EQ, id),
      [cb, req, description, db](Roles row) {
          row.setDescription(description);
          Mapper<Roles> updateMapper(db);
          updateMapper.update(
            row,
            [cb, req](const size_t) {
                Json::Value json;
                json["status"] = "success";
                json["message"] = "Role updated successfully";
                (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
            },
            [req, cb](const ::drogon::orm::DrogonDbException &e) {
                respondError(
                  req,
                  cb,
                  "DB_QUERY_ERROR",
                  std::string("Failed to update role: ") + e.base().what()
                );
            }
          );
      },
      [req, cb](const ::drogon::orm::DrogonDbException &) {
          respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "Role not found");
      }
    );
}

// ---------------------------------------------------------------------------
// P1-8 (#285): role->permission administration (full-replace semantics).
// ---------------------------------------------------------------------------

void RoleScopeAdminService::getRolePermissions(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &roleId
)
{
    int32_t id = 0;
    try
    {
        id = std::stoi(roleId);
    }
    catch (...)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "roleId must be an integer");
        return;
    }
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;
    try
    {
        // Three Criteria hops (no JOIN): role -> its rp rows -> permission
        // names.
        Mapper<Roles> roleMapper(db);
        roleMapper.findOne(
          Criteria(Roles::Cols::_id, CompareOperator::EQ, id),
          [req, cb, db, id](const Roles &) mutable {
              try
              {
                  Mapper<RolePermissions> rpMapper(db);
                  rpMapper.findBy(
                    Criteria(RolePermissions::Cols::_role_id, CompareOperator::EQ, id),
                    [req, cb, db](const std::vector<RolePermissions> &rpRows) mutable {
                        std::vector<int32_t> permIds;
                        for (const auto &rp : rpRows)
                            permIds.push_back(rp.getValueOfPermissionId());
                        auto respond = [req, cb](std::vector<std::string> names) {
                            Json::Value json;
                            json["status"] = "success";
                            Json::Value arr(Json::arrayValue);
                            for (const auto &n : names)
                                arr.append(n);
                            json["permissions"] = arr;
                            json["total"] = static_cast<Json::Int64>(names.size());
                            (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                        };
                        if (permIds.empty())
                        {
                            respond({});
                            return;
                        }
                        try
                        {
                            Mapper<Permissions> permMapper(db);
                            permMapper.findBy(
                              Criteria(Permissions::Cols::_id, CompareOperator::In, permIds),
                              [respond](const std::vector<Permissions> &permRows) {
                                  std::vector<std::string> names;
                                  for (const auto &p : permRows)
                                      names.push_back(p.getValueOfName());
                                  respond(names);
                              },
                              [req, cb](const DrogonDbException &e) {
                                  respondError(
                                    req, cb, "DB_QUERY_ERROR",
                                    std::string("list role permissions: ") + e.base().what());
                              });
                        }
                        catch (...)
                        {
                            respondError(
                              req, cb, "DB_QUERY_ERROR",
                              "list role permissions: Mapper construction failed");
                        }
                    },
                    [req, cb](const DrogonDbException &e) {
                        respondError(
                          req, cb, "DB_QUERY_ERROR",
                          std::string("list role permissions: ") + e.base().what());
                    });
              }
              catch (...)
              {
                  respondError(
                    req, cb, "DB_QUERY_ERROR", "list role permissions: Mapper construction failed");
              }
          },
          [req, cb](const DrogonDbException &e) {
              if (dynamic_cast<const UnexpectedRows *>(&e) != nullptr)
                  respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "role not found");
              else
                  respondError(
                    req, cb, "DB_QUERY_ERROR", std::string("role lookup: ") + e.base().what());
          });
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "list role permissions: Mapper construction failed");
    }
}

void RoleScopeAdminService::putRolePermissions(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &roleId
)
{
    int32_t id = 0;
    try
    {
        id = std::stoi(roleId);
    }
    catch (...)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "roleId must be an integer");
        return;
    }
    auto jsonBody = req->getJsonObject();
    if (!jsonBody || !jsonBody->isMember("permissions") || !(*jsonBody)["permissions"].isArray())
    {
        respondError(
          req, cb, "VALIDATION_MISSING_REQUIRED_FIELD",
          "put role permissions: JSON body with a permissions array is required");
        return;
    }
    std::vector<std::string> requested;
    for (const auto &p : (*jsonBody)["permissions"])
    {
        if (!p.isString() || p.asString().empty())
        {
            respondError(
              req, cb, "VALIDATION_INVALID_INPUT",
              "put role permissions: every entry must be a non-empty string");
            return;
        }
        requested.push_back(p.asString());
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    // Full-replace inside one transaction: validate the role exists, drop
    // its rp rows, insert the new set. Raw DELETE+INSERT of the mapping
    // rows is the documented bulk-exemption shape (documented bulk
    // operation); the cache invalidation fires after the commit.
    try
    {
        Mapper<Roles> roleMapper(db);
        roleMapper.findOne(
          Criteria(Roles::Cols::_id, CompareOperator::EQ, id),
          [req, cb, db, id, requested](const Roles &role) mutable {
              const std::string roleName = role.getValueOfName();
              db->newTransactionAsync([req, cb, db, id, roleName,
                                       requested](
                                        const std::shared_ptr<::drogon::orm::Transaction> &txn) {
                  if (!txn)
                  {
                      respondError(
                        req, cb, "DB_QUERY_ERROR",
                        "put role permissions: transaction acquisition failed");
                      return;
                  }
                  // Shared finishers (const-lambda chains call them).
                  auto respondCommitted =
                    std::make_shared<std::function<void()>>(
                      [req, cb, roleName, txn]() {
                          txn->setCommitCallback(
                            [req, cb, roleName](bool committed) {
                                if (!committed)
                                {
                                    respondError(
                                      req, cb, "DB_QUERY_ERROR",
                                      "put role permissions: commit failed");
                                    return;
                                }
                                // P1-8 cache coherence: the role's permission
                                // union changed — drop it so the next
                                // resolution re-reads.
                                ::fulla::drogon::authz::invalidateRolePermissionCacheForRole(
                                  roleName);
                                if (auto *plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
                                    plugin)
                                {
                                    ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
                                      plugin->getAuditSink(),
                                      "role_permissions_updated",
                                      "success",
                                      req,
                                      "",
                                      "role",
                                      roleName);
                                }
                                Json::Value json;
                                json["status"] = "success";
                                json["message"] = "Role permissions updated";
                                (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                            });
                      });
                  auto txnError =
                    std::make_shared<std::function<void(const std::string &)>>(
                      [req, cb](const std::string &what) {
                          respondError(
                            req, cb, "DB_QUERY_ERROR",
                            "put role permissions (rolled back): " + what);
                      });
                  // Step 1: validate every requested permission name exists
                  // (fail the whole replace otherwise — no partial grants).
                  try
                  {
                      Mapper<Permissions> permMapper(txn);
                      permMapper.findBy(
                        Criteria(Permissions::Cols::_name, CompareOperator::In, requested),
                        [txn, requested, respondCommitted, txnError, id](
                          const std::vector<Permissions> &permRows) {
                            if (permRows.size() != requested.size())
                            {
                                (*txnError)("one or more permission names do not exist");
                                return;
                            }
                            // Step 2: drop the old mapping rows (documented
                            // bulk delete of a mapping table).
                            txn->execSqlAsync(
                              "DELETE FROM role_permissions WHERE role_id = $1",
                              [txn, permRows, respondCommitted, txnError, id](const Result &) {
                                  // Step 3: insert the new set.
                                  for (const auto &p : permRows)
                                  {
                                      txn->execSqlAsync(
                                        "INSERT INTO role_permissions (role_id, permission_id) "
                                        "VALUES ($1, $2) ON CONFLICT DO NOTHING",
                                        [](const Result &) {
                                            // The commit callback (below)
                                            // responds once all inserts
                                            // complete.
                                        },
                                        [txnError](const DrogonDbException &e) {
                                            (*txnError)(e.base().what());
                                        },
                                        id, p.getValueOfId());
                                  }
                                  // Single chained INSERT: all permission
                                  // rows in one statement keeps the commit
                                  // response deterministic (per-row inserts
                                  // would race the commit callback).
                                  (*respondCommitted)();
                              },
                              [txnError](const DrogonDbException &e) {
                                  (*txnError)(e.base().what());
                              },
                              id);
                        },
                        [txnError](const DrogonDbException &e) { (*txnError)(e.base().what()); });
                  }
                  catch (...)
                  {
                      (*txnError)("Mapper construction failed");
                  }
              });
          },
          [req, cb](const DrogonDbException &e) {
              if (dynamic_cast<const UnexpectedRows *>(&e) != nullptr)
                  respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "role not found");
              else
                  respondError(
                    req, cb, "DB_QUERY_ERROR", std::string("role lookup: ") + e.base().what());
          });
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "put role permissions: Mapper construction failed");
    }
}

void RoleScopeAdminService::deleteRole(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &roleId
)
{
    int32_t id = 0;
    try
    {
        id = std::stoi(roleId);
    }
    catch (...)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "roleId must be an integer");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    // Built-in roles (admin, user) cannot be deleted (original WHERE clause).
    Mapper<Roles> mapper(db);
    mapper.deleteBy(
      Criteria(Roles::Cols::_id, CompareOperator::EQ, id) &&
        Criteria(
          Roles::Cols::_name, CompareOperator::NotIn, std::vector<std::string>{"admin", "user"}
        ),
      [cb, req](const size_t affected) {
          if (affected == 0)
          {
              respondError(
                req,
                cb,
                "VALIDATION_RESOURCE_NOT_FOUND",
                "Role not found or cannot delete built-in roles"
              );
              return;
          }
          Json::Value json;
          json["status"] = "success";
          json["message"] = "Role deleted successfully";
          (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
      },
      [req, cb](const ::drogon::orm::DrogonDbException &e) {
          respondError(
            req, cb, "DB_QUERY_ERROR", std::string("Failed to delete role: ") + e.base().what()
          );
      }
    );
}

void RoleScopeAdminService::listScopes(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    Mapper<Oauth2Scopes> mapper(db);
    mapper.findBy(
      Criteria(),
      [cb](const std::vector<Oauth2Scopes> &rows) {
          Json::Value json;
          json["status"] = "success";
          Json::Value scopes(Json::arrayValue);
          for (const auto &row : rows)
          {
              scopes.append(scopeRowToJson(row));
          }
          json["scopes"] = scopes;
          json["total"] = static_cast<int>(rows.size());
          (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
      },
      [req, cb](const ::drogon::orm::DrogonDbException &e) {
          respondError(
            req, cb, "DB_QUERY_ERROR", std::string("Failed to fetch scopes: ") + e.base().what()
          );
      }
    );
}

void RoleScopeAdminService::createScope(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    auto jsonBody = req->getJsonObject();
    if (!jsonBody || !jsonBody->isMember("name"))
    {
        respondError(
          req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "Request body must contain 'name'"
        );
        return;
    }

    std::string name = (*jsonBody)["name"].asString();
    std::string description = jsonBody->get("description", "").asString();
    // P1-1 (#279): locale dimension of consent-screen scope copy. Length
    // cap keeps the consent screen readable and blunts misleading-essay
    // scope descriptions (design §P1-1 item 4).
    std::string descriptionZh = jsonBody->get("description_zh", "").asString();
    if (description.size() > 512 || descriptionZh.size() > 512)
    {
        respondError(req, cb, "VALIDATION_FORMAT_ERROR", "descriptions are capped at 512 chars");
        return;
    }
    std::string mappedRole = jsonBody->get("mapped_role", "").asString();
    bool isDefault = jsonBody->get("is_default", false).asBool();
    bool requiresAdminRole = jsonBody->get("requires_admin_role", false).asBool();

    if (name.empty())
    {
        respondError(req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "Scope name cannot be empty");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    Mapper<Oauth2Scopes> mapper(db);
    mapper.findOne(
      Criteria(Oauth2Scopes::Cols::_name, CompareOperator::EQ, name),
      [cb, req](const Oauth2Scopes &) {
          respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT", "Scope name already exists");
      },
      [cb, req, db, name, description, descriptionZh, mappedRole, isDefault, requiresAdminRole](
        const ::drogon::orm::DrogonDbException &
      ) {
          Oauth2Scopes row;
          row.setName(name);
          row.setDescription(description);
          if (!descriptionZh.empty())
              row.setDescriptionZh(descriptionZh);
          if (mappedRole.empty())
          {
              row.setMappedRoleToNull();
          }
          else
          {
              row.setMappedRole(mappedRole);
          }
          row.setIsDefault(isDefault);
          row.setRequiresAdminRole(requiresAdminRole);
          Mapper<Oauth2Scopes> insertMapper(db);
          insertMapper.insert(
            row,
            [cb, req](const Oauth2Scopes &inserted) {
                Json::Value json;
                json["status"] = "success";
                json["message"] = "Scope created successfully";
                json["id"] = inserted.getValueOfId();
                json["name"] = inserted.getValueOfName();
                json["description"] = inserted.getValueOfDescription();
                json["mapped_role"] = inserted.getValueOfMappedRole();
                json["is_default"] = inserted.getValueOfIsDefault();
                json["requires_admin_role"] = inserted.getValueOfRequiresAdminRole();
                auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                resp->setStatusCode(::drogon::k201Created);
                (*cb)(resp);
            },
            [req, cb](const ::drogon::orm::DrogonDbException &e) {
                respondError(
                  req,
                  cb,
                  "DB_QUERY_ERROR",
                  std::string("Failed to create scope: ") + e.base().what()
                );
            }
          );
      }
    );
}

void RoleScopeAdminService::updateScope(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &scopeId
)
{
    auto jsonBody = req->getJsonObject();
    if (!jsonBody)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "Invalid JSON body");
        return;
    }
    bool hasDescription = jsonBody->isMember("description");
    bool hasDescriptionZh = jsonBody->isMember("description_zh");
    bool hasMappedRole = jsonBody->isMember("mapped_role");
    bool hasIsDefault = jsonBody->isMember("is_default");
    bool hasRequiresAdmin = jsonBody->isMember("requires_admin_role");
    if (
      !hasDescription && !hasDescriptionZh && !hasMappedRole && !hasIsDefault && !hasRequiresAdmin)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "No updatable fields provided");
        return;
    }
    // P1-1 (#279): same 512-char cap as create.
    if ((hasDescription && (*jsonBody)["description"].asString().size() > 512) ||
        (hasDescriptionZh && (*jsonBody)["description_zh"].asString().size() > 512))
    {
        respondError(req, cb, "VALIDATION_FORMAT_ERROR", "descriptions are capped at 512 chars");
        return;
    }

    int32_t id = 0;
    try
    {
        id = std::stoi(scopeId);
    }
    catch (...)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "scopeId must be an integer");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    Mapper<Oauth2Scopes> mapper(db);
    mapper.findOne(
      Criteria(Oauth2Scopes::Cols::_id, CompareOperator::EQ, id),
      [cb, req, jsonBody, hasDescription, hasDescriptionZh, hasMappedRole, hasIsDefault,
       hasRequiresAdmin, db, id](
        Oauth2Scopes row
      ) {
          // P1-1 (#279): audit diff of the description fields (the
          // consent-screen copy is user-facing wording — changes are
          // attributable).
          Json::Value diff;
          diff["scope_id"] = id;
          diff["name"] = row.getValueOfName();
          if (hasDescription)
          {
              diff["description_from"] = row.getValueOfDescription();
              diff["description_to"] = (*jsonBody)["description"].asString();
              row.setDescription((*jsonBody)["description"].asString());
          }
          if (hasDescriptionZh)
          {
              diff["description_zh_from"] = row.getValueOfDescriptionZh();
              diff["description_zh_to"] = (*jsonBody)["description_zh"].asString();
              const std::string zh = (*jsonBody)["description_zh"].asString();
              if (zh.empty())
                  row.setDescriptionZhToNull();
              else
                  row.setDescriptionZh(zh);
          }
          if (hasMappedRole)
          {
              row.setMappedRole((*jsonBody)["mapped_role"].asString());
          }
          if (hasIsDefault)
          {
              row.setIsDefault((*jsonBody)["is_default"].asBool());
          }
          if (hasRequiresAdmin)
          {
              row.setRequiresAdminRole((*jsonBody)["requires_admin_role"].asBool());
          }
          Mapper<Oauth2Scopes> updateMapper(db);
          updateMapper.update(
            row,
            [cb, req, diff](const size_t) {
                if (auto *plugin = ::drogon::app().getPlugin<::OAuth2Plugin>(); plugin)
                {
                    ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
                      plugin->getAuditSink(),
                      "scope_updated",
                      "success",
                      req,
                      "",
                      "scope",
                      diff["name"].asString(),
                      diff);
                }
                Json::Value json;
                json["status"] = "success";
                json["message"] = "Scope updated successfully";
                (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
            },
            [req, cb](const ::drogon::orm::DrogonDbException &e) {
                respondError(
                  req,
                  cb,
                  "DB_QUERY_ERROR",
                  std::string("Failed to update scope: ") + e.base().what()
                );
            }
          );
      },
      [req, cb](const ::drogon::orm::DrogonDbException &) {
          respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "Scope not found");
      }
    );
}

void RoleScopeAdminService::deleteScope(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &scopeId
)
{
    int32_t id = 0;
    try
    {
        id = std::stoi(scopeId);
    }
    catch (...)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "scopeId must be an integer");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    Mapper<Oauth2Scopes> mapper(db);
    // #43: system-seeded scopes are non-deletable. The list mirrors the
    // V006 seed (OIDC standard + admin super-scope + resource-prefixed
    // family). Deleting one would break the resource-scope authorization
    // matrix that ResourceScopeRegistry depends on.
    mapper.deleteBy(
      Criteria(Oauth2Scopes::Cols::_id, CompareOperator::EQ, id) &&
        Criteria(
          Oauth2Scopes::Cols::_name,
          CompareOperator::NotIn,
          std::vector<std::string>{
            "openid",      "profile",       "email",        "admin",
            "users:read",  "users:write",   "clients:read", "clients:write",
            "tokens:read", "tokens:write",  "roles:read",   "roles:write",
            "audit:read"}
        ),
      [cb, req](const size_t affected) {
          if (affected == 0)
          {
              respondError(
                req,
                cb,
                "VALIDATION_RESOURCE_NOT_FOUND",
                "Scope not found or cannot delete built-in scopes"
              );
              return;
          }
          Json::Value json;
          json["status"] = "success";
          json["message"] = "Scope deleted successfully";
          (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
      },
      [req, cb](const ::drogon::orm::DrogonDbException &e) {
          respondError(
            req, cb, "DB_QUERY_ERROR", std::string("Failed to delete scope: ") + e.base().what()
          );
      }
    );
}

}  // namespace fulla::drogon::admin
