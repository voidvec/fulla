#include <fulla/drogon/controllers/MfaController.h>
#include <fulla/identity/TotpUtils.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/adapters/DrogonAuditSink.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <drogon/drogon.h>
#include <chrono>

// Task 24 slice 5 (fulla-sdk-refactor): identity-layer services this
// controller now optionally consumes.
#include <fulla/identity/IUserRepository.h>
#include <fulla/identity/MfaService.h>

#include <fulla/storage/postgres/models/Users.h>

using namespace ::drogon::orm;

namespace fulla::drogon::controllers
{

OAuth2Plugin *MfaController::resolvePlugin() const
{
    return plugin_ ? plugin_ : ::drogon::app().getPlugin<OAuth2Plugin>();
}

namespace
{

// #122: the legacy fallback paths below use the identity-domain TOTP free
// functions (fulla::identity::totp) instead of the retired drogon-static
// copy. The crypto provider is the shared CryptoUtils.h
// detail::cryptoProvider() process-lifetime instance (no per-file duplicate).

int64_t mfaNowSeconds()
{
    return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      ).count()
    );
}

void respondError(
  const ::drogon::HttpRequestPtr &req,
  const std::shared_ptr<std::function<void(const ::drogon::HttpResponsePtr &)>> &cb,
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

struct MfaControllerDocs
{
    MfaControllerDocs()
    {
        ::fulla::drogon::observability::openapi::EndpointInfo setupDocs;
        // Path must equal the ADD_METHOD_TO route (MfaController.h): the MFA
        // self-service routes live under /api/me/mfa/*, NOT /oauth2/mfa/*
        // (those old paths have no backing routes — OpenAPI governance gate
        // checks docs == routes).
        setupDocs.path = "/api/me/mfa/setup";
        setupDocs.method = "POST";
        setupDocs.summary = "Setup MFA";
        setupDocs.description = "Initiate MFA setup by generating a TOTP secret.";
        setupDocs.tags = {"MFA"};
        setupDocs.requiresAuth = true;
        ::fulla::drogon::observability::openapi::OpenApiGenerator::addEndpoint(setupDocs);

        ::fulla::drogon::observability::openapi::EndpointInfo verifySetupDocs;
        verifySetupDocs.path = "/api/me/mfa/verify";
        verifySetupDocs.method = "POST";
        verifySetupDocs.summary = "Verify MFA Setup";
        verifySetupDocs.description = "Verify a TOTP code to finalize MFA setup.";
        verifySetupDocs.tags = {"MFA"};
        verifySetupDocs.requiresAuth = true;
        ::fulla::drogon::observability::openapi::OpenApiGenerator::addEndpoint(verifySetupDocs);

        ::fulla::drogon::observability::openapi::EndpointInfo disableDocs;
        disableDocs.path = "/api/me/mfa/disable";
        disableDocs.method = "POST";
        disableDocs.summary = "Disable MFA";
        disableDocs.description = "Disable MFA for the authenticated user.";
        disableDocs.tags = {"MFA"};
        disableDocs.requiresAuth = true;
        ::fulla::drogon::observability::openapi::OpenApiGenerator::addEndpoint(disableDocs);

        ::fulla::drogon::observability::openapi::EndpointInfo verifyDocs;
        verifyDocs.path = "/oauth2/mfa/verify";
        verifyDocs.method = "POST";
        verifyDocs.summary = "Verify MFA Code (Login)";
        verifyDocs.description =
          "Verify MFA (TOTP) code during login. Completes the authorization-code "
          "issuance started by /oauth2/login when MFA was required. The PKCE "
          "code_verifier (C4, RFC 7636) must be supplied so the internally-"
          "generated code passes PKCE verification against the code_challenge "
          "persisted on the session during the first-factor login step.";
        verifyDocs.tags = {"MFA"};
        verifyDocs.requiresAuth = false;

        // Parameters (were missing entirely — the endpoint had no documented
        // parameters, so client generators emitted clients that didn't send
        // mfa_token/code/client_id/redirect_uri/code_verifier).
        auto mkStrParam = [](const char *name, const char *desc, bool required) {
            ::fulla::drogon::observability::openapi::ParameterInfo p;
            p.name = name;
            p.description = desc;
            p.type = ::fulla::drogon::observability::openapi::ParameterType::STRING;
            p.location = ::fulla::drogon::observability::openapi::ParameterLocation::QUERY;
            p.required = required;
            return p;
        };
        verifyDocs.parameters = {
          mkStrParam("mfa_token", "The MFA pending token returned by /oauth2/login when mfa_required was true.", true),
          mkStrParam("code", "The 6-digit TOTP code from the user's authenticator.", true),
          mkStrParam("client_id", "The client_id from the original login (must match).", true),
          mkStrParam("redirect_uri", "The redirect_uri from the original login (must match).", true),
          mkStrParam("code_verifier",
                     "PKCE code_verifier (RFC 7636) matching the code_challenge sent on "
                     "the first-factor /oauth2/login step. Required for PUBLIC clients.",
                     false),
        };
        verifyDocs.responses = {{200, "MFA verification successful — returns the token pair"},
                                {400, "Invalid request (missing mfa_token/code, malformed TOTP)"},
                                {401, "Invalid MFA code or client/redirect mismatch"}};
        ::fulla::drogon::observability::openapi::OpenApiGenerator::addEndpoint(verifyDocs);
    }
};

MfaControllerDocs docs_;

}  // namespace

void MfaController::setup(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    std::string userId = req->getAttributes()->get<std::string>("userId");
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));

    // Task 24 slice 5: prefer the injected fulla::identity::MfaService
    // (constructed once at startup by
    // bootstrap::wireIdentityServices()/OAuth2Server/bootstrap/
    // IdentityAssembly.cc), falling back to the pre-Task-24 raw SQL below
    // when unwired -- same injected-with-fallback pattern established by
    // SessionController's Task 24 slice 4.
    if (mfaService_ && userRepo_)
    {
        userRepo_->findByPublicSub(
          userId, [this, sharedCb, req, userId](std::optional<fulla::identity::UserData> user) {
              if (!user)
              {
                  respondError(
                    req, sharedCb, "AUTH_INVALID_CREDENTIALS", "MFA setup: unknown user"
                  );
                  return;
              }
              // Account label = username (readable in the authenticator),
            // not the opaque public_sub.
            mfaService_->setupSecret(
                user->id,
                user->username,
                [sharedCb, req](std::optional<fulla::identity::MfaSetupResult> result) {
                    if (!result)
                    {
                        respondError(
                          req, sharedCb, "DB_QUERY_ERROR", "MFA setup: failed to store secret"
                        );
                        return;
                    }
                    Json::Value json;
                    json["secret"] = result->secret;
                    json["otpauth_uri"] = result->otpAuthUri;
                    json["message"] =
                      "Scan the QR code with your authenticator app, then verify with a code";
                    auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                    (*sharedCb)(resp);
                }
              );
          }
        );
        return;
    }

    std::string secret = ::fulla::identity::totp::generateSecret(::fulla::drogon::utils::detail::cryptoProvider());

    // Fallback leg: same TOTP issuer contract as the injected service
    // (custom_config.mfa.totp_issuer, default "Fulla").
    std::string totpIssuer = "Fulla";
    {
        const auto &customConfig = ::drogon::app().getCustomConfig();
        if (customConfig.isMember("mfa") && customConfig["mfa"].isMember("totp_issuer"))
        {
            const std::string configured = customConfig["mfa"]["totp_issuer"].asString();
            if (!configured.empty())
                totpIssuer = configured;
        }
    }

    auto db = ::drogon::app().getDbClient();
    // #54: deleted_at filter — a soft-deleted user must not mutate MFA state
    // (V024: deleted users are excluded from all queries).
    try
    {
        Mapper<drogon_model::fulla_db::Users>(db).findBy(
          Criteria(drogon_model::fulla_db::Users::Cols::_public_sub, CompareOperator::EQ, userId) &&
            Criteria(drogon_model::fulla_db::Users::Cols::_deleted_at, CompareOperator::IsNull),
          [sharedCb, secret, userId, db, req, totpIssuer](
            const std::vector<drogon_model::fulla_db::Users> &users
          ) {
              if (users.empty())
              {
                  respondError(
                    req, sharedCb, "AUTH_INVALID_CREDENTIALS", "MFA setup failed: user not found"
                  );
                  return;
              }
              drogon_model::fulla_db::Users updated = users[0];
              const std::string accountLabel = updated.getValueOfUsername();
              updated.setMfaSecret(secret);
              try
              {
                  Mapper<drogon_model::fulla_db::Users>(db).update(
                    updated,
                    [sharedCb, secret, accountLabel, totpIssuer](const size_t) {
                        std::string otpUri = ::fulla::identity::totp::generateOtpAuthUri(
                          secret, accountLabel, totpIssuer
                        );
                        Json::Value json;
                        json["secret"] = secret;
                        json["otpauth_uri"] = otpUri;
                        json["message"] =
                          "Scan the QR code with your authenticator app, then verify with a code";
                        auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                        (*sharedCb)(resp);
                    },
                    [sharedCb, req](const ::drogon::orm::DrogonDbException &e) {
                        respondError(
                          req,
                          sharedCb,
                          "DB_QUERY_ERROR",
                          std::string("MFA setup failed: ") + e.base().what()
                        );
                    }
                  );
              }
              catch (...)
              {
                  respondError(
                    req, sharedCb, "DB_QUERY_ERROR", "MFA setup: update Mapper construction failed"
                  );
              }
          },
          [sharedCb, req](const ::drogon::orm::DrogonDbException &e) {
              respondError(
                req, sharedCb, "DB_QUERY_ERROR", std::string("MFA setup failed: ") + e.base().what()
              );
          }
        );
    }
    catch (...)
    {
        respondError(
          req, sharedCb, "DB_QUERY_ERROR", "MFA setup: find Mapper construction failed"
        );
    }
}

void MfaController::verifySetup(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    std::string userId = req->getAttributes()->get<std::string>("userId");
    std::string code;
    if (req->contentType() == ::drogon::CT_APPLICATION_JSON)
    {
        auto json = req->getJsonObject();
        if (json)
            code = json->get("code", "").asString();
    }
    else
    {
        code = req->getParameter("code");
    }

    if (code.empty() || code.length() != 6)
    {
        ::fulla::common::error::ErrorResponder::respond(
          req,
          std::move(callback),
          "VALIDATION_FORMAT_ERROR",
          "verifySetup: 6-digit TOTP code is required"
        );
        return;
    }

    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));

    // Task 24 slice 5: prefer the injected MfaService, same pattern as
    // setup() above.
    if (mfaService_ && userRepo_)
    {
        userRepo_->findByPublicSub(
          userId,
          [this, sharedCb, req, userId, code](std::optional<fulla::identity::UserData> user) {
              if (!user)
              {
                  respondError(
                    req, sharedCb, "AUTH_INVALID_CREDENTIALS", "verifySetup: unknown user"
                  );
                  return;
              }
              mfaService_->verifyAndEnable(
                user->id,
                code,
                [sharedCb,
                 req,
                 userId](std::optional<fulla::identity::MfaEnableResult> result) {
                    if (!result)
                    {
                        // MfaService::verifyAndEnable collapses "no
                        // secret set up" and "code mismatch" into one
                        // nullopt signal -- mirror the legacy path's
                        // more specific AUTH_MFA_NOT_CONFIGURED (the
                        // more actionable of the two for a client that
                        // never called /setup) since we cannot
                        // distinguish here.
                        respondError(
                          req,
                          sharedCb,
                          "AUTH_MFA_NOT_CONFIGURED",
                          "verifySetup: MFA not set up or TOTP code is incorrect"
                        );
                        return;
                    }
                    ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
                      ::drogon::app().getPlugin<::OAuth2Plugin>()->getAuditSink(),
                      "mfa_enabled",
                      "success",
                      req,
                      userId,
                      "user",
                      userId
                    );
                    Json::Value codesJson(Json::arrayValue);
                    for (const auto &bc : result->backupCodes)
                        codesJson.append(bc);
                    Json::Value json;
                    json["message"] = "MFA enabled successfully";
                    json["backup_codes"] = codesJson;
                    json["warning"] =
                      "Save these backup codes securely. They cannot be shown again.";
                    auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                    (*sharedCb)(resp);
                }
              );
          }
        );
        return;
    }

    auto db = ::drogon::app().getDbClient();
    // #54: deleted_at filter — a soft-deleted user must not enable MFA.
    try
    {
        Mapper<drogon_model::fulla_db::Users>(db).findBy(
          Criteria(drogon_model::fulla_db::Users::Cols::_public_sub, CompareOperator::EQ, userId) &&
            Criteria(drogon_model::fulla_db::Users::Cols::_deleted_at, CompareOperator::IsNull),
          [sharedCb, code, userId, db, req](const std::vector<drogon_model::fulla_db::Users> &users) {
              if (users.empty())
              {
                  respondError(
                    req,
                    sharedCb,
                    "AUTH_MFA_NOT_CONFIGURED",
                    "verifySetup: MFA not set up. Call /api/me/mfa/setup first"
                  );
                  return;
              }

              std::string secret = users[0].getValueOfMfaSecret();
              if (secret.empty())
              {
                  respondError(
                    req,
                    sharedCb,
                    "AUTH_MFA_NOT_CONFIGURED",
                    "verifySetup: MFA not set up. Call /api/me/mfa/setup first"
                  );
                  return;
              }

              if (!::fulla::identity::totp::verifyCode(secret, code, mfaNowSeconds()))
              {
                  respondError(
                    req, sharedCb, "AUTH_MFA_CODE_INVALID", "verifySetup: TOTP code is incorrect"
                  );
                  return;
              }

              auto backupCodes = ::fulla::identity::totp::generateBackupCodes(::fulla::drogon::utils::detail::cryptoProvider(), 10);
              Json::Value codesJson(Json::arrayValue);
              Json::Value hashedCodesJson(Json::arrayValue);
              for (const auto &bc : backupCodes)
              {
                  codesJson.append(bc);
                  hashedCodesJson.append(::fulla::drogon::utils::hashToken(bc));
              }

              Json::StreamWriterBuilder writer;
              writer["indentation"] = "";
              std::string hashedCodesStr = Json::writeString(writer, hashedCodesJson);

              drogon_model::fulla_db::Users updated = users[0];
              updated.setMfaEnabled(true);
              updated.setMfaBackupCodes(hashedCodesStr);
              try
              {
                  Mapper<drogon_model::fulla_db::Users>(db).update(
                    updated,
                    [sharedCb, codesJson, userId, req](const size_t) {
                        ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
                          ::drogon::app().getPlugin<::OAuth2Plugin>()->getAuditSink(),
                          "mfa_enabled",
                          "success",
                          req,
                          userId,
                          "user",
                          userId
                        );
                        Json::Value json;
                        json["message"] = "MFA enabled successfully";
                        json["backup_codes"] = codesJson;
                        json["warning"] = "Save these backup codes securely. They cannot be shown again.";
                        auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                        (*sharedCb)(resp);
                    },
                    [sharedCb, req](const ::drogon::orm::DrogonDbException &e) {
                        respondError(
                          req,
                          sharedCb,
                          "DB_QUERY_ERROR",
                          std::string("MFA enable failed: ") + e.base().what()
                        );
                    }
                  );
              }
              catch (...)
              {
                  respondError(
                    req, sharedCb, "DB_QUERY_ERROR",
                    "verifySetup: update Mapper construction failed"
                  );
              }
          },
          [sharedCb, req](const ::drogon::orm::DrogonDbException &e) {
              respondError(
                req,
                sharedCb,
                "DB_QUERY_ERROR",
                std::string("MFA verify setup failed: ") + e.base().what()
              );
          }
        );
    }
    catch (...)
    {
        respondError(
          req, sharedCb, "DB_QUERY_ERROR", "verifySetup: find Mapper construction failed"
        );
    }
}

void MfaController::disable(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    std::string userId = req->getAttributes()->get<std::string>("userId");

    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));

    // Task 24 slice 5: prefer the injected MfaService, same pattern as
    // setup()/verifySetup() above.
    if (mfaService_ && userRepo_)
    {
        userRepo_->findByPublicSub(
          userId, [this, sharedCb, req](std::optional<fulla::identity::UserData> user) {
              if (!user)
              {
                  respondError(
                    req, sharedCb, "AUTH_INVALID_CREDENTIALS", "MFA disable: unknown user"
                  );
                  return;
              }
              mfaService_->disable(user->id, [sharedCb, req](bool ok) {
                  if (!ok)
                  {
                      respondError(
                        req, sharedCb, "DB_QUERY_ERROR", "MFA disable: repository write failed"
                      );
                      return;
                  }
                  Json::Value json;
                  json["message"] = "MFA disabled successfully";
                  auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                  (*sharedCb)(resp);
              });
          }
        );
        return;
    }

    auto db = ::drogon::app().getDbClient();
    // #54: deleted_at filter — a soft-deleted user must not disable MFA
    // (mutating a deleted account's state).
    try
    {
        Mapper<drogon_model::fulla_db::Users>(db).findBy(
          Criteria(drogon_model::fulla_db::Users::Cols::_public_sub, CompareOperator::EQ, userId) &&
            Criteria(drogon_model::fulla_db::Users::Cols::_deleted_at, CompareOperator::IsNull),
          [sharedCb, db, req](const std::vector<drogon_model::fulla_db::Users> &users) {
              if (users.empty())
              {
                  respondError(
                    req, sharedCb, "AUTH_INVALID_CREDENTIALS", "MFA disable failed: user not found"
                  );
                  return;
              }
              drogon_model::fulla_db::Users updated = users[0];
              updated.setMfaEnabled(false);
              updated.setMfaSecretToNull();
              updated.setMfaBackupCodesToNull();
              try
              {
                  Mapper<drogon_model::fulla_db::Users>(db).update(
                    updated,
                    [sharedCb](const size_t) {
                        Json::Value json;
                        json["message"] = "MFA disabled successfully";
                        auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                        (*sharedCb)(resp);
                    },
                    [sharedCb, req](const ::drogon::orm::DrogonDbException &e) {
                        respondError(
                          req,
                          sharedCb,
                          "DB_QUERY_ERROR",
                          std::string("MFA disable failed: ") + e.base().what()
                        );
                    }
                  );
              }
              catch (...)
              {
                  respondError(
                    req, sharedCb, "DB_QUERY_ERROR",
                    "MFA disable: update Mapper construction failed"
                  );
              }
          },
          [sharedCb, req](const ::drogon::orm::DrogonDbException &e) {
              respondError(
                req, sharedCb, "DB_QUERY_ERROR", std::string("MFA disable failed: ") + e.base().what()
              );
          }
        );
    }
    catch (...)
    {
        respondError(
          req, sharedCb, "DB_QUERY_ERROR", "MFA disable: find Mapper construction failed"
        );
    }
}

void MfaController::verifyLogin(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    std::string mfaToken, code;
    std::string clientId, redirectUri, scope, nonce;
    std::string codeVerifier;  // C4 (RFC 7636): PKCE verifier for the MFA-completed code exchange
    if (req->contentType() == ::drogon::CT_APPLICATION_JSON)
    {
        auto json = req->getJsonObject();
        if (json)
        {
            mfaToken = json->get("mfa_token", "").asString();
            code = json->get("code", "").asString();
            clientId = json->get("client_id", "").asString();
            redirectUri = json->get("redirect_uri", "").asString();
            scope = json->get("scope", "").asString();
            nonce = json->get("nonce", "").asString();
            codeVerifier = json->get("code_verifier", "").asString();
        }
    }
    else
    {
        mfaToken = req->getParameter("mfa_token");
        code = req->getParameter("code");
        clientId = req->getParameter("client_id");
        redirectUri = req->getParameter("redirect_uri");
        scope = req->getParameter("scope");
        nonce = req->getParameter("nonce");
        codeVerifier = req->getParameter("code_verifier");
    }

    if (mfaToken.empty() || code.empty())
    {
        ::fulla::common::error::ErrorResponder::respond(
          req,
          std::move(callback),
          "VALIDATION_MISSING_REQUIRED_FIELD",
          "verifyLogin: mfa_token and code are required"
        );
        return;
    }


    if (clientId.empty() || redirectUri.empty())
    {
        ::fulla::common::error::ErrorResponder::respond(
          req,
          std::move(callback),
          "VALIDATION_MISSING_REQUIRED_FIELD",
          "verifyLogin: client_id and redirect_uri are required to issue tokens after MFA"
        );
        return;
    }
    if (scope.empty())
    {
        scope = "openid profile email";
    }

    // #145: accounts flagged must_change_password must not obtain
    // authorization codes even through the MFA completion path -- the flag
    // is checked at login BEFORE the MFA branch, so a flagged session here
    // means the marker was set by that earlier login.
    if (
      req->session() && req->session()->find("must_change_password") &&
      req->session()->get<bool>("must_change_password")
    )
    {
        ::fulla::common::error::ErrorResponder::respond(
          req,
          std::move(callback),
          "AUTH_PASSWORD_CHANGE_REQUIRED",
          "verifyLogin: change the account password before tokens are issued"
        );
        return;
    }

    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));

    auto plugin = resolvePlugin();
    if (!plugin)
    {
        respondError(req, sharedCb, "INTERNAL_ERROR", "verifyLogin: OAuth2 Plugin not loaded");
        return;
    }

    // Task 24 slice 5: continuation shared by both the injected-MfaService
    // path and the legacy raw-SQL fallback below -- both resolve to the
    // same (publicSub, pendingClientId, pendingRedirectUri, onSuccess)
    // shape once TOTP verification has already succeeded, so the
    // plugin->validateClient/validateRedirectUri/generateAuthorizationCode/
    // exchangeCodeForToken orchestration (an oauth2-domain concern, see
    // MfaService.h's own scope-boundary comment on why this stays outside
    // that class) is written once, not duplicated per path.
    // F-021/F-022 (OIDC Core §3.1.3.7): MFA verify completes the second
    // factor, so the issued code/refresh tokens reflect both password and
    // MFA. auth_time = now (the moment authentication fully completed) and
    // amr = "pwd mfa" (acr will be "2" = MFA in the id_token). Also refresh
    // the session's auth_time/amr so a subsequent authorize silent re-auth
    // in the same browser session picks up the MFA-elevated values.
    // #144: the session writes happen ONLY after the TOTP code actually
    // verified (inside onTotpVerified, and mirrored in the legacy success
    // block below). Writing them here, before verification, let a FAILED
    // verify leave the session elevated to amr="pwd mfa" -- a following
    // authorize silent re-auth would then mint an MFA-claimed code although
    // the second factor never completed (amr must reflect methods actually
    // performed).
    auto mfaNowSecs = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()
    )
                        .count();
    int64_t mfaAuthTime = static_cast<int64_t>(mfaNowSecs);
    std::string mfaAmr = "pwd mfa";
    auto elevateSessionAfterMfa = [req, mfaAuthTime, mfaAmr]() {
        if (req->session())
        {
            // Session::insert does NOT overwrite an existing key (std::map
            // semantics), so re-writing auth_time/amr requires erase-first —
            // otherwise a password login's amr="pwd" would stick forever and
            // silent re-auth would never pick up the MFA-elevated values.
            req->session()->erase("auth_time");
            req->session()->insert("auth_time", mfaAuthTime);
            req->session()->erase("amr");
            req->session()->insert("amr", mfaAmr);
            // #144: the second factor completed -- the session is no longer
            // MFA-pending (consent/authorize gates stop refusing it).
            req->session()->erase("mfa_pending");
            // PR #157 review MAJOR 2: the first-factor PKCE challenge was
            // threaded onto the code just issued; consumed here so a second
            // MFA login in the same session can never bind its code to the
            // previous flow's challenge.
            req->session()->erase("mfa_code_challenge");
            req->session()->erase("mfa_code_challenge_method");
        }
    };
    auto onTotpVerified = [sharedCb, req, plugin, clientId, redirectUri, scope, nonce, mfaToken, mfaAuthTime, mfaAmr, codeVerifier, elevateSessionAfterMfa](
                            std::string publicSub,
                            std::string pendingClientId,
                            std::string pendingRedirectUri,
                            std::function<void(std::function<void()> &&)> clearPendingBinding
                          ) {
        elevateSessionAfterMfa();
        // P0-4 audit fix: the shared issuance guard replaces the former
        // validateClient + validateRedirectUri pair and adds the scope
        // allowlist check neither of them performed.
        plugin->checkCodeIssuanceGuards(
          clientId,
          redirectUri,
          scope,
          [sharedCb,
           req,
           plugin,
           clientId,
           redirectUri,
           publicSub,
           pendingClientId,
           pendingRedirectUri,
           scope,
           nonce,
           mfaAuthTime,
           mfaAmr,
           clearPendingBinding,
           codeVerifier](::OAuth2Plugin::CodeIssuanceGuardResult guard) {
              if (!guard.ok)
              {
                  // #144 contract: verifyLogin answers every client/redirect/
                  // scope rejection with the SAME generic AUTH_INVALID_CREDENTIALS
                  // (401) — distinguishing unknown-client / unregistered-redirect
                  // / over-scope would give a prober an oracle. The guard's
                  // specific reason stays in the server-side log detail.
                  respondError(
                    req,
                    sharedCb,
                    "AUTH_INVALID_CREDENTIALS",
                    "verifyLogin: code issuance guard rejected the request: " + guard.detail
                  );
                  return;
              }

                    if (clientId != pendingClientId || redirectUri != pendingRedirectUri)
                    {
                        respondError(
                          req,
                          sharedCb,
                          "AUTH_INVALID_CREDENTIALS",
                          "verifyLogin: client/redirect_uri does not match login session"
                        );
                        return;
                    }

                    // C4 (RFC 7636): read the first-factor PKCE challenge back
                    // from the session (SessionController::login stored it when
                    // MFA was triggered) and thread it onto the code generation.
                    // The client supplies the matching code_verifier (extracted
                    // above), passed to exchangeCodeForToken below. Without this
                    // the MFA path generated+exchanged with empty PKCE params.
                    std::string sessCodeChallenge;
                    std::string sessCodeChallengeMethod;
                    if (req->session())
                    {
                        if (req->session()->find("mfa_code_challenge"))
                            sessCodeChallenge = req->session()->get<std::string>("mfa_code_challenge");
                        if (req->session()->find("mfa_code_challenge_method"))
                            sessCodeChallengeMethod =
                              req->session()->get<std::string>("mfa_code_challenge_method");
                    }

                    plugin->generateAuthorizationCode(
                      clientId,
                      publicSub,
                      scope,
                      redirectUri,
                      sessCodeChallenge,
                      sessCodeChallengeMethod,
                      nonce,
                      [sharedCb,
                       req,
                       plugin,
                       clientId,
                       redirectUri,
                       publicSub,
                       clearPendingBinding,
                       codeVerifier](
                        bool success, std::string authCode, std::string genError
                      ) {
                          if (!success)
                          {
                              respondError(
                                req,
                                sharedCb,
                                "INTERNAL_ERROR",
                                "verifyLogin: failed to generate authorization code: " + genError
                              );
                              return;
                          }

                          plugin->exchangeCodeForToken(
                            authCode,
                            clientId,
                            "",
                            redirectUri,
                            codeVerifier,
                            [sharedCb, req, publicSub, clearPendingBinding](
                              const Json::Value &tokenResult
                            ) {
                                if (tokenResult.isMember("error"))
                                {
                                    std::string detail =
                                      tokenResult.isMember("error_description")
                                        ? tokenResult["error_description"].asString()
                                        : tokenResult["error"].asString();
                                    respondError(
                                      req,
                                      sharedCb,
                                      "INTERNAL_ERROR",
                                      "verifyLogin: failed to exchange authorization code: " +
                                        detail
                                    );
                                    return;
                                }

                                ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
                                  ::drogon::app().getPlugin<::OAuth2Plugin>()->getAuditSink(),
                                  "mfa_verified",
                                  "success",
                                  req,
                                  publicSub,
                                  "user",
                                  publicSub
                                );

                                Json::Value json = tokenResult;
                                json["message"] = "MFA verification successful";
                                json["mfa_verified"] = true;

                                auto sendSuccess = [sharedCb, json]() {
                                    auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                                    (*sharedCb)(resp);
                                };
                                clearPendingBinding(std::move(sendSuccess));
                            }
                          );
                      },
                      mfaAuthTime,
                      mfaAmr
                    );
              }
          );
    };

    // Task 24 slice 5: prefer the injected MfaService/IUserRepository,
    // falling back to the pre-Task-24 raw SQL when unwired -- same
    // injected-with-fallback pattern established by SessionController's
    // Task 24 slice 4. P2-2: mfaToken is now the PUBLIC subject (login
    // stopped handing out the internal auto-increment id); resolve it via
    // findByPublicSub.
    if (mfaService_ && userRepo_)
    {
        if (mfaToken.empty())
        {
            respondError(
              req, sharedCb, "AUTH_INVALID_CREDENTIALS", "verifyLogin: invalid MFA session"
            );
            return;
        }

        userRepo_->findByPublicSub(
          mfaToken,
          [this, sharedCb, req, code, mfaToken, onTotpVerified](std::optional<fulla::identity::UserData> user) {
              if (!user)
              {
                  respondError(
                    req, sharedCb, "AUTH_INVALID_CREDENTIALS", "verifyLogin: invalid MFA session"
                  );
                  return;
              }
              // #54: verifyLogin COMPLETES a login (mints an authorization
              // code + tokens), so the same liveness rules as the login
              // entry apply: a user soft-deleted or locked between the first
              // factor and this second factor must not receive fresh tokens.
              // findByPublicSub already filters deleted_at; locked is checked
              // here.
              int64_t nowSecs = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch()
              )
                                  .count();
              if (user->lockedUntil > nowSecs)
              {
                  respondError(
                    req, sharedCb, "AUTH_INVALID_CREDENTIALS", "verifyLogin: account is locked"
                  );
                  return;
              }
              std::string publicSub = user->publicSub;
              int32_t userId = user->id;
              mfaService_->verifyLoginCode(
                userId,
                code,
                [this, sharedCb, req, userId, publicSub, onTotpVerified](bool codeValid) {
                    if (!codeValid)
                    {
                        respondError(
                          req,
                          sharedCb,
                          "AUTH_INVALID_CREDENTIALS",
                          "verifyLogin: TOTP code is incorrect"
                        );
                        return;
                    }
                    mfaService_->getPendingBinding(
                      userId,
                      [this, sharedCb, publicSub, userId, onTotpVerified](
                        std::optional<std::pair<std::string, std::string>> pending
                      ) {
                          std::string pendingClientId = pending ? pending->first : "";
                          std::string pendingRedirectUri = pending ? pending->second : "";
                          auto clearPendingBinding = [this, userId](std::function<void()> &&done) {
                              mfaService_->clearPendingBinding(
                                userId, [done = std::move(done)](bool) { done(); }
                              );
                          };
                          onTotpVerified(
                            publicSub, pendingClientId, pendingRedirectUri, clearPendingBinding
                          );
                      }
                    );
                }
              );
          }
        );
        return;
    }

    // P2-2: legacy fallback — mfaToken is the public subject here too.
    if (mfaToken.empty())
    {
        respondError(req, sharedCb, "AUTH_INVALID_CREDENTIALS", "verifyLogin: invalid MFA session");
        return;
    }

    auto db = ::drogon::app().getDbClient();
    // #54: fallback lookup must match the wired path's liveness rules —
    // deleted_at IS NULL plus a locked check. Previously a soft-deleted user
    // holding an outstanding mfa_token could complete MFA and receive fresh
    // tokens (V024 contract bypass, TOCTOU window between password login and
    // MFA verify).
    try
    {
        Mapper<drogon_model::fulla_db::Users>(db).findBy(
          Criteria(drogon_model::fulla_db::Users::Cols::_public_sub, CompareOperator::EQ, mfaToken) &&
            Criteria(drogon_model::fulla_db::Users::Cols::_deleted_at, CompareOperator::IsNull),
          [sharedCb, code, mfaToken, req, clientId, redirectUri, scope, nonce, plugin, mfaAuthTime, mfaAmr, elevateSessionAfterMfa](
            const std::vector<drogon_model::fulla_db::Users> &users
          ) {
              if (users.empty())
              {
                  respondError(
                    req, sharedCb, "AUTH_INVALID_CREDENTIALS", "verifyLogin: invalid MFA session"
                  );
                  return;
              }

              int64_t nowSecs = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch()
              )
                                  .count();
              if (users[0].getValueOfLockedUntil() > nowSecs)
              {
                  respondError(
                    req, sharedCb, "AUTH_INVALID_CREDENTIALS", "verifyLogin: account is locked"
                  );
                  return;
              }

              const auto &u = users[0];
          std::string secret = u.getValueOfMfaSecret();
          std::string publicSub = u.getValueOfPublicSub();
          auto pcid = u.getMfaPendingClientId();
          std::string pendingClientId = pcid ? *pcid : "";
          auto puri = u.getMfaPendingRedirectUri();
          std::string pendingRedirectUri = puri ? *puri : "";

          if (::fulla::identity::totp::verifyCode(secret, code, mfaNowSeconds()))
          {
              // #144: legacy path mirror of the wired path's onTotpVerified
              // prologue -- the TOTP code verified, so NOW the session may be
              // elevated to amr="pwd mfa" and the mfa_pending marker cleared
              // (never before verification).
              elevateSessionAfterMfa();
              // P0-4 audit fix: shared issuance guard (client existence +
              // registered redirect_uri + scope allowlist) replaces the
              // former validateClient + validateRedirectUri pair.
              plugin->checkCodeIssuanceGuards(
                clientId,
                redirectUri,
                scope,
                [sharedCb,
                 req,
                 plugin,
                 clientId,
                 redirectUri,
                 publicSub,
                 pendingClientId,
                 pendingRedirectUri,
                 scope,
                 nonce,
                 mfaToken,
                 mfaAuthTime,
                 mfaAmr](::OAuth2Plugin::CodeIssuanceGuardResult guard) {
                    if (!guard.ok)
                    {
                        // #144 contract: same generic 401 as the wired path.
                        respondError(
                          req,
                          sharedCb,
                          "AUTH_INVALID_CREDENTIALS",
                          "verifyLogin: code issuance guard rejected the request: " + guard.detail
                        );
                        return;
                    }

                          if (clientId != pendingClientId || redirectUri != pendingRedirectUri)
                          {
                              respondError(
                                req,
                                sharedCb,
                                "AUTH_INVALID_CREDENTIALS",
                                "verifyLogin: client/redirect_uri does not match login session"
                              );
                              return;
                          }

                          plugin->generateAuthorizationCode(
                            clientId,
                            publicSub,
                            scope,
                            redirectUri,
                            "",
                            "",
                            nonce,
                            [sharedCb, req, plugin, clientId, redirectUri, publicSub, mfaToken](
                              bool success, std::string authCode, std::string genError
                            ) {
                                if (!success)
                                {
                                    respondError(
                                      req,
                                      sharedCb,
                                      "INTERNAL_ERROR",
                                      "verifyLogin: failed to generate authorization code: " +
                                        genError
                                    );
                                    return;
                                }

                                plugin->exchangeCodeForToken(
                                  authCode,
                                  clientId,
                                  "",
                                  redirectUri,
                                  "",
                                  [sharedCb, req, publicSub, mfaToken](
                                    const Json::Value &tokenResult
                                  ) {
                                      if (tokenResult.isMember("error"))
                                      {
                                          std::string detail =
                                            tokenResult.isMember("error_description")
                                              ? tokenResult["error_description"].asString()
                                              : tokenResult["error"].asString();
                                          respondError(
                                            req,
                                            sharedCb,
                                            "INTERNAL_ERROR",
                                            "verifyLogin: failed to exchange authorization code: " +
                                              detail
                                          );
                                          return;
                                      }

                                      ::fulla::drogon::adapters::DrogonAuditSink::
                                        logFromRequest(
                                          ::drogon::app()
                                            .getPlugin<::OAuth2Plugin>()
                                            ->getAuditSink(),
                                          "mfa_verified",
                                          "success",
                                          req,
                                          publicSub,
                                          "user",
                                          publicSub
                                        );

                                      Json::Value json = tokenResult;
                                      json["message"] = "MFA verification successful";
                                      json["mfa_verified"] = true;

                                      auto clearDb = ::drogon::app().getDbClient();
                                      auto sendSuccess = [sharedCb, req, json]() {
                                          auto resp =
                                            ::drogon::HttpResponse::newHttpJsonResponse(json);
                                          (*sharedCb)(resp);
                                      };
                                      if (clearDb)
                                      {
                                          // P2-2: mfaToken is the public subject —
                                          // clear the pending binding by public_sub.
                                          Mapper<drogon_model::fulla_db::Users>(clearDb).findBy(
                                            Criteria(
                                              drogon_model::fulla_db::Users::Cols::_public_sub,
                                              CompareOperator::EQ,
                                              mfaToken
                                            ),
                                            [sendSuccess, clearDb](
                                              const std::vector<drogon_model::fulla_db::Users> &u
                                            ) {
                                                if (u.empty())
                                                {
                                                    sendSuccess();
                                                    return;
                                                }
                                                drogon_model::fulla_db::Users clr = u[0];
                                                clr.setMfaPendingClientIdToNull();
                                                clr.setMfaPendingRedirectUriToNull();
                                                Mapper<drogon_model::fulla_db::Users>(clearDb)
                                                  .update(
                                                    clr,
                                                    [sendSuccess](const size_t) { sendSuccess(); },
                                                    [sendSuccess](
                                                      const ::drogon::orm::DrogonDbException &e
                                                    ) {
                                                        // Recoverable: the
                                                        // request still succeeds.
                                                        LOG_WARN
                                                          << "verifyLogin: failed to clear MFA "
                                                             "pending binding: "
                                                          << e.base().what();
                                                        sendSuccess();
                                                    }
                                                  );
                                            },
                                            [sendSuccess](
                                              const ::drogon::orm::DrogonDbException &e
                                            ) {
                                                // Recoverable: the request
                                                // still succeeds.
                                                LOG_WARN << "verifyLogin: failed to find user for "
                                                            "MFA pending clear: "
                                                         << e.base().what();
                                                sendSuccess();
                                            }
                                          );
                                      }
                                      else
                                      {
                                          sendSuccess();
                                      }
                                  }
                                );
                            },
                            mfaAuthTime,
                            mfaAmr
                          );
                    }
                );
              return;
          }

          respondError(
            req, sharedCb, "AUTH_INVALID_CREDENTIALS", "verifyLogin: TOTP code is incorrect"
          );
          },
          [sharedCb, req](const ::drogon::orm::DrogonDbException &e) {
              respondError(
                req,
                sharedCb,
                "DB_QUERY_ERROR",
                std::string("MFA login verify failed: ") + e.base().what()
              );
          }
        );
    }
    catch (...)
    {
        respondError(
          req, sharedCb, "DB_QUERY_ERROR", "verifyLogin: find Mapper construction failed"
        );
    }
}

}  // namespace fulla::drogon::controllers
