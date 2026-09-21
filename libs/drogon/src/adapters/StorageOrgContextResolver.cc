// See StorageOrgContextResolver.h for the wiring rationale (v1.5.0 M1).
// All hops are async callback + Mapper + Criteria via the shared
// repository; every failure resolves to nullopt (org_ctx is claim data:
// its failure mode is absence, never a failed request).

#include <fulla/drogon/adapters/StorageOrgContextResolver.h>

#include <fulla/storage/postgres/ClientOwnersRepository.h>

#include <drogon/drogon.h>

#include <memory>
#include <string>

namespace fulla::drogon::adapters
{

void StorageOrgContextResolver::resolveOrgContext(
  const std::string &subject,
  int32_t orgId,
  fulla::common::ports::IOrgContextResolver::OrgContextCallback &&cb
)
{
    using fulla::common::ports::OrgContextInfo;

    auto sharedCb =
      std::make_shared<fulla::common::ports::IOrgContextResolver::OrgContextCallback>(
        std::move(cb)
      );

    if (!dbClient_)
    {
        // Memory mode / unwired: no org rows, no org context.
        (*sharedCb)(std::nullopt);
        return;
    }

    auto repo = std::make_shared<::fulla::storage::postgres::ClientOwnersRepository>(dbClient_);
    repo->findUserIdBySubject(
      subject,
      [repo, sharedCb, orgId](std::optional<int32_t> internalUserId) {
          if (!internalUserId.has_value())
          {
              (*sharedCb)(std::nullopt);
              return;
          }
          using ::fulla::storage::postgres::LookupStatus;
          repo->findOrganization(
            std::to_string(orgId),
            [repo, sharedCb, orgId, internalUserId](
              const ::fulla::storage::postgres::OrganizationLookup &orgLookup) {
                if (orgLookup.status != LookupStatus::Found)
                {
                    (*sharedCb)(std::nullopt);
                    return;
                }
                // O7: CURRENT membership only -- a removed member's token
                // stops carrying org_ctx on its next claim resolution.
                repo->findMembership(
                  orgId,
                  *internalUserId,
                  [sharedCb, orgLookup](
                    const ::fulla::storage::postgres::MembershipLookup &m) {
                      if (m.status != LookupStatus::Found)
                      {
                          (*sharedCb)(std::nullopt);
                          return;
                      }
                      OrgContextInfo info;
                      info.orgName = orgLookup.row.getValueOfName();
                      info.roles = {m.row.getValueOfRole()};
                      (*sharedCb)(info);
                  }
                );
            }
          );
      }
    );
}

}  // namespace fulla::drogon::adapters
