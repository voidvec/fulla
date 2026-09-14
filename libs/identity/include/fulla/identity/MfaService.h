#pragma once

// M2.5 identity completion (fulla-sdk-refactor, design.md §5.1/§6):
// real (non-placeholder) implementation. Ports the TOTP-MFA business
// logic out of libs/drogon/src/controllers/MfaController.cc's
// setup/verifySetup/disable handlers into a framework-independent
// service, following AuthService.cc's established pattern: dependencies
// (repository + ports) are injected, no Drogon/DB-client/HTTP types
// appear anywhere in this class.
//
// Scope boundary (design.md §4.1 rule 2, oauth2 <-> identity 互不依赖):
// this class does NOT drive the "verify MFA code during login, then
// issue OAuth2 tokens" orchestration that
// MfaController::verifyLogin currently performs inline (that handler
// interleaves TOTP verification with oauth2::TokenService/ClientService
// calls -- issuing an authorization code and exchanging it). That
// orchestration crosses the identity/oauth2 boundary and belongs to the
// future product-level assembly (Task 24), which will call
// MfaService::verifyLoginCode() (this class) and then, separately, the
// already-migrated fulla::oauth2::protocol::TokenService -- not
// something this class can do itself without creating an oauth2
// dependency identity must not have.
//
// The pending-client-binding bookkeeping (mfa_pending_client_id/
// mfa_pending_redirect_uri) is included here because it's pure identity
// state (columns on the users table), even though the value being bound
// (an OAuth2 client_id) originates from the oauth2 domain -- this class
// treats it as an opaque string, same as UserRef treats internalUserId as
// opaque in the other direction (see model/UserRef.h's identical
// rationale on that pattern).

#include <fulla/common/ports/IClock.h>
#include <fulla/common/ports/ICryptoProvider.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fulla::identity
{

class IMfaRepository;

/**
 * @brief Result of MfaService::setupSecret.
 */
struct MfaSetupResult
{
    std::string secret;      // Base32 TOTP secret (shown to the user once).
    std::string otpAuthUri;  // otpauth:// URI for QR code scanning.
};

/**
 * @brief Result of MfaService::verifyAndEnable.
 */
struct MfaEnableResult
{
    std::vector<std::string> backupCodes;  // Plaintext codes, shown once.
};

/**
 * @brief TOTP-based multi-factor authentication service.
 *
 * Handles MFA setup (secret generation), setup verification (enabling MFA
 * + issuing backup codes), disabling, and login-time TOTP code
 * verification. Framework-independent -- persistence, crypto, and clock
 * are injected through the constructor.
 */
class MfaService
{
  public:
    MfaService(
      std::shared_ptr<IMfaRepository> mfaRepo,
      std::shared_ptr<fulla::common::ports::ICryptoProvider> crypto,
      std::shared_ptr<fulla::common::ports::IClock> clock,
      // Product-facing name shown by authenticator apps (Google
      // Authenticator renders "issuer:account" as its entry title).
      std::string issuerName = "Fulla"
    );

    /**
     * @brief Begin MFA setup: generate and store a new TOTP secret.
     * @param userId Internal user id.
     * @param accountLabel Label shown in the authenticator app (e.g. the
     * user's public subject or email).
     * @param callback Result with the secret + otpauth:// URI, or nullopt
     * if the repository write failed.
     */
    void setupSecret(
      int32_t userId,
      const std::string &accountLabel,
      std::function<void(std::optional<MfaSetupResult>)> &&callback
    );

    /**
     * @brief Verify a setup TOTP code and, on success, enable MFA and
     * generate backup codes.
     * @param userId Internal user id.
     * @param code 6-digit TOTP code to verify against the stored (not yet
     * enabled) secret.
     * @param callback Result with the plaintext backup codes on success;
     * nullopt if the user has no secret set up, the code does not match,
     * or the repository write failed.
     */
    void verifyAndEnable(
      int32_t userId,
      const std::string &code,
      std::function<void(std::optional<MfaEnableResult>)> &&callback
    );

    /**
     * @brief Disable MFA for a user (clears secret + backup codes).
     */
    void disable(int32_t userId, std::function<void(bool)> &&callback);

    /**
     * @brief Verify a TOTP code presented during login against the
     * user's enabled MFA secret.
     * @param userId Internal user id.
     * @param code 6-digit TOTP code.
     * @param callback true iff MFA is enabled for the user and the code
     * verifies against the current (or adjacent) time step.
     */
    void verifyLoginCode(
      int32_t userId,
      const std::string &code,
      std::function<void(bool)> &&callback
    );

    /**
     * @brief Record the OAuth2 client/redirect_uri a login is pending MFA
     * verification for. See this header's top comment for the scope
     * rationale (identity treats clientId as an opaque string).
     */
    void setPendingBinding(
      int32_t userId,
      const std::string &clientId,
      const std::string &redirectUri,
      std::function<void(bool)> &&callback
    );

    /**
     * @brief Fetch the pending client/redirect_uri binding (if any), so
     * the caller can validate the values presented at MFA-verify time
     * match what was recorded at login time.
     */
    void getPendingBinding(
      int32_t userId,
      std::function<void(std::optional<std::pair<std::string, std::string>>)> &&callback
    );

    /// Clear the pending binding after MFA verification completes.
    void clearPendingBinding(int32_t userId, std::function<void(bool)> &&callback);

  private:
    std::shared_ptr<IMfaRepository> mfaRepo_;
    std::shared_ptr<fulla::common::ports::ICryptoProvider> crypto_;
    std::shared_ptr<fulla::common::ports::IClock> clock_;
    std::string issuerName_;
};

}  // namespace fulla::identity
