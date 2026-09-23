// See OrgSuccessionRepository.h for the design anchors (v1.5.0 M3 §1.3
// item 2, R-M3-2/3/4). Every hop is async callback + Mapper + Criteria;
// the two raw statements (partial-index upsert, demotion batch UPDATE)
// carry their exemption rationale inline.

#include <fulla/storage/postgres/OrgSuccessionRepository.h>

#include <fulla/storage/postgres/AdvisoryLock.h>
#include <fulla/storage/postgres/models/OrganizationMembers.h>
#include <fulla/storage/postgres/models/Users.h>

#include <drogon/drogon.h>

#include <memory>

namespace fulla::storage::postgres
{

using namespace ::drogon::orm;
using namespace ::drogon_model::fulla_db;

namespace
{

// Steps 2+3 of the seat swap, shared by both step-1 branches (promote an
// existing member vs create the owner seat). A plain lambda captured BY
// COPY -- deliberately not a self-referencing shared_ptr helper (that
// pattern leaks the capture set and, with a transaction in it, would
// keep the transaction alive forever; see the replaceClientScopes note
// in ApplicationService.cc).
void demoteAndMark(
  const std::shared_ptr<Transaction> &txn,
  int32_t orgId,
  int32_t nomineeUserId,
  const std::shared_ptr<OrgSuccessionRepository::BoolCallback> &sharedCb
)
{
    // Step 2 (R-M3-3): demote every other owner row to admin. Documented
    // batch-UPDATE exemption (same shape as the #228 transfer): the
    // owner set is service-managed and the WHERE is fully scoped. Unlike
    // #228 this runs INSIDE the seat-swap transaction -- ordering does
    // not matter because the whole sequence is atomic.
    txn->execSqlAsync(
      "UPDATE organization_members SET role = 'admin' "
      "WHERE organization_id = $1 AND role = 'owner' AND user_id <> $2",
      [txn, orgId, sharedCb](const Result &) {
          // Step 3: mark the nomination accepted. The accepted_at IS
          // NULL predicate is the optimistic guard against a concurrent
          // acceptance -- zero affected rows means someone else took the
          // seat first and this transaction rolls back whole.
          txn->execSqlAsync(
            "UPDATE organization_succession_nominations SET accepted_at = "
            "CURRENT_TIMESTAMP WHERE organization_id = $1 AND accepted_at IS NULL",
            [sharedCb](const Result &r) {
                if (r.affectedRows() == 0)
                {
                    LOG_WARN << "effectSuccession: nomination no longer pending "
                                "(concurrent acceptance?); rolling back";
                    (*sharedCb)(false);
                }
                // Success: the caller's callback fires from the commit
                // callback registered by effectSuccession.
            },
            [sharedCb](const DrogonDbException &e) {
                LOG_ERROR << "effectSuccession nomination mark failed: " << e.base().what();
                (*sharedCb)(false);
            },
            orgId
          );
      },
      [sharedCb](const DrogonDbException &e) {
          LOG_ERROR << "effectSuccession demotion failed: " << e.base().what();
          (*sharedCb)(false);
      },
      orgId,
      nomineeUserId
    );
}

}  // namespace

OrgSuccessionRepository::OrgSuccessionRepository(::drogon::orm::DbClientPtr dbClient)
    : dbClient_(std::move(dbClient))
{
}

void OrgSuccessionRepository::upsertNomination(
  int32_t orgId,
  int32_t nomineeUserId,
  int32_t nominatedBy,
  BoolCallback &&cb
)
{
    auto sharedCb = std::make_shared<BoolCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(false);
        return;
    }

    // INSERT ... ON CONFLICT exemption: the conflict target is V037's
    // PARTIAL unique index (pending rows only), which Mapper cannot
    // express. The predicate mirrors the index: a conflicting row is by
    // definition pending, and the DO UPDATE replaces it (R-M3-3: a new
    // nomination overwrites the previous pending one, idempotently).
    // Accepted nominations (accepted_at set) are outside the index and
    // never conflict -- a fresh pending row is inserted for them.
    dbClient_->execSqlAsync(
      "INSERT INTO organization_succession_nominations "
      "(organization_id, nominee_user_id, nominated_by, created_at, accepted_at) "
      "VALUES ($1, $2, $3, CURRENT_TIMESTAMP, NULL) "
      "ON CONFLICT (organization_id) WHERE accepted_at IS NULL DO UPDATE SET "
      "nominee_user_id = EXCLUDED.nominee_user_id, "
      "nominated_by = EXCLUDED.nominated_by, "
      "created_at = CURRENT_TIMESTAMP",
      [sharedCb](const Result &) { (*sharedCb)(true); },
      [sharedCb](const DrogonDbException &e) {
          LOG_ERROR << "upsertNomination failed: " << e.base().what();
          (*sharedCb)(false);
      },
      orgId,
      nomineeUserId,
      nominatedBy
    );
}

void OrgSuccessionRepository::isLiveUser(int32_t userId, BoolCallback &&cb)
{
    auto sharedCb = std::make_shared<BoolCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(false);
        return;
    }

    try
    {
        Mapper<Users> mapper(dbClient_);
        mapper.findBy(
          Criteria(Users::Cols::_id, CompareOperator::EQ, userId) &&
            Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull),
          [sharedCb](const std::vector<Users> &rows) { (*sharedCb)(!rows.empty()); },
          [sharedCb](const DrogonDbException &e) {
              LOG_ERROR << "isLiveUser failed: " << e.base().what();
              (*sharedCb)(false);
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "isLiveUser Mapper construction failed";
        (*sharedCb)(false);
    }
}

void OrgSuccessionRepository::findPending(int32_t orgId, RowOptCallback &&cb)
{
    auto sharedCb = std::make_shared<RowOptCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(std::nullopt);
        return;
    }

    try
    {
        Mapper<OrganizationSuccessionNominations> mapper(dbClient_);
        mapper.findBy(
          Criteria(
            OrganizationSuccessionNominations::Cols::_organization_id,
            CompareOperator::EQ,
            orgId
          ) &&
            Criteria(
              OrganizationSuccessionNominations::Cols::_accepted_at,
              CompareOperator::IsNull
            ),
          [sharedCb](const std::vector<OrganizationSuccessionNominations> &rows) {
              if (rows.empty())
                  (*sharedCb)(std::nullopt);
              else
                  (*sharedCb)(rows[0]);
          },
          [sharedCb](const DrogonDbException &e) {
              LOG_ERROR << "findPending failed: " << e.base().what();
              (*sharedCb)(std::nullopt);
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "findPending Mapper construction failed";
        (*sharedCb)(std::nullopt);
    }
}

void OrgSuccessionRepository::findPendingForOrgs(
  const std::vector<int32_t> &orgIds,
  RowsCallback &&cb
)
{
    auto sharedCb = std::make_shared<RowsCallback>(std::move(cb));

    if (!dbClient_ || orgIds.empty())
    {
        static const std::vector<OrganizationSuccessionNominations> kEmpty{};
        (*sharedCb)(kEmpty);
        return;
    }

    try
    {
        Mapper<OrganizationSuccessionNominations> mapper(dbClient_);
        mapper.findBy(
          Criteria(
            OrganizationSuccessionNominations::Cols::_organization_id,
            CompareOperator::In,
            orgIds
          ) &&
            Criteria(
              OrganizationSuccessionNominations::Cols::_accepted_at,
              CompareOperator::IsNull
            ),
          [sharedCb](const std::vector<OrganizationSuccessionNominations> &rows) {
              (*sharedCb)(rows);
          },
          [sharedCb](const DrogonDbException &e) {
              LOG_ERROR << "findPendingForOrgs failed: " << e.base().what();
              static const std::vector<OrganizationSuccessionNominations> kEmpty{};
              (*sharedCb)(kEmpty);
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "findPendingForOrgs Mapper construction failed";
        static const std::vector<OrganizationSuccessionNominations> kEmpty{};
        (*sharedCb)(kEmpty);
    }
}

void OrgSuccessionRepository::findPendingWithOrgForNominee(
  int32_t userId,
  NomineeNominationsCallback &&cb
)
{
    auto sharedCb = std::make_shared<NomineeNominationsCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)({});
        return;
    }

    // Hop 1: the caller's pending nominations.
    try
    {
        Mapper<OrganizationSuccessionNominations> nomMapper(dbClient_);
        nomMapper.findBy(
          Criteria(
            OrganizationSuccessionNominations::Cols::_nominee_user_id,
            CompareOperator::EQ,
            userId
          ) &&
            Criteria(
              OrganizationSuccessionNominations::Cols::_accepted_at,
              CompareOperator::IsNull
            ),
          [this, sharedCb](const std::vector<OrganizationSuccessionNominations> &rows) {
              if (rows.empty())
              {
                  (*sharedCb)({});
                  return;
              }
              std::vector<int32_t> orgIds;
              orgIds.reserve(rows.size());
              for (const auto &r : rows)
                  orgIds.push_back(r.getValueOfOrganizationId());
              // Hop 2: the org rows for the banner (slug/name).
              try
              {
                  Mapper<Organizations> orgMapper(dbClient_);
                  orgMapper.findBy(
                    Criteria(Organizations::Cols::_id, CompareOperator::In, orgIds),
                    [sharedCb, rows](const std::vector<Organizations> &orgs) {
                        std::vector<NomineeNomination> out;
                        out.reserve(rows.size());
                        for (const auto &r : rows)
                        {
                            for (const auto &o : orgs)
                            {
                                if (o.getValueOfId() == r.getValueOfOrganizationId())
                                {
                                    NomineeNomination n;
                                    n.nomination = r;
                                    n.org = o;
                                    out.push_back(n);
                                    break;
                                }
                            }
                        }
                        (*sharedCb)(out);
                    },
                    [sharedCb](const DrogonDbException &e) {
                        LOG_ERROR << "findPendingWithOrgForNominee org hop failed: "
                                  << e.base().what();
                        (*sharedCb)({});
                    }
                  );
              }
              catch (...)
              {
                  LOG_ERROR << "findPendingWithOrgForNominee org Mapper construction failed";
                  (*sharedCb)({});
              }
          },
          [sharedCb](const DrogonDbException &e) {
              LOG_ERROR << "findPendingWithOrgForNominee nomination hop failed: "
                        << e.base().what();
              (*sharedCb)({});
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "findPendingWithOrgForNominee nomination Mapper construction failed";
        (*sharedCb)({});
    }
}

void OrgSuccessionRepository::findOrgIdsWithPendingForOwner(
  int32_t userId,
  const std::function<void(const std::vector<int32_t> &)> &&cb
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const std::vector<int32_t> &)>>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)({});
        return;
    }

    // Hop 1: the user's CURRENT owner seats.
    try
    {
        Mapper<OrganizationMembers> memberMapper(dbClient_);
        memberMapper.findBy(
          Criteria(OrganizationMembers::Cols::_user_id, CompareOperator::EQ, userId) &&
            Criteria(OrganizationMembers::Cols::_role, CompareOperator::EQ, "owner"),
          [this, sharedCb](const std::vector<OrganizationMembers> &owned) {
              std::vector<int32_t> orgIds;
              orgIds.reserve(owned.size());
              for (const auto &m : owned)
                  orgIds.push_back(m.getValueOfOrganizationId());
              if (orgIds.empty())
              {
                  (*sharedCb)({});
                  return;
              }
              // Hop 2: pending nominations among those orgs.
              try
              {
                  Mapper<OrganizationSuccessionNominations> nomMapper(dbClient_);
                  nomMapper.findBy(
                    Criteria(
                      OrganizationSuccessionNominations::Cols::_organization_id,
                      CompareOperator::In,
                      orgIds
                    ) &&
                      Criteria(
                        OrganizationSuccessionNominations::Cols::_accepted_at,
                        CompareOperator::IsNull
                      ),
                    [sharedCb](
                      const std::vector<OrganizationSuccessionNominations> &rows) {
                        std::vector<int32_t> out;
                        out.reserve(rows.size());
                        for (const auto &r : rows)
                            out.push_back(r.getValueOfOrganizationId());
                        (*sharedCb)(out);
                    },
                    [sharedCb](const DrogonDbException &e) {
                        LOG_ERROR << "findOrgIdsWithPendingForOwner nomination hop failed: "
                                  << e.base().what();
                        (*sharedCb)({});
                    }
                  );
              }
              catch (...)
              {
                  LOG_ERROR << "findOrgIdsWithPendingForOwner nomination Mapper "
                               "construction failed";
                  (*sharedCb)({});
              }
          },
          [sharedCb](const DrogonDbException &e) {
              LOG_ERROR << "findOrgIdsWithPendingForOwner membership hop failed: "
                        << e.base().what();
              (*sharedCb)({});
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "findOrgIdsWithPendingForOwner membership Mapper construction failed";
        (*sharedCb)({});
    }
}

void OrgSuccessionRepository::withdrawPending(int32_t orgId, CountCallback &&cb)
{
    auto sharedCb = std::make_shared<CountCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(0);
        return;
    }

    try
    {
        Mapper<OrganizationSuccessionNominations> mapper(dbClient_);
        mapper.deleteBy(
          Criteria(
            OrganizationSuccessionNominations::Cols::_organization_id,
            CompareOperator::EQ,
            orgId
          ) &&
            Criteria(
              OrganizationSuccessionNominations::Cols::_accepted_at,
              CompareOperator::IsNull
            ),
          [sharedCb](const size_t count) { (*sharedCb)(count); },
          [sharedCb](const DrogonDbException &e) {
              LOG_ERROR << "withdrawPending failed: " << e.base().what();
              (*sharedCb)(0);
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "withdrawPending Mapper construction failed";
        (*sharedCb)(0);
    }
}

void OrgSuccessionRepository::effectSuccession(
  int32_t orgId,
  int32_t nomineeUserId,
  BoolCallback &&cb
)
{
    auto sharedCb = std::make_shared<BoolCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(false);
        return;
    }

    // Plain transaction (no lock keys): the optimistic accepted_at guard
    // in step 3 arbitrates concurrent acceptances -- the loser finds
    // zero updated rows and the whole transaction rolls back.
    withAdvisoryXactLock(
      dbClient_,
      {},
      [sharedCb, orgId, nomineeUserId](const std::shared_ptr<Transaction> &txn) {
          // Success is reported from the COMMIT callback; every failure
          // path reports inline (a rolled-back transaction never fires
          // its commit callback, so there is no double invocation).
          txn->setCommitCallback([sharedCb](bool committed) { (*sharedCb)(committed); });

          // Step 1 (R-M3-3): the nominee's membership becomes owner --
          // promote an existing member or create the seat (the nominee
          // may be a non-member, #228 semantics).
          try
          {
              Mapper<OrganizationMembers> memberMapper(txn);
              memberMapper.findOne(
                Criteria(
                  OrganizationMembers::Cols::_organization_id, CompareOperator::EQ, orgId
                ) &&
                  Criteria(
                    OrganizationMembers::Cols::_user_id, CompareOperator::EQ, nomineeUserId
                  ),
                [txn, orgId, nomineeUserId, sharedCb](const OrganizationMembers &row) {
                    OrganizationMembers updated = row;
                    updated.setRole("owner");
                    try
                    {
                        Mapper<OrganizationMembers>(txn).update(
                          updated,
                          [txn, orgId, nomineeUserId, sharedCb](const std::size_t count) {
                              if (count == 0)
                              {
                                  // The nominee left between findOne and
                                  // update; fail (rollback).
                                  (*sharedCb)(false);
                                  return;
                              }
                              demoteAndMark(txn, orgId, nomineeUserId, sharedCb);
                          },
                          [sharedCb](const DrogonDbException &e) {
                              LOG_ERROR << "effectSuccession promote update failed: "
                                        << e.base().what();
                              (*sharedCb)(false);
                          }
                        );
                    }
                    catch (...)
                    {
                        LOG_ERROR << "effectSuccession promote update Mapper construction "
                                     "failed";
                        (*sharedCb)(false);
                    }
                },
                [txn, orgId, nomineeUserId, sharedCb](const DrogonDbException &e) {
                    if (dynamic_cast<const UnexpectedRows *>(&e) != nullptr)
                    {
                        // Not a member yet -- create the owner seat.
                        OrganizationMembers member;
                        member.setOrganizationId(orgId);
                        member.setUserId(nomineeUserId);
                        member.setRole("owner");
                        try
                        {
                            Mapper<OrganizationMembers>(txn).insert(
                              member,
                              [txn, orgId, nomineeUserId, sharedCb](const OrganizationMembers &) {
                                  demoteAndMark(txn, orgId, nomineeUserId, sharedCb);
                              },
                              [sharedCb](const DrogonDbException &e2) {
                                  LOG_ERROR << "effectSuccession owner seat insert failed: "
                                            << e2.base().what();
                                  (*sharedCb)(false);
                              }
                            );
                        }
                        catch (...)
                        {
                            LOG_ERROR << "effectSuccession owner seat insert Mapper "
                                         "construction failed";
                            (*sharedCb)(false);
                        }
                        return;
                    }
                    LOG_ERROR << "effectSuccession membership findOne failed: "
                              << e.base().what();
                    (*sharedCb)(false);
                }
              );
          }
          catch (...)
          {
              LOG_ERROR << "effectSuccession membership Mapper construction failed";
              (*sharedCb)(false);
          }
      },
      [sharedCb](const std::string &err) {
          LOG_ERROR << "effectSuccession transaction acquisition failed: " << err;
          (*sharedCb)(false);
      }
    );
}

}  // namespace fulla::storage::postgres
