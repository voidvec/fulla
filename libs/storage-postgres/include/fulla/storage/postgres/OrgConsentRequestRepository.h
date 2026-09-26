#pragma once

// Organization consent REQUEST workflow (#236 entry half, plan B; rulings
// B2-B8 of .zcode/plans/issues-batch-2/04-design-B-entry.md). A member
// files a request for (org, client); a manager approves (which writes the
// organization_consents rows via OrgConsentRepository -- the POLICY stays
// with the callers) or rejects; the requester may withdraw their own
// pending row.
//
// Every query the workflow needs lives HERE (arch-guard R4 keeps the org
// services Mapper-free); findClientScopes is the one cross-table read the
// approval needs (the client's registered scope set lives in the
// normalized oauth2_client_scopes table, not on the client row).
//
// Concurrency/lifetime: multi-hop methods keep themselves alive via
// make_shared (the CI ASan leg caught the stack-dangling shape as a UAF in
// the succession repository -- OrgConsentRepository.cc keeps the same
// pattern). Single-hop methods follow the ClientOwnersRepository
// stack-construction contract.

#include <drogon/orm/DbClient.h>

#include <fulla/storage/postgres/models/OrganizationConsentRequests.h>
#include <fulla/storage/postgres/models/OrganizationMembers.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fulla::storage::postgres
{

class OrgConsentRequestRepository
{
  public:
    using RowsCallback =
      std::function<void(const std::vector<::drogon_model::fulla_db::OrganizationConsentRequests> &)>;
    using RowCallback =
      std::function<void(bool found, ::drogon_model::fulla_db::OrganizationConsentRequests row)>;
    using CountCallback = std::function<void(std::size_t)>;
    using BoolCallback = std::function<void(bool)>;

    /// The client is copied into each query; stack construction per call
    /// site is the intended use for SINGLE-hop methods (same lifetime
    /// contract as ClientOwnersRepository). Multi-hop methods keep
    /// themselves alive internally.
    explicit OrgConsentRequestRepository(::drogon::orm::DbClientPtr dbClient);

    /// B3 filing: INSERT ... ON CONFLICT DO NOTHING (bare conflict target
    /// -- the partial unique index uq_org_consent_requests_pending cannot
    /// be addressed by a bare column list, and the no-target form covers
    /// every unique index). Reports 1 for a NEW row, 0 when an identical
    /// pending row already existed (PQcmdTuples via affectedRows; the M2
    /// lesson -- never Result::size() on a write).
    void fileRequest(
      int32_t orgId,
      const std::string &clientId,
      int32_t requesterId,
      CountCallback &&cb
    );

    /// The caller's pending request for an exact (org, client) pair -- the
    /// idempotent re-file response and the withdraw-by-identity lookup.
    void findPending(
      int32_t orgId,
      const std::string &clientId,
      int32_t requesterId,
      RowCallback &&cb
    );

    /// B9 manager list: every PENDING request of the org, oldest first
    /// (single hop; ordering is index-friendly).
    void listPendingByOrg(int32_t orgId, RowsCallback &&cb);

    /// Single row by id (approve/reject/withdraw entry lookup). Single hop.
    void findById(int32_t requestId, RowCallback &&cb);

    /// B4 decision: flips a PENDING row to the decided status. Reports the
    /// number of rows flipped (0 = the row was concurrently decided or is
    /// gone -- the callers translate that to their own 409/404 semantics).
    /// One scoped UPDATE (no raw SQL needed via the Mapper).
    void decidePending(
      int32_t requestId,
      const std::string &status,
      int32_t decidedBy,
      const std::string &rejectReason,
      CountCallback &&cb
    );

    /// B4 sibling resolution on approval: every OTHER pending request for
    /// the same (org, client) flips to approved with this decider. Zero
    /// siblings is normal (single requester). Multi-hop via self-keep.
    void resolveOtherPending(
      int32_t orgId,
      const std::string &clientId,
      int32_t exceptRequestId,
      int32_t decidedBy,
      CountCallback &&cb
    );

    /// B7 withdrawal: physically delete the caller's OWN pending row.
    /// Reports the number of rows deleted (0 = nothing matched, which the
    /// service renders as the anti-enumeration 404).
    void withdrawOwnPending(
      int32_t requestId,
      int32_t requesterId,
      CountCallback &&cb
    );

    /// B2 approval input: the client's registered scope names (the
    /// normalized oauth2_client_scopes table). Failure resolves to an
    /// empty set -- the caller then approves "zero scopes", which the
    /// design registers as a harmless degenerate outcome.
    void findClientScopes(
      const std::string &clientId,
      std::function<void(const std::vector<std::string> &)> &&cb
    );

    /// Manager-list hop 2 (the no-JOIN pairing): requester display names
    /// keyed by user id. Ids missing from the users table resolve to
    /// absent map entries (the caller renders an empty name). Lives here
    /// (not in the service) to keep the arch-guard R4 apps/server Mapper
    /// cap intact.
    void findUsernames(
      const std::vector<int32_t> &userIds,
      std::function<void(const std::map<int32_t, std::string> &)> &&cb
    );

  private:
    ::drogon::orm::DbClientPtr dbClient_;
};

}  // namespace fulla::storage::postgres
