#include <fulla/drogon/controllers/TokenEndpointController.h>
#include <fulla/drogon/adapters/DrogonAuditSink.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/storage/postgres/ClientOwnersRepository.h>
#include <fulla/drogon/validation/RuleSet.h>
#include <fulla/drogon/error/OAuth2ErrorHandler.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/drogon/utils/ScopeChecker.h>
#include <fulla/oauth2/model/Client.h>
#include <fulla/common/utils/RateLimiter.h>
#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <sstream>

#include <fulla/storage/postgres/models/Oauth2DeviceCodes.h>

using namespace fulla::drogon::controllers;
using namespace fulla::drogon::observability::openapi;
using namespace ::drogon::orm;

namespace fulla::drogon::controllers
{

::OAuth2Plugin *TokenEndpointController::resolvePlugin() const
{
    return plugin_ ? plugin_ : ::drogon::app().getPlugin<::OAuth2Plugin>();
}

void TokenEndpointController::initApiDocs()
{
    // Explicit, order-independent registration (replaces the former file-scope
    // global object whose constructor side-effect registered these docs at
    // static-init time -> cross-TU SIOF, defect 1.1). Callers invoke this during
    // startup (plugin initAndStart / server bootstrap). A function-local
    // call_once flag makes registration happen exactly once even if invoked from
    // several call sites, so endpoints are never registered twice.
    static std::once_flag docsOnce;
    std::call_once(docsOnce, [] { initApiDocsImpl(); });
}

void TokenEndpointController::initApiDocsImpl()
{
    // Token endpoint
    {
        Json::Value successExample;
        successExample["access_token"] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...";
        successExample["token_type"] = "Bearer";
        successExample["expires_in"] = 3600;
        successExample["refresh_token"] = "ref_123456789";
        successExample["scope"] = "openid profile";

        Json::Value errorExample;
        errorExample["error"] = "invalid_grant";
        errorExample["error_description"] = "Invalid authorization code";

        fulla::drogon::observability::openapi::EndpointInfo tokenEndpoint;
        tokenEndpoint.path = "/oauth2/token";
        tokenEndpoint.method = "POST";
        tokenEndpoint.summary = "Exchange authorization code for access token";
        tokenEndpoint.description =
          "OAuth2 token endpoint - exchanges authorization "
          "code or refresh token for access token.";
        tokenEndpoint.tags = {"OAuth2", "Token"};

        fulla::drogon::observability::openapi::ParameterInfo grantTypeParam;
        grantTypeParam.name = "grant_type";
        grantTypeParam.description = "Type of grant being requested";
        grantTypeParam.type = fulla::drogon::observability::openapi::ParameterType::STRING;
        grantTypeParam.location =
          fulla::drogon::observability::openapi::ParameterLocation::QUERY;
        grantTypeParam.required = true;
        grantTypeParam.enumValues = "authorization_code,refresh_token,client_credentials";

        fulla::drogon::observability::openapi::ParameterInfo codeParam;
        codeParam.name = "code";
        codeParam.description = "Authorization code (required for grant_type=authorization_code)";
        codeParam.type = fulla::drogon::observability::openapi::ParameterType::STRING;
        codeParam.location = fulla::drogon::observability::openapi::ParameterLocation::QUERY;
        codeParam.required = false;

        fulla::drogon::observability::openapi::ParameterInfo refreshParam;
        refreshParam.name = "refresh_token";
        refreshParam.description = "Refresh token (required for grant_type=refresh_token)";
        refreshParam.type = fulla::drogon::observability::openapi::ParameterType::STRING;
        refreshParam.location = fulla::drogon::observability::openapi::ParameterLocation::QUERY;
        refreshParam.required = false;

        fulla::drogon::observability::openapi::ParameterInfo clientIdParam;
        clientIdParam.name = "client_id";
        clientIdParam.description = "Client identifier (required)";
        clientIdParam.type = fulla::drogon::observability::openapi::ParameterType::STRING;
        clientIdParam.location =
          fulla::drogon::observability::openapi::ParameterLocation::QUERY;
        clientIdParam.required = true;

        fulla::drogon::observability::openapi::ParameterInfo clientSecretParam;
        clientSecretParam.name = "client_secret";
        clientSecretParam.description =
          "Client secret (required for CONFIDENTIAL clients with "
          "token_endpoint_auth_method=client_secret_post; FORBIDDEN for PUBLIC "
          "clients whose method is 'none' — F-017 rejects a secret sent by a "
          "'none' client).";
        clientSecretParam.type = fulla::drogon::observability::openapi::ParameterType::STRING;
        clientSecretParam.location =
          fulla::drogon::observability::openapi::ParameterLocation::QUERY;
        clientSecretParam.required = false;

        fulla::drogon::observability::openapi::ParameterInfo redirectUriParam;
        redirectUriParam.name = "redirect_uri";
        redirectUriParam.description = "Redirect URI (required for authorization_code grant)";
        redirectUriParam.type = fulla::drogon::observability::openapi::ParameterType::STRING;
        redirectUriParam.location =
          fulla::drogon::observability::openapi::ParameterLocation::QUERY;
        redirectUriParam.required = false;

        // PKCE (RFC 7636): code_verifier required on the authorization_code
        // grant when the corresponding /oauth2/login sent a code_challenge.
        fulla::drogon::observability::openapi::ParameterInfo codeVerifierParam;
        codeVerifierParam.name = "code_verifier";
        codeVerifierParam.description =
          "PKCE code verifier (RFC 7636). Required for the authorization_code "
          "grant when the authorize step sent a code_challenge.";
        codeVerifierParam.type = fulla::drogon::observability::openapi::ParameterType::STRING;
        codeVerifierParam.location =
          fulla::drogon::observability::openapi::ParameterLocation::QUERY;
        codeVerifierParam.required = false;

        tokenEndpoint.parameters =
          {grantTypeParam,
           codeParam,
           refreshParam,
           clientIdParam,
           clientSecretParam,
           redirectUriParam,
           codeVerifierParam};
        tokenEndpoint.responses =
          {{200, "Token response with access_token and refresh_token"},
           {400, "Invalid request"},
           {401, "Authentication failed"}};
        tokenEndpoint.responseExamples = {{200, successExample}, {400, errorExample}};
        tokenEndpoint.requiresAuth = false;
        OpenApiGenerator::addEndpoint(tokenEndpoint);
    }

    // UserInfo endpoint
    {
        Json::Value successExample;
        successExample["sub"] = "1";
        successExample["name"] = "john_doe";
        successExample["email"] = "john@example.com";
        successExample["roles"] = Json::Value(Json::arrayValue);
        successExample["roles"].append("user");
        successExample["roles"].append("admin");

        Json::Value errorExample;
        errorExample["error"] = "User not found";

        fulla::drogon::observability::openapi::EndpointInfo userInfoEndpoint;
        userInfoEndpoint.path = "/oauth2/userinfo";
        userInfoEndpoint.method = "GET";
        userInfoEndpoint.summary = "Get user information";
        userInfoEndpoint.description =
          "Returns information about the authenticated user. "
          "Provides user profile data including username, email, "
          "and assigned roles according to OpenID Connect "
          "standards.";
        userInfoEndpoint.tags = {"OAuth2", "User"};
        userInfoEndpoint.parameters = {};
        userInfoEndpoint.responses =
          {{200, "User information retrieved successfully"},
           {400, "Invalid User ID format"},
           {401, "Invalid or expired access token"},
           {404, "User not found"}};
        userInfoEndpoint.responseExamples = {{200, successExample}, {404, errorExample}};
        userInfoEndpoint.requiresAuth = true;
        // #43 M1: userinfo's openid-scope + M2M-subject checks are done
        // EXCLUSIVELY in the userInfo handler (not in the registry), so the
        // handler can distinguish 401 invalid_token (M2M client: subject)
        // from 403 insufficient_scope (missing openid) per OIDC Core §5.3.
        // If the registry gated userinfo, the filter would return 403 for
        // ALL tokens lacking openid, making the handler's 401 path dead code.
        // requiredScopes intentionally left empty here.
        OpenApiGenerator::addEndpoint(userInfoEndpoint);
    }

    // Introspect endpoint
    {
        Json::Value successExample;
        successExample["active"] = true;
        successExample["client_id"] = "client_123";
        successExample["token_type"] = "Bearer";
        successExample["exp"] = 1680000000;
        successExample["sub"] = "user_456";
        successExample["scope"] = "read write";

        fulla::drogon::observability::openapi::EndpointInfo introspectEndpoint;
        introspectEndpoint.path = "/oauth2/introspect";
        introspectEndpoint.method = "POST";
        introspectEndpoint.summary = "Introspect token";
        introspectEndpoint.description =
          "RFC 7662 OAuth 2.0 Token Introspection. Returns information about a token.";
        introspectEndpoint.tags = {"OAuth2", "Token"};

        fulla::drogon::observability::openapi::ParameterInfo tokenParam;
        tokenParam.name = "token";
        tokenParam.description = "The string value of the token (required)";
        tokenParam.type = fulla::drogon::observability::openapi::ParameterType::STRING;
        tokenParam.location = fulla::drogon::observability::openapi::ParameterLocation::QUERY;
        tokenParam.required = true;

        introspectEndpoint.parameters = {tokenParam};
        introspectEndpoint.responses =
          {{200, "Token status and metadata"},
           {400, "Invalid request"},
           {401, "Authentication failed"}};
        introspectEndpoint.responseExamples = {{200, successExample}};
        introspectEndpoint.requiresAuth = true;  // Requires client credentials
        // RFC 7662 §2.1: the introspection endpoint authenticates the CLIENT
        // (client_id/client_secret, HTTP Basic or POST body) -- NOT a user
        // bearer token. Declare clientCredentialsAuth so the generated
        // OpenAPI spec does not mislead SDK consumers into sending Bearer.
        introspectEndpoint.authType =
          fulla::drogon::observability::openapi::AuthType::ClientCredentials;
        OpenApiGenerator::addEndpoint(introspectEndpoint);
    }

    // Revoke endpoint
    {
        fulla::drogon::observability::openapi::EndpointInfo revokeEndpoint;
        revokeEndpoint.path = "/oauth2/revoke";
        revokeEndpoint.method = "POST";
        revokeEndpoint.summary = "Revoke token";
        revokeEndpoint.description =
          "RFC 7009 OAuth 2.0 Token Revocation. Revokes an access or refresh token.";
        revokeEndpoint.tags = {"OAuth2", "Token"};

        fulla::drogon::observability::openapi::ParameterInfo tokenParam;
        tokenParam.name = "token";
        tokenParam.description = "The token that the client wants to get revoked (required)";
        tokenParam.type = fulla::drogon::observability::openapi::ParameterType::STRING;
        tokenParam.location = fulla::drogon::observability::openapi::ParameterLocation::QUERY;
        tokenParam.required = true;

        revokeEndpoint.parameters = {tokenParam};
        revokeEndpoint.responses =
          {{200, "Token revoked successfully or token did not exist"},
           {400, "Invalid request"},
           {401, "Authentication failed"}};
        revokeEndpoint.requiresAuth = true;  // Requires client credentials
        // RFC 7009 §2.1: same client-authentication model as introspect.
        revokeEndpoint.authType =
          fulla::drogon::observability::openapi::AuthType::ClientCredentials;
        OpenApiGenerator::addEndpoint(revokeEndpoint);
    }
}

::drogon::HttpResponsePtr TokenEndpointController::createSuccessResponse()
{
    auto resp = ::drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(::drogon::k200OK);
    // F-019 (RFC 6749 §5.1 / RFC 7009 §2.2.1): token-introspect-revoke
    // success responses MUST NOT be cached. no-store is the normative cache
    // directive; Pragma: no-cache is the HTTP/1.0 back-compat fallback.
    applyNoStoreHeaders(resp);
    return resp;
}

void TokenEndpointController::applyNoStoreHeaders(const ::drogon::HttpResponsePtr &resp)
{
    // F-019 (RFC 6749 §5.1 / RFC 7009 §2.2.1): token, introspection, and
    // revocation success responses carry credentials / token metadata and
    // MUST NOT be stored by caches. `Cache-Control: no-store` is the
    // normative directive (RFC 7234 §5.2.2.5); `Pragma: no-cache` is sent
    // as the HTTP/1.0 back-compat fallback for legacy intermediaries.
    // addHeader() appends (does not overwrite), which is correct here -- a
    // caller that already set a stricter directive keeps it.
    resp->addHeader("Cache-Control", "no-store");
    resp->addHeader("Pragma", "no-cache");
}

// ---------------------------------------------------------------------------
// F-018: rate-limiting helpers.
//
// The token / introspect / revoke / device-code-polling endpoints are the
// high-value targets for credential brute-force and token probing, so they
// share a process-wide sliding-window failure counter bucketed per
// (client_ip, client_id). After `max_failures` (default 30) inside the
// rolling window (default 60s), the bucket is throttled and subsequent
// attempts get HTTP 429 + Retry-After. ONLY failures are counted -- a
// legitimate client that eventually authenticates has its bucket cleared
// (recordRateLimitSuccess), so the integration-test suite (which makes
// many sequential SUCCESSFUL token requests) is never throttled.
// ---------------------------------------------------------------------------

std::string TokenEndpointController::rateLimitKey(
  const ::drogon::HttpRequestPtr &req,
  const std::string &clientId
)
{
    // P1-9 audit fix: the failure limiter keys on the TCP peer address by
    // default. X-Forwarded-For / X-Real-IP are client-forgeable headers —
    // trusting them unconditionally let a direct-connection attacker rotate
    // the key on every request and bypass the (ip, client_id) bucket. They
    // are honored only behind an explicit operator opt-in
    // (auth.rate_limit_trust_forwarded_for=true), which a reverse-proxy
    // deployment that correctly sets/strips these headers sets.
    static const bool trustForwarded = [] {
        const auto &cfg = ::drogon::app().getCustomConfig();
        return cfg.isMember("auth") &&
               cfg.isMember("rate_limit_trust_forwarded_for") &&
               cfg["auth"]["rate_limit_trust_forwarded_for"].asBool();
    }();
    std::string ip;
    if (trustForwarded)
    {
        ip = req->getHeader("X-Forwarded-For");
        if (ip.empty())
            ip = req->getHeader("X-Real-IP");
    }
    if (ip.empty())
        ip = req->getPeerAddr().toIp();
    // client_id may be empty (malformed request) -- still bucket on IP alone
    // so a brute-forcer who omits client_id is throttled per source IP. The
    // composite key keeps different clients on the same IP independent.
    return ip + "|" + (clientId.empty() ? std::string{"-"} : clientId);
}

::drogon::HttpResponsePtr TokenEndpointController::checkRateLimited(
  const ::drogon::HttpRequestPtr &req,
  const std::string &clientId
)
{
    auto key = rateLimitKey(req, clientId);
    auto retry = fulla::common::utils::RateLimiter::instance().checkThrottled(key);
    if (retry.count() <= 0)
        return nullptr;
    // RFC 6749 §5.2 has no rate-limit error code, so the response is HTTP 429
    // with an OAuth2-style {error, error_description} body (error=
    // "invalid_request" is the closest §5.2 bucket for a malformed-traffic
    // rejection) plus the standard Retry-After header.
    Json::Value body;
    body["error"] = "invalid_request";
    body["error_description"] = "Too many failed attempts; please retry later";
    auto resp = ::drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(::drogon::k429TooManyRequests);
    resp->addHeader("Retry-After", std::to_string(retry.count()));
    // 429 responses must not be cached either.
    applyNoStoreHeaders(resp);
    return resp;
}

void TokenEndpointController::recordRateLimitSuccess(
  const ::drogon::HttpRequestPtr &req,
  const std::string &clientId
)
{
    fulla::common::utils::RateLimiter::instance().recordSuccess(rateLimitKey(req, clientId));
}

void TokenEndpointController::recordRateLimitFailure(
  const ::drogon::HttpRequestPtr &req,
  const std::string &clientId
)
{
    fulla::common::utils::RateLimiter::instance().recordFailure(rateLimitKey(req, clientId));
}

ClientCredentials TokenEndpointController::extractClientCredentials(
  const ::drogon::HttpRequestPtr &req
)
{
    std::string clientId, clientSecret, authScheme;

    // Prefer HTTP Basic Auth
    auto authHeader = req->getHeader("Authorization");
    if (!authHeader.empty() && authHeader.find("Basic ") == 0)
    {
        authScheme = "Basic";
        auto basicAuth = authHeader.substr(6);
        try
        {
            auto decoded = ::drogon::utils::base64Decode(basicAuth);
            auto colonPos = decoded.find(':');
            if (colonPos != std::string::npos)
            {
                clientId = decoded.substr(0, colonPos);
                clientSecret = decoded.substr(colonPos + 1);
            }
        }
        catch (...)
        {
            LOG_ERROR << "Failed to decode Basic Auth header";
        }
    }
    else
    {
        // Fallback to POST body
        clientId = req->getParameter("client_id");
        clientSecret = req->getParameter("client_secret");
    }

    return {clientId, clientSecret, authScheme};
}

std::string TokenEndpointController::enforceClientAuthMethod(
  const std::string &declaredMethod,
  const ClientCredentials &creds,
  bool secretInBody
)
{
    // F-017 (RFC 7591 §2 / RFC 6749 §3.2.1): enforce the client's declared
    // token-endpoint auth method. NULL/empty preserves the legacy lenient
    // Basic->body fallback (the historical behavior). Explicit values:
    //   client_secret_basic -> Authorization: Basic ONLY (reject body secret)
    //   client_secret_post  -> body client_secret ONLY (reject Basic header)
    //   none                -> PUBLIC client; reject ANY client_secret
    if (declaredMethod.empty())
        return "";  // legacy: no enforcement

    if (declaredMethod == "none")
    {
        if (!creds.clientSecret.empty())
            return "client declared token_endpoint_auth_method=none but supplied a client_secret";
        return "";
    }
    if (declaredMethod == "client_secret_basic")
    {
        // The secret MUST arrive via the Basic header (authScheme == "Basic"),
        // not in the POST body.
        if (creds.authScheme != "Basic")
            return "client requires token_endpoint_auth_method=client_secret_basic (HTTP Basic)";
        if (secretInBody)
            return "client_secret must not be sent in the body for client_secret_basic clients";
        return "";
    }
    if (declaredMethod == "client_secret_post")
    {
        // The secret MUST arrive in the POST body, not the Basic header.
        if (creds.authScheme == "Basic")
            return "client requires token_endpoint_auth_method=client_secret_post (body)";
        return "";
    }
    // P2-14 audit fix: an UNRECOGNIZED declared value previously fell
    // through leniently, so a malformed DB row (e.g. "none " with trailing
    // space) silently disabled the whole method-binding policy. Fail
    // closed: only the three known values pass.
    return "client declared unrecognized token_endpoint_auth_method '" + declaredMethod + "'";
}

void TokenEndpointController::introspect(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    LOG_DEBUG << "Token introspection requested";

    // Extract client credentials
    auto credentials = extractClientCredentials(req);
    auto clientId = credentials.clientId;
    auto clientSecret = credentials.clientSecret;
    auto authScheme = credentials.authScheme;

    if (clientId.empty() || clientSecret.empty())
    {
        fulla::common::error::OAuth2ErrorHandler::sendErrorResponse(
          std::move(callback), "invalid_client", "Client authentication required", "", authScheme
        );
        return;
    }

    // F-018: rate-limit gate (per (ip, client_id)). Consulted after the
    // client-credentials extraction so the bucket is accurate. A 429
    // short-circuits here; the throttled attempt itself is not counted.
    if (auto rlResp = checkRateLimited(req, clientId))
    {
        callback(rlResp);
        return;
    }

    // F-018: wrap the callback so the final response status drives success /
    // failure accounting (2xx clears the bucket; 4xx/5xx records a failure).
    // Wrapped in std::function so all downstream captures keep seeing a
    // std::function (matches the original `callback` type the grant branches
    // already expect, and lets the existing nested-lambda patterns compile
    // unchanged).
    std::function<void(const ::drogon::HttpResponsePtr &)> rateAwareCallback =
      [req, clientId, originalCallback = std::move(callback)](
        const ::drogon::HttpResponsePtr &resp) mutable {
          if (resp)
          {
              auto code = resp->getStatusCode();
              if (code >= ::drogon::k200OK && code < ::drogon::k300MultipleChoices)
                  recordRateLimitSuccess(req, clientId);
              else
                  recordRateLimitFailure(req, clientId);
          }
          originalCallback(resp);
      };

    // Get OAuth2 plugin
    auto plugin = resolvePlugin();
    if (!plugin)
    {
        fulla::common::error::OAuth2ErrorHandler::sendErrorResponse(
          std::move(rateAwareCallback), "server_error", "OAuth2 plugin not available"
        );
        return;
    }

    // Validate request parameters
    auto validationErrors = fulla::drogon::validation::RuleSet::oauth2Introspect(req);
    if (!validationErrors.empty())
    {
        fulla::common::error::OAuth2ErrorHandler::sendErrorResponse(
          std::move(rateAwareCallback), "invalid_request", validationErrors[0]
        );
        return;
    }

    // Extract token
    std::string token = req->getParameter("token");

    // F-017: look up the client to enforce its declared token-endpoint auth
    // method before authenticating. getClient is async; the actual
    // validateClient runs in the continuation when enforcement passes.
    bool secretInBody = req->getParameters().count("client_secret") > 0;
    plugin->getClient(
      clientId,
      [plugin, token, clientId, clientSecret, authScheme, secretInBody, credentials, callback = std::move(rateAwareCallback)](
        std::optional<fulla::oauth2::model::OAuth2Client> client
      ) mutable {
          // F-017: enforce the declared auth method. Only enforce when the
          // client was found and has an explicit method (NULL/empty preserves
          // the legacy lenient fallback).
          if (client)
          {
              std::string methodErr = enforceClientAuthMethod(
                client->tokenEndpointAuthMethod, credentials, secretInBody
              );
              if (!methodErr.empty())
              {
                  if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                      m->incrementCounter(
                        "oauth2_introspect_errors_total",
                        fulla::common::ports::MetricLabels{
                          {"client_id", clientId}, {"error", "invalid_client"}
                        }
                      );
                  fulla::common::error::OAuth2ErrorHandler::sendErrorResponse(
                    [callback = std::move(callback)](const ::drogon::HttpResponsePtr &r) { callback(r); },
                    "invalid_client",
                    methodErr,
                    "",
                    authScheme
                  );
                  return;
              }
          }
          // Authenticate client
          plugin->validateClient(
            clientId,
            clientSecret,
            [plugin, token, clientId, authScheme, callback = std::move(callback)](bool valid) mutable {
                if (!valid)
                {
                    if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                        m->incrementCounter(
                          "oauth2_introspect_errors_total",
                          fulla::common::ports::MetricLabels{
                            {"client_id", clientId}, {"error", "invalid_client"}
                          }
                        );
                    fulla::common::error::OAuth2ErrorHandler::sendErrorResponse(
                      std::move(callback),
                      "invalid_client",
                      "Client authentication failed",
                      "",
                authScheme
              );
              return;
          }

          // Introspect token
          plugin->introspectToken(
            token,
            [clientId, callback = std::move(callback)](
              std::optional<fulla::oauth2::model::TokenIntrospection> introspection
            ) mutable {
                if (!introspection)
                {
                    // Token not found or invalid
                    if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                        m->incrementCounter(
                          "oauth2_introspect_requests_total",
                          fulla::common::ports::MetricLabels{{"client_id", clientId}}
                        );

                    Json::Value response;
                    response["active"] = false;
                    auto resp = ::drogon::HttpResponse::newHttpJsonResponse(response);
                    resp->setStatusCode(::drogon::k200OK);
                    // F-019: introspection responses must not be cached even
                    // when active=false (RFC 7667 §2.2 + RFC 7009 §2.2.1).
                    applyNoStoreHeaders(resp);
                    callback(resp);
                    return;
                }

                // Token is active, return full metadata
                if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                    m->incrementCounter(
                      "oauth2_introspect_requests_total",
                      fulla::common::ports::MetricLabels{{"client_id", clientId}}
                    );

                Json::Value response;
                response["active"] = introspection->active;
                response["client_id"] = introspection->clientId;
                response["token_type"] = "Bearer";

                if (introspection->exp > 0)
                {
                    response["exp"] = static_cast<Json::Int64>(introspection->exp);
                }
                if (introspection->iat > 0)
                {
                    response["iat"] = static_cast<Json::Int64>(introspection->iat);
                }
                if (introspection->nbf > 0)
                {
                    response["nbf"] = static_cast<Json::Int64>(introspection->nbf);
                }
                if (!introspection->sub.empty())
                {
                    response["sub"] = introspection->sub;
                }
                // v1.5.0 M1 (design §2.1 item 4): the token's org binding.
                if (introspection->orgId.has_value())
                {
                    response["org_id"] = static_cast<Json::Int64>(*introspection->orgId);
                }
                if (!introspection->aud.empty())
                {
                    response["aud"] = introspection->aud;
                }
                if (!introspection->iss.empty())
                {
                    response["iss"] = introspection->iss;
                }
                else
                {
                    // F-016: backfill from the configured issuer when the
                    // storage row carries none (legacy rows / backends that
                    // never persisted issuer), so introspection iss is always
                    // byte-identical to the discovery document's issuer.
                    const auto &cfgIssuer =
                      ::drogon::app().getPlugin<::OAuth2Plugin>()->getIssuer();
                    if (!cfgIssuer.empty())
                        response["iss"] = cfgIssuer;
                }
                if (!introspection->scope.empty())
                {
                    response["scope"] = introspection->scope;
                }

                auto resp = ::drogon::HttpResponse::newHttpJsonResponse(response);
                resp->setStatusCode(::drogon::k200OK);
                // F-019 (RFC 7667 §2.2): introspection responses carry token
                // metadata and MUST NOT be cached.
                applyNoStoreHeaders(resp);
                callback(resp);
            }
          );
          }  // close validateClient callback
      );  // close validateClient call
      }  // close getClient callback
    );  // close getClient call
}

void TokenEndpointController::revoke(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    LOG_DEBUG << "Token revocation requested";

    // Extract client credentials
    auto credentials = extractClientCredentials(req);
    auto clientId = credentials.clientId;
    auto clientSecret = credentials.clientSecret;
    auto authScheme = credentials.authScheme;

    // RFC 7009 §2.1: a client_id is always required so the server can identify
    // the caller and enforce token ownership. A client_secret is required ONLY
    // for CONFIDENTIAL clients — PUBLIC clients (token_endpoint_auth_method=
    // 'none') are explicitly exempted from client authentication at the
    // revocation endpoint ("if the client is a public client, then it does not
    // authenticate"). The client-type check happens below (after the client
    // lookup), so here we only reject a completely missing client_id.
    if (clientId.empty())
    {
        fulla::common::error::OAuth2ErrorHandler::sendErrorResponse(
          std::move(callback), "invalid_client", "Client authentication required", "", authScheme
        );
        return;
    }

    // F-018: rate-limit gate (per (ip, client_id)). Consulted after the
    // client-credentials extraction so the bucket is accurate.
    if (auto rlResp = checkRateLimited(req, clientId))
    {
        callback(rlResp);
        return;
    }

    // F-018: wrap the callback so the final response status drives success /
    // failure accounting (2xx clears the bucket; 4xx/5xx records a failure).
    // Wrapped in std::function (see introspect for the same rationale).
    std::function<void(const ::drogon::HttpResponsePtr &)> rateAwareCallback =
      [req, clientId, originalCallback = std::move(callback)](
        const ::drogon::HttpResponsePtr &resp) mutable {
          if (resp)
          {
              auto code = resp->getStatusCode();
              if (code >= ::drogon::k200OK && code < ::drogon::k300MultipleChoices)
                  recordRateLimitSuccess(req, clientId);
              else
                  recordRateLimitFailure(req, clientId);
          }
          originalCallback(resp);
      };

    // Get OAuth2 plugin
    auto plugin = resolvePlugin();
    if (!plugin)
    {
        fulla::common::error::OAuth2ErrorHandler::sendErrorResponse(
          std::move(rateAwareCallback), "server_error", "OAuth2 plugin not available"
        );
        return;
    }

    // Validate request parameters
    auto validationErrors = fulla::drogon::validation::RuleSet::oauth2Revoke(req);
    if (!validationErrors.empty())
    {
        fulla::common::error::OAuth2ErrorHandler::sendErrorResponse(
          std::move(rateAwareCallback), "invalid_request", validationErrors[0]
        );
        return;
    }

    // Extract token
    std::string token = req->getParameter("token");

    // F-017: look up the client to enforce its declared token-endpoint auth
    // method before authenticating (same wrap pattern as introspect).
    bool secretInBody = req->getParameters().count("client_secret") > 0;
    plugin->getClient(
      clientId,
      [plugin, token, clientId, clientSecret, authScheme, secretInBody, credentials, callback = std::move(rateAwareCallback)](
        std::optional<fulla::oauth2::model::OAuth2Client> client
      ) mutable {
          if (client)
          {
              std::string methodErr = enforceClientAuthMethod(
                client->tokenEndpointAuthMethod, credentials, secretInBody
              );
              if (!methodErr.empty())
              {
                  if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                      m->incrementCounter(
                        "oauth2_revocation_errors_total",
                        fulla::common::ports::MetricLabels{
                          {"client_id", clientId}, {"error", "invalid_client"}
                        }
                      );
                  fulla::common::error::OAuth2ErrorHandler::sendErrorResponse(
                    [callback = std::move(callback)](const ::drogon::HttpResponsePtr &r) { callback(r); },
                    "invalid_client",
                    methodErr,
                    "",
                    authScheme
                  );
                  return;
              }
          }
          // Authenticate client
          plugin->validateClient(
            clientId,
            clientSecret,
            [plugin, token, clientId, authScheme, callback = std::move(callback)](bool valid) mutable {
                if (!valid)
                {
                    if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                        m->incrementCounter(
                          "oauth2_revocation_errors_total",
                          fulla::common::ports::MetricLabels{
                            {"client_id", clientId}, {"error", "invalid_client"}
                          }
                        );
                    fulla::common::error::OAuth2ErrorHandler::sendErrorResponse(
                      std::move(callback),
                      "invalid_client",
                      "Client authentication failed",
                      "",
                      authScheme
                    );
                    return;
                }

                // Check token ownership (permission control)
                plugin->introspectToken(
            token,
            [plugin, token, clientId, callback = std::move(callback)](
              std::optional<fulla::oauth2::model::TokenIntrospection> introspection
            ) mutable {
                if (!introspection || !introspection->active)
                {
                    // Token doesn't exist or inactive - return success per RFC 7009
                    // (prevents token probing attacks)
                    if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                        m->incrementCounter(
                          "oauth2_revocation_requests_total",
                          fulla::common::ports::MetricLabels{{"client_id", clientId}}
                        );
                    callback(createSuccessResponse());
                    return;
                }

                // Check permission: only token owner can revoke
                if (introspection->clientId != clientId)
                {
                    if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                        m->incrementCounter(
                          "oauth2_revocation_errors_total",
                          fulla::common::ports::MetricLabels{
                            {"client_id", clientId}, {"error", "unauthorized_client"}
                          }
                        );
                    fulla::common::error::OAuth2ErrorHandler::sendErrorResponse(
                      std::move(callback),
                      "unauthorized_client",
                      "This client is not allowed to revoke the token"
                    );
                    return;
                }

                // Has permission, execute revocation. C3 (RFC 7009 §2.1): the
                // revocation endpoint must revoke ANY token type. revokeAccessToken
                // only touches oauth2_access_tokens (+ its nested refresh-token
                // cascade fires only when the access-token lookup hits). A token
                // presented here may be a refresh token; revokeAccessToken is a
                // silent no-op for those. Follow it with revokeRefreshToken so a
                // refresh token is actually revoked. One of the two is always a
                // no-op (the token is one type or the other), and RFC 7009 §2.2
                // mandates 200 regardless.
                plugin->revokeAccessToken(
                  token, clientId, [plugin, clientId, token, callback = std::move(callback)]() mutable {
                      plugin->revokeRefreshToken(token, [clientId, token, callback = std::move(callback)]() mutable {
                          ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
                            ::drogon::app().getPlugin<::OAuth2Plugin>()->getAuditSink(),
                            "token_revoked",
                            "success",
                            nullptr,
                            clientId,
                            "token",
                            token
                          );
                          if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                              m->incrementCounter(
                                "oauth2_revocation_requests_total",
                                fulla::common::ports::MetricLabels{{"client_id", clientId}}
                              );
                          callback(createSuccessResponse());
                      });
                  }
                );
            }
          );
          }  // close validateClient callback
      );  // close validateClient call
      }  // close getClient callback
    );  // close getClient call
}

void TokenEndpointController::token(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    // Use ValidatorHelper for consistent validation
    auto errors = fulla::drogon::validation::RuleSet::oauth2Token(req);

    // F-008 (RFC 6749 §5.2): /oauth2/token is a protocol endpoint, so
    // validation failures MUST be RFC 6749 error envelopes
    // (error: invalid_request), not the application JSON Error Envelope
    // emitted by HttpResponder.
    if (!errors.empty())
    {
        fulla::common::error::OAuth2ErrorHandler::sendErrorResponse(
          std::move(callback), "invalid_request", errors[0]
        );
        if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
            m->incrementCounter(
              "oauth2_requests_total",
              fulla::common::ports::MetricLabels{{"endpoint", "token"}},
              static_cast<double>(400)
            );
        return;
    }

    auto plugin = resolvePlugin();
    if (!plugin)
    {
        auto resp = ::drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(::drogon::k500InternalServerError);
        resp->setBody("OAuth2 Plugin not loaded");
        callback(resp);
        return;
    }

    std::string grantType, code, redirectUri, clientId, clientSecret;
    std::string refreshToken;
    std::string codeVerifier;

    std::string authHeader = req->getHeader("Authorization");
    if (!authHeader.empty() && authHeader.substr(0, 6) == "Basic ")
    {
        LOG_DEBUG << "Token endpoint: Attempting HTTP Basic Authentication";
        try
        {
            std::string decoded = ::drogon::utils::base64Decode(authHeader.substr(6));
            size_t colonPos = decoded.find(':');
            if (colonPos != std::string::npos)
            {
                clientId = decoded.substr(0, colonPos);
                clientSecret = decoded.substr(colonPos + 1);
                LOG_DEBUG << "Token endpoint: Parsed Basic Auth for client_id=" << clientId;
            }
            else
            {
                LOG_WARN << "Token endpoint: Invalid Basic Auth format (missing colon)";
            }
        }
        catch (const std::exception &e)
        {
            LOG_WARN << "Token endpoint: Base64 decode failed - " << e.what();
        }
    }

    if (clientId.empty() || req->method() == ::drogon::Post)
    {
        auto params = req->getParameters();
        if (clientId.empty())
            clientId = params["client_id"];
        if (clientSecret.empty())
            clientSecret = params["client_secret"];
        grantType = params["grant_type"];
        code = params["code"];
        redirectUri = params["redirect_uri"];
        refreshToken = params["refresh_token"];
        codeVerifier = params["code_verifier"];
    }
    else
    {
        if (clientId.empty())
            clientId = req->getParameter("client_id");
        if (clientSecret.empty())
            clientSecret = req->getParameter("client_secret");
        grantType = req->getParameter("grant_type");
        code = req->getParameter("code");
        redirectUri = req->getParameter("redirect_uri");
        refreshToken = req->getParameter("refresh_token");
        codeVerifier = req->getParameter("code_verifier");
    }

    // F-017 (RFC 7591 §2 / RFC 6749 §3.2.1): enforce the client's declared
    // token-endpoint auth method before dispatching any grant. The whole
    // grant-type dispatch below is wrapped in a std::function so the getClient
    // gate can run enforcement, then call dispatchGrant() unchanged on
    // success. clientId may legitimately be empty here for some flows (e.g.
    // device_code discovers it from the body); in that case skip enforcement
    // (the branches that need client auth already re-resolve the client).
    //
    // F-018: rate-limit gate. Consulted AFTER clientId resolution so the
    // (ip, client_id) bucket is accurate; the validation gate above already
    // rejected structurally-invalid requests. A 429 short-circuits here --
    // no grant work is done, so neither success nor failure is recorded for
    // the throttled attempt itself (the bucket stays over threshold from the
    // prior failures that put it there).
    if (auto rlResp = checkRateLimited(req, clientId))
    {
        callback(rlResp);
        return;
    }
    // F-018: wrap the callback so the FINAL response status determines
    // success vs failure accounting. A 2xx response clears the (ip,
    // client_id) failure bucket; any 4xx/5xx response records a failure.
    // This is the single chokepoint -- every grant branch (authorization_
    // code, refresh, client_credentials, device_code) funnels its response
    // through `sharedCb`, so the accounting is uniform without per-branch
    // instrumentation. The validation-gate early-returns above are NOT
    // rate-counted (they fire before this wrap) -- structurally malformed
    // requests are not auth failures and counting them would let a single
    // buggy client throttle legitimate users on a shared IP.
    auto rlReq = req;
    auto rlClientId = clientId;
    auto rateAwareCallback =
      [rlReq, rlClientId, originalCallback = std::move(callback)](
        const ::drogon::HttpResponsePtr &resp) mutable {
          if (resp)
          {
              auto code = resp->getStatusCode();
              if (code >= ::drogon::k200OK && code < ::drogon::k300MultipleChoices)
                  recordRateLimitSuccess(rlReq, rlClientId);
              else
                  recordRateLimitFailure(rlReq, rlClientId);
          }
          originalCallback(resp);
      };
    auto dispatchCb = std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
      std::move(rateAwareCallback)
    );
    // F-017: dispatchGrant is held via shared_ptr so the getClient callback
    // can capture it by value (the callback may run AFTER this function
    // returns, so a `&` reference would dangle). It is assigned BEFORE the
    // getClient call because in-memory repositories invoke the getClient
    // callback inline (synchronously) -- assigning it after would leave an
    // empty std::function that the inline callback dereferences, aborting.
    auto dispatchGrant = std::make_shared<std::function<void()>>();

    *dispatchGrant = [plugin,
                     req,
                     clientId,
                     clientSecret,
                     grantType,
                     code,
                     redirectUri,
                     refreshToken,
                     codeVerifier,
                     dispatchCb,
                     authHeader]() mutable {
        // Re-bind `callback` for the existing dispatch body (unchanged below).
        std::function<void(const ::drogon::HttpResponsePtr &)> callback =
          [dispatchCb](const ::drogon::HttpResponsePtr &r) { (*dispatchCb)(r); };

        if (grantType == "authorization_code")
        {
        plugin->exchangeCodeForToken(
          code,
          clientId,
          clientSecret,
          redirectUri,
          codeVerifier,
          [callback = std::move(callback)](const Json::Value &result) {
              if (result.isMember("error"))
              {
                  auto resp = ::drogon::HttpResponse::newHttpJsonResponse(result);
                  std::string errorCode = result.get("error", "").asString();
                  ::drogon::HttpStatusCode statusCode =
                    fulla::common::error::OAuth2ErrorHandler::getHttpStatusCode(errorCode);
                  resp->setStatusCode(statusCode);
                  if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                      m->incrementCounter(
                        "oauth2_requests_total",
                        fulla::common::ports::MetricLabels{{"endpoint", "token"}},
                        static_cast<double>(static_cast<int>(statusCode))
                      );
                  callback(resp);
                  return;
              }

              auto resp = ::drogon::HttpResponse::newHttpJsonResponse(result);
              if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                  m->incrementCounter(
                    "oauth2_requests_total",
                    fulla::common::ports::MetricLabels{{"endpoint", "token"}},
                    static_cast<double>(200)
                  );
              if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                  m->setGauge(
                    "oauth2_active_tokens",
                    fulla::common::ports::MetricLabels{},
                    static_cast<double>(1)
                  );
              // F-019 (RFC 6749 §5.1): token responses MUST NOT be cached.
              applyNoStoreHeaders(resp);
              callback(resp);
          }
        );
    }
    else if (grantType == "refresh_token")
    {
        // F-003 (RFC 6749 §3.2.1 / §6): the token endpoint MUST authenticate
        // the client on the refresh_token grant as well. Previously this
        // branch relied solely on TokenService's stored client_id string
        // comparison, so anyone holding a leaked refresh token (plus the
        // non-secret client_id) could mint fresh access tokens.
        // - CONFIDENTIAL clients: require + validate the client_secret.
        // - PUBLIC clients: client_id existence check only (RFC 6749 §10.2).
        std::string authScheme =
          (!authHeader.empty() && authHeader.substr(0, 6) == "Basic ") ? "Basic" : "";
        auto refreshCb = std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
          std::move(callback)
        );

        plugin->getClient(
          clientId,
          [plugin, clientId, clientSecret, refreshToken, authScheme, refreshCb](
            std::optional<fulla::oauth2::model::OAuth2Client> client
          ) {
              auto respondInvalidClient = [refreshCb, authScheme](const std::string &desc) {
                  fulla::common::error::OAuth2ErrorHandler::sendErrorResponse(
                    [refreshCb](const ::drogon::HttpResponsePtr &r) { (*refreshCb)(r); },
                    "invalid_client",
                    desc,
                    "",
                    authScheme
                  );
              };

              if (!client)
              {
                  respondInvalidClient("Unknown client_id");
                  return;
              }

              auto proceedRefresh = [plugin, clientId, refreshToken, refreshCb]() {
                  plugin->refreshAccessToken(
                    refreshToken, clientId, [refreshCb](const Json::Value &result) {
                        if (result.isMember("error"))
                        {
                            auto resp = ::drogon::HttpResponse::newHttpJsonResponse(result);
                            std::string errorCode = result.get("error", "").asString();
                            ::drogon::HttpStatusCode statusCode =
                              fulla::common::error::OAuth2ErrorHandler::getHttpStatusCode(
                                errorCode
                              );
                            resp->setStatusCode(statusCode);
                            if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                                m->incrementCounter(
                                  "oauth2_requests_total",
                                  fulla::common::ports::MetricLabels{{"endpoint", "token"}},
                                  static_cast<double>(static_cast<int>(statusCode))
                                );
                            (*refreshCb)(resp);
                            return;
                        }

                        auto resp = ::drogon::HttpResponse::newHttpJsonResponse(result);
                        if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                            m->incrementCounter(
                              "oauth2_requests_total",
                              fulla::common::ports::MetricLabels{{"endpoint", "token"}},
                              static_cast<double>(200)
                            );
                        // F-019 (RFC 6749 §5.1): token responses MUST NOT be cached.
                        applyNoStoreHeaders(resp);
                        (*refreshCb)(resp);
                    }
                  );
              };

              if (client->clientType == fulla::oauth2::model::ClientType::CONFIDENTIAL)
              {
                  if (clientSecret.empty())
                  {
                      respondInvalidClient(
                        "Client authentication required for refresh_token grant"
                      );
                      return;
                  }
                  plugin->validateClient(
                    clientId,
                    clientSecret,
                    [proceedRefresh, respondInvalidClient](bool valid) {
                        if (!valid)
                        {
                            respondInvalidClient("Client authentication failed");
                            return;
                        }
                        proceedRefresh();
                    }
                  );
                  return;
              }

              // PUBLIC client: existence verified via getClient above.
              proceedRefresh();
          }
        );
    }
    else if (grantType == "client_credentials")
    {
        // Client Credentials Grant (RFC 6749 Section 4.4)
        // Only CONFIDENTIAL clients can use this grant type
        if (clientId.empty() || clientSecret.empty())
        {
            Json::Value error;
            error["error"] = "invalid_client";
            error["error_description"] =
              "Client authentication required for client_credentials grant";
            auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(::drogon::k401Unauthorized);
            callback(resp);
            return;
        }

        auto sharedCb = std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
          std::move(callback)
        );

        plugin
          ->validateClient(clientId, clientSecret, [plugin, clientId, req, sharedCb](bool valid) {
              if (!valid)
              {
                  Json::Value error;
                  error["error"] = "invalid_client";
                  error["error_description"] = "Client authentication failed";
                  auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                  resp->setStatusCode(::drogon::k401Unauthorized);
                  (*sharedCb)(resp);
                  return;
              }

              // Verify client is CONFIDENTIAL (PUBLIC clients cannot use client_credentials)
              // Phase 4.3: route through plugin->getClient (NEW IClientRepository
              // via the bridge) instead of getStorage()->getClient. The plugin
              // outlives this request (it is a config-driven singleton), so no
              // shared_ptr capture of storage is needed across this async hop.
              plugin->getClient(
                clientId,
                [plugin, clientId, req, sharedCb](
                  std::optional<fulla::oauth2::model::OAuth2Client> client
                ) {
                    if (!client)
                    {
                        Json::Value error;
                        error["error"] = "invalid_client";
                        auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                        resp->setStatusCode(::drogon::k401Unauthorized);
                        (*sharedCb)(resp);
                        return;
                    }

                    if (client->clientType == fulla::oauth2::model::ClientType::PUBLIC)
                    {
                        Json::Value error;
                        error["error"] = "unauthorized_client";
                        error["error_description"] =
                          "Public clients cannot use client_credentials grant";
                        auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                        resp->setStatusCode(::drogon::k401Unauthorized);
                        (*sharedCb)(resp);
                        return;
                    }

                    // P0 #2 (评审问题点 2, RFC 6749 §3.3): validate the requested
                    // scope against the client's registered allowlist instead of
                    // echoing it back unchecked (or hardcoding "read" as default).
                    // - requested scope exceeding the allowlist -> invalid_scope
                    // - omitted scope -> default to the full registered scope set
                    // - omitted scope + empty registration -> invalid_scope (the
                    //   server has no pre-defined default to fall back on)
                    fulla::oauth2::model::Client aggregate(*client);
                    std::string requestedScope = req->getParameter("scope");
                    std::string grantedScope;
                    if (!requestedScope.empty())
                    {
                        if (!aggregate.allowsAllScopes(requestedScope))
                        {
                            Json::Value error;
                            error["error"] = "invalid_scope";
                            error["error_description"] =
                              "Requested scope exceeds the scopes registered for this client";
                            auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                            resp->setStatusCode(::drogon::k400BadRequest);
                            (*sharedCb)(resp);
                            return;
                        }
                        grantedScope = requestedScope;
                    }
                    else
                    {
                        const auto &allowed = aggregate.allowedScopes();
                        if (allowed.empty())
                        {
                            Json::Value error;
                            error["error"] = "invalid_scope";
                            error["error_description"] =
                              "No scope requested and no default scope registered for this "
                              "client";
                            auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                            resp->setStatusCode(::drogon::k400BadRequest);
                            (*sharedCb)(resp);
                            return;
                        }
                        for (const auto &s : allowed)
                        {
                            if (!grantedScope.empty())
                                grantedScope += " ";
                            grantedScope += s;
                        }
                    }

                    // v1.5.0 M3 (design §2.3, R-M3-1): an org-owned
                    // client's CC token carries the org anchor
                    // (oauth2_access_tokens.org_id; introspection exposes
                    // it -- the M1 DTO plumbing already persists the field
                    // across PG/memory/redis storages). sub stays the
                    // client itself (V4). Personal apps carry no org
                    // context. Storage-type-guarded like the M1 resolver
                    // wiring: memory mode has no org rows and must not
                    // reach getDbClient() (Debug builds assert, not
                    // throw). The ruling's org_name JWT claim has no
                    // carrier here -- fulla access tokens are opaque
                    // (only id_tokens are JWTs) and the ruling itself
                    // caps introspection at org_id; org_name is dropped
                    // from the CC surface (PR body records this).
                    auto mintCcToken =
                      [plugin, clientId, grantedScope, sharedCb](
                          std::optional<int32_t> orgId) {
                        // Generate access token (no refresh token for client_credentials)
                        auto tokenStr = fulla::drogon::utils::generateSecureToken();
                        auto nowSecs = std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::system_clock::now().time_since_epoch()
                        )
                                         .count();
                        // P1 #6: use the configured TTL instead of a hardcoded 3600 so
                        // expiresAt and the advertised expires_in stay consistent with
                        // the real token lifetime (RFC 6749 §5.1).
                        auto accessTokenTtl = plugin->getAccessTokenTtl();

                        fulla::oauth2::model::OAuth2AccessToken token;
                        token.token = fulla::drogon::utils::hashToken(tokenStr);
                        token.clientId = clientId;
                        token.userId = "client:" + clientId;  // M2M: subject is the client itself
                        token.scope = grantedScope;
                        token.issuedAt = nowSecs;  // P2 #10: introspection iat
                        token.expiresAt = nowSecs + accessTokenTtl;
                        // F-016: M2M tokens get the configured issuer too (the
                        // controller constructs these tokens outside TokenService,
                        // so the stamp happens here via plugin->getIssuer()).
                        token.issuer = plugin->getIssuer();
                        token.orgId = orgId;  // M3 org anchor (nullopt = personal)

                        // Phase 4.3: route through plugin->saveAccessToken (NEW
                        // ITokenRepository) instead of getStorage()->saveAccessToken.
                        plugin->saveAccessToken(
                          token, [sharedCb, tokenStr, grantedScope, accessTokenTtl]() {
                              Json::Value json;
                              json["access_token"] = tokenStr;
                              json["token_type"] = "Bearer";
                              json["expires_in"] = (Json::Int64)accessTokenTtl;
                              json["scope"] = grantedScope;
                              // No refresh_token for client_credentials
                              auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                              if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                                  m->incrementCounter(
                                    "oauth2_requests_total",
                                    fulla::common::ports::MetricLabels{{"endpoint", "token"}},
                                    static_cast<double>(200)
                                  );
                              // F-019 (RFC 6749 §5.1): token responses MUST NOT be cached.
                              applyNoStoreHeaders(resp);
                              (*sharedCb)(resp);
                          }
                        );
                    };

                    ::drogon::orm::DbClientPtr ccOrgDb;
                    if (plugin->getStorageType() != "memory")
                    {
                        try
                        {
                            ccOrgDb = ::drogon::app().getDbClient();
                        }
                        catch (...)
                        {
                            ccOrgDb = nullptr;
                        }
                    }
                    if (!ccOrgDb)
                    {
                        mintCcToken(std::nullopt);
                        return;
                    }
                    auto ccOwnersRepo =
                      std::make_shared<::fulla::storage::postgres::ClientOwnersRepository>(
                        ccOrgDb
                      );
                    ccOwnersRepo->findOwnerRow(
                      clientId,
                      [ccOwnersRepo, mintCcToken = std::move(mintCcToken)](
                        const ::fulla::storage::postgres::OwnerRowLookup &o) {
                          using ::fulla::storage::postgres::LookupStatus;
                          std::optional<int32_t> orgId;
                          if (o.status == LookupStatus::Found && o.row.getOrgId() != nullptr)
                          {
                              orgId = *o.row.getOrgId();
                          }
                          // NoRow (admin-seeded client) or Error both mean
                          // "no org context": the token still issues (the
                          // org anchor is claim data, not a gate).
                          mintCcToken(orgId);
                      }
                    );
                }
              );
          });
    }
    else if (grantType == "urn:ietf:params:oauth:grant-type:device_code")
    {
        // Device Authorization Grant (RFC 8628)
        std::string deviceCode = req->getParameter("device_code");
        if (clientId.empty())
        {
            clientId = req->getParameter("client_id");
        }

        if (deviceCode.empty() || clientId.empty())
        {
            Json::Value error;
            error["error"] = "invalid_request";
            error["error_description"] = "device_code and client_id are required";
            auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(::drogon::k400BadRequest);
            if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                m->incrementCounter(
                  "oauth2_requests_total",
                  fulla::common::ports::MetricLabels{{"endpoint", "token"}},
                  static_cast<double>(400)
                );
            callback(resp);
            return;
        }

        std::string deviceCodeHash = fulla::drogon::utils::hashToken(deviceCode);

        auto dbClient = ::drogon::app().getDbClient();
        if (!dbClient)
        {
            Json::Value error;
            error["error"] = "server_error";
            error["error_description"] = "Database not available";
            auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(::drogon::k500InternalServerError);
            callback(resp);
            return;
        }

        auto sharedCb = std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
          std::move(callback)
        );

        // P1 #5 (评审问题点 5, RFC 8628 §3.4): device_code redemption previously
        // only string-matched client_id against the device-code row, skipping
        // client authentication entirely. RFC 8628 defers to RFC 6749 §3.2.1:
        // CONFIDENTIAL clients MUST authenticate at the token endpoint; PUBLIC
        // clients need only identify themselves. Branch on client_type:
        //   CONFIDENTIAL -> require a valid client_secret (validateClient)
        //   PUBLIC       -> current behavior (client_id bound to the device row)
        // The device-code lookup (Mapper::findBy + atomic consume) is the same
        // for both; wrapped in a local lambda so both paths converge on it.
        auto runDeviceCodeLookup = [plugin, sharedCb, clientId, deviceCodeHash, dbClient]() {
            Mapper<drogon_model::fulla_db::Oauth2DeviceCodes> mapper(dbClient);
            mapper.findBy(
              Criteria(
                drogon_model::fulla_db::Oauth2DeviceCodes::Cols::_device_code_hash,
                CompareOperator::EQ,
                deviceCodeHash
              ),
              [plugin, sharedCb, clientId, deviceCodeHash, dbClient](
                const std::vector<drogon_model::fulla_db::Oauth2DeviceCodes> &results
              ) {
                  if (results.empty())
                  {
                      Json::Value error;
                      error["error"] = "invalid_grant";
                      error["error_description"] = "Invalid device_code";
                      auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                      resp->setStatusCode(::drogon::k400BadRequest);
                      if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                          m->incrementCounter(
                            "oauth2_requests_total",
                            fulla::common::ports::MetricLabels{{"endpoint", "token"}},
                            static_cast<double>(400)
                          );
                      (*sharedCb)(resp);
                      return;
                  }

                  const auto &row = results[0];
                  std::string storedClientId = row.getValueOfClientId();
                  std::string status = row.getValueOfStatus();
                  int64_t expiresAt = row.getValueOfExpiresAt();
                  std::string scope = row.getValueOfScope();
                  std::string userId = row.getValueOfUserId();

                  // Verify client_id matches
                  if (storedClientId != clientId)
                  {
                      Json::Value error;
                      error["error"] = "invalid_grant";
                      error["error_description"] = "client_id mismatch";
                      auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                      resp->setStatusCode(::drogon::k400BadRequest);
                      if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                          m->incrementCounter(
                            "oauth2_requests_total",
                            fulla::common::ports::MetricLabels{{"endpoint", "token"}},
                            static_cast<double>(400)
                          );
                      (*sharedCb)(resp);
                      return;
                  }

                  // Check expiration
                  auto now = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch()
                  )
                               .count();
                  if (now >= expiresAt)
                  {
                      Json::Value error;
                      error["error"] = "expired_token";
                      error["error_description"] = "The device_code has expired";
                      auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                      resp->setStatusCode(::drogon::k400BadRequest);
                      if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                          m->incrementCounter(
                            "oauth2_requests_total",
                            fulla::common::ports::MetricLabels{{"endpoint", "token"}},
                            static_cast<double>(400)
                          );
                      (*sharedCb)(resp);
                      return;
                  }

                  // Check status
                  if (status == "pending")
                  {
                      auto respondPending = [sharedCb]() {
                          Json::Value error;
                          error["error"] = "authorization_pending";
                          error["error_description"] =
                            "The authorization request is still pending";
                          auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                          resp->setStatusCode(::drogon::k400BadRequest);
                          if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                              m->incrementCounter(
                                "oauth2_requests_total",
                                fulla::common::ports::MetricLabels{{"endpoint", "token"}},
                                static_cast<double>(400)
                              );
                          (*sharedCb)(resp);
                      };

                      // F-012 (RFC 8628 §3.5): a poll arriving sooner than
                      // interval_seconds after the previous one gets
                      // slow_down, and the server SHOULD increase the
                      // interval by 5 seconds and persist it. Every poll
                      // records last_polled_at (raw UPDATE is a db-operations
                      // exemption: read-modify-write on a polled timestamp).
                      int intervalSeconds = row.getValueOfIntervalSeconds();
                      int64_t lastPolledAt = row.getValueOfLastPolledAt();
                      bool tooFast = lastPolledAt > 0 && (now - lastPolledAt) < intervalSeconds;

                      if (tooFast)
                      {
                          int newInterval = intervalSeconds + 5;
                          auto respondSlowDown = [sharedCb, newInterval]() {
                              Json::Value error;
                              error["error"] = "slow_down";
                              error["error_description"] =
                                "Polling too frequently; increase the polling interval";
                              error["interval"] = newInterval;
                              auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                              resp->setStatusCode(::drogon::k400BadRequest);
                              if (
                                auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics()
                              )
                                  m->incrementCounter(
                                    "oauth2_requests_total",
                                    fulla::common::ports::MetricLabels{{"endpoint", "token"}},
                                    static_cast<double>(400)
                                  );
                              (*sharedCb)(resp);
                          };
                          dbClient->execSqlAsync(
                            "UPDATE oauth2_device_codes "
                            "SET interval_seconds = $2, last_polled_at = $3 "
                            "WHERE device_code_hash = $1",
                            [respondSlowDown](const ::drogon::orm::Result &) { respondSlowDown(); },
                            [respondSlowDown](const DrogonDbException &e) {
                                LOG_ERROR << "slow_down: failed to persist interval bump: "
                                          << e.base().what();
                                // Protocol response still goes out (the client
                                // backs off either way).
                                respondSlowDown();
                            },
                            deviceCodeHash,
                            newInterval,
                            static_cast<int64_t>(now)
                          );
                          return;
                      }

                      dbClient->execSqlAsync(
                        "UPDATE oauth2_device_codes SET last_polled_at = $2 "
                        "WHERE device_code_hash = $1",
                        [respondPending](const ::drogon::orm::Result &) { respondPending(); },
                        [respondPending](const DrogonDbException &e) {
                            LOG_ERROR << "device poll: failed to persist last_polled_at: "
                                      << e.base().what();
                            respondPending();
                        },
                        deviceCodeHash,
                        static_cast<int64_t>(now)
                      );
                      return;
                  }

                  if (status == "denied")
                  {
                      Json::Value error;
                      error["error"] = "access_denied";
                      error["error_description"] = "The user denied the authorization request";
                      auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                      resp->setStatusCode(::drogon::k400BadRequest);
                      if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                          m->incrementCounter(
                            "oauth2_requests_total",
                            fulla::common::ports::MetricLabels{{"endpoint", "token"}},
                            static_cast<double>(400)
                          );
                      (*sharedCb)(resp);
                      return;
                  }

                  if (status != "approved")
                  {
                      Json::Value error;
                      error["error"] = "invalid_grant";
                      error["error_description"] = "Invalid device code status";
                      auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                      resp->setStatusCode(::drogon::k400BadRequest);
                      if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                          m->incrementCounter(
                            "oauth2_requests_total",
                            fulla::common::ports::MetricLabels{{"endpoint", "token"}},
                            static_cast<double>(400)
                          );
                      (*sharedCb)(resp);
                      return;
                  }

                  // Status is "approved" -- attempt atomic consume before issuing.
                  // P1 #3: the previous flow trusted the in-memory status read and
                  // only deleted the row *after* issuance, so two concurrent
                  // redemptions of the same approved device_code could both issue
                  // tokens (race). We now atomically transition approved -> consumed
                  // and gate issuance on the affected row: a concurrent loser's
                  // UPDATE matches 0 rows and gets invalid_grant (fail-closed).
                  // Raw SQL (UPDATE ... RETURNING) is used because the Mapper cannot
                  // express a conditional atomic state transition; this is one of the
                  // raw-SQL exemptions in .claude/rules/db-operations.md
                  // (UPDATE ... RETURNING). Pattern mirrors
                  // PostgresGrantRepository::consumeAuthCode.
                  // P1 #6: use configured TTLs instead of hardcoded 3600 / 30 days.
                  auto accessTokenTtl = plugin->getAccessTokenTtl();
                  auto refreshTokenTtl = plugin->getRefreshTokenTtl();
                  try
                  {
                      dbClient->execSqlAsync(
                        "UPDATE oauth2_device_codes SET status = 'consumed' "
                        "WHERE device_code_hash = $1 AND status = 'approved' "
                        "RETURNING device_code_hash",
                        [plugin,
                         sharedCb,
                         clientId,
                         userId,
                         scope,
                         now,
                         accessTokenTtl,
                         refreshTokenTtl](const ::drogon::orm::Result &r) {
                            if (r.empty())
                            {
                                // Lost the race (or row was no longer approved): the
                                // atomic UPDATE matched nothing. Fail closed.
                                Json::Value error;
                                error["error"] = "invalid_grant";
                                error["error_description"] =
                                  "device code already consumed or no longer approved";
                                auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                                resp->setStatusCode(::drogon::k400BadRequest);
                                if (
                                  auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics()
                                )
                                    m->incrementCounter(
                                      "oauth2_requests_total",
                                      fulla::common::ports::MetricLabels{{"endpoint", "token"}},
                                      static_cast<double>(400)
                                    );
                                (*sharedCb)(resp);
                                return;
                            }

                            // Won the consume race -- safe to issue.
                            auto accessTokenStr = fulla::drogon::utils::generateSecureToken();
                            auto refreshTokenStr = fulla::drogon::utils::generateSecureToken();
                            std::string familyId =
                              fulla::drogon::utils::generateSecureToken(16);

                            fulla::oauth2::model::OAuth2AccessToken accessToken;
                            accessToken.token = fulla::drogon::utils::hashToken(accessTokenStr);
                            accessToken.clientId = clientId;
                            accessToken.userId = userId;
                            accessToken.scope = scope;
                            accessToken.issuedAt = now;
                            accessToken.expiresAt = now + accessTokenTtl;
                            // F-016: device-code tokens are constructed here
                            // (outside TokenService), stamp the issuer too.
                            accessToken.issuer = plugin->getIssuer();

                            fulla::oauth2::model::OAuth2RefreshToken refreshToken;
                            refreshToken.token =
                              fulla::drogon::utils::hashToken(refreshTokenStr);
                            refreshToken.accessToken = accessToken.token;
                            refreshToken.clientId = clientId;
                            refreshToken.userId = userId;
                            refreshToken.scope = scope;
                            refreshToken.expiresAt = now + refreshTokenTtl;
                            refreshToken.familyId = familyId;

                            // Phase 4.3: route through plugin->saveTokenPair (NEW
                            // ITokenRepository) instead of getStorage()->saveTokenPair.
                            plugin->saveTokenPair(
                              accessToken,
                              refreshToken,
                              [plugin, sharedCb, clientId, userId, accessTokenStr, refreshTokenStr, scope, accessTokenTtl](bool ok) {
                                  if (!ok)
                                  {
                                      // Persistence failed: do NOT hand out
                                      // tokens that were never stored (silent
                                      // failure -- later introspection/refresh
                                      // would all miss). Report server_error
                                      // like the other DB failure paths here.
                                      LOG_ERROR << "Device code flow: saveTokenPair failed";
                                      Json::Value error;
                                      error["error"] = "server_error";
                                      error["error_description"] = "Failed to persist token pair";
                                      auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                                      resp->setStatusCode(::drogon::k500InternalServerError);
                                      (*sharedCb)(resp);
                                      return;
                                  }
                                  Json::Value json;
                                  json["access_token"] = accessTokenStr;
                                  json["token_type"] = "Bearer";
                                  json["expires_in"] = (Json::Int64)accessTokenTtl;
                                  json["refresh_token"] = refreshTokenStr;
                                  if (!scope.empty())
                                  {
                                      json["scope"] = scope;
                                  }
                                  // F-025 (OIDC Core §5/§12): device_code grant
                                  // with an openid scope issues an id_token,
                                  // signed via the plugin helper (the device
                                  // flow constructs tokens outside TokenService,
                                  // so it cannot reuse that class's inline
                                  // signing). No nonce on device flow.
                                  if (fulla::drogon::utils::hasScope(scope, "openid"))
                                  {
                                      std::string idToken =
                                        plugin->signIdToken(userId, clientId);
                                      if (!idToken.empty())
                                          json["id_token"] = idToken;
                                  }

                                  auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                                  if (
                                    auto m =
                                      ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics()
                                  )
                                      m->incrementCounter(
                                        "oauth2_requests_total",
                                        fulla::common::ports::MetricLabels{
                                          {"endpoint", "token"}
                                        },
                                        static_cast<double>(200)
                                      );
                                  if (
                                    auto m =
                                      ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics()
                                  )
                                      m->setGauge(
                                        "oauth2_active_tokens",
                                        fulla::common::ports::MetricLabels{},
                                        static_cast<double>(1)
                                      );
                                  // F-019 (RFC 6749 §5.1): device-code token
                                  // responses MUST NOT be cached.
                                  applyNoStoreHeaders(resp);
                                  (*sharedCb)(resp);
                              }
                            );
                        },
                        [sharedCb](const ::drogon::orm::DrogonDbException &e) {
                            LOG_ERROR << "Device code atomic consume failed: " << e.base().what();
                            Json::Value error;
                            error["error"] = "server_error";
                            error["error_description"] = "Failed to consume device code";
                            auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                            resp->setStatusCode(::drogon::k500InternalServerError);
                            (*sharedCb)(resp);
                        },
                        deviceCodeHash
                      );
                  }
                  catch (const std::exception &e)
                  {
                      // db-operations.md requirement: execSqlAsync setup can throw
                      // (DbClient internal state); surface it to the caller rather
                      // than escaping into the event loop.
                      LOG_ERROR << "Device code consume setup failed: " << e.what();
                      Json::Value error;
                      error["error"] = "server_error";
                      error["error_description"] = "Failed to process device code";
                      auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                      resp->setStatusCode(::drogon::k500InternalServerError);
                      (*sharedCb)(resp);
                  }
                  catch (...)
                  {
                      Json::Value error;
                      error["error"] = "server_error";
                      error["error_description"] = "Failed to process device code";
                      auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                      resp->setStatusCode(::drogon::k500InternalServerError);
                      (*sharedCb)(resp);
                  }
              },
              [sharedCb](const ::drogon::orm::DrogonDbException &e) {
                  LOG_ERROR << "Device code lookup failed: " << e.base().what();
                  Json::Value error;
                  error["error"] = "server_error";
                  error["error_description"] = "Failed to process device code";
                  auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                  resp->setStatusCode(::drogon::k500InternalServerError);
                  (*sharedCb)(resp);
              }
            );
        };  // end runDeviceCodeLookup

        plugin->getClient(
          clientId,
          [plugin,
           sharedCb,
           clientId,
           clientSecret,
           runDeviceCodeLookup = std::move(runDeviceCodeLookup)](
            std::optional<fulla::oauth2::model::OAuth2Client> client
          ) mutable {
              if (!client)
              {
                  Json::Value error;
                  error["error"] = "invalid_client";
                  error["error_description"] = "Client authentication failed";
                  auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                  resp->setStatusCode(::drogon::k401Unauthorized);
                  (*sharedCb)(resp);
                  return;
              }

              // PUBLIC clients: client_id alone suffices (RFC 8628 §3.4 / RFC
              // 6749 §3.2.1). Proceed straight to the device-code lookup.
              if (client->clientType == fulla::oauth2::model::ClientType::PUBLIC)
              {
                  runDeviceCodeLookup();
                  return;
              }

              // CONFIDENTIAL clients: require a valid client_secret.
              plugin->validateClient(
                clientId,
                clientSecret,
                [plugin,
                 sharedCb,
                 runDeviceCodeLookup = std::move(runDeviceCodeLookup)](bool valid) mutable {
                    if (!valid)
                    {
                        Json::Value error;
                        error["error"] = "invalid_client";
                        error["error_description"] = "Client authentication failed";
                        auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                        resp->setStatusCode(::drogon::k401Unauthorized);
                        (*sharedCb)(resp);
                        return;
                    }
                    runDeviceCodeLookup();
                }
              );
          }
        );
    }
    else
    {
        Json::Value error;
        error["error"] = "unsupported_grant_type";
        error["error_description"] =
          "Supported types: authorization_code, refresh_token, client_credentials, "
          "urn:ietf:params:oauth:grant-type:device_code";
        auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(::drogon::k400BadRequest);
        if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
            m->incrementCounter(
              "oauth2_requests_total",
              fulla::common::ports::MetricLabels{{"endpoint", "token"}},
              static_cast<double>(400)
            );
        callback(resp);
    }
    };  // close dispatchGrant lambda

    // F-017 (RFC 7591 §2 / RFC 6749 §3.2.1): enforce the client's declared
    // token-endpoint auth method, then dispatch the grant. dispatchGrant is
    // assigned above BEFORE this getClient call because in-memory repos fire
    // the callback inline (synchronously); an after-the-fact assignment would
    // leave dispatchGrant empty when the inline callback dereferences it.
    plugin->getClient(
      clientId,
      [plugin, req, clientId, clientSecret, authHeader, grantType, dispatchCb, dispatchGrant](
        std::optional<fulla::oauth2::model::OAuth2Client> client
      ) mutable {
          if (client)
          {
              ClientCredentials creds;
              creds.clientId = clientId;
              creds.clientSecret = clientSecret;
              creds.authScheme =
                (!authHeader.empty() && authHeader.substr(0, 6) == "Basic ") ? "Basic" : "";
              bool secretInBody = req->getParameters().count("client_secret") > 0;
              std::string methodErr =
                enforceClientAuthMethod(client->tokenEndpointAuthMethod, creds, secretInBody);
              if (!methodErr.empty())
              {
                  Json::Value error;
                  error["error"] = "invalid_client";
                  error["error_description"] = methodErr;
                  auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                  resp->setStatusCode(::drogon::k401Unauthorized);
                  (*dispatchCb)(resp);
                  return;
              }

              // #220 (RFC 6749 §3.2.1): enforce the client's registered grant
              // list before dispatching. Empty list = legacy row, unrestricted.
              // refresh_token is implied by an authorization_code or device_code
              // registration: those flows mint refresh tokens unconditionally
              // at issuance, so rejecting the follow-up refresh would brick a
              // legitimately issued token. client_credentials/device_code/
              // authorization_code are matched strictly against the list.
              if (!client->allowedGrantTypes.empty() && !grantType.empty())
              {
                  bool grantAllowed = false;
                  for (const auto &registered : client->allowedGrantTypes)
                  {
                      if (registered == grantType ||
                          (grantType == "refresh_token" &&
                           (registered == "authorization_code" ||
                            registered == "urn:ietf:params:oauth:grant-type:device_code")))
                      {
                          grantAllowed = true;
                          break;
                      }
                  }
                  if (!grantAllowed)
                  {
                      Json::Value error;
                      error["error"] = "unauthorized_client";
                      error["error_description"] =
                        "Client is not registered for grant_type: " + grantType;
                      auto resp = ::drogon::HttpResponse::newHttpJsonResponse(error);
                      resp->setStatusCode(
                        fulla::common::error::OAuth2ErrorHandler::getHttpStatusCode(
                          "unauthorized_client"
                        )
                      );
                      if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                          m->incrementCounter(
                            "oauth2_requests_total",
                            fulla::common::ports::MetricLabels{{"endpoint", "token"}},
                            static_cast<double>(
                              static_cast<int>(resp->getStatusCode())
                            )
                          );
                      (*dispatchCb)(resp);
                      return;
                  }
              }
          }
          // Enforcement passed (or no client resolved / legacy NULL method):
          // proceed with the grant dispatch.
          if (*dispatchGrant)
              (*dispatchGrant)();
      }
    );
}

void TokenEndpointController::userInfo(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    if (req->method() == ::drogon::Options)
    {
        auto resp = ::drogon::HttpResponse::newHttpResponse();
        callback(resp);
        return;
    }

    std::string userId;
    auto attrs = req->getAttributes();
    if (!attrs->find("userId"))
    {
        auto resp = ::drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(::drogon::k401Unauthorized);
        resp->setBody("User ID not found in request attributes");
        callback(resp);
        return;
    }
    userId = attrs->get<std::string>("userId");

    // F-023 / #43 R2 (OIDC Core §5.3): the UserInfo endpoint requires a
    // user access token whose scope includes "openid". Two distinct rejection
    // conditions -- kept as SEPARATE RFC 6750 error paths (R2: do not conflate
    // a token-type mismatch with a scope insufficiency):
    //
    //   1. M2M (client_credentials) tokens carry a "client:<id>" subject and
    //      have no user identity -> 401 invalid_token (the token is a valid
    //      bearer but is the wrong TOKEN TYPE for a user-info resource).
    //   2. A user token lacking the "openid" scope -> 403 insufficient_scope.
    //      NOTE (#60 item 4): this handler check is the SOLE enforcement for
    //      userinfo — the ResourceScopeRegistry deliberately EXCLUDES
    //      /oauth2/userinfo (see ResourceScopeRegistry.cc isAuthGatedPath:
    //      the 401-vs-403 split is handler-exclusive by design), so there is
    //      no upstream filter gate behind it. Do NOT remove this check.
    std::string scope = attrs->get<std::string>("scope");
    if (userId.rfind("client:", 0) == 0)
    {
        auto resp = ::drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(::drogon::k401Unauthorized);
        resp->addHeader(
          "WWW-Authenticate",
          "Bearer realm=\"fulla\", error=\"invalid_token\", "
          "error_description=\"userinfo requires a user access token, not a client token\""
        );
        Json::Value err;
        err["error"] = "invalid_token";
        err["error_description"] = "Client-credentials tokens have no user identity for userinfo";
        resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
        Json::StreamWriterBuilder w;
        resp->setBody(Json::writeString(w, err));
        callback(resp);
        return;
    }
    if (!fulla::drogon::utils::hasScope(scope, "openid"))
    {
        auto resp = ::drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(::drogon::k403Forbidden);
        // RFC 6750 §3: WWW-Authenticate challenge with insufficient_scope,
        // including the scope attribute naming what is needed.
        resp->addHeader(
          "WWW-Authenticate",
          "Bearer realm=\"fulla\", error=\"insufficient_scope\", "
          "error_description=\"userinfo requires an openid-scoped user access token\", "
          "scope=\"openid\""
        );
        Json::Value err;
        err["error"] = "insufficient_scope";
        err["error_description"] =
          "The access token does not have the openid scope required for userinfo";
        resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
        Json::StreamWriterBuilder w;
        resp->setBody(Json::writeString(w, err));
        callback(resp);
        return;
    }

    auto plugin = resolvePlugin();
    if (!plugin)
    {
        Json::Value userInfo;
        userInfo["sub"] = userId;
        auto resp = ::drogon::HttpResponse::newHttpJsonResponse(userInfo);
        callback(resp);
        return;
    }
    // First get user roles
    // P1 #4 (评审问题点 4, intentional): `[this]` is safe here -- Drogon
    // HttpController<> instances are process-wide singletons whose lifetime
    // spans the whole process run (same reasoning documented in
    // AuthorizationFilter.cc doFilter, ~L90). `this` therefore outlives every
    // async continuation; shared_from_this is not applicable (Drogon manages
    // controllers via raw pointers). Comment added to deter repeat reports.
    plugin->getUserRoles(userId, [this, req, userId, scope, callback](std::vector<std::string> roles) {
        // Phase 4.5: route through plugin->getUserInfo (today still the god
        // facade; the identity-side migration to fulla::identity::* is a
        // separate follow-up). No getStorage() reach-in.
        auto plugin = resolvePlugin();
        plugin
          ->getUserInfo(userId, [plugin, req, userId, scope, roles, callback](std::optional<Json::Value> dbUserInfo) mutable {
              Json::Value userInfo;
              userInfo["sub"] = userId;

              // Build OIDC claims from storage result.
              // storage->getUserInfo returns {id, username?, email?} with no name field,
              // so compute the 'name' claim here: username preferred, fallback to email
              // (username is optional in the email-first model) so strict OIDC clients
              // never see an empty/missing name.
              if (dbUserInfo)
              {
                  std::string uname =
                    dbUserInfo->isMember("username") ? (*dbUserInfo)["username"].asString() : "";
                  std::string email =
                    dbUserInfo->isMember("email") ? (*dbUserInfo)["email"].asString() : "";
                  // V033: display_name (when set) takes precedence for the
                  // OIDC `name` claim, then username, then email (the
                  // username is optional in the email-first model) so strict
                  // OIDC clients never see an empty/missing name.
                  std::string displayName = dbUserInfo->isMember("display_name")
                                              ? (*dbUserInfo)["display_name"].asString()
                                              : "";
                  userInfo["name"] = !displayName.empty() ? displayName
                                     : (!uname.empty() ? uname : email);
                  if (!uname.empty())
                  {
                      userInfo["username"] = uname;
                  }
                  if (dbUserInfo->isMember("picture") &&
                      !(*dbUserInfo)["picture"].asString().empty())
                  {
                      userInfo["picture"] = (*dbUserInfo)["picture"].asString();
                  }
                  if (!email.empty())
                  {
                      userInfo["email"] = email;
                      // F-024 (OIDC Core §5.1): email_verified accompanies the
                      // email claim when the server knows the verification
                      // status (the identity repository reads it from the users
                      // row). Default to false when the field is absent.
                      bool emailVerified = false;
                      if (dbUserInfo->isMember("email_verified"))
                          emailVerified = (*dbUserInfo)["email_verified"].asBool();
                      userInfo["email_verified"] = emailVerified;
                  }
              }
              else
              {
                  // P2-5 audit fix: a bearer token whose subject resolves to
                  // no live user is not a valid token for userinfo purposes —
                  // answer 401 invalid_token (RFC 6750 3) instead of
                  // fabricating a 200 profile from the subject string.
                  auto resp = ::drogon::HttpResponse::newHttpResponse();
                  resp->setStatusCode(::drogon::k401Unauthorized);
                  resp->addHeader(
                    "WWW-Authenticate",
                    "Bearer realm=\"fulla\", error=\"invalid_token\", "
                    "error_description=\"token subject no longer resolves to a user\""
                  );
                  Json::Value err;
                  err["error"] = "invalid_token";
                  err["error_description"] = "Token subject no longer resolves to a user";
                  resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                  Json::StreamWriterBuilder w;
                  resp->setBody(Json::writeString(w, err));
                  callback(resp);
                  return;
              }

              // Add roles
              if (!roles.empty())
              {
                  userInfo["roles"] = Json::Value(Json::arrayValue);
                  for (const auto &role : roles)
                  {
                      userInfo["roles"].append(role);
                  }
              }

              // v1.5.0 M1 (design §2.1 item 3): org_ctx -- the ACTIVE org
              // bound to this access token (selected by the org_id
              // parameter at authorize, carried through the code->token
              // chain) with the user's CURRENT roles in it (O7 real-time
              // re-check: a removed member's userinfo drops org_ctx
              // immediately). Emitted only when the token was issued in
              // org context AND carries the `org` scope.
              auto finishUserInfo = [callback](Json::Value userInfoFinal) mutable {
                  auto resp = ::drogon::HttpResponse::newHttpJsonResponse(userInfoFinal);
                  callback(resp);
              };
              if (!fulla::drogon::utils::hasScope(scope, "org"))
              {
                  finishUserInfo(userInfo);
                  return;
              }
              {
                  // v1.5.0 M1 (review nit 6): the org binding comes from the
                  // AuthorizationFilter's request attributes (it already
                  // holds the validated token row) -- no second
                  // introspection round-trip for a field the filter has.
                  int32_t orgId = 0;
                  const bool hasOrgId = [&] {
                      auto attrs2 = req->getAttributes();
                      if (!attrs2->find("orgId"))
                          return false;
                      try
                      {
                          orgId = std::stoi(attrs2->get<std::string>("orgId"));
                          return true;
                      }
                      catch (...)
                      {
                          return false;
                      }
                  }();
                  auto withOrgId = [plugin, req, userId, userInfo,
                                    finishUserInfo = std::move(finishUserInfo),
                                    hasOrgId,
                                    orgId]() mutable {
                        if (!hasOrgId)
                        {
                            finishUserInfo(userInfo);
                            return;
                        }
                        // Storage-type-guarded (Debug asserts on an
                        // unknown client name -- see the plugin wiring
                        // note); memory mode has no org rows anyway.
                        ::drogon::orm::DbClientPtr orgDb;
                        if (plugin->getStorageType() != "memory")
                        {
                            try
                            {
                                orgDb = ::drogon::app().getDbClient();
                            }
                            catch (...)
                            {
                                orgDb = nullptr;
                            }
                        }
                        if (!orgDb)
                        {
                            // Memory mode: no org rows, no org_ctx.
                            finishUserInfo(userInfo);
                            return;
                        }
                        auto sharedFinish =
                          std::make_shared<std::function<void(Json::Value)>>(
                            [finishUserInfo = std::move(finishUserInfo)](
                              Json::Value final) mutable {
                                finishUserInfo(final);
                            }
                        );
                        auto repo = std::make_shared<
                          ::fulla::storage::postgres::ClientOwnersRepository>(orgDb);
                        using ::fulla::storage::postgres::LookupStatus;
                        // Dual-key subject resolution (public_sub | numeric
                        // id): login-issued tokens carry the UUID subject,
                        // authorize/consent-issued ones the internal id.
                        repo->findUserIdBySubject(
                          userId,
                          [repo, sharedFinish, userInfo, orgId](
                            std::optional<int32_t> internalUserId) {
                              if (!internalUserId.has_value())
                              {
                                  (*sharedFinish)(userInfo);
                                  return;
                              }
                              repo->findOrganization(
                                std::to_string(orgId),
                                [repo, sharedFinish, userInfo, orgId, internalUserId](
                                  const ::fulla::storage::postgres::OrganizationLookup &o) {
                                    if (o.status != LookupStatus::Found)
                                    {
                                        (*sharedFinish)(userInfo);
                                        return;
                                    }
                                    // O7: CURRENT membership only.
                                    repo->findMembership(
                                      orgId,
                                      *internalUserId,
                                      [sharedFinish, userInfo, o](
                                        const ::fulla::storage::postgres::MembershipLookup &m
                                      ) {
                                          Json::Value final(userInfo);
                                          if (m.status == LookupStatus::Found)
                                          {
                                              Json::Value orgCtx;
                                              orgCtx["org_id"] =
                                                (Json::Int64)o.row.getValueOfId();
                                              orgCtx["org_name"] = o.row.getValueOfName();
                                              Json::Value orgRoles(Json::arrayValue);
                                              orgRoles.append(m.row.getValueOfRole());
                                              orgCtx["roles"] = orgRoles;
                                              final["org_ctx"] = orgCtx;
                                          }
                                          (*sharedFinish)(final);
                                      }
                                    );
                                }
                              );
                          }
                        );
                  };
                  withOrgId();
              }
          });
    });
}

}  // namespace fulla::drogon::controllers
