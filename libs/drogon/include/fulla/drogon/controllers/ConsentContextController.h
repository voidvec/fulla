#pragma once

// v1.5.0 M1b (real-tenant design 2.1 item 6 / 2.1 item 7, #223 second half):
// server-side consent-screen context. The consent page used to read
// `owner_name` off the authorize -> consent redirect URL, which a phisher
// could freely forge ("provided by Your Bank..."). The attribution (and the
// new org-membership banner) now come from session state instead: this
// endpoint re-derives them for the flow whose server-minted consent_csrf
// nonce the caller presents.
//
// AutoCreation=false + explicit registerController (AEC pattern); plugin
// pointer via setPlugin() + resolvePlugin() fallback.

#include <drogon/HttpController.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>

namespace fulla::drogon::controllers
{

class ConsentContextController
    : public ::drogon::HttpController<ConsentContextController, false>
{
  public:
    static void initApiDocs();

    void setPlugin(::OAuth2Plugin *plugin)
    {
        plugin_ = plugin;
    }

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(
      ConsentContextController::context, "/oauth2/consent/context", ::drogon::Get
    );
    METHOD_LIST_END

    void context(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );

  private:
    static void initApiDocsImpl();

    ::OAuth2Plugin *plugin_ = nullptr;

    ::OAuth2Plugin *resolvePlugin() const;
};

}  // namespace fulla::drogon::controllers
