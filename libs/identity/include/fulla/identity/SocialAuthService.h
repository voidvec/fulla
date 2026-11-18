#pragma once

#ifdef WITH_SOCIAL

// M2.5 identity completion, Social auth slice (fulla-sdk-refactor,
// design.md §5.1/§6): real (non-placeholder) implementation, replacing
// the previous `#ifdef WITH_SOCIAL ... TODO` placeholder. Ports the
// business logic out of
// libs/drogon/src/controllers/{Google,WeChat,GitHub}Controller.cc's
// login() handlers into three framework-independent services, following
// AuthService.h/MfaService.h/WebAuthnService.h's established pattern:
// dependencies (repository/port) are injected through the constructor,
// no Drogon/DB-client/HTTP-client types appear anywhere in these
// classes -- the actual outbound HTTP calls to each provider's token/
// userinfo endpoints go through the injected IOAuthHttpClient port (see
// IOAuthHttpClient.h's header comment for the Adapter/Domain split
// rationale) instead of drogon::HttpClient directly.
//
// Scope per provider, confirmed by reading each source controller in
// full before writing this file:
//   - GoogleController.cc's login() and WeChatController.cc's login()
//     ONLY exchange the code + fetch/filter profile info and hand it
//     back to the caller -- neither one touches the `users` table, the
//     `oauth2_subject_mappings` table, or issues any OAuth2 tokens. So
//     GoogleAuthService/WeChatAuthService mirror that exact narrow
//     scope: login() in, filtered profile struct out, nothing more.
//   - GitHubController.cc's login() is broader: after exchanging the
//     code + fetching the GitHub profile, it also finds-or-creates a
//     local `users` row, links it via `oauth2_subject_mappings`, and
//     assigns the default 'user' role (via the injected
//     ISocialAccountRepository here) -- but it explicitly does NOT stop
//     there: it goes on to mint and persist OAuth2 access_token/
//     refresh_token rows directly (raw INSERT INTO
//     oauth2_access_tokens/oauth2_refresh_tokens). That token-issuance
//     step is an oauth2-domain concern (design.md §4.1 rule 2, identity
//     <-> oauth2 互不依赖) -- identical scope-boundary rationale to
//     MfaService.h's own header comment on why "verify code, then issue
//     OAuth2 tokens" orchestration does not belong in this package.
//     GitHubAuthService::login() therefore stops at "local user
//     identified or created, default role assigned" and returns that
//     outcome to the caller; minting/persisting OAuth2 tokens for the
//     resulting user is future product-level assembly (Task 24), same
//     as MfaService::verifyLoginCode()'s and AuthService::validateUser()'s
//     existing precedent of stopping short of token issuance.

#include <fulla/identity/IOAuthHttpClient.h>
#include <fulla/identity/ISocialAccountRepository.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fulla::identity
{

// ---------------------------------------------------------------------
// Google
// ---------------------------------------------------------------------

/**
 * @brief Filtered Google userinfo fields -- mirrors GoogleController.cc's
 * own field allow-list (sub/name/email/picture), which is itself a
 * deliberate security-conscious filter over the raw
 * https://www.googleapis.com/oauth2/v3/userinfo response (see that
 * file's "Filter response to only include necessary fields" comment).
 */
struct GoogleUserInfo
{
    std::string sub;
    std::string name;
    std::string email;
    std::string picture;
};

/**
 * @brief Result of GoogleAuthService::login.
 */
struct GoogleLoginResult
{
    // Empty on success. On failure, a structured Error_Code (registered
    // in ErrorCatalog) mirroring GoogleController.cc's existing
    // ErrorResponder calls verbatim: "NET_CONNECTION_FAILED" (token
    // exchange or userinfo fetch could not be contacted / did not return
    // HTTP 200) or "VALIDATION_INVALID_INPUT" (token response missing
    // access_token). #70 adds "AUTH_SOCIAL_ACCOUNT_NOT_LINKED" (no
    // mapping and auto-create disabled) and "AUTH_INVALID_CREDENTIALS"
    // (linked account soft-deleted/locked) and "VALIDATION_USERNAME_TAKEN"
    // (username collision exhausted the retry) for the account-linked
    // path.
    std::string errorCode;
    GoogleUserInfo profile;
    // #70 account-linking fields — populated only when an
    // ISocialAccountRepository has been injected (setAccountRepository).
    // userId/publicSub empty = profile-only (degraded) result; callers
    // keep the legacy profile-response behavior for that case.
    int32_t userId = 0;      // Internal user id (existing or newly created).
    std::string username;    // Local username (existing, or "google_" + sub prefix).
    bool isNewUser = false;  // True iff a new local account was created by this call.
    std::string publicSub;   // Platform subject (UUID) — what token rows must store.
};

/**
 * @brief Google OAuth2 authorization-code exchange + userinfo fetch.
 * Framework-independent -- the actual HTTP calls go through the injected
 * IOAuthHttpClient port.
 */
class GoogleAuthService
{
  public:
    /**
     * @param httpClient Outbound HTTP port (required).
     * @param clientId Google OAuth2 client_id.
     * @param clientSecret Google OAuth2 client_secret.
     * @param redirectUri Google OAuth2 redirect_uri (must match the one
     * registered with Google / used to obtain `code`).
     */
    GoogleAuthService(
      std::shared_ptr<IOAuthHttpClient> httpClient,
      std::string clientId,
      std::string clientSecret,
      std::string redirectUri
    );

    // #70 account-linking seam (additive): with a repository injected,
    // login() additionally resolves-or-creates the local account behind
    // the Google identity (GitHub's four-state flow) and populates
    // userId/username/isNewUser/publicSub on the result. Without one the
    // service keeps its profile-only behavior (assembly always injects
    // when built with WITH_SOCIAL and DB storage; the profile-only shape
    // remains for direct construction/tests).
    void setAccountRepository(std::shared_ptr<ISocialAccountRepository> repo)
    {
        accountRepo_ = std::move(repo);
    }

    // #70: gate for first-login auto account creation (assembly-level
    // global switch external_auth.auto_create_on_first_login, default
    // true). When false, a NoMapping lookup ends with
    // AUTH_SOCIAL_ACCOUNT_NOT_LINKED instead of creating a new account.
    void setAutoCreate(bool allow) { autoCreate_ = allow; }

    // P2-1c: hot credential swap from the upstream registry (admin edits the
    // google row at runtime; the runtime applies it on refresh). Same
    // not-configured semantics as the constructor guard: placeholder/YOUR_*
    // values leave the service in the NotConfigured state.
    void setCredentials(std::string clientId, std::string clientSecret)
    {
        clientId_ = std::move(clientId);
        clientSecret_ = std::move(clientSecret);
    }

    /**
     * @brief Exchange an authorization code for Google userinfo.
     * @param code Authorization code from Google's OAuth2 callback
     * (caller is responsible for extracting it from the request --
     * mirrors GoogleController.cc's own "missing code" check happening
     * before this business logic runs).
     * @param callback Result with the filtered profile on success, or a
     * non-empty errorCode on failure.
     */
    void login(const std::string &code, std::function<void(GoogleLoginResult)> &&callback);

  private:
    std::shared_ptr<IOAuthHttpClient> httpClient_;
    std::string clientId_;
    std::string clientSecret_;
    std::string redirectUri_;
    std::shared_ptr<ISocialAccountRepository> accountRepo_;
    bool autoCreate_ = true;
};

// ---------------------------------------------------------------------
// WeChat
// ---------------------------------------------------------------------

/**
 * @brief Filtered WeChat userinfo fields -- mirrors WeChatController.cc's
 * own field allow-list (openid/nickname/headimgurl/sex/city/province/
 * country).
 */
struct WeChatUserInfo
{
    std::string openid;
    std::string nickname;
    std::string headimgurl;
    int sex = 0;
    std::string city;
    std::string province;
    std::string country;
};

/**
 * @brief Result of WeChatAuthService::login.
 */
struct WeChatLoginResult
{
    // Empty on success. On failure, a structured Error_Code mirroring
    // WeChatController.cc's existing ErrorResponder calls verbatim:
    // "NET_CONNECTION_FAILED" (token exchange or userinfo fetch could
    // not be contacted / did not return HTTP 200) or
    // "VALIDATION_INVALID_INPUT" (WeChat returned a non-zero errcode
    // during token exchange). #70 account-linking codes as documented on
    // GoogleLoginResult.
    std::string errorCode;
    WeChatUserInfo profile;
    // #70 account-linking fields — see GoogleLoginResult's counterpart
    // comment (populated only with an injected repository).
    int32_t userId = 0;
    std::string username;    // "wx_" + openid prefix for new accounts.
    bool isNewUser = false;
    std::string publicSub;
};

/**
 * @brief WeChat OAuth2 authorization-code exchange + userinfo fetch.
 * Framework-independent -- the actual HTTP calls go through the injected
 * IOAuthHttpClient port. Unlike Google/GitHub, WeChat's API is
 * GET-with-query-params for both the token-exchange and userinfo steps
 * (no POST body, no Bearer header) -- see IOAuthHttpClient.h's header
 * comment for how that maps onto the shared port.
 */
class WeChatAuthService
{
  public:
    /**
     * @param httpClient Outbound HTTP port (required).
     * @param appId WeChat Open Platform appid.
     * @param secret WeChat Open Platform secret.
     */
    WeChatAuthService(
      std::shared_ptr<IOAuthHttpClient> httpClient,
      std::string appId,
      std::string secret
    );

    // #70 account-linking seam — see GoogleAuthService's counterpart
    // comments (same semantics; new-account usernames are "wx_" + the
    // openid's first 12 characters).
    void setAccountRepository(std::shared_ptr<ISocialAccountRepository> repo)
    {
        accountRepo_ = std::move(repo);
    }

    void setAutoCreate(bool allow) { autoCreate_ = allow; }

    // P2-1c: hot credential swap from the upstream registry (see
    // GoogleAuthService::setCredentials).
    void setCredentials(std::string appId, std::string secret)
    {
        appId_ = std::move(appId);
        secret_ = std::move(secret);
    }

    /**
     * @brief Exchange an authorization code for WeChat userinfo.
     * @param code Authorization code from WeChat's OAuth2 callback.
     * @param callback Result with the filtered profile on success, or a
     * non-empty errorCode on failure.
     */
    void login(const std::string &code, std::function<void(WeChatLoginResult)> &&callback);

  private:
    std::shared_ptr<IOAuthHttpClient> httpClient_;
    std::string appId_;
    std::string secret_;
    std::shared_ptr<ISocialAccountRepository> accountRepo_;
    bool autoCreate_ = true;
};

// ---------------------------------------------------------------------
// GitHub
// ---------------------------------------------------------------------

/**
 * @brief Result of GitHubAuthService::login.
 */
struct GitHubLoginResult
{
    // Empty on success. On failure, a structured Error_Code mirroring
    // GitHubController.cc's existing ErrorResponder calls verbatim:
    // "NET_CONNECTION_FAILED" (token exchange or userinfo fetch could
    // not be contacted / did not return HTTP 200), "VALIDATION_INVALID_INPUT"
    // (token response missing access_token, or GitHub returned no user
    // login), or "DB_QUERY_ERROR"/"INTERNAL_ERROR" (repository failure
    // during account lookup/creation -- collapsed from the controller's
    // several distinct DrogonDbException call sites into the single
    // repository-level failure signal ISocialAccountRepository exposes).
    std::string errorCode;

    int32_t userId = 0;      // Internal user id (existing or newly created).
    std::string username;    // Local username (existing, or "gh_" + github login for new accounts).
    bool isNewUser = false;  // True iff a new local account was created by this call.
    std::string publicSub;   // #70: platform subject (UUID) — what token rows must store.
};

/**
 * @brief Result of GitHubAuthService::fetchProfile.
 */
struct GitHubProfileResult
{
    // Empty on success; same code set as GitHubLoginResult.errorCode for the
    // exchange/userinfo steps ("NET_CONNECTION_FAILED" /
    // "VALIDATION_INVALID_INPUT").
    std::string errorCode;
    int64_t githubId = 0;   // Provider subject (stringified when persisted).
    std::string login;
    std::string email;
};

/**
 * @brief GitHub OAuth2 authorization-code exchange + userinfo fetch, plus
 * (via login()) local-account find-or-create/link. Framework-independent --
 * the actual HTTP calls go through the injected IOAuthHttpClient port, and
 * the account-linking persistence goes through the injected
 * ISocialAccountRepository. Does NOT issue OAuth2 tokens -- see this
 * header's top comment for the scope-boundary rationale.
 */
class GitHubAuthService
{
  public:
    /**
     * @param httpClient Outbound HTTP port (required).
     * @param accountRepo Local-account find-or-create/link persistence
     * (required).
     * @param clientId GitHub OAuth App client_id.
     * @param clientSecret GitHub OAuth App client_secret.
     */
    GitHubAuthService(
      std::shared_ptr<IOAuthHttpClient> httpClient,
      std::shared_ptr<ISocialAccountRepository> accountRepo,
      std::string clientId,
      std::string clientSecret
    );

    // #70: gate for first-login auto account creation — the same global
    // external_auth.auto_create_on_first_login switch as Google/WeChat
    // (one social policy, no per-provider asymmetry). When false, a
    // NoMapping lookup ends with AUTH_SOCIAL_ACCOUNT_NOT_LINKED instead
    // of creating an account; ALREADY-LINKED users are unaffected.
    void setAutoCreate(bool allow) { autoCreate_ = allow; }

    // P2-1c: hot credential swap from the upstream registry (see
    // GoogleAuthService::setCredentials).
    void setCredentials(std::string clientId, std::string clientSecret)
    {
        clientId_ = std::move(clientId);
        clientSecret_ = std::move(clientSecret);
    }

    /**
     * @brief Exchange an authorization code for a GitHub profile, then
     * find or create the linked local account.
     * @param code Authorization code from GitHub's OAuth2 callback.
     * @param callback Result with the local user id/username on
     * success, or a non-empty errorCode on failure.
     */
    void login(const std::string &code, std::function<void(GitHubLoginResult)> &&callback);

    /**
     * @brief Exchange an authorization code for a GitHub profile ONLY --
     * no local-account lookup/creation. B2 social link/unlink uses this to
     * resolve the provider subject for an already-authenticated local user
     * without find-or-create side effects; login() is a fetchProfile +
     * find-or-create composition of the same exchange steps.
     * @param code Authorization code from GitHub's OAuth2 callback.
     * @param callback Result with the GitHub numeric id/login/email on
     * success, or a non-empty errorCode on failure.
     */
    void fetchProfile(
      const std::string &code,
      std::function<void(GitHubProfileResult)> &&callback
    );

  private:
    std::shared_ptr<IOAuthHttpClient> httpClient_;
    std::shared_ptr<ISocialAccountRepository> accountRepo_;
    std::string clientId_;
    std::string clientSecret_;
    bool autoCreate_ = true;
};

}  // namespace fulla::identity

#endif  // WITH_SOCIAL
