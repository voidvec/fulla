#include <fulla/drogon/controllers/GoogleController.h>
#include <drogon/HttpClient.h>
#include <drogon/drogon.h>
#include <fulla/drogon/controllers/SocialTokenIssuer.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/error/ErrorResponder.h>

#ifdef WITH_SOCIAL
// Task 24 slice 5 (fulla-sdk-refactor): identity-layer service this
// controller now optionally consumes.
#include <fulla/identity/SocialAuthService.h>
#endif  // WITH_SOCIAL

#include "SocialAuthorizeUrl.h"

namespace fulla::drogon::controllers
{

namespace
{

// Provider configuration lives in custom_config "external_auth.google"
// (client_id / client_secret / redirect_uri); see the deployment docs. A
// missing or YOUR_*-placeholder credential disables the provider (#111).
const std::string GOOGLE_CLIENT_ID_KEY = "client_id";
const std::string GOOGLE_CLIENT_SECRET_KEY = "client_secret";

std::string getGoogleConfig(const std::string &key)
{
    auto config = ::drogon::app().getCustomConfig();
    if (config.isMember("external_auth") && config["external_auth"].isMember("google"))
    {
        return config["external_auth"]["google"].get(key, "").asString();
    }
    return "";
}

// Emit an Application error via the unified ErrorResponder entry point so the
// body is always an Error Envelope (Requirement 7.1 / 7.3 / 7.5).
void respondError(
  const ::drogon::HttpRequestPtr &req,
  const std::shared_ptr<std::function<void(const ::drogon::HttpResponsePtr &)>> &cb,
  std::string code,
  std::string detailForLog = ""
)
{
    ::fulla::common::error::ErrorResponder::respond(
      req,
      [cb](const ::drogon::HttpResponsePtr &r) { (*cb)(r); },
      std::move(code),
      std::move(detailForLog)
    );
}

// Register OpenAPI documentation (executed once at startup)
struct GoogleControllerDocs
{
    GoogleControllerDocs()
    {
        using namespace ::fulla::drogon::observability::openapi;

        Json::Value successExample;
        successExample["access_token"] = "opaque-access-token";
        successExample["refresh_token"] = "opaque-refresh-token";
        successExample["token_type"] = "Bearer";
        successExample["expires_in"] = 3600;

        Json::Value errorExample;
        errorExample["error"]["code"] = "VALIDATION_MISSING_REQUIRED_FIELD";

        // C++17 compatible initialization (avoid designated initializers)
        ::fulla::drogon::observability::openapi::EndpointInfo googleEndpoint;
        googleEndpoint.path = "/api/google/login";
        googleEndpoint.method = "POST";
        googleEndpoint.summary = "Google OAuth2 Login";
        googleEndpoint.description =
          "Exchange Google authorization code for a first-party token pair "
          "(#70): the Google identity is resolved to a local account (auto-"
          "created on first login unless external_auth.auto_create_on_first_"
          "login is disabled, which yields 403 AUTH_SOCIAL_ACCOUNT_NOT_LINKED) "
          "and an access/refresh pair is issued for the configured first-party "
          "client. A SOCIAL_LOGIN_TOKEN_ISSUED audit event records the "
          "issuance.";
        googleEndpoint.tags = {"External Auth", "Google"};

        googleEndpoint.requestBody = social_detail::socialLoginRequestBody();
        googleEndpoint.description += " Requires code and one-time state in a JSON or form body, "
          "an allowed Origin and the browser binding cookie; query credentials are rejected.";

        // Initialize responses
        googleEndpoint.responses =
          {{200, "First-party token pair for the Google-linked account"},
           {400, "Missing, expired, mismatched or replayed transaction"},
           {404, "Provider unavailable"}, {500, "State service unavailable"},
           {403, "Not linked to a local account and auto-create disabled"},
           {502, "Failed to contact Google API"}};

        // Initialize response examples
        googleEndpoint.responseExamples = {{200, successExample}, {400, errorExample}};

        googleEndpoint.requiresAuth = false;

        OpenApiGenerator::addEndpoint(googleEndpoint);
    }
};

GoogleControllerDocs docs_;

}  // namespace

OAuth2Plugin *GoogleController::resolvePlugin() const
{
    return plugin_ ? plugin_ : ::drogon::app().getPlugin<OAuth2Plugin>();
}

void GoogleController::login(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    if (req->method() == ::drogon::Options)
    {
        callback(::drogon::HttpResponse::newHttpResponse());
        return;
    }
    if (!loginGuard_)
    {
        services::SocialLoginGuard::error(req, std::move(callback), "INTERNAL_ERROR");
        return;
    }
    auto sharedCb = std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
      services::SocialLoginGuard::noStore(std::move(callback)));
    loginGuard_->validate(req, "google",
      [this, req, sharedCb](const std::string &code, const std::string &error) {
          if (!error.empty())
          {
              services::SocialLoginGuard::error(req,
                [sharedCb](const auto &response) { (*sharedCb)(response); }, error);
              return;
          }
          loginValidated(req, [sharedCb](const auto &response) { (*sharedCb)(response); }, code);
      });
}

void GoogleController::loginValidated(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &code)
{
    try
    {
#ifdef WITH_SOCIAL
        // Task 24 slice 5: prefer the injected GoogleAuthService (constructed
        // once at startup by bootstrap::wireIdentityServices(), backed by
        // DrogonOAuthHttpClient), falling back to the pre-Task-24
        // drogon::HttpClient-direct path when unwired -- same
        // injected-with-fallback pattern established by SessionController's
        // Task 24 slice 4.
        if (googleAuthService_)
        {
            auto sharedCb =
              std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
                std::move(callback)
              );
            googleAuthService_
              ->login(code, [this, sharedCb, req](fulla::identity::GoogleLoginResult result) {
                  if (!result.errorCode.empty())
                  {
                      respondError(
                        req, sharedCb, result.errorCode, "google login: " + result.errorCode
                      );
                      return;
                  }
                  if (!result.publicSub.empty())
                  {
                      // #70: account-linked login — mint + persist a
                      // first-party token pair (stored under the platform
                      // subject; audit-traced) through the same shared issuer
                      // as GitHub/WeChat.
                      auto plugin = resolvePlugin();
                      if (!plugin)
                      {
                          respondError(
                            req, sharedCb, "INTERNAL_ERROR",
                            "google login: OAuth2Plugin not available"
                          );
                          return;
                      }
                      SocialTokenIssuer::issueTokensForUser(
                        req, sharedCb, plugin, "google", result.userId, result.publicSub
                      );
                      return;
                  }
                  // Degraded (service constructed without a repository —
                  // direct construction/tests): the pre-#70 profile-only
                  // response shape.
                  Json::Value filteredJson;
                  filteredJson["sub"] = result.profile.sub;
                  filteredJson["name"] = result.profile.name;
                  filteredJson["email"] = result.profile.email;
                  filteredJson["picture"] = result.profile.picture;
                  (*sharedCb)(::drogon::HttpResponse::newHttpJsonResponse(filteredJson));
              });
            return;
        }
#endif  // WITH_SOCIAL

        // #111: empty OR still the YOUR_* template -> the provider is
        // disabled; fail fast with an envelope instead of a doomed upstream
        // call. See docs on provider configuration (external_auth.google).
        {
            const auto credentialConfigured = [](const std::string &v) {
                return !v.empty() && v.rfind("YOUR_", 0) != 0;
            };
            if (!credentialConfigured(getGoogleConfig(GOOGLE_CLIENT_ID_KEY)) ||
                !credentialConfigured(getGoogleConfig(GOOGLE_CLIENT_SECRET_KEY)))
            {
                ::fulla::common::error::ErrorResponder::respond(
                  req,
                  std::move(callback),
                  "INTERNAL_ERROR",
                  "google login: Google OAuth not configured"
                );
                return;
            }
        }

        // 1. Exchange Code for Access Token
        // API: https://oauth2.googleapis.com/token
        auto client = ::drogon::HttpClient::newHttpClient("https://oauth2.googleapis.com");
        auto request = ::drogon::HttpRequest::newHttpRequest();
        request->setMethod(::drogon::Post);
        request->setPath("/token");
        request->setParameter("code", code);
        request->setParameter("client_id", getGoogleConfig(GOOGLE_CLIENT_ID_KEY));
        request->setParameter("client_secret", getGoogleConfig(GOOGLE_CLIENT_SECRET_KEY));
        request->setParameter("redirect_uri", social_detail::socialRedirectUri(
          ::drogon::app().getCustomConfig(), "google"));
        request->setParameter("grant_type", "authorization_code");

        auto callbackPtr = std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
          std::move(callback)
        );

        client->sendRequest(
          request,
          [callbackPtr,
           client,
           req](::drogon::ReqResult result, const ::drogon::HttpResponsePtr &response) {
              try
              {
                  if (
                    result != ::drogon::ReqResult::Ok || !response ||
                    response->getStatusCode() != ::drogon::k200OK
                  )
                  {
                      respondError(
                        req,
                        callbackPtr,
                        "NET_CONNECTION_FAILED",
                        "google login: failed to contact Google Token API"
                      );
                      return;
                  }

                  auto json = response->getJsonObject();
                  if (
                    !json || !json->isMember("access_token") || !(*json)["access_token"].isString()
                  )
                  {
                      respondError(
                        req,
                        callbackPtr,
                        "VALIDATION_INVALID_INPUT",
                        "google login: invalid token response"
                      );
                      return;
                  }

                  std::string accessToken = (*json)["access_token"].asString();

                  // 2. Fetch User Info
                  // API: https://www.googleapis.com/oauth2/v3/userinfo
                  auto client2 = ::drogon::HttpClient::newHttpClient("https://www.googleapis.com");
                  auto req2 = ::drogon::HttpRequest::newHttpRequest();
                  req2->setPath("/oauth2/v3/userinfo");
                  req2->addHeader("Authorization", "Bearer " + accessToken);

                  client2->sendRequest(
                    req2,
                    [callbackPtr,
                     req](::drogon::ReqResult res2, const ::drogon::HttpResponsePtr &resp2) {
                        try
                        {
                            if (res2 != ::drogon::ReqResult::Ok || !resp2)
                            {
                                respondError(
                                  req,
                                  callbackPtr,
                                  "NET_CONNECTION_FAILED",
                                  "google login: failed to fetch Google UserInfo"
                                );
                                return;
                            }

                            // Filter response to only include necessary fields
                            // (security best practice)
                            auto googleData = resp2->getJsonObject();
                            Json::Value filteredJson;
                            filteredJson["sub"] = (*googleData).get("sub", "").asString();
                            filteredJson["name"] = (*googleData).get("name", "").asString();
                            filteredJson["email"] = (*googleData).get("email", "").asString();
                            filteredJson["picture"] = (*googleData).get("picture", "").asString();

                            auto finalResp =
                              ::drogon::HttpResponse::newHttpJsonResponse(filteredJson);
                            (*callbackPtr)(finalResp);
                        }
                        catch (const std::exception &e)
                        {
                            LOG_ERROR << "GoogleController::login inner async callback exception: "
                                      << e.what();
                            respondError(
                              req,
                              callbackPtr,
                              "INTERNAL_ERROR",
                              "google login: " + std::string(e.what())
                            );
                        }
                        catch (...)
                        {
                            LOG_ERROR
                              << "GoogleController::login inner async callback unknown exception";
                            respondError(
                              req, callbackPtr, "INTERNAL_ERROR", "google login: unknown error"
                            );
                        }
                    }
                  );
              }
              catch (const std::exception &e)
              {
                  LOG_ERROR << "GoogleController::login async callback exception: " << e.what();
                  respondError(
                    req, callbackPtr, "INTERNAL_ERROR", "google login: " + std::string(e.what())
                  );
              }
              catch (...)
              {
                  LOG_ERROR << "GoogleController::login async callback unknown exception";
                  respondError(req, callbackPtr, "INTERNAL_ERROR", "google login: unknown error");
              }
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "GoogleController::login exception: " << e.what();
        ::fulla::common::error::ErrorResponder::respond(
          req, std::move(callback), "INTERNAL_ERROR", "google login: " + std::string(e.what())
        );
    }
    catch (...)
    {
        LOG_ERROR << "GoogleController::login unknown exception";
        ::fulla::common::error::ErrorResponder::respond(
          req, std::move(callback), "INTERNAL_ERROR", "google login: unknown error"
        );
    }
}

}  // namespace fulla::drogon::controllers
