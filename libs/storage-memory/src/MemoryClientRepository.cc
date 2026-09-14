#include <fulla/storage/memory/MemoryClientRepository.h>
#include <fulla/common/utils/ConstantTimeCompare.h>
#include <drogon/drogon.h>

namespace fulla::storage::memory
{

// F-004: constant-time comparison now comes from the shared
// fulla::common::utils::constantTimeMemcmp (previously a verbatim
// anonymous-namespace copy lived here and in the Postgres/Redis backends).
using ::fulla::common::utils::constantTimeMemcmp;

// Task 27.5: callback type aliases now live on the new base interface
// (fulla::oauth2::repository::IClientRepository); bring them into scope
// for the out-of-class method definitions below.
using ClientCallback = IClientRepositoryBase::ClientCallback;
using BoolCallback = IClientRepositoryBase::BoolCallback;

void MemoryClientRepository::initFromConfig(const Json::Value &clientsConfig)
{
    if (clientsConfig.isNull() || !clientsConfig.isObject())
    {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const auto &clientId : clientsConfig.getMemberNames())
    {
        const auto &clientData = clientsConfig[clientId];
        ::fulla::oauth2::model::OAuth2Client client;
        client.clientId = clientId;

        // Parse client type. The canonical config key is "client_type" (every
        // apps/server/config/*.json uses it); bare "type" stays accepted for
        // pre-canonical configs. Surfaced by #69's refresh-grant e2e: with
        // the old single-"type" read, a configured PUBLIC fulla-portal silently
        // defaulted to CONFIDENTIAL in memory mode and rejected public-style
        // (empty-secret) token requests with invalid_client.
        std::string clientTypeStr =
          clientData.get("client_type", clientData.get("type", "CONFIDENTIAL").asString())
            .asString();
        try
        {
            client.clientType = ::fulla::oauth2::model::stringToClientType(clientTypeStr);
        }
        catch (const std::exception &)
        {
            LOG_WARN << "MemoryClientRepository: Invalid client type '" << clientTypeStr << "' for "
                     << clientId << ", defaulting to CONFIDENTIAL";
            client.clientType = ::fulla::oauth2::model::ClientType::CONFIDENTIAL;
        }

        // In memory mode, we store plain text or whatever provided as "secret"
        // Ideally we should hash it here too if we want parity, but for memory
        // it's fine.
        client.clientSecretHash = clientData.get("secret", "").asString();

        // Handle redirect_uri (single or array)
        if (clientData["redirect_uri"].isArray())
        {
            for (const auto &uri : clientData["redirect_uri"])
            {
                client.redirectUris.push_back(uri.asString());
            }
        }
        else if (clientData["redirect_uri"].isString())
        {
            client.redirectUris.push_back(clientData["redirect_uri"].asString());
        }

        // Handle allowed_scopes (single or array)
        if (clientData["allowed_scopes"].isArray())
        {
            for (const auto &scope : clientData["allowed_scopes"])
            {
                client.allowedScopes.push_back(scope.asString());
            }
        }
        else if (clientData["allowed_scopes"].isString())
        {
            client.allowedScopes.push_back(clientData["allowed_scopes"].asString());
        }
        // If no allowed_scopes specified, add default scopes for backward compatibility
        else if (clientId == "fulla-portal")
        {
            client.allowedScopes.push_back("openid");
            client.allowedScopes.push_back("profile");
            client.allowedScopes.push_back("email");
            LOG_DEBUG << "MemoryClientRepository: Added default scopes for fulla-portal";
        }

        LOG_DEBUG << "MemoryClientRepository: Loaded client " << clientId << " with "
                  << client.allowedScopes.size() << " allowed scopes";

        clients_[clientId] = client;
    }
}

void MemoryClientRepository::getClient(const std::string &clientId, ClientCallback &&cb)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = clients_.find(clientId);
    if (it != clients_.end())
    {
        cb(it->second);
    }
    else
    {
        cb(std::nullopt);
    }
}

void MemoryClientRepository::validateClient(
  const std::string &clientId,
  const std::string &clientSecret,
  BoolCallback &&cb
)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = clients_.find(clientId);
    if (it == clients_.end())
    {
        LOG_DEBUG << "MemoryClientRepository validateClient: Client not found - " << clientId;
        cb(false);
        return;
    }

    const auto &client = it->second;

    // PUBLIC clients skip secret validation
    if (client.clientType == ::fulla::oauth2::model::ClientType::PUBLIC)
    {
        LOG_DEBUG << "MemoryClientRepository validateClient: PUBLIC client " << clientId
                  << " accepted without secret";
        cb(true);
        return;
    }

    // CONFIDENTIAL clients MUST validate secret
    if (clientSecret.empty())
    {
        LOG_WARN << "MemoryClientRepository validateClient: CONFIDENTIAL client " << clientId
                 << " missing secret";
        cb(false);
        return;
    }

    // Constant-time comparison to prevent timing attacks
    const std::string &storedHash = client.clientSecretHash;
    size_t cmpLen =
      (clientSecret.length() < storedHash.length()) ? clientSecret.length() : storedHash.length();
    bool valid = (constantTimeMemcmp(clientSecret.c_str(), storedHash.c_str(), cmpLen) == 0) &&
                 clientSecret.length() == storedHash.length();

    // Review MINOR #3 (round 2): no match-result LOG_DEBUG -- the Postgres
    // and Redis paths removed theirs for parity (the result is already
    // observable via the HTTP status; match-result logging is pure noise).
    cb(valid);
}

}  // namespace fulla::storage::memory
