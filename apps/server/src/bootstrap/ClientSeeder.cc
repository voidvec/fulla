#include "ClientSeeder.h"

#include <fulla/drogon/utils/CryptoUtils.h>
#include <drogon/drogon.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <sstream>

using namespace drogon;
using namespace drogon::orm;

namespace bootstrap
{
namespace
{
struct RunState
{
    std::atomic<int> pending{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> finished{false};
    ClientSeeder::DoneCallback done;
    std::string firstError;

    void finishOne()
    {
        if (--pending == 0 && !finished.exchange(true))
        {
            done(!failed.load(), failed ? firstError : "config clients seeded/verified");
        }
    }
};

// Config accepted shapes mirror MemoryClientRepository::initFromConfig:
// client_type ("type" tolerated), redirect_uri string-or-array,
// allowed_scopes string-or-array.
bool isPublicClient(const Json::Value &c)
{
    std::string type = c.get("client_type", c.get("type", "CONFIDENTIAL")).asString();
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    if (type == "PUBLIC")
        return true;
    if (c.isMember("token_endpoint_auth_method"))
        return c["token_endpoint_auth_method"].asString() == "none";
    return false;
}

std::string redirectUrisCsv(const Json::Value &c)
{
    if (c.isMember("redirect_uri"))
    {
        const Json::Value &r = c["redirect_uri"];
        if (r.isArray())
        {
            std::string joined;
            for (const auto &uri : r)
            {
                if (!joined.empty())
                    joined += ",";
                joined += uri.asString();
            }
            return joined;
        }
        return r.asString();
    }
    return {};
}

}  // namespace

void ClientSeeder::run(const Json::Value &clientsConfig, DoneCallback &&done)
{
    auto db = app().getDbClient();
    if (!db)
    {
        done(false, "no db client (memory storage?)");
        return;
    }
    if (!clientsConfig.isObject() || clientsConfig.empty())
    {
        done(true, "no config clients declared");
        return;
    }

    auto run = std::make_shared<RunState>();
    run->done = std::move(done);

    const auto memberNames = clientsConfig.getMemberNames();
    run->pending = static_cast<int>(memberNames.size());

    for (const auto &clientId : memberNames)
    {
        const Json::Value &c = clientsConfig[clientId];
        const bool publicClient = isPublicClient(c);

        std::string secret;
        std::string salt;
        if (publicClient)
        {
            // NOT NULL columns, unused under token_endpoint_auth_method=none:
            // store a hash of a random throwaway so no known constant ships.
            salt = fulla::drogon::utils::generateSecureToken(16);
            secret = fulla::drogon::utils::hashClientSecretWithSalt(
              fulla::drogon::utils::generateSecureToken(32), salt);
        }
        else
        {
            salt = fulla::drogon::utils::generateSecureToken(16);
            secret = fulla::drogon::utils::hashClientSecretWithSalt(
              c.get("secret", "").asString(), salt);
        }

        auto afterClientRow =
          [run, db, clientId, c](bool clientRowReady) mutable {
              if (!clientRowReady)
              {
                  run->finishOne();
                  return;
              }
              const Json::Value scopes = c.isMember("allowed_scopes")
                                           ? c["allowed_scopes"]
                                           : Json::Value(Json::arrayValue);
              std::vector<std::string> scopeNames;
              if (scopes.isString())
              {
                  std::istringstream iss(scopes.asString());
                  std::string tok;
                  while (std::getline(iss, tok, ','))
                  {
                      if (!tok.empty())
                          scopeNames.push_back(tok);
                  }
              }
              else if (scopes.isArray())
              {
                  for (const auto &sc : scopes)
                      scopeNames.push_back(sc.asString());
              }
              if (scopeNames.empty())
              {
                  run->finishOne();
                  return;
              }
              run->pending += static_cast<int>(scopeNames.size());
              // The client-insert task itself completes here; the scope
              // grants registered above account for their own completions.
              run->finishOne();
              for (const auto &scope : scopeNames)
              {
                  try
                  {
                      db->execSqlAsync(
                        "INSERT INTO oauth2_client_scopes (client_id, scope_name) "
                        "SELECT $1, name FROM oauth2_scopes WHERE name = $2 "
                        "ON CONFLICT (client_id, scope_name) DO NOTHING",
                        [run](const Result &) { run->finishOne(); },
                        [run, clientId, scope](const DrogonDbException &e) {
                            LOG_ERROR << "ClientSeeder: scope grant failed for "
                                      << clientId << "/" << scope << ": "
                                      << e.base().what();
                            if (!run->failed.exchange(true))
                                run->firstError = std::string("scope grant failed: ")
                                                 + e.base().what();
                            run->finishOne();
                        },
                        clientId,
                        scope);
                  }
                  catch (...)
                  {
                      LOG_ERROR << "ClientSeeder: scope grant threw for " << clientId;
                      if (!run->failed.exchange(true))
                          run->firstError = "scope grant threw";
                      run->finishOne();
                  }
              }
          };

        try
        {
            db->execSqlAsync(
              "INSERT INTO oauth2_clients (client_id, client_type, client_secret, "
              "salt, name, redirect_uris, allowed_grant_types, token_endpoint_auth_method) "
              "VALUES ($1, $2, $3, $4, $5, $6, $7, $8) "
              "ON CONFLICT (client_id) DO NOTHING",
              [run, afterClientRow, clientId](const Result &r) mutable {
                  if (r.affectedRows() > 0)
                      LOG_INFO << "ClientSeeder: seeded client '" << clientId << "'";
                  else
                      LOG_INFO << "ClientSeeder: client '" << clientId
                               << "' already present, left untouched";
                  afterClientRow(true);
              },
              [run, afterClientRow, clientId](const DrogonDbException &e) mutable {
                  LOG_ERROR << "ClientSeeder: client insert failed for " << clientId
                            << ": " << e.base().what();
                  if (!run->failed.exchange(true))
                      run->firstError = std::string("client insert failed: ")
                                       + e.base().what();
                  afterClientRow(false);
              },
              clientId,
              publicClient ? std::string("PUBLIC") : std::string("CONFIDENTIAL"),
              secret,
              salt,
              c.get("name", clientId).asString(),
              redirectUrisCsv(c),
              std::string("authorization_code,refresh_token"),
              publicClient ? std::string("none") : std::string("client_secret_basic"));
        }
        catch (...)
        {
            LOG_ERROR << "ClientSeeder: client insert threw for " << clientId;
            if (!run->failed.exchange(true))
                run->firstError = "client insert threw";
            run->finishOne();
        }
    }
}
}  // namespace bootstrap
