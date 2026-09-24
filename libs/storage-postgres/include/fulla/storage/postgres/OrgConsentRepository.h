#pragma once

// Organization-level admin consent data access (v1.5.0 M2, design §2.2).
// The V036 organization_consents table holds one row per
// (org, client, scope) -- same shape as oauth2_user_consents -- so the
// authorize-time consent decision becomes a UNION: a scope is consented
// when the user's personal row exists OR any org the user is CURRENTLY a
// member of holds an active (revoked_at IS NULL) row. This repository
// owns every organization_consents query plus the membership fan-out the
// union needs (two Mapper hops -- the no-JOIN rule), mirroring
// ClientOwnersRepository (#222): the data access lives in storage, the
// POLICY (who may grant, what a revocation means) stays with the callers.
//
// Placement rationale (arch-guard R4): apps/server's Mapper-construction
// count is frozen at 57 and only allowed to decrease, so the portal
// consent endpoints' queries live HERE, not in the product services.
//
// Like ClientOwnersRepository this is a concrete class (no Domain
// interface): consumers exchange ORM rows; the Domain-facing seam for
// the union read is the IOrgConsentResolver port adapted by
// StorageOrgConsentResolver (libs/drogon).
//
// Concurrency: saveConsent serializes GRANTS per (org, client) with an
// advisory transaction lock (#219 / R-M2-5 key
// "orgconsent:<org_id>:<client_id>"). revokeClientConsents deliberately
// takes no lock: it is one atomic batch UPDATE, and a grant racing a
// revoke resolves last-writer-wins per row (the end state is exactly
// one of the two intents, never a mix -- O4 semantics unaffected).

#include <drogon/orm/DbClient.h>

#include <fulla/storage/postgres/models/OrganizationConsents.h>

#include <functional>
#include <string>
#include <vector>

namespace fulla::storage::postgres
{

class OrgConsentRepository
{
  public:
    using BoolCallback = std::function<void(bool)>;
    using RowsCallback = std::function<void(const std::vector<::drogon_model::fulla_db::OrganizationConsents> &)>;
    using CountCallback = std::function<void(size_t)>;

    /// The client is copied into each query; stack construction per call
    /// site is the intended use (same lifetime contract as
    /// ClientOwnersRepository).
    explicit OrgConsentRepository(::drogon::orm::DbClientPtr dbClient);

    /// R-M2-1 union input: does ANY org the user is currently a member
    /// of hold an active (org, client, scope) consent row? Two Mapper
    /// hops (memberships by user, then active consents by
    /// Criteria::In(orgIds)) -- JOINs are not used in this codebase.
    /// Any lookup failure resolves to false (consent data's failure mode
    /// is "prompt the user", never a failed authorization).
    void hasActiveConsentForUser(
      int32_t internalUserId,
      const std::string &clientId,
      const std::string &scope,
      BoolCallback &&cb
    );

    /// R-M2-2 write path: upsert one (org, client, scope) row as
    /// grantedBy, reviving a revoked row if present. Uses the
    /// INSERT ... ON CONFLICT DO UPDATE raw-SQL exemption (R-M2-6) and
    /// runs under the per-(org, client) advisory lock (R-M2-5).
    void saveConsent(
      int32_t orgId,
      int32_t grantedBy,
      const std::string &clientId,
      const std::string &scope,
      BoolCallback &&cb
    );

    /// R-M2-4 revocation: revoke every ACTIVE row of the (org, client)
    /// pair (documented batch-UPDATE exemption -- the pair's rows are
    /// service-managed and the WHERE is fully scoped; same pattern as
    /// the #228 owner demotion). Reports the number of rows revoked; 0
    /// means "no active consents" (the caller renders 404, the
    /// removeMember count==0 convention).
    void revokeClientConsents(
      int32_t orgId,
      const std::string &clientId,
      CountCallback &&cb
    );

    /// R-M2-4 listing: every ACTIVE row of the org (the caller groups
    /// by client).
    void listActiveByOrg(int32_t orgId, RowsCallback &&cb);

  private:
    ::drogon::orm::DbClientPtr dbClient_;
};

}  // namespace fulla::storage::postgres
