#pragma once

// Shared client-ownership data access (#222, v1.5.0 M0). The same
// "owner row for this client" read used to exist in three shapes with
// three different error mappings (PostgresClientRepository fail-closed,
// ApplicationService everything-is-404 -- see #230 -- and the consent
// page's degrade-to-empty fan-out). This repository owns that read once;
// callers keep their own POLICY (suspension semantics, 404 uniformity,
// label degradation) but stop hand-rolling the queries.
//
// Deliberately NOT an IClientRepository-style Domain interface: the
// callers exchange Drogon ORM rows, and inventing DTO + interface layers
// here would double the moving parts of a predicate-extraction refactor.
// It is a concrete storage-postgres class consumed by storage-postgres,
// apps/server product services and libs/drogon controllers.
//
// Concurrency/lifetime: no member is captured into async continuations
// (the DbClientPtr is copied by value and the callback is shared), so the
// class needs neither enable_shared_from_this nor heap allocation --
// constructing one on the stack per call site is the intended use.

#include <drogon/orm/DbClient.h>

#include <fulla/storage/postgres/models/Oauth2ClientOwners.h>
#include <fulla/storage/postgres/models/OrganizationMembers.h>
#include <fulla/storage/postgres/models/Organizations.h>

#include <functional>
#include <optional>
#include <string>

namespace fulla::storage::postgres
{

/// Tri-state outcome shared by the row lookups.
enum class LookupStatus
{
    Found,  ///< exactly one row matched
    NoRow,  ///< zero rows matched (a legitimate outcome, not a failure)
    Error   ///< the query itself failed (connection, schema, ...)
};

/// Result of findOwnerRow(). `row` is default-constructed unless
/// status == Found; `error` carries the exception text when Error.
struct OwnerRowLookup
{
    LookupStatus status = LookupStatus::Error;
    ::drogon_model::fulla_db::Oauth2ClientOwners row{};
    std::string error;
};

/// Result of findMembership(). Same contract as OwnerRowLookup.
struct MembershipLookup
{
    LookupStatus status = LookupStatus::Error;
    ::drogon_model::fulla_db::OrganizationMembers row{};
    std::string error;
};

/// Result of findOrganization(). Same contract as OwnerRowLookup.
struct OrganizationLookup
{
    LookupStatus status = LookupStatus::Error;
    ::drogon_model::fulla_db::Organizations row{};
    std::string error;
};

using OwnerRowCallback = std::function<void(const OwnerRowLookup &)>;
using MembershipCallback = std::function<void(const MembershipLookup &)>;
using OwnerLabelCallback = std::function<void(const std::string &)>;

class ClientOwnersRepository
{
  public:
    /// The client must outlive nothing -- it is copied into each query.
    explicit ClientOwnersRepository(::drogon::orm::DbClientPtr dbClient);

    /// Single owners row for a self-registered client (client_id is the
    /// table's PK, so at most one row). #230: UnexpectedRows is mapped to
    /// NoRow here (uniform 404 semantics, #227); every other exception is
    /// surfaced as Error so a broken DB can no longer impersonate
    /// "no such application".
    void findOwnerRow(const std::string &clientId, OwnerRowCallback &&cb);

    /// Current membership row of `userId` in `orgId` (no soft-delete
    /// filter: organization_members has none; role POLICY -- owner/admin
    /// checks -- stays with the caller).
    void findMembership(int32_t orgId, int32_t userId, MembershipCallback &&cb);

    /// Organization row by reference: an all-digits `orgRef` resolves by
    /// integer id, anything else by slug (v1.5.0 M1's authorize org_id
    /// parameter accepts both shapes; design §2.1 item 2 / O6). The
    /// reference is matched with Criteria, never string-interpolated.
    void findOrganization(const std::string &orgRef, std::function<void(const OrganizationLookup &)> &&cb);

    /// Dual-key subject resolution (v1.5.0 M1): the canonical /api/me
    /// pattern (V024) -- an all-digits `subject` resolves by internal id,
    /// anything else by users.public_sub. Soft-deleted users never match.
    /// Used by the org-context paths whose subjects may be either the
    /// login-issued public_sub (UUID) or the session/internal id string.
    void findUserIdBySubject(
      const std::string &subject,
      std::function<void(std::optional<int32_t>)> &&cb
    );

    /// Consent-screen attribution label: an org app resolves to the
    /// organization's name, a personal app to the creator's display_name
    /// (username fallback), an admin-seeded client (no owners row) to "".
    /// Any lookup failure degrades to "" -- the pre-v1.4.0 consent URL
    /// shape -- never to an error (mechanical port of the AEC fan-out,
    /// zero behavior change).
    void resolveOwnerLabel(const std::string &clientId, OwnerLabelCallback &&cb);

  private:
    ::drogon::orm::DbClientPtr dbClient_;
};

}  // namespace fulla::storage::postgres
