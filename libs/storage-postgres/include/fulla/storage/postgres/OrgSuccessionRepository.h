#pragma once

// Ownership succession data access (v1.5.0 M3, design §1.3 item 2,
// rulings R-M3-2/3/4). The V037 organization_succession_nominations
// table holds at most ONE pending (accepted_at IS NULL) nomination per
// org (partial unique index); a nomination names any live user as the
// successor candidate and only takes effect when accepted.
//
// effectSuccession() is the single-transaction seat swap both acceptance
// paths share (the portal accept endpoint and the owner-soft-delete
// auto-effect): nominee membership upserted to owner + every other owner
// row demoted to admin + the nomination row marked accepted -- all or
// nothing (the #228 admin transfer's fail-open ordering is deliberately
// NOT used here: two-step confirmation makes atomicity the right bias,
// per R-M3-3).
//
// Like ClientOwnersRepository/OrgConsentRepository this is a concrete
// storage class (no Domain interface) consumed by apps/server product
// services and libs/drogon controllers; arch-guard R4 keeps the
// succession queries out of apps/server entirely.

#include <drogon/orm/DbClient.h>

#include <fulla/storage/postgres/models/OrganizationSuccessionNominations.h>
#include <fulla/storage/postgres/models/Organizations.h>

#include <functional>
#include <optional>
#include <vector>

namespace fulla::storage::postgres
{

class OrgSuccessionRepository
{
  public:
    using BoolCallback = std::function<void(bool)>;
    using CountCallback = std::function<void(size_t)>;
    using RowOptCallback = std::function<void(
      const std::optional<::drogon_model::fulla_db::OrganizationSuccessionNominations> &)>;
    using RowsCallback = std::function<void(
      const std::vector<::drogon_model::fulla_db::OrganizationSuccessionNominations> &)>;

    explicit OrgSuccessionRepository(::drogon::orm::DbClientPtr dbClient);

    /// Create or overwrite the org's pending nomination (R-M3-3: a new
    /// nomination replaces any previous pending one; an ACCEPTED
    /// nomination is history and never conflicts). INSERT ... ON
    /// CONFLICT with the partial index predicate (raw-SQL exemption:
    /// the partial unique target cannot be expressed through Mapper).
    void upsertNomination(
      int32_t orgId,
      int32_t nomineeUserId,
      int32_t nominatedBy,
      BoolCallback &&cb
    );

    /// Is the user live (exists, not soft-deleted)? Nomination validity
    /// is a nomination concern (a soft-deleted nominee would recreate the
    /// #221 deadlock), so the check lives with the succession data
    /// access -- and keeps apps/server Mapper-free (arch-guard R4).
    void isLiveUser(int32_t userId, BoolCallback &&cb);

    /// The org's pending nomination, if any.
    void findPending(int32_t orgId, RowOptCallback &&cb);

    /// Pending nominations across the given orgs (listMyOrgs'
    /// owner/admin enrichment: one query instead of one per org).
    void findPendingForOrgs(
      const std::vector<int32_t> &orgIds,
      RowsCallback &&cb
    );

    /// Pending nominations naming `userId` as the nominee, each paired
    /// with its organization row (the nominee-facing discovery list --
    /// org slug/name for the accept banner; the nominee may be a
    /// non-member, so the org rows are resolved here, two Mapper hops).
    struct NomineeNomination
    {
        ::drogon_model::fulla_db::OrganizationSuccessionNominations nomination;
        ::drogon_model::fulla_db::Organizations org;
    };
    using NomineeNominationsCallback =
      std::function<void(const std::vector<NomineeNomination> &)>;
    void findPendingWithOrgForNominee(int32_t userId, NomineeNominationsCallback &&cb);

    /// Orgs where `userId` is a live owner AND a pending nomination
    /// exists (the soft-delete paths' auto-effect input). Two Mapper
    /// hops (owned memberships, then pending nominations by org ids).
    /// Error semantics: a failure on either hop reports nullopt --
    /// DISTINCT from an empty list -- so the SuccessionGuard can abort
    /// the deletion instead of silently proceeding past a read error
    /// (fail-closed).
    void findOrgIdsWithPendingForOwner(
      int32_t userId,
      const std::function<void(const std::optional<std::vector<int32_t>> &)> &&cb);

    /// Owner withdraws the pending nomination (physical delete of the
    /// pending row -- it is a state, not history; the audit trail keeps
    /// the record). Reports the number of rows removed (0 = nothing
    /// pending).
    void withdrawPending(int32_t orgId, CountCallback &&cb);

    /// The single-transaction seat swap (see the class comment). `cb`
    /// fires from the COMMIT callback: true = the seat moved to
    /// `nomineeUserId`; false = anything failed (including the
    /// optimistic accepted_at guard -- a concurrent acceptance).
    void effectSuccession(int32_t orgId,
                          int32_t nomineeUserId,
                          BoolCallback &&cb);

  private:
    ::drogon::orm::DbClientPtr dbClient_;
};

}  // namespace fulla::storage::postgres
