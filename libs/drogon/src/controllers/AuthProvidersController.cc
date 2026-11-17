#include <fulla/drogon/controllers/AuthProvidersController.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>
#include <fulla/drogon/error/ErrorResponder.h>

#include "SocialAuthorizeUrl.h"

#ifdef WITH_SOCIAL
#include <fulla/drogon/services/UpstreamProviderRuntime.h>
#endif  // WITH_SOCIAL

#include <drogon/drogon.h>
#include <json/json.h>

namespace fulla::drogon::controllers
{

namespace openapi = ::fulla::drogon::observability::openapi;

// Config is restart-static by design and fully loaded by the time
// IdentityAssembly runs (registerBeginningAdvice ordering), so the response
// body is built eagerly at wiring time and served lock-free afterwards.
// Discovery advertises provider names without reusable authorization URLs.
// P2-1c: entries disabled via their registry row (runtime toggle) are
// filtered out so the buttons can never advertise a provider the guard
// would 404.
void AuthProvidersController::buildBody()
{
    Json::Value providers(Json::arrayValue);
    for (const auto &name : providerNames_)
    {
        if (disabledProviders_.count(name) > 0)
            continue;
        Json::Value provider;
        provider["provider"] = name;
        provider["authorize_url"] = "";  // Deprecated; never publish a stateless login URL.
        providers.append(std::move(provider));
    }
    cachedBody_ = Json::Value(Json::objectValue);
    cachedBody_["providers"] = std::move(providers);
}

void AuthProvidersController::setUpstreamRuntime(
  std::shared_ptr<services::UpstreamProviderRuntime> runtime)
{
#ifdef WITH_SOCIAL
    upstreamRuntime_ = std::move(runtime);
#else
    (void)runtime;
#endif
}

void AuthProvidersController::list(
  const ::drogon::HttpRequestPtr & /*req*/,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    // Always 200 — an empty providers array is a valid, meaningful answer
    // (external login disabled), not an error. No auth, no DB, no per-
    // request work: the body is the startup snapshot merged (in memory)
    // with the registry runtime's last-refresh snapshot.
    Json::Value body = cachedBody_;
#ifdef WITH_SOCIAL
    if (upstreamRuntime_)
    {
        for (const auto &record : upstreamRuntime_->enabledSnapshot())
        {
            Json::Value provider;
            provider["provider"] = record.providerKey;
            provider["display_name"] = record.displayName;
            provider["type"] = "oidc";
            body["providers"].append(std::move(provider));
        }
    }
#endif
    auto resp = ::drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(::drogon::k200OK);
    resp->addHeader("Cache-Control", "no-store");
    callback(resp);
}

void AuthProvidersController::authorize(const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  [[maybe_unused]] const std::string &provider)
{
#ifdef WITH_SOCIAL
    if (loginGuard_)
    {
        loginGuard_->authorize(req, provider, std::move(callback));
        return;
    }
#endif
    auto noStore = [callback = std::move(callback)](const auto &response) {
        response->addHeader("Cache-Control", "no-store");
        callback(response);
    };
    common::error::ErrorResponder::respond(req, std::move(noStore),
                                          "VALIDATION_RESOURCE_NOT_FOUND", "Provider unavailable");
}

void AuthProvidersController::initApiDocs()
{
    static std::once_flag docsOnce;
    std::call_once(docsOnce, [] {
        openapi::EndpointInfo ep;
        ep.path = "/api/auth/providers";
        ep.method = "GET";
        ep.summary = "List Enabled External Login Providers";
        ep.description =
          "Public discovery of the external login providers this deployment "
          "currently offers (v1.4.0 provider tiers; P2-1 registry-configured "
          "OIDC upstreams carry type=oidc and display_name). The login page "
          "renders its provider buttons from this response; an empty list "
          "means external login is disabled. authorize_url is deprecated and "
          "empty; POST to the provider authorize endpoint for a browser-bound "
          "login.";
        ep.tags = {"External Auth"};
        ep.requiresAuth = false;
        ep.responses = {{200, "Enabled providers with deprecated empty authorize_url"}};
        openapi::OpenApiGenerator::addEndpoint(std::move(ep));

        openapi::EndpointInfo start;
        start.path = "/api/auth/providers/{provider}/authorize";
        start.method = "POST";
        start.summary = "Start Browser-Bound Social Login";
        start.description = "Initialize a binding cookie (204) or issue a one-time login state (200). "
                            "Requires an allowed Origin and a browser accepting cookies.";
        start.tags = {"External Auth"};
        start.requiresAuth = false;
        start.requestBody["required"] = false;
        start.requestBody["content"]["application/json"]["schema"]["type"] = "object";
        openapi::ParameterInfo parameter;
        parameter.name = "provider";
        parameter.description = "Enabled social provider: github, google or wechat";
        parameter.type = openapi::ParameterType::STRING;
        parameter.location = openapi::ParameterLocation::PATH;
        parameter.required = true;
        start.parameters.push_back(std::move(parameter));
        start.responses = {{200, "Bound authorization transaction"}, {204, "Browser binding initialized"},
                           {400, "Invalid browser binding"}, {403, "Origin denied"},
                           {404, "Provider unavailable"}, {500, "State service unavailable"}};
        Json::Value transaction;
        transaction["state"] = std::string(64, 'a');
        transaction["authorize_url"] = "https://github.com/login/oauth/authorize?state=" + std::string(64, 'a');
        transaction["redirect_uri"] = "https://portal.example/callback/github";
        transaction["expires_in"] = 600;
        start.responseExamples = {{200, transaction}};
        openapi::OpenApiGenerator::addEndpoint(std::move(start));
    });
}

}  // namespace fulla::drogon::controllers
