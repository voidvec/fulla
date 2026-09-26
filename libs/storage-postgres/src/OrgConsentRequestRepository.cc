// See OrgConsentRequestRepository.h for the workflow contract (rulings
// B2-B8 of .zcode/plans/issues-batch-2/04-design-B-entry.md). Every method
// is a single async hop whose callbacks never dereference `this`, so the
// stack-construction contract of ClientOwnersRepository applies; the
// SERVICE chains the hops and keeps the repository alive via shared_ptr
// (the OrgConsentRepository self-keeping pattern is not needed here).
//
// Raw SQL: the INSERT ... ON CONFLICT (fileRequest) and the two scoped
// UPDATEs (decidePending / resolveOtherPending) carry their exemption
// rationale inline; everything else is Mapper + Criteria.

#include <fulla/storage/postgres/OrgConsentRequestRepository.h>

#include <fulla/storage/postgres/models/Oauth2ClientScopes.h>

#include <drogon/drogon.h>

#include <memory>

namespace fulla::storage::postgres
{

using namespace ::drogon::orm;
using namespace ::drogon_model::fulla_db;

OrgConsentRequestRepository::OrgConsentRequestRepository(
  ::drogon::orm::DbClientPtr dbClient
)
    : dbClient_(std::move(dbClient))
{
}

void OrgConsentRequestRepository::fileRequest(
  int32_t orgId,
  const std::string &clientId,
  int32_t requesterId,
  CountCallback &&cb
)
{
    auto sharedCb = std::make_shared<CountCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(0);
        return;
    }

    // INSERT ... ON CONFLICT DO NOTHING raw-SQL exemption (B3): the
    // arbiter is the partial unique index uq_org_consent_requests_pending,
    // which a bare column-list conflict target cannot address (no full
    // UNIQUE constraint on those columns) -- the no-target form covers it
    // and is the documented ON CONFLICT exemption. requested_at/status
    // take their column defaults. affectedRows is 1 for a new row, 0 when
    // an identical pending row already existed (PQcmdTuples; never
    // Result::size() on a write -- the M2 UPDATE lesson).
    dbClient_->execSqlAsync(
      "INSERT INTO organization_consent_requests "
      "(organization_id, client_id, requested_by) VALUES ($1, $2, $3) "
      "ON CONFLICT DO NOTHING",
      [sharedCb](const Result &r) { (*sharedCb)(r.affectedRows()); },
      [sharedCb](const DrogonDbException &e) {
          LOG_ERROR << "fileRequest insert failed: " << e.base().what();
          (*sharedCb)(0);
      },
      orgId,
      clientId,
      requesterId
    );
}

void OrgConsentRequestRepository::findPending(
  int32_t orgId,
  const std::string &clientId,
  int32_t requesterId,
  RowCallback &&cb
)
{
    auto sharedCb = std::make_shared<RowCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(false, OrganizationConsentRequests{});
        return;
    }

    try
    {
        Mapper<OrganizationConsentRequests> mapper(dbClient_);
        mapper.findOne(
          Criteria(OrganizationConsentRequests::Cols::_organization_id,
                   CompareOperator::EQ, orgId) &&
            Criteria(OrganizationConsentRequests::Cols::_client_id,
                     CompareOperator::EQ, clientId) &&
            Criteria(OrganizationConsentRequests::Cols::_requested_by,
                     CompareOperator::EQ, requesterId) &&
            Criteria(OrganizationConsentRequests::Cols::_status,
                     CompareOperator::EQ, "pending"),
          [sharedCb](const OrganizationConsentRequests &row) {
              (*sharedCb)(true, row);
          },
          [sharedCb](const DrogonDbException &) {
              //findOne throws on no row (or a real DB error); the service
              // re-issues the write on the not-found path, so both fold to
              // "no pending row" without an oracle.
              (*sharedCb)(false, OrganizationConsentRequests{});
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "findPending Mapper construction failed";
        (*sharedCb)(false, OrganizationConsentRequests{});
    }
}

void OrgConsentRequestRepository::listPendingByOrg(
  int32_t orgId,
  RowsCallback &&cb
)
{
    auto sharedCb = std::make_shared<RowsCallback>(std::move(cb));

    if (!dbClient_)
    {
        static const std::vector<OrganizationConsentRequests> kEmpty{};
        (*sharedCb)(kEmpty);
        return;
    }

    try
    {
        Mapper<OrganizationConsentRequests> mapper(dbClient_);
        mapper.findBy(
          Criteria(OrganizationConsentRequests::Cols::_organization_id,
                   CompareOperator::EQ, orgId) &&
            Criteria(OrganizationConsentRequests::Cols::_status,
                     CompareOperator::EQ, "pending"),
          [sharedCb](const std::vector<OrganizationConsentRequests> &rows) {
              (*sharedCb)(rows);
          },
          [sharedCb](const DrogonDbException &e) {
              LOG_ERROR << "listPendingByOrg failed: " << e.base().what();
              static const std::vector<OrganizationConsentRequests> kEmpty{};
              (*sharedCb)(kEmpty);
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "listPendingByOrg Mapper construction failed";
        static const std::vector<OrganizationConsentRequests> kEmpty{};
        (*sharedCb)(kEmpty);
    }
}

void OrgConsentRequestRepository::findById(int64_t requestId, RowCallback &&cb)
{
    auto sharedCb = std::make_shared<RowCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(false, OrganizationConsentRequests{});
        return;
    }

    try
    {
        Mapper<OrganizationConsentRequests> mapper(dbClient_);
        mapper.findOne(
          Criteria(OrganizationConsentRequests::Cols::_id,
                   CompareOperator::EQ, requestId),
          [sharedCb](const OrganizationConsentRequests &row) {
              (*sharedCb)(true, row);
          },
          [sharedCb](const DrogonDbException &) {
              (*sharedCb)(false, OrganizationConsentRequests{});
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "findById Mapper construction failed";
        (*sharedCb)(false, OrganizationConsentRequests{});
    }
}

void OrgConsentRequestRepository::decidePending(
  int64_t requestId,
  const std::string &status,
  int32_t decidedBy,
  const std::string &rejectReason,
  CountCallback &&cb
)
{
    auto sharedCb = std::make_shared<CountCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(0);
        return;
    }

    // Scoped single-row decision UPDATE raw-SQL exemption (the documented
    // batch-UPDATE shape: the WHERE is fully qualified by id + pending
    // status, decided_at needs the server clock -- the
    // revokeClientConsents precedent). affectedRows 0 = the row was
    // concurrently decided or is gone.
    dbClient_->execSqlAsync(
      "UPDATE organization_consent_requests "
      "SET status = $2, decided_by = $3, decided_at = CURRENT_TIMESTAMP, "
      "reject_reason = $4 "
      "WHERE id = $1 AND status = 'pending'",
      [sharedCb](const Result &r) { (*sharedCb)(r.affectedRows()); },
      [sharedCb](const DrogonDbException &e) {
          LOG_ERROR << "decidePending failed: " << e.base().what();
          (*sharedCb)(0);
      },
      requestId,
      status,
      decidedBy,
      rejectReason
    );
}

void OrgConsentRequestRepository::resolveOtherPending(
  int32_t orgId,
  const std::string &clientId,
  int64_t exceptRequestId,
  int32_t decidedBy,
  CountCallback &&cb
)
{
    auto sharedCb = std::make_shared<CountCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(0);
        return;
    }

    // B4 sibling resolution: one scoped batch UPDATE raw-SQL exemption
    // (same family as revokeClientConsents -- the rows are
    // service-managed and the WHERE is fully qualified by org + client +
    // pending + the approved sibling). Last-writer-wins is impossible:
    // only this approval path writes 'approved' for these rows.
    dbClient_->execSqlAsync(
      "UPDATE organization_consent_requests "
      "SET status = 'approved', decided_by = $3, decided_at = CURRENT_TIMESTAMP "
      "WHERE organization_id = $1 AND client_id = $2 AND status = 'pending' "
      "AND id != $4",
      [sharedCb](const Result &r) { (*sharedCb)(r.affectedRows()); },
      [sharedCb](const DrogonDbException &e) {
          LOG_ERROR << "resolveOtherPending failed: " << e.base().what();
          (*sharedCb)(0);
      },
      orgId,
      clientId,
      decidedBy,
      exceptRequestId
    );
}

void OrgConsentRequestRepository::withdrawOwnPending(
  int64_t requestId,
  int32_t requesterId,
  CountCallback &&cb
)
{
    auto sharedCb = std::make_shared<CountCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(0);
        return;
    }

    // B7: physical delete of the caller's OWN pending row only (the
    // compound criteria makes another user's id resolve to 0 rows = the
    // service's uniform 404). Mapper::deleteBy precedent:
    // OrgSuccessionRepository.cc.
    try
    {
        Mapper<OrganizationConsentRequests> mapper(dbClient_);
        mapper.deleteBy(
          Criteria(OrganizationConsentRequests::Cols::_id,
                   CompareOperator::EQ, requestId) &&
            Criteria(OrganizationConsentRequests::Cols::_requested_by,
                     CompareOperator::EQ, requesterId) &&
            Criteria(OrganizationConsentRequests::Cols::_status,
                     CompareOperator::EQ, "pending"),
          [sharedCb](const std::size_t deleted) { (*sharedCb)(deleted); },
          [sharedCb](const DrogonDbException &e) {
              LOG_ERROR << "withdrawOwnPending failed: " << e.base().what();
              (*sharedCb)(0);
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "withdrawOwnPending Mapper construction failed";
        (*sharedCb)(0);
    }
}

void OrgConsentRequestRepository::findClientScopes(
  const std::string &clientId,
  std::function<void(const std::vector<std::string> &)> &&cb
)
{
    auto sharedCb =
      std::make_shared<std::function<void(const std::vector<std::string> &)>>(
        std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)({});
        return;
    }

    try
    {
        Mapper<Oauth2ClientScopes> mapper(dbClient_);
        mapper.findBy(
          Criteria(Oauth2ClientScopes::Cols::_client_id,
                   CompareOperator::EQ, clientId),
          [sharedCb](const std::vector<Oauth2ClientScopes> &rows) {
              std::vector<std::string> scopes;
              scopes.reserve(rows.size());
              for (const auto &r : rows)
                  scopes.push_back(r.getValueOfScopeName());
              (*sharedCb)(scopes);
          },
          [sharedCb](const DrogonDbException &e) {
              LOG_ERROR << "findClientScopes failed: " << e.base().what();
              (*sharedCb)({});
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "findClientScopes Mapper construction failed";
        (*sharedCb)({});
    }
}

}  // namespace fulla::storage::postgres
