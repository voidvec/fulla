#include <fulla/drogon/filters/OAuth2AuthFilter.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/common/error/ErrorTypes.h>
#include <fulla/drogon/error/RequestId.h>
#include <fulla/drogon/authz/ResourceScopeRegistry.h>
#include <fulla/drogon/authz/ScopeResolver.h>
#include <fulla/drogon/authz/InsufficientScopeResponder.h>
#include <drogon/drogon.h>

OAuth2Plugin *fulla::drogon::filters::OAuth2AuthFilter::resolvePlugin() const
{
    return plugin_ ? plugin_ : ::drogon::app().getPlugin<OAuth2Plugin>();
}

void fulla::drogon::filters::OAuth2AuthFilter::doFilter(
  const HttpRequestPtr &req,
  FilterCallback &&fcb,
  FilterChainCallback &&fccb
)
{
    try
    {
        auto plugin = resolvePlugin();
        if (!plugin)
        {
            LOG_ERROR << "OAuth2AuthFilter: OAuth2Plugin not found";
            auto error = fulla::common::error::Error::fromCode(
              "INTERNAL_ERROR", fulla::common::error::RequestId::resolve(req)
            );
            error.message = "OAuth2 plugin not available";
            auto resp = fulla::common::error::ErrorResponder::buildResponse(req, error);
            fcb(resp);
            return;
        }

        if (req->method() == Options)
        {
            fccb();
            return;
        }

        auto authHeader = req->getHeader("Authorization");
        if (authHeader.empty() || authHeader.substr(0, 7) != "Bearer ")
        {
            LOG_WARN << "OAuth2AuthFilter: Missing or invalid Authorization header";
            auto error = fulla::common::error::Error::fromCode(
              "AUTH_TOKEN_INVALID", fulla::common::error::RequestId::resolve(req)
            );
            error.message = "Missing or invalid Authorization header";
            auto resp = fulla::common::error::ErrorResponder::buildResponse(req, error);
            fcb(resp);
            return;
        }

        std::string token = authHeader.substr(7);

        // Async Token Validation
        plugin->validateAccessToken(
          token,
          [req,
           fcb = std::move(fcb),
           fccb = std::move(fccb)](std::shared_ptr<OAuth2Plugin::AccessToken> tokenInfo) {
              if (!tokenInfo)
              {
                  LOG_WARN << "OAuth2AuthFilter: Token validation failed";
                  auto error = fulla::common::error::Error::fromCode(
                    "AUTH_TOKEN_INVALID", fulla::common::error::RequestId::resolve(req)
                  );
                  error.message = "Invalid or expired token";
                  auto resp = fulla::common::error::ErrorResponder::buildResponse(req, error);
                  // F-006 (RFC 6750 §3): the request carried Bearer
                  // credentials that failed validation, so the 401 MUST carry
                  // a WWW-Authenticate challenge with error="invalid_token".
                  // (The no-credentials branch above intentionally sends none.)
                  resp->addHeader(
                    "WWW-Authenticate",
                    "Bearer realm=\"fulla\", error=\"invalid_token\", "
                    "error_description=\"Invalid or expired token\""
                  );
                  fcb(resp);
                  return;
              }

              // Success: token validated. Persist the principal for the
              // downstream handler.
              (*req->getAttributes())["userId"] = tokenInfo->userId;
              (*req->getAttributes())["scope"] = tokenInfo->scope;
              (*req->getAttributes())["clientId"] = tokenInfo->clientId;
              // v1.5.0 M1 (review nit 6): expose the token's org binding so
              // org-aware handlers (userinfo's org_ctx) skip a second
              // introspection round-trip. Stringified for attribute-shape
              // consistency. (Mirrored in AuthorizationFilter.)
              if (tokenInfo->orgId.has_value())
              {
                  (*req->getAttributes())["orgId"] = std::to_string(*tokenInfo->orgId);
              }

              // #43 resource-scope authorization (RFC 6750 §3.1): consult the
              // central ResourceScopeRegistry for this route's scope
              // requirement (replaces the former hardcoded
              // requiredScopeForPath). Order is deliberate: prove the bearer
              // is valid FIRST (401 invalid_token above), THEN prove it is
              // scoped for the resource (403 insufficient_scope here).
              if (auto *reqmt = fulla::drogon::authz::ResourceScopeRegistry::lookup(
                    req->path(), req->method()
                  ))
              {
                  if (!fulla::drogon::authz::satisfies(tokenInfo->scope, *reqmt))
                  {
                      LOG_WARN << "OAuth2AuthFilter: insufficient_scope for path "
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
                      fcb(fulla::drogon::authz::respondInsufficientScope(req, *reqmt));
                      return;
                  }
              }

              fccb();
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "OAuth2AuthFilter: Unhandled exception: " << e.what();
        auto error = fulla::common::error::Error::fromCode(
          "INTERNAL_ERROR", fulla::common::error::RequestId::resolve(req)
        );
        error.message = "Internal filter error";
        auto resp = fulla::common::error::ErrorResponder::buildResponse(req, error);
        fcb(resp);
    }
    catch (...)
    {
        LOG_ERROR << "OAuth2AuthFilter: Unknown exception";
        auto error = fulla::common::error::Error::fromCode(
          "INTERNAL_ERROR", fulla::common::error::RequestId::resolve(req)
        );
        error.message = "Internal filter error";
        auto resp = fulla::common::error::ErrorResponder::buildResponse(req, error);
        fcb(resp);
    }
}
