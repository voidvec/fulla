#include "ApplicationService.h"
#include "OpenPlatformConfig.h"

#include <fulla/drogon/adapters/DrogonAuditSink.h>
#include <fulla/drogon/utils/ClientCacheInvalidator.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/drogon/validation/RuleSet.h>
#include <fulla/storage/postgres/AdvisoryLock.h>
#include <fulla/storage/postgres/ClientOwnersRepository.h>
#include <fulla/storage/postgres/models/Oauth2ClientOwners.h>
#include <fulla/storage/postgres/models/Oauth2ClientScopes.h>
#include <fulla/storage/postgres/models/Oauth2Clients.h>
#include <fulla/storage/postgres/models/Oauth2Scopes.h>
#include <fulla/storage/postgres/models/OrganizationMembers.h>
#include <fulla/storage/postgres/models/Organizations.h>
#include <fulla/storage/postgres/models/Users.h>

#include <drogon/drogon.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <sstream>

// v1.4.0 open platform service (see ApplicationService.h). All DB access is
// Mapper + Criteria (db-operations.md); no JOINs — fan-out queries; every
// Mapper<...> constructor guarded; every failure reaches (*sharedCb); all
// lambda captures by value (no [this]/[&]; static service methods).

namespace openplatform
{
using ResponseCallback = ApplicationService::ResponseCallback;

namespace
{
using namespace ::drogon::orm;
using ClientModel = ::drogon_model::fulla_db::Oauth2Clients;
using OwnerModel = ::drogon_model::fulla_db::Oauth2ClientOwners;
using ScopeModel = ::drogon_model::fulla_db::Oauth2Scopes;
using ClientScopeModel = ::drogon_model::fulla_db::Oauth2ClientScopes;
using OrgModel = ::drogon_model::fulla_db::Organizations;
using MemberModel = ::drogon_model::fulla_db::OrganizationMembers;
using UserModel = ::drogon_model::fulla_db::Users;

// Shared client-ownership reads (#222, v1.5.0 M0).
using ::fulla::storage::postgres::ClientOwnersRepository;
using ::fulla::storage::postgres::LookupStatus;
using ::fulla::storage::postgres::MembershipLookup;
using ::fulla::storage::postgres::OwnerRowLookup;
using ::fulla::storage::postgres::withAdvisoryXactLock;

// The grant types a self-registered app may declare (design §4.2); the
// password grant does not exist in fulla and is never addable here.
const std::set<std::string> kAllowedGrantTypes = {
    "authorization_code", "refresh_token", "client_credentials", "device_code"
};
// PUBLIC clients are browser/SPA apps: they cannot hold secrets, so the
// client_credentials grant (CONFIDENTIAL-only by RFC 6749 §4.4) is excluded.
const std::set<std::string> kPublicClientGrantTypes = {
    "authorization_code", "refresh_token", "device_code"
};

void respondError(
  const ::drogon::HttpRequestPtr &req,
  const ResponseCallback &cb,
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
  const ::drogon::HttpRequestPtr &req, const ResponseCallback &cb
)
{
    try
    {
        return ::drogon::app().getDbClient();
    }
    catch (...)
    {
        respondError(req, cb, "DB_CONNECTION_ERROR", "applications: database unavailable");
        return nullptr;
    }
}

struct ResolvedUser
{
    int32_t id;
    std::string email;
    std::string username;
    std::string displayName;
};

using ResolvedCallback = std::function<void(bool found, const ResolvedUser &user)>;

// Dual-key caller resolution (public_sub | internal id as string), V024
// soft-delete enforced — the canonical /api/me pattern.
void resolveCaller(
  const DbClientPtr &db,
  const std::string &userIdAttr,
  const ::drogon::HttpRequestPtr &req,
  const ResponseCallback &cb,
  ResolvedCallback &&onResolved
)
{
    bool isNumeric = false;
    int32_t numericId = 0;
    try
    {
        size_t pos = 0;
        int parsed = std::stoi(userIdAttr, &pos);
        isNumeric = (pos == userIdAttr.length());
        if (isNumeric)
            numericId = parsed;
    }
    catch (...)
    {
        isNumeric = false;
    }

    try
    {
        Mapper<UserModel> mapper(db);
        mapper.findBy(
          (isNumeric ? Criteria(UserModel::Cols::_id, CompareOperator::EQ, numericId)
                     : Criteria(UserModel::Cols::_public_sub, CompareOperator::EQ, userIdAttr)) &&
            Criteria(UserModel::Cols::_deleted_at, CompareOperator::IsNull),
          [cb, req, onResolved = std::move(onResolved)](const std::vector<UserModel> &users) {
              if (users.empty())
              {
                  respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "user not found");
                  return;
              }
              ResolvedUser u;
              u.id = users[0].getValueOfId();
              u.email = users[0].getValueOfEmail();
              u.username = users[0].getValueOfUsername();
              u.displayName = users[0].getValueOfDisplayName();
              onResolved(true, u);
          },
          [req, cb](const DrogonDbException &e) {
              respondError(
                req, cb, "DB_QUERY_ERROR", std::string("user lookup failed: ") + e.base().what()
              );
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "user lookup: Mapper construction failed");
    }
}

bool isManagerRole(const std::string &role)
{
    return role == "owner" || role == "admin";
}

void audit(
  const ::drogon::HttpRequestPtr &req, const char *action, const std::string &targetId
)
{
    auto *plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
    if (plugin)
    {
        ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
          plugin->getAuditSink(), action, "success", req, "", "client", targetId
        );
    }
}

using OwnerCallback = std::function<void(bool exists, const OwnerModel &owner)>;

void loadOwnerRow(
  const DbClientPtr &db,
  const std::string &clientId,
  const ::drogon::HttpRequestPtr &req,
  const ResponseCallback &cb,
  OwnerCallback &&onLoaded
)
{
    // #222/#230 (v1.5.0 M0): the raw owners-row query lives in the shared
    // ClientOwnersRepository. NoRow keeps the uniform-404 shape (#227), but
    // a REAL query failure now surfaces as 500 DB_QUERY_ERROR instead of
    // masquerading as "no such application" (#230: the old path swallowed
    // every DrogonDbException as no-owner-row, without even a log).
    ClientOwnersRepository ownersRepo(db);
    ownersRepo.findOwnerRow(
      clientId,
      [req, cb, onLoaded = std::move(onLoaded)](const OwnerRowLookup &lookup) {
          if (lookup.status == LookupStatus::Error)
          {
              respondError(
                req, cb, "DB_QUERY_ERROR", "owner lookup failed: " + lookup.error
              );
              return;
          }
          if (lookup.status == LookupStatus::NoRow)
          {
              // No owners row = admin-seeded client: exists as a client,
              // but is not user-manageable.
              OwnerModel none;
              onLoaded(false, none);
              return;
          }
          onLoaded(true, lookup.row);
      });
}

// Management permission (design §2.3): personal app (org_id NULL) -> creator;
// org app -> that org's owner/admin members. Continuation receives ok=false
// with the error response already sent.
//
// #223 anti-enumeration: every "the caller may not manage this app" outcome
// (admin-seeded client, someone else's personal app, org app without the
// manager role) responds with the SAME 404 shape as a non-existent client —
// same code, same status — so probes cannot distinguish client provenance
// by error shape (the response message derives from the catalog per code,
// making the bodies identical by construction). Only callers who ARE
// authorized managers can observe state-level 403s (suspension).
void requireManagePermission(
  const DbClientPtr &db,
  const std::string &clientId,
  const ResolvedUser &caller,
  const ::drogon::HttpRequestPtr &req,
  const ResponseCallback &cb,
  std::function<void(bool ok, const OwnerModel &owner)> &&continuation,
  bool blockWhenSuspended = true
)
{
    loadOwnerRow(db, clientId, req, cb,
      [req, cb, db, clientId, caller, continuation = std::move(continuation), blockWhenSuspended](
        bool exists, const OwnerModel &owner) {
          if (!exists)
          {
              // Admin-seeded client (no owners row) or non-existent client:
              // one uniform 404 (#223 — no provenance text).
              respondError(
                req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "application not found"
              );
              return;
          }
          if (owner.getOrgId() == nullptr)
          {
              // Review C1: a personal app is manageable ONLY by its creator.
              // #223: a non-creator gets the uniform 404 (indistinguishable
              // from a non-existent client); the response must still be sent
              // here — the continuation contract is "ok == true means
              // continue; ok == false means the response has already been
              // sent" (denying without responding would hang the request).
              const bool ok = owner.getValueOfCreatorUserId() == caller.id;
              if (!ok)
              {
                  respondError(
                    req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "application not found"
                  );
                  return;
              }
              // Review M5: a suspended app is frozen for management writes
              // (delete stays open as the owner's exit path).
              if (blockWhenSuspended && owner.getValueOfStatus() == "suspended")
              {
                  respondError(
                    req, cb, "AUTHZ_ACCESS_DENIED",
                    "application is suspended; contact the administrator"
                  );
                  return;
              }
              continuation(true, owner);
              return;
          }
          // Org app: caller must CURRENTLY hold owner/admin in that org (a
          // creator who left the org has no residual rights — ratified).
          const int32_t orgId = *owner.getOrgId();
          // #222 (v1.5.0 M0): membership read sunk into the shared
          // repository; the manager-role POLICY stays here.
          ClientOwnersRepository ownersRepo(db);
          ownersRepo.findMembership(
            orgId,
            caller.id,
            [req, cb, continuation = std::move(continuation), owner, blockWhenSuspended](
              const MembershipLookup &lookup) {
                if (lookup.status == LookupStatus::Error)
                {
                    respondError(
                      req, cb, "DB_QUERY_ERROR", "membership lookup failed: " + lookup.error
                    );
                    return;
                }
                if (lookup.status == LookupStatus::NoRow ||
                    !isManagerRole(lookup.row.getValueOfRole()))
                {
                    // #223: non-members and non-manager members get the
                    // uniform 404 — identical to probing an admin-seeded
                    // or non-existent client. (Plain members also do not
                    // see org apps in the list endpoint, so 404 is
                    // coherent UX, not just hardening.)
                    respondError(
                      req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "application not found"
                    );
                    return;
                }
                if (blockWhenSuspended && owner.getValueOfStatus() == "suspended")
                {
                    respondError(
                      req, cb, "AUTHZ_ACCESS_DENIED",
                      "application is suspended; contact the administrator"
                    );
                    return;
                }
                continuation(true, owner);
            });
      });
}

// Redirect URIs: JSON array -> validated comma-separated storage string.
// Absent key -> nullopt (untouched).
std::optional<std::string> validateAndJoinRedirectUris(
  const Json::Value &body, int maxUris, std::string &joined
)
{
    if (!body.isMember("redirect_uris"))
        return std::nullopt;
    if (!body["redirect_uris"].isArray())
        return std::string("redirect_uris must be an array of strings");
    const Json::Value &arr = body["redirect_uris"];
    if (static_cast<int>(arr.size()) > maxUris)
        return std::string("at most " + std::to_string(maxUris) + " redirect URIs allowed");
    std::set<std::string> seen;
    for (const auto &uri : arr)
    {
        if (!uri.isString())
            return std::string("redirect_uris must be an array of strings");
        const std::string value = uri.asString();
        if (value.empty())
            continue;
        if (seen.count(value) != 0)
            return std::string("duplicate redirect_uri '" + value + "'");
        seen.insert(value);
        if (auto err = ::fulla::drogon::validation::RuleSet::validateRedirectUri(value))
            return std::string("invalid redirect_uri '" + value + "': " + *err);
    }
    bool first = true;
    for (const auto &value : seen)
    {
        if (!first)
            joined += ",";
        joined += value;
        first = false;
    }
    return std::nullopt;
}

// Grant types: JSON array -> validated comma-separated storage string.
std::optional<std::string> validateAndJoinGrantTypes(
  const Json::Value &body, const std::string &clientType, std::string &joined
)
{
    if (!body.isMember("allowed_grant_types"))
        return std::nullopt;
    if (!body["allowed_grant_types"].isArray())
        return std::string("allowed_grant_types must be an array of strings");
    const Json::Value &arr = body["allowed_grant_types"];
    if (arr.empty())
        return std::string("at least one grant type is required");
    std::set<std::string> seen;
    for (const auto &gt : arr)
    {
        if (!gt.isString())
            return std::string("allowed_grant_types must be an array of strings");
        const std::string value = gt.asString();
        if (kAllowedGrantTypes.count(value) == 0)
            return std::string(
              "grant type '" + value + "' is not available to self-registered applications"
            );
        if (clientType == "PUBLIC" && kPublicClientGrantTypes.count(value) == 0)
            return std::string("grant type '" + value + "' requires a CONFIDENTIAL client");
        seen.insert(value);
    }
    bool first = true;
    for (const auto &value : seen)
    {
        if (!first)
            joined += ",";
        joined += value;
        first = false;
    }
    return std::nullopt;
}

using ScopesCallback = std::function<void(bool ok, const std::set<std::string> &allowed)>;

// Every requested scope must exist in the registry AND be flagged
// self_service (V035). Absent key -> ok with empty set + scopesRequested=false
// via the out-param; present-but-empty -> ok with empty set.
void validateSelfServiceScopes(
  const DbClientPtr &db,
  const Json::Value &body,
  const ::drogon::HttpRequestPtr &req,
  const ResponseCallback &cb,
  ScopesCallback &&onChecked
)
{
    std::set<std::string> requested;
    if (body.isMember("scopes"))
    {
        if (!body["scopes"].isArray())
        {
            respondError(req, cb, "VALIDATION_INVALID_INPUT", "scopes must be an array of strings");
            onChecked(false, {});
            return;
        }
        for (const auto &s : body["scopes"])
        {
            if (!s.isString() || s.asString().empty())
            {
                respondError(req, cb, "VALIDATION_INVALID_INPUT", "scopes must be an array of strings");
                onChecked(false, {});
                return;
            }
            requested.insert(s.asString());
        }
    }
    if (requested.empty())
    {
        onChecked(true, {});
        return;
    }
    std::vector<std::string> names(requested.begin(), requested.end());
    try
    {
        Mapper<ScopeModel> mapper(db);
        mapper.findBy(
          Criteria(ScopeModel::Cols::_name, CompareOperator::In, names),
          [req, cb, requested, onChecked = std::move(onChecked)](
            const std::vector<ScopeModel> &rows) {
              std::set<std::string> allowed;
              for (const auto &row : rows)
              {
                  if (row.getValueOfSelfService())
                      allowed.insert(row.getValueOfName());
              }
              for (const auto &name : requested)
              {
                  if (allowed.count(name) == 0)
                  {
                      respondError(
                        req, cb, "VALIDATION_INVALID_INPUT",
                        "scope '" + name + "' is not available to self-registered applications"
                      );
                      onChecked(false, {});
                      return;
                  }
              }
              onChecked(true, allowed);
          },
          [req, cb](const DrogonDbException &e) {
              respondError(req, cb, "DB_QUERY_ERROR",
                std::string("scope lookup failed: ") + e.base().what());
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "scope lookup: Mapper construction failed");
    }
}

void replaceClientScopes(
  const DbClientPtr &db,
  const std::string &clientId,
  const std::set<std::string> &scopes,
  const ::drogon::HttpRequestPtr &req,
  const ResponseCallback &cb,
  std::function<void()> &&onDone
)
{
    // Parallel inserts with a completion counter. The previous recursive
    // insertNext helper captured its own shared_ptr (a cycle: the lambda
    // owned the shared_ptr owning the lambda) -- a benign leak when `db`
    // was the process-lifetime pool client, but under the #219 advisory
    // transactions `db` IS the transaction: a leaked reference means the
    // transaction never destructs and never commits. No self-references
    // here; each callback holds the transaction until its statement
    // completes, and the last one releases it for commit.
    try
    {
        Mapper<ClientScopeModel>(db).deleteBy(
          Criteria(ClientScopeModel::Cols::_client_id, CompareOperator::EQ, clientId),
          [req, cb, db, clientId, scopes, onDone = std::move(onDone)](const std::size_t) {
              if (scopes.empty())
              {
                  onDone();
                  return;
              }
              auto remaining = std::make_shared<std::atomic<int>>(
                static_cast<int>(scopes.size()));
              for (const auto &name : scopes)
              {
                  ClientScopeModel row;
                  row.setClientId(clientId);
                  row.setScopeName(name);
                  try
                  {
                      Mapper<ClientScopeModel>(db).insert(
                        row,
                        [remaining, onDone, db](const ClientScopeModel &) {
                            if (remaining->fetch_sub(1) == 1)
                                onDone();
                        },
                        [req, cb](const DrogonDbException &e) {
                            respondError(req, cb, "DB_QUERY_ERROR",
                              std::string("scope write failed: ") + e.base().what());
                        }
                      );
                  }
                  catch (...)
                  {
                      respondError(req, cb, "DB_QUERY_ERROR", "scope write: Mapper construction failed");
                  }
              }
          },
          [req, cb](const DrogonDbException &e) {
              respondError(req, cb, "DB_QUERY_ERROR",
                std::string("scope reset failed: ") + e.base().what());
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "scope reset: Mapper construction failed");
    }
}

// Count ALIVE (non-soft-deleted) client rows among the given client ids.
void countAliveClients(
  const DbClientPtr &db,
  const std::vector<std::string> &clientIds,
  const ::drogon::HttpRequestPtr &req,
  const ResponseCallback &cb,
  std::function<void(std::size_t)> &&onCounted
)
{
    if (clientIds.empty())
    {
        onCounted(0);
        return;
    }
    try
    {
        Mapper<ClientModel>(db).findBy(
          Criteria(ClientModel::Cols::_client_id, CompareOperator::In, clientIds) &&
            Criteria(ClientModel::Cols::_deleted_at, CompareOperator::IsNull),
          [req, cb, onCounted = std::move(onCounted)](const std::vector<ClientModel> &rows) {
              onCounted(rows.size());
          },
          [req, cb](const DrogonDbException &e) {
              respondError(req, cb, "DB_QUERY_ERROR",
                std::string("app count failed: ") + e.base().what());
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "app count: Mapper construction failed");
    }
}

using OwnersCallback = std::function<void(const std::vector<OwnerModel> &owners)>;

void findOwners(
  const DbClientPtr &db,
  const Criteria &criteria,
  const ::drogon::HttpRequestPtr &req,
  const ResponseCallback &cb,
  OwnersCallback &&onFound
)
{
    try
    {
        Mapper<OwnerModel> mapper(db);
        mapper.findBy(
          criteria,
          [req, cb, onFound = std::move(onFound)](const std::vector<OwnerModel> &rows) {
              onFound(rows);
          },
          [req, cb](const DrogonDbException &e) {
              respondError(req, cb, "DB_QUERY_ERROR",
                std::string("owner query failed: ") + e.base().what());
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "owner query: Mapper construction failed");
    }
}

Json::Value appJsonFromRow(const ClientModel &client, const OwnerModel &owner)
{
    Json::Value app;
    app["client_id"] = client.getValueOfClientId();
    app["name"] = client.getValueOfName();
    app["client_type"] = client.getValueOfClientType();
    Json::Value uris(Json::arrayValue);
    std::stringstream ssUris(client.getValueOfRedirectUris());
    std::string item;
    while (std::getline(ssUris, item, ','))
        if (!item.empty())
            uris.append(item);
    app["redirect_uris"] = uris;
    Json::Value gts(Json::arrayValue);
    std::stringstream ssGts(client.getValueOfAllowedGrantTypes());
    while (std::getline(ssGts, item, ','))
        if (!item.empty())
            gts.append(item);
    app["allowed_grant_types"] = gts;
    app["status"] = owner.getValueOfStatus();
    app["org_id"] = owner.getOrgId() != nullptr ? Json::Value(*owner.getOrgId())
                                                : Json::Value(Json::nullValue);
    app["creator_user_id"] = owner.getValueOfCreatorUserId();
    // oauth2_clients has no created_at column (V002); the owners row is the
    // registration timestamp for self-registered apps.
    app["created_at"] = owner.getValueOfCreatedAt().toDbStringLocal();
    return app;
}

// Create the client + owner rows + scope rows. orgId == nullptr -> personal.
// The secret (CONFIDENTIAL only) is part of the single 201 response, which
// fires from the transaction's COMMIT callback (#219): an inline response
// would race the commit and an immediate follow-up read could miss the new
// rows; the cache invalidation also defers to the callback (a pre-commit DEL
// lets a concurrent read refill the cache with the stale row for a full
// TTL -- ClientManagementService's ordering note).
void insertApplication(
  const ::drogon::HttpRequestPtr &req,
  const ResponseCallback &cb,
  const std::shared_ptr<::drogon::orm::Transaction> &db,
  const std::string &name,
  const std::string &clientType,
  const std::string &redirectUris,
  const std::string &grantTypes,
  const std::set<std::string> &scopes,
  const ResolvedUser &caller,
  std::shared_ptr<int32_t> orgId
)
{
    const std::string clientId = "app_" + ::fulla::drogon::utils::generateSecureToken(8);
    // F-002 semantics: salt FIRST, then salted hash (validateClient computes
    // sha256(secret + salt)).
    const bool confidential = clientType == "CONFIDENTIAL";
    std::string secret;
    std::string salt;
    std::string secretHash;
    if (confidential)
    {
        secret = ::fulla::drogon::utils::generateSecureToken();
        salt = ::drogon::utils::getUuid().substr(0, 36);
        secretHash = ::fulla::drogon::utils::hashClientSecretWithSalt(secret, salt);
    }
    else
    {
        // PUBLIC clients carry a random unusable placeholder (ClientSeeder
        // convention): the column is NOT NULL and PUBLIC validation never
        // reads it.
        salt = ::drogon::utils::getUuid().substr(0, 36);
        secretHash = ::fulla::drogon::utils::hashClientSecretWithSalt(
          ::fulla::drogon::utils::generateSecureToken(), salt
        );
    }

    ClientModel row;
    row.setClientId(clientId);
    row.setClientType(clientType);
    row.setClientSecret(secretHash);
    row.setSalt(salt);
    row.setName(name);
    row.setRedirectUris(redirectUris);
    row.setAllowedGrantTypes(grantTypes);
    // F-017 per-type default, same as the admin/dynamic registration paths.
    row.setTokenEndpointAuthMethod(confidential ? "client_secret_basic" : "none");

    try
    {
        Mapper<ClientModel>(db).insert(
          row,
          [req, cb, db, clientId, secret, confidential, scopes, caller, orgId](
            const ClientModel &) {
              OwnerModel owner;
              owner.setClientId(clientId);
              owner.setCreatorUserId(caller.id);
              if (orgId != nullptr)
                  owner.setOrgId(*orgId);
              else
                  owner.setOrgIdToNull();
              owner.setStatus("active");
              try
              {
                  Mapper<OwnerModel>(db).insert(
                    owner,
                    [req, cb, db, clientId, secret, confidential, scopes, orgId, caller](
                      const OwnerModel &) {
                        replaceClientScopes(db, clientId, scopes, req, cb,
                          [req, cb, db, clientId, secret, confidential, orgId, caller]() {
                              // #219: respond + invalidate from the COMMIT
                              // callback -- the terminal scope insert's
                              // callback returns while the transaction is
                              // still open (it commits when this chain's
                              // last db reference drops, i.e. right after
                              // this callback returns).
                              db->setCommitCallback(
                                [req, cb, clientId, secret, confidential, orgId, caller](
                                  bool committed) {
                                    if (!committed)
                                    {
                                        respondError(req, cb, "DB_QUERY_ERROR",
                                          "application creation commit failed");
                                        return;
                                    }
                                    audit(req, "application_created",
                                      clientId);
                                    // Review C2: every client write invalidates the
                                    // Redis client cache or a rotated/created row
                                    // stays trusted for up to the cache TTL.
                                    // (After the commit -- see the function comment.)
                                    ::fulla::drogon::ClientCacheInvalidator::instance().invalidate(
                                      clientId);
                                    Json::Value json;
                                    json["client_id"] = clientId;
                                    json["client_type"] = confidential ? "CONFIDENTIAL" : "PUBLIC";
                                    if (confidential)
                                    {
                                        // Shown EXACTLY once (design §3/§8).
                                        json["client_secret"] = secret;
                                    }
                                    if (orgId != nullptr)
                                        json["org_id"] = *orgId;
                                    json["message"] = confidential
                                                        ? "Application created; store the secret now — it is not shown again"
                                                        : "Application created";
                                    auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                                    resp->setStatusCode(::drogon::k201Created);
                                    (*cb)(resp);
                                });
                          });
                    },
                    [req, cb, db, clientId](const DrogonDbException &e) {
                        // Review M2: an ownerless client row is a governance
                        // blind spot (admin-owned semantics, unquota'd,
                        // unsuspendable) — compensate by removing the client
                        // row we just inserted.
                        LOG_ERROR << "application owner insert failed for " << clientId
                                  << ", compensating: " << e.base().what();
                        try
                        {
                            Mapper<ClientModel>(db).deleteBy(
                              Criteria(ClientModel::Cols::_client_id, CompareOperator::EQ, clientId),
                              [](const std::size_t) {},
                              [clientId](const DrogonDbException &ce) {
                                  LOG_ERROR << "compensation delete failed for " << clientId
                                            << " (orphaned client row remains): "
                                            << ce.base().what();
                              }
                            );
                        }
                        catch (...)
                        {
                            LOG_ERROR << "compensation Mapper construction failed for "
                                      << clientId;
                        }
                        respondError(req, cb, "DB_QUERY_ERROR",
                          std::string("application owner insert failed: ") + e.base().what());
                    }
                  );
              }
              catch (...)
              {
                  respondError(req, cb, "DB_QUERY_ERROR", "owner insert: Mapper construction failed");
              }
          },
          [req, cb](const DrogonDbException &e) {
              respondError(req, cb, "DB_QUERY_ERROR",
                std::string("application insert failed: ") + e.base().what());
          }
        );
    }
    catch (...)
    {
        respondError(req, cb, "DB_QUERY_ERROR", "application insert: Mapper construction failed");
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// GET /api/me/applications
// ---------------------------------------------------------------------------
void ApplicationService::list(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          try
          {
              Mapper<MemberModel>(db).findBy(
                Criteria(MemberModel::Cols::_user_id, CompareOperator::EQ, caller.id),
                [req, cb, db, caller](const std::vector<MemberModel> &memberships) {
                    std::vector<int32_t> managerOrgIds;
                    for (const auto &m : memberships)
                        if (isManagerRole(m.getValueOfRole()))
                            managerOrgIds.push_back(m.getValueOfOrganizationId());

                    auto merged = std::make_shared<std::map<std::string, OwnerModel>>();
                    auto pending = std::make_shared<int>(1 + (managerOrgIds.empty() ? 0 : 1));
                    // Review minor: if one parallel findOwners fails AFTER
                    // the other already responded, the error callback would
                    // emit a second response — guard every emission.
                    auto responded = std::make_shared<bool>(false);
                    auto guardedRespond =
                      [cb, responded](const ::drogon::HttpResponsePtr &r) {
                          if (*responded)
                          {
                              LOG_WARN << "applications list: suppressing duplicate response";
                              return;
                          }
                          *responded = true;
                          (*cb)(r);
                      };
                    auto finish = std::make_shared<std::function<void()>>();
                    *finish = [req, cb, guardedRespond, db, merged]() {
                        // Error paths route through the same guard so a
                        // late failure can't double-respond after a success.
                        auto guardedCb = std::make_shared<ResponseCallback::element_type>(
                          guardedRespond);
                        if (merged->empty())
                        {
                            Json::Value json;
                            json["applications"] = Json::Value(Json::arrayValue);
                            json["total"] = 0;
                            guardedRespond(::drogon::HttpResponse::newHttpJsonResponse(json));
                            return;
                        }
                        std::vector<std::string> clientIds;
                        for (const auto &kv : *merged)
                            clientIds.push_back(kv.first);
                        try
                        {
                            Mapper<ClientModel>(db).findBy(
                              Criteria(ClientModel::Cols::_client_id, CompareOperator::In, clientIds) &&
                                Criteria(ClientModel::Cols::_deleted_at, CompareOperator::IsNull),
                              [req, guardedRespond, merged](const std::vector<ClientModel> &clients) {
                                  Json::Value json;
                                  Json::Value arr(Json::arrayValue);
                                  for (const auto &c : clients)
                                  {
                                      auto it = merged->find(c.getValueOfClientId());
                                      if (it == merged->end())
                                          continue;
                                      arr.append(appJsonFromRow(c, it->second));
                                  }
                                  json["applications"] = arr;
                                  json["total"] = static_cast<int>(arr.size());
                                  guardedRespond(::drogon::HttpResponse::newHttpJsonResponse(json));
                              },
                              [req, guardedCb](const DrogonDbException &e) {
                                  respondError(req, guardedCb, "DB_QUERY_ERROR",
                                    std::string("application query failed: ") + e.base().what());
                              }
                            );
                        }
                        catch (...)
                        {
                            respondError(req, guardedCb, "DB_QUERY_ERROR",
                              "application query: Mapper construction failed");
                        }
                    };

                    findOwners(db,
                      Criteria(OwnerModel::Cols::_creator_user_id, CompareOperator::EQ, caller.id) &&
                        Criteria(OwnerModel::Cols::_org_id, CompareOperator::IsNull),
                      req, cb, [merged, pending, finish](const std::vector<OwnerModel> &rows) {
                          for (const auto &r : rows)
                              (*merged)[r.getValueOfClientId()] = r;
                          if (--(*pending) == 0)
                              (*finish)();
                      });
                    if (!managerOrgIds.empty())
                    {
                        findOwners(db,
                          Criteria(OwnerModel::Cols::_org_id, CompareOperator::In, managerOrgIds),
                          req, cb, [merged, pending, finish](const std::vector<OwnerModel> &rows) {
                              for (const auto &r : rows)
                                  (*merged)[r.getValueOfClientId()] = r;
                              if (--(*pending) == 0)
                                  (*finish)();
                          });
                    }
                },
                [req, cb](const DrogonDbException &e) {
                    respondError(req, cb, "DB_QUERY_ERROR",
                      std::string("memberships lookup failed: ") + e.base().what());
                }
              );
          }
          catch (...)
          {
              respondError(req, cb, "DB_QUERY_ERROR", "memberships lookup: Mapper construction failed");
          }
      });
}

// ---------------------------------------------------------------------------
// POST /api/me/applications
// ---------------------------------------------------------------------------
void ApplicationService::create(const ::drogon::HttpRequestPtr &req, ResponseCallback cb)
{
    const auto cfg = OpenPlatformConfig::load();
    if (!cfg.enabled)
    {
        respondError(req, cb, "AUTHZ_ACCESS_DENIED",
          "application self-registration is disabled on this deployment");
        return;
    }

    auto jsonBody = req->getJsonObject();
    if (!jsonBody)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "create application: JSON body required");
        return;
    }
    const std::string name = (*jsonBody).get("name", "").asString();
    const std::string clientType = (*jsonBody).get("client_type", "CONFIDENTIAL").asString();
    if (name.empty() || name.size() > 100)
    {
        respondError(req, cb, "VALIDATION_MISSING_REQUIRED_FIELD",
          "create application: name is required (<=100 chars)");
        return;
    }
    if (clientType != "PUBLIC" && clientType != "CONFIDENTIAL")
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT",
          "create application: client_type must be PUBLIC or CONFIDENTIAL");
        return;
    }
    std::string redirectUris;
    if (auto err = validateAndJoinRedirectUris(*jsonBody, cfg.maxRedirectUris, redirectUris))
    {
        respondError(req, cb, "VALIDATION_FORMAT_ERROR", "create application: " + *err);
        return;
    }
    std::string grantTypes;
    if (auto err = validateAndJoinGrantTypes(*jsonBody, clientType, grantTypes))
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "create application: " + *err);
        return;
    }
    if (grantTypes.empty())
        grantTypes = clientType == "PUBLIC" ? "authorization_code" : "client_credentials";

    const bool hasOrgSlug = (*jsonBody).isMember("org_slug") &&
                            (*jsonBody)["org_slug"].isString() &&
                            !(*jsonBody)["org_slug"].asString().empty();
    if (cfg.requireOrg && !hasOrgSlug)
    {
        respondError(req, cb, "AUTHZ_ACCESS_DENIED",
          "this deployment requires applications to be registered under an organization");
        return;
    }

    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    validateSelfServiceScopes(db, *jsonBody, req, cb,
      [req, cb, db, cfg, name, clientType, redirectUris, grantTypes, hasOrgSlug, userIdAttr,
       jsonBody](bool ok, const std::set<std::string> &scopes) {
          if (!ok)
              return;
          resolveCaller(db, userIdAttr, req, cb,
            [req, cb, db, cfg, name, clientType, redirectUris, grantTypes, scopes, hasOrgSlug,
             jsonBody](bool found, const ResolvedUser &caller) {
                if (!found)
                    return;
                const int64_t nowSec = ::trantor::Date::now().secondsSinceEpoch();

                // #219 (R-M2-5): rate limit + quota checks and the insert
                // run inside ONE advisory-locked transaction per quota
                // domain. Personal branch: quota:user:<id> (24h rate limit
                // + personal quota share the user domain). Org branch:
                // quota:user:<id> THEN quota:org:<org_id> in the SAME
                // transaction (consistent user-before-org order everywhere
                // prevents cross-flow deadlocks).
                auto runCreationChecks =
                  [req, cb, cfg, caller, nowSec, name, clientType, redirectUris, grantTypes,
                   scopes](const std::shared_ptr<::drogon::orm::Transaction> &txn,
                           std::shared_ptr<int32_t> orgId) {
                      // Rate limit + personal quota both key off "my creations".
                      findOwners(txn,
                        Criteria(OwnerModel::Cols::_creator_user_id, CompareOperator::EQ, caller.id),
                        req, cb,
                        [req, cb, txn, cfg, name, clientType, redirectUris, grantTypes, scopes,
                         caller, nowSec, orgId](const std::vector<OwnerModel> &mine) {
                          std::size_t recent = 0;
                          std::vector<std::string> personalIds;
                          for (const auto &o : mine)
                          {
                              if (o.getValueOfCreatedAt().secondsSinceEpoch() > nowSec - 86400)
                                  ++recent;
                              if (o.getOrgId() == nullptr)
                                  personalIds.push_back(o.getValueOfClientId());
                          }
                          if (static_cast<int>(recent) >= cfg.creationRatePerDay)
                          {
                              respondError(req, cb, "VALIDATION_RATE_LIMITED",
                                "application creation rate limit reached; try again later");
                              return;
                          }
                          if (orgId == nullptr)
                          {
                              countAliveClients(txn, personalIds, req, cb,
                                [req, cb, txn, name, clientType, redirectUris, grantTypes, scopes,
                                 caller, cfg](std::size_t alive) {
                                    if (static_cast<int>(alive) >= cfg.maxAppsPerUser)
                                    {
                                        respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT",
                                          "application quota exceeded (" +
                                            std::to_string(cfg.maxAppsPerUser) + ")");
                                        return;
                                    }
                                    insertApplication(req, cb, txn, name, clientType, redirectUris,
                                      grantTypes, scopes, caller, nullptr);
                                });
                              return;
                          }
                          // Org context: the org must be under its app quota.
                          findOwners(txn,
                            Criteria(OwnerModel::Cols::_org_id,
                                     CompareOperator::EQ, *orgId),
                            req, cb,
                            [req, cb, txn, cfg, name, clientType, redirectUris, grantTypes,
                             scopes, caller, orgId](
                              const std::vector<OwnerModel> &orgApps) {
                                std::vector<std::string> ids;
                                for (const auto &o : orgApps)
                                    ids.push_back(o.getValueOfClientId());
                                countAliveClients(txn, ids, req, cb,
                                  [req, cb, txn, cfg, name, clientType, redirectUris,
                                   grantTypes, scopes, caller, orgId](std::size_t alive) {
                                      if (static_cast<int>(alive) >= cfg.maxOrgApps)
                                      {
                                          respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT",
                                            "organization application quota exceeded (" +
                                              std::to_string(cfg.maxOrgApps) + ")");
                                          return;
                                      }
                                      insertApplication(req, cb, txn, name, clientType,
                                        redirectUris, grantTypes, scopes, caller,
                                        orgId);
                                  });
                            });
                          return;
                      });
                  };
                auto lockFailed = [req, cb](const std::string &err) {
                    respondError(req, cb, "DB_QUERY_ERROR",
                      "create application: quota serialization failed: " + err);
                };

                if (!hasOrgSlug)
                {
                    withAdvisoryXactLock(
                      db,
                      {"quota:user:" + std::to_string(caller.id)},
                      [runCreationChecks](const std::shared_ptr<::drogon::orm::Transaction> &txn) {
                          runCreationChecks(txn, nullptr);
                      },
                      lockFailed);
                    return;
                }
                // Org context: caller must be owner/admin of the target
                // org (checked outside the lock -- an authz decision, not
                // a quota), and the org must be under its app quota.
                const std::string orgSlug = (*jsonBody)["org_slug"].asString();
                try
                {
                    Mapper<OrgModel>(db).findOne(
                      Criteria(OrgModel::Cols::_slug, CompareOperator::EQ, orgSlug),
                      [req, cb, db, cfg, name, clientType, redirectUris, grantTypes,
                       scopes, caller, runCreationChecks, lockFailed](const OrgModel &org) {
                          const int32_t orgId = org.getValueOfId();
                          try
                          {
                              Mapper<MemberModel>(db).findBy(
                                Criteria(MemberModel::Cols::_organization_id,
                                         CompareOperator::EQ, orgId) &&
                                  Criteria(MemberModel::Cols::_user_id,
                                           CompareOperator::EQ, caller.id),
                                [req, cb, db, cfg, name, clientType, redirectUris, grantTypes,
                                 scopes, caller, orgId, runCreationChecks, lockFailed](
                                  const std::vector<MemberModel> &rows) {
                                  if (rows.empty() || !isManagerRole(rows[0].getValueOfRole()))
                                  {
                                      respondError(req, cb, "AUTHZ_ACCESS_DENIED",
                                        "org owner or admin role required to register an application for it");
                                      return;
                                  }
                                  withAdvisoryXactLock(
                                    db,
                                    {"quota:user:" + std::to_string(caller.id),
                                     "quota:org:" + std::to_string(orgId)},
                                    [runCreationChecks, orgId](
                                      const std::shared_ptr<::drogon::orm::Transaction> &txn) {
                                        runCreationChecks(
                                          txn, std::make_shared<int32_t>(orgId));
                                    },
                                    lockFailed);
                              },
                              [req, cb](const DrogonDbException &e) {
                                  respondError(req, cb, "DB_QUERY_ERROR",
                                    std::string("membership lookup failed: ") + e.base().what());
                              }
                            );
                          }
                          catch (...)
                          {
                              respondError(req, cb, "DB_QUERY_ERROR",
                                "membership lookup: Mapper construction failed");
                          }
                      },
                      [req, cb](const DrogonDbException &) {
                          respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND",
                            "organization not found");
                      }
                    );
                }
                catch (...)
                {
                    respondError(req, cb, "DB_QUERY_ERROR", "org lookup: Mapper construction failed");
                }
          });
      });
}

// ---------------------------------------------------------------------------
// PATCH /api/me/applications/{clientId}
// ---------------------------------------------------------------------------
void ApplicationService::update(
  const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &clientId
)
{
    const auto cfg = OpenPlatformConfig::load();
    auto jsonBody = req->getJsonObject();
    if (!jsonBody)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "update application: JSON body required");
        return;
    }
    const bool nameRequested = (*jsonBody).isMember("name");
    const bool urisRequested = (*jsonBody).isMember("redirect_uris");
    const bool grantsRequested = (*jsonBody).isMember("allowed_grant_types");
    const bool scopesRequested = (*jsonBody).isMember("scopes");
    if (!nameRequested && !urisRequested && !grantsRequested && !scopesRequested)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT",
          "update application: no editable field present "
          "(name / redirect_uris / allowed_grant_types / scopes)");
        return;
    }
    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, cfg, clientId, jsonBody, nameRequested, urisRequested, grantsRequested,
       scopesRequested](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          requireManagePermission(db, clientId, caller, req, cb,
            [req, cb, db, cfg, clientId, jsonBody, nameRequested, urisRequested,
             grantsRequested, scopesRequested](bool ok, const OwnerModel &) {
                if (!ok)
                    return;
                try
                {
                    Mapper<ClientModel>(db).findOne(
                      Criteria(ClientModel::Cols::_client_id, CompareOperator::EQ, clientId) &&
                        Criteria(ClientModel::Cols::_deleted_at, CompareOperator::IsNull),
                      [req, cb, db, cfg, clientId, jsonBody, nameRequested, urisRequested,
                       grantsRequested, scopesRequested](const ClientModel &client) {
                          std::string name;
                          std::string redirectUris;
                          std::string grantTypes;
                          if (nameRequested)
                          {
                              name = (*jsonBody).get("name", "").asString();
                              if (name.empty() || name.size() > 100)
                              {
                                  respondError(req, cb, "VALIDATION_INVALID_INPUT",
                                    "update application: name must be 1-100 chars");
                                  return;
                              }
                          }
                          if (urisRequested)
                          {
                              if (auto err = validateAndJoinRedirectUris(
                                    *jsonBody, cfg.maxRedirectUris, redirectUris))
                              {
                                  respondError(req, cb, "VALIDATION_FORMAT_ERROR",
                                    "update application: " + *err);
                                  return;
                              }
                          }
                          if (grantsRequested)
                          {
                              if (auto err = validateAndJoinGrantTypes(
                                    *jsonBody, client.getValueOfClientType(), grantTypes))
                              {
                                  respondError(req, cb, "VALIDATION_INVALID_INPUT",
                                    "update application: " + *err);
                                  return;
                              }
                              if (grantTypes.empty())
                              {
                                  respondError(req, cb, "VALIDATION_INVALID_INPUT",
                                    "update application: at least one grant type is required");
                                  return;
                              }
                          }
                          validateSelfServiceScopes(db, *jsonBody, req, cb,
                            [req, cb, db, clientId, jsonBody, client, name, redirectUris,
                             grantTypes, nameRequested, urisRequested, grantsRequested,
                             scopesRequested](bool scopesOk, const std::set<std::string> &scopes) {
                                if (!scopesOk)
                                    return;
                                ClientModel updated = client;
                                bool touched = false;
                                if (nameRequested)
                                {
                                    updated.setName(name);
                                    touched = true;
                                }
                                if (urisRequested)
                                {
                                    updated.setRedirectUris(redirectUris);
                                    touched = true;
                                }
                                if (grantsRequested)
                                {
                                    updated.setAllowedGrantTypes(grantTypes);
                                    touched = true;
                                }
                                ::fulla::drogon::ClientCacheInvalidator::instance()
                                  .invalidate(clientId);
                                auto respondOk = [req, cb]() {
                                    Json::Value json;
                                    json["message"] = "Application updated";
                                    (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                                };
                                auto afterRow = [req, cb, db, clientId, scopes, scopesRequested,
                                                 respondOk](const std::size_t) {
                                    if (scopesRequested)
                                    {
                                        replaceClientScopes(db, clientId, scopes, req, cb,
                                          [respondOk]() {});
                                    }
                                    else
                                    {
                                        respondOk();
                                    }
                                };
                                if (touched)
                                {
                                    try
                                    {
                                        Mapper<ClientModel>(db).update(
                                          updated, afterRow,
                                          [req, cb](const DrogonDbException &e) {
                                              respondError(req, cb, "DB_QUERY_ERROR",
                                                std::string("application update failed: ") +
                                                  e.base().what());
                                          }
                                        );
                                    }
                                    catch (...)
                                    {
                                        respondError(req, cb, "DB_QUERY_ERROR",
                                          "application update: Mapper construction failed");
                                    }
                                }
                                else if (scopesRequested)
                                {
                                    replaceClientScopes(db, clientId, scopes, req, cb,
                                      [respondOk]() {});
                                }
                            });
                      },
                      [req, cb](const DrogonDbException &) {
                          respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND",
                            "application not found");
                      }
                    );
                }
                catch (...)
                {
                    respondError(req, cb, "DB_QUERY_ERROR",
                      "application lookup: Mapper construction failed");
                }
            });
      });
}

// ---------------------------------------------------------------------------
// POST /api/me/applications/{clientId}/rotate-secret
// ---------------------------------------------------------------------------
void ApplicationService::rotateSecret(
  const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &clientId
)
{
    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, clientId](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          requireManagePermission(db, clientId, caller, req, cb,
            [req, cb, db, clientId](bool ok, const OwnerModel &) {
                if (!ok)
                    return;
                try
                {
                    Mapper<ClientModel>(db).findOne(
                      Criteria(ClientModel::Cols::_client_id, CompareOperator::EQ, clientId) &&
                        Criteria(ClientModel::Cols::_deleted_at, CompareOperator::IsNull),
                      [req, cb, db, clientId](const ClientModel &client) {
                          if (client.getValueOfClientType() != "CONFIDENTIAL")
                          {
                              respondError(req, cb, "VALIDATION_INVALID_INPUT",
                                "rotate-secret: PUBLIC clients have no secret");
                              return;
                          }
                          const std::string secret = ::fulla::drogon::utils::generateSecureToken();
                          const std::string salt = ::drogon::utils::getUuid().substr(0, 36);
                          ClientModel updated = client;
                          updated.setClientSecret(
                            ::fulla::drogon::utils::hashClientSecretWithSalt(secret, salt)
                          );
                          updated.setSalt(salt);
                          try
                          {
                              Mapper<ClientModel>(db).update(
                                updated,
                                [req, cb, clientId, secret](const std::size_t) {
                                    ::fulla::drogon::ClientCacheInvalidator::instance()
                                      .invalidate(clientId);
                                    audit(req, "application_secret_rotated", clientId);
                                    Json::Value json;
                                    json["client_id"] = clientId;
                                    // Shown EXACTLY once (design §3/§8).
                                    json["client_secret"] = secret;
                                    json["message"] =
                                      "Secret rotated; store it now — the previous secret is invalid";
                                    (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                                },
                                [req, cb](const DrogonDbException &e) {
                                    respondError(req, cb, "DB_QUERY_ERROR",
                                      std::string("secret rotation failed: ") + e.base().what());
                                }
                              );
                          }
                          catch (...)
                          {
                              respondError(req, cb, "DB_QUERY_ERROR",
                                "secret rotation: Mapper construction failed");
                          }
                      },
                      [req, cb](const DrogonDbException &) {
                          respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND",
                            "application not found");
                      }
                    );
                }
                catch (...)
                {
                    respondError(req, cb, "DB_QUERY_ERROR",
                      "application lookup: Mapper construction failed");
                }
            });
      });
}

// ---------------------------------------------------------------------------
// POST /api/me/applications/{clientId}/transfer  {org_slug} | {org_slug: null}
// ---------------------------------------------------------------------------
void ApplicationService::transfer(
  const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &clientId
)
{
    const auto cfg = OpenPlatformConfig::load();
    auto jsonBody = req->getJsonObject();
    if (!jsonBody)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT", "transfer: JSON body required");
        return;
    }
    const bool toPersonal = !(*jsonBody).isMember("org_slug") ||
                            (*jsonBody)["org_slug"].isNull();
    const bool toOrg = (*jsonBody).isMember("org_slug") &&
                       (*jsonBody)["org_slug"].isString() &&
                       !(*jsonBody)["org_slug"].asString().empty();
    if (!toPersonal && !toOrg)
    {
        respondError(req, cb, "VALIDATION_INVALID_INPUT",
          "transfer: org_slug must be a non-empty string or null");
        return;
    }

    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, cfg, clientId, jsonBody, toPersonal, toOrg](bool found,
                                                               const ResolvedUser &caller) {
          if (!found)
              return;
          requireManagePermission(db, clientId, caller, req, cb,
            [req, cb, db, cfg, clientId, jsonBody, toPersonal, toOrg, caller](
              bool ok, const OwnerModel &owner) {
                if (!ok)
                    return;
                if (toPersonal)
                {
                    if (owner.getOrgId() == nullptr)
                    {
                        respondError(req, cb, "VALIDATION_INVALID_INPUT",
                          "transfer: application is already personal");
                        return;
                    }
                    // Review M8: moving back to personal must respect the
                    // CREATOR's personal-app quota (personal apps are keyed by
                    // creator_user_id, not by whoever manages the org today).
                    findOwners(db,
                      Criteria(OwnerModel::Cols::_creator_user_id,
                               CompareOperator::EQ, owner.getValueOfCreatorUserId()) &&
                        Criteria(OwnerModel::Cols::_org_id, CompareOperator::IsNull),
                      req, cb,
                      [req, cb, db, cfg, clientId, owner](const std::vector<OwnerModel> &personal) {
                          std::vector<std::string> ids;
                          for (const auto &o : personal)
                              ids.push_back(o.getValueOfClientId());
                          countAliveClients(db, ids, req, cb,
                            [req, cb, db, cfg, clientId, owner](std::size_t alive) {
                                if (static_cast<int>(alive) >= cfg.maxAppsPerUser)
                                {
                                    respondError(req, cb, "VALIDATION_RESOURCE_CONFLICT",
                                      "application quota exceeded (" +
                                        std::to_string(cfg.maxAppsPerUser) + ")");
                                    return;
                                }
                                OwnerModel updated = owner;
                                updated.setOrgIdToNull();
                    try
                    {
                        Mapper<OwnerModel>(db).update(
                          updated,
                          [req, cb, clientId](const std::size_t) {
                              audit(req, "application_transferred", clientId);
                              Json::Value json;
                              json["message"] = "Application transferred to personal ownership";
                              (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                          },
                          [req, cb](const DrogonDbException &e) {
                              respondError(req, cb, "DB_QUERY_ERROR",
                                std::string("transfer failed: ") + e.base().what());
                          }
                        );
                    }
                    catch (...)
                    {
                        respondError(req, cb, "DB_QUERY_ERROR", "transfer: Mapper construction failed");
                    }
                    return;
                            });
                      });
                    // Self-review: the M8 quota gate is an async chain — after
                    // launching it we MUST return here, or execution falls
                    // through to the toOrg section below (double response:
                    // immediate 400 "already belongs to one" + the chain's).
                    return;
                }
                const std::string orgSlug = (*jsonBody)["org_slug"].asString();
                if (owner.getOrgId() != nullptr)
                {
                    respondError(req, cb, "VALIDATION_INVALID_INPUT",
                      "transfer: target organization required (application already belongs to one)");
                    return;
                }
                // Target org: caller must be owner/admin; org under quota.
                try
                {
                    Mapper<OrgModel>(db).findOne(
                      Criteria(OrgModel::Cols::_slug, CompareOperator::EQ, orgSlug),
                      [req, cb, db, cfg, clientId, caller, owner](const OrgModel &org) {
                          const int32_t orgId = org.getValueOfId();
                          try
                          {
                              Mapper<MemberModel>(db).findBy(
                                Criteria(MemberModel::Cols::_organization_id,
                                         CompareOperator::EQ, orgId) &&
                                  Criteria(MemberModel::Cols::_user_id,
                                           CompareOperator::EQ, caller.id),
                                [req, cb, db, cfg, clientId, owner, orgId, caller](
                                  const std::vector<MemberModel> &rows) {
                                    if (rows.empty() || !isManagerRole(rows[0].getValueOfRole()))
                                    {
                                        respondError(req, cb, "AUTHZ_ACCESS_DENIED",
                                          "org owner or admin role required to receive a transfer");
                                        return;
                                    }
                                    findOwners(db,
                                      Criteria(OwnerModel::Cols::_org_id,
                                               CompareOperator::EQ, orgId),
                                      req, cb,
                                      [req, cb, db, cfg, clientId, owner, orgId](
                                        const std::vector<OwnerModel> &orgApps) {
                                          std::vector<std::string> ids;
                                          for (const auto &o : orgApps)
                                              if (o.getValueOfClientId() != clientId)
                                                  ids.push_back(o.getValueOfClientId());
                                          countAliveClients(db, ids, req, cb,
                                            [req, cb, db, cfg, clientId, owner, orgId](
                                              std::size_t alive) {
                                                if (static_cast<int>(alive) >= cfg.maxOrgApps)
                                                {
                                                    respondError(req, cb,
                                                      "VALIDATION_RESOURCE_CONFLICT",
                                                      "organization application quota exceeded (" +
                                                        std::to_string(cfg.maxOrgApps) + ")");
                                                    return;
                                                }
                                                OwnerModel updated = owner;
                                                updated.setOrgId(orgId);
                                                try
                                                {
                                                    Mapper<OwnerModel>(db).update(
                                                      updated,
                                                      [req, cb, clientId, orgId](const std::size_t) {
                                                          audit(req, "application_transferred",
                                                            clientId);
                                                          Json::Value json;
                                                          json["message"] =
                                                            "Application transferred to organization";
                                                          json["org_id"] = orgId;
                                                          (*cb)(::drogon::HttpResponse::newHttpJsonResponse(
                                                            json));
                                                      },
                                                      [req, cb](const DrogonDbException &e) {
                                                          respondError(req, cb, "DB_QUERY_ERROR",
                                                            std::string("transfer failed: ") +
                                                              e.base().what());
                                                      }
                                                    );
                                                }
                                                catch (...)
                                                {
                                                    respondError(req, cb, "DB_QUERY_ERROR",
                                                      "transfer: Mapper construction failed");
                                                }
                                            });
                                      });
                                },
                                [req, cb](const DrogonDbException &e) {
                                    respondError(req, cb, "DB_QUERY_ERROR",
                                      std::string("membership lookup failed: ") + e.base().what());
                                }
                              );
                      }
                      catch (...)
                      {
                          respondError(req, cb, "DB_QUERY_ERROR", "membership lookup: Mapper construction failed");
                      }
                  },
                  [req, cb](const DrogonDbException &) {
                      respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND", "organization not found");
                  }
                );
                }
                catch (...)
                {
                    respondError(req, cb, "DB_QUERY_ERROR", "org lookup: Mapper construction failed");
                }
            }
          );
      });
}

// ---------------------------------------------------------------------------
// DELETE /api/me/applications/{clientId} — soft delete (V035).
// ---------------------------------------------------------------------------
void ApplicationService::remove(
  const ::drogon::HttpRequestPtr &req, ResponseCallback cb, const std::string &clientId
)
{
    std::string userIdAttr = req->getAttributes()->get<std::string>("userId");
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    resolveCaller(db, userIdAttr, req, cb,
      [req, cb, db, clientId](bool found, const ResolvedUser &caller) {
          if (!found)
              return;
          requireManagePermission(db, clientId, caller, req, cb,
            [req, cb, db, clientId](bool ok, const OwnerModel &) {
                if (!ok)
                    return;
                try
                {
                    Mapper<ClientModel>(db).findOne(
                      Criteria(ClientModel::Cols::_client_id, CompareOperator::EQ, clientId) &&
                        Criteria(ClientModel::Cols::_deleted_at, CompareOperator::IsNull),
                      [req, cb, db, clientId](const ClientModel &client) {
                          // delete stays open for suspended apps (exit path).
                          ClientModel updated = client;
                          updated.setDeletedAt(::trantor::Date::now());
                          try
                          {
                              Mapper<ClientModel>(db).update(
                                updated,
                                [req, cb, clientId](const std::size_t) {
                                    ::fulla::drogon::ClientCacheInvalidator::instance()
                                      .invalidate(clientId);
                                    audit(req, "application_deleted", clientId);
                                    Json::Value json;
                                    json["message"] = "Application deleted";
                                    (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                                },
                                [req, cb](const DrogonDbException &e) {
                                    respondError(req, cb, "DB_QUERY_ERROR",
                                      std::string("application delete failed: ") + e.base().what());
                                }
                              );
                          }
                          catch (...)
                          {
                              respondError(req, cb, "DB_QUERY_ERROR",
                                "application delete: Mapper construction failed");
                          }
                      },
                      [req, cb](const DrogonDbException &) {
                          respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND",
                            "application not found");
                      }
                    );
                }
                catch (...)
                {
                    respondError(req, cb, "DB_QUERY_ERROR",
                      "application lookup: Mapper construction failed");
                }
            }, false);
      });
}

// ---------------------------------------------------------------------------
// POST /api/admin/clients/{clientId}/suspend | /resume
// ---------------------------------------------------------------------------
void ApplicationService::setStatus(
  const ::drogon::HttpRequestPtr &req,
  ResponseCallback cb,
  const std::string &clientId,
  bool suspended
)
{
    auto db = getDbOrRespond(req, cb);
    if (!db)
        return;

    loadOwnerRow(db, clientId, req, cb,
      [req, cb, db, clientId, suspended](bool exists, const OwnerModel &owner) {
          if (!exists)
          {
              respondError(req, cb, "VALIDATION_RESOURCE_NOT_FOUND",
                "not a self-registered application (admin-managed clients are managed by delete)");
              return;
          }
          OwnerModel updated = owner;
          updated.setStatus(suspended ? "suspended" : "active");
          try
          {
              Mapper<OwnerModel>(db).update(
                updated,
                [req, cb, clientId, suspended, db](const std::size_t) {
                    ::fulla::drogon::ClientCacheInvalidator::instance().invalidate(clientId);
                    audit(req, suspended ? "client_suspended" : "client_resumed", clientId);
                    if (suspended)
                    {
                        // Review M5: "suspended = no new codes or tokens" must
                        // also cover tokens ALREADY issued — revoke them in
                        // place (documented batch-UPDATE exemption; the
                        // response is not delayed by this fire-and-forget).
                        db->execSqlAsync(
                          "UPDATE oauth2_access_tokens SET revoked = true WHERE client_id = $1",
                          [db, clientId](const ::drogon::orm::Result &) {
                              db->execSqlAsync(
                                "UPDATE oauth2_refresh_tokens SET revoked = true WHERE client_id = $1",
                                [clientId](const ::drogon::orm::Result &) {},
                                [clientId](const ::drogon::orm::DrogonDbException &e) {
                                    LOG_ERROR << "suspend: refresh revocation failed for "
                                              << clientId << ": " << e.base().what();
                                },
                                clientId
                              );
                          },
                          [clientId](const ::drogon::orm::DrogonDbException &e) {
                              LOG_ERROR << "suspend: access revocation failed for "
                                        << clientId << ": " << e.base().what();
                          },
                          clientId
                        );
                    }
                    Json::Value json;
                    json["client_id"] = clientId;
                    json["status"] = suspended ? "suspended" : "active";
                    (*cb)(::drogon::HttpResponse::newHttpJsonResponse(json));
                },
                [req, cb](const DrogonDbException &e) {
                    respondError(req, cb, "DB_QUERY_ERROR",
                      std::string("status update failed: ") + e.base().what());
                }
              );
          }
          catch (...)
          {
              respondError(req, cb, "DB_QUERY_ERROR", "status update: Mapper construction failed");
          }
      });
}

}  // namespace openplatform
