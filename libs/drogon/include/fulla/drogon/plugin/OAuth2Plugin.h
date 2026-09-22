#pragma once

#include <drogon/plugins/Plugin.h>
#include <fulla/drogon/plugin/OAuth2CleanupService.h>
#include <fulla/drogon/services/IdentityService.h>
#include <fulla/drogon/adapters/StorageRoleProvider.h>
// Phase 1.5d (Task 39): the plugin's 3 identity-side members now hold the NEW
// fulla::identity::* interfaces (Memory/Postgres impls were widened in
// 1.5a-c to be a superset of the legacy oauth2::* shapes). The plugin
// constructs the concrete repos itself (no longer via RepositoryBundle's 3
// identity accessors), so the concrete-impl headers are included here too.
#include <fulla/identity/IRoleRepository.h>
#include <fulla/identity/IUserRepository.h>
#include <fulla/identity/ISubjectMappingRepository.h>
#include <fulla/storage/memory/MemoryIdentityRepository.h>
#include <fulla/storage/postgres/PostgresIdentityRepository.h>
#include <fulla/oauth2/repository/IConsentRepository.h>
#include <fulla/oauth2/model/Dto.h>
#include <fulla/oauth2/jwk/JwkManager.h>
#include <fulla/oauth2/protocol/TokenService.h>
#include <fulla/oauth2/protocol/ClientService.h>
#include <fulla/oauth2/repository/ITokenRepository.h>
#include <fulla/oauth2/repository/IClientRepository.h>
#include <fulla/oauth2/model/Dto.h>
#include <fulla/common/ports/IAuditSink.h>
#include <fulla/common/ports/IMetrics.h>
#include <fulla/oauth2/protocol/AuthorizationService.h>
#include <optional>
#include <string>
#include <memory>
#include <functional>

// M3 Task 24 slice 2 (fulla-sdk-refactor, PROGRESS.md "Task 24 切分
// 方案"): tokenService_/clientService_ now hold the NEW Domain-layer
// classes (fulla::oauth2::protocol::TokenService/ClientService, Task
// 17) instead of the old oauth2::TokenService/ClientService. Every
// forwarding method below (validateClient/generateAuthorizationCode/
// exchangeCodeForToken/etc) keeps its EXACT existing signature -- no
// controller call site changes -- because OAuth2Plugin was already a
// clean forwarding facade (every controller calls plugin->xxx(...), never
// oauth2::TokenService directly). Only OAuth2Plugin.cc's internals change:
// construction goes through a LegacyStorageRepositoryBridge (Task 24
// slice 1) that adapts storage_ (still the old oauth2::IOAuth2Storage) to
// the new split repository interfaces the new services require, and each
// forwarding method's body does the old<->new DTO conversion at the
// boundary. identityService_ (consent/roles/subject-mapping) is
// deliberately UNCHANGED in this slice -- AuthorizationService's
// integration needs ISubjectResolver wiring, which is a separate,
// larger slice (see PROGRESS.md).

class OAuth2Plugin : public drogon::Plugin<OAuth2Plugin>
{
  public:
    // Phase 4.6a: AccessToken/Client aliases now point at the NEW
    // fulla::oauth2::model::* DTOs (was the legacy oauth2::* structs).
    using AccessToken = fulla::oauth2::model::OAuth2AccessToken;
    using Client = fulla::oauth2::model::OAuth2Client;

    OAuth2Plugin() = default;
    void initAndStart(const Json::Value &config) override;
    void shutdown() override;

    // ========== Client seed config (bootstrap/ClientSeeder) ==========

    /** Clients declared in this plugin's config (keyed by client_id); empty
     *  object when none are declared. Read by main.cc's startup seeder
     *  (written once in initAndStart; happens-before the advice thread). */
    const Json::Value &clientsSeedConfig() const
    {
        return clientsSeedConfig_;
    }

    // ========== Service Accessors ==========
    // M3 Task 24 slice 2: these now return the NEW Domain-layer service
    // types. Grep-confirmed zero call sites use these two accessors
    // (unlike getIdentityService()/getStorage()/getJwkManager(), which
    // are used by controllers) -- safe to change return type without a
    // wider ripple.
    std::shared_ptr<fulla::oauth2::protocol::TokenService> getTokenService() const
    {
        return tokenService_;
    }

    std::shared_ptr<fulla::oauth2::protocol::ClientService> getClientService() const
    {
        return clientService_;
    }

    std::shared_ptr<fulla::identity::IdentityService> getIdentityService() const
    {
        return identityService_;
    }

    // Defect 1.5 fix (immutable publish): the JwkManager is published as a
    // std::shared_ptr<const JwkManager>. It is built and init()'d exactly once
    // during initAndStart() (before requests are served); thereafter it is
    // read-only, and the const pointer enforces that at the type level for
    // every holder (this plugin, TokenService, the JWKS controller).
    std::shared_ptr<const fulla::oauth2::JwkManager> getJwkManager() const
    {
        return jwkManager_;
    }

    // P1 #6 (评审问题点 6, RFC 6749 §5.1): expose the configured token TTLs so
    // the controllers' inline issuance paths (client_credentials / device_code)
    // advertise expires_in matching the real token lifetime instead of a
    // hardcoded 3600. Set once during initAndStart() (happens-before requests),
    // read-only thereafter -- same thread-safety reasoning as the members below.
    long long getAccessTokenTtl() const noexcept
    {
        return accessTokenTtl_;
    }

    long long getRefreshTokenTtl() const noexcept
    {
        return refreshTokenTtl_;
    }

    // Returns shared ownership of the storage (defect 1.3 / 1.11 fix). Both
    // overloads return std::shared_ptr<IOAuth2Storage> so callers (e.g. the
    // controller async chains) can capture it and keep the storage alive across
    // async hops instead of holding a raw pointer whose lifetime is implicit.
    // Phase 4.6a: REMOVED -- storage_ (the god IOAuth2Storage) is gone.
    // Controllers now route through the plugin's forwarding methods (getClient
    // / saveAccessToken / saveTokenPair / getUserInfo) over the split repos.

    // ========== Async API with Callbacks ==========

    /**
     * @brief Validate if client exists and secret matches (Async)
     */
    void validateClient(
      const std::string &clientId,
      const std::string &clientSecret,
      std::function<void(bool)> &&callback
    );

    // #233: authorize-only client gate. The authorization endpoint lives on
    // the front channel, where client authentication is optional and a
    // confidential client cannot present its secret (RFC 6749 3.1), so unlike
    // validateClient this never checks a secret. Existence + governance only:
    // getClient resolves soft-deleted (V035) and suspended clients to nullopt.
    void validateClientForAuthorize(
      const std::string &clientId,
      std::function<void(bool)> &&callback
    );

    // Phase 4.3: storage-forwarding accessors routed through the NEW split
    // repository interfaces (today the bridges over storage_) so controllers no
    // longer need to call getStorage() directly. Return/accept the NEW
    // fulla::oauth2::model::* DTOs. These exist alongside getStorage()
    // (kept for remaining callers like getUserInfo, retired in phase 4.5) and
    // will collapse to thin delegations once the god facade is deleted.
    void getClient(
      const std::string &clientId,
      fulla::oauth2::repository::IClientRepository::ClientCallback &&callback
    );
    void saveAccessToken(
      const fulla::oauth2::model::OAuth2AccessToken &token,
      std::function<void()> &&callback
    );
    void saveTokenPair(
      const fulla::oauth2::model::OAuth2AccessToken &accessToken,
      const fulla::oauth2::model::OAuth2RefreshToken &refreshToken,
      // ok == false means the pair was NOT persisted (backend failure or
      // no token repository configured). Callers MUST return an error to
      // the client in that case; the callback is ALWAYS invoked (no
      // request hang), even when tokenRepo_ is absent.
      std::function<void(bool ok)> &&callback
    );
    // Phase 4.5: getUserInfo forwarding (today via storage_; the identity-side
    // migration to fulla::identity::IUserRepository is a separate
    // follow-up). Lets controllers drop their getStorage() reach-in.
    void getUserInfo(
      const std::string &userId,
      std::function<void(std::optional<Json::Value>)> &&callback
    );

    /**
     * @brief Validate redirect URI (Async)
     */
    void validateRedirectUri(
      const std::string &clientId,
      const std::string &redirectUri,
      std::function<void(bool)> &&callback
    );

    // P0-4 audit fix: hard authorization boundary shared by EVERY code
    // issuance path (login, consent approve, MFA verify, authorize silent
    // re-auth). Previously only /oauth2/authorize enforced these, so the
    // other paths minted codes for unknown clients, unregistered
    // redirect_uris (open redirect on success) and scopes outside the
    // client's allowlist. errorCode is an ErrorCatalog code (empty when the
    // guard passes); device_code-style protocol endpoints map it to an
    // OAuth2 error themselves.
    struct CodeIssuanceGuardResult
    {
        bool ok = false;
        std::string errorCode;
        std::string detail;
    };
    using CodeIssuanceGuardCallback = std::function<void(CodeIssuanceGuardResult)>;
    void checkCodeIssuanceGuards(
      const std::string &clientId,
      const std::string &redirectUri,
      const std::string &scope,
      CodeIssuanceGuardCallback &&callback
    );

    /**
     * @brief Generate Authorization Code (Async)
     * @param clientId Client identifier
     * @param subject OAuth2 subject (e.g., "local:alice", "google:sub123")
     * @param scope Requested scopes
     * @param redirectUri Redirect URI
     * @param codeChallenge PKCE code challenge (optional, empty if not
     * provided)
     * @param codeChallengeMethod PKCE code challenge method ("plain", "S256",
     * or empty)
     * @param callback Callback with authorization code or empty string on
     * failure
     */
    void generateAuthorizationCode(
      const std::string &clientId,
      const std::string &subject,
      const std::string &scope,
      const std::string &redirectUri,
      const std::string &codeChallenge,
      const std::string &codeChallengeMethod,
      const std::string &nonce,
      std::function<void(bool, std::string, std::string)> &&callback,
      int64_t authTime = 0,
      const std::string &amr = "",
      const std::optional<int32_t> &orgId = std::nullopt
    );

    /**
     * @brief Exchange Code for Access Token (Async)
     * Returns JSON with {access_token, refresh_token, expires_in} or {error}
     *
     * @param code Authorization code from authorize endpoint
     * @param clientId Client identifier
     * @param clientSecret Client secret (required for CONFIDENTIAL clients,
     * empty for PUBLIC)
     * @param redirectUri Redirect URI from token request (must match
     * authorization request per OAuth2 RFC 6749 Section 4.1.3)
     * @param callback Callback with token response or error
     */
    void exchangeCodeForToken(
      const std::string &code,
      const std::string &clientId,
      const std::string &clientSecret,
      const std::string &redirectUri,
      const std::string &codeVerifier,  // P0-3: PKCE code verifier
      std::function<void(const Json::Value &)> &&callback
    );

    /**
     * @brief Refresh Access Token (Async)
     * Returns JSON with {access_token, refresh_token, expires_in} or {error}
     */
    void refreshAccessToken(
      const std::string &refreshToken,
      const std::string &clientId,
      std::function<void(const Json::Value &)> &&callback
    );

    /**
     * @brief Validate Access Token (Async)
     */
    void validateAccessToken(
      const std::string &token,
      std::function<void(std::shared_ptr<AccessToken>)> &&callback
    );

    /**
     * @brief Get User Roles (Async)
     */
    void getUserRoles(
      const std::string &userId,
      std::function<void(std::vector<std::string>)> &&callback
    );

    // ========== P0-2: Consent Management Methods ==========

    /**
     * @brief Get internal user ID from subject (Async)
     * @param subject OAuth2 subject (e.g., "local:alice", "google:sub123")
     * @param callback Callback with optional internal user ID
     */
    void getInternalUserId(
      const std::string &subject,
      std::function<void(std::optional<int32_t>)> &&callback
    );

    /**
     * @brief Check if user has consented to a scope for a client (Async)
     * @param internalUserId Internal user ID
     * @param clientId Client identifier
     * @param scope Scope to check consent for
     * @param callback Callback with consent status
     */
    void hasUserConsent(
      int32_t internalUserId,
      const std::string &clientId,
      const std::string &scope,
      std::function<void(bool)> &&callback
    );

    /**
     * @brief Save user consent for a scope (Async)
     * @param internalUserId Internal user ID
     * @param clientId Client identifier
     * @param scope Scope to save consent for
     * @param callback Callback with success status
     */
    void saveUserConsent(
      int32_t internalUserId,
      const std::string &clientId,
      const std::string &scope,
      std::function<void(bool)> &&callback
    );

    // ========== P0-3: PKCE Validation Methods ==========

    /**
     * @brief Validate PKCE code verifier against challenge
     * @param codeVerifier Code verifier from token request
     * @param codeChallenge Code challenge from authorization request
     * @param codeChallengeMethod Challenge method ("plain" or "S256")
     * @return true if verifier is valid, false otherwise
     */
    static bool validatePkceCodeVerifier(
      const std::string &codeVerifier,
      const std::string &codeChallenge,
      const std::string &codeChallengeMethod
    );

    /**
     * @brief Generate SHA-256 hash for PKCE S256 method
     * @param input String to hash
     * @return Base64-url encoded hash
     */
    static std::string generateSha256Hash(const std::string &input);

    // ========== P0-5: Scope Permission Control Methods ==========

    /**
     * @brief Validate requested scopes against client allowlist (Tier 1)
     * @param clientId Client identifier
     * @param requestedScopes Scopes requested by client
     * @param callback Callback with validation result and error message
     */
    void validateClientScopes(
      const std::string &clientId,
      const std::vector<std::string> &requestedScopes,
      std::function<void(bool, std::string)> &&callback
    );

    /**
     * @brief Validate user roles for admin scopes (Tier 2)
     * @param userId User identifier (subject)
     * @param scopes Requested scopes
     * @param callback Callback with validation result and error message
     */
    void validateUserRolesForScopes(
      const std::string &userId,
      const std::vector<std::string> &scopes,
      std::function<void(bool, std::string)> &&callback
    );

    /**
     * @brief Check if scope requires admin role
     * @param scope Scope to check
     * @return true if scope requires admin role, false otherwise
     */
    static bool scopeRequiresAdminRole(const std::string &scope);

    // ========== P1: Token Introspection (RFC 7662) ==========

    /**
     * @brief Introspect token metadata (RFC 7662) (Async)
     * @param token The access token to introspect
     * @param callback Callback with token introspection metadata or nullopt if invalid
     */
    void introspectToken(
      const std::string &token,
      std::function<void(std::optional<fulla::oauth2::model::TokenIntrospection>)> &&callback
    );

    /**
     * @brief Increment introspection count for monitoring (Async)
     * @param token The access token
     * @param callback Callback invoked when update completes
     */
    void incrementIntrospectCount(const std::string &token, std::function<void()> &&callback);

    // ========== P1: Token Revocation (RFC 7009) ==========

    /**
     * @brief Revoke access token with audit trail (RFC 7009) (Async)
     * @param token The access token to revoke
     * @param revokedBy Client ID performing the revocation
     * @param callback Callback invoked when revocation completes
     */
    void revokeAccessToken(
      const std::string &token,
      const std::string &revokedBy,
      std::function<void()> &&callback
    );

    /**
     * @brief Revoke a refresh token (RFC 7009 §2.1 — the revocation endpoint
     * must revoke ANY token type). Used by the revoke handler after
     * revokeAccessToken so a refresh token presented to /oauth2/revoke is
     * actually revoked (C3 fix).
     */
    void revokeRefreshToken(
      const std::string &token,
      std::function<void()> &&callback
    );

    // ========== Storage Access ==========
    // Phase 4.6a: getStorage() (god-IOAuth2Storage accessor) removed; storage_
    // is gone. Use the split-repo forwarding methods above.

    const std::string &getStorageType() const
    {
        return storageType_;
    }

    // F-016: the configured issuer URL (custom config metadata.issuer, default
    // http://localhost:5555, trailing slash normalized at startup). Read once
    // in initAndStart(); controllers use it to stamp issuer on access tokens
    // (client_credentials/device paths) and to backfill introspection iss when
    // a storage backend returns none -- keeping it byte-identical to the
    // discovery document's issuer.
    const std::string &getIssuer() const
    {
        return issuer_;
    }

    // ========== Observability Ports (M8 Task 40, decision b) ==========
    // Expose the Adapter-side IAuditSink / IMetrics instances so Drogon-layer
    // code (libs/drogon controllers) can emit audit events / metrics through
    // the fulla::common::ports port instead of calling AuditLogger/Metrics
    // statics directly. Controllers fetch these via
    // drogon::app().getPlugin<OAuth2Plugin>()->getAuditSink(). May return
    // nullptr if called before initAndStart() completes (controllers guard).
    std::shared_ptr<fulla::common::ports::IAuditSink> getAuditSink() const
    {
        return auditSink_;
    }

    std::shared_ptr<fulla::common::ports::IMetrics> getMetrics() const
    {
        return metrics_;
    }

    // B10 / Task 45: the Domain-layer authorization engine, exposed so
    // AuthorizationEndpointController can call evaluateScopes() (replacing its
    // old inline 3-tier chain). May return nullptr before initAndStart().
    std::shared_ptr<fulla::oauth2::protocol::AuthorizationService>
    getAuthorizationService() const
    {
        return authorizationService_;
    }

    // F-025 (OIDC Core §12): sign an id_token for the refresh_token and
    // device_code grant paths (which issue tokens outside TokenService and
    // therefore cannot reuse its inline id_token signing). Builds the
    // standard claims (iss/sub/aud/iat/exp) and signs with the configured
    // JwkManager. Returns an empty string when the JwkManager is not
    // initialized (callers then omit id_token, matching the
    // authorization_code path's behavior). authTime/amr, when non-default,
    // are carried over so RPs that requested max_age still get auth_time on
    // refresh-issued id_tokens.
    std::string signIdToken(
      const std::string &subject,
      const std::string &clientId,
      int64_t authTime = 0,
      const std::string &amr = ""
    ) const;

  private:
    // Phase 4.6a: storage_ (the god IOAuth2Storage) is gone. The plugin now
    // holds the split-repository handles directly, extracted from the
    // per-backend RepositoryBundle constructed in initStorage().
    std::shared_ptr<fulla::oauth2::repository::IClientRepository> clientRepo_;
    std::shared_ptr<fulla::oauth2::repository::IGrantRepository> grantRepo_;
    std::shared_ptr<fulla::oauth2::repository::ITokenRepository> tokenRepo_;
    std::shared_ptr<fulla::oauth2::repository::IConsentRepository> consentRepo_;
    // Phase 1.5d (Task 39): identity-side members now hold the NEW
    // fulla::identity::* interfaces. All three are populated from a
    // single PostgresIdentityRepository / MemoryIdentityRepository instance
    // (each concrete class multiply-inherits all 3 interfaces), so the
    // shared_ptr conversions are pointer-adjusted aliases of one object.
    std::shared_ptr<fulla::identity::IRoleRepository> roleRepo_;
    std::shared_ptr<fulla::identity::IUserRepository> userRepo_;
    std::shared_ptr<fulla::identity::ISubjectMappingRepository> subjectMappingRepo_;
    std::shared_ptr<fulla::drogon::OAuth2CleanupService> cleanupService_;
    std::shared_ptr<fulla::oauth2::protocol::TokenService> tokenService_;
    std::shared_ptr<fulla::oauth2::protocol::ClientService> clientService_;
    std::shared_ptr<fulla::identity::IdentityService> identityService_;
    // M2b Task 17 slice 12: first production instantiation of
    // fulla::common::ports::IRoleProvider (via the Adapter-side
    // StorageRoleProvider, backed by storage_). Not yet consumed by any
    // caller -- IdentityService's role lookups are keyed by subject
    // string today, not the internalUserId this port takes, so wiring it
    // into an actual call site requires ISubjectResolver too (a later
    // slice). Constructed here so the Adapter class has a real production
    // instantiation point once that wiring happens.
    std::shared_ptr<fulla::drogon::adapters::StorageRoleProvider> roleProvider_;
    std::shared_ptr<const fulla::oauth2::JwkManager> jwkManager_;
    // M8 Task 40 decision b: Adapter-side observability ports, exposed via
    // getAuditSink()/getMetrics() for Drogon-layer consumers.
    std::shared_ptr<fulla::common::ports::IAuditSink> auditSink_;
    std::shared_ptr<fulla::common::ports::IMetrics> metrics_;
    // B10 / Task 45: authorization engine + its subject resolver.
    std::shared_ptr<fulla::common::ports::ISubjectResolver> subjectResolver_;
    std::shared_ptr<fulla::oauth2::protocol::AuthorizationService> authorizationService_;

    std::string storageType_;

    // F-016: configured issuer, read once in initAndStart() (see getIssuer()).
    std::string issuer_;

    // TTL Configuration (Seconds)
    // Note: These are set once during initAndStart() and only read afterwards
    // Thread-safe due to happens-before guarantee (init before requests)
    long long authCodeTtl_{600};
    long long accessTokenTtl_{3600};
    long long refreshTokenTtl_{3600 * 24 * 30};

    // #204: the "clients" object of the plugin config, captured during
    // initAndStart() for the bootstrap::ClientSeeder startup pass (postgres
    // mode). Happens-before safe: written in initAndStart(), read from the
    // beginning-advice thread after the event loop starts.
    Json::Value clientsSeedConfig_;

    void initStorage(const Json::Value &config);

    // ========== Subject Mapping Methods ==========

    /**
     * @brief Ensure subject mapping exists, create if needed
     * @param subject Full subject (e.g., "local:alice")
     * @param username Original username for logging
     * @param internalUserId Internal user ID from users table
     * @param callback Callback invoked when mapping is ensured
     */
    void ensureSubjectMapping(
      const std::string &subject,
      const std::string &username,
      int32_t internalUserId,
      std::function<void()> &&callback
    );

    /**
     * @brief Handle first-time login for new users
     * @param subject Full subject
     * @param provider Provider name
     * @param callback Callback with internal user ID or 0 if failed
     */
    void handleFirstTimeLogin(
      const std::string &subject,
      const std::string &provider,
      std::function<void(int32_t)> &&callback
    );
};
