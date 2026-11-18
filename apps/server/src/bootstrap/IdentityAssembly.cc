#include "IdentityAssembly.h"

#include <fulla/common/observability/AuditEvent.h>
#include <fulla/drogon/adapters/BackchannelLogoutNotifier.h>
#include <fulla/drogon/adapters/DrogonOAuthHttpClient.h>
#include <fulla/drogon/observability/AuditLogger.h>
#include <fulla/drogon/controllers/AuthProvidersController.h>
#ifdef WITH_SOCIAL
#include <fulla/drogon/controllers/GitHubController.h>
#include <fulla/drogon/controllers/GoogleController.h>
#include <fulla/drogon/controllers/WeChatController.h>
#endif  // WITH_SOCIAL
#include <fulla/drogon/controllers/MfaController.h>
#include <fulla/drogon/controllers/SessionController.h>
#include <fulla/drogon/controllers/UserSelfServiceController.h>
#ifdef WITH_WEBAUTHN
#include <fulla/drogon/controllers/WebAuthnController.h>
#endif  // WITH_WEBAUTHN
#include <fulla/identity/AuthService.h>
#include <fulla/identity/IMfaRepository.h>
#include <fulla/identity/IWebAuthnRepository.h>
#include <fulla/identity/MfaService.h>
#include <fulla/identity/SessionManager.h>
#include <fulla/identity/SocialAuthService.h>
#include <fulla/identity/SocialLinkService.h>
#include <fulla/identity/WebAuthnService.h>
#include <fulla/drogon/adapters/RedisSocialLinkStateStore.h>
#include <fulla/drogon/adapters/RedisSocialLoginStateStore.h>
#include <fulla/drogon/services/SocialLoginGuard.h>
#include <fulla/common/utils/SocialAuthConfig.h>
#include <fulla/common/config/ConfigManager.h>
#include <fulla/storage/postgres/PostgresIdentityRepository.h>
#include <fulla/storage/postgres/PostgresMfaRepository.h>
#include <fulla/storage/postgres/PostgresSocialAccountRepository.h>
#include <fulla/storage/postgres/PostgresUpstreamProviderRepository.h>
#include <fulla/storage/postgres/PostgresWebAuthnRepository.h>
#include <drogon/DrClassMap.h>
#include <drogon/drogon.h>
#ifdef WITH_SOCIAL
// P2-1: OIDC-in upstream federation (registry runtime + controllers).
#include <fulla/drogon/controllers/UpstreamLoginController.h>
#include <fulla/drogon/controllers/UpstreamProviderAdminController.h>
#include <fulla/drogon/services/UpstreamProviderRuntime.h>
#include <fulla/identity/OidcUpstreamAuthService.h>
#endif  // WITH_SOCIAL

#include <string>
#include <vector>
#include <fulla/drogon/adapters/OpenSslCryptoProvider.h>
#include <fulla/drogon/adapters/SystemClock.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>

#include <memory>

namespace bootstrap
{

void wireIdentityServices([[maybe_unused]] const Json::Value &runtimeConfig)
{
    // Memory-only config (e.g. config.ci.json: "storage_type":"memory",
    // "db_clients":[]) has NO default DbClient. Drogon's
    // DbClientManager::getDbClient() hits a hard `assert(dbClientsMap_.find(
    // name) != dbClientsMap_.end())` -- a process-terminating assert, NOT a
    // catchable throw -- so the `if (!dbClient)` check below would never be
    // reached. Guard with the storage-type check BEFORE the getDbClient()
    // call (same pattern as ContractFixtures.h::getPostgresClientOrNull()).
    // SessionController/Mfa/WebAuthn/Social then stay on their legacy
    // (non-injected) fallback paths, as already documented for the no-DB
    // case. This also fixes a real memory-storage server startup crash, not
    // just the test leg.
    auto plugin = drogon::app().getPlugin<::OAuth2Plugin>();
    if (plugin && plugin->getStorageType() == "memory")
    {
        LOG_WARN << "wireIdentityServices: memory storage (no DB client) -- "
                    "SessionController/Mfa/WebAuthn/Social stay on legacy paths";
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    if (!dbClient)
    {
        LOG_WARN << "wireIdentityServices: no default DB client configured; "
                    "SessionController falls back to the legacy "
                    "fulla::drogon::services::AuthService path";
        return;
    }

    auto userRepo =
      std::make_shared<fulla::storage::postgres::PostgresIdentityRepository>(dbClient);
    auto crypto = std::make_shared<fulla::drogon::adapters::OpenSslCryptoProvider>();
    auto clock = std::make_shared<fulla::drogon::adapters::SystemClock>();

    // Task 24 slice 5: MfaService, sharing the same IUserRepository/
    // ICryptoProvider/IClock instances constructed above (MfaService only
    // needs its own IMfaRepository -- a distinct interface/table-column
    // scope, see IMfaRepository.h's header comment -- not a new crypto/
    // clock).
    auto mfaRepo = std::make_shared<fulla::storage::postgres::PostgresMfaRepository>(dbClient);

    // custom_config is read by both WebAuthn (rp_id/rp_name) and Social
    // (external_auth.*) config below; read it once here (only the
    // feature-specific sub-blocks are guarded).
    auto customConfig = ::drogon::app().getCustomConfig();

    // Task 24 slice 5: WebAuthnService. rp_id/rp_name mirror
    // WebAuthnController.cc's own getRpId()/getRpName() config reads
    // (custom_config "webauthn" block, defaulting to "localhost"/"OAuth2
    // Server") -- read once here at startup instead of per-request, same
    // pattern as OAuth2Plugin.cc's own issuer/TTL config reads.
#ifdef WITH_WEBAUTHN
    auto webAuthnRepo =
      std::make_shared<fulla::storage::postgres::PostgresWebAuthnRepository>(dbClient);
    std::string rpId = "localhost";
    std::string rpName = "Fulla";
    std::vector<std::string> rpOrigins;  // #142: strict origin allowlist
    if (customConfig.isMember("webauthn"))
    {
        rpId = customConfig["webauthn"].get("rp_id", rpId).asString();
        rpName = customConfig["webauthn"].get("rp_name", rpName).asString();
        if (customConfig["webauthn"].isMember("rp_origins") &&
            customConfig["webauthn"]["rp_origins"].isArray())
        {
            for (const auto &o : customConfig["webauthn"]["rp_origins"])
                if (o.isString() && !o.asString().empty())
                    rpOrigins.push_back(o.asString());
        }
    }
#endif  // WITH_WEBAUTHN

    // Owned for the lifetime of the process (same static-local-in-a-
    // free-function pattern OAuth2Plugin.cc's static jwkManagerLogger
    // uses) -- SessionController/MfaController hold non-owning raw
    // pointers to these (see SessionController.h's/MfaController.h's
    // setIdentityAuthService()/setSessionManager()/setMfaService()/
    // setUserRepository() comments), and this function's only caller
    // (main.cc's registerBeginningAdvice) runs exactly once at startup.
    static auto authService =
      std::make_shared<fulla::identity::AuthService>(userRepo, crypto, clock);
    // #103: auth.allow_legacy_hash — legacy unsalted-SHA256 verification
    // window, CLOSED by default: a missing key explicitly disables the
    // gate here (AuthService.h's field initializer stays true only for
    // the api-diff SDK baseline; assembly semantics are authoritative).
    // Operators reopen the window explicitly to let legacy users log in
    // and be transparently rehashed to PBKDF2 (docs/operate/
    // configuration-guide.md, "legacy hash migration window").
    {
        const auto &custom = ::drogon::app().getCustomConfig();
        bool allowLegacy = false;
        if (custom.isMember("auth") && custom["auth"].isMember("allow_legacy_hash"))
            allowLegacy = custom["auth"]["allow_legacy_hash"].asBool();
        authService->setAllowLegacyHash(allowLegacy);
        // Observability (#103): surface legacy-format rejections to the
        // server log (the domain layer stays logging-free). Response body
        // stays the generic AUTH_INVALID_CREDENTIALS — no oracle.
        authService->setLegacyHashRejectionNotifier([](int32_t internalUserId) {
            LOG_WARN << "AUTH_LEGACY_HASH_REJECTED: login denied for user "
                     << internalUserId
                     << " (stored hash is legacy-format and auth.allow_legacy_hash is "
                        "false; migrate via password reset or reopen the window — see "
                        "docs/operate/configuration-guide.md)";
        });
    }
    // B1: shared outbound-HTTP client (Drogon-backed) used by both the
    // backchannel-logout notifier and, when built, the social auth services.
    static auto oauthHttpClient =
      std::make_shared<fulla::drogon::adapters::DrogonOAuthHttpClient>();
    // B1 (OIDC Back-Channel Logout 1.0): real notifier -- finds each relying
    // party with an active session + a registered backchannel_logout_uri and
    // POSTs a signed logout_token, fire-and-forget. If the plugin's signing
    // key / audit sink is unavailable, notify() degrades to a no-op fan-out
    // (the logout flow is never blocked). plugin is initialized by the time
    // this runs (getStorageType() above already relied on initAndStart()).
    static auto notifier =
      std::make_shared<fulla::drogon::adapters::BackchannelLogoutNotifier>(
        dbClient,
        plugin ? plugin->getJwkManager() : nullptr,
        plugin ? plugin->getIssuer() : std::string{},
        oauthHttpClient,
        plugin ? plugin->getAuditSink() : nullptr);
    static auto sessionManager = std::make_shared<fulla::identity::SessionManager>(notifier);
    // TOTP issuer shown by authenticator apps (custom_config.mfa.totp_issuer,
    // env override FULLA_MFA_TOTP_ISSUER); "Fulla" instead of the historical
    // "OAuth2Server" default, which is what users saw in their authenticator.
    std::string totpIssuer = "Fulla";
    if (customConfig.isMember("mfa") && customConfig["mfa"].isMember("totp_issuer"))
    {
        const std::string configured = customConfig["mfa"]["totp_issuer"].asString();
        if (!configured.empty())
            totpIssuer = configured;
    }
    static auto mfaService =
      std::make_shared<fulla::identity::MfaService>(mfaRepo, crypto, clock, totpIssuer);
#ifdef WITH_WEBAUTHN
    static auto webAuthnService =
      std::make_shared<fulla::identity::WebAuthnService>(webAuthnRepo, crypto, rpId, rpName);
    // #142: strict origin allowlist — verification fails closed while
    // empty (an unconfigured deployment must not accept assertions from
    // ANY origin). Clone detection surfaces as an audit action (the
    // domain service stays logging-free).
    webAuthnService->setRpOrigins(rpOrigins);
    webAuthnService->setCloneDetectorNotifier([](int32_t userId, const std::string &credentialId) {
        LOG_WARN << "WEBAUTHN_CLONE_DETECTED: user " << userId << " credential "
                 << credentialId
                 << " presented a non-increasing signCount — assertion rejected "
                    "(possible cloned authenticator)";
        fulla::common::observability::AuditEvent event;
        event.action = "webauthn_clone_detected";
        event.outcome = "failure";
        event.actorType = "user";
        event.actorId = std::to_string(userId);
        event.targetType = "credential";
        event.targetId = credentialId;
        ::fulla::drogon::observability::AuditLogger::log(event);
    });
    if (rpOrigins.empty())
        LOG_WARN << "IdentityAssembly: webauthn.rp_origins is empty — WebAuthn "
                    "registration/authentication finish will fail closed until it "
                    "is configured (see docs/operate/configuration-guide.md)";
#endif  // WITH_WEBAUTHN

#ifdef WITH_SOCIAL
    // Task 24 slice 5: Social auth services (Google/WeChat/GitHub). Config
    // keys mirror each pre-Task-24 controller's own getXxxConfig() reads
    // (custom_config "external_auth.{google,wechat,github}" blocks) --
    // read once here at startup instead of per-request.
    // #111: a provider whose required credentials are absent or still carry
    // the legacy YOUR_* template placeholders is DISABLED at startup (its
    // service is not injected): misconfigured providers fail at request time
    // with a clear NotConfigured error instead of phoning an upstream that
    // can never accept them.
    const auto credentialConfigured = [](const std::string &value) {
        return fulla::common::utils::socialCredentialConfigured(value);
    };
    std::string googleClientId, googleClientSecret, googleRedirectUri;
    std::string wechatAppId, wechatSecret;
    std::string githubClientId, githubClientSecret;
    // v1.4.0 provider tiers (external_auth.tiers.*): domestic gates WeChat,
    // international gates GitHub/Google. Defaults true so a config without
    // the block (or without external_auth at all) keeps every configured
    // provider enabled — upgrade compatibility; only an explicit false
    // turns a tier off.
    bool domesticTier = true;
    bool internationalTier = true;
    if (customConfig.isMember("external_auth"))
    {
        const auto &externalAuth = customConfig["external_auth"];
        if (externalAuth.isMember("google"))
        {
            googleClientId = externalAuth["google"].get("client_id", "").asString();
            googleClientSecret = externalAuth["google"].get("client_secret", "").asString();
            googleRedirectUri = externalAuth["google"].get("redirect_uri", "").asString();
        }
        if (externalAuth.isMember("wechat"))
        {
            wechatAppId = externalAuth["wechat"].get("appid", "").asString();
            wechatSecret = externalAuth["wechat"].get("secret", "").asString();
        }
        if (externalAuth.isMember("github"))
        {
            githubClientId = externalAuth["github"].get("client_id", "").asString();
            githubClientSecret = externalAuth["github"].get("client_secret", "").asString();
        }
        if (externalAuth.isMember("tiers"))
        {
            const auto &tiers = externalAuth["tiers"];
            domesticTier = tiers.get("domestic", true).asBool();
            internationalTier = tiers.get("international", true).asBool();
        }
    }
    const bool googleConfigured =
      credentialConfigured(googleClientId) && credentialConfigured(googleClientSecret);
    const bool wechatConfigured =
      credentialConfigured(wechatAppId) && credentialConfigured(wechatSecret);
    const bool githubConfigured =
      credentialConfigured(githubClientId) && credentialConfigured(githubClientSecret);
    // Effective enablement = tier on AND credentials set (#111): a tier
    // switch composes with the credential gate instead of replacing it, and
    // login/link endpoints inherit tier behavior through the same
    // nullptr-injection surface — no new error paths.
    const bool googleEnabled = googleConfigured && internationalTier;
    const bool wechatEnabled = wechatConfigured && domesticTier;
    const bool githubEnabled = githubConfigured && internationalTier;
    // Resolve once with the same policy used by authorization URLs and state.
    googleRedirectUri = fulla::common::utils::socialRedirectUri(customConfig, "google");
    if (!googleEnabled)
        LOG_INFO << "IdentityAssembly: social provider 'google' disabled ("
                 << (internationalTier ? "external_auth.google credentials not configured"
                                       : "external_auth.tiers.international=false")
                 << ")";
    if (!wechatEnabled)
        LOG_INFO << "IdentityAssembly: social provider 'wechat' disabled ("
                 << (domesticTier ? "external_auth.wechat credentials not configured"
                                  : "external_auth.tiers.domestic=false")
                 << ")";
    if (!githubEnabled)
        LOG_INFO << "IdentityAssembly: social provider 'github' disabled ("
                 << (internationalTier ? "external_auth.github credentials not configured"
                                       : "external_auth.tiers.international=false")
                 << ")";
    auto socialAccountRepo =
      std::make_shared<fulla::storage::postgres::PostgresSocialAccountRepository>(dbClient);

    static auto googleAuthService = std::make_shared<fulla::identity::GoogleAuthService>(
      oauthHttpClient, googleClientId, googleClientSecret, googleRedirectUri
    );
    static auto weChatAuthService = std::make_shared<fulla::identity::WeChatAuthService>(
      oauthHttpClient, wechatAppId, wechatSecret
    );
    static auto gitHubAuthService = std::make_shared<fulla::identity::GitHubAuthService>(
      oauthHttpClient, socialAccountRepo, githubClientId, githubClientSecret
    );
    // #70: Google/WeChat get the same account-linking repository GitHub was
    // constructed with (additive setter seam — their constructors and the
    // SocialLinkService wiring below stay untouched), plus the global
    // first-login auto-create gate. One switch for all three providers: a
    // social policy, not a per-provider toggle.
    {
        bool autoCreate = true;  // default keeps GitHub's historical behavior
        if (customConfig.isMember("external_auth") &&
            customConfig["external_auth"].isMember("auto_create_on_first_login"))
        {
            autoCreate =
              customConfig["external_auth"]["auto_create_on_first_login"].asBool();
        }
        googleAuthService->setAccountRepository(socialAccountRepo);
        googleAuthService->setAutoCreate(autoCreate);
        weChatAuthService->setAccountRepository(socialAccountRepo);
        weChatAuthService->setAutoCreate(autoCreate);
        gitHubAuthService->setAutoCreate(autoCreate);
        if (!autoCreate)
            LOG_INFO << "IdentityAssembly: social first-login auto-create disabled "
                        "(external_auth.auto_create_on_first_login=false; unlinked "
                        "provider logins will answer AUTH_SOCIAL_ACCOUNT_NOT_LINKED)";
    }
    // B2 social link/unlink: the same provider services + mapping repository
    // back the self-service /api/me/social/links* routes. Process-lifetime
    // static (same contract as the services above) so the raw pointer handed
    // to the controller singleton stays valid.
    // #73b: the last-credential guard also counts WebAuthn credentials when
    // built with WebAuthn support (passkey-only users may unlink their last
    // social link). cryptoProvider is wired for the server-side link-state
    // flow (#71).
#if defined(WITH_WEBAUTHN)
    std::shared_ptr<fulla::identity::IWebAuthnRepository> linkGuardWebAuthnRepo = webAuthnRepo;
#else
    std::shared_ptr<fulla::identity::IWebAuthnRepository> linkGuardWebAuthnRepo = nullptr;
#endif
    // #71: Redis-backed one-time link state (fail-closed when Redis is not
    // configured -- linking then answers NotConfigured rather than running
    // stateless, which is the exact surface this flow closes).
    // Drogon asserts when getRedisClient names an unconfigured client;
    // inspect the loaded top-level configuration before accessing it.
    ::drogon::nosql::RedisClientPtr socialRedis;
    for (const auto &client : runtimeConfig["redis_clients"])
    {
        if (client.get("name", "default").asString() == "default")
        {
            socialRedis = ::drogon::app().getRedisClient("default");
            break;
        }
    }
    std::shared_ptr<fulla::identity::ISocialLinkStateStore> linkStateStore;
    if (socialRedis)
        linkStateStore = std::make_shared<fulla::drogon::adapters::RedisSocialLinkStateStore>(socialRedis, crypto);
    else
        LOG_WARN << "Social state storage unconfigured; login and linking fail closed";
    static auto socialLinkService = std::make_shared<fulla::identity::SocialLinkService>(
      // #111: disabled providers enter as nullptr -> NotConfigured at the
      // link endpoints (same error surface as the login controllers). The
      // tier flags fold into the same effective-enablement booleans.
      githubEnabled ? gitHubAuthService : nullptr,
      googleEnabled ? googleAuthService : nullptr,
      wechatEnabled ? weChatAuthService : nullptr,
      socialAccountRepo,
      linkGuardWebAuthnRepo,
      crypto,
      linkStateStore
    );
#endif  // WITH_SOCIAL

    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::SessionController>()
      ->setIdentityAuthService(authService.get());
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::SessionController>()
      ->setSessionManager(sessionManager.get());
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::MfaController>()
      ->setMfaService(mfaService.get());
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::MfaController>()
      ->setUserRepository(userRepo.get());
#ifdef WITH_WEBAUTHN
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::WebAuthnController>()
      ->setWebAuthnService(webAuthnService.get());
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::WebAuthnController>()
      ->setUserRepository(userRepo.get());
#endif  // WITH_WEBAUTHN
#ifdef WITH_SOCIAL
    // #111: inject nullptr for disabled providers (envelope NotConfigured at
    // request time instead of a doomed upstream call). Effective enablement
    // includes the v1.4.0 tier flags.
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::GoogleController>()
      ->setGoogleAuthService(googleEnabled ? googleAuthService.get() : nullptr);
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::WeChatController>()
      ->setWeChatAuthService(wechatEnabled ? weChatAuthService.get() : nullptr);
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::GitHubController>()
      ->setGitHubAuthService(githubEnabled ? gitHubAuthService.get() : nullptr);
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::UserSelfServiceController>()
      ->setSocialLinkService(socialLinkService.get());
#endif  // WITH_SOCIAL

    // v1.4.0 provider discovery: the login SPA renders its external-login
    // buttons from GET /api/auth/providers instead of build-time VITE_* env
    // vars. Only the ENABLED provider names are pushed here — computed from
    // the SAME effective-enablement gate as the service injections above —
    // and the transaction issuer uses the shared redirect resolution, so
    // the buttons and the server-side enforcement
    // can never disagree (single source of truth). Wired unconditionally
    // (outside WITH_SOCIAL): without social support the list is simply
    // empty. Memory-storage deployments returned early at the top of this
    // function, so they also serve an empty list — social login needs the
    // account repository anyway.
#ifdef WITH_SOCIAL
    std::vector<std::string> enabledProviderNames;
    if (githubEnabled)
        enabledProviderNames.push_back("github");
    if (googleEnabled)
        enabledProviderNames.push_back("google");
    if (wechatEnabled)
        enabledProviderNames.push_back("wechat");
    std::shared_ptr<fulla::identity::ISocialLoginStateStore> loginStateStore;
    if (socialRedis)
        loginStateStore = std::make_shared<fulla::drogon::adapters::RedisSocialLoginStateStore>(socialRedis);
    auto loginStates = std::make_shared<fulla::identity::SocialLoginStateService>(loginStateStore, crypto);
    const char *loginEnvironment = fulla::common::config::ConfigManager::getEnv("FULLA_ENV");
    const bool allowInsecureLoopback = !loginEnvironment || std::string(loginEnvironment) != "production";
    auto loginGuard = std::make_shared<fulla::drogon::services::SocialLoginGuard>(
      loginStates, customConfig, enabledProviderNames, allowInsecureLoopback);
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::AuthProvidersController>()
      ->setSocialLoginGuard(loginGuard);
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::GoogleController>()
      ->setSocialLoginGuard(loginGuard);
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::WeChatController>()
      ->setSocialLoginGuard(loginGuard);
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::GitHubController>()
      ->setSocialLoginGuard(loginGuard);

    // ------------------------------------------------------------------
    // P2-1: OIDC-in upstream registry (upstream_providers). The runtime
    // snapshots enabled rows, pre-resolves endpoints and pushes them into
    // the guard as DynamicProviders -- registry keys then ride the exact
    // same state/cookie/origin machinery as the three social providers.
    // Memory deployments returned at the top of wireIdentityServices, so
    // this block only runs with a real dbClient.
    // ------------------------------------------------------------------
    {
        auto upstreamRepo =
          std::make_shared<fulla::storage::postgres::PostgresUpstreamProviderRepository>(dbClient);
        bool upstreamAutoCreate = true;
        if (customConfig.isMember("external_auth") &&
            customConfig["external_auth"].isMember("auto_create_on_first_login"))
        {
            upstreamAutoCreate =
              customConfig["external_auth"]["auto_create_on_first_login"].asBool();
        }
        // Transport seams: drogon HttpClient GET / form POST. HttpClient's
        // factory caches per host internally, so per-call construction is
        // cheap; hosts come from registry issuer/discovery documents.
        auto httpGet = [](const std::string &url, const std::string &bearer,
                          fulla::identity::OidcUpstreamAuthService::FetchCallback &&cb) {
            const auto sep = url.find('/', url.find("://") + 3);
            const std::string host = sep == std::string::npos ? url : url.substr(0, sep);
            const std::string path = sep == std::string::npos ? "/" : url.substr(sep);
            auto req = ::drogon::HttpRequest::newHttpRequest();
            req->setMethod(::drogon::Get);
            req->setPath(path);
            if (!bearer.empty())
                req->addHeader("authorization", "Bearer " + bearer);
            ::drogon::HttpClient::newHttpClient(host, ::drogon::app().getLoop())
              ->sendRequest(req, [cb = std::make_shared<
                    fulla::identity::OidcUpstreamAuthService::FetchCallback>(std::move(cb))](
                    ::drogon::ReqResult result, const ::drogon::HttpResponsePtr &resp) {
                  if (result != ::drogon::ReqResult::Ok || !resp)
                  {
                      (*cb)(false, 0, "");
                      return;
                  }
                  (*cb)(true, resp->getStatusCode(),
                        std::string(resp->getBody().data(), resp->getBody().size()));
              });
        };
        auto httpPostForm = [](const std::string &url,
                               const std::vector<std::pair<std::string, std::string>> &form,
                               fulla::identity::OidcUpstreamAuthService::FetchCallback &&cb) {
            const auto sep = url.find('/', url.find("://") + 3);
            const std::string host = sep == std::string::npos ? url : url.substr(0, sep);
            const std::string path = sep == std::string::npos ? "/" : url.substr(sep);
            std::string body;
            for (const auto &[key, value] : form)
            {
                if (!body.empty())
                    body += '&';
                body += ::drogon::utils::urlEncodeComponent(key) + "=" +
                        ::drogon::utils::urlEncodeComponent(value);
            }
            auto req = ::drogon::HttpRequest::newHttpRequest();
            req->setMethod(::drogon::Post);
            req->setPath(path);
            // setContentTypeString (not addHeader): drogon then treats the
            // body as a form and won't append a second default content-type,
            // which made upstreams skip form parsing entirely.
            req->setContentTypeString("application/x-www-form-urlencoded");
            req->setBody(std::move(body));
            ::drogon::HttpClient::newHttpClient(host, ::drogon::app().getLoop())
              ->sendRequest(req, [cb = std::make_shared<
                    fulla::identity::OidcUpstreamAuthService::FetchCallback>(std::move(cb))](
                    ::drogon::ReqResult result, const ::drogon::HttpResponsePtr &resp) {
                  if (result != ::drogon::ReqResult::Ok || !resp)
                  {
                      (*cb)(false, 0, "");
                      return;
                  }
                  (*cb)(true, resp->getStatusCode(),
                        std::string(resp->getBody().data(), resp->getBody().size()));
              });
        };
        static auto oidcUpstreamService = std::make_shared<fulla::identity::OidcUpstreamAuthService>(
          socialAccountRepo, upstreamAutoCreate, httpGet, httpPostForm);
        static auto upstreamRuntime =
          std::make_shared<fulla::drogon::services::UpstreamProviderRuntime>(
            upstreamRepo,
            oidcUpstreamService,
            loginGuard,
            [customConfig](const std::string &key) {
                // Same per-key override convention as the social providers
                // (external_auth.{key}.redirect_uri, else frontend/callback).
                return fulla::common::utils::socialRedirectUri(customConfig, key);
            });

        // --------------------------------------------------------------
        // P2-1c: the three legacy social providers migrate ONTO the
        // registry. Config stays the FIRST-BOOT seed (INSERT-only: admin
        // edits to the row win and survive restarts); after that the row
        // is the runtime source of truth -- admin credential rotation or
        // an enabled toggle takes effect on the next refresh (60s loop or
        // any registry mutation) WITHOUT a restart:
        //   * service credentials (token/userinfo exchanges)
        //   * the guard's static entry (authorize URL + state binding + 404
        //     on disabled)
        //   * the login-page listing (no advertising a 404ing provider)
        // Rows are only seeded while config credentials are configured; a
        // deployment that never configured google/wechat/github keeps the
        // exact pre-P2-1c behavior (nothing seeded, everything disabled).
        // --------------------------------------------------------------
        struct LegacySeed
        {
            const char *key;
            const char *displayName;
            const char *issuer;
            const char *authorizeEndpoint;
            const char *scopes;
        };
        const std::vector<LegacySeed> legacySeeds = {
            {"google", "Google", "https://accounts.google.com",
             "https://accounts.google.com/o/oauth2/v2/auth", "openid email profile"},
            {"wechat", "WeChat", "https://open.weixin.qq.com",
             "https://open.weixin.qq.com/connect/qrconnect", "snsapi_login"},
            {"github", "GitHub", "https://github.com",
             "https://github.com/login/oauth/authorize", "user:email"},
        };
        const bool legacyEnabled[3] = {googleEnabled, wechatEnabled, githubEnabled};
        const std::string legacyCreds[3][2] = {
            {googleClientId, googleClientSecret},
            {wechatAppId, wechatSecret},
            {githubClientId, githubClientSecret}};
        for (size_t i = 0; i < legacySeeds.size(); ++i)
        {
            const auto &seed = legacySeeds[i];
            // Reuses the enclosing credentialConfigured lambda (line ~223);
            // MSVC C4456 shadows it if redeclared here.
            if (!credentialConfigured(legacyCreds[i][0]) ||
                !credentialConfigured(legacyCreds[i][1]))
            {
                continue;  // never configured: no row, legacy behavior
            }
            fulla::identity::UpstreamProviderRecord record;
            record.providerKey = seed.key;
            record.displayName = seed.displayName;
            record.issuer = seed.issuer;
            record.clientId = legacyCreds[i][0];
            record.clientSecret = legacyCreds[i][1];
            record.scopes = seed.scopes;
            // The legacy authorize shapes are pinned verbatim; the row
            // carries the endpoint for documentation/audit only (the
            // static guard surface builds URLs from its own table).
            record.endpointOverrides =
              std::string("{\"authorize\":\"") + seed.authorizeEndpoint + "\"}";
            record.enabled = legacyEnabled[i];
            upstreamRepo->insertRecord(
              record,
              [key = std::string(seed.key)](bool ok, const std::string &error, auto) {
                  // duplicate_key = the admin already owns this row: the
                  // seed must NOT override admin decisions.
                  if (!ok && error != "duplicate_key")
                  {
                      LOG_WARN << "IdentityAssembly: legacy seed for '" << key
                               << "' failed (" << error << "); config-derived "
                                            "behavior stays in effect";
                  }
              });
        }
        // The applier routes guard updates through the RUNTIME's current
        // guard (the social mock fixture swaps guards mid-test-process) and
        // holds only a weak runtime reference (the runtime owns the applier
        // -- a strong capture would be a self-cycle). The three services
        // are function-local statics (process lifetime) and are referenced
        // WITHOUT capture -- lambdas cannot capture statics and do not need
        // to.
        std::weak_ptr<fulla::drogon::services::UpstreamProviderRuntime> weakRuntime =
          upstreamRuntime;
        upstreamRuntime->setLegacyApplier(
          [weakRuntime](
            const std::string &key, const std::string &clientId,
            const std::string &clientSecret, bool enabled) {
              if (auto runtime = weakRuntime.lock())
              {
                  runtime->updateLegacyGuardEntry(key, clientId, clientSecret, enabled);
              }
              if (key == "google")
                  googleAuthService->setCredentials(clientId, clientSecret);
              else if (key == "wechat")
                  weChatAuthService->setCredentials(clientId, clientSecret);
              else if (key == "github")
                  gitHubAuthService->setCredentials(clientId, clientSecret);
              ::drogon::DrClassMap::getSingleInstance<
                fulla::drogon::controllers::AuthProvidersController>()
                ->setStaticProviderEnabled(key, enabled);
          });

        upstreamRuntime->refresh();
        ::drogon::app().getLoop()->runEvery(
          60.0, [runtime = upstreamRuntime]() { runtime->refresh(); });
        drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::UpstreamLoginController>()
          ->setRuntime(upstreamRuntime);
        drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::UpstreamLoginController>()
          ->setSocialLoginGuard(loginGuard);
        drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::UpstreamProviderAdminController>()
          ->setRuntime(upstreamRuntime);
        drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::AuthProvidersController>()
          ->setUpstreamRuntime(upstreamRuntime);
    }
#else
    // No social support compiled in: no provider is ever offered.
    std::vector<std::string> enabledProviderNames;
#endif
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::AuthProvidersController>()
      ->setProviders(std::move(enabledProviderNames));

    LOG_INFO << "Identity services wired into SessionController/MfaController/"
                "WebAuthnController/Google|WeChat|GitHubController "
                "(fulla::identity::AuthService + SessionManager + MfaService + "
                "WebAuthnService + Social auth services)";
}

}  // namespace bootstrap
