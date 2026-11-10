#include <fulla/drogon/controllers/ClientRegistrationController.h>
#include <fulla/drogon/services/ClientRegistrationService.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>

#include <drogon/drogon.h>

namespace fulla::drogon::controllers
{

namespace
{
struct ClientRegistrationControllerDocs
{
    ClientRegistrationControllerDocs()
    {
        ::fulla::drogon::observability::openapi::EndpointInfo registerDocs;
        // Root cause fix (遗留事项 L3): commit 9796672 ("chore:
        // clang-format") accidentally rewrote this docs block -- path became
        // "/api/oauth2/register" and requiresAuth false, drifting from the
        // REAL route (ADD_METHOD_TO "/oauth2/register" + AuthorizationFilter
        // in ClientRegistrationController.h). Property4_3_1's whole-spec
        // fingerprint baseline caught exactly this drift. Keep these two
        // fields in lockstep with the header's route registration.
        registerDocs.path = "/oauth2/register";
        registerDocs.method = "POST";
        registerDocs.summary = "Register OAuth2 Client (RFC 7591)";
        registerDocs.description =
          "Dynamically register a new OAuth2 client. Requires a valid Bearer token "
          "and the admin role under the shipped AuthorizationFilter rbac_rules. "
          "Missing/invalid credentials return 401; authenticated callers without "
          "the required role receive 403 (AUTHZ_INSUFFICIENT_PERMISSIONS). "
          "Self-service application registration uses /api/me/applications.";
        registerDocs.tags = {"OAuth2", "Dynamic Registration"};
        registerDocs.requiresAuth = true;
        ::fulla::drogon::observability::openapi::OpenApiGenerator::addEndpoint(registerDocs);
    }
};

ClientRegistrationControllerDocs docs_;
}  // namespace

}  // namespace fulla::drogon::controllers

namespace fulla::drogon::controllers
{

// Task B5: business logic extracted to ClientRegistrationService.

void ClientRegistrationController::registerClient(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));
    services::ClientRegistrationService::registerClient(req, sharedCb);
}

}  // namespace fulla::drogon::controllers
