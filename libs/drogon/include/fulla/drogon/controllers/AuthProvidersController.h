#pragma once

// v1.5.0 social provider tiers: public, unauthenticated discovery of the
// external login providers this deployment currently offers. The user SPA
// renders its login-page buttons from this response instead of build-time
// VITE_* env vars, so a tier switch (custom_config external_auth.tiers.*)
// or a credential removal takes effect on server restart without rebuilding
// the frontend.
//
// IdentityAssembly pushes the ENABLED provider names at startup — computed
// from the exact same enablement gate (#111 credential check ANDed with the
// tier flags) that injects or withholds the provider services, so the
// buttons can never advertise a provider the login endpoints would refuse.
// The authorize URLs themselves are built here per response via the shared
// src-local builder (SocialAuthorizeUrl.h — the same redirect resolution
// the link flow uses), statelessly: the login flow posts the provider code
// straight back and carries no state today. An empty list means external
// login is fully disabled.
//
// AutoCreation=false process-wide singleton — see GitHubController.h's
// [this]-capture note for the lifetime contract (raw-pointer wiring via
// DrClassMap::getSingleInstance, no shared_from_this).

#include <drogon/HttpController.h>
#include <json/json.h>

#include <string>
#include <vector>

namespace fulla::drogon::controllers
{

class AuthProvidersController : public ::drogon::HttpController<AuthProvidersController, false>
{
  public:
    // Called once at startup by IdentityAssembly (before any request can be
    // dispatched — registerBeginningAdvice ordering, see wireController-
    // PluginDependencies' comment on the same mechanism). Builds the response
    // body eagerly (config is restart-static); afterwards list() serves the
    // snapshot lock-free.
    void setProviders(std::vector<std::string> providerNames)
    {
        providerNames_ = std::move(providerNames);
        buildBody();
    }

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AuthProvidersController::list, "/api/auth/providers", ::drogon::Get);
    METHOD_LIST_END

    void list(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );

    // OpenAPI registration (main.cc calls it explicitly, same as the other
    // controllers — see main.cc's "Initializing API documentation" block).
    static void initApiDocs();

  private:
    std::vector<std::string> providerNames_;
    // Default body is the schema-valid empty answer: when wireIdentityServices
    // early-returns (memory storage / no DB client), setProviders never runs,
    // and the published contract requires the `providers` KEY (empty array =
    // external login off), not a bare object.
    Json::Value cachedBody_{Json::objectValue};
    void buildBody();

  public:
    AuthProvidersController()
    {
        buildBody();  // {"providers":[]} until IdentityAssembly refines it
    }
};

}  // namespace fulla::drogon::controllers
