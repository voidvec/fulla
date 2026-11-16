#include <fulla/drogon/controllers/UpstreamLoginController.h>

#ifdef WITH_SOCIAL

// src-local shared helper (not part of the installed SDK surface).
#include "SocialAuthorizeUrl.h"
#include <fulla/drogon/controllers/SocialTokenIssuer.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/services/UpstreamProviderRuntime.h>
#include <fulla/identity/OidcUpstreamAuthService.h>

#include <json/json.h>

namespace fulla::drogon::controllers
{

namespace
{
namespace openapi = ::fulla::drogon::observability::openapi;
using CallbackPtr =
  std::shared_ptr<std::function<void(const ::drogon::HttpResponsePtr &)>>;

void respondError(
  const ::drogon::HttpRequestPtr &req,
  const CallbackPtr &cb,
  const std::string &code,
  const std::string &detail)
{
    common::error::ErrorResponder::respond(
      req,
      [cb](const ::drogon::HttpResponsePtr &resp) {
          services::SocialLoginGuard::noStore([cb](const ::drogon::HttpResponsePtr &wrapped) {
              (*cb)(wrapped);
          })(resp);
      },
      code,
      detail);
}

std::once_flag docsOnce;
}  // namespace

void UpstreamLoginController::initApiDocs()
{
    std::call_once(docsOnce, [] {
        openapi::EndpointInfo ep;
        ep.path = "/api/auth/oidc/{provider}/login";
        ep.method = "POST";
        ep.summary = "OIDC Upstream Login (registry provider)";
        ep.description =
          "Exchange an authorization code from a registry-configured OIDC "
          "upstream (upstream_providers row) for a first-party token pair "
          "(P2-1): code -> token endpoint -> userinfo -> claims mapping -> "
          "the same find-or-create account flow as google/wechat/github "
          "(oauth2_subject_mappings.provider = provider_key). Requires the "
          "one-time state from the authorize endpoint, an allowed Origin and "
          "the browser binding cookie.";
        ep.tags = {"External Auth", "OIDC Upstream"};
        ep.requiresAuth = false;
        ep.requestBody = social_detail::socialLoginRequestBody();
        openapi::ParameterInfo provider;
        provider.name = "provider";
        provider.description = "Enabled registry provider key (upstream_providers.provider_key)";
        provider.type = openapi::ParameterType::STRING;
        provider.location = openapi::ParameterLocation::PATH;
        provider.required = true;
        ep.parameters.push_back(std::move(provider));
        ep.responses = {
          {200, "First-party token pair for the upstream-linked account"},
          {400, "Missing, expired, mismatched or replayed transaction"},
          {403, "Not linked to a local account and auto-create disabled"},
          {404, "Provider unavailable"},
          {500, "State service unavailable"},
          {502, "Failed to contact the upstream token/userinfo endpoints"}};
        openapi::OpenApiGenerator::addEndpoint(std::move(ep));
    });
}

OAuth2Plugin *UpstreamLoginController::resolvePlugin() const
{
    return plugin_ ? plugin_ : ::drogon::app().getPlugin<OAuth2Plugin>();
}

void UpstreamLoginController::login(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &provider)
{
    if (req->method() == ::drogon::Options)
    {
        callback(::drogon::HttpResponse::newHttpResponse());
        return;
    }
    auto sharedCb = std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
      services::SocialLoginGuard::noStore(std::move(callback)));
    if (!loginGuard_ || !runtime_)
    {
        respondError(req, sharedCb, "INTERNAL_ERROR", "upstream login: runtime not wired");
        return;
    }
    loginGuard_->validate(
      req, provider,
      [this, req, sharedCb, provider](const std::string &code, const std::string &error) {
          if (!error.empty())
          {
              respondError(req, sharedCb, error, "upstream login: " + error);
              return;
          }
          loginValidated(
            req, [sharedCb](const ::drogon::HttpResponsePtr &resp) { (*sharedCb)(resp); }, provider,
            code);
      });
}

void UpstreamLoginController::loginValidated(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &provider,
  const std::string &code)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    runtime_->findForLogin(
      provider,
      [this, req, sharedCb, provider, code](bool ok, fulla::identity::UpstreamProviderRecord record) {
          if (!ok)
          {
              respondError(req, sharedCb, "VALIDATION_RESOURCE_NOT_FOUND",
                           "upstream login: provider '" + provider + "' not enabled");
              return;
          }
          auto plugin = resolvePlugin();
          auto service = runtime_ ? runtime_->oidcService() : nullptr;
          if (!plugin || !service)
          {
              respondError(req, sharedCb, "INTERNAL_ERROR",
                           "upstream login: plugin/service unavailable");
              return;
          }
          const auto endpoints = runtime_->endpointsFor(provider);
          // The redirect_uri used at authorize time lives in the guard's
          // dynamic-provider snapshot; the exchange MUST repeat the exact
          // same value (upstreams validate the match).
          std::string redirectUri;
          for (const auto &dynamic : loginGuard_->dynamicProviders())
          {
              if (dynamic.key == provider)
              {
                  redirectUri = dynamic.redirectUri;
                  break;
              }
          }
          if (redirectUri.empty() || endpoints.token.empty() || endpoints.userinfo.empty())
          {
              respondError(req, sharedCb, "INTERNAL_ERROR",
                           "upstream login: endpoints unresolved for '" + provider + "'");
              return;
          }
          service->login(
            record, endpoints, redirectUri, code,
            [this, req, sharedCb, provider](
              fulla::identity::OidcUpstreamAuthService::LoginResult result) {
                if (!result.errorCode.empty())
                {
                    respondError(req, sharedCb, result.errorCode,
                                 "upstream login (" + provider + "): " + result.errorCode);
                    return;
                }
                SocialTokenIssuer::issueTokensForUser(
                  req, sharedCb, resolvePlugin(), provider, result.userId, result.publicSub);
            });
      });
}

}  // namespace fulla::drogon::controllers

#endif  // WITH_SOCIAL
