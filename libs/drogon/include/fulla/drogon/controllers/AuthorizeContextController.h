#pragma once

// P0-6 (#260): public application-name discovery for the login and consent
// screens. The login page previously used client_id only to resume the
// authorization flow — the user never saw WHICH application is asking for
// credentials until the consent screen, and even there only the raw id.
//
// GET /oauth2/authorize-context?client_id=... answers {client_name} for
// active clients (deleted_at IS NULL). The response deliberately carries
// ONLY the display name: client_id is not a secret (it appears in every
// authorize redirect), and /oauth2/consent/context already exposes
// comparable public data gated by knowledge of a registered redirect_uri.
// Rate limiting rides the process-wide Hodor config (prod profiles list the
// URL in sub_limits; dev/CI baselines run unlimited like the rest of it).

#include <drogon/HttpController.h>

class OAuth2Plugin;

namespace fulla::drogon::controllers
{

class AuthorizeContextController : public ::drogon::HttpController<AuthorizeContextController, false>
{
  public:
    void setPlugin(OAuth2Plugin *plugin)
    {
        plugin_ = plugin;
    }

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AuthorizeContextController::context, "/oauth2/authorize-context", ::drogon::Get);
    METHOD_LIST_END

    void context(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );

    static void initApiDocs();

  private:
    OAuth2Plugin *plugin_ = nullptr;
    OAuth2Plugin *resolvePlugin() const;
};

}  // namespace fulla::drogon::controllers
