#include <fulla/drogon/services/EmailVerificationService.h>

#include <fulla/common/utils/EmailNormalizer.h>
#include <fulla/common/utils/RateLimiter.h>
#include <fulla/storage/postgres/models/EmailVerificationTokens.h>
#include <fulla/storage/postgres/models/Users.h>
#include <fulla/drogon/services/EmailTemplateService.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/drogon/utils/EmailService.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>

#include <drogon/drogon.h>

// Wave-2 P1: email_verified is a cached profile field — revoke on write.
#include "../UserReadCache.h"

#include <chrono>

namespace fulla::drogon::services
{

namespace
{
// Emit an Application error via the unified ErrorResponder entry point.
// Verbatim from the pre-B5 EmailVerificationController -- behavior unchanged.
void respondError(
  const ::drogon::HttpRequestPtr &req,
  const EmailVerificationService::ResponseCallback &cb,
  std::string code,
  std::string detailForLog = ""
)
{
    ::fulla::common::error::ErrorResponder::respond(
      req,
      [cb](const ::drogon::HttpResponsePtr &r) { (*cb)(r); },
      std::move(code),
      std::move(detailForLog)
    );
}

// Lazy accessor for EmailService -- avoids static init order issues.
::fulla::drogon::utils::IEmailService &getEmailSvc()
{
    return ::fulla::drogon::utils::getEmailService();
}

// Lazily resolve the DbClient. Kept identical to pre-B5 controller behavior.
::drogon::orm::DbClientPtr getDbOrRespond(
  const ::drogon::HttpRequestPtr &req,
  const EmailVerificationService::ResponseCallback &cb
)
{
    try
    {
        return ::drogon::app().getDbClient();
    }
    catch (...)
    {
        respondError(req, cb, "DB_CONNECTION_ERROR", "Database unavailable");
        return nullptr;
    }
}
}  // namespace

// Bring ORM + model names into scope. Fully qualified (::) to avoid
// namespace collision inside fulla::drogon::services.
using namespace ::drogon::orm;
using namespace ::drogon_model::fulla_db;

// Memory storage mode has no database at all: app().getDbClient() would
// return a null client (or assert in debug builds). Email verification is
// persistence-backed by design, so every entry point below no-ops (or
// answers generically) in that mode. Registration DOES reach these paths
// in memory mode via notifyNewRegistration, so the guard is load-bearing.
bool verificationStorageAvailable()
{
    auto *plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
    return plugin != nullptr && plugin->getStorageType() != "memory";
}

// ---- internal helper ----

void EmailVerificationService::sendVerificationEmail(int internalUserId, const std::string &email)
{
    sendVerificationEmail(internalUserId, email, "en");
}

void EmailVerificationService::sendVerificationEmail(
  int internalUserId,
  const std::string &email,
  const std::string &locale
)
{
    if (email.empty())
        return;

    std::string rawToken = ::fulla::drogon::utils::generateSecureToken();
    std::string tokenHash = ::fulla::drogon::utils::hashToken(rawToken);

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now().time_since_epoch()
    )
                 .count();
    int64_t expiresAt = now + 86400;  // 24 hours

    auto db = ::drogon::app().getDbClient();

    EmailVerificationTokens token;
    token.setTokenHash(tokenHash);
    token.setUserId(static_cast<int32_t>(internalUserId));
    token.setEmail(email);
    token.setExpiresAt(expiresAt);

    Mapper<EmailVerificationTokens> mapper(db);
    mapper.insert(
      token,
      [rawToken, email, locale](const EmailVerificationTokens &) {
          // P1-2 (#278): localized template send. The link domain comes ONLY
          // from global config (config.frontend.url) -- org-controlled input
          // is never interpolated into a verification link (D8 red line).
          auto customConfig = ::drogon::app().getCustomConfig();
          std::string frontendUrl = "http://localhost:5173";
          if (customConfig.isMember("frontend") && customConfig["frontend"].isMember("url"))
          {
              frontendUrl = customConfig["frontend"]["url"].asString();
          }
          std::string verifyLink = frontendUrl + "/verify-email?token=" + rawToken;

          EmailTemplateService::send(
            EmailTemplateService::kKeyEmailVerification,
            locale,
            email,
            {{"verify_link", verifyLink}});
      },
      [](const DrogonDbException &e) {
          LOG_ERROR << "Failed to store verification token: " << e.base().what();
      }
    );
}

// ---- public methods ----

void EmailVerificationService::verifyToken(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback sharedCb
)
{
    std::string token = req->getParameter("token");
    if (token.empty())
    {
        respondError(
          req, sharedCb, "VALIDATION_MISSING_REQUIRED_FIELD", "verify: token parameter is required"
        );
        return;
    }

    std::string tokenHash = ::fulla::drogon::utils::hashToken(token);
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now().time_since_epoch()
    )
                 .count();

    auto db = getDbOrRespond(req, sharedCb);
    if (!db)
        return;

    // DELETE...RETURNING is a documented raw-SQL exemption (db-operations.md).
    // Atomic consume + get user info in one step.
    db->execSqlAsync(
      "DELETE FROM email_verification_tokens "
      "WHERE token_hash = $1 AND expires_at > $2 "
      "RETURNING user_id, email",
      [sharedCb, db, req](const ::drogon::orm::Result &r) {
          if (r.empty())
          {
              respondError(
                req,
                sharedCb,
                "VALIDATION_VERIFICATION_TOKEN_INVALID",
                "verify: token is invalid or expired"
              );
              return;
          }

          int userId = r[0]["user_id"].as<int>();

          // Mark email as verified via Mapper
          Criteria crit(Users::Cols::_id, CompareOperator::EQ, userId);
          Mapper<Users>(db).findOne(
            crit,
            [sharedCb, db, req](const Users &user) {
                Users updated = user;
                updated.setEmailVerified(true);
                Mapper<Users>(db).update(
                  updated,
                  [sharedCb, user](const size_t) {
                      fulla::drogon::UserCacheInvalidator::instance().invalidateUser(
                        std::to_string(user.getValueOfId()), user.getValueOfPublicSub());
                      Json::Value json;
                      json["message"] = "Email verified successfully";
                      auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                      (*sharedCb)(resp);
                  },
                  [sharedCb, req](const DrogonDbException &e) {
                      respondError(
                        req,
                        sharedCb,
                        "DB_QUERY_ERROR",
                        std::string("Failed to update email_verified: ") + e.base().what()
                      );
                  }
                );
            },
            [sharedCb, req](const DrogonDbException &e) {
                respondError(
                  req,
                  sharedCb,
                  "DB_QUERY_ERROR",
                  std::string("Failed to find user for email_verified update: ") + e.base().what()
                );
            }
          );
      },
      [sharedCb, req](const DrogonDbException &e) {
          respondError(
            req,
            sharedCb,
            "DB_QUERY_ERROR",
            std::string("Email verification failed: ") + e.base().what()
          );
      },
      tokenHash,
      now
    );
}

void EmailVerificationService::resendVerification(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback sharedCb
)
{
    // Get userId from request attributes (set by OAuth2 middleware)
    std::string userId = req->getAttributes()->get<std::string>("userId");
    if (userId.empty())
    {
        respondError(req, sharedCb, "AUTH_TOKEN_INVALID", "resend: missing authenticated user");
        return;
    }

    auto db = getDbOrRespond(req, sharedCb);
    if (!db)
        return;

    // userId attribute holds the OAuth2 subject (public_sub), not internal id.
    Criteria crit(Users::Cols::_public_sub, CompareOperator::EQ, userId);
    Mapper<Users> mapper(db);
    mapper.findOne(
      crit,
      [sharedCb, req](const Users &user) {
          if (user.getValueOfEmailVerified())
          {
              Json::Value json;
              json["message"] = "Email is already verified";
              auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
              (*sharedCb)(resp);
              return;
          }

          std::string email = user.getValueOfEmail().empty() ? "" : user.getValueOfEmail();

          if (email.empty())
          {
              respondError(
                req, sharedCb, "VALIDATION_INVALID_INPUT", "resend: no email address on file"
              );
              return;
          }

          sendVerificationEmail(user.getValueOfId(), email, EmailTemplateService::resolveLocale(req));

          Json::Value json;
          json["message"] = "Verification email sent";
          auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
          (*sharedCb)(resp);
      },
      [sharedCb, req](const DrogonDbException &e) {
          respondError(
            req,
            sharedCb,
            "DB_QUERY_ERROR",
            std::string("Resend verification failed: ") + e.base().what()
          );
      }
    );
}

void EmailVerificationService::notifyNewRegistration(const std::string &email)
{
    if (email.empty())
        return;
    if (!verificationStorageAvailable())
        return;

    auto db = ::drogon::app().getDbClient();
    if (!db)
        return;

    // Registration stores the canonical (normalized) address; normalize the
    // lookup key to match. Fire-and-forget: failures are logged, never
    // surfaced -- the register response must not depend on email delivery.
    const std::string normalized = fulla::common::utils::normalizeEmail(email);
    try
    {
        Mapper<Users> mapper(db);
        mapper.findOne(
          Criteria(Users::Cols::_email, CompareOperator::EQ, normalized),
          [normalized](const Users &user) {
              // A freshly registered account is unverified by definition.
              sendVerificationEmail(user.getValueOfId(), user.getValueOfEmail());
          },
          [](const DrogonDbException &e) {
              LOG_ERROR << "notifyNewRegistration: user lookup failed for "
                           "verification email: "
                        << e.base().what();
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "notifyNewRegistration: Mapper construction failed: " << e.what();
    }
}

void EmailVerificationService::requestVerificationByEmail(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback sharedCb
)
{
    std::string email;
    if (req->contentType() == ::drogon::CT_APPLICATION_JSON)
    {
        auto json = req->getJsonObject();
        if (json)
            email = json->get("email", "").asString();
    }
    else
    {
        email = req->getParameter("email");
    }

    if (email.empty())
    {
        respondError(
          req, sharedCb, "VALIDATION_MISSING_REQUIRED_FIELD", "resend-by-email: email is required"
        );
        return;
    }

    // F-018 request-rate limiter: every accepted request counts toward the
    // per-(ip, email) bucket, so email bombing through this unauthenticated
    // endpoint is capped at the shared auth.rate_limit budget (30/min by
    // default; prod Hodor adds a tighter per-ip sub_limit on top).
    const std::string rlKey = req->getPeerAddr().toIp() + "|verify-resend:" + email;
    auto retry = fulla::common::utils::RateLimiter::instance().checkThrottled(rlKey);
    if (retry.count() > 0)
    {
        Json::Value body;
        body["message"] = "Too many requests; please retry later";
        auto resp = ::drogon::HttpResponse::newHttpJsonResponse(body);
        resp->setStatusCode(::drogon::k429TooManyRequests);
        resp->addHeader("Retry-After", std::to_string(retry.count()));
        resp->addHeader("Cache-Control", "no-store");
        (*sharedCb)(resp);
        return;
    }
    fulla::common::utils::RateLimiter::instance().recordFailure(rlKey);

    if (!verificationStorageAvailable())
    {
        // Memory mode: no persistence, so no token can be stored or
        // verified. Answer the generic response (nothing is sent).
        Json::Value json;
        json["message"] =
          "If the email exists and is unverified, a verification link has been sent";
        auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
        (*sharedCb)(resp);
        return;
    }

    auto db = getDbOrRespond(req, sharedCb);
    if (!db)
        return;

    const std::string normalized = fulla::common::utils::normalizeEmail(email);

    // Anti-enumeration: one identical generic response for "no such user",
    // "already verified", and "sent" -- the caller learns nothing about
    // which emails exist.
    auto respondGeneric = [sharedCb]() {
        Json::Value json;
        json["message"] =
          "If the email exists and is unverified, a verification link has been sent";
        auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
        (*sharedCb)(resp);
    };

    try
    {
        Mapper<Users> mapper(db);
        mapper.findOne(
          Criteria(Users::Cols::_email, CompareOperator::EQ, normalized),
          [respondGeneric, req](const Users &user) {
              if (user.getValueOfEmailVerified())
              {
                  respondGeneric();
                  return;
              }
              std::string stored = user.getValueOfEmail();
              if (stored.empty())
              {
                  respondGeneric();
                  return;
              }
              sendVerificationEmail(
                user.getValueOfId(), stored, EmailTemplateService::resolveLocale(req));
              respondGeneric();
          },
          [respondGeneric](const DrogonDbException &e) {
              // Lookup miss (unknown email) or DB failure -- both answer the
              // generic response; failures are logged server-side only.
              LOG_ERROR << "resend-by-email: user lookup failed: " << e.base().what();
              respondGeneric();
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "resend-by-email: Mapper construction failed: " << e.what();
        respondGeneric();
    }
}

}  // namespace fulla::drogon::services
