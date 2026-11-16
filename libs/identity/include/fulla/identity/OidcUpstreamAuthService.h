// OidcUpstreamAuthService.h
//
// P2-1: the generic OIDC-in federation flow for registry-configured
// upstream providers (upstream_providers rows): discovery -> authorize URL
// -> code exchange -> userinfo -> claims mapping -> find-or-create the
// local account via the SAME AccountLinkFlow the google/wechat/github
// services use (oauth2_subject_mappings.provider = provider_key).
//
// HTTP is injected through two transport seams (HttpGet / HttpPostForm) so
// the whole protocol is unit-testable without a live upstream; the drogon
// wiring lives in the adapter layer (IdentityAssembly), production simply
// passes HttpClient-backed lambdas.
//
// Identity source in this slice is the userinfo endpoint (claim mapping);
// id_token validation (JWKS/nonce) is deliberately deferred to the P2-1
// hardening batch -- userinfo over TLS with the state-binding guard is the
// baseline guarantee, matching how the social providers behave today.
//
// Lifetime: domain service -- continuations capture `self`
// (shared_from_this), never `this` (see .claude/rules/db-operations.md).

#pragma once

#ifdef WITH_SOCIAL

#include <fulla/identity/IUpstreamProviderRepository.h>
#include <fulla/identity/ISocialAccountRepository.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fulla::identity
{

class OidcUpstreamAuthService : public std::enable_shared_from_this<OidcUpstreamAuthService>
{
  public:
    // ---- transport seams -------------------------------------------------
    // ok=false covers transport-level failure (unreachable/reset/timeout).
    using FetchCallback = std::function<void(bool ok, int status, std::string body)>;
    using HttpGet =
      std::function<void(const std::string &url, const std::string &bearer, FetchCallback &&cb)>;
    using HttpPostForm = std::function<
      void(const std::string &url,
           const std::vector<std::pair<std::string, std::string>> &form,
           FetchCallback &&cb)>;

    struct OidcEndpoints
    {
        std::string authorize;
        std::string token;
        std::string userinfo;
    };

    struct LoginResult
    {
        int32_t userId = 0;
        std::string username;
        std::string publicSub;
        std::string errorCode;  // empty on success; existing catalog codes only
        bool isNewUser = false;
    };

    OidcUpstreamAuthService(std::shared_ptr<ISocialAccountRepository> accountRepo,
                            bool autoCreate,
                            HttpGet httpGet,
                            HttpPostForm httpPostForm);

    // Resolve a provider's three endpoints: endpoint_overrides win over
    // discovery; discovery is cached per issuer (10 min TTL). ok=false means
    // neither source produced a usable set.
    void resolveEndpoints(const UpstreamProviderRecord &record,
                          std::function<void(bool ok, OidcEndpoints endpoints)> &&cb);

    // Pure URL builder (synchronous; caller supplies pre-resolved endpoints
    // so the request path never blocks on discovery).
    static std::string buildAuthorizeUrl(const UpstreamProviderRecord &record,
                                         const OidcEndpoints &endpoints,
                                         const std::string &redirectUri,
                                         const std::string &state);

    // Full login: token exchange -> userinfo -> claims mapping ->
    // resolve-or-create. Error codes reuse the existing catalog
    // (NET_CONNECTION_FAILED / VALIDATION_INVALID_INPUT / the
    // AccountLinkFlow set).
    void login(const UpstreamProviderRecord &record,
               const OidcEndpoints &endpoints,
               const std::string &redirectUri,
               const std::string &code,
               std::function<void(LoginResult)> &&cb);

  private:
    // Normalized upstream identity after claims mapping.
    struct UpstreamIdentity
    {
        std::string subject;
        std::string email;
        std::string username;  // seed for local username generation
        std::string displayName;
    };

    // claimsMappingJson empty/invalid -> spec defaults
    // (sub/email/preferred_username/name). Returns false when `sub` cannot
    // be resolved (the only hard-required claim).
    static bool mapClaims(const std::string &claimsMappingJson,
                          const std::string &userinfoBody,
                          UpstreamIdentity &out);

    void fetchUserinfo(const UpstreamProviderRecord &record,
                       const OidcEndpoints &endpoints,
                       const std::string &accessToken,
                       std::shared_ptr<std::function<void(LoginResult)>> sharedCb);

    std::shared_ptr<ISocialAccountRepository> accountRepo_;
    bool autoCreate_ = true;
    HttpGet httpGet_;
    HttpPostForm httpPostForm_;

    // Discovery cache: issuer -> {endpoints, fetchedAt}
    std::mutex discoveryMutex_;
    struct CachedDiscovery
    {
        OidcEndpoints endpoints;
        bool valid = false;
        std::chrono::steady_clock::time_point fetchedAt{};
    };
    std::map<std::string, CachedDiscovery> discoveryCache_;
};

}  // namespace fulla::identity

#endif  // WITH_SOCIAL
