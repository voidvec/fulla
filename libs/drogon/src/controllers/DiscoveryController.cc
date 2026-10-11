#include <fulla/drogon/controllers/DiscoveryController.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/error/OAuth2ErrorHandler.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>
#include <fulla/storage/postgres/models/Oauth2Scopes.h>
#include <drogon/drogon.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <vector>

using namespace fulla::drogon::controllers;
using namespace fulla::drogon::observability::openapi;

namespace fulla::drogon::controllers
{

namespace
{
// F-016: normalize a trailing slash so the advertised issuer (and every
// endpoint URL derived from it) is byte-identical to the issuer OAuth2Plugin
// stamps on tokens at startup (which applies the same normalization). Without
// this, metadata.issuer="https://auth.example.com/" produced
// "https://auth.example.com//oauth2/token" and an iss/issuer mismatch.
std::string normalizeIssuer(std::string url)
{
    while (url.size() > 1 && url.back() == '/')
        url.pop_back();
    return url;
}

// P0-5 (#259): scopes_supported is served from the DB scope catalog (60 s
// cache) instead of the former hardcoded four-entry list, so `org` (v1.4.0)
// and the resource-scope family (V023) stop being
// implemented-but-unadvertised. Memory-mode deployments (no DbClient) and
// fetch failures fall back to the core set / last-good cache — discovery
// must answer even when the catalog is unreachable.
std::mutex g_scopesMutex;
std::vector<std::string> g_scopesCache;
std::chrono::steady_clock::time_point g_scopesFetchedAt;
constexpr auto kScopesCacheTtl = std::chrono::seconds(60);

const std::vector<std::string> &coreScopesFallback()
{
    static const std::vector<std::string> kCore = {"admin", "email", "openid", "org", "profile"};
    return kCore;
}

void scopesSupportedAsync(std::function<void(std::vector<std::string>)> &&cb)
{
    auto sharedCb =
      std::make_shared<std::function<void(std::vector<std::string>)>>(std::move(cb));
    {
        std::lock_guard<std::mutex> lock(g_scopesMutex);
        if (
          !g_scopesCache.empty() &&
          std::chrono::steady_clock::now() - g_scopesFetchedAt < kScopesCacheTtl)
        {
            (*sharedCb)(g_scopesCache);
            return;
        }
    }
    // Memory-mode guard (the M1 Debug-assert pitfall): getDbClient() ABORTS
    // in Debug builds when no db_clients are configured — memory deployments
    // serve the core fallback set instead of the catalog.
    if (
      auto *p = ::drogon::app().getPlugin<::OAuth2Plugin>();
      p != nullptr && p->getStorageType() == "memory"
    )
    {
        (*sharedCb)(coreScopesFallback());
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
        std::lock_guard<std::mutex> lock(g_scopesMutex);
        (*sharedCb)(g_scopesCache.empty() ? coreScopesFallback() : g_scopesCache);
        return;
    }
    try
    {
        // The catalog is tiny; fetch all rows and filter in code. V023 left
        // the legacy bare read/write rows as inert orphans — they are NOT
        // advertised.
        ::drogon::orm::Mapper<drogon_model::fulla_db::Oauth2Scopes>(db)
          .findBy(
            ::drogon::orm::Criteria(
              drogon_model::fulla_db::Oauth2Scopes::Cols::_id,
              ::drogon::orm::CompareOperator::GT,
              0
            ),
            [sharedCb](const std::vector<drogon_model::fulla_db::Oauth2Scopes> &rows) {
                std::vector<std::string> scopes;
                for (const auto &row : rows)
                {
                    const std::string name = row.getValueOfName();
                    if (name.empty() || name == "read" || name == "write")
                        continue;
                    scopes.push_back(name);
                }
                std::sort(scopes.begin(), scopes.end());
                {
                    std::lock_guard<std::mutex> lock(g_scopesMutex);
                    if (!scopes.empty())
                    {
                        g_scopesCache = scopes;
                        g_scopesFetchedAt = std::chrono::steady_clock::now();
                    }
                }
                (*sharedCb)(scopes.empty() ? coreScopesFallback() : scopes);
            },
            [sharedCb](const ::drogon::orm::DrogonDbException &e) {
                LOG_WARN << "scopes_supported catalog fetch failed: " << e.base().what();
                std::vector<std::string> out;
                {
                    std::lock_guard<std::mutex> lock(g_scopesMutex);
                    out = g_scopesCache.empty() ? coreScopesFallback() : g_scopesCache;
                }
                (*sharedCb)(out);
            }
          );
    }
    catch (...)
    {
        std::vector<std::string> out;
        {
            std::lock_guard<std::mutex> lock(g_scopesMutex);
            out = g_scopesCache.empty() ? coreScopesFallback() : g_scopesCache;
        }
        (*sharedCb)(out);
    }
}
}  // namespace

::OAuth2Plugin *DiscoveryController::resolvePlugin() const
{
    return plugin_ ? plugin_ : ::drogon::app().getPlugin<::OAuth2Plugin>();
}

void DiscoveryController::initApiDocs()
{
    static std::once_flag docsOnce;
    std::call_once(docsOnce, [] { initApiDocsImpl(); });
}

void DiscoveryController::initApiDocsImpl()
{
    // OIDC Discovery endpoint
    {
        fulla::drogon::observability::openapi::EndpointInfo discoveryEndpoint;
        discoveryEndpoint.path = "/.well-known/openid-configuration";
        discoveryEndpoint.method = "GET";
        discoveryEndpoint.summary = "OpenID Connect Discovery";
        discoveryEndpoint.description =
          "Returns OIDC discovery metadata including endpoints and supported scopes.";
        discoveryEndpoint.tags = {"OpenID Connect"};
        discoveryEndpoint.parameters = {};
        discoveryEndpoint.responses = {{200, "OIDC Provider Metadata"}};
        discoveryEndpoint.requiresAuth = false;
        OpenApiGenerator::addEndpoint(discoveryEndpoint);
    }

    // JWKS endpoint
    {
        fulla::drogon::observability::openapi::EndpointInfo jwksEndpoint;
        jwksEndpoint.path = "/.well-known/jwks.json";
        jwksEndpoint.method = "GET";
        jwksEndpoint.summary = "JSON Web Key Set";
        jwksEndpoint.description = "Returns the public keys used by this server to sign JWTs.";
        jwksEndpoint.tags = {"OpenID Connect", "Security"};
        jwksEndpoint.parameters = {};
        jwksEndpoint.responses = {{200, "JSON Web Key Set"}};
        jwksEndpoint.requiresAuth = false;
        OpenApiGenerator::addEndpoint(jwksEndpoint);
    }

    // RFC 8414 OAuth 2.0 Authorization Server Metadata
    {
        fulla::drogon::observability::openapi::EndpointInfo asMetadataEndpoint;
        asMetadataEndpoint.path = "/.well-known/oauth-authorization-server";
        asMetadataEndpoint.method = "GET";
        asMetadataEndpoint.summary = "OAuth 2.0 Authorization Server Metadata";
        asMetadataEndpoint.description =
          "RFC 8414 authorization server metadata (endpoints, grant types, "
          "auth methods). Unlike the OIDC discovery document this carries no "
          "userinfo/jwks entries.";
        asMetadataEndpoint.tags = {"OAuth2", "Discovery"};
        asMetadataEndpoint.parameters = {};
        asMetadataEndpoint.responses = {{200, "Authorization Server Metadata"}};
        asMetadataEndpoint.requiresAuth = false;
        OpenApiGenerator::addEndpoint(asMetadataEndpoint);
    }
}

void DiscoveryController::metadata(
  const ::drogon::HttpRequestPtr & /*req*/,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    LOG_DEBUG << "Metadata endpoint requested";

    // Get OAuth2 plugin
    auto plugin = resolvePlugin();
    if (!plugin)
    {
        fulla::common::error::OAuth2ErrorHandler::sendErrorResponse(
          std::move(callback), "server_error", "OAuth2 plugin not available"
        );
        return;
    }

    // Base URL from configuration (required for production)
    std::string baseUrl;
    auto customConfig = ::drogon::app().getCustomConfig();
    if (customConfig.isMember("metadata") && customConfig["metadata"].isMember("issuer"))
    {
        baseUrl = normalizeIssuer(customConfig["metadata"]["issuer"].asString());
    }
    if (baseUrl.empty())
    {
        // Fallback: construct from listener (dev mode only)
        baseUrl = "http://localhost:5555";
        LOG_WARN << "metadata.issuer not configured, using fallback: " << baseUrl;
    }

    Json::Value metadata;

    // Basic server info
    metadata["issuer"] = baseUrl;
    metadata["authorization_endpoint"] = baseUrl + "/oauth2/authorize";
    metadata["token_endpoint"] = baseUrl + "/oauth2/token";
    metadata["device_authorization_endpoint"] = baseUrl + "/oauth2/device_authorization";

    // P1 endpoints
    metadata["introspection_endpoint"] = baseUrl + "/oauth2/introspect";
    metadata["introspection_endpoint_auth_methods_supported"] = Json::Value(Json::arrayValue);
    metadata["introspection_endpoint_auth_methods_supported"].append("client_secret_post");
    metadata["introspection_endpoint_auth_methods_supported"].append("client_secret_basic");

    metadata["revocation_endpoint"] = baseUrl + "/oauth2/revoke";
    metadata["revocation_endpoint_auth_methods_supported"] = Json::Value(Json::arrayValue);
    metadata["revocation_endpoint_auth_methods_supported"].append("client_secret_post");
    metadata["revocation_endpoint_auth_methods_supported"].append("client_secret_basic");

    // R-5: registration_endpoint is implemented (POST /oauth2/register, admin
    // gated) -- advertise it so clients can discover it.
    metadata["registration_endpoint"] = baseUrl + "/oauth2/register";

    // P0-5 (#259): scopes_supported is filled from the DB scope catalog in
    // the async continuation at the end of this handler (see
    // scopesSupportedAsync).

    metadata["response_types_supported"] = Json::Value(Json::arrayValue);
    metadata["response_types_supported"].append("code");

    metadata["response_modes_supported"] = Json::Value(Json::arrayValue);
    metadata["response_modes_supported"].append("query");

    metadata["grant_types_supported"] = Json::Value(Json::arrayValue);
    metadata["grant_types_supported"].append("authorization_code");
    metadata["grant_types_supported"].append("refresh_token");
    metadata["grant_types_supported"].append("client_credentials");
    metadata["grant_types_supported"].append("urn:ietf:params:oauth:grant-type:device_code");

    // R-5 (RFC 8414 §2 REQUIRED): subject_types_supported. Only "public" is
    // implemented (no pairwise pseudonymous subject support).
    metadata["subject_types_supported"] = Json::Value(Json::arrayValue);
    metadata["subject_types_supported"].append("public");

    // PKCE support — P0-5 (#259): S256-only (RFC 9700 §2.1.1 direction);
    // plain is no longer accepted at authorize (short of the
    // auth.allow_pkce_plain escape hatch) and is no longer advertised.
    metadata["code_challenge_methods_supported"] = Json::Value(Json::arrayValue);
    metadata["code_challenge_methods_supported"].append("S256");

    // Client authentication methods
    metadata["token_endpoint_auth_methods_supported"] = Json::Value(Json::arrayValue);
    metadata["token_endpoint_auth_methods_supported"].append("client_secret_post");
    metadata["token_endpoint_auth_methods_supported"].append("client_secret_basic");

    // Documentation and policies (if configured)
    if (customConfig.isMember("metadata"))
    {
        if (customConfig["metadata"].isMember("service_documentation"))
            metadata["service_documentation"] = customConfig["metadata"]["service_documentation"];
        if (customConfig["metadata"].isMember("op_policy_uri"))
            metadata["op_policy_uri"] = customConfig["metadata"]["op_policy_uri"];
        if (customConfig["metadata"].isMember("op_tos_uri"))
            metadata["op_tos_uri"] = customConfig["metadata"]["op_tos_uri"];
    }

    // P0-5 (#259): catalog-backed scopes land here (async continuation).
    scopesSupportedAsync(
      [metadata, callback](std::vector<std::string> scopes) mutable {
          metadata["scopes_supported"] = Json::Value(Json::arrayValue);
          for (const auto &s : scopes)
              metadata["scopes_supported"].append(s);
          auto resp = ::drogon::HttpResponse::newHttpJsonResponse(metadata);
          resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
          callback(resp);
      });
}

void DiscoveryController::oidcDiscovery(
  const ::drogon::HttpRequestPtr & /*req*/,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    // OpenID Connect Discovery 1.0
    std::string baseUrl;
    auto customConfig = ::drogon::app().getCustomConfig();
    if (customConfig.isMember("metadata") && customConfig["metadata"].isMember("issuer"))
    {
        baseUrl = normalizeIssuer(customConfig["metadata"]["issuer"].asString());
    }
    if (baseUrl.empty())
    {
        baseUrl = "http://localhost:5555";
    }

    Json::Value discovery;
    discovery["issuer"] = baseUrl;
    discovery["authorization_endpoint"] = baseUrl + "/oauth2/authorize";
    discovery["token_endpoint"] = baseUrl + "/oauth2/token";
    discovery["device_authorization_endpoint"] = baseUrl + "/oauth2/device_authorization";
    discovery["userinfo_endpoint"] = baseUrl + "/oauth2/userinfo";
    discovery["jwks_uri"] = baseUrl + "/.well-known/jwks.json";
    discovery["introspection_endpoint"] = baseUrl + "/oauth2/introspect";
    discovery["revocation_endpoint"] = baseUrl + "/oauth2/revoke";
    // R-5: registration_endpoint is implemented (POST /oauth2/register, admin
    // gated) -- advertise it.
    discovery["registration_endpoint"] = baseUrl + "/oauth2/register";

    // P0-5 (#259): scopes_supported is filled from the DB scope catalog in
    // the async continuation at the end of this handler (see
    // scopesSupportedAsync).

    discovery["response_types_supported"] = Json::Value(Json::arrayValue);
    discovery["response_types_supported"].append("code");

    discovery["grant_types_supported"] = Json::Value(Json::arrayValue);
    discovery["grant_types_supported"].append("authorization_code");
    discovery["grant_types_supported"].append("refresh_token");
    discovery["grant_types_supported"].append("client_credentials");
    discovery["grant_types_supported"].append("urn:ietf:params:oauth:grant-type:device_code");

    discovery["subject_types_supported"] = Json::Value(Json::arrayValue);
    discovery["subject_types_supported"].append("public");

    discovery["id_token_signing_alg_values_supported"] = Json::Value(Json::arrayValue);
    discovery["id_token_signing_alg_values_supported"].append("RS256");

    discovery["token_endpoint_auth_methods_supported"] = Json::Value(Json::arrayValue);
    discovery["token_endpoint_auth_methods_supported"].append("client_secret_basic");
    discovery["token_endpoint_auth_methods_supported"].append("client_secret_post");

    discovery["code_challenge_methods_supported"] = Json::Value(Json::arrayValue);
    // P0-5 (#259): S256-only — plain is no longer accepted at authorize and
    // is no longer advertised.
    discovery["code_challenge_methods_supported"].append("S256");

    // F-022 (OIDC Core §3.1.2.1 / §5.1): advertise prompt/max_age support and
    // the auth_time/acr/amr claims the id_token now carries.
    // R-4: only the values the authorize endpoint actually honors are
    // advertised -- select_account is NOT implemented (no account-picker
    // branch), so it is omitted to avoid an advertised-but-not-honored gap.
    discovery["prompt_values_supported"] = Json::Value(Json::arrayValue);
    discovery["prompt_values_supported"].append("none");
    discovery["prompt_values_supported"].append("login");
    discovery["prompt_values_supported"].append("consent");

    discovery["acr_values_supported"] = Json::Value(Json::arrayValue);
    discovery["acr_values_supported"].append("1");  // password-only
    discovery["acr_values_supported"].append("2");  // MFA

    // F-027 (OIDC RP-Initiated Logout 1.0): the end_session endpoint URL.
    discovery["end_session_endpoint"] = baseUrl + "/oauth2/end_session";

    // B1 (OIDC Back-Channel Logout 1.0): the OP delivers a signed logout_token
    // to each RP's registered backchannel_logout_uri on logout. Session-level
    // support is false because this OP issues subject-scoped (sub) logout_tokens
    // without a session id (sid) -- RPs with backchannel_logout_session_required
    // =true are a documented gap.
    discovery["backchannel_logout_supported"] = true;
    discovery["backchannel_logout_session_supported"] = false;

    // P0-5 (#259): reconciled against what is actually emitted (the
    // reconciliation test freezes this set): id_token carries
    // iss/sub/aud/iat/exp/nonce + conditional auth_time/acr/amr/org_ctx;
    // userinfo (P0-2, scope-gated) adds name/preferred_username/picture/
    // email/email_verified; roles ride the token response and userinfo.
    discovery["claims_supported"] = Json::Value(Json::arrayValue);
    discovery["claims_supported"].append("sub");
    discovery["claims_supported"].append("iss");
    discovery["claims_supported"].append("aud");
    discovery["claims_supported"].append("exp");
    discovery["claims_supported"].append("iat");
    discovery["claims_supported"].append("nonce");
    discovery["claims_supported"].append("auth_time");
    discovery["claims_supported"].append("acr");
    discovery["claims_supported"].append("amr");
    discovery["claims_supported"].append("org_ctx");
    discovery["claims_supported"].append("name");
    discovery["claims_supported"].append("preferred_username");
    discovery["claims_supported"].append("picture");
    discovery["claims_supported"].append("email");
    discovery["claims_supported"].append("email_verified");
    discovery["claims_supported"].append("roles");

    // P0-5 (#259): the catalog-backed scope list lands here (async).
    scopesSupportedAsync(
      [discovery, callback](std::vector<std::string> scopes) mutable {
          discovery["scopes_supported"] = Json::Value(Json::arrayValue);
          for (const auto &s : scopes)
              discovery["scopes_supported"].append(s);
          auto resp = ::drogon::HttpResponse::newHttpJsonResponse(discovery);
          resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
          callback(resp);
      });
}

void DiscoveryController::jwks(
  const ::drogon::HttpRequestPtr & /*req*/,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto plugin = resolvePlugin();
    if (!plugin || !plugin->getJwkManager())
    {
        // P1 #9 (评审问题点 9, intentional): this branch is effectively
        // unreachable in production -- jwkManager_ is constructed and init()'d
        // unconditionally in OAuth2Plugin::initAndStart() (even without an OIDC
        // config), so getJwkManager() is non-null whenever the plugin is loaded.
        // It is only reached if the plugin itself failed to load, in which case
        // the whole OAuth2 service is down and JWKS caching is moot.
        // NOTE: do NOT add `Cache-Control: public, max-age=...` here (the naive
        // "align with the normal branch" fix). Caching an EMPTY keys array would
        // make downstream RPs/gateways cache "no verification keys" for an hour,
        // so id_token verification keeps failing even after the service recovers.
        // The current no-cache-header behavior is correct (HTTP heuristic
        // caching does not cache validator-less JSON responses).
        Json::Value empty;
        empty["keys"] = Json::Value(Json::arrayValue);
        auto resp = ::drogon::HttpResponse::newHttpJsonResponse(empty);
        callback(resp);
        return;
    }

    auto jwks = plugin->getJwkManager()->getJwks();
    auto resp = ::drogon::HttpResponse::newHttpJsonResponse(jwks);
    resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
    // Cache JWKS for 1 hour (keys don't change frequently)
    resp->addHeader("Cache-Control", "public, max-age=3600");
    callback(resp);
}

}  // namespace fulla::drogon::controllers
