#pragma once

// v1.4.0 open platform routes (see ApplicationService.h). /api/me routes use
// OAuth2AuthFilter (user tokens, `profile` scope); the two admin governance
// routes use AuthorizationFilter like the rest of /api/admin.

#include <drogon/HttpController.h>

namespace openplatform
{

class ApplicationController : public ::drogon::HttpController<ApplicationController, false>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(
      ApplicationController::list,
      "/api/me/applications",
      ::drogon::Get,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      ApplicationController::create,
      "/api/me/applications",
      ::drogon::Post,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      ApplicationController::update,
      "/api/me/applications/{clientId}",
      ::drogon::Patch,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      ApplicationController::rotateSecret,
      "/api/me/applications/{clientId}/rotate-secret",
      ::drogon::Post,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      ApplicationController::transfer,
      "/api/me/applications/{clientId}/transfer",
      ::drogon::Post,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      ApplicationController::remove,
      "/api/me/applications/{clientId}",
      ::drogon::Delete,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      ApplicationController::suspend,
      "/api/admin/clients/{clientId}/suspend",
      ::drogon::Post,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    ADD_METHOD_TO(
      ApplicationController::resume,
      "/api/admin/clients/{clientId}/resume",
      ::drogon::Post,
      "fulla::drogon::filters::AuthorizationFilter"
    );
    METHOD_LIST_END

    void list(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void create(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void update(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &clientId
    );
    void rotateSecret(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &clientId
    );
    void transfer(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &clientId
    );
    void remove(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &clientId
    );
    void suspend(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &clientId
    );
    void resume(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &clientId
    );

    /// #43 pattern: OpenAPI scope declarations for the new routes.
    static void initApiDocs();

  private:
    static void initApiDocsImpl();
};

}  // namespace openplatform
