#include <fulla/drogon/controllers/SocialTokenIssuer.h>
#include <fulla/drogon/controllers/GitHubController.h>
#include <drogon/drogon.h>

#include <fulla/common/observability/AuditEvent.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/drogon/observability/AuditLogger.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/oauth2/model/Dto.h>

#include <chrono>

namespace fulla::drogon::controllers
{

namespace
{
// #70: the OAuth2 client the first-party social token pair is issued for.
// Configurable (external_auth.social_token_client_id, default "fulla-portal" —
// the behavior GitHub login always had); must be a FIRST-PARTY client: the
// issuance has no consent interaction, so pointing this at a third-party
// client would hand it tokens nobody agreed to.
std::string socialTokenClientId()
{
    const auto &config = ::drogon::app().getCustomConfig();
    if (config.isMember("external_auth") &&
        config["external_auth"].isMember("social_token_client_id"))
    {
        std::string v = config["external_auth"]["social_token_client_id"].asString();
        if (!v.empty())
            return v;
    }
    return "fulla-portal";
}

void respondError(
  const ::drogon::HttpRequestPtr &req,
  const SocialTokenIssuer::CallbackPtr &cb,
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

void issueTokensForRegisteredClient(
  const ::drogon::HttpRequestPtr &req,
  const SocialTokenIssuer::CallbackPtr &callbackPtr,
  ::OAuth2Plugin *plugin,
  const std::string &provider,
  int64_t internalUserId,
  const std::string &userPublicSub,
  const std::string &clientId,
  const std::string &scope
)
{
    if (!plugin)
    {
        respondError(req, callbackPtr, "INTERNAL_ERROR", provider + " login: OAuth2Plugin not available");
        return;
    }
    if (userPublicSub.empty())
    {
        // Without a platform subject the token rows would be unresolvable by
        // every authenticated handler — refuse rather than mint dead tokens.
        respondError(
          req, callbackPtr, "INTERNAL_ERROR", provider + " login: no public subject resolved"
        );
        return;
    }

    // Same storage path as every other issuance (saveTokenPair forwards to
    // the ITokenRepository selected by storage_type; the historical direct
    // Mapper path crashed memory-storage deployments and bypassed the
    // storage abstraction).
    auto accessTokenStr = ::fulla::drogon::utils::generateSecureToken();
    auto refreshTokenStr = ::fulla::drogon::utils::generateSecureToken();
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()
    )
                 .count();
    const long long accessTokenTtl = plugin->getAccessTokenTtl();
    const long long refreshTokenTtl = plugin->getRefreshTokenTtl();

    // #69: store the HASH of the token (hash-based lookups in
    // validateAccessToken / introspection / refresh would miss raw values).
    // #70: store the PUBLIC SUBJECT, not the internal numeric id —
    // Bearer-authenticated handlers resolve tokens via users.public_sub.
    const std::string accessTokenHash = ::fulla::drogon::utils::hashToken(accessTokenStr);
    const std::string refreshTokenHash = ::fulla::drogon::utils::hashToken(refreshTokenStr);

    fulla::oauth2::model::OAuth2AccessToken accessToken;
    accessToken.token = accessTokenHash;
    accessToken.clientId = clientId;
    accessToken.userId = userPublicSub;
    accessToken.scope = scope;
    accessToken.issuedAt = now;
    accessToken.expiresAt = now + accessTokenTtl;
    accessToken.issuer = plugin->getIssuer();

    fulla::oauth2::model::OAuth2RefreshToken refreshToken;
    refreshToken.token = refreshTokenHash;
    refreshToken.accessToken = accessTokenHash;
    refreshToken.clientId = clientId;
    refreshToken.userId = userPublicSub;
    refreshToken.scope = scope;
    refreshToken.expiresAt = now + refreshTokenTtl;

    plugin->saveTokenPair(
      accessToken,
      refreshToken,
      [req, callbackPtr, provider, internalUserId, userPublicSub, clientId, scope, accessTokenStr, refreshTokenStr, accessTokenTtl](bool ok) {
          if (!ok)
          {
              // Persistence failed: returning 200 + these tokens would be a
              // silent failure (never stored -> every lookup misses).
              respondError(
                req, callbackPtr, "INTERNAL_ERROR", provider + " login: failed to persist token pair"
              );
              return;
          }

          // Compliance trace (#70): audit event, NOT an oauth2_user_consents
          // row (a consent row would silently satisfy the Tier-3 consent
          // check and pre-approve this client for scopes the user was never
          // prompted for — see this header's top comment).
          fulla::common::observability::AuditEvent event;
          event.action = "SOCIAL_LOGIN_TOKEN_ISSUED";
          event.outcome = "success";
          event.actorType = "user";
          event.actorId = userPublicSub;
          event.targetType = "token";
          event.targetId = clientId;
          event.details = Json::Value(Json::objectValue);
          event.details["provider"] = provider;
          event.details["internal_user_id"] = (Json::Int64)internalUserId;
          event.details["client_id"] = clientId;
          event.details["scope"] = scope;
          try
          {
              event.ip = req->peerAddr().toIp();
              event.userAgent = req->getHeader("User-Agent");
          }
          catch (...)
          {
          }
          ::fulla::drogon::observability::AuditLogger::log(event);

          Json::Value result;
          result["access_token"] = accessTokenStr;
          result["refresh_token"] = refreshTokenStr;
          result["token_type"] = "Bearer";
          result["expires_in"] = (Json::Int64)accessTokenTtl;
          (*callbackPtr)(::drogon::HttpResponse::newHttpJsonResponse(result));
      }
    );
}

void SocialTokenIssuer::issueTokensForUser(
  const ::drogon::HttpRequestPtr &req,
  const CallbackPtr &callbackPtr,
  ::OAuth2Plugin *plugin,
  const std::string &provider,
  int64_t internalUserId,
  const std::string &userPublicSub
)
{
    if (!plugin)
    {
        respondError(req, callbackPtr, "INTERNAL_ERROR", provider + " login: OAuth2Plugin not available");
        return;
    }
    if (userPublicSub.empty())
    {
        // Without a platform subject the token rows would be unresolvable by
        // every authenticated handler — refuse rather than mint dead tokens.
        respondError(
          req, callbackPtr, "INTERNAL_ERROR", provider + " login: no public subject resolved"
        );
        return;
    }

    // #70 PR review Low: the configured first-party client must actually be
    // a REGISTERED client -- a typo'd/unregistered social_token_client_id
    // would otherwise mint tokens no introspection/refresh path can
    // resolve, silently. (Whether a registered client is first-party stays
    // an operator responsibility -- documented in social-login.md; there is
    // no first-party flag in the data model.)
    const std::string clientId = socialTokenClientId();
    const std::string scope = "openid profile email";
    plugin->getClient(
      clientId,
      [req, callbackPtr, plugin, provider, internalUserId, userPublicSub, clientId, scope](
        std::optional<fulla::oauth2::model::OAuth2Client> clientRow) {
          if (!clientRow)
          {
              LOG_ERROR << "SocialTokenIssuer: external_auth.social_token_client_id '"
                        << clientId
                        << "' is not a registered client -- refusing to issue social tokens";
              respondError(
                req,
                callbackPtr,
                "INTERNAL_ERROR",
                provider + " login: social token client not registered"
              );
              return;
          }
          issueTokensForRegisteredClient(
            req, callbackPtr, plugin, provider, internalUserId, userPublicSub, clientId, scope);
      });
}

}  // namespace fulla::drogon::controllers
