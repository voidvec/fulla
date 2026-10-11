#include <fulla/drogon/controllers/AuthorizeContextController.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>
#include <fulla/storage/postgres/models/Oauth2Clients.h>
#include <drogon/drogon.h>

#include <memory>
#include <string>

namespace fulla::drogon::controllers
{

OAuth2Plugin *AuthorizeContextController::resolvePlugin() const
{
    return plugin_ ? plugin_ : ::drogon::app().getPlugin<OAuth2Plugin>();
}

namespace
{
void respondError(
  const ::drogon::HttpRequestPtr &req,
  const std::shared_ptr<std::function<void(const ::drogon::HttpResponsePtr &)>> &cb,
  std::string code,
  std::string detailForLog = ""
)
{
    ::fulla::common::error::ErrorResponder::respond(
      req, [cb](const ::drogon::HttpResponsePtr &r) { (*cb)(r); }, std::move(code), std::move(detailForLog)
    );
}

struct AuthorizeContextDocs
{
    AuthorizeContextDocs()
    {
        ::fulla::drogon::observability::openapi::EndpointInfo docs;
        docs.path = "/oauth2/authorize-context";
        docs.method = "GET";
        docs.summary = "Authorize Context (client display name)";
        docs.description =
          "P0-6: public application-name discovery for the login/consent "
          "screens. Answers {client_name} for active clients; unknown or "
          "soft-deleted clients answer 404. Carries only the display name "
          "(client_id is not secret); rate-limited via the process-wide "
          "limiter config on prod profiles.";
        docs.tags = {"OAuth2"};
        docs.requiresAuth = false;
        ::fulla::drogon::observability::openapi::OpenApiGenerator::addEndpoint(docs);
    }
};

AuthorizeContextDocs docs_;
}  // namespace

void AuthorizeContextController::initApiDocs()
{
    // The anonymous-namespace docs_ instance above registers the endpoint at
    // static-init in translation units that include this .cc; the explicit
    // no-op keeps the main.cc initApiDocs() call shape uniform with the other
    // controllers (the struct is instantiated here once).
}

void AuthorizeContextController::context(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));

    const std::string clientId = req->getParameter("client_id");
    if (clientId.empty())
    {
        respondError(
          req, sharedCb, "VALIDATION_MISSING_REQUIRED_FIELD", "authorize-context: client_id is required"
        );
        return;
    }

    auto plugin = resolvePlugin();
    if (!plugin)
    {
        respondError(req, sharedCb, "INTERNAL_ERROR", "authorize-context: OAuth2 Plugin not loaded");
        return;
    }

    // Neutral degrade shape shared by every non-404 failure below: the id
    // itself as the name (the screens render exactly what they rendered
    // pre-P0-6 — never a hard error for a branding lookup).
    auto respondDegraded = [sharedCb, clientId]() {
        Json::Value json;
        json["client_name"] = clientId;
        (*sharedCb)(::drogon::HttpResponse::newHttpJsonResponse(json));
    };

    // Memory-mode guard (the M1 Debug-assert pitfall): db_clients is empty
    // in memory deployments and getDbClient() aborts there in Debug builds.
    if (plugin->getStorageType() == "memory")
    {
        respondDegraded();
        return;
    }

    ::drogon::orm::DbClientPtr db;
    try
    {
        db = ::drogon::app().getDbClient();
    }
    catch (...)
    {
        db = nullptr;
    }
    if (!db)
    {
        respondDegraded();
        return;
    }

    try
    {
        ::drogon::orm::Mapper<drogon_model::fulla_db::Oauth2Clients>(db)
          .findBy(
            ::drogon::orm::Criteria(
              drogon_model::fulla_db::Oauth2Clients::Cols::_client_id,
              ::drogon::orm::CompareOperator::EQ,
              clientId
            ) &&
              ::drogon::orm::Criteria(
                drogon_model::fulla_db::Oauth2Clients::Cols::_deleted_at,
                ::drogon::orm::CompareOperator::IsNull
              ),
            [sharedCb, req, clientId](const std::vector<drogon_model::fulla_db::Oauth2Clients> &rows) {
                if (rows.empty())
                {
                    respondError(
                      req,
                      sharedCb,
                      "VALIDATION_RESOURCE_NOT_FOUND",
                      "authorize-context: unknown client"
                    );
                    return;
                }
                const std::string name = rows[0].getValueOfName();
                Json::Value json;
                json["client_name"] = !name.empty() ? name : clientId;
                (*sharedCb)(::drogon::HttpResponse::newHttpJsonResponse(json));
            },
            [respondDegraded](const ::drogon::orm::DrogonDbException &e) {
                LOG_WARN << "authorize-context lookup failed: " << e.base().what();
                respondDegraded();
            }
          );
    }
    catch (...)
    {
        respondDegraded();
    }
}

}  // namespace fulla::drogon::controllers
