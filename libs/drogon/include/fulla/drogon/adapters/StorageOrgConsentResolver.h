#pragma once

// v1.5.0 M2 (real-tenant design §2.2, R-M2-1): adapter implementing the
// common IOrgConsentResolver port for the assembled server, over the
// shared OrgConsentRepository (storage-postgres). Mirrors
// StorageOrgContextResolver (M1): constructed by the composition root
// (OAuth2Plugin) with a storage-type-guarded DbClient, so memory-mode
// deployments pass nullptr and the consent UNION simply degrades to the
// personal-only decision (no org rows exist there). The org tables'
// only home is Postgres regardless of the oauth2 storage type -- a
// redis-storage deployment still resolves org consents through here.

#include <fulla/common/ports/IOrgConsentResolver.h>

#include <drogon/orm/DbClient.h>

#include <memory>
#include <string>

namespace fulla::drogon::adapters
{

class StorageOrgConsentResolver : public fulla::common::ports::IOrgConsentResolver
{
  public:
    /// `dbClient` may be nullptr (memory-mode deployments: every lookup
    /// resolves to false).
    explicit StorageOrgConsentResolver(::drogon::orm::DbClientPtr dbClient)
        : dbClient_(std::move(dbClient))
    {
    }

    void hasOrgConsentForUser(
      int32_t internalUserId,
      const std::string &clientId,
      const std::string &scope,
      fulla::common::ports::IOrgConsentResolver::BoolCallback &&cb
    ) override;

  private:
    ::drogon::orm::DbClientPtr dbClient_;
};

}  // namespace fulla::drogon::adapters
