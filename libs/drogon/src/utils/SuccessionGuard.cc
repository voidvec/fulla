// See SuccessionGuard.h for the R-M3-4 rationale. The per-org walk is
// sequential (an owner's orgs number 1-3 by quota); every hop routes
// failures to the caller's done() exactly once.

#include <fulla/drogon/utils/SuccessionGuard.h>

#include <fulla/drogon/adapters/DrogonAuditSink.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/storage/postgres/OrgSuccessionRepository.h>

#include <drogon/drogon.h>

#include <memory>
#include <optional>
#include <vector>

namespace fulla::drogon::utils
{

namespace
{

using ::fulla::storage::postgres::OrgSuccessionRepository;
using SuccessionModel = ::drogon_model::fulla_db::OrganizationSuccessionNominations;

void processNextOrg(
  const std::shared_ptr<OrgSuccessionRepository> &repos,
  const std::shared_ptr<std::vector<int32_t>> &orgIds,
  size_t index,
  const ::drogon::HttpRequestPtr &req,
  const std::shared_ptr<std::function<void(bool, const std::string &)>> &done
)
{
    if (index >= orgIds->size())
    {
        (*done)(true, "");
        return;
    }
    const int32_t orgId = (*orgIds)[index];
    repos->findPending(
      orgId,
      [repos, orgIds, index, req, done, orgId](const std::optional<SuccessionModel> &pending) {
          if (!pending.has_value())
          {
              // No pending nomination for this org: the admin-takeover
              // state is the documented fallback (#228); nothing to do.
              processNextOrg(repos, orgIds, index + 1, req, done);
              return;
          }
          const int32_t nominee = pending->getValueOfNomineeUserId();
          // A soft-deleted nominee must not take the seat (it would
          // recreate the freeze); skip -- this org falls to admin
          // takeover as well.
          repos->isLiveUser(
            nominee,
            [repos, orgIds, index, req, done, orgId, nominee](bool live) {
                if (!live)
                {
                    LOG_WARN << "[SuccessionGuard] pending nomination names a "
                                "soft-deleted nominee for org "
                             << orgId << "; leaving admin-takeover state";
                    processNextOrg(repos, orgIds, index + 1, req, done);
                    return;
                }
                repos->effectSuccession(
                  orgId,
                  nominee,
                  [repos, orgIds, index, req, done, orgId, nominee](bool ok) {
                      if (!ok)
                      {
                          (*done)(false, "organization " + std::to_string(orgId));
                          return;
                      }
                      if (auto *plugin = ::drogon::app().getPlugin<::OAuth2Plugin>())
                      {
                          ::fulla::drogon::adapters::DrogonAuditSink::logFromRequest(
                            plugin->getAuditSink(),
                            "org_successor_auto_effected",
                            "success",
                            req,
                            "",
                            "organization",
                            std::to_string(orgId)
                          );
                      }
                      LOG_INFO << "[SuccessionGuard] auto-effected succession for org "
                               << orgId << " -> user " << nominee;
                      processNextOrg(repos, orgIds, index + 1, req, done);
                  }
                );
            }
          );
      }
    );
}

}  // namespace

void effectPendingSuccessionsForUser(
  const ::drogon::orm::DbClientPtr &db,
  int32_t userId,
  const ::drogon::HttpRequestPtr &req,
  std::function<void(bool, const std::string &)> &&done
)
{
    auto sharedDone =
      std::make_shared<std::function<void(bool, const std::string &)>>(std::move(done));

    if (!db)
    {
        (*sharedDone)(false, "no database client");
        return;
    }

    auto repos = std::make_shared<OrgSuccessionRepository>(db);
    repos->findOrgIdsWithPendingForOwner(
      userId,
      [repos, sharedDone, req](const std::vector<int32_t> &orgIds) {
          if (orgIds.empty())
          {
              (*sharedDone)(true, "");
              return;
          }
          auto ids = std::make_shared<std::vector<int32_t>>(orgIds);
          processNextOrg(repos, ids, 0, req, sharedDone);
      }
    );
}

}  // namespace fulla::drogon::utils
