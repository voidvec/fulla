#include <fulla/drogon/admin/ClientManagementService.h>

#include <fulla/storage/postgres/models/Oauth2Clients.h>
#include <fulla/storage/postgres/models/Oauth2ClientScopes.h>
#include <fulla/storage/postgres/models/Oauth2ClientOwners.h>
#include <fulla/storage/postgres/models/Organizations.h>
#include <fulla/storage/postgres/models/Users.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/drogon/validation/RuleSet.h>

#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>

// Wave-2 P0: revoke the Redis-cached client row on every successful write
// (validateClient trusts the cached secret/scope data — see header for the
// TTL-bounded best-effort semantics).
#include "../ClientCacheInvalidator.h"

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>

namespace fulla::drogon::admin
{

namespace
{
// Emit an Application error via the unified ErrorResponder entry point so the
// body is always an Error Envelope (Requirement 7.1 / 7.3 / 7.5). Verbatim
// from the pre-29b ClientAdminController's anonymous-namespace helper --
// behavior unchanged.
void respondError(
  const ::drogon::HttpRequestPtr &req,
  const ClientManagementService::ResponseCallback &cb,
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

// Lazily resolve the DbClient the way the controllers used to (global lookup).
// Kept identical so connection-error behavior is unchanged.
::drogon::orm::DbClientPtr getDbOrRespond(
  const ::drogon::HttpRequestPtr &req,
  const ClientManagementService::ResponseCallback &cb
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

// F-014: enforce the redirect_uri scheme policy (https required, loopback
// IP-literal exemption, auth.allow_http_redirect_uri override) on the
// comma-separated redirect_uris column value.
std::optional<std::string> validateRedirectUriList(const std::string &list)
{
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        if (item.empty())
            continue;
        auto err = ::fulla::drogon::validation::RuleSet::validateRedirectUri(item);
        if (err)
            return "invalid redirect_uri '" + item + "': " + *err;
    }
    return std::nullopt;
}
}  // namespace

// Bring the ORM + model names into scope for the out-of-class method
// definitions below (mirrors PostgresClientRepository.cc's same pattern). These
// must live INSIDE the fulla::drogon::admin namespace so the method bodies
// (which are in this namespace) can see them. NOTE: `drogon::` and
// `drogon_model::` MUST be globally qualified (::) here -- inside
// fulla::drogon::admin, a bare `drogon::` resolves to fulla::drogon
// first (the namespace-visibility trap documented in design.md §5.5 / Task 20).
using namespace ::drogon::orm;
using namespace ::drogon_model::fulla_db;

void ClientManagementService::listClients(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    // Mapper::findBy with an empty Criteria returns all rows (matches the
    // original `SELECT ... ORDER BY client_id`). ORDER BY client_id is applied
    // post-fetch since Mapper doesn't carry an order API here; the original
    // ORDER BY client_id was only a stable-display ordering, not a behavioral
    // contract (Admin API tests don't depend on order).
    Mapper<Oauth2Clients> mapper(db);
    mapper.findBy(
      Criteria(),
      [req, cb, db](const std::vector<Oauth2Clients> &rows) {
          // v1.4.0 open platform governance: fan out to the owners table so
          // the admin list can show who each client belongs to. Split queries
          // (no JOIN, per db-operations.md); a missing/failed owners lookup
          // degrades to owner=null rather than failing the whole listing.
          auto emitListing = [cb, rows](
            const std::map<std::string, ::drogon_model::fulla_db::Oauth2ClientOwners>
              &ownerByClient,
            const std::map<int32_t, std::string> &creatorNames,
            const std::map<int32_t, std::string> &orgNames) {
              Json::Value json;
              json["status"] = "success";
              Json::Value clients(Json::arrayValue);
              for (const auto &row : rows)
              {
                  Json::Value client;
                  client["client_id"] = row.getValueOfClientId();
                  client["client_type"] = row.getValueOfClientType();
                  client["name"] = row.getValueOfName();
                  client["redirect_uris"] = row.getValueOfRedirectUris();
                  client["allowed_grant_types"] = row.getValueOfAllowedGrantTypes();
                  // F-017: surface the declared token-endpoint auth method.
                  client["token_endpoint_auth_method"] = row.getValueOfTokenEndpointAuthMethod();
                  // B1: surface the backchannel-logout URI.
                  client["backchannel_logout_uri"] = row.getValueOfBackchannelLogoutUri();
                  auto it = ownerByClient.find(row.getValueOfClientId());
                  client["self_registered"] = (it != ownerByClient.end());
                  if (it != ownerByClient.end())
                  {
                      Json::Value owner;
                      owner["creator_user_id"] = it->second.getValueOfCreatorUserId();
                      auto nameIt = creatorNames.find(it->second.getValueOfCreatorUserId());
                      owner["creator_name"] =
                        nameIt != creatorNames.end() ? nameIt->second : "";
                      if (it->second.getOrgId() != nullptr)
                      {
                          owner["org_id"] = *it->second.getOrgId();
                          auto orgIt = orgNames.find(*it->second.getOrgId());
                          owner["org_name"] = orgIt != orgNames.end() ? orgIt->second : "";
                      }
                      owner["status"] = it->second.getValueOfStatus();
                      client["owner"] = owner;
                  }
                  else
                  {
                      client["owner"] = Json::Value();
                  }
                  clients.append(client);
              }
              json["clients"] = clients;
              json["total"] = static_cast<int>(rows.size());
              (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
          };

          try
          {
              Mapper<::drogon_model::fulla_db::Oauth2ClientOwners> ownersMapper(db);
              ownersMapper.findBy(
                Criteria(),
                [db, emitListing](
                  const std::vector<::drogon_model::fulla_db::Oauth2ClientOwners> &owners) {
                    std::map<std::string, ::drogon_model::fulla_db::Oauth2ClientOwners>
                      ownerByClient;
                    std::set<int32_t> creatorIds;
                    std::set<int32_t> orgIds;
                    for (const auto &o : owners)
                    {
                        ownerByClient[o.getValueOfClientId()] = o;
                        creatorIds.insert(o.getValueOfCreatorUserId());
                        if (o.getOrgId() != nullptr)
                            orgIds.insert(*o.getOrgId());
                    }
                    // Resolve creator display names (display_name preferred,
                    // username fallback) and org names; both fan-outs are
                    // optional for the listing (missing rows degrade to "").
                    auto finish = std::make_shared<std::function<void(
                      const std::map<int32_t, std::string> &,
                      const std::map<int32_t, std::string> &)>>();
                    *finish = [ownerByClient, emitListing](
                                 const std::map<int32_t, std::string> &creatorNames,
                                 const std::map<int32_t, std::string> &orgNames) {
                        emitListing(ownerByClient, creatorNames, orgNames);
                    };
                    auto orgDone = std::make_shared<std::function<void(
                      std::map<int32_t, std::string>)>>();
                    *orgDone = [creatorIds, finish, db](std::map<int32_t, std::string> orgNames) {
                        if (creatorIds.empty())
                        {
                            std::map<int32_t, std::string> empty;
                            (*finish)(empty, orgNames);
                            return;
                        }
                        std::vector<int32_t> ids(creatorIds.begin(), creatorIds.end());
                        try
                        {
                            Mapper<::drogon_model::fulla_db::Users> usersMapper(db);
                            usersMapper.findBy(
                              ::drogon::orm::Criteria(
                                ::drogon_model::fulla_db::Users::Cols::_id,
                                ::drogon::orm::CompareOperator::In,
                                ids),
                              [orgNames, finish](
                                const std::vector<::drogon_model::fulla_db::Users> &users) {
                                  std::map<int32_t, std::string> creatorNames;
                                  for (const auto &u : users)
                                  {
                                      std::string label = u.getValueOfDisplayName();
                                      if (label.empty())
                                          label = u.getValueOfUsername();
                                      creatorNames[u.getValueOfId()] = label;
                                  }
                                  (*finish)(creatorNames, orgNames);
                              },
                              [orgNames, finish](
                                const ::drogon::orm::DrogonDbException &) {
                                  std::map<int32_t, std::string> empty;
                                  (*finish)(empty, orgNames);
                              });
                        }
                        catch (...)
                        {
                            std::map<int32_t, std::string> empty;
                            (*finish)(empty, orgNames);
                        }
                    };
                    if (orgIds.empty())
                    {
                        std::map<int32_t, std::string> empty;
                        (*orgDone)(empty);
                        return;
                    }
                    std::vector<int32_t> oids(orgIds.begin(), orgIds.end());
                    try
                    {
                        Mapper<::drogon_model::fulla_db::Organizations> orgsMapper(db);
                        orgsMapper.findBy(
                          ::drogon::orm::Criteria(
                            ::drogon_model::fulla_db::Organizations::Cols::_id,
                            ::drogon::orm::CompareOperator::In,
                            oids),
                          [orgDone](
                            const std::vector<::drogon_model::fulla_db::Organizations>
                              &orgs) {
                              std::map<int32_t, std::string> orgNames;
                              for (const auto &org : orgs)
                                  orgNames[org.getValueOfId()] = org.getValueOfName();
                              (*orgDone)(orgNames);
                          },
                          [orgDone](const ::drogon::orm::DrogonDbException &) {
                              std::map<int32_t, std::string> empty;
                              (*orgDone)(empty);
                          });
                    }
                    catch (...)
                    {
                        std::map<int32_t, std::string> empty;
                        (*orgDone)(empty);
                    }
                },
                [emitListing](
                  const ::drogon::orm::DrogonDbException &) {
                    // Owners lookup failed (pre-V035 databases) — degrade to
                    // an admin-only listing, exactly the pre-v1.4.0 shape.
                    std::map<std::string, ::drogon_model::fulla_db::Oauth2ClientOwners>
                      empty;
                    std::map<int32_t, std::string> noNames;
                    emitListing(empty, noNames, noNames);
                });
          }
          catch (...)
          {
              std::map<std::string, ::drogon_model::fulla_db::Oauth2ClientOwners> empty;
              std::map<int32_t, std::string> noNames;
              emitListing(empty, noNames, noNames);
          }
      },
      [req, cb](const ::drogon::orm::DrogonDbException &e) {
          respondError(
            req, cb, "DB_QUERY_ERROR", std::string("Failed to fetch clients: ") + e.base().what()
          );
      }
    );
}

void ClientManagementService::createClient(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    std::string name;
    std::string redirectUris;
    std::string allowedGrantTypes = "authorization_code";
    std::string clientType = "CONFIDENTIAL";
    std::string tokenEndpointAuthMethod;
    std::string backchannelLogoutUri;

    auto jsonBody = req->getJsonObject();
    if (jsonBody)
    {
        name = jsonBody->get("name", "").asString();
        redirectUris = jsonBody->get("redirect_uris", "").asString();
        allowedGrantTypes = jsonBody->get("allowed_grant_types", "authorization_code").asString();
        clientType = jsonBody->get("client_type", "CONFIDENTIAL").asString();
        tokenEndpointAuthMethod = jsonBody->get("token_endpoint_auth_method", "").asString();
        backchannelLogoutUri = jsonBody->get("backchannel_logout_uri", "").asString();
    }
    // F-017: apply per-type defaults (PUBLIC -> none, CONFIDENTIAL ->
    // client_secret_basic) when the admin omits the field.
    if (tokenEndpointAuthMethod.empty())
    {
        tokenEndpointAuthMethod =
          (clientType == "PUBLIC") ? "none" : "client_secret_basic";
    }

    // RFC 7591 §2.2: client_name is a REQUIRED field of a registration
    // request. Reject empty/missing names with 400 rather than silently
    // creating a nameless client (which the test Test 6d correctly expects
    // and which the pre-fix code accepted with 201).
    if (name.empty())
    {
        respondError(
          req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "createClient: name is required"
        );
        return;
    }

    // F-014: reject non-compliant redirect URIs at creation time.
    if (!redirectUris.empty())
    {
        if (auto uriError = validateRedirectUriList(redirectUris))
        {
            respondError(req, cb, "VALIDATION_FORMAT_ERROR", "createClient: " + *uriError);
            return;
        }
    }

    // B1: backchannel_logout_uri must use https (OIDC Back-Channel Logout 1.0
    // §2.3). Empty is allowed == "not configured".
    if (auto bcError = ::fulla::drogon::validation::RuleSet::validateBackchannelLogoutUri(backchannelLogoutUri))
    {
        respondError(req, cb, "VALIDATION_FORMAT_ERROR", "createClient: " + *bcError);
        return;
    }

    std::string clientId = ::drogon::utils::getUuid();
    std::string clientSecret = ::fulla::drogon::utils::generateSecureToken();
    // F-002: salt FIRST, then salted hash -- validateClient computes
    // sha256(secret + salt); an unsalted stored hash never matches.
    std::string salt = ::drogon::utils::getUuid().substr(0, 36);
    std::string secretHash =
      ::fulla::drogon::utils::hashClientSecretWithSalt(clientSecret, salt);

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    Oauth2Clients row;
    row.setClientId(clientId);
    row.setClientType(clientType);
    row.setClientSecret(secretHash);
    row.setSalt(salt);
    row.setName(name);
    row.setRedirectUris(redirectUris);
    row.setAllowedGrantTypes(allowedGrantTypes);
    // F-017: persist the declared token-endpoint auth method.
    row.setTokenEndpointAuthMethod(tokenEndpointAuthMethod);
    // B1: persist the optional backchannel_logout_uri (NULL when unset).
    if (!backchannelLogoutUri.empty())
        row.setBackchannelLogoutUri(backchannelLogoutUri);

    Mapper<Oauth2Clients> mapper(db);
    mapper.insert(
      row,
      [cb, clientId, clientSecret, tokenEndpointAuthMethod](const Oauth2Clients &) {
          Json::Value json;
          json["status"] = "success";
          json["message"] = "Client created successfully";
          json["client_id"] = clientId;
          json["client_secret"] = clientSecret;  // Only returned once at creation time
          json["token_endpoint_auth_method"] = tokenEndpointAuthMethod;
          json["note"] = "Store the client_secret securely. It will not be shown again.";
          auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
          resp->setStatusCode(::drogon::k201Created);
          (*cb)(resp);
      },
      [req, cb](const ::drogon::orm::DrogonDbException &e) {
          respondError(
            req, cb, "DB_QUERY_ERROR", std::string("Failed to create client: ") + e.base().what()
          );
      }
    );
}

void ClientManagementService::getClient(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &clientId
)
{
    if (clientId.empty())
    {
        respondError(req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "clientId is required");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    Mapper<Oauth2Clients> mapper(db);
    mapper.findOne(
      Criteria(Oauth2Clients::Cols::_client_id, CompareOperator::EQ, clientId),
      [cb, req, clientId, db](const Oauth2Clients &row) {
          Json::Value json;
          json["status"] = "success";
          json["client_id"] = row.getValueOfClientId();
          json["client_type"] = row.getValueOfClientType();
          json["name"] = row.getValueOfName();
          json["redirect_uris"] = row.getValueOfRedirectUris();
          json["allowed_grant_types"] = row.getValueOfAllowedGrantTypes();
          // F-017: surface the declared token-endpoint auth method.
          json["token_endpoint_auth_method"] = row.getValueOfTokenEndpointAuthMethod();
          // B1: surface the backchannel-logout configuration.
          json["backchannel_logout_uri"] = row.getValueOfBackchannelLogoutUri();
          json["backchannel_logout_session_required"] =
            row.getValueOfBackchannelLogoutSessionRequired();

          // Fetch scopes for this client (separate query -- JOIN-in-a-single-
          // query is forbidden per db-operations.md; the original code already
          // did this as a second execSqlAsync).
          Mapper<Oauth2ClientScopes> scopeMapper(db);
          scopeMapper.findBy(
            Criteria(Oauth2ClientScopes::Cols::_client_id, CompareOperator::EQ, clientId),
            [json, cb](const std::vector<Oauth2ClientScopes> &scopeRows) mutable {
                Json::Value scopes(Json::arrayValue);
                for (const auto &scopeRow : scopeRows)
                {
                    scopes.append(scopeRow.getValueOfScopeName());
                }
                Json::Value resp = json;
                resp["scopes"] = scopes;
                (*cb)(::drogon::HttpResponse::newHttpJsonResponse(resp));
            },
            [json, cb](const ::drogon::orm::DrogonDbException &) mutable {
                // Return client info even if scope query fails (preserved behavior)
                Json::Value resp = json;
                resp["scopes"] = Json::Value(Json::arrayValue);
                (*cb)(::drogon::HttpResponse::newHttpJsonResponse(resp));
            }
          );
      },
      [req, cb](const ::drogon::orm::DrogonDbException &e) {
          // NoRowsException is a subclass of DrogonDbException -- matches the
          // original "empty result -> not found" branch.
          respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "Client not found");
          (void)e;
      }
    );
}

void ClientManagementService::updateClient(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &clientId
)
{
    if (clientId.empty())
    {
        respondError(req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "clientId is required");
        return;
    }

    auto jsonBody = req->getJsonObject();
    if (!jsonBody)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "Invalid JSON body");
        return;
    }

    // Track which fields the request wants to change (matches the original
    // dynamic SET-clause builder). Mapper::update persists the whole row, so
    // we only set the fields that are present to avoid clobbering others.
    bool hasName = jsonBody->isMember("name");
    bool hasRedirectUris = jsonBody->isMember("redirect_uris");
    bool hasGrantTypes = jsonBody->isMember("allowed_grant_types");
    bool hasBackchannelUri = jsonBody->isMember("backchannel_logout_uri");
    if (!hasName && !hasRedirectUris && !hasGrantTypes && !hasBackchannelUri)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "No fields to update");
        return;
    }

    // F-014: reject non-compliant redirect URIs at update time.
    if (hasRedirectUris)
    {
        if (auto uriError = validateRedirectUriList((*jsonBody)["redirect_uris"].asString()))
        {
            respondError(req, cb, "VALIDATION_FORMAT_ERROR", "updateClient: " + *uriError);
            return;
        }
    }

    // B1: validate backchannel_logout_uri (https per OIDC §2.3); an empty
    // value clears it.
    if (hasBackchannelUri)
    {
        const std::string bcUri = (*jsonBody)["backchannel_logout_uri"].asString();
        if (auto bcError = ::fulla::drogon::validation::RuleSet::validateBackchannelLogoutUri(bcUri))
        {
            respondError(req, cb, "VALIDATION_FORMAT_ERROR", "updateClient: " + *bcError);
            return;
        }
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    Mapper<Oauth2Clients> mapper(db);
    mapper.findOne(
      Criteria(Oauth2Clients::Cols::_client_id, CompareOperator::EQ, clientId),
      [cb, req, clientId, jsonBody, hasName, hasRedirectUris, hasGrantTypes, hasBackchannelUri, db](
        Oauth2Clients row
      ) {
          if (hasName)
          {
              row.setName((*jsonBody)["name"].asString());
          }
          if (hasRedirectUris)
          {
              row.setRedirectUris((*jsonBody)["redirect_uris"].asString());
          }
          if (hasGrantTypes)
          {
              row.setAllowedGrantTypes((*jsonBody)["allowed_grant_types"].asString());
          }
          if (hasBackchannelUri)
          {
              const std::string bcUri = (*jsonBody)["backchannel_logout_uri"].asString();
              if (bcUri.empty())
                  row.setBackchannelLogoutUriToNull();
              else
                  row.setBackchannelLogoutUri(bcUri);
          }
          Mapper<Oauth2Clients> updateMapper(db);
          updateMapper.update(
            row,
            [cb, req, clientId](const size_t) {
                fulla::drogon::ClientCacheInvalidator::instance().invalidate(clientId);
                Json::Value json;
                json["status"] = "success";
                json["message"] = "Client updated successfully";
                json["client_id"] = clientId;
                (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
            },
            [req, cb](const ::drogon::orm::DrogonDbException &e) {
                respondError(
                  req,
                  cb,
                  "DB_QUERY_ERROR",
                  std::string("Failed to update client: ") + e.base().what()
                );
            }
          );
      },
      [req, cb](const ::drogon::orm::DrogonDbException &e) {
          // Row not found -- equivalent to the original affectedRows==0 branch.
          respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "Client not found");
          (void)e;
      }
    );
}

void ClientManagementService::deleteClient(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &clientId
)
{
    if (clientId.empty())
    {
        respondError(req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "clientId is required");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    Mapper<Oauth2Clients> mapper(db);
    mapper.deleteBy(
      Criteria(Oauth2Clients::Cols::_client_id, CompareOperator::EQ, clientId),
      [cb, req, clientId](const size_t affected) {
          if (affected == 0)
          {
              respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "Client not found");
              return;
          }
          fulla::drogon::ClientCacheInvalidator::instance().invalidate(clientId);
          Json::Value json;
          json["status"] = "success";
          json["message"] = "Client deleted successfully";
          json["client_id"] = clientId;
          (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
      },
      [req, cb](const ::drogon::orm::DrogonDbException &e) {
          respondError(
            req, cb, "DB_QUERY_ERROR", std::string("Failed to delete client: ") + e.base().what()
          );
      }
    );
}

void ClientManagementService::resetClientSecret(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &clientId
)
{
    if (clientId.empty())
    {
        respondError(req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "clientId is required");
        return;
    }

    std::string newSecret = ::fulla::drogon::utils::generateSecureToken();
    // F-002: reset MUST also rotate the salt and hash with it; the old
    // implementation kept the stale salt and stored an unsalted hash.
    std::string newSalt = ::drogon::utils::getUuid().substr(0, 36);
    std::string newSecretHash =
      ::fulla::drogon::utils::hashClientSecretWithSalt(newSecret, newSalt);

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    Mapper<Oauth2Clients> mapper(db);
    mapper.findOne(
      Criteria(Oauth2Clients::Cols::_client_id, CompareOperator::EQ, clientId),
      [cb, req, clientId, newSecret, newSecretHash, newSalt, db](Oauth2Clients row) {
          row.setClientSecret(newSecretHash);
          row.setSalt(newSalt);
          Mapper<Oauth2Clients> updateMapper(db);
          updateMapper.update(
            row,
            [cb, clientId, newSecret](const size_t) {
                // Secret AND salt rotated: drop the cached client row or the
                // OLD secret stays trusted (and the new one 401s) for up to
                // the cache TTL (300s). Same contract as update/delete below.
                fulla::drogon::ClientCacheInvalidator::instance().invalidate(clientId);
                Json::Value json;
                json["status"] = "success";
                json["message"] = "Client secret reset successfully";
                json["client_id"] = clientId;
                json["client_secret"] = newSecret;
                json["note"] = "Store the new client_secret securely. It will not be shown again.";
                (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
            },
            [req, cb](const ::drogon::orm::DrogonDbException &e) {
                respondError(
                  req,
                  cb,
                  "DB_QUERY_ERROR",
                  std::string("Failed to reset client secret: ") + e.base().what()
                );
            }
          );
      },
      [req, cb](const ::drogon::orm::DrogonDbException &e) {
          respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "Client not found");
          (void)e;
      }
    );
}

void ClientManagementService::getClientScopes(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &clientId
)
{
    if (clientId.empty())
    {
        respondError(req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "clientId is required");
        return;
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    Mapper<Oauth2ClientScopes> mapper(db);
    mapper.findBy(
      Criteria(Oauth2ClientScopes::Cols::_client_id, CompareOperator::EQ, clientId),
      [cb](const std::vector<Oauth2ClientScopes> &rows) {
          Json::Value json;
          json["status"] = "success";
          Json::Value scopes(Json::arrayValue);
          for (const auto &row : rows)
          {
              scopes.append(row.getValueOfScopeName());
          }
          json["scopes"] = scopes;
          (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
      },
      [req, cb](const ::drogon::orm::DrogonDbException &e) {
          respondError(
            req,
            cb,
            "DB_QUERY_ERROR",
            std::string("Failed to fetch client scopes: ") + e.base().what()
          );
      }
    );
}

void ClientManagementService::updateClientScopes(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &clientId
)
{
    if (clientId.empty())
    {
        respondError(req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "clientId is required");
        return;
    }

    auto jsonBody = req->getJsonObject();
    if (!jsonBody || !jsonBody->isMember("scopes") || !(*jsonBody)["scopes"].isArray())
    {
        respondError(
          req, cb, "VALIDATION_MISSING_REQUIRED_FIELD", "Request body must contain a 'scopes' array"
        );
        return;
    }

    std::vector<std::string> scopes;
    for (const auto &scope : (*jsonBody)["scopes"])
    {
        if (scope.isString())
        {
            scopes.push_back(scope.asString());
        }
    }

    auto db = getDbOrRespond(req, cb);
    if (!db)
    {
        return;
    }

    // Preserve the original transactional semantics: delete existing scopes for
    // the client, then insert the new set atomically. db->newTransaction() is
    // used (Mapper operates on the transaction's DbClient connection via the
    // same Mapper<T> API -- the only raw-SQL exemption here would be none; we
    // use Mapper deleteBy + insert on the transaction connection).
    auto transaction = db->newTransaction();
    // Invalidation must fire AFTER the commit, not after the last statement:
    // a DEL issued pre-commit lets a concurrent validateClient miss read the
    // OLD allowedScopes (cached row includes them) and refill the cache with
    // the stale set for a full TTL. The commit callback fires when the
    // transaction actually commits; a rolled-back one skips the DEL (nothing
    // changed → the cached state is still correct).
    transaction->setCommitCallback([clientId](bool committed) {
        if (committed)
            fulla::drogon::ClientCacheInvalidator::instance().invalidate(clientId);
    });
    Mapper<Oauth2ClientScopes> mapper(transaction);
    mapper.deleteBy(
      Criteria(Oauth2ClientScopes::Cols::_client_id, CompareOperator::EQ, clientId),
      [cb, req, clientId, scopes, transaction](const size_t) {
          if (scopes.empty())
          {
              Json::Value json;
              json["status"] = "success";
              json["message"] = "Scopes updated";
              json["scopes"] = Json::Value(Json::arrayValue);
              (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
              return;
          }

          auto remaining = std::make_shared<std::atomic<int>>(static_cast<int>(scopes.size()));
          auto insertedScopes = std::make_shared<std::vector<std::string>>();
          auto mu = std::make_shared<std::mutex>();

          for (const auto &scopeName : scopes)
          {
              Oauth2ClientScopes scopeRow;
              scopeRow.setClientId(clientId);
              scopeRow.setScopeName(scopeName);
              Mapper<Oauth2ClientScopes> insertMapper(transaction);
              insertMapper.insert(
                scopeRow,
                [cb, clientId, scopeName, remaining, insertedScopes, mu](const Oauth2ClientScopes &) {
                    {
                        std::lock_guard<std::mutex> lock(*mu);
                        insertedScopes->push_back(scopeName);
                    }
                    if (remaining->fetch_sub(1) == 1)
                    {
                        // All inserts done. Invalidation defers to the commit
                        // callback registered above; the response goes out now
                        // (same ordering as before relative to the caller).
                        Json::Value json;
                        json["status"] = "success";
                        json["message"] = "Scopes updated";
                        Json::Value scopesJson(Json::arrayValue);
                        {
                            std::lock_guard<std::mutex> lock(*mu);
                            for (const auto &s : *insertedScopes)
                            {
                                scopesJson.append(s);
                            }
                        }
                        json["scopes"] = scopesJson;
                        (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                    }
                },
                [cb, req, remaining](const ::drogon::orm::DrogonDbException &e) {
                    if (remaining->fetch_sub(1) == 1)
                    {
                        respondError(
                          req,
                          cb,
                          "DB_QUERY_ERROR",
                          std::string("Failed to assign some scopes: ") + e.base().what()
                        );
                    }
                }
              );
          }
      },
      [req, cb](const ::drogon::orm::DrogonDbException &e) {
          respondError(
            req,
            cb,
            "DB_QUERY_ERROR",
            std::string("Failed to clear existing scopes: ") + e.base().what()
          );
      }
    );
}

}  // namespace fulla::drogon::admin
