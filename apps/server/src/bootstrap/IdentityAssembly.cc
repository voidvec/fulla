#include "IdentityAssembly.h"

#include <fulla/common/observability/AuditEvent.h>
#include <fulla/drogon/adapters/BackchannelLogoutNotifier.h>
#include <fulla/drogon/adapters/DrogonOAuthHttpClient.h>
#include <fulla/drogon/observability/AuditLogger.h>
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
#include <fulla/storage/postgres/PostgresIdentityRepository.h>
#include <fulla/storage/postgres/PostgresMfaRepository.h>
#include <fulla/storage/postgres/PostgresSocialAccountRepository.h>
#include <fulla/storage/postgres/PostgresWebAuthnRepository.h>
#include <drogon/DrClassMap.h>
#include <drogon/drogon.h>

#include <string>
#include <vector>
#include <fulla/drogon/adapters/OpenSslCryptoProvider.h>
#include <fulla/drogon/adapters/SystemClock.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>

#include <memory>

namespace bootstrap
{

void wireIdentityServices()
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
    std::string rpName = "OAuth2 Server";
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
        return !value.empty() && value.rfind("YOUR_", 0) != 0;
    };
    std::string googleClientId, googleClientSecret, googleRedirectUri;
    std::string wechatAppId, wechatSecret;
    std::string githubClientId, githubClientSecret;
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
    }
    const bool googleConfigured =
      credentialConfigured(googleClientId) && credentialConfigured(googleClientSecret);
    const bool wechatConfigured =
      credentialConfigured(wechatAppId) && credentialConfigured(wechatSecret);
    const bool githubConfigured =
      credentialConfigured(githubClientId) && credentialConfigured(githubClientSecret);
    if (!googleConfigured)
        LOG_INFO << "IdentityAssembly: social provider 'google' disabled (external_auth.google "
                    "credentials not configured; see docs on provider setup)";
    if (!wechatConfigured)
        LOG_INFO << "IdentityAssembly: social provider 'wechat' disabled (external_auth.wechat "
                    "credentials not configured; see docs on provider setup)";
    if (!githubConfigured)
        LOG_INFO << "IdentityAssembly: social provider 'github' disabled (external_auth.github "
                    "credentials not configured; see docs on provider setup)";
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
    std::shared_ptr<fulla::identity::ISocialLinkStateStore> linkStateStore = nullptr;
    try
    {
        auto redisClient = ::drogon::app().getRedisClient("default");
        linkStateStore =
          std::make_shared<fulla::drogon::adapters::RedisSocialLinkStateStore>(redisClient, crypto);
    }
    catch (const std::exception &)
    {
        LOG_WARN << "IdentityAssembly: no Redis client configured -- social link "
                    "authorization disabled (link endpoints fail closed, #71)";
    }
    static auto socialLinkService = std::make_shared<fulla::identity::SocialLinkService>(
      // #111: disabled providers enter as nullptr -> NotConfigured at the
      // link endpoints (same error surface as the login controllers).
      githubConfigured ? gitHubAuthService : nullptr,
      googleConfigured ? googleAuthService : nullptr,
      wechatConfigured ? weChatAuthService : nullptr,
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
    // request time instead of a doomed upstream call).
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::GoogleController>()
      ->setGoogleAuthService(googleConfigured ? googleAuthService.get() : nullptr);
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::WeChatController>()
      ->setWeChatAuthService(wechatConfigured ? weChatAuthService.get() : nullptr);
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::GitHubController>()
      ->setGitHubAuthService(githubConfigured ? gitHubAuthService.get() : nullptr);
    drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::UserSelfServiceController>()
      ->setSocialLinkService(socialLinkService.get());
#endif  // WITH_SOCIAL

    LOG_INFO << "Identity services wired into SessionController/MfaController/"
                "WebAuthnController/Google|WeChat|GitHubController "
                "(fulla::identity::AuthService + SessionManager + MfaService + "
                "WebAuthnService + Social auth services)";
}

}  // namespace bootstrap
