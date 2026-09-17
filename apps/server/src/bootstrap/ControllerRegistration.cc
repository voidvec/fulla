#include "ControllerRegistration.h"
#include <drogon/drogon.h>
#include <drogon/DrClassMap.h>
#include <fulla/drogon/controllers/AuthorizationEndpointController.h>
#include <fulla/drogon/controllers/TokenEndpointController.h>
#include <fulla/drogon/controllers/DiscoveryController.h>
#include <fulla/drogon/controllers/HealthController.h>
#ifdef WITH_SOCIAL
#include <fulla/drogon/controllers/GoogleController.h>
#include <fulla/drogon/controllers/WeChatController.h>
#endif  // WITH_SOCIAL
// M5 Task 30: OrganizationController moved to the product app
// (apps/server/src/organization/, namespace `organization`).
#include <OrganizationController.h>
#include <OrgMemberController.h>
#include <ApplicationController.h>
#include <fulla/drogon/controllers/ClientRegistrationController.h>
#include <fulla/drogon/controllers/ApiDocController.h>
#include <fulla/drogon/controllers/DeviceAuthController.h>
#include <fulla/drogon/controllers/EmailVerificationController.h>
#ifdef WITH_SOCIAL
#include <fulla/drogon/controllers/GitHubController.h>
#endif  // WITH_SOCIAL
#include <fulla/drogon/controllers/MfaController.h>
#include <fulla/drogon/controllers/PasswordResetController.h>
#include <fulla/drogon/controllers/SessionController.h>
#include <fulla/drogon/controllers/UserSelfServiceController.h>
#ifdef WITH_WEBAUTHN
#include <fulla/drogon/controllers/WebAuthnController.h>
#endif  // WITH_WEBAUTHN
#include <fulla/drogon/controllers/ClientAdminController.h>
#include <fulla/drogon/controllers/UserAdminController.h>
#include <fulla/drogon/controllers/RoleScopeAdminController.h>
#include <fulla/drogon/controllers/TokenAdminController.h>
#include <fulla/drogon/controllers/AuditController.h>
#include <fulla/drogon/filters/AuthorizationFilter.h>
#include <fulla/drogon/filters/OAuth2AuthFilter.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>

namespace bootstrap
{

void registerAllControllers()
{
    // M3 Task 20 (verified mechanism, see PROGRESS.md): every controller
    // below uses HttpController<T, false> (AutoCreation=false), so route
    // registration only happens via this explicit call chain -- there is
    // no static-initialization side effect to rely on. This establishes a
    // real, linker-visible reference into each controller's translation
    // unit inside the fulla-drogon static library, which is why a
    // PLAIN (non-whole-archive) link is sufficient.
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::HealthController>()
    );
#ifdef WITH_SOCIAL
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::GoogleController>()
    );
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::WeChatController>()
    );
#endif  // WITH_SOCIAL
    drogon::app().registerController(std::make_shared<::organization::OrganizationController>());
    drogon::app().registerController(std::make_shared<::organization::OrgMemberController>());
    drogon::app().registerController(std::make_shared<::openplatform::ApplicationController>());
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::ClientRegistrationController>()
    );
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::ApiDocController>()
    );
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::DeviceAuthController>()
    );
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::EmailVerificationController>()
    );
#ifdef WITH_SOCIAL
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::GitHubController>()
    );
#endif  // WITH_SOCIAL
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::MfaController>()
    );
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::PasswordResetController>()
    );
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::SessionController>()
    );
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::UserSelfServiceController>()
    );
#ifdef WITH_WEBAUTHN
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::WebAuthnController>()
    );
#endif  // WITH_WEBAUTHN
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::ClientAdminController>()
    );
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::UserAdminController>()
    );
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::RoleScopeAdminController>()
    );
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::TokenAdminController>()
    );
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::AuditController>()
    );
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::AuthorizationEndpointController>()
    );
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::TokenEndpointController>()
    );
    drogon::app().registerController(
      std::make_shared<fulla::drogon::controllers::DiscoveryController>()
    );
}

void wireControllerPluginDependencies()
{
    // M3 Task 23: fetch the plugin exactly once (config-reflection
    // construction has completed by the time registerBeginningAdvice
    // callbacks run -- see this function's header comment for the
    // ordering requirement) and push it into every controller/filter that
    // exposes a setPlugin(). Using drogon::DrClassMap::getSingleInstance<T>()
    // rather than re-constructing each controller: registerController()
    // already called DrClassMap::setSingleInstance(ctrlPtr), so the
    // SAME instance drogon dispatches requests to is the one we mutate
    // here (a fresh std::make_shared<T>() would wire a different, unused
    // object).
    auto plugin = drogon::app().getPlugin<OAuth2Plugin>();
    if (!plugin)
    {
        // Recoverable: an explicit per-request getPlugin<OAuth2Plugin>()
        // fallback exists, so startup continues (just less efficiently).
        LOG_WARN << "wireControllerPluginDependencies: OAuth2Plugin not found; "
                    "controllers/filters will fall back to per-request "
                    "getPlugin<OAuth2Plugin>() lookups";
        return;
    }

    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::HealthController>()
      ->setPlugin(plugin);
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::DeviceAuthController>()
      ->setPlugin(plugin);
#ifdef WITH_SOCIAL
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::GitHubController>()
      ->setPlugin(plugin);
#endif  // WITH_SOCIAL
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::MfaController>()
      ->setPlugin(plugin);
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::SessionController>()
      ->setPlugin(plugin);
    drogon::DrClassMap::getSingleInstance<
      fulla::drogon::controllers::AuthorizationEndpointController>()
      ->setPlugin(plugin);
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::TokenEndpointController>()
      ->setPlugin(plugin);
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::DiscoveryController>()
      ->setPlugin(plugin);

    // Filters are looked up by the same by-name DrClassMap mechanism their
    // ADD_METHOD_TO string references use -- the
    // fulla::drogon::filters::{AuthorizationFilter,OAuth2AuthFilter} classes
    // now live in libs/drogon (include/fulla/drogon/filters/*.h) since the
    // old OAuth2Plugin/ directory was dissolved in Phase 4 of the directory
    // restructure.
    drogon::DrClassMap::getSingleInstance<fulla::drogon::filters::AuthorizationFilter>()
      ->setPlugin(plugin);
    drogon::DrClassMap::getSingleInstance<fulla::drogon::filters::OAuth2AuthFilter>()
      ->setPlugin(plugin);
}

}  // namespace bootstrap
