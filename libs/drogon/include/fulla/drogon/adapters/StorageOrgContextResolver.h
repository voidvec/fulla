#pragma once

// v1.5.0 M1 (real-tenant design §2.1 item 3): adapter implementing the
// common IOrgContextResolver port for the assembled server. Mirrors the
// StorageRoleProvider pattern (an Adapter-layer class constructed with
// its storage dependency INJECTED by the composition root -- the port's
// whole point is that the Domain stays testable against fakes). Subject
// -> internal id uses the DUAL-KEY users lookup (public_sub | numeric
// id, the canonical /api/me pattern): login-issued codes carry the UUID
// public_sub while authorize/consent-issued ones carry the internal id
// string, and subject_mappings only knows the latter. O7's real-time
// semantics hold because every resolution re-reads CURRENT membership.

#include <fulla/common/ports/IOrgContextResolver.h>

#include <drogon/orm/DbClient.h>

#include <memory>
#include <string>

namespace fulla::drogon::adapters
{

class StorageOrgContextResolver : public fulla::common::ports::IOrgContextResolver
{
  public:
    /// `dbClient` may be nullptr (memory-mode deployments: every
    /// resolution resolves to nullopt -- no org rows exist there).
    explicit StorageOrgContextResolver(::drogon::orm::DbClientPtr dbClient)
        : dbClient_(std::move(dbClient))
    {
    }

    void resolveOrgContext(
      const std::string &subject,
      int32_t orgId,
      fulla::common::ports::IOrgContextResolver::OrgContextCallback &&cb
    ) override;

  private:
    ::drogon::orm::DbClientPtr dbClient_;
};

}  // namespace fulla::drogon::adapters
