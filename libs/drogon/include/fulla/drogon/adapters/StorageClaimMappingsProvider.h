#pragma once

// P1-3 (#280): Adapter-side IClaimMappingsProvider + the user-profile
// JSON resolver backing TokenService's mapped claims.
//
// Postgres-backed (the claim_mappings table is a Postgres concern
// regardless of the oauth2 storage type, same as the org-context
// resolver); memory deployments wire NOTHING (TokenService's unset
// provider keeps the pre-P1-3 claim set byte-for-byte).
//
// The profile resolver answers users.profile_json — the row's
// display_name + custom_attributes composed into one document for the
// Domain-layer scalar source-path walk.

#include <fulla/common/ports/IClaimMappingsProvider.h>

#include <drogon/drogon.h>

#include <functional>
#include <memory>
#include <string>

namespace fulla::drogon::adapters
{

class StorageClaimMappingsProvider : public fulla::common::ports::IClaimMappingsProvider
{
  public:
    explicit StorageClaimMappingsProvider(::drogon::orm::DbClientPtr dbClient);
    ~StorageClaimMappingsProvider() override;

    void getEnabledMappings(MappingsCallback &&cb) override;

  private:
    ::drogon::orm::DbClientPtr dbClient_;
};

/// The users-row profile JSON for claim resolution ({display_name?,
/// custom_attributes}) keyed by the token subject. `cb` receives
/// nullopt on any failure (mapped claims then resolve nothing).
using ProfileJsonCallback = std::function<void(std::optional<std::string>)>;
void resolveUserProfileJson(
  const ::drogon::orm::DbClientPtr &db,
  const std::string &subject,
  ProfileJsonCallback &&cb
);

}  // namespace fulla::drogon::adapters
