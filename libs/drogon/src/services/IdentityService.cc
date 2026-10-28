#include <fulla/drogon/services/IdentityService.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/authz/RolePermissionResolver.h>
#include <fulla/drogon/utils/SubjectGenerator.h>
#include <drogon/drogon.h>

namespace
{
// Wrap an int32 internalUserId into the NEW consent repo's UserRef (F4:
// consent no longer takes a bare int32). Per UserRef.h's documented contract
// this boundary (a storage/service consumer) is permitted to set internalUserId
// directly.
fulla::oauth2::model::UserRef toUserRef(int32_t internalUserId)
{
    fulla::oauth2::model::UserRef u;
    u.internalUserId = internalUserId;
    return u;
}
}  // namespace

namespace fulla::identity
{

IdentityService::IdentityService(Repos repos) : repos_(std::move(repos))
{
}

void IdentityService::getUserRoles(
  const std::string &userId,
  std::function<void(std::vector<std::string>)> &&callback
)
{
    if (!repos_.role)
    {
        callback({});
        return;
    }
    repos_.role->getRoles(userId, std::move(callback));
}

void IdentityService::ensureSubjectMapping(
  const std::string &subject,
  const std::string & /*username*/,
  int32_t internalUserId,
  std::function<void()> &&callback
)
{
    if (!repos_.subjectMapping)
    {
        callback();
        return;
    }

    auto [provider, sub] = fulla::common::utils::SubjectGenerator::parse(subject);

    // Defect 1.9 fix: capture `self` (shared owner) at the OUTERMOST async call
    // and thread the SAME `self` through the nested continuation, so the
    // service stays alive until the in-flight callback completes.
    auto self = shared_from_this();
    repos_.subjectMapping->getInternalUserId(
      sub,
      provider,
      // init-captures: plain capture of structured bindings is a C++20
      // extension (AppleClang -Wc++20-extensions), copies are C++17-clean
      [self, this, sub = sub, provider = provider, internalUserId, callback = std::move(callback)](
        auto existingUserId
      ) {
          if (existingUserId)
          {
              callback();
              return;
          }

          repos_.subjectMapping->createSubjectMapping(
            sub, internalUserId, provider, [callback = std::move(callback)](bool /*success*/) {
                callback();
            }
          );
      }
    );
}

void IdentityService::handleFirstTimeLogin(
  const std::string &subject,
  const std::string & /*providerArg*/,
  std::function<void(int32_t)> &&callback
)
{
    if (!repos_.subjectMapping)
    {
        callback(0);
        return;
    }

    auto [prov, sub] = fulla::common::utils::SubjectGenerator::parse(subject);

    // Create a real user in the database via the subject-mapping repo
    // (createUserForExternalLogin lives on ISubjectMappingRepository, carved
    // from the god interface alongside createSubjectMapping).
    //
    // Defect 1.9 fix: capture `self` (shared owner) at the OUTERMOST async call
    // and thread the SAME `self` through the nested continuation, so the
    // service stays alive until the in-flight callback completes.
    auto self = shared_from_this();
    repos_.subjectMapping->createUserForExternalLogin(
      sub,
      prov,
      // init-captures: see note above on structured bindings vs C++17
      [self, this, sub = sub, prov = prov, callback = std::move(callback)](
        std::optional<int32_t> newUserId
      ) {
          if (!newUserId || *newUserId == 0)
          {
              LOG_ERROR << "Failed to create user for external login: " << prov << ":" << sub;
              callback(0);
              return;
          }
          // Create subject mapping
          repos_.subjectMapping->createSubjectMapping(
            sub,
            *newUserId,
            prov,
            [newUserId = *newUserId, callback = std::move(callback)](bool success) {
                callback(success ? newUserId : 0);
            }
          );
      }
    );
}

void IdentityService::getInternalUserId(
  const std::string &subject,
  std::function<void(std::optional<int32_t>)> &&callback
)
{
    if (!repos_.subjectMapping)
    {
        callback(std::nullopt);
        return;
    }

    auto [provider, sub] = fulla::common::utils::SubjectGenerator::parse(subject);

    repos_.subjectMapping->getInternalUserId(sub, provider, std::move(callback));
}

void IdentityService::hasUserConsent(
  int32_t internalUserId,
  const std::string &clientId,
  const std::string &scope,
  std::function<void(bool)> &&callback
)
{
    if (!repos_.consent)
    {
        callback(false);
        return;
    }
    repos_.consent->hasUserConsent(toUserRef(internalUserId), clientId, scope, std::move(callback));
}

void IdentityService::saveUserConsent(
  int32_t internalUserId,
  const std::string &clientId,
  const std::string &scope,
  std::function<void(bool)> &&callback
)
{
    if (!repos_.consent)
    {
        callback(false);
        return;
    }
    repos_.consent
      ->saveUserConsent(toUserRef(internalUserId), clientId, scope, std::move(callback));
}

void IdentityService::validateUserRolesForScopes(
  const std::string &userId,
  const std::vector<std::string> &scopes,
  std::function<void(bool, std::string)> &&callback
)
{
    if (!repos_.role)
    {
        callback(false, "Storage not initialized");
        return;
    }

    std::vector<std::string> adminScopes;
    for (const auto &scope : scopes)
    {
        if (scopeRequiresAdminRole(scope))
        {
            adminScopes.push_back(scope);
        }
    }

    if (adminScopes.empty())
    {
        callback(true, "");
        return;
    }

    getUserRoles(
      userId,
      [this, callback = std::move(callback), adminScopes](
        std::vector<std::string> userRoles) mutable {
          bool hasAdminRole = false;
          for (const auto &role : userRoles)
          {
              if (role == "admin")
              {
                  hasAdminRole = true;
                  break;
              }
          }
          if (hasAdminRole)
          {
              callback(true, "");
              return;
          }

          // P1-8 (#285): determination is permission-based (chain② joins
          // the three-gate model): an admin-class scope passes when the
          // user's role->permission union covers EVERY admin-class scope
          // requested (classification vs determination decoupled — a
          // helpdesk grant never de-classifies anything). Resolution rides
          // the plugin's shared resolver; no plugin/no resolver keeps the
          // historical literal-admin-role gate.
          auto *plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
          auto resolver = plugin ? plugin->getRolePermissionResolver() : nullptr;
          if (!resolver)
          {
              callback(false, "Admin role required for requested scopes");
              return;
          }
          resolver->resolveForRoles(
            userRoles,
            [callback = std::move(callback), adminScopes](
              std::unordered_set<std::string> permissions) mutable {
                for (const auto &scope : adminScopes)
                {
                    if (permissions.find(scope) == permissions.end())
                    {
                        callback(false, "Insufficient permissions for requested admin-class scopes");
                        return;
                    }
                }
                callback(true, "");
            });
      }
    );
}

bool IdentityService::scopeRequiresAdminRole(const std::string &scope)
{
    // P1-8 (#285) chain② re-point: the data source is now the DB-loaded
    // admin-class set (oauth2_scopes.requires_admin_role, pushed by
    // OAuth2Plugin's startup load via setAdminScopeClass) with the
    // compile-time default below as the pre-load / memory fallback. The
    // signature is unchanged (api-diff: query changed, signature kept).
    static const std::vector<std::string> defaultAdminScopes =
      {"admin", "users:read", "users:write", "clients:read", "clients:write",
       "tokens:read", "tokens:write", "roles:read", "roles:write", "audit:read"};

    {
        std::shared_lock<std::shared_mutex> lock(adminScopeClassMutex());
        const auto &loaded = adminScopeClassSet();
        if (!loaded.empty())
            return loaded.count(scope) > 0;
    }
    // Prefix semantics preserved for the default set (custom deployments
    // that relied on scoped children like "users:read:x" keep working
    // pre-load).
    for (const auto &adminScope : defaultAdminScopes)
    {
        if (scope == adminScope || scope.find(adminScope + ":") == 0)
            return true;
    }
    return false;
}

void IdentityService::setAdminScopeClass(std::unordered_set<std::string> scopes)
{
    std::unique_lock<std::shared_mutex> lock(adminScopeClassMutex());
    adminScopeClassSet() = std::move(scopes);
}

std::unordered_set<std::string> &IdentityService::adminScopeClassSet()
{
    static std::unordered_set<std::string> instance;
    return instance;
}

std::shared_mutex &IdentityService::adminScopeClassMutex()
{
    static std::shared_mutex mutex;
    return mutex;
}

}  // namespace fulla::identity
