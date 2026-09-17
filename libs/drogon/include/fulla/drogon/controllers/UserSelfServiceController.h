#pragma once

// M3 Task 20 slice 7 (fulla-sdk-refactor): relocated from
// OAuth2Server/controllers/UserSelfServiceController.h into
// fulla::drogon::controllers, following the AutoCreation=false
// pattern verified in slice 3 (HealthController) -- see PROGRESS.md.

#include <drogon/HttpController.h>

#ifdef WITH_SOCIAL
// B2 social link/unlink: the injected orchestration service backing the
// /api/me/social/links* routes (raw-pointer setter = the same
// DrClassMap-singleton mock seam Google/WeChat/GitHubController use).
namespace fulla::identity
{
class SocialLinkService;
}
#endif  // WITH_SOCIAL

namespace fulla::drogon::controllers
{

class UserSelfServiceController : public ::drogon::HttpController<UserSelfServiceController, false>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(
      UserSelfServiceController::getProfile,
      "/api/me",
      ::drogon::Get,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      UserSelfServiceController::updateProfile,
      "/api/me/profile",
      ::drogon::Patch,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      UserSelfServiceController::changePassword,
      "/api/me/password",
      ::drogon::Put,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      UserSelfServiceController::listAuthorizedApps,
      "/api/me/authorized-apps",
      ::drogon::Get,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      UserSelfServiceController::revokeAuthorizedApp,
      "/api/me/authorized-apps/{clientId}",
      ::drogon::Delete,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      UserSelfServiceController::deleteAccount,
      "/api/me",
      ::drogon::Delete,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
#ifdef WITH_SOCIAL
    // B2 social link/unlink (design doc §3): list / link / unlink the
    // current user's provider identity mappings. Routes exist only in
    // WITH_SOCIAL builds -- the social controllers are compiled out
    // entirely without it; this controller is always compiled, so the
    // method-list segment carries the guard instead.
    ADD_METHOD_TO(
      UserSelfServiceController::listSocialLinks,
      "/api/me/social/links",
      ::drogon::Get,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    // #71: server-side link state -- the SPA starts here, gets the provider
    // authorize URL (with the one-time state embedded), and the link POST
    // below must present the same state.
    ADD_METHOD_TO(
      UserSelfServiceController::beginSocialLink,
      "/api/me/social/links/{provider}/authorize",
      ::drogon::Post,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      UserSelfServiceController::linkSocialAccount,
      "/api/me/social/links/{provider}",
      ::drogon::Post,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      UserSelfServiceController::unlinkSocialAccount,
      "/api/me/social/links/{provider}",
      ::drogon::Delete,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
#endif  // WITH_SOCIAL
    METHOD_LIST_END

    void getProfile(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void updateProfile(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void changePassword(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void listAuthorizedApps(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void revokeAuthorizedApp(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &clientId
    );
    void deleteAccount(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
#ifdef WITH_SOCIAL
    void listSocialLinks(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void beginSocialLink(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &provider
    );
    void linkSocialAccount(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &provider
    );
    void unlinkSocialAccount(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &provider
    );

    // Mock-injection seam (process-lifetime raw pointer, same contract as
    // GitHubController::setGitHubAuthService -- see tests/common/
    // SocialMockFixture.h's lifetime-safety notes).
    void setSocialLinkService(::fulla::identity::SocialLinkService *service)
    {
        socialLinkService_ = service;
    }
#endif  // WITH_SOCIAL

    // #43: explicit endpoint + scope-requirement registration (replaces the
    // former static-init struct, defect 1.1 SIOF).
    static void initApiDocs();

  private:
    static void initApiDocsImpl();
#ifdef WITH_SOCIAL
    ::fulla::identity::SocialLinkService *socialLinkService_ = nullptr;
#endif  // WITH_SOCIAL
};

}  // namespace fulla::drogon::controllers
