#pragma once

#include <functional>
#include <json/json.h>
#include <string>

namespace bootstrap
{
/**
 * #204: startup seeding of the OAuth2 clients declared in
 * plugins[name=OAuth2Plugin].config.clients into the oauth2_clients /
 * oauth2_client_scopes tables.
 *
 * Production previously had no path from config to the client table (the
 * dev seed SQL is explicitly DEV-ONLY), so a freshly deployed server passed
 * the credential and email-verified checks at login and then failed code
 * issuance with VALIDATION_INVALID_INPUT (3001) because the client row did
 * not exist — a failure that logs nothing at ERROR level.
 *
 * Semantics:
 *   * INSERT ... ON CONFLICT (client_id) DO NOTHING — config seeds the
 *     initial state; rows created or edited through the admin client API
 *     win and are never overwritten by a restart.
 *   * PUBLIC clients (token_endpoint_auth_method 'none') get a random
 *     throwaway secret hash + salt: both columns are NOT NULL but unused
 *     for the none auth method.
 *   * CONFIDENTIAL clients hash the config plaintext per the F-002 rule
 *     (random salt first, lowercase hex sha256(secret + salt)).
 *   * allowed_scopes become oauth2_client_scopes grant rows (only for
 *     scope names present in oauth2_scopes — same contract as the seed SQL).
 *   * Memory-storage deployments already init clients from config
 *     in-process (MemoryRepositoryBundle::initFromConfig); this seeder is
 *     for the postgres path.
 *
 * Idempotent and safe to call at every startup; deliberately direct-
 * callable so integration tests can exercise it in isolation.
 */
class ClientSeeder
{
  public:
    using DoneCallback = std::function<void(bool ok, const std::string &detail)>;

    /**
     * @param clientsConfig  the "clients" object of the OAuth2Plugin config,
     *                       keyed by client_id; empty/absent -> no-op.
     */
    static void run(const Json::Value &clientsConfig, DoneCallback &&done);
};
}  // namespace bootstrap
