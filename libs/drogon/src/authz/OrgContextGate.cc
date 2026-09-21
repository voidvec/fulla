// See OrgContextGate.h for the policy rationale (v1.5.0 M1, design §2.1
// item 5 / O1 / §2.5). Every hop is async callback + Mapper + Criteria via
// the shared repository, each failure mapped to the uniform Invalid
// decision (or StorageError, which the callers render identically).
// ClientOwnersRepository is a cheap value object (one DbClientPtr); each
// hop constructs its own instance instead of threading one through the
// async chain.

#include <fulla/drogon/authz/OrgContextGate.h>

#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/storage/postgres/ClientOwnersRepository.h>

#include <drogon/drogon.h>

#include <memory>
#include <optional>
#include <string>

namespace fulla::drogon::authz
{

namespace
{

using ::fulla::storage::postgres::ClientOwnersRepository;
using ::fulla::storage::postgres::LookupStatus;
using Decision = OrgContextDecision;

bool amrHasMfa(const std::string &amr)
{
    // amr is space-separated ("pwd", "mfa"); the id_token path derives
    // acr="2" from an "mfa" entry the same way (TokenService.cc).
    size_t s = 0;
    while (s < amr.size())
    {
        size_t e = amr.find(' ', s);
        if (e == std::string::npos)
            e = amr.size();
        if (e > s && amr.substr(s, e - s) == "mfa")
            return true;
        s = e + 1;
    }
    return false;
}

Decision uniformReject(LookupStatus status, const std::string &errorDetail)
{
    Decision d;
    if (status == LookupStatus::Error)
    {
        LOG_ERROR << "[OrgContextGate] lookup failed: " << errorDetail;
        d.kind = Decision::Kind::StorageError;
    }
    else
    {
        d.kind = Decision::Kind::Invalid;
    }
    return d;
}

}  // namespace

void OrgContextGate::validate(
  const std::string &orgRef,
  const std::string &clientId,
  int32_t internalUserId,
  const std::string &amr,
  DecisionCallback &&cb
)
{
    if (orgRef.empty() || clientId.empty())
    {
        Decision d;
        d.kind = Decision::Kind::None;
        cb(d);
        return;
    }

    // Memory-mode / DB-less deployments have no org rows at all: the org
    // parameter can never be valid there, and it must not crash the
    // request either (getDbClient() asserts without a configured client).
    // Storage-type-guarded (Debug builds assert, not throw, on an
    // unknown client name -- see the plugin wiring note): memory mode has
    // no org rows, so the hint can never be valid there either way. Same
    // signal the test suite's postgresAvailable() uses.
    ::drogon::orm::DbClientPtr db;
    auto *gatePlugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
    if (gatePlugin != nullptr && gatePlugin->getStorageType() != "memory")
    {
        try
        {
            db = ::drogon::app().getDbClient();
        }
        catch (...)
        {
            db = nullptr;
        }
    }
    if (!db)
    {
        LOG_WARN << "[OrgContextGate] no DB client; rejecting org_id hint";
        Decision d;
        d.kind = Decision::Kind::StorageError;
        cb(d);
        return;
    }

    auto sharedCb = std::make_shared<DecisionCallback>(std::move(cb));

    ClientOwnersRepository(db).findOrganization(
      orgRef,
      [db, sharedCb, clientId, internalUserId, amr](
        const ::fulla::storage::postgres::OrganizationLookup &lookup) {
          if (lookup.status != LookupStatus::Found)
          {
              (*sharedCb)(uniformReject(lookup.status, lookup.error));
              return;
          }

          const int32_t orgId = lookup.row.getValueOfId();
          const std::string orgName = lookup.row.getValueOfName();
          // §2.5 core minimal policy: orgs flagged require_mfa only lend
          // their context to an MFA-elevated session.
          const bool requireMfa = lookup.row.getValueOfRequireMfa();

          ClientOwnersRepository(db).findMembership(
            orgId,
            internalUserId,
            [db, sharedCb, clientId, amr, orgId, orgName, requireMfa](
              const ::fulla::storage::postgres::MembershipLookup &m) {
                if (m.status != LookupStatus::Found)
                {
                    (*sharedCb)(uniformReject(m.status, m.error));
                    return;
                }

                ClientOwnersRepository(db).findOwnerRow(
                  clientId,
                  [sharedCb, amr, orgId, orgName, requireMfa](
                    const ::fulla::storage::postgres::OwnerRowLookup &o) {
                      const bool related =
                        o.status == LookupStatus::Found && o.row.getOrgId() != nullptr &&
                        *o.row.getOrgId() == orgId;
                      if (!related)
                      {
                          (*sharedCb)(uniformReject(o.status, o.error));
                          return;
                      }

                      if (requireMfa && !amrHasMfa(amr))
                      {
                          Decision d;
                          d.kind = Decision::Kind::MfaRequired;
                          d.orgId = orgId;
                          d.orgName = orgName;
                          (*sharedCb)(d);
                          return;
                      }

                      Decision d;
                      d.kind = Decision::Kind::Proceed;
                      d.orgId = orgId;
                      d.orgName = orgName;
                      (*sharedCb)(d);
                  }
                );
            }
          );
      }
    );
}

}  // namespace fulla::drogon::authz
