#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/drogon.h>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include <regex>

using namespace drogon;

class OAuth2Plugin;

namespace fulla::drogon::filters
{

class AuthorizationFilter : public HttpFilter<AuthorizationFilter>
{
  public:
    AuthorizationFilter();

    // M3 Task 23 (fulla-sdk-refactor, evaluation H4 "controller/filter
    // 去单例化"): explicit dependency injection point, set once at startup
    // (bootstrap::wireFilterPluginDependencies(), via
    // drogon::DrClassMap::getSingleInstance<AuthorizationFilter>() after
    // the plugin has been constructed) instead of doFilter() calling
    // drogon::app().getPlugin<OAuth2Plugin>() on every request. Non-owning
    // (see HealthController::setPlugin()'s identical comment on plugin
    // lifetime). doFilter() falls back to the global lookup if unset, so
    // this is additive, not a behavior-changing requirement.
    void setPlugin(::OAuth2Plugin *plugin)
    {
        plugin_ = plugin;
    }

    void doFilter(
      const HttpRequestPtr &req,
      FilterCallback &&fcb,
      FilterChainCallback &&fccb
    ) override;

  private:
    ::OAuth2Plugin *plugin_ = nullptr;
    ::OAuth2Plugin *resolvePlugin() const;

    // Path regex -> Allowed Roles
    struct RbacRule
    {
        // P1-8: pattern source retained for the load-time specificity sort
        // (a compiled regex cannot recover its source string).
        std::string sourcePattern;
        std::regex pathPattern;
        std::vector<std::string> allowedRoles;
        // P1-8 (#285) gate-③ permission semantics: every listed permission
        // must be covered by the caller's role->permission union. Entries
        // are additive (array form = roles-only, unchanged); an object
        // form {"roles": [...], "permissions": [...]} enables delegated
        // administration (e.g. /api/admin/users.* -> {permissions:
        // [users:read]} listed BEFORE the /api/admin/.* catch-all).
        std::vector<std::string> requiredPermissions;
    };

    std::vector<RbacRule> rules_;
    std::vector<std::regex> publicPaths_;

    // Per-instance, non-static once_flag. MUST NOT be a function-local
    // `static std::once_flag`: a function-local static is shared across ALL
    // instances, but the init body writes this->rules_/this->publicPaths_, so a
    // shared flag would fill only the first instance and leave every other
    // instance with silently-empty rules. A per-instance member flag guarantees
    // each instance's rule-loading body runs exactly once (defect 1.4 fix).
    std::once_flag initFlag_;

    void loadConfig();
    void loadRulesSafely();
    // P1-8: the permission-aware overload is the live path; the
    // roles-only signature is preserved verbatim (api-diff: change the
    // query, keep the signature) and delegates with an empty permission
    // set (the pre-P1-8 semantics).
    bool checkAccess(const std::vector<std::string> &userRoles, const std::string &path);
    bool checkAccess(
      const std::vector<std::string> &userRoles,
      const std::unordered_set<std::string> &userPermissions,
      const std::string &path
    );
};

}  // namespace fulla::drogon::filters
