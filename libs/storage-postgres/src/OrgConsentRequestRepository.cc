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
#include "OrgConsentTransaction.h"

#include <fulla/storage/postgres/models/Oauth2ClientScopes.h>
#include <fulla/storage/postgres/models/Oauth2Clients.h>
#include <fulla/storage/postgres/models/Organizations.h>
#include <fulla/storage/postgres/models/OrganizationConsents.h>
#include <fulla/storage/postgres/models/Users.h>

#include <drogon/drogon.h>

#include <memory>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace fulla::storage::postgres
{

using namespace ::drogon::orm;
using namespace ::drogon_model::fulla_db;

namespace
{
using DecisionStatus = OrgConsentDecisionStatus;
using DecisionResult = OrgConsentDecisionResult;

struct DecisionCompletion
{
    OrgConsentRequestRepository::DecisionCallback callback;
    std::atomic<bool> completed{false};
    std::mutex mutex;
    DecisionResult pending;

    void finish(DecisionResult result)
    {
        if (completed.exchange(true))
            return;
        auto deliver = std::move(callback);
        if (deliver)
            deliver(std::move(result));
    }

    void stage(DecisionResult result)
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending = std::move(result);
    }

    void commit(bool committed)
    {
        DecisionResult result;
        if (committed)
        {
            std::lock_guard<std::mutex> lock(mutex);
            result = pending;
        }
        finish(std::move(result));
    }
};

class ConsentDecision : public std::enable_shared_from_this<ConsentDecision>
{
  public:
    ConsentDecision(
      DbClientPtr db,
      int32_t orgId,
      int32_t requestId,
      int32_t actorId,
      bool approve,
      std::string reason,
      OrgConsentRequestRepository::DecisionCallback cb
    )
        : db_(std::move(db)),
          orgId_(orgId),
          requestId_(requestId),
          actorId_(actorId),
          approve_(approve),
          reason_(std::move(reason)),
          completion_(std::make_shared<DecisionCompletion>())
    {
        completion_->callback = std::move(cb);
    }

    void start()
    {
        if (!db_)
        {
            abort(DecisionStatus::StorageError);
            return;
        }
        auto self = shared_from_this();
        findRows<OrganizationConsentRequests>(
          Criteria(OrganizationConsentRequests::Cols::_id, CompareOperator::EQ, requestId_) &&
            Criteria(
              OrganizationConsentRequests::Cols::_organization_id, CompareOperator::EQ, orgId_
            ),
          false,
          [self](const std::vector<OrganizationConsentRequests> &rows) {
              if (rows.empty())
              {
                  self->abort(DecisionStatus::NotFound);
                  return;
              }
              self->clientId_ = rows.front().getValueOfClientId();
              self->begin();
          }
        );
    }

  private:
    DbClientPtr db_;
    detail::ConsentTransaction txn_;
    int32_t orgId_;
    int32_t requestId_;
    int32_t actorId_;
    bool approve_;
    bool alreadyApproved_ = false;
    std::string reason_;
    std::string clientId_;
    std::vector<std::string> scopes_;
    std::shared_ptr<DecisionCompletion> completion_;

    void abort(DecisionStatus status)
    {
        if (txn_)
        {
            try
            {
                txn_->rollback();
            }
            catch (...)
            {
            }
        }
        DecisionResult result;
        result.status = status;
        completion_->finish(std::move(result));
    }

    template <typename Row, typename Success>
    void findRows(const Criteria &criteria, bool rowLock, Success success)
    {
        auto self = shared_from_this();
        try
        {
            DbClientPtr client = txn_ ? std::static_pointer_cast<DbClient>(txn_) : db_;
            Mapper<Row> mapper(client);
            if (rowLock)
                mapper.forUpdate();
            mapper.findBy(
              criteria,
              [self, success](const std::vector<Row> &rows) {
                  if (self->completion_->completed.load())
                      return;
                  try
                  {
                      success(rows);
                  }
                  catch (const std::exception &)
                  {
                      self->abort(DecisionStatus::StorageError);
                  }
                  catch (...)
                  {
                      self->abort(DecisionStatus::StorageError);
                  }
              },
              [self](const DrogonDbException &e) {
                  LOG_ERROR << "organization decision query failed: " << e.base().what();
                  self->abort(DecisionStatus::StorageError);
              }
            );
        }
        catch (const std::exception &)
        {
            abort(DecisionStatus::StorageError);
        }
        catch (...)
        {
            abort(DecisionStatus::StorageError);
        }
    }

    template <typename Success, typename... Args>
    void execute(const std::string &sql, Success success, const Args &...args)
    {
        auto self = shared_from_this();
        try
        {
            txn_->execSqlAsync(
              sql,
              [self, success](const Result &rows) {
                  if (self->completion_->completed.load())
                      return;
                  try
                  {
                      success(rows);
                  }
                  catch (const std::exception &)
                  {
                      self->abort(DecisionStatus::StorageError);
                  }
                  catch (...)
                  {
                      self->abort(DecisionStatus::StorageError);
                  }
              },
              [self](const DrogonDbException &e) {
                  LOG_ERROR << "organization decision write failed: " << e.base().what();
                  self->abort(DecisionStatus::StorageError);
              },
              args...
            );
        }
        catch (const std::exception &)
        {
            abort(DecisionStatus::StorageError);
        }
        catch (...)
        {
            abort(DecisionStatus::StorageError);
        }
    }

    void begin()
    {
        auto self = shared_from_this();
        // Match organization deletion first, then single-scope grant and
        // pair revoke. No caller acquires these keys in reverse order.
        detail::withConsentTransaction(
          db_,
          {"org:" + std::to_string(orgId_),
           "orgconsent:" + std::to_string(orgId_) + ":" + clientId_},
          [self](const detail::ConsentTransaction &txn) {
              self->txn_ = txn;
              // Never capture self/txn here: dropping the last transaction
              // owner is what requests COMMIT in Drogon.
              auto completion = self->completion_;
              txn->setCommitCallback([completion](bool ok) { completion->commit(ok); });
              self->readOrganization();
          },
          [self]() { self->abort(DecisionStatus::StorageError); }
        );
    }

    void readOrganization()
    {
        auto self = shared_from_this();
        findRows<Organizations>(
          Criteria(Organizations::Cols::_id, CompareOperator::EQ, orgId_) &&
            Criteria(Organizations::Cols::_deleted_at, CompareOperator::IsNull),
          true,
          [self](const std::vector<Organizations> &rows) {
              if (rows.empty())
                  self->abort(DecisionStatus::NotFound);
              else
                  self->readActor();
          }
        );
    }

    void readActor()
    {
        auto self = shared_from_this();
        findRows<OrganizationMembers>(
          Criteria(OrganizationMembers::Cols::_organization_id, CompareOperator::EQ, orgId_) &&
            Criteria(OrganizationMembers::Cols::_user_id, CompareOperator::EQ, actorId_),
          true,
          [self](const std::vector<OrganizationMembers> &rows) {
              if (
                rows.empty() || (rows.front().getValueOfRole() != "owner" &&
                                 rows.front().getValueOfRole() != "admin")
              )
                  self->abort(DecisionStatus::Forbidden);
              else
                  self->readRequest();
          }
        );
    }

    void readRequest()
    {
        auto self = shared_from_this();
        findRows<OrganizationConsentRequests>(
          Criteria(OrganizationConsentRequests::Cols::_id, CompareOperator::EQ, requestId_) &&
            Criteria(
              OrganizationConsentRequests::Cols::_organization_id, CompareOperator::EQ, orgId_
            ),
          true,
          [self](const std::vector<OrganizationConsentRequests> &rows) {
              if (rows.empty())
              {
                  self->abort(DecisionStatus::NotFound);
                  return;
              }
              const auto &row = rows.front();
              if (row.getValueOfClientId() != self->clientId_)
              {
                  self->abort(DecisionStatus::Conflict);
                  return;
              }
              const auto &status = row.getValueOfStatus();
              if (status != "pending" && !(self->approve_ && status == "approved"))
              {
                  self->abort(DecisionStatus::Conflict);
                  return;
              }
              self->alreadyApproved_ = status == "approved";
              if (!self->approve_)
                  self->reject();
              else
                  self->readClient();
          }
        );
    }

    void readClient()
    {
        auto self = shared_from_this();
        findRows<Oauth2Clients>(
          Criteria(Oauth2Clients::Cols::_client_id, CompareOperator::EQ, clientId_) &&
            Criteria(Oauth2Clients::Cols::_deleted_at, CompareOperator::IsNull),
          false,
          [self](const std::vector<Oauth2Clients> &rows) {
              if (rows.empty())
                  // Deliberate tightening vs. the pre-S04 flow (which
                  // approved leftover scope rows for a soft-deleted
                  // client): an unapprovable request renders as the same
                  // 404 as a missing/foreign id. Rejection and withdrawal
                  // skip this read, so the request stays resolvable.
                  self->abort(DecisionStatus::NotFound);
              else
                  self->readActive();
          }
        );
    }

    void readActive()
    {
        auto self = shared_from_this();
        findRows<OrganizationConsents>(
          Criteria(OrganizationConsents::Cols::_organization_id, CompareOperator::EQ, orgId_) &&
            Criteria(OrganizationConsents::Cols::_client_id, CompareOperator::EQ, clientId_) &&
            Criteria(OrganizationConsents::Cols::_revoked_at, CompareOperator::IsNull),
          false,
          [self](const std::vector<OrganizationConsents> &rows) {
              // F-2: an existing active approval is a no-op. Current client
              // scope expansion is not authorization to extend an old grant.
              if (self->alreadyApproved_ && !rows.empty())
                  self->stage(DecisionStatus::AlreadyApproved);
              else
                  self->readScopes();
          }
        );
    }

    void readScopes()
    {
        auto self = shared_from_this();
        findRows<Oauth2ClientScopes>(
          Criteria(Oauth2ClientScopes::Cols::_client_id, CompareOperator::EQ, clientId_),
          false,
          [self](const std::vector<Oauth2ClientScopes> &rows) {
              for (const auto &row : rows)
                  self->scopes_.push_back(row.getValueOfScopeName());
              std::sort(self->scopes_.begin(), self->scopes_.end());
              self->scopes_.erase(
                std::unique(self->scopes_.begin(), self->scopes_.end()), self->scopes_.end()
              );
              self->writeScope(0);
          }
        );
    }

    void writeScope(size_t index)
    {
        if (index == scopes_.size())
        {
            markApproved();
            return;
        }
        auto self = shared_from_this();
        // Parameter-bound INSERT ON CONFLICT: revive exactly the registered
        // scope snapshot using V036's existing unique constraint.
        execute(
          "INSERT INTO organization_consents "
          "(organization_id, client_id, scope_name, granted_by, granted_at, revoked_at) "
          "VALUES ($1,$2,$3,$4,CURRENT_TIMESTAMP,NULL) "
          "ON CONFLICT (organization_id,client_id,scope_name) DO UPDATE SET "
          "revoked_at=NULL, granted_by=EXCLUDED.granted_by, granted_at=CURRENT_TIMESTAMP",
          [self, index](const Result &) { self->writeScope(index + 1); },
          orgId_,
          clientId_,
          scopes_[index],
          actorId_
        );
    }

    void markApproved()
    {
        if (alreadyApproved_)
        {
            resolveSiblings();
            return;
        }
        auto self = shared_from_this();
        // Conditional UPDATE RETURNING arbitrates any unexpected state
        // change. A zero-row result rolls back every scope written above.
        execute(
          "UPDATE organization_consent_requests SET status='approved', decided_by=$3, "
          "decided_at=CURRENT_TIMESTAMP, reject_reason='' "
          "WHERE id=$1 AND organization_id=$2 AND status='pending' RETURNING id",
          [self](const Result &rows) {
              if (rows.size() != 1)
                  self->abort(DecisionStatus::Conflict);
              else
                  self->resolveSiblings();
          },
          requestId_,
          orgId_,
          actorId_
        );
    }

    void resolveSiblings()
    {
        auto self = shared_from_this();
        // Documented batch update: resolve visible pending siblings of this
        // exact org/client in the SAME transaction as all grant writes.
        execute(
          "UPDATE organization_consent_requests SET status='approved', decided_by=$3, "
          "decided_at=CURRENT_TIMESTAMP WHERE organization_id=$1 AND client_id=$2 "
          "AND status='pending' AND id<>$4",
          [self](const Result &) { self->stage(DecisionStatus::Approved); },
          orgId_,
          clientId_,
          actorId_,
          requestId_
        );
    }

    void reject()
    {
        auto self = shared_from_this();
        execute(
          "UPDATE organization_consent_requests SET status='rejected', decided_by=$3, "
          "decided_at=CURRENT_TIMESTAMP, reject_reason=$4 "
          "WHERE id=$1 AND organization_id=$2 AND status='pending' RETURNING id",
          [self](const Result &rows) {
              if (rows.size() != 1)
                  self->abort(DecisionStatus::Conflict);
              else
                  self->stage(DecisionStatus::Rejected);
          },
          requestId_,
          orgId_,
          actorId_,
          reason_
        );
    }

    void stage(DecisionStatus status)
    {
        DecisionResult result;
        result.status = status;
        result.requestId = requestId_;
        result.clientId = clientId_;
        result.scopes = scopes_;
        completion_->stage(std::move(result));
        // No further query holds self after its current callback returns.
        // Destruction releases txn_; its COMMIT callback owns completion only.
    }
};
}  // namespace

void OrgConsentRequestRepository::decideAtomically(
  int32_t orgId,
  int32_t requestId,
  int32_t actorId,
  bool approve,
  const std::string &reason,
  DecisionCallback &&cb
)
{
    auto operation = std::make_shared<ConsentDecision>(
      dbClient_, orgId, requestId, actorId, approve, reason, std::move(cb)
    );
    operation->start();
}

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
              // findOne throws on no row (or a real DB error); both fold to
              // "no pending row" without an oracle -- the service renders
              // the race as the ratified 409. Decision paths that must
              // distinguish use findPendingEx/findByIdEx.
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

void OrgConsentRequestRepository::findById(int32_t requestId, RowCallback &&cb)
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

// ---------------------------------------------------------------------------
// v1.4.0 review hardening: three-state lookups (see header). The old
// RowCallback variants fold infra failures into "no row"; these keep the
// #230 Error-vs-NoRow distinction so decision endpoints answer 5xx.
// ---------------------------------------------------------------------------

namespace
{
/// UnexpectedRows => genuine no-row; any other DrogonDbException => infra
/// failure (Mapper construction failures report the same shape inline).
void dispatchRowExError(
  const OrgConsentRequestRepository::RowExCallback &cb, const DrogonDbException &e
)
{
    if (dynamic_cast<const UnexpectedRows *>(&e) != nullptr)
    {
        cb(true, false, OrganizationConsentRequests{});
        return;
    }
    LOG_ERROR << "consent-request lookup failed: " << e.base().what();
    cb(false, false, OrganizationConsentRequests{});
}
}  // namespace

void OrgConsentRequestRepository::findPendingEx(
  int32_t orgId,
  const std::string &clientId,
  int32_t requesterId,
  RowExCallback &&cb
)
{
    auto sharedCb = std::make_shared<RowExCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(false, false, OrganizationConsentRequests{});
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
              (*sharedCb)(true, true, row);
          },
          [sharedCb](const DrogonDbException &e) {
              dispatchRowExError(*sharedCb, e);
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "findPendingEx Mapper construction failed";
        (*sharedCb)(false, false, OrganizationConsentRequests{});
    }
}

void OrgConsentRequestRepository::findByIdEx(int32_t requestId, RowExCallback &&cb)
{
    auto sharedCb = std::make_shared<RowExCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(false, false, OrganizationConsentRequests{});
        return;
    }

    try
    {
        Mapper<OrganizationConsentRequests> mapper(dbClient_);
        mapper.findOne(
          Criteria(OrganizationConsentRequests::Cols::_id,
                   CompareOperator::EQ, requestId),
          [sharedCb](const OrganizationConsentRequests &row) {
              (*sharedCb)(true, true, row);
          },
          [sharedCb](const DrogonDbException &e) {
              dispatchRowExError(*sharedCb, e);
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "findByIdEx Mapper construction failed";
        (*sharedCb)(false, false, OrganizationConsentRequests{});
    }
}

void OrgConsentRequestRepository::decidePending(
  int32_t requestId,
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
  int32_t exceptRequestId,
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
  int32_t orgId,
  int32_t requestId,
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
    // compound criteria makes another user's id -- or the same request
    // through a DIFFERENT org's URL -- resolve to 0 rows = the service's
    // uniform 404, and the audit event names the org that actually owns
    // the row). Mapper::deleteBy precedent: OrgSuccessionRepository.cc.
    try
    {
        Mapper<OrganizationConsentRequests> mapper(dbClient_);
        mapper.deleteBy(
          Criteria(OrganizationConsentRequests::Cols::_id,
                   CompareOperator::EQ, requestId) &&
            Criteria(OrganizationConsentRequests::Cols::_organization_id,
                     CompareOperator::EQ, orgId) &&
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
  std::function<void(bool dbOk, const std::vector<std::string> &)> &&cb
)
{
    auto sharedCb =
      std::make_shared<
        std::function<void(bool dbOk, const std::vector<std::string> &)>>(
        std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(false, {});
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
              (*sharedCb)(true, scopes);
          },
          [sharedCb](const DrogonDbException &e) {
              LOG_ERROR << "findClientScopes failed: " << e.base().what();
              (*sharedCb)(false, {});
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "findClientScopes Mapper construction failed";
        (*sharedCb)(false, {});
    }
}

void OrgConsentRequestRepository::findUsernames(
  const std::vector<int32_t> &userIds,
  std::function<void(const std::map<int32_t, std::string> &)> &&cb
)
{
    auto sharedCb =
      std::make_shared<
        std::function<void(const std::map<int32_t, std::string> &)>>(std::move(cb));

    if (!dbClient_ || userIds.empty())
    {
        static const std::map<int32_t, std::string> kEmpty{};
        (*sharedCb)(kEmpty);
        return;
    }

    try
    {
        Mapper<Users> mapper(dbClient_);
        mapper.findBy(
          Criteria(Users::Cols::_id, CompareOperator::In, userIds),
          [sharedCb](const std::vector<Users> &rows) {
              std::map<int32_t, std::string> names;
              for (const auto &u : rows)
                  names[u.getValueOfId()] = u.getValueOfUsername();
              (*sharedCb)(names);
          },
          [sharedCb](const DrogonDbException &e) {
              LOG_ERROR << "findUsernames failed: " << e.base().what();
              static const std::map<int32_t, std::string> kEmpty{};
              (*sharedCb)(kEmpty);
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "findUsernames Mapper construction failed";
        static const std::map<int32_t, std::string> kEmpty{};
        (*sharedCb)(kEmpty);
    }
}

}  // namespace fulla::storage::postgres
