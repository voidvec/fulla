#include <fulla/oauth2/protocol/TokenService.h>

#include <fulla/common/model/Subject.h>
#include <fulla/oauth2/pkce/Pkce.h>
#include <fulla/oauth2/protocol/TokenCrypto.h>

#include <chrono>

namespace fulla::oauth2::protocol
{

namespace
{
int64_t nowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()
    )
      .count();
}

Json::Value makeError(const std::string &error, const std::string &desc = "")
{
    Json::Value json;
    json["error"] = error;
    if (!desc.empty())
        json["error_description"] = desc;
    return json;
}

// Review MINOR #2: token-exact scope membership. OAuth scopes are
// space-separated tokens (RFC 6749 §3.3); a substring match would let
// `myopenid` falsely satisfy an `openid` check. libs/oauth2 cannot depend
// on libs/drogon's ScopeChecker (wrong dependency direction), so this is a
// framework-free equivalent. Mirrors fulla::drogon::utils::hasScope.
bool scopeContains(std::string_view scopes, std::string_view required)
{
    if (required.empty())
        return true;
    std::string_view s = scopes;
    while (!s.empty())
    {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
            s.remove_prefix(1);
        if (s.empty())
            break;
        auto pos = s.find(' ');
        std::string_view tok = (pos == std::string_view::npos) ? s : s.substr(0, pos);
        if (tok == required)
            return true;
        s = (pos == std::string_view::npos) ? std::string_view{} : s.substr(pos);
    }
    return false;
}
}  // namespace

TokenService::TokenService(
  std::shared_ptr<fulla::oauth2::repository::IClientRepository> clients,
  std::shared_ptr<fulla::oauth2::repository::IGrantRepository> grants,
  std::shared_ptr<fulla::oauth2::repository::ITokenRepository> tokens,
  std::shared_ptr<fulla::common::ports::ICryptoProvider> crypto,
  std::shared_ptr<fulla::common::ports::IAuditSink> auditSink,
  std::shared_ptr<fulla::common::ports::ISubjectResolver> subjectResolver,
  std::shared_ptr<fulla::common::ports::IRoleProvider> roleProvider,
  int64_t authCodeTtl,
  int64_t accessTokenTtl,
  int64_t refreshTokenTtl,
  std::string issuer
)
    : clients_(std::move(clients)),
      grants_(std::move(grants)),
      tokens_(std::move(tokens)),
      crypto_(std::move(crypto)),
      auditSink_(std::move(auditSink)),
      subjectResolver_(std::move(subjectResolver)),
      roleProvider_(std::move(roleProvider)),
      authCodeTtl_(authCodeTtl),
      accessTokenTtl_(accessTokenTtl),
      refreshTokenTtl_(refreshTokenTtl),
      issuer_(std::move(issuer))
{
}

void TokenService::resolveRoles(
  const std::string &subject,
  std::function<void(std::vector<std::string>)> &&cb
)
{
    if (!roleProvider_)
    {
        cb({});
        return;
    }

    // Phase 4.5: prefer the subject-string role lookup when the provider
    // supports it -- preserves the legacy "roles keyed by subject string"
    // semantics (MemoryRoleRepository's userRoles_ map, populated from
    // admin_users config) byte-for-byte, without needing a subject_mapping
    // row for config-only subjects like "admin". Falls back to the pure
    // two-port chain (ISubjectResolver -> getRoles(int32)) otherwise.
    if (roleProvider_->supportsSubjectLookup())
    {
        roleProvider_->getRoles(subject, std::move(cb));
        return;
    }

    if (!subjectResolver_)
    {
        cb({});
        return;
    }

    fulla::common::model::Subject subjectValue(subject);
    auto roleProvider = roleProvider_;
    subjectResolver_->resolve(
      subjectValue,
      [roleProvider, cb = std::move(cb)](std::optional<int32_t> internalUserId) mutable {
          if (!internalUserId)
          {
              cb({});
              return;
          }
          roleProvider->getRoles(*internalUserId, std::move(cb));
      }
    );
}

void TokenService::resolveOrgContextForClaims(
  const std::string &subject,
  const std::optional<int32_t> &orgId,
  const std::string &scope,
  std::function<void(std::optional<fulla::common::ports::OrgContextInfo>)> &&cb
)
{
    // Claim release is gated on the `org` scope (design §2.1 item 3): the
    // org_id parameter SELECTS the context, the scope AUTHORIZES its
    // release to the RP. No binding, no scope, or no resolver -> no claim
    // (org_ctx is claim data; its failure mode is absence).
    if (!orgContextResolver_ || !orgId.has_value() || !scopeContains(scope, "org"))
    {
        cb(std::nullopt);
        return;
    }
    orgContextResolver_->resolveOrgContext(subject, *orgId, std::move(cb));
}

void TokenService::audit(
  const std::string &action,
  const std::string &outcome,
  const std::string &actorId,
  const std::string &targetType,
  const std::string &targetId
)
{
    if (!auditSink_)
        return;

    fulla::common::observability::AuditEvent event;
    event.actorType = "user";
    event.actorId = actorId;
    event.action = action;
    event.targetType = targetType;
    event.targetId = targetId;
    event.outcome = outcome;
    auditSink_->record(event);
}

void TokenService::generateAuthorizationCode(
  const std::string &clientId,
  const std::string &subject,
  const std::string &scope,
  const std::string &redirectUri,
  const std::string &codeChallenge,
  const std::string &codeChallengeMethod,
  const std::string &nonce,
  std::function<void(bool, std::string, std::string)> &&callback,
  int64_t authTime,
  const std::string &amr,
  const std::optional<int32_t> &orgId
)
{
    if (!grants_ || !crypto_)
    {
        callback(false, "", "Storage not initialized");
        return;
    }

    auto code = generateSecureToken(*crypto_);
    // P2-1: "" means the CSPRNG failed — refuse issuance instead of
    // persisting/handing out a deterministic value.
    if (code.empty())
    {
        callback(false, "", "CSPRNG failure");
        return;
    }
    // Codex review (PR #197): the oauth2_codes.nonce column is VARCHAR(512);
    // an over-long nonce makes the insert fail, and saveAuthCode's void
    // callback surfaces that as a dead code at exchange time. Reject here at
    // the single issuance chokepoint instead.
    if (nonce.size() > 512)
    {
        callback(false, "", "nonce exceeds 512 characters");
        return;
    }
    fulla::oauth2::model::OAuth2AuthCode authCode;
    authCode.code = hashToken(*crypto_, code);
    authCode.clientId = clientId;
    authCode.userId = subject;
    authCode.scope = scope;
    authCode.redirectUri = redirectUri;
    authCode.codeChallenge = codeChallenge;
    authCode.codeChallengeMethod = codeChallengeMethod;
    authCode.nonce = nonce;
    authCode.expiresAt = nowSeconds() + authCodeTtl_;
    // F-022 (OIDC Core §3.1.3.7): thread the session's auth_time + amr onto
    // the code so the token endpoint can stamp them into the id_token.
    authCode.authTime = authTime;
    authCode.amr = amr;
    // v1.5.0 M1 (design §2.1 item 4): the org binding picked at authorize
    // time rides the code row; token exchange inherits it down the chain.
    authCode.orgId = orgId;

    grants_->saveAuthCode(authCode, [callback = std::move(callback), code]() {
        callback(true, code, "");
    });
}

void TokenService::exchangeCodeForToken(
  const std::string &code,
  const std::string &clientId,
  const std::string &clientSecret,
  const std::string &redirectUri,
  const std::string &codeVerifier,
  std::function<void(const Json::Value &)> &&callback
)
{
    if (!clients_ || !grants_ || !tokens_ || !crypto_)
    {
        callback(makeError("server_error"));
        return;
    }

    // Same lifetime-safety pattern as the original (defect 1.9 fix): capture
    // `self` at the outermost async call and thread it through every nested
    // continuation.
    auto self = shared_from_this();
    clients_->validateClient(
      clientId,
      clientSecret,
      [self, code, clientId, redirectUri, codeVerifier, callback = std::move(callback)](
        bool isValid
      ) mutable {
          if (!isValid)
          {
              callback(makeError("invalid_client", "Client authentication failed"));
              return;
          }

          self->grants_->consumeAuthCode(
            hashToken(*self->crypto_, code),
            redirectUri,
            [self, callback = std::move(callback), clientId, codeVerifier](
              std::optional<fulla::oauth2::model::OAuth2AuthCode> authCodeOpt
            ) {
                if (!authCodeOpt)
                {
                    callback(makeError("invalid_grant", "Invalid authorization code"));
                    return;
                }
                if (authCodeOpt->clientId != clientId)
                {
                    // P2-6(1): RFC 6749 5.2 — a code issued to another
                    // client is invalid_grant, not invalid_client.
                    callback(makeError("invalid_grant", "Authorization code was issued to another client"));
                    return;
                }

                if (!authCodeOpt->codeChallenge.empty())
                {
                    // PKCE was used - validate code_verifier.
                    if (
                      codeVerifier.empty() ||
                      !self->validatePkceCodeVerifier(
                        codeVerifier, authCodeOpt->codeChallenge, authCodeOpt->codeChallengeMethod
                      )
                    )
                    {
                        callback(makeError("invalid_grant", "PKCE validation failed"));
                        return;
                    }
                }
                // No advisory PUBLIC-client-without-PKCE warning here (the
                // original logs it via ILogger, which this Domain-layer port
                // does not depend on to avoid growing the constructor
                // parameter list for a warning-only side channel; full
                // enforcement happens at /oauth2/authorize time regardless).

                auto now = nowSeconds();
                if (now > authCodeOpt->expiresAt)
                {
                    callback(makeError("invalid_grant", "Code expired"));
                    return;
                }

                auto authCode = *authCodeOpt;
                // v1.5.0 M1 (design §2.1): resolve the org_ctx claim data
                // first -- a nullopt hop when the code carries no org
                // binding, the granted scopes lack `org`, or no resolver
                // is wired -- then roles, then issuance.
                self->resolveOrgContextForClaims(
                  authCode.userId,
                  authCode.orgId,
                  authCode.scope,
                  [self, callback, authCode, now](
                    std::optional<fulla::common::ports::OrgContextInfo> orgCtx
                  ) {
                  self->resolveRoles(
                    authCode.userId,
                    [self, callback, authCode, now, orgCtx](std::vector<std::string> roles) {
                      Json::Value rolesJson(Json::arrayValue);
                      for (const auto &r : roles)
                          rolesJson.append(r);

                      auto tokenStr = generateSecureToken(*self->crypto_);
                      auto refreshTokenStr = generateSecureToken(*self->crypto_);
                      auto familyId = generateSecureToken(*self->crypto_, 16);
                      // P2-1: refuse issuance on CSPRNG failure ("" results).
                      if (tokenStr.empty() || refreshTokenStr.empty() || familyId.empty())
                      {
                          self->audit(
                            "token_issued", "failure", authCode.userId, "token", ""
                          );
                          callback(makeError("server_error", "Token generation failed"));
                          return;
                      }
                      fulla::oauth2::model::OAuth2AccessToken token;
                      token.token = hashToken(*self->crypto_, tokenStr);
                      token.clientId = authCode.clientId;
                      token.userId = authCode.userId;
                      token.scope = authCode.scope;
                      // P2 #10: record the real issue time so introspection's
                      // iat (RFC 7662 §2.2) is populated, not left at the 0
                      // default (which the introspect endpoint silently omits).
                      token.issuedAt = now;
                      token.expiresAt = now + self->accessTokenTtl_;
                      // F-016: stamp the configured issuer at issuance so
                      // introspection's iss matches the discovery document
                      // (previously the column was never written and the DB
                      // default leaked a hardcoded example.com URL).
                      token.issuer = self->issuer_;
                      // v1.5.0 M1 (design §2.1 item 4): the org binding
                      // inherited from the code rides the token pair.
                      token.orgId = authCode.orgId;

                      fulla::oauth2::model::OAuth2RefreshToken refreshToken;
                      refreshToken.token = hashToken(*self->crypto_, refreshTokenStr);
                      refreshToken.accessToken = token.token;
                      refreshToken.clientId = authCode.clientId;
                      refreshToken.userId = authCode.userId;
                      refreshToken.scope = authCode.scope;
                      refreshToken.expiresAt = now + self->refreshTokenTtl_;
                      refreshToken.familyId = familyId;
                      refreshToken.orgId = authCode.orgId;

                      self->tokens_->saveTokenPair(
                        token,
                        refreshToken,
                        [self,
                         callback,
                         tokenStr,
                         refreshTokenStr,
                         rolesJson,
                         authCode,
                         now,
                         orgCtx](bool ok) {
                            if (!ok)
                            {
                                // Persistence failed: the tokens we would
                                // hand out were never stored, so issuing
                                // them anyway would be a silent failure
                                // (introspection/refresh lookups all miss).
                                self->audit(
                                  "token_issued",
                                  "failure",
                                  authCode.userId,
                                  "token",
                                  ""
                                );
                                callback(makeError("server_error", "Failed to persist token pair"));
                                return;
                            }
                            Json::Value json;
                            json["access_token"] = tokenStr;
                            json["token_type"] = "Bearer";
                            // P1 #6: advertise the real configured lifetime, not a
                            // hardcoded 3600 (RFC 6749 §5.1 requires expires_in to
                            // be the token's actual remaining lifetime).
                            json["expires_in"] = (Json::Int64)(self->accessTokenTtl_);
                            json["refresh_token"] = refreshTokenStr;
                            json["roles"] = rolesJson;

                            if (
                              self->jwkManager_ && self->jwkManager_->isInitialized() &&
                              scopeContains(authCode.scope, "openid")
                            )
                            {
                                Json::Value idTokenClaims;
                                idTokenClaims["iss"] = self->issuer_;
                                idTokenClaims["sub"] = authCode.userId;
                                idTokenClaims["aud"] = authCode.clientId;
                                idTokenClaims["iat"] = (Json::Int64)now;
                                // P1 #6: id_token exp follows the access token TTL
                                // (OIDC Core §2 requires exp to be the real expiry).
                                idTokenClaims["exp"] = (Json::Int64)(now + self->accessTokenTtl_);
                                if (!authCode.nonce.empty())
                                {
                                    idTokenClaims["nonce"] = authCode.nonce;
                                }
                                // F-022 (OIDC Core §2/§3.1.3.7): auth_time is
                                // REQUIRED when max_age is requested; we always
                                // emit it so RPs can enforce max_age client-side.
                                if (authCode.authTime > 0)
                                {
                                    idTokenClaims["auth_time"] = (Json::Int64)authCode.authTime;
                                }
                                // F-022: acr/amr reflect the authentication
                                // strength. acr "1" = password only, "2" = MFA
                                // (amr contains "mfa"); emit only when amr is set.
                                if (!authCode.amr.empty())
                                {
                                    Json::Value amrArray(Json::arrayValue);
                                    // amr is space-separated on the code; split
                                    // into the JSON array per OIDC Core §2.
                                    size_t s = 0;
                                    while (s < authCode.amr.size())
                                    {
                                        size_t e = authCode.amr.find(' ', s);
                                        if (e == std::string::npos)
                                            e = authCode.amr.size();
                                        if (e > s)
                                            amrArray.append(authCode.amr.substr(s, e - s));
                                        s = e + 1;
                                    }
                                    if (!amrArray.empty())
                                    {
                                        idTokenClaims["amr"] = amrArray;
                                        // MFA if any amr entry is "mfa",
                                        // otherwise password-level only.
                                        bool mfa = false;
                                        for (const auto &v : amrArray)
                                        {
                                            if (v.asString() == "mfa")
                                            {
                                                mfa = true;
                                                break;
                                            }
                                        }
                                        // R-1 (OIDC Core §2): acr is a STRING
                                        // claim, and discovery advertises string
                                        // values "1"/"2" -- emit as string, not
                                        // integer.
                                        idTokenClaims["acr"] = mfa ? "2" : "1";
                                    }
                                }
                                // v1.5.0 M1 (design §2.1 item 3): org_ctx =
                                // the active org + the user's CURRENT roles
                                // in it (O7: the resolver re-checks
                                // membership, so a removed member's refresh
                                // stops carrying org_ctx). orgCtx present
                                // implies authCode.orgId present.
                                if (orgCtx.has_value() && authCode.orgId.has_value())
                                {
                                    Json::Value orgCtxClaim;
                                    orgCtxClaim["org_id"] =
                                      (Json::Int64)*authCode.orgId;
                                    orgCtxClaim["org_name"] = orgCtx->orgName;
                                    Json::Value orgRoles(Json::arrayValue);
                                    for (const auto &r : orgCtx->roles)
                                    {
                                        orgRoles.append(r);
                                    }
                                    orgCtxClaim["roles"] = orgRoles;
                                    idTokenClaims["org_ctx"] = orgCtxClaim;
                                }

                                std::string idToken = self->jwkManager_->signJwt(idTokenClaims);
                                if (!idToken.empty())
                                {
                                    json["id_token"] = idToken;
                                }
                            }

                            self->audit("token_issued", "success", authCode.userId, "token", "");
                            callback(json);
                        }
                      );
                  }
              );
                  }
              );  // resolveOrgContextForClaims (v1.5.0 M1 org_ctx hop)
            }
          );
      }
    );
}

void TokenService::refreshAccessToken(
  const std::string &refreshTokenStr,
  const std::string &clientId,
  std::function<void(const Json::Value &)> &&callback
)
{
    if (!tokens_ || !crypto_)
    {
        callback(makeError("server_error"));
        return;
    }

    auto hashedRt = hashToken(*crypto_, refreshTokenStr);

    auto self = shared_from_this();
    tokens_->atomicRevokeRefreshToken(
      hashedRt,
      [self, callback = std::move(callback), clientId, hashedRt](
        std::optional<fulla::oauth2::model::OAuth2RefreshToken> storedRt
      ) mutable {
          if (!storedRt)
          {
              self->tokens_->getRefreshToken(
                hashedRt,
                [self, callback = std::move(callback)](
                  std::optional<fulla::oauth2::model::OAuth2RefreshToken> maybeRevoked
                ) {
                    if (maybeRevoked && maybeRevoked->revoked && !maybeRevoked->familyId.empty())
                    {
                        self->audit(
                          "refresh_token_reuse_detected",
                          "failure",
                          maybeRevoked->userId,
                          "token_family",
                          maybeRevoked->familyId
                        );
                        self->tokens_->revokeTokenFamily(maybeRevoked->familyId, [callback]() {
                            callback(makeError("invalid_grant", "Token reuse detected"));
                        });
                    }
                    else
                    {
                        callback(makeError("invalid_grant", "Invalid or revoked refresh token"));
                    }
                }
              );
              return;
          }

          if (storedRt->clientId != clientId)
          {
              callback(makeError("invalid_grant", "Client mismatch"));
              return;
          }

          auto now = nowSeconds();
          if (now > storedRt->expiresAt)
          {
              callback(makeError("invalid_grant", "Token expired"));
              return;
          }

          // v1.5.0 M1 (design §2.1 item 4): refresh preserves the org
          // binding along the family and re-resolves org_ctx (O7: a
          // member removed from the org stops getting org_ctx on refresh-
          // issued id_tokens immediately). Nullopt hop when not applicable.
          self->resolveOrgContextForClaims(
            storedRt->userId,
            storedRt->orgId,
            storedRt->scope,
            [self, callback, storedRt, now](
              std::optional<fulla::common::ports::OrgContextInfo> orgCtx
            ) {
          auto newTokenStr = generateSecureToken(*self->crypto_);
          auto newRefreshTokenStr = generateSecureToken(*self->crypto_);
          // P2-1: refuse issuance on CSPRNG failure ("" results).
          if (newTokenStr.empty() || newRefreshTokenStr.empty())
          {
              callback(makeError("server_error", "Token generation failed"));
              return;
          }
          fulla::oauth2::model::OAuth2AccessToken token;
          token.token = hashToken(*self->crypto_, newTokenStr);
          token.clientId = storedRt->clientId;
          token.userId = storedRt->userId;
          token.scope = storedRt->scope;
          // P2 #10: record real issue time for introspection iat.
          token.issuedAt = now;
          token.expiresAt = now + self->accessTokenTtl_;
          // F-016: same issuer stamping as the authorization_code path above.
          token.issuer = self->issuer_;
          // v1.5.0 M1: the org binding survives rotation.
          token.orgId = storedRt->orgId;

          fulla::oauth2::model::OAuth2RefreshToken newRt;
          newRt.token = hashToken(*self->crypto_, newRefreshTokenStr);
          newRt.accessToken = token.token;
          newRt.clientId = storedRt->clientId;
          newRt.userId = storedRt->userId;
          newRt.scope = storedRt->scope;
          newRt.expiresAt = now + self->refreshTokenTtl_;
          newRt.familyId = storedRt->familyId;
          newRt.orgId = storedRt->orgId;

          self->tokens_->saveTokenPair(
            token, newRt, [self, callback, newTokenStr, newRefreshTokenStr, storedRt, now, orgCtx](bool ok) {
                if (!ok)
                {
                    // Same silent-failure guard as exchangeCodeForToken:
                    // never hand out a rotated pair that was never stored.
                    callback(makeError("server_error", "Failed to persist token pair"));
                    return;
                }
                self->audit("token_refreshed", "success", storedRt->userId, "token", "");
                Json::Value json;
                json["access_token"] = newTokenStr;
                json["token_type"] = "Bearer";
                // P1 #6: real configured lifetime, not hardcoded 3600.
                json["expires_in"] = (Json::Int64)self->accessTokenTtl_;
                json["refresh_token"] = newRefreshTokenStr;
                // F-025 (OIDC Core §12): refresh_token grant with an openid
                // scope MUST re-issue an id_token when the server supports
                // OIDC. There is no nonce on refresh (the original request's
                // nonce is not carried forward per §12), and auth_time/amr
                // are omitted (the OAuth2RefreshToken DTO does not persist
                // them; acceptable per §12 -- only sub/aud/iss/iat/exp are
                // required on refresh-issued id_tokens).
                if (
                  self->jwkManager_ && self->jwkManager_->isInitialized() &&
                  scopeContains(storedRt->scope, "openid")
                )
                {
                    Json::Value idTokenClaims;
                    idTokenClaims["iss"] = self->issuer_;
                    idTokenClaims["sub"] = storedRt->userId;
                    idTokenClaims["aud"] = storedRt->clientId;
                    idTokenClaims["iat"] = (Json::Int64)now;
                    idTokenClaims["exp"] = (Json::Int64)(now + self->accessTokenTtl_);
                    // v1.5.0 M1: org_ctx mirrors the code-exchange id_token
                    // (O7 re-resolution; orgCtx present implies the binding).
                    if (orgCtx.has_value() && storedRt->orgId.has_value())
                    {
                        Json::Value orgCtxClaim;
                        orgCtxClaim["org_id"] = (Json::Int64)*storedRt->orgId;
                        orgCtxClaim["org_name"] = orgCtx->orgName;
                        Json::Value orgRoles(Json::arrayValue);
                        for (const auto &r : orgCtx->roles)
                        {
                            orgRoles.append(r);
                        }
                        orgCtxClaim["roles"] = orgRoles;
                        idTokenClaims["org_ctx"] = orgCtxClaim;
                    }
                    std::string idToken = self->jwkManager_->signJwt(idTokenClaims);
                    if (!idToken.empty())
                        json["id_token"] = idToken;
                }
                callback(json);
            }
          );
              }
              );  // resolveOrgContextForClaims (v1.5.0 M1 org_ctx hop)
      }
    );
}

void TokenService::validateAccessToken(
  const std::string &token,
  std::function<void(std::shared_ptr<fulla::oauth2::model::OAuth2AccessToken>)> &&callback
)
{
    if (!tokens_ || !crypto_)
    {
        callback(nullptr);
        return;
    }

    auto hashedToken = hashToken(*crypto_, token);
    // P1 #8 (评审问题点 8, intentional): this async callback captures only
    // [callback] and references neither `this` nor any member -- the body uses
    // only the parameter `t` and the free function nowSeconds(). shared_from_this
    // is therefore deliberately NOT captured (it would be dead weight). If a
    // future change adds member access inside this callback, switch to capturing
    // `auto self = shared_from_this()` to keep lifetime safety -- that is the
    // real latent risk here, not a current one.
    tokens_->getAccessToken(
      hashedToken, [callback](std::optional<fulla::oauth2::model::OAuth2AccessToken> t) {
          if (!t || t->revoked)
          {
              callback(nullptr);
              return;
          }

          if (nowSeconds() > t->expiresAt)
          {
              callback(nullptr);
              return;
          }

          callback(std::make_shared<fulla::oauth2::model::OAuth2AccessToken>(*t));
      }
    );
}

void TokenService::introspectToken(
  const std::string &token,
  std::function<void(std::optional<fulla::oauth2::model::TokenIntrospection>)> &&callback
)
{
    if (!tokens_ || !crypto_)
    {
        callback(std::nullopt);
        return;
    }
    auto hashedToken = hashToken(*crypto_, token);
    tokens_->introspectToken(hashedToken, std::move(callback));
}

void TokenService::revokeAccessToken(
  const std::string &token,
  const std::string &revokedBy,
  std::function<void()> &&callback
)
{
    if (!tokens_ || !crypto_)
    {
        if (callback)
            callback();
        return;
    }
    auto hashedToken = hashToken(*crypto_, token);
    tokens_->revokeAccessToken(hashedToken, revokedBy, [callback = std::move(callback)]() {
        if (callback)
            callback();
    });
}

void TokenService::revokeRefreshToken(
  const std::string &token,
  std::function<void()> &&callback
)
{
    if (!tokens_ || !crypto_)
    {
        if (callback)
            callback();
        return;
    }
    // C3 (RFC 7009 §2.1): hash the raw token before delegating — refresh
    // tokens are stored hashed (same as access tokens), so a revoke that
    // forwards the raw value to the repository is a silent no-op (the store
    // never holds the raw token). This mirrors revokeAccessToken above.
    auto hashedToken = hashToken(*crypto_, token);
    tokens_->revokeRefreshToken(hashedToken, [callback = std::move(callback)]() {
        if (callback)
            callback();
    });
}

bool TokenService::validatePkceCodeVerifier(
  const std::string &codeVerifier,
  const std::string &codeChallenge,
  const std::string &codeChallengeMethod
)
{
    if (!crypto_)
        return false;

    fulla::common::model::PkceChallenge challenge(
      codeChallenge, codeChallengeMethod.empty() ? "plain" : codeChallengeMethod
    );
    return fulla::oauth2::pkce::verifyCodeVerifier(codeVerifier, challenge, *crypto_);
}

}  // namespace fulla::oauth2::protocol
