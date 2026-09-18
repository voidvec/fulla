#include <fulla/storage/postgres/PostgresClientRepository.h>
#include <fulla/common/utils/ConstantTimeCompare.h>
#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>

#include <fulla/storage/postgres/models/Oauth2Clients.h>
#include <fulla/storage/postgres/models/Oauth2Scopes.h>
#include <fulla/storage/postgres/models/Oauth2ClientScopes.h>
#include <fulla/storage/postgres/models/Oauth2ClientOwners.h>

namespace fulla::storage::postgres
{

// F-004: constant-time comparison now comes from the shared
// fulla::common::utils::constantTimeMemcmp (previously a verbatim
// anonymous-namespace copy lived here and in Memory/Redis backends).
using ::fulla::common::utils::constantTimeMemcmp;

// Task 27.5: callback + DTO aliases for the new base interface; safe at namespace scope here (this
// .cc does not include IOAuth2Storage.h, so no oauth2::* clash).
using OAuth2Client = ::fulla::oauth2::model::OAuth2Client;
using ClientType = ::fulla::oauth2::model::ClientType;
using ::fulla::oauth2::model::stringToClientType;
using ClientCallback = IClientRepositoryBase::ClientCallback;
using BoolCallback = IClientRepositoryBase::BoolCallback;

using namespace ::drogon::orm;
using namespace drogon_model::fulla_db;

void PostgresClientRepository::getClient(const std::string &clientId, ClientCallback &&cb)
{
    LOG_DEBUG << "Postgres getClient: " << clientId;

    // Lazy initialization of DB clients if they are null
    if (!dbClientReader_)
    {
        try
        {
            dbClientMaster_ = ::drogon::app().getDbClient(dbClientName_);
            dbClientReader_ = ::drogon::app().getDbClient(dbClientReaderName_);
            LOG_INFO << "Postgres DB Clients initialized lazily for getClient";
        }
        catch (...)
        {
            LOG_ERROR << "Postgres getClient: Failed to get DB clients lazily. Name="
                      << dbClientReaderName_;
            cb(std::nullopt);
            return;
        }
    }

    if (!dbClientReader_)
    {
        LOG_ERROR << "Postgres getClient: dbClientReader_ is STILL NULL!";
        cb(std::nullopt);
        return;
    }

    auto sharedCb = std::make_shared<ClientCallback>(std::move(cb));
    try
    {
        Mapper<Oauth2Clients> mapper(dbClientReader_);
        // V035 open platform: soft-deleted clients are invisible to every
        // flow; suspended self-registered clients (oauth2_client_owners)
        // resolve as not-found, which funnels into the existing
        // invalid-client error paths with zero new protocol semantics.
        mapper.findOne(
          Criteria(Oauth2Clients::Cols::_client_id, CompareOperator::EQ, clientId) &&
            Criteria(Oauth2Clients::Cols::_deleted_at, CompareOperator::IsNull),
          [sharedCb, clientId, self = shared_from_this(), this](const Oauth2Clients &row) {
              // V035 open platform: an owners row with status != active
              // suspends a self-registered client; admin-seeded clients have
              // no owners row and stay active. Linear async chain — the
              // owners lookup runs first and `buildClient` continues.
              auto buildClient = [sharedCb, clientId, self = shared_from_this(), this,
                                  row](bool suspended) {
                  if (suspended)
                  {
                      LOG_INFO << "Postgres getClient: client " << clientId
                               << " is suspended (open platform governance)";
                      (*sharedCb)(std::nullopt);
                      return;
                  }
                  OAuth2Client client;
                  client.clientId = row.getValueOfClientId();
                  LOG_DEBUG << "Postgres getClient: Found -> " << client.clientId;

                  std::string clientTypeStr = row.getValueOfClientType();
                  try
                  {
                      client.clientType = stringToClientType(clientTypeStr);
                      LOG_DEBUG << "Postgres getClient: Type -> " << clientTypeStr;
                  }
                  catch (const std::exception &)
                  {
                      LOG_WARN << "Postgres getClient: Invalid client type '" << clientTypeStr
                               << "' for " << client.clientId << ", defaulting to CONFIDENTIAL";
                      client.clientType = ClientType::CONFIDENTIAL;
                  }

                  client.clientSecretHash = row.getValueOfClientSecret();
                  client.salt = row.getValueOfSalt();
                  // F-017: read the declared token-endpoint auth method so the
                  // token/introspect/revoke endpoints can enforce it. Empty (NULL
                  // column) preserves the legacy lenient Basic->body fallback.
                  client.tokenEndpointAuthMethod = row.getValueOfTokenEndpointAuthMethod();

                  std::string uris = row.getValueOfRedirectUris();
                  LOG_DEBUG << "Postgres getClient: Redirect URIs -> " << uris;
                  std::stringstream ss(uris);
                  std::string uri;
                  while (std::getline(ss, uri, ','))
                  {
                      client.redirectUris.push_back(uri);
                  }

                  // #220: comma-joined column, same storage convention as
                  // redirect_uris. Empty (NULL column) stays empty = the token
                  // endpoint's grant gate treats it as unrestricted (legacy rows).
                  std::string grants = row.getValueOfAllowedGrantTypes();
                  if (!grants.empty())
                  {
                      std::stringstream gs(grants);
                      std::string grant;
                      while (std::getline(gs, grant, ','))
                      {
                          client.allowedGrantTypes.push_back(grant);
                      }
                  }

                  // Fetch allowed scopes from oauth2_client_scopes table
                  LOG_DEBUG << "Postgres getClient: Fetching allowed scopes for " << client.clientId;
                  row.getScope(
                    dbClientReader_,
                    [client, sharedCb](
                      const std::vector<std::pair<Oauth2Scopes, Oauth2ClientScopes>> &scopes
                    ) mutable {
                        for (const auto &scopePair : scopes)
                        {
                            const Oauth2Scopes &scope = scopePair.first;
                            client.allowedScopes.push_back(scope.getValueOfName());
                            LOG_DEBUG << "Postgres getClient: Allowed scope -> "
                                      << scope.getValueOfName();
                        }

                        LOG_DEBUG << "Postgres getClient: Total allowed scopes -> "
                                  << client.allowedScopes.size();
                        (*sharedCb)(client);
                    },
                    [sharedCb, clientId](const DrogonDbException &e) {
                        LOG_WARN << "Postgres getClient: Failed to fetch scopes for " << clientId
                                 << ", returning client with empty scopes: " << e.base().what();
                        // Even if scope fetch fails, return the client with empty scopes
                        // This maintains backward compatibility
                        (*sharedCb)(std::nullopt);
                    }
                  );
              };

              try
              {
                  Mapper<Oauth2ClientOwners> ownersMapper(dbClientReader_);
                  ownersMapper.findOne(
                    Criteria(Oauth2ClientOwners::Cols::_client_id, CompareOperator::EQ, clientId),
                    [buildClient](const Oauth2ClientOwners &owner) {
                        buildClient(owner.getValueOfStatus() != "active");
                    },
                    [buildClient, clientId](const DrogonDbException &e) {
                        // Review C3: only "no owners row" means admin-owned
                        // (active). A genuine DB failure must fail CLOSED —
                        // treating it as active lets a suspended client
                        // through on a connection blip.
                        auto *noRows = dynamic_cast<const UnexpectedRows *>(&e);
                        if (noRows != nullptr)
                        {
                            buildClient(false);  // admin-owned, active
                            return;
                        }
                        LOG_ERROR << "Postgres getClient: owners lookup failed for "
                                  << clientId << ", failing closed: " << e.base().what();
                        buildClient(true);  // treat as suspended -> not found
                    }
                  );
              }
              catch (...)
              {
                  // Mapper construction failure is also a real error: fail
                  // closed rather than silently treating the client as active.
                  LOG_ERROR << "Postgres getClient: owners Mapper construction failed for "
                            << clientId << ", failing closed";
                  buildClient(true);
              }
          },
          [sharedCb, clientId](const DrogonDbException &e) {
              LOG_DEBUG << "Postgres getClient: Not found or Error -> " << clientId << " ("
                        << e.base().what() << ")";
              (*sharedCb)(std::nullopt);
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "Postgres getClient Exception";
        (*sharedCb)(std::nullopt);
    }
}

void PostgresClientRepository::validateClient(
  const std::string &clientId,
  const std::string &clientSecret,
  BoolCallback &&cb
)
{
    LOG_DEBUG << "Postgres validateClient: " << clientId;

    // Lazy initialization of DB clients if they are null
    if (!dbClientReader_)
    {
        try
        {
            dbClientMaster_ = ::drogon::app().getDbClient(dbClientName_);
            dbClientReader_ = ::drogon::app().getDbClient(dbClientReaderName_);
            LOG_INFO << "Postgres DB Clients initialized lazily for validateClient";
        }
        catch (...)
        {
            LOG_ERROR << "Postgres validateClient: Failed to get DB clients lazily. Name="
                      << dbClientReaderName_;
            cb(false);
            return;
        }
    }

    if (!dbClientReader_)
    {
        LOG_ERROR << "Postgres validateClient: dbClientReader_ is STILL NULL!";
        cb(false);
        return;
    }

    auto sharedCb = std::make_shared<BoolCallback>(std::move(cb));
    try
    {
        Mapper<Oauth2Clients> mapper(dbClientReader_);

        // First, get client information including type. V035: soft-deleted
        // clients never validate (they are invisible to every flow).
        mapper.findOne(
          Criteria(Oauth2Clients::Cols::_client_id, CompareOperator::EQ, clientId) &&
            Criteria(Oauth2Clients::Cols::_deleted_at, CompareOperator::IsNull),
          [sharedCb, clientId, clientSecret, this](const Oauth2Clients &row) {
              // V035: suspended self-registered clients fail validation;
              // no owners row (admin-owned) continues to the secret check.
              auto checkSecret = [sharedCb, clientId, clientSecret, row](bool suspended) {
                  if (suspended)
                  {
                      LOG_INFO << "Postgres validateClient: client " << clientId
                               << " is suspended (open platform governance)";
                      (*sharedCb)(false);
                      return;
                  }
                  // Get client type
                  std::string clientTypeStr = row.getValueOfClientType();
                  ClientType clientType = ClientType::CONFIDENTIAL;  // Default fallback
                  try
                  {
                      clientType = stringToClientType(clientTypeStr);
                  }
                  catch (const std::exception &)
                  {
                      LOG_WARN << "Postgres validateClient: Invalid client type '" << clientTypeStr
                               << "' for " << clientId << ", defaulting to CONFIDENTIAL";
                  }

                  // PUBLIC clients skip secret validation
                  if (clientType == ClientType::PUBLIC)
                  {
                      LOG_DEBUG << "Postgres validateClient: PUBLIC client " << clientId
                                << " accepted without secret";
                      (*sharedCb)(true);
                      return;
                  }

                  // CONFIDENTIAL clients MUST validate secret
                  if (clientSecret.empty())
                  {
                      LOG_WARN << "Postgres validateClient: CONFIDENTIAL client " << clientId
                               << " missing secret";
                      (*sharedCb)(false);
                      return;
                  }

                  // Constant-time secret comparison to prevent timing attacks
                  std::string storedHash = row.getValueOfClientSecret();
                  std::string salt = row.getValueOfSalt();
                  std::string computedHash = ::drogon::utils::getSha256(clientSecret + salt);

                  LOG_DEBUG << "Postgres validateClient: Verifying secret for " << clientId;

                  // Normalize both to lowercase for case-insensitive hex comparison
                  std::transform(
                    computedHash.begin(), computedHash.end(), computedHash.begin(), ::tolower
                  );
                  std::string storedLower = storedHash;
                  std::transform(
                    storedLower.begin(), storedLower.end(), storedLower.begin(), ::tolower
                  );

                  // Use constant-time comparison to prevent timing attacks
                  size_t cmpLen = (computedHash.length() < storedLower.length()) ? computedHash.length()
                                                                                 : storedLower.length();
                  bool match =
                    (constantTimeMemcmp(computedHash.c_str(), storedLower.c_str(), cmpLen) == 0) &&
                    computedHash.length() == storedLower.length();

                  if (!match)
                  {
                      LOG_WARN << "Postgres validateClient: Secret MISMATCH for client " << clientId;
                  }

                  // Review MINOR #3: do not LOG_DEBUG the match result -- the
                  // Redis path dropped this line in F-004 for parity (the result
                  // is already observable via the HTTP status, so this is a
                  // uniformity fix, not a timing-side-channel fix).
                  (*sharedCb)(match);
              };
              try
              {
                  Mapper<Oauth2ClientOwners> ownersMapper(dbClientReader_);
                  ownersMapper.findOne(
                    Criteria(Oauth2ClientOwners::Cols::_client_id, CompareOperator::EQ, clientId),
                    [checkSecret](const Oauth2ClientOwners &owner) {
                        checkSecret(owner.getValueOfStatus() != "active");
                    },
                    [checkSecret, clientId](const DrogonDbException &e) {
                        // Review C3: fail CLOSED on real DB errors (see the
                        // getClient twin above for the rationale).
                        auto *noRows = dynamic_cast<const UnexpectedRows *>(&e);
                        if (noRows != nullptr)
                        {
                            checkSecret(false);  // admin-owned, active
                            return;
                        }
                        LOG_ERROR << "Postgres validateClient: owners lookup failed for "
                                  << clientId << ", failing closed: " << e.base().what();
                        checkSecret(true);  // treat as suspended -> reject
                    }
                  );
              }
              catch (...)
              {
                  LOG_ERROR << "Postgres validateClient: owners Mapper construction failed for "
                            << clientId << ", failing closed";
                  checkSecret(true);
              }
          },
          [sharedCb, clientId](const DrogonDbException &e) {
              LOG_ERROR << "Postgres validateClient Error (Database Exception) for " << clientId
                        << ": " << e.base().what();
              (*sharedCb)(false);
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "Postgres validateClient Exception: " << e.what();
        (*sharedCb)(false);
    }
    catch (...)
    {
        LOG_ERROR << "Postgres validateClient Unknown Exception";
        (*sharedCb)(false);
    }
}

}  // namespace fulla::storage::postgres
