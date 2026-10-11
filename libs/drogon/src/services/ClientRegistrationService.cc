#include <fulla/drogon/services/ClientRegistrationService.h>

#include <fulla/drogon/adapters/DrogonAuditSink.h>
#include <fulla/storage/postgres/models/Oauth2Clients.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/drogon/validation/RuleSet.h>

#include <drogon/drogon.h>

#include <chrono>

namespace fulla::drogon::services
{

namespace
{
void respondError(
  const ::drogon::HttpRequestPtr &req,
  const ClientRegistrationService::ResponseCallback &cb,
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
}  // namespace

using namespace ::drogon::orm;
using namespace ::drogon_model::fulla_db;

void ClientRegistrationService::registerClient(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback sharedCb
)
{
    // Parse request body
    auto jsonBody = req->getJsonObject();
    if (!jsonBody)
    {
        respondError(
          req,
          sharedCb,
          "VALIDATION_INVALID_INPUT",
          "registerClient: request body must be valid JSON"
        );
        return;
    }

    std::string clientName = (*jsonBody).get("client_name", "").asString();
    std::string clientType = (*jsonBody).get("client_type", "CONFIDENTIAL").asString();
    std::string tokenEndpointAuthMethod =
      (*jsonBody).get("token_endpoint_auth_method", "").asString();
    // F-017 (RFC 7591 §2 / RFC 6749 §3.2.1): apply per-type defaults when the
    // client omits the field. PUBLIC clients can only use "none" (they have no
    // secret); CONFIDENTIAL default to "client_secret_basic". An explicit
    // value from the client is validated against the allowlist below.
    if (tokenEndpointAuthMethod.empty())
    {
        tokenEndpointAuthMethod =
          (clientType == "PUBLIC") ? "none" : "client_secret_basic";
    }
    // Validate the (possibly client-supplied) method against the OIDC set we
    // enforce at the token endpoint.
    if (
      tokenEndpointAuthMethod != "none" &&
      tokenEndpointAuthMethod != "client_secret_basic" &&
      tokenEndpointAuthMethod != "client_secret_post"
    )
    {
        respondError(
          req,
          sharedCb,
          "VALIDATION_FORMAT_ERROR",
          "registerClient: token_endpoint_auth_method must be one of: none, "
          "client_secret_basic, client_secret_post"
        );
        return;
    }
    // PUBLIC clients cannot declare a secret-bearing method (they have none).
    if (clientType == "PUBLIC" && tokenEndpointAuthMethod != "none")
    {
        respondError(
          req,
          sharedCb,
          "VALIDATION_FORMAT_ERROR",
          "registerClient: PUBLIC clients must use token_endpoint_auth_method='none'"
        );
        return;
    }

    if (clientName.empty())
    {
        respondError(
          req,
          sharedCb,
          "VALIDATION_MISSING_REQUIRED_FIELD",
          "registerClient: client_name is required"
        );
        return;
    }

    if (clientType != "CONFIDENTIAL" && clientType != "PUBLIC")
    {
        respondError(
          req,
          sharedCb,
          "VALIDATION_FORMAT_ERROR",
          "registerClient: client_type must be CONFIDENTIAL or PUBLIC"
        );
        return;
    }

    std::string redirectUris;
    Json::Value redirectUrisArray(Json::arrayValue);
    if (jsonBody->isMember("redirect_uris") && (*jsonBody)["redirect_uris"].isArray())
    {
        const auto &uris = (*jsonBody)["redirect_uris"];
        for (Json::ArrayIndex i = 0; i < uris.size(); ++i)
        {
            if (i > 0)
                redirectUris += ",";
            redirectUris += uris[i].asString();
            redirectUrisArray.append(uris[i].asString());
        }
    }

    if (redirectUris.empty() && clientType == "CONFIDENTIAL")
    {
        respondError(
          req,
          sharedCb,
          "VALIDATION_MISSING_REQUIRED_FIELD",
          "registerClient: redirect_uris is required for confidential clients"
        );
        return;
    }

    // F-014: enforce the redirect_uri scheme policy (https required,
    // loopback IP-literal exemption, auth.allow_http_redirect_uri override)
    // at registration time, per RFC 8252 §7.3 / RFC 9700 §2.1.
    for (Json::ArrayIndex i = 0; i < redirectUrisArray.size(); ++i)
    {
        std::string uri = redirectUrisArray[i].asString();
        auto uriError = ::fulla::drogon::validation::RuleSet::validateRedirectUri(uri);
        if (uriError)
        {
            respondError(
              req,
              sharedCb,
              "VALIDATION_FORMAT_ERROR",
              "registerClient: invalid redirect_uri '" + uri + "': " + *uriError
            );
            return;
        }
    }

    std::string allowedGrantTypes;
    Json::Value grantTypesArray(Json::arrayValue);
    if (jsonBody->isMember("grant_types") && (*jsonBody)["grant_types"].isArray())
    {
        const auto &grants = (*jsonBody)["grant_types"];
        for (Json::ArrayIndex i = 0; i < grants.size(); ++i)
        {
            std::string grantType = grants[i].asString();
            if (
              grantType != "authorization_code" && grantType != "refresh_token" &&
              grantType != "client_credentials"
            )
            {
                respondError(
                  req,
                  sharedCb,
                  "VALIDATION_FORMAT_ERROR",
                  "registerClient: unsupported grant_type: " + grantType +
                    ". Allowed: authorization_code, refresh_token, client_credentials"
                );
                return;
            }
            if (i > 0)
                allowedGrantTypes += ",";
            allowedGrantTypes += grantType;
            grantTypesArray.append(grantType);
        }
    }
    else
    {
        allowedGrantTypes = "authorization_code";
        grantTypesArray.append("authorization_code");
    }

    // P0-7b (#261): optional dedicated post-logout registrations (same
    // scheme policy as redirect_uris; comma-joined column).
    std::string postLogoutUris;
    Json::Value postLogoutUrisArray(Json::arrayValue);
    if (jsonBody->isMember("post_logout_redirect_uris") &&
        (*jsonBody)["post_logout_redirect_uris"].isArray())
    {
        const auto &uris = (*jsonBody)["post_logout_redirect_uris"];
        for (Json::ArrayIndex i = 0; i < uris.size(); ++i)
        {
            std::string uri = uris[i].asString();
            auto uriError = ::fulla::drogon::validation::RuleSet::validateRedirectUri(uri);
            if (uriError)
            {
                respondError(
                  req,
                  sharedCb,
                  "VALIDATION_FORMAT_ERROR",
                  "registerClient: invalid post_logout_redirect_uri '" + uri + "': " + *uriError
                );
                return;
            }
            if (i > 0)
                postLogoutUris += ",";
            postLogoutUris += uri;
            postLogoutUrisArray.append(uri);
        }
    }

    std::string clientId = ::drogon::utils::getUuid();
    std::string clientSecret = ::fulla::drogon::utils::generateSecureToken();
    // F-002: salt FIRST, then salted hash. The validation paths
    // (Postgres/RedisClientRepository::validateClient) compute
    // sha256(secret + salt), so storing an unsalted hash made every
    // registered client permanently unable to authenticate.
    std::string salt = ::drogon::utils::getUuid().substr(0, 36);
    std::string secretHash =
      ::fulla::drogon::utils::hashClientSecretWithSalt(clientSecret, salt);

    auto now = std::chrono::system_clock::now();
    auto issuedAt =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    try
    {
        auto db = ::drogon::app().getDbClient();

        Oauth2Clients client;
        client.setClientId(clientId);
        client.setClientType(clientType);
        client.setClientSecret(secretHash);
        client.setSalt(salt);
        client.setName(clientName);
        client.setRedirectUris(redirectUris);
        client.setAllowedGrantTypes(allowedGrantTypes);
        // F-017: persist the declared token-endpoint auth method so the
        // token/introspect/revoke endpoints can enforce it.
        client.setTokenEndpointAuthMethod(tokenEndpointAuthMethod);
        // P0-7b (#261)
        client.setPostLogoutRedirectUris(postLogoutUris);

        Mapper<Oauth2Clients> mapper(db);
        mapper.insert(
          client,
          [sharedCb,
           clientId,
           clientSecret,
           clientName,
           redirectUrisArray,
           grantTypesArray,
           postLogoutUrisArray,
           issuedAt,
           tokenEndpointAuthMethod,
           req](const Oauth2Clients &) {
              Json::Value json;
              json["client_id"] = clientId;
              json["client_secret"] = clientSecret;
              json["client_name"] = clientName;
              json["redirect_uris"] = redirectUrisArray;
              json["grant_types"] = grantTypesArray;
              json["token_endpoint_auth_method"] = tokenEndpointAuthMethod;
              json["post_logout_redirect_uris"] = postLogoutUrisArray;
              json["client_id_issued_at"] = static_cast<Json::Int64>(issuedAt);
              json["client_secret_expires_at"] = 0;

              auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
              resp->setStatusCode(::drogon::k201Created);

              ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
                ::drogon::app().getPlugin<::OAuth2Plugin>()->getAuditSink(),
                "client_registered",
                "success",
                req,
                "",
                "client",
                clientId
              );

              (*sharedCb)(resp);
          },
          [sharedCb, req](const DrogonDbException &e) {
              ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
                ::drogon::app().getPlugin<::OAuth2Plugin>()->getAuditSink(),
                "client_registered",
                "failure",
                req,
                "",
                "client",
                "",
                Json::Value(e.base().what())
              );
              respondError(
                req,
                sharedCb,
                "DB_QUERY_ERROR",
                std::string("registerClient: failed to register client: ") + e.base().what()
              );
          }
        );
    }
    catch (...)
    {
        respondError(
          req, sharedCb, "DB_CONNECTION_ERROR", "registerClient: database connection error"
        );
    }
}

}  // namespace fulla::drogon::services
