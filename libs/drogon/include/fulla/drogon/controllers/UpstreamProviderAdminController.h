#pragma once

// P2-1: admin CRUD over the OIDC-in upstream provider registry
// (upstream_providers). Scope conventions mirror the claim-mappings admin
// (roles:read / roles:write, impliedBy admin); every mutation refreshes the
// UpstreamProviderRuntime so authorize/login see the change immediately.
// client_secret is write-only (never echoed; empty on update = keep).

#include <drogon/HttpController.h>

#include <functional>
#include <memory>
#include <string>

namespace fulla::drogon::services
{
class UpstreamProviderRuntime;
}

namespace fulla::drogon::controllers
{

class UpstreamProviderAdminController : public ::drogon::HttpController<UpstreamProviderAdminController, false>
{
  public:
    static void initApiDocs();

    void setRuntime(std::shared_ptr<services::UpstreamProviderRuntime> runtime)
    {
        runtime_ = std::move(runtime);
    }

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(
      UpstreamProviderAdminController::list,
      "/api/admin/upstream-providers", ::drogon::Get,
      "fulla::drogon::filters::AuthorizationFilter");
    ADD_METHOD_TO(
      UpstreamProviderAdminController::getOne,
      "/api/admin/upstream-providers/{providerKey}", ::drogon::Get,
      "fulla::drogon::filters::AuthorizationFilter");
    ADD_METHOD_TO(
      UpstreamProviderAdminController::create,
      "/api/admin/upstream-providers", ::drogon::Post,
      "fulla::drogon::filters::AuthorizationFilter");
    ADD_METHOD_TO(
      UpstreamProviderAdminController::update,
      "/api/admin/upstream-providers/{providerKey}", ::drogon::Put,
      "fulla::drogon::filters::AuthorizationFilter");
    ADD_METHOD_TO(
      UpstreamProviderAdminController::remove,
      "/api/admin/upstream-providers/{providerKey}", ::drogon::Delete,
      "fulla::drogon::filters::AuthorizationFilter");
    METHOD_LIST_END

    void list(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback);
    void getOne(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &providerKey);
    void create(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback);
    void update(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &providerKey);
    void remove(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &providerKey);

  private:
    static void initApiDocsImpl();
    std::shared_ptr<services::UpstreamProviderRuntime> runtime_;
};

}  // namespace fulla::drogon::controllers
