#include <fulla/drogon/filters/AuthorizationFilter.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/common/error/ErrorTypes.h>
#include <fulla/drogon/error/RequestId.h>
#include <fulla/drogon/authz/ResourceScopeRegistry.h>
#include <fulla/drogon/authz/ScopeResolver.h>
#include <fulla/drogon/authz/InsufficientScopeResponder.h>
#include <drogon/drogon.h>

#include <string>

namespace fulla::drogon::filters
{

using namespace drogon;

AuthorizationFilter::AuthorizationFilter()
{
}

OAuth2Plugin *AuthorizationFilter::resolvePlugin() const
{
    return plugin_ ? plugin_ : app().getPlugin<OAuth2Plugin>();
}

void AuthorizationFilter::loadConfig()
{
    // Defect 1.4 fix: thread-safe, exactly-once initialization. std::call_once
    // provides its own efficient fast path, so the previous non-atomic
    // `if (initialized_) return;` check-then-act fast path is removed entirely
    // (that read raced with the writes inside the init body).
    std::call_once(initFlag_, [this] { loadRulesSafely(); });
}

void AuthorizationFilter::loadRulesSafely()
{
    // Strong exception guarantee: build the complete result in LOCAL vectors,
    // then commit by swapping into the members only after everything succeeded.
    // std::regex(pattern) may throw std::regex_error and push_back may throw; if
    // we mutated rules_/publicPaths_ directly and threw mid-build, std::call_once
    // would (correctly) NOT consume the flag, leaving the members partially
    // filled and causing duplicate appends on the next retry. Building locally
    // and swapping avoids both partial fill and duplicate-append.
    std::vector<RbacRule> localRules;
    std::vector<std::regex> localPublic;

    auto config = app().getCustomConfig();
    if (config.isMember("rbac_rules") && config["rbac_rules"].isObject())
    {
        auto rules = config["rbac_rules"];
        for (auto it = rules.begin(); it != rules.end(); ++it)
        {
            std::string pattern = it.name();
            RbacRule rule;
            rule.pathPattern = std::regex(pattern);

            auto rolesJson = *it;
            if (rolesJson.isArray())
            {
                for (const auto &role : rolesJson)
                {
                    rule.allowedRoles.push_back(role.asString());
                }
            }
            localRules.push_back(rule);
            LOG_DEBUG << "RBAC Rule Loaded: " << pattern << " -> " << rule.allowedRoles.size()
                      << " roles";
        }
    }
    // Load public paths (no auth required)
    if (config.isMember("public_paths") && config["public_paths"].isArray())
    {
        for (const auto &path : config["public_paths"])
        {
            localPublic.push_back(std::regex(path.asString()));
            LOG_DEBUG << "Public path loaded: " << path.asString();
        }
    }

    // Commit atomically (w.r.t. exceptions): only reached when the full build
    // succeeded, so the members are never left in a partially-filled state.
    rules_.swap(localRules);
    publicPaths_.swap(localPublic);
}

void AuthorizationFilter::doFilter(
  const HttpRequestPtr &req,
  FilterCallback &&fcb,
  FilterChainCallback &&fccb
)
{
    loadConfig();

    // Lifetime contract: Drogon Filter instances are process-wide singletons
    // whose lifetime spans the entire process run, same as controllers (see
    // OAuth2StandardController.h). [this] captures in async callbacks below are
    // therefore safe — `this` outlives every async continuation.
    // shared_from_this() is not applicable here because Drogon uses raw pointers
    // (not shared_ptr) to manage Filter instances.

    // 1. Extract Token
    // P1-8 audit fix: Bearer header ONLY. The previous fallback to an
    // access_token query/form parameter (RFC 6750 2.3, forbidden by RFC 9700)
    // leaked tokens into access logs, proxies and Referer headers on every
    // /api/admin/* request. OAuth2AuthFilter already accepts the header only.
    std::string token;
    auto authHeader = req->getHeader("Authorization");
    if (!authHeader.empty() && authHeader.find("Bearer ") == 0)
    {
        token = authHeader.substr(7);
    }

    if (token.empty())
    {
        // Session not found or invalid - use Error Envelope (Req 7.1/7.3)
        LOG_WARN << "Authorization failed: token missing";
        auto error = fulla::common::error::Error::fromCode(
          "AUTH_TOKEN_INVALID", fulla::common::error::RequestId::resolve(req)
        );
        error.message = "Authentication required";
        auto resp = fulla::common::error::ErrorResponder::buildResponse(req, error);
        fcb(resp);  // Return response -> Use fcb
        return;
    }

    // 2. Validate Token
    auto plugin = resolvePlugin();
    if (!plugin)
    {
        LOG_ERROR << "OAuth2Plugin not found!";
        auto error = fulla::common::error::Error::fromCode(
          "INTERNAL_ERROR", fulla::common::error::RequestId::resolve(req)
        );
        error.message = "OAuth2 plugin not available";
        auto resp = fulla::common::error::ErrorResponder::buildResponse(req, error);
        fcb(resp);  // Return response -> Use fcb
        return;
    }

    // Wrap callbacks to avoid move/copy issues in nested lambdas
    // FilterCallback (Arg 2) = Return Response (Stop/Deny)
    // FilterChainCallback (Arg 3) = Continue (Pass)
    auto denyCbPtr = std::make_shared<FilterCallback>(std::move(fcb));
    auto nextCbPtr = std::make_shared<FilterChainCallback>(std::move(fccb));

    plugin->validateAccessToken(
      token,
      [this, req, denyCbPtr, nextCbPtr, plugin](
        std::shared_ptr<OAuth2Plugin::AccessToken> at
      ) mutable {
          if (!at)
          {
              LOG_WARN << "Authorization failed: invalid or expired token";
              auto error = fulla::common::error::Error::fromCode(
                "AUTH_TOKEN_INVALID", fulla::common::error::RequestId::resolve(req)
              );
              error.message = "Invalid or expired token";
              auto resp = fulla::common::error::ErrorResponder::buildResponse(req, error);
              // F-006 (RFC 6750 §3): credentials were presented (Bearer
              // header or access_token parameter) but failed validation --
              // the 401 MUST carry a WWW-Authenticate challenge with
              // error="invalid_token". The missing-credentials branch sends
              // no such header.
              resp->addHeader(
                "WWW-Authenticate",
                "Bearer realm=\"fulla\", error=\"invalid_token\", "
                "error_description=\"Invalid or expired token\""
              );
                  (*denyCbPtr)(resp);
                  return;
              }

              // Persist the validated principal onto the request so downstream
              // admin handlers can identify the actor (e.g. for audit logging
              // on create/update). Mirrors OAuth2AuthFilter's attribute writes;
              // without this, admin services had no way to know who called them.
              (*req->getAttributes())["userId"] = at->userId;
              (*req->getAttributes())["scope"] = at->scope;
              (*req->getAttributes())["clientId"] = at->clientId;
              // v1.5.0 M1 (review nit 6): expose the token's org binding
              // so org-aware handlers (userinfo's org_ctx) do not need a
              // second introspection round-trip for a field this filter
              // already holds. Stringified for attribute-shape consistency.
              if (at->orgId.has_value())
              {
                  (*req->getAttributes())["orgId"] = std::to_string(*at->orgId);
              }

              // 3. Get User Roles
              plugin->getUserRoles(
                at->userId,
            [this, req, denyCbPtr, nextCbPtr, scope = at->scope](
              std::vector<std::string> roles
            ) mutable {
                // #43 resource-scope authorization (RFC 6750 §3.1): consult
                // the central ResourceScopeRegistry for this route's scope
                // requirement (replaces the former hardcoded
                // requiredAdminScopeForPath). This is a second independent
                // gate -- both the scope AND a matching RBAC role must pass.
                // The scope gate is checked first (cheaper, clearer error);
                // the RBAC role gate follows below.
                if (auto *reqmt = fulla::drogon::authz::ResourceScopeRegistry::lookup(
                      req->path(), req->method()
                    ))
                {
                    if (!fulla::drogon::authz::satisfies(scope, *reqmt))
                    {
                        LOG_WARN << "Authorization failed: insufficient scope for path "
                                 << req->path() << " (requires '"
                                 << [&] {
                                        std::string s;
                                        for (size_t i = 0; i < reqmt->scopes.size(); ++i)
                                        {
                                            if (i > 0)
                                                s += " ";
                                            s += reqmt->scopes[i];
                                        }
                                        return s;
                                    }()
                                 << "')";
                        (*denyCbPtr)(fulla::drogon::authz::respondInsufficientScope(req, *reqmt)
                        );
                        return;
                    }
                }

                // 4. Check Access (RBAC role gate -- unchanged)
                if (checkAccess(roles, req->path()))
                {
                    (*nextCbPtr)();  // ALLOW -> Continue
                }
                else
                {
                    LOG_WARN << "Authorization failed: insufficient permissions for path "
                             << req->path();
                    auto error = fulla::common::error::Error::fromCode(
                      "AUTHZ_INSUFFICIENT_PERMISSIONS",
                      fulla::common::error::RequestId::resolve(req)
                    );
                    error.message = "Insufficient permissions";
                    auto resp = fulla::common::error::ErrorResponder::buildResponse(req, error);
                    (*denyCbPtr)(resp);  // DENY -> Return 403
                }
            }
          );
      }
    );
}

bool AuthorizationFilter::checkAccess(
  const std::vector<std::string> &userRoles,
  const std::string &path
)
{
    // Check public paths first (no auth required)
    for (const auto &publicPath : publicPaths_)
    {
        if (std::regex_match(path, publicPath))
            return true;
    }

    // Check RBAC rules
    for (const auto &rule : rules_)
    {
        if (std::regex_match(path, rule.pathPattern))
        {
            // Rule matched - check if user has any of the allowed roles
            for (const auto &allowed : rule.allowedRoles)
            {
                for (const auto &userRole : userRoles)
                {
                    if (userRole == allowed)
                        return true;
                }
            }
            // Rule matched but roles didn't -> DENY
            return false;
        }
    }

    // DEFAULT DENY: no rule matched and not in public_paths
    return false;
}

}  // namespace fulla::drogon::filters
