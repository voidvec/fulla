#include <fulla/drogon/services/PasswordResetService.h>
#include <fulla/drogon/validation/RuleSet.h>

#include <fulla/storage/postgres/models/PasswordResetTokens.h>
#include <fulla/storage/postgres/models/Users.h>
#include <fulla/drogon/adapters/DrogonAuditSink.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/services/EmailTemplateService.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/common/utils/EmailNormalizer.h>
#include <fulla/drogon/utils/EmailService.h>
#include <fulla/drogon/utils/PasswordHasher.h>

#include <drogon/drogon.h>

#include <chrono>

namespace fulla::drogon::services
{

namespace
{
void respondError(
  const ::drogon::HttpRequestPtr &req,
  const PasswordResetService::ResponseCallback &cb,
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

::drogon::orm::DbClientPtr getDbOrRespond(
  const ::drogon::HttpRequestPtr &req,
  const PasswordResetService::ResponseCallback &cb
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

using namespace ::drogon::orm;
using namespace ::drogon_model::fulla_db;

// ---- public methods ----

void PasswordResetService::requestReset(
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
          req,
          sharedCb,
          "VALIDATION_MISSING_REQUIRED_FIELD",
          "password-reset request: email is required"
        );
        return;
    }

    email = ::fulla::common::utils::normalizeEmail(email);

    auto db = getDbOrRespond(req, sharedCb);
    if (!db)
        return;

    // Look up user by email via Mapper
    Criteria crit(Users::Cols::_email, CompareOperator::EQ, email);
    Mapper<Users> mapper(db);
    mapper.findOne(
      crit,
      [sharedCb, email, db, req](const Users &user) {
          Json::Value json;
          json["message"] = "If the email exists, a reset link has been sent";

          int32_t userId = user.getValueOfId();

          std::string rawToken = ::fulla::drogon::utils::generateSecureToken();
          std::string tokenHash = ::fulla::drogon::utils::hashToken(rawToken);

          auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch()
          )
                       .count();
          int64_t expiresAt = now + 900;  // 15 minutes

          // Insert reset token via Mapper
          PasswordResetTokens resetToken;
          resetToken.setTokenHash(tokenHash);
          resetToken.setUserId(userId);
          resetToken.setExpiresAt(expiresAt);
          // used defaults to false (DB default)

          Mapper<PasswordResetTokens>(db).insert(
            resetToken,
            [sharedCb, json, rawToken, email, req](const PasswordResetTokens &) {
                // P1-2 (#278): localized template send. Reset-link domains
                // come ONLY from global config (config.frontend.url) -- the
                // D8 red line (org-controlled sources would be a phishing
                // chain).
                auto customConfig = ::drogon::app().getCustomConfig();
                std::string frontendUrl = "http://localhost:5173";
                if (customConfig.isMember("frontend") && customConfig["frontend"].isMember("url"))
                    frontendUrl = customConfig["frontend"]["url"].asString();
                std::string resetLink = frontendUrl + "/reset-password?token=" + rawToken;

                EmailTemplateService::send(
                  EmailTemplateService::kKeyPasswordReset,
                  EmailTemplateService::resolveLocale(req),
                  email,
                  {{"reset_link", resetLink}});

                auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                (*sharedCb)(resp);
            },
            [sharedCb, json](const DrogonDbException &e) {
                LOG_ERROR << "Failed to store reset token: " << e.base().what();
                auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                (*sharedCb)(resp);
            }
          );
      },
      [sharedCb](const DrogonDbException &e) {
          LOG_ERROR << "Password reset lookup failed: " << e.base().what();
          Json::Value json;
          json["message"] = "If the email exists, a reset link has been sent";
          auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
          (*sharedCb)(resp);
      }
    );
}

void PasswordResetService::confirmReset(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback sharedCb
)
{
    std::string token, newPassword;
    if (req->contentType() == ::drogon::CT_APPLICATION_JSON)
    {
        auto json = req->getJsonObject();
        if (json)
        {
            token = json->get("token", "").asString();
            newPassword = json->get("new_password", "").asString();
        }
    }
    else
    {
        token = req->getParameter("token");
        newPassword = req->getParameter("new_password");
    }

    if (token.empty() || newPassword.empty())
    {
        respondError(
          req,
          sharedCb,
          "VALIDATION_MISSING_REQUIRED_FIELD",
          "password-reset confirm: token and new_password are required"
        );
        return;
    }

    if (newPassword.length() < fulla::drogon::validation::RuleSet::passwordMinLength())
    {
        respondError(
          req,
          sharedCb,
          "VALIDATION_PASSWORD_TOO_SHORT",
          "password-reset confirm: password must be at least " +
            std::to_string(fulla::drogon::validation::RuleSet::passwordMinLength()) + " characters"
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

    // UPDATE...RETURNING is a documented raw-SQL exemption
    db->execSqlAsync(
      "UPDATE password_reset_tokens SET used = true "
      "WHERE token_hash = $1 AND used = false AND expires_at > $2 "
      "RETURNING user_id",
      [sharedCb, newPassword, db, req](const ::drogon::orm::Result &r) {
          if (r.empty())
          {
              respondError(
                req,
                sharedCb,
                "VALIDATION_RESET_TOKEN_INVALID",
                "password-reset confirm: token is invalid, expired, or already used"
              );
              return;
          }

          int userId = r[0]["user_id"].as<int>();

          std::string newHash;
          try
          {
              newHash = ::fulla::common::utils::PasswordHasher::hash(newPassword);
          }
          catch (const std::exception &e)
          {
              respondError(
                req, sharedCb, "INTERNAL_ERROR", std::string("Password hashing failed: ") + e.what()
              );
              return;
          }

          // Update password via Mapper
          Criteria userCrit(Users::Cols::_id, CompareOperator::EQ, userId);
          Mapper<Users>(db).findOne(
            userCrit,
            [sharedCb, req, db, userId, newHash](const Users &user) {
                Users updated = user;
                updated.setPasswordHash(newHash);
                updated.setSalt("");

                Mapper<Users>(db).update(
                  updated,
                  [sharedCb, req, db, userId, publicSub = user.getValueOfPublicSub()](
                    const size_t) {
                      std::string userIdStr = std::to_string(userId);

                      // P0-3 audit fix: token rows carry the subject in the
                      // shape the issuing path used (login/MFA/social store
                      // the public_sub UUID; authorize silent re-auth and
                      // consent store the internal id). Revoke under BOTH
                      // keys, like deleteAccount does — the previous
                      // internal-id-only predicate missed every
                      // public-sub-keyed token while the response claimed
                      // all sessions had been revoked.
                      // Bulk token revocation: documented raw-SQL exemptions
                      // (db-operations.md — documented batch operations).
                      auto respond = [sharedCb, req, userId]() {
                          ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
                            ::drogon::app().getPlugin<::OAuth2Plugin>()->getAuditSink(),
                            "password_reset",
                            "success",
                            req,
                            std::to_string(userId),
                            "user",
                            std::to_string(userId)
                          );
                          Json::Value json;
                          json["message"] = "Password reset successful";
                          json["note"] = "All existing sessions have been revoked";
                          auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                          (*sharedCb)(resp);
                      };
                      // P2-11: also consume any OTHER outstanding reset
                      // tokens for this user — an intercepted second link
                      // must not survive the password it was minted for.
                      auto invalidateOtherResetTokens = [db, userId, respond]() {
                          db->execSqlAsync(
                            "UPDATE password_reset_tokens SET used = true "
                            "WHERE user_id = $1 AND used = false",
                            [respond](const ::drogon::orm::Result &) { respond(); },
                            [respond](const ::drogon::orm::DrogonDbException &) { respond(); },
                            userId
                          );
                      };
                      auto revokeRefreshTokens = [sharedCb, req, db, userId, userIdStr, publicSub,
                                                  invalidateOtherResetTokens]() {
                          db->execSqlAsync(
                            "UPDATE oauth2_refresh_tokens SET revoked = true "
                            "WHERE user_id = $1 OR user_id = $2",
                            [invalidateOtherResetTokens](const ::drogon::orm::Result &) {
                                invalidateOtherResetTokens();
                            },
                            [invalidateOtherResetTokens](const ::drogon::orm::DrogonDbException &) {
                                invalidateOtherResetTokens();
                            },
                            publicSub,
                            userIdStr
                          );
                      };
                      db->execSqlAsync(
                        "UPDATE oauth2_access_tokens SET revoked = true "
                        "WHERE user_id = $1 OR user_id = $2",
                        [revokeRefreshTokens](const ::drogon::orm::Result &) {
                            revokeRefreshTokens();
                        },
                        [revokeRefreshTokens](const ::drogon::orm::DrogonDbException &) {
                            revokeRefreshTokens();
                        },
                        publicSub,
                        userIdStr
                      );
                  },
                  [sharedCb, req](const DrogonDbException &e) {
                      respondError(
                        req,
                        sharedCb,
                        "DB_QUERY_ERROR",
                        std::string("Failed to update password: ") + e.base().what()
                      );
                  }
                );
            },
            [sharedCb, req](const DrogonDbException &e) {
                respondError(
                  req,
                  sharedCb,
                  "DB_QUERY_ERROR",
                  std::string("Failed to find user for password update: ") + e.base().what()
                );
            }
          );
      },
      [sharedCb, req](const DrogonDbException &e) {
          respondError(
            req,
            sharedCb,
            "DB_QUERY_ERROR",
            std::string("Reset token lookup failed: ") + e.base().what()
          );
      },
      tokenHash,
      now
    );
}

}  // namespace fulla::drogon::services
