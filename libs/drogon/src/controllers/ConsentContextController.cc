#include <fulla/drogon/controllers/ConsentContextController.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>
#include <fulla/drogon/utils/ConsentCsrfSlots.h>
#include <fulla/drogon/utils/OrgContextSlots.h>
#include <fulla/storage/postgres/ClientOwnersRepository.h>
#include <fulla/storage/postgres/models/Oauth2Clients.h>
#include <fulla/storage/postgres/models/Oauth2Scopes.h>

#include <json/json.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// v1.4.0 M1b (#223 second half): the consent screen's owner attribution and
// org-membership banner are server-derived, not URL-trusted. Gates follow
// SessionController::consent's fail-closed chain (session -> user match ->
// CSRF nonce -> client binding) with deliberate differences for a read-only
// endpoint: the nonce is PEEKED, not consumed (the one-shot consume belongs
// to the consent POST), and the POST's mfa_pending / must_change_password
// gates (2a/2b) are absent here — display-only data, and AEC never mints a
// consent csrf for sessions paused at those states anyway (they are bounced
// to login before the consent redirect).
//
// Anti-enumeration: the (client_id, redirect_uri) pair is validated against
// the client registration (validateRedirectUri) before any owner data is
// derived, so a caller holding a live nonce for their own flow cannot probe
// arbitrary client_ids for owner labels - knowing a client's registered
// redirect_uri is exactly the knowledge today's authorize -> consent redirect
// already requires to see the consent screen at all.

namespace fulla::drogon::controllers
{

namespace
{
namespace openapi = ::fulla::drogon::observability::openapi;

// Same ErrorResponder entry point as SessionController's file-local helper
// (Requirement 7.1 / 7.3 / 7.5: the body is always an Error Envelope).
void respondError(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> cb,
  std::string code,
  std::string detailForLog = ""
)
{
    ::fulla::common::error::ErrorResponder::respond(
      req,
      [cb = std::move(cb)](const ::drogon::HttpResponsePtr &r) { cb(r); },
      std::move(code),
      std::move(detailForLog)
    );
}
}  // namespace

OAuth2Plugin *ConsentContextController::resolvePlugin() const
{
    return plugin_ ? plugin_ : ::drogon::app().getPlugin<OAuth2Plugin>();
}

// P1-1 tail of the context chain (owner label + org banner + brand
// fields); defined below context().
void finishOwnerAndOrg(
  const ::drogon::HttpRequestPtr &req,
  const std::shared_ptr<std::function<void(const ::drogon::HttpResponsePtr &)>> &sharedCb,
  const ::drogon::orm::DbClientPtr &db,
  const std::string &clientId,
  const std::string &state,
  Json::Value json
);

void ConsentContextController::initApiDocs()
{
    static std::once_flag docsOnce;
    std::call_once(docsOnce, [] { initApiDocsImpl(); });
}

void ConsentContextController::initApiDocsImpl()
{
    openapi::EndpointInfo ep;
    ep.path = "/oauth2/consent/context";
    ep.method = "GET";
    ep.summary = "Consent screen context (owner attribution + org banner)";
    ep.description =
      "Server-side consent-screen context (v1.4.0 M1b): the owner "
      "attribution label and the organization-membership banner for the "
      "consent flow whose server-minted consent_csrf nonce is presented. "
      "Session-cookie authenticated; the nonce is validated but NOT "
      "consumed (the one-shot consume stays with POST /oauth2/consent). "
      "The (client_id, redirect_uri) pair must be registered, mirroring "
      "what reaching the consent screen via authorize already requires.";
    ep.tags = {"OAuth2", "Consent"};
    ep.requiresAuth = false;
    openapi::ParameterInfo csrfParam;
    csrfParam.name = "consent_csrf";
    csrfParam.description =
      "Server-minted one-shot CSRF nonce from the authorize->consent redirect (required)";
    csrfParam.type = openapi::ParameterType::STRING;
    csrfParam.location = openapi::ParameterLocation::QUERY;
    csrfParam.required = true;
    openapi::ParameterInfo clientIdParam;
    clientIdParam.name = "client_id";
    clientIdParam.description = "The client the consent flow is for (required)";
    clientIdParam.type = openapi::ParameterType::STRING;
    clientIdParam.location = openapi::ParameterLocation::QUERY;
    clientIdParam.required = true;
    openapi::ParameterInfo redirectUriParam;
    redirectUriParam.name = "redirect_uri";
    redirectUriParam.description = "The flow's redirect_uri; must be registered for client_id (required)";
    redirectUriParam.type = openapi::ParameterType::STRING;
    redirectUriParam.location = openapi::ParameterLocation::QUERY;
    redirectUriParam.required = true;
    openapi::ParameterInfo stateParam;
    stateParam.name = "state";
    stateParam.description =
      "The flow's state value; resolves the org binding stashed at authorize "
      "time (no binding or unknown state -> org is null)";
    stateParam.type = openapi::ParameterType::STRING;
    stateParam.location = openapi::ParameterLocation::QUERY;
    stateParam.required = false;
    openapi::ParameterInfo userIdParam;
    userIdParam.name = "user_id";
    userIdParam.description = "The session user id echoed from the consent URL (required; must match the session)";
    userIdParam.type = openapi::ParameterType::STRING;
    userIdParam.location = openapi::ParameterLocation::QUERY;
    userIdParam.required = true;
    ep.parameters = {csrfParam, clientIdParam, redirectUriParam, stateParam, userIdParam};
    ep.responses = {
      {200, "Consent context (org is null for flows without an org binding; owner_name is empty for admin-seeded clients and on storage degradation)"},
      {400, "Missing/unknown/expired consent_csrf, missing parameters, or redirect_uri not registered for client_id"},
      {401, "No authenticated session"},
      {403, "user_id does not match the authenticated session"},
    };
    openapi::OpenApiGenerator::addEndpoint(ep);
}

void ConsentContextController::context(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    auto sharedCb = std::make_shared<
      std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback)
    );

    const std::string consentCsrf = req->getParameter("consent_csrf");
    const std::string clientId = req->getParameter("client_id");
    const std::string redirectUri = req->getParameter("redirect_uri");
    const std::string state = req->getParameter("state");
    const std::string userId = req->getParameter("user_id");
    // P1-1 (#279): branding + localized scope descriptions.
    const std::string scopesParam = req->getParameter("scope");
    const std::string localeParam = req->getParameter("locale");
    const bool zh = localeParam.rfind("zh", 0) == 0;

    // Gate 1 - authenticated session (consent POST Gate 1's exact rule: sub
    // AND userId must be present; an anonymous session never saw an
    // authorize, so it cannot have a live nonce either).
    std::string sessSub;
    std::string sessUserId;
    if (req->session())
    {
        if (req->session()->find("sub"))
            sessSub = req->session()->get<std::string>("sub");
        if (req->session()->find("userId"))
            sessUserId = req->session()->get<std::string>("userId");
    }
    if (sessSub.empty() || sessUserId.empty())
    {
        respondError(
          req, *sharedCb, "AUTH_SESSION_REQUIRED",
          "consent context: an authenticated session is required"
        );
        return;
    }

    // Gate 2 - the context is for the session user (consent POST Gate 2's
    // exact rule, P0-1 (#255) form: the authorize->consent redirect embeds
    // the OIDC subject (UUID public_sub), so the comparison key is
    // session["sub"]).
    if (userId.empty() || userId != sessSub)
    {
        respondError(
          req, *sharedCb, "AUTHZ_ACCESS_DENIED",
          "consent context: user_id does not match the authenticated session"
        );
        return;
    }

    // Gate 3 - the CSRF nonce must be live for THIS session (peek; the
    // consume stays with the consent POST).
    {
        const int64_t now = static_cast<int64_t>(
          ::trantor::Date::now().secondsSinceEpoch()
        );
        if (!::fulla::drogon::utils::ConsentCsrfSlots::peek(req->session(), consentCsrf, now))
        {
            respondError(
              req, *sharedCb, "VALIDATION_INVALID_INPUT",
              "consent context: missing, expired, or mismatched consent_csrf"
            );
            return;
        }
    }

    if (clientId.empty() || redirectUri.empty())
    {
        respondError(
          req, *sharedCb, "VALIDATION_INVALID_INPUT",
          "consent context: client_id and redirect_uri are required"
        );
        return;
    }

    auto plugin = resolvePlugin();
    if (!plugin)
    {
        respondError(
          req, *sharedCb, "INTERNAL_ERROR", "consent context: OAuth2 Plugin not loaded"
        );
        return;
    }

    // Gate 4 - the (client_id, redirect_uri) pair must be registered. This
    // is the anti-enumeration binding: owner data is derived only for
    // clients whose redirect_uri the caller already knows - the same
    // knowledge the authorize -> consent redirect path demands today.
    plugin->validateRedirectUri(
      clientId,
      redirectUri,
      [req, sharedCb, plugin, clientId, state, scopesParam, zh](
        bool validUri
      ) {
          if (!validUri)
          {
              respondError(
                req, *sharedCb, "VALIDATION_REDIRECT_URI_NOT_REGISTERED",
                "consent context: redirect_uri is not registered for the client"
              );
              return;
          }

          Json::Value json;
          json["owner_name"] = "";
          json["org"] = Json::Value(Json::nullValue);
          // P1-1 (#279): client display block + localized scope
          // descriptions (additive; degrade to the v1.4.0 shape on any
          // failure below).
          json["client_name"] = clientId;
          json["scope_descriptions"] = Json::Value(Json::arrayValue);

          // Memory-mode guard (the M1 Debug-assert pitfall): db_clients is
          // empty in memory deployments and getDbClient() aborts there in
          // Debug builds - never touch it when the storage is memory. Same
          // source of truth as the M1 fixes (getStorageType()).
          if (plugin->getStorageType() == "memory")
          {
              (*sharedCb)(::drogon::HttpResponse::newHttpJsonResponse(json));
              return;
          }

          ::drogon::orm::DbClientPtr db;
          try
          {
              db = ::drogon::app().getDbClient();
          }
          catch (...)
          {
              // DB unavailable - degrade to the empty context (the consent
              // screen simply shows no attribution, exactly like the
              // pre-v1.4.0 redirect without owner_name).
              (*sharedCb)(::drogon::HttpResponse::newHttpJsonResponse(json));
              return;
          }

          // Stage 1 (P1-1): client display row (name + logo_uri, V041).
          try
          {
              ::drogon::orm::Mapper<drogon_model::fulla_db::Oauth2Clients> clientMapper(db);
              clientMapper.findBy(
                ::drogon::orm::Criteria(
                  drogon_model::fulla_db::Oauth2Clients::Cols::_client_id,
                  ::drogon::orm::CompareOperator::EQ,
                  clientId) &&
                  ::drogon::orm::Criteria(
                    drogon_model::fulla_db::Oauth2Clients::Cols::_deleted_at,
                    ::drogon::orm::CompareOperator::IsNull),
                [req, sharedCb, db, clientId, state, scopesParam, zh, json](
                  const std::vector<drogon_model::fulla_db::Oauth2Clients> &rows
                ) mutable {
                    if (!rows.empty())
                    {
                        const std::string name = rows[0].getValueOfName();
                        if (!name.empty())
                            json["client_name"] = name;
                        const std::string logo = rows[0].getValueOfLogoUri();
                        if (!logo.empty())
                            json["logo_uri"] = logo;
                    }

                    // Stage 2 (P1-1): localized scope descriptions for the
                    // flow's requested scopes. Unknown scope names are
                    // simply omitted (the frontend dictionary is the
                    // missing-key fallback).
                    auto next = std::make_shared<std::function<void()>>(
                      [req, sharedCb, db, clientId, state, json]() mutable {
                          finishOwnerAndOrg(req, sharedCb, db, clientId, state, json);
                      });
                    if (scopesParam.empty())
                    {
                        (*next)();
                        return;
                    }
                    std::vector<std::string> names;
                    std::string current;
                    auto flush = [&names, &current]() {
                        if (!current.empty())
                        {
                            names.push_back(current);
                            current.clear();
                        }
                    };
                    for (char c : scopesParam)
                    {
                        if (c == ' ' || c == ',' || c == '+')
                            flush();
                        else
                            current.push_back(c);
                    }
                    flush();
                    if (names.empty())
                    {
                        (*next)();
                        return;
                    }
                    try
                    {
                        ::drogon::orm::Mapper<drogon_model::fulla_db::Oauth2Scopes> scopeMapper(db);
                        scopeMapper.findBy(
                          ::drogon::orm::Criteria(
                            drogon_model::fulla_db::Oauth2Scopes::Cols::_name,
                            ::drogon::orm::CompareOperator::In,
                            names),
                          [next, zh, json](const std::vector<drogon_model::fulla_db::Oauth2Scopes> &scopeRows
                          ) mutable {
                              Json::Value arr(Json::arrayValue);
                              for (const auto &row : scopeRows)
                              {
                                  Json::Value d;
                                  d["scope"] = row.getValueOfName();
                                  // Locale dimension (V041 description_zh);
                                  // a NULL zh column falls back to the en
                                  // description server-side.
                                  if (zh && row.getDescriptionZh() != nullptr)
                                      d["description_i18n"] = row.getValueOfDescriptionZh();
                                  else
                                      d["description_i18n"] = row.getValueOfDescription();
                                  arr.append(d);
                              }
                              json["scope_descriptions"] = arr;
                              (*next)();
                          },
                          [next](const ::drogon::orm::DrogonDbException &) {
                              // Scope descriptions are display-only: degrade
                              // to the empty array (frontend fallback).
                              (*next)();
                          });
                    }
                    catch (...)
                    {
                        (*next)();
                    }
                },
                [req, sharedCb, db, clientId, state, json](
                  const ::drogon::orm::DrogonDbException &) mutable {
                    // Client display lookup failed: continue with the
                    // id-as-name degrade shape.
                    finishOwnerAndOrg(req, sharedCb, db, clientId, state, json);
                });
          }
          catch (...)
          {
              finishOwnerAndOrg(req, sharedCb, db, clientId, state, json);
          }
      }
    );
}

// Shared tail of the consent-context chain: owner attribution label (P0-6
// semantics) + org banner (v1.4.0 M1b), now with the P1-1 org brand fields
// (logo_uri / primary_color). Every failure degrades to the pre-P1-1
// shape, never to an error.
void finishOwnerAndOrg(
  const ::drogon::HttpRequestPtr &req,
  const std::shared_ptr<std::function<void(const ::drogon::HttpResponsePtr &)>> &sharedCb,
  const ::drogon::orm::DbClientPtr &db,
  const std::string &clientId,
  const std::string &state,
  Json::Value json
)
{
    // Owner label (org name / creator display name / "" for
    // admin-seeded clients; every failure degrades to "").
    ::fulla::storage::postgres::ClientOwnersRepository ownersRepo(db);
    ownersRepo.resolveOwnerLabel(
      clientId,
      [req, sharedCb, db, state, json](
        const std::string &label
      ) mutable {
          json["owner_name"] = label;

          // Org banner: the binding stashed at authorize time (peek -
          // the issuance-path consume stays untouched). Unknown/expired
          // state or no binding -> org stays null.
          const int64_t now = static_cast<int64_t>(
            ::trantor::Date::now().secondsSinceEpoch()
          );
          auto orgId = ::fulla::drogon::utils::OrgContextSlots::peek(
            req->session(), state, now
          );
          if (!orgId.has_value())
          {
              (*sharedCb)(::drogon::HttpResponse::newHttpJsonResponse(json));
              return;
          }

          // Org name by id (the slot's orgId is server-stashed, never
          // client-supplied). NoRow (org deleted mid-flight) and Error
          // both degrade to org=null - never a wrong-org banner.
          try
          {
              ::fulla::storage::postgres::ClientOwnersRepository repo(db);
              repo.findOrganization(
                std::to_string(*orgId),
                [sharedCb, orgId = *orgId, json](
                  const ::fulla::storage::postgres::OrganizationLookup &lookup
                ) mutable {
                    if (lookup.status ==
                        ::fulla::storage::postgres::LookupStatus::Found)
                    {
                        Json::Value org;
                        org["org_id"] = orgId;
                        org["org_name"] = lookup.row.getValueOfName();
                        // P1-1 (#279): org brand fields (display-only;
                        // absent when unset).
                        const std::string orgLogo = lookup.row.getValueOfLogoUri();
                        if (!orgLogo.empty())
                            org["logo_uri"] = orgLogo;
                        const std::string orgColor = lookup.row.getValueOfPrimaryColor();
                        if (!orgColor.empty())
                            org["primary_color"] = orgColor;
                        json["org"] = org;
                    }
                    (*sharedCb)(
                      ::drogon::HttpResponse::newHttpJsonResponse(json)
                    );
                }
              );
          }
          catch (...)
          {
              (*sharedCb)(::drogon::HttpResponse::newHttpJsonResponse(json));
          }
      }
    );
}

}  // namespace fulla::drogon::controllers
