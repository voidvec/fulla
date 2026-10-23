#include <fulla/drogon/authz/RolePermissionResolver.h>

#include <trantor/utils/Logger.h>

#include <map>
#include <shared_mutex>
#include <unordered_map>

namespace fulla::drogon::authz
{

namespace
{
using namespace ::drogon::orm;
using RoleModel = ::drogon_model::fulla_db::Roles;
using PermissionModel = ::drogon_model::fulla_db::Permissions;
using RolePermissionModel = ::drogon_model::fulla_db::RolePermissions;

// Single-instance coherence cache: role name -> permission union. Writes
// to permissions / role_permissions route through the invalidate* free
// functions below (the admin role-permissions PUT and future catalog
// writes). User-role-set freshness is governed by UserReadCache elsewhere
// (documented limitation).
std::shared_mutex gCacheMutex;
std::unordered_map<std::string, std::unordered_set<std::string>> gRolePermissionsCache;
}  // namespace

void invalidateRolePermissionCacheForRole(const std::string &role)
{
    std::unique_lock<std::shared_mutex> lock(gCacheMutex);
    gRolePermissionsCache.erase(role);
}

void invalidateRolePermissionCacheAll()
{
    std::unique_lock<std::shared_mutex> lock(gCacheMutex);
    gRolePermissionsCache.clear();
}

RolePermissionResolver::RolePermissionResolver(bool postgresStorage)
    : postgresStorage_(postgresStorage)
{
}

RolePermissionResolver::~RolePermissionResolver() = default;

void RolePermissionResolver::resolveForRoles(
  const std::vector<std::string> &roles,
  PermissionsCallback &&cb
)
{
    auto sharedCb = std::make_shared<PermissionsCallback>(std::move(cb));

    // Memory mode: the compile-time default map IS the whole source (no
    // cache indirection — the map is static and mirrors the V043 seed).
    if (!postgresStorage_)
    {
        std::unordered_set<std::string> merged;
        for (const auto &role : roles)
        {
            const auto &perms = defaultPermissionsForRole(role);
            merged.insert(perms.begin(), perms.end());
        }
        (*sharedCb)(merged);
        return;
    }

    // Postgres: serve cached roles, query the misses.
    std::vector<std::string> misses;
    std::unordered_set<std::string> merged;
    {
        std::shared_lock<std::shared_mutex> lock(gCacheMutex);
        for (const auto &role : roles)
        {
            auto it = gRolePermissionsCache.find(role);
            if (it != gRolePermissionsCache.end())
                merged.insert(it->second.begin(), it->second.end());
            else
                misses.push_back(role);
        }
    }

    auto finish = std::make_shared<std::function<void(std::unordered_set<std::string>)>>(
      [sharedCb](std::unordered_set<std::string> out) { (*sharedCb)(std::move(out)); });

    if (misses.empty())
    {
        (*finish)(merged);
        return;
    }

    ::drogon::orm::DbClientPtr db;
    try
    {
        db = ::drogon::app().getDbClient();
    }
    catch (...)
    {
    }
    if (!db)
    {
        // Conservative degradation: unresolved roles grant nothing.
        LOG_WARN << "RolePermissionResolver: no DB client; unresolved roles grant nothing";
        (*finish)(merged);
        return;
    }

    auto cacheMissesAsEmpty = [misses]() {
        std::unique_lock<std::shared_mutex> lock(gCacheMutex);
        for (const auto &role : misses)
            gRolePermissionsCache.try_emplace(role);
    };

    // Hop 1: role names -> role rows.
    try
    {
        Mapper<RoleModel> roleMapper(db);
        roleMapper.findBy(
          Criteria(RoleModel::Cols::_name, CompareOperator::In, misses),
          [db, finish, merged, misses, cacheMissesAsEmpty](
            const std::vector<RoleModel> &roleRows) mutable {
              if (roleRows.empty())
              {
                  cacheMissesAsEmpty();
                  (*finish)(merged);
                  return;
              }
              std::vector<int32_t> roleIds;
              for (const auto &r : roleRows)
                  roleIds.push_back(r.getValueOfId());

              // Hop 2: role ids -> role_permissions rows.
              try
              {
                  Mapper<RolePermissionModel> rpMapper(db);
                  rpMapper.findBy(
                    Criteria(RolePermissionModel::Cols::_role_id, CompareOperator::In, roleIds),
                    [db, finish, merged, roleRows, misses, cacheMissesAsEmpty](
                      const std::vector<RolePermissionModel> &rpRows) mutable {
                        // roleId -> permission ids (in-memory grouping; no
                        // JOIN-in-one-query per db-operations.md).
                        std::map<int32_t, std::vector<int32_t>> roleToPermIds;
                        std::vector<int32_t> permissionIds;
                        for (const auto &rp : rpRows)
                        {
                            roleToPermIds[rp.getValueOfRoleId()].push_back(
                              rp.getValueOfPermissionId());
                            permissionIds.push_back(rp.getValueOfPermissionId());
                        }
                        // Cache roles with no grants immediately.
                        {
                            std::unique_lock<std::shared_mutex> lock(gCacheMutex);
                            for (const auto &r : roleRows)
                            {
                                if (roleToPermIds.find(r.getValueOfId()) == roleToPermIds.end())
                                    gRolePermissionsCache[r.getValueOfName()] = {};
                            }
                        }
                        if (permissionIds.empty())
                        {
                            (*finish)(merged);
                            return;
                        }

                        // Hop 3: permission ids -> names.
                        try
                        {
                            Mapper<PermissionModel> permMapper(db);
                            permMapper.findBy(
                              Criteria(PermissionModel::Cols::_id, CompareOperator::In,
                                       permissionIds),
                              [finish, merged, roleRows, roleToPermIds](
                                const std::vector<PermissionModel> &permRows) mutable {
                                  std::unordered_map<int32_t, std::string> permNames;
                                  for (const auto &p : permRows)
                                      permNames[p.getValueOfId()] = p.getValueOfName();
                                  {
                                      std::unique_lock<std::shared_mutex> lock(gCacheMutex);
                                      for (const auto &r : roleRows)
                                      {
                                          auto it = roleToPermIds.find(r.getValueOfId());
                                          if (it == roleToPermIds.end())
                                              continue;
                                          std::unordered_set<std::string> names;
                                          for (int32_t pid : it->second)
                                          {
                                              auto pit = permNames.find(pid);
                                              if (pit != permNames.end())
                                                  names.insert(pit->second);
                                          }
                                          gRolePermissionsCache[r.getValueOfName()] =
                                            std::move(names);
                                      }
                                  }
                                  for (const auto &[rid, pids] : roleToPermIds)
                                  {
                                      for (int32_t pid : pids)
                                      {
                                          auto pit = permNames.find(pid);
                                          if (pit != permNames.end())
                                              merged.insert(pit->second);
                                      }
                                  }
                                  (*finish)(merged);
                              },
                              [finish, merged](const DrogonDbException &) { (*finish)(merged); });
                        }
                        catch (...)
                        {
                            (*finish)(merged);
                        }
                    },
                    [finish, merged](const DrogonDbException &) { (*finish)(merged); });
              }
              catch (...)
              {
                  (*finish)(merged);
              }
          },
          [finish, merged, cacheMissesAsEmpty](const DrogonDbException &) mutable {
              cacheMissesAsEmpty();
              (*finish)(merged);
          });
    }
    catch (...)
    {
        (*finish)(merged);
    }
}

}  // namespace fulla::drogon::authz
