#pragma once

// Task B5 (fulla-sdk-refactor): application-service extraction for the
// email-verification non-admin domain. Raw SQL inline in
// EmailVerificationController is now Mapper<T> + Criteria on the ORM
// EmailVerificationTokens/Users models (per db-operations.md). The
// DELETE...RETURNING on verifyToken is a documented raw-SQL exemption.
// Behavior equivalent (all existing tests must stay green).
//
// Lives in libs/drogon (Adapter layer, namespace
// fulla::drogon::services) so the SDK stays self-contained.

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>
#include <memory>
#include <string>

namespace fulla::drogon::services
{

/**
 * @brief Application service for email verification (token CRUD +
 * users.email_verified update). Follows the same static-method pattern
 * as admin::ClientManagementService.
 */
class EmailVerificationService
{
  public:
    using ResponseCallback =
      std::shared_ptr<std::function<void(const ::drogon::HttpResponsePtr &)>>;

    // ---- POST /api/verify-email/resend ----
    static void resendVerification(const ::drogon::HttpRequestPtr &req, ResponseCallback cb);

    // ---- GET /api/verify-email?token=xxx ----
    static void verifyToken(const ::drogon::HttpRequestPtr &req, ResponseCallback cb);

    // ---- POST /api/verify-email/resend-by-email (unauthenticated) ----
    // Issue #198: a self-registered user has no credential yet, so the
    // Bearer-gated /resend is unreachable for exactly the users who need it.
    // This variant identifies the account by email address instead, applies
    // a per-(ip, email) request-rate limit, and answers with an identical
    // generic response for every outcome (anti-enumeration).
    static void requestVerificationByEmail(
      const ::drogon::HttpRequestPtr &req, ResponseCallback cb);

    // ---- registration hook (fire-and-forget) ----
    // Resolves the freshly registered account by email and delivers the
    // verification email. Called from the register flow right after the
    // user row is created; without it an unverified user could never obtain
    // a token (issue #198). Silent no-op when the email is empty or the
    // lookup fails (failures are logged, never surfaced to the caller).
    static void notifyNewRegistration(const std::string &email);

  private:
    /// Utility: generate a token, hash it, INSERT into
    /// email_verification_tokens, and send the verification email.
    /// @param internalUserId The internal (integer) user id.
    /// @param email The recipient email address.
    /// P1-2: the template variant renders the localized email_templates row
    /// (fallback en -> built-in); the 2-arg overload keeps "en" for callers
    /// without a request context.
    static void sendVerificationEmail(int internalUserId, const std::string &email);
    static void sendVerificationEmail(
      int internalUserId, const std::string &email, const std::string &locale
    );
};

}  // namespace fulla::drogon::services
