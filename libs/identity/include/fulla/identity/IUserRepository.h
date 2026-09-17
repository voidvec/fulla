#pragma once

#include <functional>
#include <optional>
#include <string>
#include <cstdint>
#include <json/json.h>

namespace fulla::identity
{

/**
 * @brief User data from repository
 */
struct UserData
{
    int32_t id;
    std::string username;
    std::string email;
    std::string passwordHash;
    std::string salt;
    std::string publicSub;
    bool emailVerified = false;
    bool mfaEnabled = false;
    bool mustChangePassword = false;  // #145: forced first-login password change flag
    int64_t lockedUntil = 0;
    int failedLoginCount = 0;
    // V033 profile minimal set (empty = not set). displayName feeds the OIDC
    // `name` claim (preferred over username) and avatarUrl the `picture`
    // claim; both are only emitted when non-empty.
    std::string displayName;
    std::string avatarUrl;
};

/**
 * @brief Repository interface for user management
 */
class IUserRepository
{
  public:
    virtual ~IUserRepository() = default;

    /**
     * @brief Find user by email (normalized)
     */
    virtual void findByEmail(
      const std::string &email,
      std::function<void(std::optional<UserData>)> &&callback
    ) = 0;

    /**
     * @brief Find user by username
     */
    virtual void findByUsername(
      const std::string &username,
      std::function<void(std::optional<UserData>)> &&callback
    ) = 0;

    /**
     * @brief Find user by internal ID
     */
    virtual void findById(
      int32_t userId,
      std::function<void(std::optional<UserData>)> &&callback
    ) = 0;

    /**
     * @brief Find user by public subject (the UUID exposed in OAuth2
     * tokens/OIDC claims -- never the internal id). Task 24 slice 5
     * (fulla-sdk-refactor): controllers resolve the authenticated
     * caller from `req->getAttributes()->get<std::string>("userId")`,
     * which -- despite the "userId" attribute name -- is actually the
     * OAuth2AccessToken's `userId` field, itself set from the OAuth2
     * `subject` at token-issuance time (see
     * fulla::oauth2::protocol::TokenService::generateAuthorizationCode's
     * `authCode.userId = subject;`), i.e. the public_sub string, not the
     * internal auto-increment id. MFA/WebAuthn/Social identity services
     * are keyed by the internal int32_t id, so this method is the
     * resolution step every one of those call sites needs before it can
     * call into them.
     */
    virtual void findByPublicSub(
      const std::string &publicSub,
      std::function<void(std::optional<UserData>)> &&callback
    ) = 0;

    /**
     * @brief Create a new user
     * @param userData User data to create
     * @param callback Returns (user ID, empty string) on success, or
     * (nullopt, Error_Code) on failure. The Error_Code is a structured
     * value registered in ErrorCatalog (e.g. "VALIDATION_USERNAME_TAKEN",
     * "VALIDATION_EMAIL_TAKEN") when the implementation can identify the
     * specific conflicting constraint, or "INTERNAL_ERROR"/"" for a
     * generic/unclassified failure -- mirrors
     * OAuth2Server/AuthService.cc's pre-existing registerUser contract
     * (auth-flow-error-code-gaps spec) so callers (AuthService::
     * registerUser) can forward the value verbatim to ErrorResponder
     * instead of collapsing every failure into one generic code.
     */
    virtual void create(
      const UserData &userData,
      std::function<void(std::optional<int32_t>, std::string errorCode)> &&callback
    ) = 0;

    /**
     * @brief Update user's password hash
     */
    virtual void updatePasswordHash(
      int32_t userId,
      const std::string &newHash,
      std::function<void(bool)> &&callback
    ) = 0;

    /**
     * @brief Reset failed login count
     */
    virtual void resetFailedLogins(int32_t userId, std::function<void(bool)> &&callback) = 0;

    /**
     * @brief Increment failed login count and optionally lock account
     */
    virtual void incrementFailedLogins(int32_t userId, std::function<void(bool)> &&callback) = 0;

    /**
     * @brief Get user info with roles (for userinfo endpoint)
     */
    virtual void getUserInfoWithRoles(
      int32_t userId,
      std::function<void(std::optional<Json::Value>)> &&callback
    ) = 0;
};

}  // namespace fulla::identity
