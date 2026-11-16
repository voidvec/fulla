#pragma once

// P2-1: generic OIDC-in login exchange for registry-configured upstream
// providers. POST /api/auth/oidc/{provider}/login takes the same
// {code, state} body as /api/{google|wechat|github}/login, rides the SAME
// SocialLoginGuard protections (origin allowlist, binding cookie, one-time
// state), and answers the same first-party token-pair shape via
// SocialTokenIssuer -- account linking goes through
// oauth2_subject_mappings.provider = provider_key.

#include <drogon/HttpController.h>

#include <functional>
#include <memory>
#include <string>

class OAuth2Plugin;

namespace fulla::drogon::services
{
class SocialLoginGuard;
class UpstreamProviderRuntime;
}  // namespace fulla::drogon::services

namespace fulla::drogon::controllers
{

class UpstreamLoginController : public ::drogon::HttpController<UpstreamLoginController, false>
{
  public:
    static void initApiDocs();

    void setRuntime(std::shared_ptr<services::UpstreamProviderRuntime> runtime)
    {
        runtime_ = std::move(runtime);
    }
    // Fixture seam: the social mock fixture re-binds the guard and needs the
    // runtime to re-target it (production wiring never swaps guards).
    std::shared_ptr<services::UpstreamProviderRuntime> runtime() const
    {
        return runtime_;
    }
    void setSocialLoginGuard(std::shared_ptr<services::SocialLoginGuard> guard)
    {
        loginGuard_ = std::move(guard);
    }
    // Same seam as GoogleController::setPlugin(): token issuance needs the
    // plugin (saveTokenPair/TTLs/issuer).
    void setPlugin(OAuth2Plugin *plugin)
    {
        plugin_ = plugin;
    }

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(
      UpstreamLoginController::login,
      "/api/auth/oidc/{provider}/login",
      ::drogon::Post,
      ::drogon::Options);
    METHOD_LIST_END

    void login(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &provider
    );

  private:
    OAuth2Plugin *resolvePlugin() const;
    void loginValidated(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &provider,
      const std::string &code
    );

    std::shared_ptr<services::UpstreamProviderRuntime> runtime_;
    std::shared_ptr<services::SocialLoginGuard> loginGuard_;
    OAuth2Plugin *plugin_ = nullptr;
};

}  // namespace fulla::drogon::controllers
