// See OrgConsentRepository.h for the design anchors (v1.5.0 M2 §2.2,
// rulings R-M2-1/2/4/5/6). Every hop is async callback + Mapper +
// Criteria; the two raw-SQL statements (upsert, batch revoke) carry
// their exemption rationale inline.

#include <fulla/storage/postgres/OrgConsentRepository.h>

#include <fulla/storage/postgres/AdvisoryLock.h>
#include <fulla/storage/postgres/models/OrganizationMembers.h>

#include <drogon/drogon.h>

#include <memory>

namespace fulla::storage::postgres
{

using namespace ::drogon::orm;
using namespace ::drogon_model::fulla_db;

OrgConsentRepository::OrgConsentRepository(::drogon::orm::DbClientPtr dbClient)
    : dbClient_(std::move(dbClient))
{
}

void OrgConsentRepository::hasActiveConsentForUser(
  int32_t internalUserId,
  const std::string &clientId,
  const std::string &scope,
  BoolCallback &&cb
)
{
    auto sharedCb = std::make_shared<BoolCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(false);
        return;
    }

    // Hop 1: the user's CURRENT memberships (the org a user was removed
    // from stops contributing to the union immediately -- R-M2-1's
    // "当前所属" wording, same real-time semantics as O7).
    try
    {
        Mapper<OrganizationMembers> mapper(dbClient_);
        mapper.findBy(
          Criteria(
            OrganizationMembers::Cols::_user_id, CompareOperator::EQ, internalUserId
          ),
          [this, sharedCb, clientId, scope](const std::vector<OrganizationMembers> &rows) {
              std::vector<int32_t> orgIds;
              orgIds.reserve(rows.size());
              for (const auto &m : rows)
                  orgIds.push_back(m.getValueOfOrganizationId());
              if (orgIds.empty())
              {
                  (*sharedCb)(false);
                  return;
              }
              // Hop 2: any ACTIVE (org, client, scope) row among those
              // orgs. The V036 partial index idx_org_consents_active
              // backs this exact shape.
              try
              {
                  Mapper<OrganizationConsents> consentMapper(dbClient_);
                  consentMapper.findBy(
                    Criteria(
                      OrganizationConsents::Cols::_organization_id, CompareOperator::In,
                      orgIds
                    ) &&
                      Criteria(
                        OrganizationConsents::Cols::_client_id, CompareOperator::EQ, clientId
                      ) &&
                      Criteria(
                        OrganizationConsents::Cols::_scope_name, CompareOperator::EQ, scope
                      ) &&
                      Criteria(
                        OrganizationConsents::Cols::_revoked_at, CompareOperator::IsNull
                      ),
                    [sharedCb](const std::vector<OrganizationConsents> &consents) {
                        (*sharedCb)(!consents.empty());
                    },
                    [sharedCb](const DrogonDbException &e) {
                        LOG_ERROR << "hasActiveConsentForUser consent query error: "
                                  << e.base().what();
                        (*sharedCb)(false);
                    }
                  );
              }
              catch (...)
              {
                  LOG_ERROR << "hasActiveConsentForUser consent Mapper construction failed";
                  (*sharedCb)(false);
              }
          },
          [sharedCb](const DrogonDbException &e) {
              LOG_ERROR << "hasActiveConsentForUser membership query error: "
                        << e.base().what();
              (*sharedCb)(false);
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "hasActiveConsentForUser membership Mapper construction failed";
        (*sharedCb)(false);
    }
}

void OrgConsentRepository::saveConsent(
  int32_t orgId,
  int32_t grantedBy,
  const std::string &clientId,
  const std::string &scope,
  BoolCallback &&cb
)
{
    auto sharedCb = std::make_shared<BoolCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(false);
        return;
    }

    // R-M2-5: serialize grant/revoke per (org, client) so an upsert
    // cannot interleave against a revocation. Key per the ruling:
    // orgconsent:<org_id>:<client_id>.
    withAdvisoryXactLock(
      dbClient_,
      {"orgconsent:" + std::to_string(orgId) + ":" + clientId},
      [sharedCb, orgId, grantedBy, clientId, scope](
        const std::shared_ptr<::drogon::orm::Transaction> &txn) {
          // Success is reported from the COMMIT callback (the row is
          // durable then); the upsert error path reports inline (the
          // transaction rolls back and a commit callback registered
          // here never fires -- no double invocation).
          txn->setCommitCallback(
            [sharedCb](bool committed) { (*sharedCb)(committed); }
          );
          // R-M2-6: INSERT ... ON CONFLICT DO UPDATE (raw-SQL exemption)
          // -- re-granting a revoked row revives it and reattributes the
          // grant; the UNIQUE (organization_id, client_id, scope_name)
          // constraint is the conflict target.
          txn->execSqlAsync(
            "INSERT INTO organization_consents "
            "(organization_id, client_id, scope_name, granted_by, granted_at, revoked_at) "
            "VALUES ($1, $2, $3, $4, CURRENT_TIMESTAMP, NULL) "
            "ON CONFLICT (organization_id, client_id, scope_name) DO UPDATE SET "
            "revoked_at = NULL, granted_by = EXCLUDED.granted_by, "
            "granted_at = CURRENT_TIMESTAMP",
            [txn](const Result &) {},
            [sharedCb, txn](const DrogonDbException &e) {
                LOG_ERROR << "saveConsent upsert failed: " << e.base().what();
                (*sharedCb)(false);
            },
            orgId,
            clientId,
            scope,
            grantedBy
          );
      },
      [sharedCb](const std::string &err) {
          LOG_ERROR << "saveConsent advisory lock path failed: " << err;
          (*sharedCb)(false);
      }
    );
}

void OrgConsentRepository::revokeClientConsents(
  int32_t orgId,
  const std::string &clientId,
  CountCallback &&cb
)
{
    auto sharedCb = std::make_shared<CountCallback>(std::move(cb));

    if (!dbClient_)
    {
        (*sharedCb)(0);
        return;
    }

    // Documented batch-UPDATE exemption (R-M2-4: revoke the whole pair,
    // never a physical delete -- O4 keeps history): the pair's rows are
    // service-managed and the WHERE is fully scoped by org + client +
    // active. revocation only affects FUTURE authorizations; issued
    // tokens are not touched (O4/V9). affectedRows() (not size()) --
    // an UPDATE result carries no rows on the PG driver.
    dbClient_->execSqlAsync(
      "UPDATE organization_consents SET revoked_at = CURRENT_TIMESTAMP "
      "WHERE organization_id = $1 AND client_id = $2 AND revoked_at IS NULL",
      [sharedCb](const Result &r) { (*sharedCb)(r.affectedRows()); },
      [sharedCb](const DrogonDbException &e) {
          LOG_ERROR << "revokeClientConsents failed: " << e.base().what();
          (*sharedCb)(0);
      },
      orgId,
      clientId
    );
}

void OrgConsentRepository::listActiveByOrg(int32_t orgId, RowsCallback &&cb)
{
    auto sharedCb = std::make_shared<RowsCallback>(std::move(cb));

    if (!dbClient_)
    {
        static const std::vector<OrganizationConsents> kEmpty{};
        (*sharedCb)(kEmpty);
        return;
    }

    try
    {
        Mapper<OrganizationConsents> mapper(dbClient_);
        mapper.findBy(
          Criteria(
            OrganizationConsents::Cols::_organization_id, CompareOperator::EQ, orgId
          ) &&
            Criteria(OrganizationConsents::Cols::_revoked_at, CompareOperator::IsNull),
          [sharedCb](const std::vector<OrganizationConsents> &rows) { (*sharedCb)(rows); },
          [sharedCb](const DrogonDbException &e) {
              LOG_ERROR << "listActiveByOrg failed: " << e.base().what();
              static const std::vector<OrganizationConsents> kEmpty{};
              (*sharedCb)(kEmpty);
          }
        );
    }
    catch (...)
    {
        LOG_ERROR << "listActiveByOrg Mapper construction failed";
        static const std::vector<OrganizationConsents> kEmpty{};
        (*sharedCb)(kEmpty);
    }
}

}  // namespace fulla::storage::postgres
