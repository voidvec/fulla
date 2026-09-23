// See StorageOrgConsentResolver.h for the wiring rationale (v1.5.0 M2).
// The repository owns the queries (async callback + Mapper + Criteria);
// every failure resolves to false (the union's failure mode is "prompt
// the user", mirroring IConsentRepository's hasUserConsent error path).

#include <fulla/drogon/adapters/StorageOrgConsentResolver.h>

#include <fulla/storage/postgres/OrgConsentRepository.h>

#include <memory>

namespace fulla::drogon::adapters
{

void StorageOrgConsentResolver::hasOrgConsentForUser(
  int32_t internalUserId,
  const std::string &clientId,
  const std::string &scope,
  fulla::common::ports::IOrgConsentResolver::BoolCallback &&cb
)
{
    auto sharedCb =
      std::make_shared<fulla::common::ports::IOrgConsentResolver::BoolCallback>(std::move(cb));

    if (!dbClient_)
    {
        // Memory mode / unwired: no org rows, no org consents.
        (*sharedCb)(false);
        return;
    }

    auto repo = std::make_shared<::fulla::storage::postgres::OrgConsentRepository>(dbClient_);
    repo->hasActiveConsentForUser(
      internalUserId,
      clientId,
      scope,
      [repo, sharedCb](bool hasConsent) { (*sharedCb)(hasConsent); }
    );
}

}  // namespace fulla::drogon::adapters
