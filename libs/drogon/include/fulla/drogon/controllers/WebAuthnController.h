#pragma once

// M3 Task 20 slice 7 (fulla-sdk-refactor): relocated from
// OAuth2Server/controllers/WebAuthnController.h into
// fulla::drogon::controllers, following the AutoCreation=false
// pattern verified in slice 3 (HealthController) -- see PROGRESS.md.

#include <drogon/HttpController.h>

// Task 24 slice 5 (fulla-sdk-refactor): see SessionController.h's
// identical forward-declaration comment.
namespace fulla::identity
{
class WebAuthnService;
class IUserRepository;
}  // namespace fulla::identity

namespace fulla::drogon::controllers
{

/**
 * @brief WebAuthn / Passkey Controller
 *
 * Provides endpoints for passwordless authentication using WebAuthn.
 * Registration: user registers a new credential (passkey)
 * Authentication: user authenticates with an existing credential
 */
class WebAuthnController : public ::drogon::HttpController<WebAuthnController, false>
{
  public:
    // Task 24 slice 5: identity-layer service injection, same
    // non-owning-raw-pointer + setter pattern as SessionController's
    // setIdentityAuthService()/etc. Each handler below falls back to the
    // pre-Task-24 raw-SQL path when unset.
    void setWebAuthnService(fulla::identity::WebAuthnService *webAuthnService)
    {
        webAuthnService_ = webAuthnService;
    }

    // Needed to resolve the "userId" request attribute (actually the
    // OAuth2 public_sub) into the internal id WebAuthnService is keyed
    // by, and to resolve a WebAuthnService result's internal id back
    // into a public_sub for authenticateFinish's response -- see
    // IUserRepository::findByPublicSub's header comment.
    void setUserRepository(fulla::identity::IUserRepository *userRepo)
    {
        userRepo_ = userRepo;
    }

    METHOD_LIST_BEGIN
    // Registration flow (requires existing auth)
    ADD_METHOD_TO(
      WebAuthnController::registerBegin,
      "/api/me/webauthn/register/begin",
      ::drogon::Post,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      WebAuthnController::registerFinish,
      "/api/me/webauthn/register/finish",
      ::drogon::Post,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    // Authentication flow (no auth required - this IS the auth)
    ADD_METHOD_TO(
      WebAuthnController::authenticateBegin,
      "/oauth2/webauthn/authenticate/begin",
      ::drogon::Post
    );
    ADD_METHOD_TO(
      WebAuthnController::authenticateFinish,
      "/oauth2/webauthn/authenticate/finish",
      ::drogon::Post
    );
    // List credentials (requires auth)
    ADD_METHOD_TO(
      WebAuthnController::listCredentials,
      "/api/me/webauthn/credentials",
      ::drogon::Get,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    // P0-7a (#261): remove a registered passkey (ownership-scoped delete)
    ADD_METHOD_TO(
      WebAuthnController::deleteCredential,
      "/api/me/webauthn/credentials/{credentialId}",
      ::drogon::Delete,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    METHOD_LIST_END

    void registerBegin(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void registerFinish(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void authenticateBegin(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void authenticateFinish(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void listCredentials(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    // P0-7a (#261): DELETE /api/me/webauthn/credentials/{credentialId}
    void deleteCredential(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &credentialId
    );

  private:
    fulla::identity::WebAuthnService *webAuthnService_ = nullptr;
    fulla::identity::IUserRepository *userRepo_ = nullptr;
};

}  // namespace fulla::drogon::controllers
