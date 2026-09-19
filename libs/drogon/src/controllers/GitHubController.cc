#include <fulla/drogon/controllers/GitHubController.h>
#include <drogon/HttpClient.h>
#include <drogon/drogon.h>
#include <fulla/drogon/controllers/SocialTokenIssuer.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/drogon/error/ErrorResponder.h>
// Wave-2 P1: the default-role grant is a user_roles write — revoke the
// cached roles for the (possibly pre-probed) subject.
#include "../UserReadCache.h"

#ifdef WITH_SOCIAL
// Task 24 slice 5 (fulla-sdk-refactor): identity-layer service this
// controller now optionally consumes.
#include <fulla/identity/SocialAuthService.h>
#endif  // WITH_SOCIAL

// OAuth2 token DTOs for issueTokensForUser -> plugin->saveTokenPair (the
// storage-abstraction route; replaces the former direct Mapper<Oauth2Access/
//RefreshTokens> persistence).
#include <fulla/oauth2/model/Dto.h>

#include <fulla/storage/postgres/models/Oauth2SubjectMappings.h>
#include <fulla/storage/postgres/models/Roles.h>
#include <fulla/storage/postgres/models/UserRoles.h>
#include <fulla/storage/postgres/models/Users.h>

using namespace ::drogon::orm;
using namespace drogon_model::fulla_db;

namespace fulla::drogon::controllers
{

namespace
{

std::string getGitHubConfig(const std::string &key)
{
    auto config = ::drogon::app().getCustomConfig();
    if (config.isMember("external_auth") && config["external_auth"].isMember("github"))
    {
        return config["external_auth"]["github"].get(key, "").asString();
    }
    return "";
}

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

// Shorthand for the (very common) DB-exception path: report a DB_QUERY_ERROR
// with a "github login: <ctx>: <what>" detail string. Collapses the ~6 places
// that previously spelled this template out inline.
void respondDbError(
  const ::drogon::HttpRequestPtr &req,
  const std::shared_ptr<std::function<void(const ::drogon::HttpResponsePtr &)>> &cb,
  const char *ctx,
  const ::drogon::orm::DrogonDbException &e
)
{
    respondError(
      req, cb, "DB_QUERY_ERROR", std::string("github login: ") + ctx + ": " + e.base().what()
    );
}

// Same as above but for a generic std::exception (used by the try/catch guards
// around synchronous Mapper construction inside async callbacks, where the
// thrown type is not necessarily a DrogonDbException).
void respondDbError(
  const ::drogon::HttpRequestPtr &req,
  const std::shared_ptr<std::function<void(const ::drogon::HttpResponsePtr &)>> &cb,
  const char *ctx,
  const std::exception &e
)
{
    respondError(req, cb, "DB_QUERY_ERROR", std::string("github login: ") + ctx + ": " + e.what());
}

struct GitHubControllerDocs
{
    GitHubControllerDocs()
    {
        ::fulla::drogon::observability::openapi::EndpointInfo ep;
        ep.path = "/api/github/login";
        ep.method = "POST";
        ep.summary = "GitHub OAuth2 Login";
        ep.description = "Exchange GitHub authorization code for user information.";
        ep.tags = {"External Auth", "GitHub"};
        ep.requiresAuth = false;

        ::fulla::drogon::observability::openapi::ParameterInfo codeParam;
        codeParam.name = "code";
        codeParam.description = "Authorization code from GitHub OAuth2 callback";
        codeParam.type = ::fulla::drogon::observability::openapi::ParameterType::STRING;
        codeParam.location = ::fulla::drogon::observability::openapi::ParameterLocation::QUERY;
        codeParam.required = true;
        ep.parameters = {codeParam};

        ep.responses =
          {{200, "First-party token pair for the GitHub-linked account"},
           {400, "Invalid request (missing or invalid code)"},
           {403, "Not linked to a local account and auto-create disabled"},
           {502, "Failed to contact GitHub API"}};

        ::fulla::drogon::observability::openapi::OpenApiGenerator::addEndpoint(ep);
    }
};

GitHubControllerDocs docs_;

}  // namespace

OAuth2Plugin *GitHubController::resolvePlugin() const
{
    return plugin_ ? plugin_ : ::drogon::app().getPlugin<OAuth2Plugin>();
}

void GitHubController::login(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    if (req->method() == ::drogon::Options)
    {
        callback(::drogon::HttpResponse::newHttpResponse());
        return;
    }

    // Extract code from POST body or query
    std::string code;
    auto jsonBody = req->getJsonObject();
    // isString guard: a non-string "code" (object/array/number) is a
    // validation error, not a 500-class jsoncpp LogicError from asString().
    if (jsonBody && (*jsonBody)["code"].isString())
    {
        code = (*jsonBody)["code"].asString();
    }
    if (code.empty())
    {
        code = req->getParameter("code");
    }

    if (code.empty())
    {
        ::fulla::common::error::ErrorResponder::respond(
          req,
          std::move(callback),
          "VALIDATION_MISSING_REQUIRED_FIELD",
          "github login: missing code parameter"
        );
        return;
    }

    auto callbackPtr = CallbackPtr(std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
      std::move(callback)
    ));

#ifdef WITH_SOCIAL
    // Task 24 slice 5: prefer the injected GitHubAuthService for the
    // code-exchange + userinfo-fetch + local-account find-or-create
    // steps, falling back to the pre-Task-24 drogon::HttpClient-direct
    // path when unwired. Token issuance (issueTokensForUser below) stays
    // in this controller either way -- GitHubAuthService::login() deliberately
    // stops short of it (identity <-> oauth2 boundary, see
    // SocialAuthService.h's own scope-boundary comment). The service's own
    // credentials were validated at assembly time (#111) -- no config re-read
    // on this path.
    if (gitHubAuthService_)
    {
        gitHubAuthService_->login(
          code, [this, req, callbackPtr](fulla::identity::GitHubLoginResult result) {
              if (!result.errorCode.empty())
              {
                  respondError(
                    req, callbackPtr, result.errorCode, "github login: " + result.errorCode
                  );
                  return;
              }
              issueTokensForUser(req, callbackPtr, result.userId, result.publicSub);
          }
        );
        return;
    }
#endif  // WITH_SOCIAL

    // Fallback path: raw drogon::HttpClient + direct DB. Previously this was
    // a single ~560-line method with 7 nested callbacks; it now reads as the
    // linear step sequence below (see the step helpers' header comments).
    // #111 (fallback scope only): empty OR still the YOUR_* template -> the
    // provider is disabled; fail fast with an envelope instead of a doomed
    // upstream call.
    {
        std::string clientId = getGitHubConfig("client_id");
        std::string clientSecret = getGitHubConfig("client_secret");
        const auto credentialConfigured = [](const std::string &v) {
            return !v.empty() && v.rfind("YOUR_", 0) != 0;
        };
        if (!credentialConfigured(clientId) || !credentialConfigured(clientSecret))
        {
            ::fulla::common::error::ErrorResponder::respond(
              req, std::move(callback), "INTERNAL_ERROR", "github login: GitHub OAuth not configured"
            );
            return;
        }
        exchangeCodeForToken(req, callbackPtr, clientId, clientSecret, code);
    }
}

// ---------------------------------------------------------------------------
// Token issuance (shared by both WITH_SOCIAL and fallback paths)
// ---------------------------------------------------------------------------

void GitHubController::issueTokensForUser(
  const ::drogon::HttpRequestPtr &req,
  const CallbackPtr &callbackPtr,
  int64_t userId,
  const std::string& userPublicSub
)
{
    // #70: token issuance delegated to the shared SocialTokenIssuer (the
    // same flow as Google/WeChat). The platform subject — what token rows
    // must store (the original implementation stored
    // std::to_string(internal id), so social tokens 404'd on every
    // Bearer-authenticated handler) — is threaded in by every call site;
    // resolving it here via the DB would crash memory-storage deployments
    // (getDbClient assert). Empty fails closed inside the issuer.
    auto plugin = resolvePlugin();
    if (!plugin)
    {
        respondError(req, callbackPtr, "INTERNAL_ERROR", "github login: OAuth2Plugin not available");
        return;
    }
    ::fulla::drogon::controllers::SocialTokenIssuer::issueTokensForUser(
      req, callbackPtr, plugin, "github", userId, userPublicSub
    );
}

// ---------------------------------------------------------------------------
// Fallback path steps
// ---------------------------------------------------------------------------

void GitHubController::exchangeCodeForToken(
  const ::drogon::HttpRequestPtr &req,
  const CallbackPtr &callbackPtr,
  const std::string &clientId,
  const std::string &clientSecret,
  const std::string &code
)
{
    // Step 1: Exchange code for access token
    auto client = ::drogon::HttpClient::newHttpClient("https://github.com");
    auto request = ::drogon::HttpRequest::newHttpRequest();
    request->setMethod(::drogon::Post);
    request->setPath("/login/oauth/access_token");
    request->addHeader("Accept", "application/json");
    request->setParameter("client_id", clientId);
    request->setParameter("client_secret", clientSecret);
    request->setParameter("code", code);

    client->sendRequest(
      request,
      [this, req, callbackPtr](::drogon::ReqResult result, const ::drogon::HttpResponsePtr &response) {
          try
          {
              if (
                result != ::drogon::ReqResult::Ok || !response ||
                response->getStatusCode() != ::drogon::k200OK
              )
              {
                  respondError(
                    req, callbackPtr, "NET_CONNECTION_FAILED", "github login: failed to contact GitHub Token API"
                  );
                  return;
              }

              auto json = response->getJsonObject();
              if (!json || !json->isMember("access_token") || !(*json)["access_token"].isString())
              {
                  std::string detail = "github login: GitHub returned invalid token response";
                  if (
                    json && json->isMember("error_description") &&
                    (*json)["error_description"].isString()
                  )
                      detail += ": " + (*json)["error_description"].asString();
                  respondError(req, callbackPtr, "VALIDATION_INVALID_INPUT", detail);
                  return;
              }

              std::string accessToken = (*json)["access_token"].asString();
              fetchGitHubUserInfo(req, callbackPtr, accessToken);
          }
          catch (const std::exception &e)
          {
              LOG_ERROR << "GitHubController::login async callback exception: " << e.what();
              respondError(req, callbackPtr, "INTERNAL_ERROR", "github login: " + std::string(e.what()));
          }
          catch (...)
          {
              LOG_ERROR << "GitHubController::login async callback unknown exception";
              respondError(req, callbackPtr, "INTERNAL_ERROR", "github login: unknown error");
          }
      }
    );
}

void GitHubController::fetchGitHubUserInfo(
  const ::drogon::HttpRequestPtr &req,
  const CallbackPtr &callbackPtr,
  const std::string &accessToken
)
{
    // Step 2: Fetch user info from GitHub API
    auto apiClient = ::drogon::HttpClient::newHttpClient("https://api.github.com");
    auto userReq = ::drogon::HttpRequest::newHttpRequest();
    userReq->setPath("/user");
    userReq->addHeader("Authorization", "Bearer " + accessToken);
    userReq->addHeader("User-Agent", "fulla-server");
    userReq->addHeader("Accept", "application/json");

    apiClient->sendRequest(
      userReq,
      [this, req, callbackPtr](::drogon::ReqResult res2, const ::drogon::HttpResponsePtr &resp2) {
          try
          {
              if (
                res2 != ::drogon::ReqResult::Ok || !resp2 ||
                resp2->getStatusCode() != ::drogon::k200OK
              )
              {
                  respondError(
                    req, callbackPtr, "NET_CONNECTION_FAILED", "github login: failed to fetch GitHub user info"
                  );
                  return;
              }

              auto githubData = resp2->getJsonObject();
              // GitHub can return a non-JSON body (e.g. an HTML error page on
              // a 5xx), in which case getJsonObject() yields an empty
              // shared_ptr; dereferencing it would crash. Mirror the null
              // check already done for the token response above.
              if (!githubData)
              {
                  respondError(
                    req,
                    callbackPtr,
                    "VALIDATION_INVALID_INPUT",
                    "github login: GitHub returned non-JSON user info"
                  );
                  return;
              }
              std::string githubLogin = (*githubData).get("login", "").asString();
              std::string githubEmail = (*githubData).get("email", "").asString();
              int64_t githubId = (*githubData).get("id", 0).asInt64();

              if (githubLogin.empty())
              {
                  respondError(
                    req,
                    callbackPtr,
                    "VALIDATION_INVALID_INPUT",
                    "github login: GitHub returned no user login"
                  );
                  return;
              }

              resolveSubjectMapping(req, callbackPtr, githubLogin, githubEmail, githubId);
          }
          catch (const std::exception &e)
          {
              LOG_ERROR << "GitHubController::login inner async callback exception: " << e.what();
              respondError(req, callbackPtr, "INTERNAL_ERROR", "github login: " + std::string(e.what()));
          }
          catch (...)
          {
              LOG_ERROR << "GitHubController::login inner async callback unknown exception";
              respondError(req, callbackPtr, "INTERNAL_ERROR", "github login: unknown error");
          }
      }
    );
}

void GitHubController::resolveSubjectMapping(
  const ::drogon::HttpRequestPtr &req,
  const CallbackPtr &callbackPtr,
  const std::string &githubLogin,
  const std::string &githubEmail,
  int64_t githubId
)
{
    // Step 3: Find or create local user linked to this GitHub account
    auto db = ::drogon::app().getDbClient();
    std::string provider = "github";
    std::string subject = std::to_string(githubId);

    try
    {
        Criteria crit(
          Oauth2SubjectMappings::Cols::_provider, CompareOperator::EQ, provider
        );
        crit = crit &&
               Criteria(
                 Oauth2SubjectMappings::Cols::_subject, CompareOperator::EQ, subject
               );

        Mapper<Oauth2SubjectMappings>(db).findBy(
          crit,
          [this, req, callbackPtr, githubLogin, githubEmail, provider, subject](
            const std::vector<Oauth2SubjectMappings> &mappings
          ) {
              if (!mappings.empty())
              {
                  // Existing linked account
                  int32_t userId = mappings[0].getValueOfInternalUserId();
                  linkExistingUser(req, callbackPtr, userId);
              }
              else
              {
                  // New GitHub user - create local account + link
                  createNewLinkedUser(req, callbackPtr, githubLogin, githubEmail, provider, subject);
              }
          },
          [req, callbackPtr](const ::drogon::orm::DrogonDbException &e) {
              respondDbError(req, callbackPtr, "database error during account linking", e);
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "GitHubController::login Mapper exception: " << e.what();
        respondDbError(req, callbackPtr, "database error", e);
    }
    catch (...)
    {
        LOG_ERROR << "GitHubController::login Mapper unknown exception";
        respondError(req, callbackPtr, "DB_QUERY_ERROR", "github login: unknown database error");
    }
}

void GitHubController::linkExistingUser(
  const ::drogon::HttpRequestPtr &req,
  const CallbackPtr &callbackPtr,
  int32_t userId
)
{
    // Step 4a: existing mapping -- look up the user, then issue tokens.
    //
    // #54 (V024 soft-delete contract): the fetch filters deleted_at IS NULL
    // and rejects locked users. Previously the row's existence was ignored
    // entirely (the result was `(void)`-cast away) and tokens were issued
    // unconditionally — a soft-deleted user could log straight back in via
    // GitHub. Rejection uses the same generic AUTH_INVALID_CREDENTIALS the
    // password path serves for locked/deleted accounts (no status leak).
    auto db = ::drogon::app().getDbClient();
    // Guard: runs inside the subject-mapping findBy's async success callback;
    // the caller's try/catch cannot reach it.
    try
    {
        Mapper<Users>(db).findBy(
          Criteria(Users::Cols::_id, CompareOperator::EQ, userId) &&
            Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull),
          [this, req, callbackPtr, userId](const std::vector<Users> &users) {
              auto reject = [req, callbackPtr]() {
                  respondError(
                    req, callbackPtr, "AUTH_INVALID_CREDENTIALS",
                    "github login: linked account is not available"
                  );
              };
              if (users.empty())
              {
                  // Mapping points at a missing (hard-deleted) or
                  // soft-deleted user.
                  reject();
                  return;
              }
              // Locked users are rejected — parity with the password path
              // (AuthService.cc's lockedUntil check) so "disabled" means
              // cannot log in through ANY flow.
              int64_t nowSecs = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch()
              )
                                  .count();
              if (users[0].getValueOfLockedUntil() > nowSecs)
              {
                  reject();
                  return;
              }
              issueTokensForUser(
                req, callbackPtr, static_cast<int64_t>(userId),
                users[0].getValueOfPublicSub()
              );
          },
          [req, callbackPtr](const ::drogon::orm::DrogonDbException &e) {
              respondDbError(req, callbackPtr, "failed to fetch user", e);
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "GitHubController::linkExistingUser Mapper exception: " << e.what();
        respondError(req, callbackPtr, "DB_QUERY_ERROR", "github login: failed to fetch user");
    }
    catch (...)
    {
        LOG_ERROR << "GitHubController::linkExistingUser Mapper unknown exception";
        respondError(req, callbackPtr, "DB_QUERY_ERROR", "github login: failed to fetch user");
    }
}

void GitHubController::createNewLinkedUser(
  const ::drogon::HttpRequestPtr &req,
  const CallbackPtr &callbackPtr,
  const std::string &githubLogin,
  const std::string &githubEmail,
  const std::string &provider,
  const std::string &subject
)
{
    // Step 4b: no mapping -- create local user, then subject mapping, then
    // default role, then issue tokens.
    auto db = ::drogon::app().getDbClient();
    std::string username = "gh_" + githubLogin;
    std::string passwordHash = ::fulla::drogon::utils::generateSecureToken();
    // Exemption (db-operations.md §3): INSERT...RETURNING to capture the
    // auto-generated user id for subsequent subject-mapping and role inserts.
    //
    // #54: ON CONFLICT DO NOTHING (fail-closed) — the previous DO UPDATE
    // "adopted" whatever row already held the username (soft-deleted rows
    // got resurrected; an ACTIVE row registered by someone else meant full
    // account takeover: the upsert returned the victim's id and the flow
    // issued tokens as the victim). A conflict now returns no row and the
    // login fails.
    db->execSqlAsync(
      "INSERT INTO users (username, password_hash, salt, email, email_verified) "
      "VALUES ($1, $2, '', $3, true) "
      "ON CONFLICT (username) DO NOTHING "
      "RETURNING id, public_sub",
      [this, req, callbackPtr, db, provider, subject, username](const ::drogon::orm::Result &userResult) {
          // DO NOTHING on conflict → empty result; check BEFORE indexing
          // (Result::operator[](0) on an empty Result is UB).
          if (userResult.empty())
          {
              LOG_ERROR << "GitHubController::createNewLinkedUser: username '" << username
                        << "' conflicts with an existing row — refusing to adopt it";
              respondError(
                req, callbackPtr, "AUTH_INVALID_CREDENTIALS",
                "github login: derived username is unavailable"
              );
              return;
          }
          int32_t userId = userResult[0]["id"].as<int32_t>();
          std::string publicSub = userResult[0]["public_sub"].as<std::string>();
          // Create subject mapping.
          // Guard: runs inside the execSqlAsync success callback; caller's
          // try/catch cannot reach it.
          try
          {
              Oauth2SubjectMappings mapping;
              mapping.setSubject(subject);
              mapping.setInternalUserId(userId);
              mapping.setProvider(provider);
              Mapper<Oauth2SubjectMappings>(db).insert(
                mapping,
                [this, req, callbackPtr, db, userId, publicSub](const Oauth2SubjectMappings &) {
                    // Assign default 'user' role. Best-effort (a role-grant
                    // failure must not block login), but LOUD: this insert
                    // previously set ONLY user_id — role_id is NOT NULL with
                    // no default, so every grant failed with a swallowed
                    // constraint violation and every social account was
                    // created with ZERO roles (PR-review finding 1). Resolve
                    // the 'user' role id first, then insert both columns.
                    // ([this] is the controller-singleton exemption to the
                    // domain no-[this] rule.)
                    auto issueTokens =
                      [this, req, callbackPtr, userId, publicSub]() {
                          issueTokensForUser(
                            req, callbackPtr, static_cast<int64_t>(userId), publicSub
                          );
                      };
                    auto grantRole =
                      [db, userId, publicSub, issueTokens](int32_t roleId) {
                          UserRoles ur;
                          ur.setUserId(userId);
                          ur.setRoleId(roleId);
                          try
                          {
                              Mapper<UserRoles>(db).insert(
                                ur,
                                [issueTokens, userId, publicSub](const UserRoles &) {
                                    // Dual key form (UserReadCache contract): the
                                    // empty-roles entry a concurrent read may have
                                    // cached between user-create and this grant is
                                    // keyed by either subject form.
                                    fulla::drogon::UserCacheInvalidator::instance().invalidateUser(
                                      std::to_string(userId), publicSub);
                                    issueTokens();
                                },
                                [issueTokens](const ::drogon::orm::DrogonDbException &e) {
                                    LOG_ERROR << "GitHubController::createNewLinkedUser: default-"
                                                 "role grant failed: "
                                              << e.base().what();
                                    issueTokens();
                                }
                              );
                          }
                          catch (...)
                          {
                              LOG_ERROR << "GitHubController::createNewLinkedUser: user-roles "
                                           "Mapper construction failed";
                              issueTokens();
                          }
                      };
                    // Guard: runs inside the subject-mapping insert's async
                    // success callback; caller's try/catch cannot reach it.
                    try
                    {
                        Mapper<Roles>(db).findOne(
                          Criteria(Roles::Cols::_name, CompareOperator::EQ, std::string("user")),
                          [grantRole](const Roles &r) {
                              grantRole(r.getValueOfId());
                          },
                          [issueTokens](const ::drogon::orm::DrogonDbException &e) {
                              LOG_ERROR << "GitHubController::createNewLinkedUser: 'user' role "
                                           "lookup failed: "
                                        << e.base().what();
                              issueTokens();
                          }
                        );
                    }
                    catch (...)
                    {
                        LOG_ERROR << "GitHubController::createNewLinkedUser: roles Mapper "
                                     "construction failed";
                        issueTokens();
                    }
                },
                [req, callbackPtr](const ::drogon::orm::DrogonDbException &e) {
                    respondDbError(req, callbackPtr, "failed to link GitHub account", e);
                }
              );
          }
          catch (const std::exception &e)
          {
              LOG_ERROR << "GitHubController::createNewLinkedUser SubjectMappings Mapper exception: "
                        << e.what();
              respondDbError(req, callbackPtr, "failed to link GitHub account", e);
          }
          catch (...)
          {
              LOG_ERROR << "GitHubController::createNewLinkedUser SubjectMappings Mapper unknown exception";
              respondError(req, callbackPtr, "DB_QUERY_ERROR", "github login: failed to link GitHub account");
          }
      },
      [req, callbackPtr](const ::drogon::orm::DrogonDbException &e) {
          respondDbError(req, callbackPtr, "failed to create user account", e);
      },
      username,
      passwordHash,
      githubEmail
    );
}

}  // namespace fulla::drogon::controllers
