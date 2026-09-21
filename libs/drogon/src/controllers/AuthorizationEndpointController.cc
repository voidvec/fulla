#include <fulla/drogon/controllers/AuthorizationEndpointController.h>
#include <fulla/oauth2/access/ScopeDecision.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/validation/RuleSet.h>
#include <fulla/drogon/validation/HttpResponder.h>
#include <fulla/drogon/error/OAuth2ErrorHandler.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/drogon/utils/ConsentCsrfSlots.h>
#include <fulla/drogon/utils/PortalUrl.h>
#include <fulla/storage/postgres/ClientOwnersRepository.h>
#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <sstream>

using namespace fulla::drogon::controllers;
using namespace fulla::drogon::observability::openapi;

namespace
{
// F-007 (RFC 6749 §4.1.2.1): once client_id and redirect_uri have been
// verified, every authorization-endpoint error MUST be delivered as a 302
// back to the client's redirect_uri with error/error_description/state in
// the query -- never as a direct JSON/HTML 4xx.
::drogon::HttpResponsePtr buildAuthorizeErrorRedirect(
  const std::string &redirectUri,
  const std::string &error,
  const std::string &description,
  const std::string &state
)
{
    // Review NIT #5: urlEncode `error` for defensive consistency with the
  // error_description/state params (encoded on the next lines). OAuth2 error
  // values are ASCII-safe tokens today, so this is belt-and-suspenders.
  std::string location = redirectUri + "?error=" + ::drogon::utils::urlEncode(error);
    if (!description.empty())
        location += "&error_description=" + ::drogon::utils::urlEncode(description);
    if (!state.empty())
        location += "&state=" + ::drogon::utils::urlEncode(state);
    return ::drogon::HttpResponse::newRedirectionResponse(location);
}
}  // namespace

namespace fulla::drogon::controllers
{

::OAuth2Plugin *AuthorizationEndpointController::resolvePlugin() const
{
    return plugin_ ? plugin_ : ::drogon::app().getPlugin<::OAuth2Plugin>();
}

void AuthorizationEndpointController::initApiDocs()
{
    static std::once_flag docsOnce;
    std::call_once(docsOnce, [] { initApiDocsImpl(); });
}

void AuthorizationEndpointController::initApiDocsImpl()
{
    // Authorize endpoint
    fulla::drogon::observability::openapi::EndpointInfo authorizeEndpoint;
    authorizeEndpoint.path = "/oauth2/authorize";
    authorizeEndpoint.method = "GET";
    authorizeEndpoint.summary = "Request authorization";
    authorizeEndpoint.description = "OAuth2 authorization endpoint - initiates authorization flow";
    authorizeEndpoint.tags = {"OAuth2", "Authorization"};
    authorizeEndpoint.parameters =
      {{"client_id",
        "Client identifier (required)",
        fulla::drogon::observability::openapi::ParameterType::STRING,
        fulla::drogon::observability::openapi::ParameterLocation::QUERY,
        true},
       {"redirect_uri",
        "Redirect URI (required)",
        fulla::drogon::observability::openapi::ParameterType::STRING,
        fulla::drogon::observability::openapi::ParameterLocation::QUERY,
        true},
       {"response_type",
        "Response type, must be 'code' (required)",
        fulla::drogon::observability::openapi::ParameterType::STRING,
        fulla::drogon::observability::openapi::ParameterLocation::QUERY,
        true},
       {"scope",
        "Requested scope (optional)",
        fulla::drogon::observability::openapi::ParameterType::STRING,
        fulla::drogon::observability::openapi::ParameterLocation::QUERY,
        false},
       {"state",
        "Opaque value to maintain state between request and callback "
        "(recommended)",
        fulla::drogon::observability::openapi::ParameterType::STRING,
        fulla::drogon::observability::openapi::ParameterLocation::QUERY,
        false},
       {"prompt",
        "Space-separated prompt values (none|login|consent). "
        "none forbids UI; login forces re-auth; consent forces the consent "
        "screen (OIDC Core §3.1.2.1).",
        fulla::drogon::observability::openapi::ParameterType::STRING,
        fulla::drogon::observability::openapi::ParameterLocation::QUERY,
        false},
       {"max_age",
        "Maximum allowable age of the user's authentication in seconds. If "
        "the session auth_time is older, re-authentication is forced.",
        fulla::drogon::observability::openapi::ParameterType::STRING,
        fulla::drogon::observability::openapi::ParameterLocation::QUERY,
        false}};
    authorizeEndpoint
      .responses = {{302, "Redirect to client with authorization code"}, {400, "Invalid request"}};
    authorizeEndpoint.requiresAuth = false;
    OpenApiGenerator::addEndpoint(authorizeEndpoint);
}

void AuthorizationEndpointController::authorize(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    // Use ValidatorHelper for consistent validation
    auto errors = fulla::drogon::validation::RuleSet::oauth2Authorize(req);

    // Return validation errors if any
    if (fulla::drogon::validation::HttpResponder::respondIfErrors(errors, std::move(callback)))
    {
        if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
            m->incrementCounter(
              "oauth2_requests_total",
              fulla::common::ports::MetricLabels{{"endpoint", "authorize"}},
              static_cast<double>(400)
            );
        return;
    }

    auto params = req->getParameters();
    std::string responseType = params["response_type"];
    std::string clientId = params["client_id"];
    std::string redirectUri = params["redirect_uri"];
    std::string scope = params["scope"];
    std::string state = params["state"];
    // Review finding #1 (评审问题点 P0): this endpoint used to read only the
    // five parameters above and silently dropped code_challenge/
    // code_challenge_method/nonce, so the silent re-authorization path
    // (logged-in + prior consent) issued codes with an empty challenge --
    // TokenService's PKCE check is conditional on a stored challenge
    // (RFC 7636 §4.4), so the whole PKCE line of defense was bypassed and
    // the id_token lost its nonce claim (OIDC Core §3.1.3.7). Extract and
    // thread all three through every branch below, matching the
    // SessionController login/consent issuance paths.
    std::string codeChallenge = params["code_challenge"];
    std::string codeChallengeMethod = params["code_challenge_method"];
    std::string nonce = params["nonce"];
    // F-022 (OIDC Core §3.1.2.1): prompt and max_age drive whether the
    // authorization server may reuse an existing session or must force
    // re-authentication / consent. prompt is a space-separated list of
    // none|login|consent|select_account; max_age is the max acceptable
    // age of the user's auth_time in seconds.
    std::string promptParam = params["prompt"];
    std::string maxAgeParam = params["max_age"];

    // Parse the prompt values once (space-separated).
    std::vector<std::string> promptValues;
    {
        std::stringstream pss(promptParam);
        std::string pitem;
        while (std::getline(pss, pitem, ' '))
        {
            if (!pitem.empty())
                promptValues.push_back(pitem);
        }
    }
    auto promptHas = [&promptValues](const std::string &v) {
        return std::find(promptValues.begin(), promptValues.end(), v) != promptValues.end();
    };

    // F-022 (OIDC Core §3.1.2.1): prompt=none combined with any other value
    // is a malformed request -> invalid_request direct 400 (not a redirect:
    // the request is self-contradictory before client_id/redirect_uri are
    // even validated, so this is a client error per F-007's "direct 4xx for
    // malformed-request" rule).
    if (promptHas("none") && promptValues.size() > 1)
    {
        auto resp = ::drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(::drogon::k400BadRequest);
        resp->setBody(
          "prompt=none cannot be combined with other prompt values (OIDC Core §3.1.2.1)"
        );
        callback(resp);
        return;
    }

    // P0-4: State Parameter Enforcement
    if (state.empty())
    {
        LOG_WARN
          << "Authorization request missing state parameter (CSRF vulnerability) for client: "
          << clientId;
        if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
            m->incrementCounter(
              "oauth2_requests_total",
              fulla::common::ports::MetricLabels{{"endpoint", "authorize"}},
              static_cast<double>(400)
            );
        if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
            m->incrementCounter(
              "oauth2_login_failures_total",
              fulla::common::ports::MetricLabels{{"reason", "missing_state_parameter"}}
            );

        auto resp = ::drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(::drogon::k400BadRequest);
        resp->setBody(
          "state parameter is required for CSRF protection. "
          "Please include a state parameter in your authorization request."
        );
        callback(resp);
        return;
    }

    if (state.length() < 8 || state.length() > 512)
    {
        LOG_WARN << "Authorization request has invalid state parameter length (must be 8-512 "
                    "chars) for client: "
                 << clientId << ", state length: " << state.length();
        if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
            m->incrementCounter(
              "oauth2_requests_total",
              fulla::common::ports::MetricLabels{{"endpoint", "authorize"}},
              static_cast<double>(400)
            );
        if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
            m->incrementCounter(
              "oauth2_login_failures_total",
              fulla::common::ports::MetricLabels{{"reason", "invalid_state_parameter"}}
            );

        auto resp = ::drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(::drogon::k400BadRequest);
        resp->setBody("state parameter must be between 8 and 512 characters.");
        callback(resp);
        return;
    }

    if (
      state.find('?') != std::string::npos || state.find('#') != std::string::npos ||
      state.find('&') != std::string::npos
    )
    {
        LOG_WARN << "Authorization request has potentially malicious state parameter (contains URL "
                    "delimiters) for client: "
                 << clientId << ", state: " << state.substr(0, 20) << "...";
        if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
            m->incrementCounter(
              "oauth2_requests_total",
              fulla::common::ports::MetricLabels{{"endpoint", "authorize"}},
              static_cast<double>(400)
            );
        if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
            m->incrementCounter(
              "oauth2_login_failures_total",
              fulla::common::ports::MetricLabels{{"reason", "suspicious_state_parameter"}}
            );

        auto resp = ::drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(::drogon::k400BadRequest);
        resp->setBody("state parameter contains invalid characters.");
        callback(resp);
        return;
    }

    LOG_DEBUG << "Authorization request with valid state parameter for client: " << clientId
              << ", state: " << state.substr(0, 8) << "...";

    auto plugin = resolvePlugin();
    if (!plugin)
    {
        auto resp = ::drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(::drogon::k500InternalServerError);
        resp->setBody("OAuth2 Plugin not loaded");
        callback(resp);
        return;
    }

    // F-022: collapse the parsed prompt/max_age into the booleans the
    // downstream branches act on, so the async chain only needs to thread a
    // few flags (not the raw strings).
    bool promptNone = promptHas("none");
    bool promptLogin = promptHas("login");
    bool promptConsent = promptHas("consent");
    // max_age: integer seconds. Invalid (non-numeric / negative) is treated
    // as absent to stay lenient with malformed clients (OIDC Core §3.1.2.1
    // only specifies the semantics for a valid max_age).
    int64_t maxAgeSeconds = -1;
    if (!maxAgeParam.empty())
    {
        try
        {
            maxAgeSeconds = std::stoll(maxAgeParam);
            if (maxAgeSeconds < 0)
                maxAgeSeconds = -1;
        }
        catch (const std::exception &)
        {
            maxAgeSeconds = -1;
        }
    }

    // Validate Client (Async)
    plugin->validateClient(
      clientId,
      "",
      [plugin,
       clientId,
       redirectUri,
       scope,
       state,
       responseType,
       codeChallenge,
       codeChallengeMethod,
       nonce,
       req,
       promptNone,
       promptLogin,
       promptConsent,
       maxAgeSeconds,
       callback = std::move(callback)](bool validClient) mutable {
          if (!validClient)
          {
              if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                  m->incrementCounter(
                    "oauth2_requests_total",
                    fulla::common::ports::MetricLabels{{"endpoint", "authorize"}},
                    static_cast<double>(400)
                  );
              if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                  m->incrementCounter(
                    "oauth2_login_failures_total",
                    fulla::common::ports::MetricLabels{{"reason", "invalid_client_id"}}
                  );

              auto resp = ::drogon::HttpResponse::newHttpResponse();
              resp->setStatusCode(::drogon::k400BadRequest);
              resp->setBody("Invalid client_id");
              callback(resp);
              return;
          }

          // Validate Redirect URI (Async)
          plugin->validateRedirectUri(
            clientId,
            redirectUri,
            [plugin,
             clientId,
             redirectUri,
             scope,
             state,
             responseType,
             codeChallenge,
             codeChallengeMethod,
             nonce,
             req,
             promptNone,
             promptLogin,
             promptConsent,
             maxAgeSeconds,
             callback = std::move(callback)](bool validUri) mutable {
                if (!validUri)
                {
                    auto resp = ::drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(::drogon::k400BadRequest);
                    resp->setBody("Invalid redirect_uri");
                    callback(resp);
                    return;
                }

                // F-013 (RFC 7636 §4.2): code_challenge_method, when present,
                // MUST be "plain" or "S256"; omitted defaults to "plain".
                // client_id/redirect_uri are already verified above, so per the
                // F-007 rule the error is redirected back to the client.
                if (!codeChallengeMethod.empty())
                {
                    if (codeChallengeMethod != "plain" && codeChallengeMethod != "S256")
                    {
                        callback(
                          buildAuthorizeErrorRedirect(
                            redirectUri,
                            "invalid_request",
                            "code_challenge_method must be 'plain' or 'S256'",
                            state
                          )
                        );
                        return;
                    }
                }
                else if (!codeChallenge.empty())
                {
                    codeChallengeMethod = "plain";
                }

                // P2-6(4) audit fix: reject a malformed code_challenge
                // upfront (RFC 7636 4.2: 43-128 chars of [A-Za-z0-9-._~]).
                // An over-long value would otherwise overflow the
                // VARCHAR(128) column and surface as a dead code at
                // exchange time.
                if (!codeChallenge.empty() &&
                    !fulla::drogon::utils::isValidCodeChallenge(codeChallenge))
                {
                    callback(buildAuthorizeErrorRedirect(
                      redirectUri,
                      "invalid_request",
                      "code_challenge must be 43-128 characters of [A-Za-z0-9-._~]",
                      state
                    ));
                    return;
                }

                std::vector<std::string> requestedScopes;
                std::stringstream ss(scope);
                std::string scopeItem;
                while (std::getline(ss, scopeItem, ' '))
                {
                    if (!scopeItem.empty())
                    {
                        requestedScopes.push_back(scopeItem);
                    }
                }

                // B10 / Task 45: the scope/role/consent decision is now driven
                // by the Domain-layer AuthorizationService (evaluateScopes),
                // replacing the old inline 3-tier chain
                // (validateClientScopes -> validateUserRolesForScopes ->
                // checkUserConsentAndProceed). The engine returns a single
                // ScopeValidationSummary the controller maps to a response.
                std::string userId;
                bool mfaPendingSession = false;
                bool mustChangePasswordSession = false;
                if (req->session())
                {
                    userId = req->session()->get<std::string>("userId");
                    // #144: a session paused at the MFA challenge (password
                    // verified, second factor outstanding) is NOT a usable
                    // authenticated session for authorize — treating it as
                    // logged in would mint codes with amr="pwd" for an
                    // MFA-required account. #145: same for sessions flagged
                    // must_change_password (no codes until the password is
                    // changed).
                    if (req->session()->find("mfa_pending"))
                        mfaPendingSession = req->session()->get<bool>("mfa_pending");
                    if (req->session()->find("must_change_password"))
                        mustChangePasswordSession =
                          req->session()->get<bool>("must_change_password");
                }

                if (userId.empty() || mfaPendingSession || mustChangePasswordSession)
                {
                    // F-022 (OIDC Core §3.1.2.1): prompt=none requires that
                    // no UI be shown. Without a fully usable session (none,
                    // MFA-pending, or password-change-pending) the request
                    // cannot be satisfied non-interactively -> return an
                    // error redirect with login_required (never show UI).
                    if (promptNone)
                    {
                        callback(buildAuthorizeErrorRedirect(
                          redirectUri,
                          "login_required",
                          "prompt=none requested but no fully authenticated session",
                          state
                        ));
                        return;
                    }
                    // #145: flagged accounts are routed to their portal's
                    // login page (fulla-admin-console -> admin_console.url, other
                    // clients -> frontend.url; PR #157 review MAJOR 4), which
                    // renders the inline change-password form; no authorize
                    // context is carried (no return URL -> no open redirect).
                    if (mustChangePasswordSession)
                    {
                        auto resp = ::drogon::HttpResponse::newRedirectionResponse(
                          ::fulla::drogon::utils::mustChangePasswordRedirectUrl(clientId)
                        );
                        callback(resp);
                        return;
                    }
                    // Not logged in (or MFA-pending -> re-authenticate to
                    // complete the second factor) -> redirect to the login
                    // screen (unchanged behavior; the engine's scope/consent
                    // tiers only apply to a fully authenticated subject).
                    auto customConfig = ::drogon::app().getCustomConfig();
                    std::string loginUrl = "/login";
                    if (
                      customConfig.isMember("oauth2") &&
                      customConfig["oauth2"].isMember("login_url")
                    )
                    {
                        loginUrl = customConfig["oauth2"]["login_url"].asString();
                    }
                    std::string location =
                      loginUrl + "?client_id=" + ::drogon::utils::urlEncode(clientId) +
                      "&redirect_uri=" + ::drogon::utils::urlEncode(redirectUri) +
                      "&scope=" + ::drogon::utils::urlEncode(scope) +
                      "&state=" + ::drogon::utils::urlEncode(state) +
                      "&response_type=" + ::drogon::utils::urlEncode(responseType);
                    // Review finding #1: carry the PKCE triple into the login
                    // screen so login.csp's hidden fields can hand it back to
                    // SessionController::login (which already threads it into
                    // generateAuthorizationCode).
                    if (!codeChallenge.empty())
                    {
                        location += "&code_challenge=" + ::drogon::utils::urlEncode(codeChallenge) +
                                    "&code_challenge_method=" +
                                    ::drogon::utils::urlEncode(codeChallengeMethod);
                    }
                    if (!nonce.empty())
                        location += "&nonce=" + ::drogon::utils::urlEncode(nonce);
                    auto resp = ::drogon::HttpResponse::newRedirectionResponse(location);
                    callback(resp);
                    return;
                }

                // F-022 (OIDC Core §3.1.2.1): the session exists, but
                // prompt/max_age may still force re-authentication. Read the
                // session's auth_time (set by SessionController::login /
                // MfaController::verifyLogin) to evaluate max_age, and honor
                // prompt=login by dropping the silent path entirely.
                int64_t sessAuthTime = 0;
                std::string sessAmr;
                if (req->session())
                {
                    if (req->session()->find("auth_time"))
                        sessAuthTime = req->session()->get<int64_t>("auth_time");
                    if (req->session()->find("amr"))
                        sessAmr = req->session()->get<std::string>("amr");
                }
                // max_age: if the session's auth_time is older than max_age
                // (or absent), force re-authentication.
                bool maxAgeExceeded = false;
                if (maxAgeSeconds >= 0)
                {
                    if (sessAuthTime <= 0)
                    {
                        maxAgeExceeded = true;
                    }
                    else
                    {
                        auto nowSecs = std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::system_clock::now().time_since_epoch()
                        )
                                         .count();
                        if ((static_cast<int64_t>(nowSecs) - sessAuthTime) > maxAgeSeconds)
                            maxAgeExceeded = true;
                    }
                }
                if (promptLogin || maxAgeExceeded)
                {
                    // Force re-authentication: redirect to the login screen,
                    // preserving the authorize context the same way the
                    // no-session branch does. prompt=none + login is already
                    // rejected above, so promptNone is false here.
                    auto customConfig = ::drogon::app().getCustomConfig();
                    std::string loginUrl = "/login";
                    if (
                      customConfig.isMember("oauth2") &&
                      customConfig["oauth2"].isMember("login_url")
                    )
                    {
                        loginUrl = customConfig["oauth2"]["login_url"].asString();
                    }
                    std::string location =
                      loginUrl + "?client_id=" + ::drogon::utils::urlEncode(clientId) +
                      "&redirect_uri=" + ::drogon::utils::urlEncode(redirectUri) +
                      "&scope=" + ::drogon::utils::urlEncode(scope) +
                      "&state=" + ::drogon::utils::urlEncode(state) +
                      "&response_type=" + ::drogon::utils::urlEncode(responseType);
                    if (!codeChallenge.empty())
                    {
                        location += "&code_challenge=" + ::drogon::utils::urlEncode(codeChallenge) +
                                    "&code_challenge_method=" +
                                    ::drogon::utils::urlEncode(codeChallengeMethod);
                    }
                    if (!nonce.empty())
                        location += "&nonce=" + ::drogon::utils::urlEncode(nonce);
                    auto resp = ::drogon::HttpResponse::newRedirectionResponse(location);
                    callback(resp);
                    return;
                }

                auto authService = plugin->getAuthorizationService();
                if (!authService)
                {
                    auto resp = ::drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(::drogon::k500InternalServerError);
                    resp->setBody("Authorization engine not available");
                    callback(resp);
                    return;
                }

                authService->evaluateScopes(
                  clientId,
                  userId,
                  requestedScopes,
                  [plugin,
                   req,
                   clientId,
                   userId,
                   redirectUri,
                   state,
                   scope,
                   codeChallenge,
                   codeChallengeMethod,
                   nonce,
                   promptNone,
                   promptConsent,
                   sessAuthTime,
                   sessAmr,
                   callback = std::move(callback)](
                    fulla::oauth2::access::ScopeValidationSummary summary
                  ) mutable {
                      if (summary.hasErrors())
                      {
                          // Tier 1 (scope_not_allowed_for_client) -> invalid_scope;
                          // Tier 2 (admin_role_required) -> access_denied.
                          // F-007: client_id/redirect_uri are verified, so the
                          // error goes back to the client as a 302 redirect.
                          std::string error = "invalid_scope";
                          std::string desc = summary.invalidReasons.empty()
                                               ? "scope validation failed"
                                               : summary.invalidReasons[0];
                          if (!desc.empty() && desc.find("admin_role") != std::string::npos)
                          {
                              error = "access_denied";
                          }
                          callback(buildAuthorizeErrorRedirect(redirectUri, error, desc, state));
                          return;
                      }

                      // F-022 (OIDC Core §3.1.2.1): prompt=consent forces the
                      // consent screen even if prior consent covers every scope;
                      // the OR makes the engine treat consent as required.
                      bool needsConsent = summary.needsConsent() || promptConsent;
                      if (needsConsent)
                      {
                          // F-022: prompt=none forbids any UI. If consent is
                          // required (either by the engine or prompt=consent),
                          // the request cannot be satisfied non-interactively ->
                          // consent_required error redirect.
                          if (promptNone)
                          {
                              callback(buildAuthorizeErrorRedirect(
                                redirectUri,
                                "consent_required",
                                "prompt=none requested but user interaction (consent) is required",
                                state
                              ));
                              return;
                          }
                          // At least one requested scope lacks recorded consent
                          // (or prompt=consent forced it) -> route to the consent
                          // screen (unchanged redirect shape).
                          auto customConfig = ::drogon::app().getCustomConfig();
                          std::string consentUrl = "/consent";
                          if (
                            customConfig.isMember("oauth2") &&
                            customConfig["oauth2"].isMember("consent_url")
                          )
                          {
                              consentUrl = customConfig["oauth2"]["consent_url"].asString();
                          }
                          std::string location =
                            consentUrl + "?client_id=" + ::drogon::utils::urlEncode(clientId) +
                            "&user_id=" + ::drogon::utils::urlEncode(userId) +
                            "&scope=" + ::drogon::utils::urlEncode(scope) +
                            "&redirect_uri=" + ::drogon::utils::urlEncode(redirectUri) +
                            "&state=" + ::drogon::utils::urlEncode(state);
                          // Review finding #1: the consent screen (frontend
                          // ConsentPage.vue) echoes these query params back to
                          // POST /oauth2/consent, whose handler already reads
                          // and threads them into generateAuthorizationCode.
                          if (!codeChallenge.empty())
                          {
                              location +=
                                "&code_challenge=" + ::drogon::utils::urlEncode(codeChallenge) +
                                "&code_challenge_method=" +
                                ::drogon::utils::urlEncode(codeChallengeMethod);
                          }
                          if (!nonce.empty())
                              location += "&nonce=" + ::drogon::utils::urlEncode(nonce);
                          // F1 (consent auth): mint a SERVER-side random CSRF
                          // nonce for the consent round-trip. The consent form
                          // must echo it and POST /oauth2/consent verifies it
                          // against this session (one-shot per nonce,
                          // TTL-bounded). It deliberately does NOT reuse the
                          // client-chosen `state` (client-controllable,
                          // empty-matchable).
                          // #144: the nonce is stored in a bounded multi-slot
                          // list (ConsentCsrfSlots) so concurrent authorize
                          // flows on one session no longer overwrite each
                          // other's slot (single-slot regression for
                          // multi-tab users).
                          if (req->session())
                          {
                              std::string consentCsrf =
                                ::fulla::drogon::utils::generateSecureToken();
                              ::fulla::drogon::utils::ConsentCsrfSlots::mint(
                                req->session(),
                                consentCsrf,
                                static_cast<int64_t>(
                                  ::trantor::Date::now().secondsSinceEpoch()
                                )
                              );
                              location +=
                                "&consent_csrf=" + ::drogon::utils::urlEncode(consentCsrf);
                          }
                          // v1.4.0 open platform (M3): surface WHO offers the
                          // app on the consent screen — an org app shows the
                          // org name, a personal app the creator's display
                          // name (username fallback), admin-seeded clients
                          // show nothing (official first-party apps). Lookup
                          // failure or memory-mode (no DB) degrades to no
                          // owner_name, exactly the pre-v1.4.0 consent URL.
                          auto sendConsentRedirect =
                            [callback, location](const std::string &ownerName) {
                                std::string finalLocation = location;
                                if (!ownerName.empty())
                                {
                                    finalLocation +=
                                      "&owner_name=" + ::drogon::utils::urlEncode(ownerName);
                                }
                                callback(::drogon::HttpResponse::newRedirectionResponse(
                                  finalLocation));
                            };
                          try
                          {
                              // #222 (v1.5.0 M0): the owner->label fan-out
                              // (org app -> org name, personal app ->
                              // creator display_name with username fallback,
                              // admin-seeded -> nothing) moved into the
                              // shared ClientOwnersRepository. Mechanical
                              // extraction, zero behavior change: any lookup
                              // failure or memory-mode (no DB) still degrades
                              // to no owner_name, exactly the pre-v1.4.0
                              // consent URL.
                              ::fulla::storage::postgres::ClientOwnersRepository
                                ownersRepo(::drogon::app().getDbClient());
                              ownersRepo.resolveOwnerLabel(
                                clientId,
                                [sendConsentRedirect](const std::string &label) {
                                    sendConsentRedirect(label);
                                }
                              );
                          }
                          catch (...)
                          {
                              // No DB (memory mode) — consent proceeds without
                              // owner attribution.
                              sendConsentRedirect("");
                          }
                          return;
                      }

                      // Review finding #1: mirror SessionController::login's
                      // CHECK 3 -- with auth.require_pkce_for_public enabled,
                      // the silent re-authorization path must not issue codes
                      // without a code_challenge either (previously only the
                      // login path enforced this, so a returning user's
                      // /oauth2/authorize replay bypassed the policy).
                      auto customConfig = ::drogon::app().getCustomConfig();
                      // F-011 (RFC 9700 §2.1.1): PKCE mandatory by default
                      // for all authorization_code clients.
                      bool requirePkce = true;
                      if (
                        customConfig.isMember("auth") &&
                        customConfig["auth"].isMember("require_pkce_for_public")
                      )
                      {
                          requirePkce = customConfig["auth"]["require_pkce_for_public"].asBool();
                      }
                      if (requirePkce && codeChallenge.empty())
                      {
                          LOG_WARN << "[SECURITY] client " << clientId
                                   << " re-authorization without PKCE (enforcement enabled)";
                          // F-007: PKCE-required is a post-validation error ->
                          // 302 back to the client, not a direct 400.
                          callback(
                            buildAuthorizeErrorRedirect(
                              redirectUri,
                              "invalid_request",
                              "PKCE (code_challenge) is required for public clients",
                              state
                            )
                          );
                          return;
                      }

                      // canProceed(): every scope Valid -> issue an auth code.
                      plugin->generateAuthorizationCode(
                        clientId,
                        userId,
                        scope,
                        redirectUri,
                        codeChallenge,
                        codeChallengeMethod,
                        nonce,
                        [redirectUri, state, callback = std::move(callback)](
                          bool success, std::string code, std::string error
                        ) mutable {
                            if (!success)
                            {
                                LOG_ERROR << "Failed to generate authorization code: " << error;
                                // F-007: server_error also redirects back to
                                // the client per RFC 6749 §4.1.2.1.
                                callback(
                                  buildAuthorizeErrorRedirect(
                                    redirectUri,
                                    "server_error",
                                    "Failed to generate authorization code",
                                    state
                                  )
                                );
                                return;
                            }

                            // F-020 (RFC 6749 §4.1.2/§4.1.3): urlEncode the
                            // code and state so special chars survive the query.
                            std::string location =
                              redirectUri + "?code=" + ::drogon::utils::urlEncode(code);
                            if (!state.empty())
                                location += "&state=" + ::drogon::utils::urlEncode(state);
                            auto resp = ::drogon::HttpResponse::newRedirectionResponse(location);
                            if (auto m = ::drogon::app().getPlugin<::OAuth2Plugin>()->getMetrics())
                                m->incrementCounter(
                                  "oauth2_requests_total",
                                  fulla::common::ports::MetricLabels{{"endpoint", "authorize"}},
                                  static_cast<double>(302)
                                );
                            callback(resp);
                        },
                        sessAuthTime,
                        sessAmr
                      );
                  }
                );
            }
          );
      }
    );
}

}  // namespace fulla::drogon::controllers
