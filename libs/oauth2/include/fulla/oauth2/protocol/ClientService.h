#pragma once

// Task 17 remainder (fulla-sdk-refactor, design.md §6/§8 "protocol/"):
// Domain-layer ClientService, ported from
// OAuth2Plugin/include/oauth2/services/ClientService.h onto the new
// fulla::oauth2::repository::IClientRepository (Task 17 slice 3)
// instead of the old god interface oauth2::IOAuth2Storage. Behavior is
// unchanged (same three methods, same semantics); only the storage
// dependency type changed, which is exactly the point of this migration
// (design.md §4.1: libs/oauth2 must not depend on OAuth2Plugin).
//
// Not yet wired into production (OAuth2Plugin still uses its own
// oauth2::ClientService against the old IOAuth2Storage) -- that wiring is
// Task 24 (apps/server assembly), deferred until libs/identity's
// remaining services are filled in. This class is additive and
// independently unit-tested (libs/oauth2/test).

#include <fulla/oauth2/model/Client.h>
#include <fulla/oauth2/repository/IClientRepository.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fulla::oauth2::protocol
{

class ClientService
{
  public:
    explicit ClientService(
      std::shared_ptr<fulla::oauth2::repository::IClientRepository> clients
    )
        : clients_(std::move(clients))
    {
    }

    /// Validate client credentials. Original: oauth2::ClientService::validateClient.
    void validateClient(
      const std::string &clientId,
      const std::string &clientSecret,
      std::function<void(bool)> &&callback
    );

    /// Validate that `redirectUri` is exactly one of the client's
    /// registered redirect URIs. Original:
    /// oauth2::ClientService::validateRedirectUri.
    void validateRedirectUri(
      const std::string &clientId,
      const std::string &redirectUri,
      std::function<void(bool)> &&callback
    );

    /// P0-7b (#261, OIDC RP-Initiated Logout §2.3): validate a
    /// post_logout_redirect_uri. A non-empty dedicated
    /// post_logout_redirect_uris registration is AUTHORITATIVE (a URI valid
    /// for login but not registered for logout is rejected); an empty one
    /// (unmigrated client) falls back to the login redirect_uris — the
    /// pre-v1.5.0 behavior.
    void validatePostLogoutRedirectUri(
      const std::string &clientId,
      const std::string &redirectUri,
      std::function<void(bool)> &&callback
    );

    /// Validate that every requested scope is in the client's allowlist
    /// (Tier 1). On failure, the error message lists every disallowed
    /// scope (matches the original's comma-joined message format).
    /// Original: oauth2::ClientService::validateClientScopes.
    void validateClientScopes(
      const std::string &clientId,
      const std::vector<std::string> &requestedScopes,
      std::function<void(bool, std::string)> &&callback
    );

  private:
    std::shared_ptr<fulla::oauth2::repository::IClientRepository> clients_;
};

}  // namespace fulla::oauth2::protocol
