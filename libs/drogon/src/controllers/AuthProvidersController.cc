#include <fulla/drogon/controllers/AuthProvidersController.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>

#include "SocialAuthorizeUrl.h"

#include <drogon/drogon.h>
#include <json/json.h>

namespace fulla::drogon::controllers
{

namespace openapi = ::fulla::drogon::observability::openapi;

// Config is restart-static by design and fully loaded by the time
// IdentityAssembly runs (registerBeginningAdvice ordering), so the response
// body is built eagerly at wiring time and served lock-free afterwards.
// Entries whose URL builder returns "" (should not happen — the wiring gate
// already required configured credentials) are dropped rather than
// advertised broken.
void AuthProvidersController::buildBody()
{
    Json::Value providers(Json::arrayValue);
    for (const auto &name : providerNames_)
    {
        const std::string authorizeUrl = social_detail::buildSocialAuthorizeUrl(name, "");
        if (authorizeUrl.empty())
            continue;
        Json::Value provider;
        provider["provider"] = name;
        provider["authorize_url"] = authorizeUrl;
        providers.append(std::move(provider));
    }
    cachedBody_ = Json::Value(Json::objectValue);
    cachedBody_["providers"] = std::move(providers);
}

void AuthProvidersController::list(
  const ::drogon::HttpRequestPtr & /*req*/,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    // Always 200 — an empty providers array is a valid, meaningful answer
    // (external login disabled), not an error. No auth, no DB, no per-
    // request work: the body is the startup snapshot.
    auto resp = ::drogon::HttpResponse::newHttpJsonResponse(cachedBody_);
    resp->setStatusCode(::drogon::k200OK);
    callback(resp);
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
          "currently offers (v1.4.0 provider tiers). The login page renders "
          "its provider buttons from this response; an empty list means "
          "external login is disabled. authorize_url is the fully built "
          "provider authorize URL (redirect target resolved from the "
          "per-provider redirect_uri override, else frontend.url + "
          "/callback/{provider}) — the SPA redirects to it as-is.";
        ep.tags = {"External Auth"};
        ep.requiresAuth = false;
        openapi::OpenApiGenerator::addEndpoint(std::move(ep));
    });
}

}  // namespace fulla::drogon::controllers
