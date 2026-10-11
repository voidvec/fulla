#include <fulla/drogon/controllers/WebAuthnController.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/drogon/adapters/DrogonAuditSink.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/drogon/adapters/OpenSslCryptoProvider.h>
#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <chrono>

// Task 24 slice 5 (fulla-sdk-refactor): identity-layer services this
// controller now optionally consumes.
#include <fulla/identity/IUserRepository.h>
#include <fulla/identity/WebAuthnService.h>
#include <fulla/storage/postgres/models/WebauthnCredentials.h>

using namespace ::drogon::orm;

namespace fulla::drogon::controllers
{

namespace
{
// Emit an Application error via the unified ErrorResponder entry point so the
// body is always an Error Envelope (Requirement 7.1 / 7.3 / 7.5).
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

// #142: the register endpoints run behind OAuth2AuthFilter (Bearer token,
// no session-cookie contract — the SPA sends no credentials), so a
// registration challenge is bound to the AUTHENTICATED SUBJECT instead of
// a session. Fail closed when the service/repository are not wired
// (memory-storage mode): the endpoints must be explicitly unavailable
// rather than silently available with the old unverified contract.
void respondWebAuthnUnavailable(
  const ::drogon::HttpRequestPtr &req,
  const std::shared_ptr<std::function<void(const ::drogon::HttpResponsePtr &)>> &cb,
  const std::string &flow
)
{
    LOG_ERROR << "WebAuthn " << flow
              << ": service not configured (memory storage?) -- failing closed (#142)";
    respondError(
      req,
      cb,
      "INTERNAL_ERROR",
      "WebAuthn " + flow + ": service not configured (memory storage deployments have no "
                           "WebAuthn credentials; the endpoint is unavailable by design)"
    );
}

// #142 helpers: RP config reads (challenge/rp info split -- the subject-
// bound challenge store returns a plain string; the rp/timeout boilerplate
// stays view construction) and base64url codec through the shared adapter
// (same static-instance pattern as PasswordHasher).
std::string getRpId()
{
    auto config = ::drogon::app().getCustomConfig();
    if (config.isMember("webauthn") && config["webauthn"].isMember("rp_id"))
        return config["webauthn"]["rp_id"].asString();
    return "localhost";
}

std::string getRpName()
{
    auto config = ::drogon::app().getCustomConfig();
    if (config.isMember("webauthn") && config["webauthn"].isMember("rp_name"))
        return config["webauthn"]["rp_name"].asString();
    return "OAuth2 Server";
}

std::string b64UrlEncode(const std::string &raw)
{
    static fulla::drogon::adapters::OpenSslCryptoProvider crypto;
    return crypto.base64UrlEncode(
      reinterpret_cast<const unsigned char *>(raw.data()), raw.size());
}

std::string b64UrlDecode(const std::string &encoded)
{
    static fulla::drogon::adapters::OpenSslCryptoProvider crypto;
    auto bytes = crypto.base64UrlDecode(encoded);
    return std::string(bytes.begin(), bytes.end());
}

struct WebAuthnControllerDocs
{
    WebAuthnControllerDocs()
    {
        ::fulla::drogon::observability::openapi::EndpointInfo regBeginDocs;
        regBeginDocs.path = "/api/me/webauthn/register/begin";
        regBeginDocs.method = "POST";
        regBeginDocs.summary = "WebAuthn Register Begin";
        regBeginDocs.description =
          "Start passkey registration (#142): returns creation options with a "
          "subject-bound challenge (Bearer authentication, no session cookie), "
          "ES256-only pubKeyCredParams, userVerification=required, and "
          "excludeCredentials for credentials already registered to the user. "
          "Requires webauthn.rp_origins to be configured.";
        regBeginDocs.tags = {"WebAuthn"};
        regBeginDocs.requiresAuth = true;
        ::fulla::drogon::observability::openapi::OpenApiGenerator::addEndpoint(regBeginDocs);

        ::fulla::drogon::observability::openapi::EndpointInfo regFinishDocs;
        regFinishDocs.path = "/api/me/webauthn/register/finish";
        regFinishDocs.method = "POST";
        regFinishDocs.summary = "WebAuthn Register Finish";
        regFinishDocs.description =
          "Finish passkey registration (#142): body is the browser "
          "PublicKeyCredential {id, rawId, response:{attestationObject, "
          "clientDataJSON}, name?} (base64url). The server verifies the "
          "subject-bound challenge, clientDataJSON (type/origin/tokenBinding), "
          "the none-format attestation object, authenticator data (rpIdHash/UP/"
          "AT/credIdLen) and the ES256 COSE key before storing anything; the "
          "client-submitted public_key field of the old contract is ignored.";
        regFinishDocs.tags = {"WebAuthn"};
        regFinishDocs.requiresAuth = true;
        ::fulla::drogon::observability::openapi::OpenApiGenerator::addEndpoint(regFinishDocs);

        ::fulla::drogon::observability::openapi::EndpointInfo loginBeginDocs;
        loginBeginDocs.path = "/oauth2/webauthn/authenticate/begin";
        loginBeginDocs.method = "POST";
        loginBeginDocs.summary = "WebAuthn Authenticate Begin";
        loginBeginDocs.description =
          "Start passkey authentication (#142): returns a challenge bound to "
          "the caller's SESSION (send cookies -- credentials:'include'). "
          "userVerification=required; ES256 only. Requires webauthn.rp_origins.";
        loginBeginDocs.tags = {"WebAuthn"};
        loginBeginDocs.requiresAuth = false;
        ::fulla::drogon::observability::openapi::OpenApiGenerator::addEndpoint(loginBeginDocs);

        ::fulla::drogon::observability::openapi::EndpointInfo loginFinishDocs;
        loginFinishDocs.path = "/oauth2/webauthn/authenticate/finish";
        loginFinishDocs.method = "POST";
        loginFinishDocs.summary = "WebAuthn Authenticate Finish";
        loginFinishDocs.description =
          "Finish passkey authentication (#142): body is the browser "
          "PublicKeyCredential {id, rawId, response:{authenticatorData, "
          "clientDataJSON, signature}, userHandle?} (base64url). The session "
          "challenge is consumed unconditionally; the ES256 signature over "
          "authData || SHA256(clientDataJSON) is verified against the STORED "
          "COSE key; UV=1 is enforced; signCount regression is treated as "
          "authenticator cloning. On success a browser session is established "
          "(userId/sub/auth_time/amr) and Set-Cookie is returned. All failures "
          "answer the generic AUTH_INVALID_CREDENTIALS.";
        loginFinishDocs.tags = {"WebAuthn"};
        loginFinishDocs.requiresAuth = false;
        ::fulla::drogon::observability::openapi::OpenApiGenerator::addEndpoint(loginFinishDocs);

        ::fulla::drogon::observability::openapi::EndpointInfo credentialsDocs;
        credentialsDocs.path = "/api/me/webauthn/credentials";
        credentialsDocs.method = "GET";
        credentialsDocs.summary = "List WebAuthn Credentials";
        credentialsDocs.description = "List registered passkey credentials.";
        credentialsDocs.tags = {"WebAuthn"};
        credentialsDocs.requiresAuth = true;
        ::fulla::drogon::observability::openapi::OpenApiGenerator::addEndpoint(credentialsDocs);

        // P0-7a (#261)
        ::fulla::drogon::observability::openapi::EndpointInfo deleteCredDocs;
        deleteCredDocs.path = "/api/me/webauthn/credentials/{credentialId}";
        deleteCredDocs.method = "DELETE";
        deleteCredDocs.summary = "Delete WebAuthn Credential";
        deleteCredDocs.description =
          "Remove a registered passkey. The delete is ownership-scoped "
          "(credential must belong to the Bearer subject); a foreign or "
          "unknown credential id answers the same 404. Deleting the last "
          "credential is allowed — WebAuthn and TOTP are parallel second "
          "factors; the security page warns about dropping to a single "
          "factor.";
        deleteCredDocs.tags = {"WebAuthn"};
        deleteCredDocs.requiresAuth = true;
        ::fulla::drogon::observability::openapi::OpenApiGenerator::addEndpoint(deleteCredDocs);
    }
};

WebAuthnControllerDocs docs_;
}  // namespace

void WebAuthnController::registerBegin(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    // The attribute carries the Bearer subject (public_sub).
    std::string subject = req->getAttributes()->get<std::string>("userId");
    auto sharedCb = std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
      std::move(callback)
    );

    if (!webAuthnService_ || !userRepo_)
    {
        respondWebAuthnUnavailable(req, sharedCb, "registerBegin");
        return;
    }

    // Resolve the internal user (excludeCredentials + the spec-correct
    // user.id form need it).
    userRepo_->findByPublicSub(
      subject,
      [this, sharedCb, req, subject](std::optional<fulla::identity::UserData> user) {
          if (!user)
          {
              respondError(req, sharedCb, "AUTH_INVALID_CREDENTIALS", "registerBegin: unknown user");
              return;
          }

          auto challenge = webAuthnService_->issueRegistrationChallenge(subject);
          if (!challenge)
          {
              respondError(
                req, sharedCb, "INTERNAL_ERROR", "registerBegin: challenge generation failed"
              );
              return;
          }

          webAuthnService_->listCredentials(
            user->id,
            [sharedCb, req, user, challenge = std::move(*challenge)](
              std::vector<fulla::identity::WebAuthnCredentialSummary> creds) {
                // Spec correction (#142): user.id is the base64url of the
                // RP-scoped user handle BYTES. Registration uses the decimal
                // internal id's bytes (authenticateFinish validates echoes
                // against exactly this).
                Json::Value options;
                options["challenge"] = challenge;
                Json::Value rp;
                rp["id"] = getRpId();
                rp["name"] = getRpName();
                options["rp"] = rp;
                Json::Value userJson;
                userJson["id"] = b64UrlEncode(std::to_string(user->id));
                userJson["name"] = user->username.empty() ? user->email : user->username;
                userJson["displayName"] =
                  user->username.empty() ? user->email : user->username;
                options["user"] = userJson;
                // Advertise EXACTLY what verification enforces (#142): ES256
                // only — -257 (RS256) was advertised but never verifiable.
                Json::Value pubKeyCredParams(Json::arrayValue);
                Json::Value es256;
                es256["type"] = "public-key";
                es256["alg"] = -7;
                pubKeyCredParams.append(es256);
                options["pubKeyCredParams"] = pubKeyCredParams;
                options["timeout"] = 60000;
                Json::Value authenticatorSelection;
                // BEGIN/FINISH POLICY ALIGNMENT (#142): finish enforces
                // UV=1, so begin must REQUIRE it — "preferred" would let
                // no-UV authenticators create credentials that can never
                // authenticate. (Excludes UV-incapable authenticators from
                // the supported surface; documented.)
                authenticatorSelection["userVerification"] = "required";
                authenticatorSelection["residentKey"] = "preferred";
                options["authenticatorSelection"] = authenticatorSelection;
                Json::Value exclude(Json::arrayValue);
                for (const auto &c : creds)
                {
                    Json::Value e;
                    e["type"] = "public-key";
                    e["id"] = c.credentialId;
                    exclude.append(e);
                }
                options["excludeCredentials"] = exclude;

                Json::Value response;
                response["options"] = options;
                (*sharedCb)(::drogon::HttpResponse::newHttpJsonResponse(response));
            }
          );
      }
    );
}

void WebAuthnController::registerFinish(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    std::string subject = req->getAttributes()->get<std::string>("userId");
    auto sharedCb = std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
      std::move(callback)
    );

    auto jsonBody = req->getJsonObject();
    if (!jsonBody || !(*jsonBody).isObject() || !(*jsonBody).isMember("response") ||
        !(*jsonBody)["response"].isObject())
    {
        respondError(
          req,
          sharedCb,
          "VALIDATION_INVALID_INPUT",
          "registerFinish: body must be {id, rawId, response:{attestationObject, clientDataJSON}, name?}"
        );
        return;
    }
    const Json::Value &body = *jsonBody;
    const Json::Value &resp = body["response"];

    fulla::identity::WebAuthnService::RegistrationInput input;
    input.id = body.get("id", "").asString();
    input.rawId = body.get("rawId", "").asString();
    input.attestationObject = resp.get("attestationObject", "").asString();
    input.clientDataJSON = resp.get("clientDataJSON", "").asString();
    input.name = body.get("name", "").asString();

    if (input.id.empty() || input.rawId.empty() || input.attestationObject.empty() ||
        input.clientDataJSON.empty())
    {
        respondError(
          req,
          sharedCb,
          "VALIDATION_MISSING_REQUIRED_FIELD",
          "registerFinish: id/rawId/response.attestationObject/response.clientDataJSON are required"
        );
        return;
    }

    // The challenge echoed inside clientDataJSON (the browser returns it
    // exactly as begin issued it).
    std::string presentedChallenge;
    {
        try
        {
            std::string raw = b64UrlDecode(input.clientDataJSON);
            std::string jsonStr(raw);
            Json::Reader reader;
            Json::Value parsed;
            if (reader.parse(jsonStr, parsed) && parsed.isMember("challenge"))
                presentedChallenge = parsed.get("challenge", "").asString();
        }
        catch (...)
        {
        }
    }
    if (presentedChallenge.empty())
    {
        respondError(
          req, sharedCb, "WEBAUTHN_INVALID_ATTESTATION", "registerFinish: unreadable clientDataJSON"
        );
        return;
    }

    if (!webAuthnService_ || !userRepo_)
    {
        respondWebAuthnUnavailable(req, sharedCb, "registerFinish");
        return;
    }

    userRepo_->findByPublicSub(
      subject,
      [this, sharedCb, req, subject, input, presentedChallenge](
        std::optional<fulla::identity::UserData> user) {
          if (!user)
          {
              respondError(req, sharedCb, "AUTH_INVALID_CREDENTIALS", "registerFinish: unknown user");
              return;
          }
          webAuthnService_->finishRegistrationVerified(
            user->id,
            subject,
            presentedChallenge,
            input,
            [sharedCb, req, subject, input](const std::string &errorCode) {
                if (!errorCode.empty())
                {
                    respondError(req, sharedCb, errorCode, "registerFinish: " + errorCode);
                    return;
                }
                ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
                  ::drogon::app().getPlugin<::OAuth2Plugin>()->getAuditSink(),
                  "webauthn_registered",
                  "success",
                  req,
                  subject,
                  "credential",
                  input.rawId
                );
                Json::Value json;
                json["message"] = "Passkey registered successfully";
                json["credential_id"] = input.rawId;
                auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                resp->setStatusCode(::drogon::k201Created);
                (*sharedCb)(resp);
            }
          );
      }
    );
}

void WebAuthnController::authenticateBegin(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb = std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
      std::move(callback)
    );

    if (!webAuthnService_)
    {
        respondWebAuthnUnavailable(req, sharedCb, "authenticateBegin");
        return;
    }
    // The authentication flow is ANONYMOUS and session-carried — a session
    // is mandatory (fail closed without one; the cookie contract is
    // documented in docs/domains and the endpoint description).
    if (!req->session())
    {
        respondError(
          req, sharedCb, "INTERNAL_ERROR", "authenticateBegin: no session (cookies required)"
        );
        return;
    }

    // #142: single challenge generation — issueAuthenticationChallenge
    // mints the value used AND the session-carried TTL form (the earlier
    // double mint discarded one CSPRNG draw; PR-review m-3).
    auto sessionChallenge = webAuthnService_->issueAuthenticationChallenge();
    if (!sessionChallenge)
    {
        respondError(req, sharedCb, "INTERNAL_ERROR", "authenticateBegin: challenge generation failed");
        return;
    }
    req->session()->insert("webauthn_auth_challenge", sessionChallenge->sessionValue);

    {
        Json::Value options;
        options["challenge"] = sessionChallenge->challenge;
        options["rpId"] = getRpId();
        options["timeout"] = 60000;
        // BEGIN/FINISH POLICY ALIGNMENT (#142): finish enforces UV=1.
        options["userVerification"] = "required";
        options["allowCredentials"] = Json::Value(Json::arrayValue);
        Json::Value response;
        response["options"] = options;
        (*sharedCb)(::drogon::HttpResponse::newHttpJsonResponse(response));
    }
}

void WebAuthnController::authenticateFinish(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb = std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
      std::move(callback)
    );

    auto jsonBody = req->getJsonObject();
    if (!jsonBody || !(*jsonBody).isObject() || !(*jsonBody).isMember("response") ||
        !(*jsonBody)["response"].isObject())
    {
        respondError(
          req,
          sharedCb,
          "VALIDATION_INVALID_INPUT",
          "authenticateFinish: body must be {id, rawId, response:{authenticatorData, clientDataJSON, signature}, userHandle?}"
        );
        return;
    }
    const Json::Value &body = *jsonBody;
    const Json::Value &resp = body["response"];

    fulla::identity::WebAuthnService::AssertionInput input;
    input.id = body.get("id", "").asString();
    input.rawId = body.get("rawId", "").asString();
    input.authenticatorData = resp.get("authenticatorData", "").asString();
    input.clientDataJSON = resp.get("clientDataJSON", "").asString();
    input.signature = resp.get("signature", "").asString();
    input.userHandle = body.get("userHandle", "").asString();

    if (input.id.empty() || input.rawId.empty() || input.authenticatorData.empty() ||
        input.clientDataJSON.empty() || input.signature.empty())
    {
        respondError(
          req,
          sharedCb,
          "VALIDATION_MISSING_REQUIRED_FIELD",
          "authenticateFinish: id/rawId/response.authenticatorData/clientDataJSON/signature are required"
        );
        return;
    }

    if (!webAuthnService_ || !req->session())
    {
        respondWebAuthnUnavailable(req, sharedCb, "authenticateFinish");
        return;
    }

    // Session challenge: unconditional consumption FIRST (erased whether
    // or not anything downstream matches — no validity oracle).
    std::string sessionValue = req->session()->get<std::string>("webauthn_auth_challenge");
    req->session()->erase("webauthn_auth_challenge");

    std::string presentedChallenge;
    {
        try
        {
            std::string raw = b64UrlDecode(input.clientDataJSON);
            std::string jsonStr(raw);
            Json::Reader reader;
            Json::Value parsed;
            if (reader.parse(jsonStr, parsed) && parsed.isMember("challenge"))
                presentedChallenge = parsed.get("challenge", "").asString();
        }
        catch (...)
        {
        }
    }

    const bool challengeOk =
      !sessionValue.empty() && !presentedChallenge.empty() &&
      webAuthnService_->verifyAuthenticationChallenge(sessionValue, presentedChallenge);
    if (!challengeOk)
    {
        // Generic: indistinguishable from any other credential failure.
        respondError(req, sharedCb, "AUTH_INVALID_CREDENTIALS", "authenticateFinish: failed");
        return;
    }

    webAuthnService_->finishAuthenticationVerified(
      presentedChallenge,
      input,
      [sharedCb, req, input](std::optional<fulla::identity::WebAuthnAuthResult> result) {
          if (!result)
          {
              respondError(req, sharedCb, "AUTH_INVALID_CREDENTIALS", "authenticateFinish: failed");
              return;
          }

          // Establish the browser session, mirroring SessionController::login
          // (#55 backchannel-logout attribution needs `sub`; F-021/F-022 need
          // auth_time/amr). amr values are RFC 8176 registry entries:
          // "hwk" (proof-of-possession of a hardware-protected key) plus
          // "user" (user verification — enforced by the assertion policy);
          // the server cannot distinguish PIN vs biometrics, so no "pin".
          // erase-first everywhere: Session::insert never overwrites, so a
          // re-login on a live session must not inherit the previous
          // account's userId/sub or a stale amr (PR #157 review MAJOR 1).
          req->session()->erase("userId");
          req->session()->insert("userId", std::to_string(result->userId));
          req->session()->erase("sub");
          req->session()->insert("sub", result->publicSub);
          auto nowSecs = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch()
          )
                           .count();
          req->session()->erase("auth_time");
          req->session()->insert("auth_time", static_cast<int64_t>(nowSecs));
          req->session()->erase("amr");
          req->session()->insert("amr", std::string("hwk user"));
          req->session()->erase("mfa_pending");
          req->session()->erase("must_change_password");
          // Fresh authentication -> rotate the session identifier (fixation
          // defense, same as SessionController::login).
          req->session()->changeSessionIdToClient();

          // Audit/respond with the server-normalized credential id (the
          // stored canonical form), not the client-submitted encoding
          // (PR review Info: consistency + no client-controlled audit text).
          const std::string &auditCredId =
            result->credentialId.empty() ? input.rawId : result->credentialId;
          ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
            ::drogon::app().getPlugin<::OAuth2Plugin>()->getAuditSink(),
            "webauthn_authenticated",
            "success",
            req,
            result->publicSub,
            "credential",
            auditCredId
          );
          Json::Value json;
          json["authenticated"] = true;
          json["user_id"] = result->publicSub;
          json["sign_count"] = result->signCount;
          (*sharedCb)(::drogon::HttpResponse::newHttpJsonResponse(json));
      }
    );
}

void WebAuthnController::listCredentials(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    std::string userId = req->getAttributes()->get<std::string>("userId");
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));

    if (!webAuthnService_ || !userRepo_)
    {
        respondWebAuthnUnavailable(req, sharedCb, "listCredentials");
        return;
    }

    userRepo_->findByPublicSub(
      userId, [this, sharedCb](std::optional<fulla::identity::UserData> user) {
          if (!user)
          {
              Json::Value json;
              json["credentials"] = Json::Value(Json::arrayValue);
              json["total"] = 0;
              (*sharedCb)(::drogon::HttpResponse::newHttpJsonResponse(json));
              return;
          }
          webAuthnService_->listCredentials(
            user->id,
            [sharedCb](std::vector<fulla::identity::WebAuthnCredentialSummary> creds) {
                Json::Value json;
                Json::Value credsJson(Json::arrayValue);
                for (const auto &c : creds)
                {
                    Json::Value cred;
                    cred["credential_id"] = c.credentialId;
                    cred["name"] = c.name;
                    cred["sign_count"] = c.signCount;
                    credsJson.append(cred);
                }
                json["credentials"] = credsJson;
                json["total"] = static_cast<int>(creds.size());
                (*sharedCb)(::drogon::HttpResponse::newHttpJsonResponse(json));
            }
          );
      }
    );
}

// P0-7a (#261): remove a registered passkey. Ownership is enforced by the
// delete predicate itself (user_id = the Bearer subject's internal id AND
// credential_id = the path id), so a foreign credential and an unknown id
// both answer the same generic 404 — no enumeration oracle. Runs in the
// drogon layer against the Postgres row directly: identity's repository
// interfaces gain no methods in a minor release (api-diff gate), same
// constraint P0-3 documented for backup codes.
void WebAuthnController::deleteCredential(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &credentialId
)
{
    std::string subject = req->getAttributes()->get<std::string>("userId");
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));

    if (!webAuthnService_ || !userRepo_)
    {
        respondWebAuthnUnavailable(req, sharedCb, "deleteCredential");
        return;
    }

    userRepo_->findByPublicSub(
      subject,
      [sharedCb, req, subject, credentialId](std::optional<fulla::identity::UserData> user) {
          if (!user)
          {
              respondError(req, sharedCb, "RESOURCE_NOT_FOUND", "deleteCredential: unknown subject");
              return;
          }
          ::drogon::orm::DbClientPtr db;
          try
          {
              db = ::drogon::app().getDbClient();
          }
          catch (...)
          {
              db = nullptr;
          }
          if (!db)
          {
              respondError(req, sharedCb, "DB_QUERY_ERROR", "deleteCredential: no database client");
              return;
          }
          try
          {
              Mapper<drogon_model::fulla_db::WebauthnCredentials>(db)
                .deleteBy(
                  Criteria(
                    drogon_model::fulla_db::WebauthnCredentials::Cols::_user_id,
                    CompareOperator::EQ,
                    user->id
                  ) &&
                    Criteria(
                      drogon_model::fulla_db::WebauthnCredentials::Cols::_credential_id,
                      CompareOperator::EQ,
                      credentialId
                    ),
                  [sharedCb, req, subject, credentialId](const size_t count) {
                      if (count == 0)
                      {
                          respondError(
                            req, sharedCb, "RESOURCE_NOT_FOUND", "deleteCredential: credential not found"
                          );
                          return;
                      }
                      ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
                        ::drogon::app().getPlugin<::OAuth2Plugin>()->getAuditSink(),
                        "webauthn_credential_deleted",
                        "success",
                        req,
                        subject,
                        "credential",
                        credentialId
                      );
                      Json::Value json;
                      json["message"] = "Passkey deleted";
                      json["credential_id"] = credentialId;
                      (*sharedCb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                  },
                  [sharedCb, req](const ::drogon::orm::DrogonDbException &e) {
                      respondError(
                        req,
                        sharedCb,
                        "DB_QUERY_ERROR",
                        std::string("deleteCredential: ") + e.base().what()
                      );
                  }
                );
          }
          catch (...)
          {
              respondError(
                req, sharedCb, "DB_QUERY_ERROR", "deleteCredential: Mapper construction failed"
              );
          }
      }
    );
}

}  // namespace fulla::drogon::controllers
