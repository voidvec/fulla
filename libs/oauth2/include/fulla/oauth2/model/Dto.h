#pragma once

// Task 17 slice 2 (fulla-sdk-refactor, design.md §6/§8 "Data Models" /
// "DTO 结构（框架无关，放 Domain）"): ports the framework-agnostic DTOs
// currently nested inside OAuth2Plugin/include/oauth2/storage/
// IOAuth2Storage.h (OAuth2Client / OAuth2AuthCode / OAuth2AccessToken /
// OAuth2RefreshToken / TokenIntrospection / AuthorizationTransaction) into
// fulla::oauth2::model, per design.md's explicit note: "当前定义在
// OAuth2Plugin/include/oauth2/storage/IOAuth2Storage.h（不是
// OAuth2Types.h）——M2b（Task 17）迁移时以此为源".
//
// Fields are unchanged (design.md: "字段不变，按聚合归位到 oauth2 Domain
// 包"). This slice deliberately does NOT yet wrap fields in the value
// objects design.md's table calls for (Scope/ClientId/RedirectUri/
// PkceChallenge/TokenValue) -- that would require touching every
// production call site that constructs/reads these structs, which is a
// separate, larger slice (mirroring Task 14's call-site-by-call-site
// discipline). This slice only relocates the plain-struct shape so
// AuthorizationService/TokenService (a later slice) and the repository
// interfaces (next slice) have a Drogon-free, OAuth2Plugin-independent
// home to depend on.
//
// This header is purely additive: IOAuth2Storage.h is untouched, and no
// production call site has been switched to these types yet.
//
// TokenIntrospection::toJson() is ported here using Json::Value (jsoncpp),
// which libs/oauth2 has transitively available via fulla::common
// (design.md §4.1 rule 1 explicitly allows jsoncpp in the Domain layer).
//
// Naming note (design.md §5.8 "文件名 = 主类名", Phase 6 decision): this
// header intentionally stays an aggregate of six closely-related plain DTO
// structs and is exempt from the one-class-per-file rule — splitting it into
// six single-struct headers would touch 13 includers for zero behavioral
// gain. These are hand-written Domain DTOs, NOT drogon_ctl-generated ORM
// models (those live in libs/storage-postgres/**/models/ and must never be
// hand-edited).

#include <fulla/oauth2/model/ClientType.h>

#include <json/json.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fulla::oauth2::model
{

/**
 * @brief OAuth2 Client data structure.
 */
struct OAuth2Client
{
    std::string clientId;
    ClientType clientType;
    std::string clientSecretHash;
    std::string salt;
    std::vector<std::string> redirectUris;
    std::vector<std::string> allowedScopes;
    // F-017 (RFC 7591 §2 / RFC 6749 §3.2.1): the client's declared token-
    // endpoint authentication method. "" / unset preserves the legacy lenient
    // Basic->body fallback (NULL column); explicit values are enforced at
    // token/introspect/revoke (client_secret_basic | client_secret_post | none).
    std::string tokenEndpointAuthMethod;
    // #220 (RFC 6749 §3.2.1): the client's registered grant types. Empty =
    // legacy/unregistered row, all grants allowed (grandfathered); non-empty
    // lists are enforced at the token endpoint's grant dispatch gate, where
    // refresh_token is implied by an authorization_code or device_code
    // registration (those flows mint refresh tokens unconditionally).
    std::vector<std::string> allowedGrantTypes;
};

/**
 * @brief Authorization Code data structure.
 */
struct OAuth2AuthCode
{
    std::string code;
    std::string clientId;
    std::string userId;
    std::string scope;
    std::string redirectUri;
    std::string codeChallenge;        // PKCE support
    std::string codeChallengeMethod;  // "plain" or "S256"
    std::string nonce;                // OIDC nonce (anti-replay)
    int64_t expiresAt;                // Unix timestamp (seconds)
    bool used = false;
    // F-021/F-022 (OIDC Core §2/§3.1.3.7): carried from the login session
    // (set on session at login time) to the id_token at code exchange.
    // authTime = epoch seconds of the user's most recent authentication;
    // amr = space-separated Authentication Method References ("pwd", "mfa").
    int64_t authTime = 0;
    std::string amr;
};

/**
 * @brief Access Token data structure (extended for P1 features).
 */
struct OAuth2AccessToken
{
    std::string token;
    std::string clientId;
    std::string userId;
    std::string scope;
    int64_t expiresAt;  // Unix timestamp (seconds)
    bool revoked = false;

    // P1: RFC 7662 Token Introspection fields
    int64_t issuedAt = 0;     // Unix timestamp when token was issued (iat)
    std::string issuer;       // Issuer identifier (iss)
    std::string audience;     // Audience identifier (aud)
    int64_t notBefore = 0;    // Token not valid before (nbf)
    int introspectCount = 0;  // Number of introspection requests
    int64_t revokedAt = 0;    // Unix timestamp when token was revoked
    std::string revokedBy;    // Client ID that revoked the token
};

/**
 * @brief Refresh Token data structure (extended for P1 features).
 */
struct OAuth2RefreshToken
{
    std::string token;
    std::string accessToken;
    std::string clientId;
    std::string userId;
    std::string scope;
    int64_t expiresAt;
    bool revoked = false;
    std::string familyId;  // Token family for reuse detection

    // P1: Token Revocation audit fields (RFC 7009)
    int64_t revokedAt = 0;  // Unix timestamp when token was revoked
    std::string revokedBy;  // Client ID that revoked the token
};

/**
 * @brief Token Introspection response (RFC 7662).
 */
struct TokenIntrospection
{
    bool active = false;               // Whether the token is currently active
    std::string clientId;              // Client ID that was issued the token
    std::string tokenType = "Bearer";  // Token type (always "Bearer" for OAuth 2.0)
    int64_t exp = 0;                   // Expiration time (exp)
    int64_t iat = 0;                   // Issued at time (iat)
    int64_t nbf = 0;                   // Not before time (nbf)
    std::string sub;                   // Subject (user ID)
    std::string aud;                   // Audience (client ID)
    std::string iss;                   // Issuer (authorization server URL)
    std::string scope;                 // Granted scopes

    /**
     * @brief Convert to JSON for HTTP response.
     */
    Json::Value toJson() const
    {
        Json::Value json;
        json["active"] = active;

        if (active)
        {
            json["client_id"] = clientId;
            json["token_type"] = tokenType;

            if (exp > 0)
                json["exp"] = static_cast<Json::Int64>(exp);
            if (iat > 0)
                json["iat"] = static_cast<Json::Int64>(iat);
            if (nbf > 0)
                json["nbf"] = static_cast<Json::Int64>(nbf);

            if (!sub.empty())
                json["sub"] = sub;
            if (!aud.empty())
                json["aud"] = aud;
            if (!iss.empty())
                json["iss"] = iss;
            if (!scope.empty())
                json["scope"] = scope;
        }

        return json;
    }
};

/**
 * @brief Authorization Transaction for the consent flow (the
 * `AuthorizationGrant` aggregate per design.md's Data Models table).
 * Preserves complete OAuth2 authorization context across the user consent
 * interaction.
 */
struct AuthorizationTransaction
{
    std::string transactionId;
    std::string clientId;
    std::string subject;
    std::string redirectUri;
    std::string state;
    std::string codeChallenge;
    std::string codeChallengeMethod;
    std::vector<std::string> requestedScopes;
    std::vector<std::string> validScopes;
    std::vector<std::string> consentRequiredScopes;
    bool consumed = false;
    int64_t expiresAt;
};

}  // namespace fulla::oauth2::model
