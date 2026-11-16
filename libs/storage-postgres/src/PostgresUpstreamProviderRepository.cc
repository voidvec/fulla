// PostgresUpstreamProviderRepository.cc -- P2-1 registry storage.
//
// All access is async-callback Mapper style (db-operations.md); each Mapper
// construction sits in its own try/catch and every failure path delivers the
// error through the callback. The UNIQUE(provider_key) violation is mapped
// to the stable "duplicate_key" error token the admin API reports as
// VALIDATION_RESOURCE_CONFLICT.

#include <fulla/storage/postgres/PostgresUpstreamProviderRepository.h>
#include <fulla/storage/postgres/models/UpstreamProviders.h>

#include <drogon/drogon.h>
#include <drogon/orm/Criteria.h>
#include <drogon/orm/Mapper.h>
#include <trantor/utils/Logger.h>

#include <utility>

namespace fulla::storage::postgres
{

namespace
{
using Record = fulla::identity::UpstreamProviderRecord;
using drogon_model::fulla_db::UpstreamProviders;

Record toRecord(const UpstreamProviders &row)
{
    Record r;
    r.id = row.getValueOfId();
    r.providerKey = row.getValueOfProviderKey();
    r.displayName = row.getValueOfDisplayName();
    r.issuer = row.getValueOfIssuer();
    r.clientId = row.getValueOfClientId();
    r.clientSecret = row.getValueOfClientSecret();
    r.scopes = row.getValueOfScopes();
    r.claimsMapping = row.getValueOfClaimsMapping();
    r.endpointOverrides = row.getValueOfEndpointOverrides();
    r.enabled = row.getValueOfEnabled();
    return r;
}

// UNIQUE(provider_key) collisions are the one expected constraint failure;
// everything else is a generic db_error.
std::string mutationError(const std::exception &e)
{
    const std::string what = e.what();
    if (what.find("upstream_providers_provider_key_key") != std::string::npos ||
        what.find("duplicate key value") != std::string::npos)
    {
        return "duplicate_key";
    }
    return "db_error";
}
}  // namespace

void PostgresUpstreamProviderRepository::listAll(ListCallback &&cb)
{
    auto sharedCb = std::make_shared<ListCallback>(std::move(cb));
    try
    {
        // findBy with a vector callback (the repository's documented idiom:
        // findOne conflates "zero rows" with "DB exception"). The id>0
        // predicate is the house "list everything" pattern.
        drogon::orm::Mapper<UpstreamProviders> mapper(dbClient_);
        mapper.findBy(
          drogon::orm::Criteria(UpstreamProviders::Cols::_id, drogon::orm::CompareOperator::GT, 0),
          [sharedCb](const std::vector<UpstreamProviders> &rows) {
              std::vector<Record> out;
              out.reserve(rows.size());
              for (const auto &row : rows)
              {
                  out.push_back(toRecord(row));
              }
              (*sharedCb)(std::move(out));
          },
          [sharedCb](const drogon::orm::DrogonDbException &e) {
              LOG_ERROR << "UpstreamProviderRepository listAll failed: " << e.base().what();
              (*sharedCb)({});
          });
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "UpstreamProviderRepository listAll mapper failed: " << e.what();
        (*sharedCb)({});
    }
}

void PostgresUpstreamProviderRepository::listEnabled(ListCallback &&cb)
{
    auto sharedCb = std::make_shared<ListCallback>(std::move(cb));
    try
    {
        drogon::orm::Mapper<UpstreamProviders> mapper(dbClient_);
        mapper.findBy(
          drogon::orm::Criteria(UpstreamProviders::Cols::_enabled,
                                drogon::orm::CompareOperator::EQ,
                                true),
          [sharedCb](const std::vector<UpstreamProviders> &rows) {
              std::vector<Record> out;
              out.reserve(rows.size());
              for (const auto &row : rows)
              {
                  out.push_back(toRecord(row));
              }
              (*sharedCb)(std::move(out));
          },
          [sharedCb](const drogon::orm::DrogonDbException &e) {
              LOG_ERROR << "UpstreamProviderRepository listEnabled failed: " << e.base().what();
              (*sharedCb)({});
          });
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "UpstreamProviderRepository listEnabled mapper failed: " << e.what();
        (*sharedCb)({});
    }
}

void PostgresUpstreamProviderRepository::findByKey(const std::string &providerKey,
                                                   FindCallback &&cb)
{
    auto sharedCb = std::make_shared<FindCallback>(std::move(cb));
    try
    {
        drogon::orm::Mapper<UpstreamProviders> mapper(dbClient_);
        mapper.findBy(
          drogon::orm::Criteria(UpstreamProviders::Cols::_provider_key,
                                drogon::orm::CompareOperator::EQ,
                                providerKey),
          [sharedCb](const std::vector<UpstreamProviders> &rows) {
              if (rows.empty())
              {
                  (*sharedCb)(std::nullopt);
                  return;
              }
              (*sharedCb)(toRecord(rows.front()));
          },
          [sharedCb](const drogon::orm::DrogonDbException &e) {
              LOG_ERROR << "UpstreamProviderRepository findByKey failed: " << e.base().what();
              (*sharedCb)(std::nullopt);
          });
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "UpstreamProviderRepository findByKey mapper failed: " << e.what();
        (*sharedCb)(std::nullopt);
    }
}

void PostgresUpstreamProviderRepository::insertRecord(const Record &record,
                                                      MutationCallback &&cb)
{
    auto sharedCb = std::make_shared<MutationCallback>(std::move(cb));
    try
    {
        UpstreamProviders row;
        row.setProviderKey(record.providerKey);
        row.setDisplayName(record.displayName);
        row.setIssuer(record.issuer);
        row.setClientId(record.clientId);
        row.setClientSecret(record.clientSecret);
        if (!record.scopes.empty())
            row.setScopes(record.scopes);
        if (!record.claimsMapping.empty())
            row.setClaimsMapping(record.claimsMapping);
        if (!record.endpointOverrides.empty())
            row.setEndpointOverrides(record.endpointOverrides);
        row.setEnabled(record.enabled);

        drogon::orm::Mapper<UpstreamProviders> mapper(dbClient_);
        auto self = shared_from_this();
        mapper.insert(
          std::move(row),
          [self, sharedCb](const UpstreamProviders &inserted) {
              (*sharedCb)(true, "", toRecord(inserted));
          },
          [sharedCb](const drogon::orm::DrogonDbException &e) {
              const std::string token = mutationError(e.base());
              LOG_ERROR << "UpstreamProviderRepository insert failed (" << token
                        << "): " << e.base().what();
              (*sharedCb)(false, token, std::nullopt);
          });
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "UpstreamProviderRepository insert mapper failed: " << e.what();
        (*sharedCb)(false, "db_error", std::nullopt);
    }
}

void PostgresUpstreamProviderRepository::updateRecord(const std::string &providerKey,
                                                      const Record &record,
                                                      MutationCallback &&cb)
{
    auto sharedCb = std::make_shared<MutationCallback>(std::move(cb));
    try
    {
        drogon::orm::Mapper<UpstreamProviders> mapper(dbClient_);
        auto self = shared_from_this();
        mapper.findBy(
          drogon::orm::Criteria(UpstreamProviders::Cols::_provider_key,
                                drogon::orm::CompareOperator::EQ,
                                providerKey),
          [self, record, sharedCb](const std::vector<UpstreamProviders> &rows) mutable {
              if (rows.empty())
              {
                  (*sharedCb)(false, "not_found", std::nullopt);
                  return;
              }
              UpstreamProviders row = rows.front();
              // provider_key itself is immutable (it keys oauth2_subject_
              // mappings); everything else is admin-editable.
              row.setDisplayName(record.displayName);
              row.setIssuer(record.issuer);
              row.setClientId(record.clientId);
              if (!record.clientSecret.empty())
                  row.setClientSecret(record.clientSecret);  // empty = keep current
              if (!record.scopes.empty())
                  row.setScopes(record.scopes);
              if (!record.claimsMapping.empty())
                  row.setClaimsMapping(record.claimsMapping);
              if (!record.endpointOverrides.empty())
                  row.setEndpointOverrides(record.endpointOverrides);
              row.setEnabled(record.enabled);
              row.setUpdatedAt(::trantor::Date::now());
              try
              {
                  drogon::orm::Mapper<UpstreamProviders> updateMapper(self->dbClient_);
                  updateMapper.update(
                    std::move(row),
                    [sharedCb](const size_t count) {
                        if (count == 0)
                        {
                            (*sharedCb)(false, "not_found", std::nullopt);
                            return;
                        }
                        (*sharedCb)(true, "", std::nullopt);
                    },
                    [sharedCb](const drogon::orm::DrogonDbException &e) {
                        const std::string token = mutationError(e.base());
                        LOG_ERROR << "UpstreamProviderRepository update failed (" << token
                                  << "): " << e.base().what();
                        (*sharedCb)(false, token, std::nullopt);
                    });
              }
              catch (const std::exception &e)
              {
                  LOG_ERROR << "UpstreamProviderRepository update mapper failed: " << e.what();
                  (*sharedCb)(false, "db_error", std::nullopt);
              }
          },
          [sharedCb](const drogon::orm::DrogonDbException &e) {
              LOG_ERROR << "UpstreamProviderRepository update lookup failed: " << e.base().what();
              (*sharedCb)(false, "db_error", std::nullopt);
          });
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "UpstreamProviderRepository update mapper failed: " << e.what();
        (*sharedCb)(false, "db_error", std::nullopt);
    }
}

void PostgresUpstreamProviderRepository::deleteByKey(const std::string &providerKey,
                                                     MutationCallback &&cb)
{
    auto sharedCb = std::make_shared<MutationCallback>(std::move(cb));
    try
    {
        drogon::orm::Mapper<UpstreamProviders> mapper(dbClient_);
        mapper.deleteBy(
          drogon::orm::Criteria(UpstreamProviders::Cols::_provider_key,
                                drogon::orm::CompareOperator::EQ,
                                providerKey),
          [sharedCb](const size_t count) {
              if (count == 0)
              {
                  (*sharedCb)(false, "not_found", std::nullopt);
                  return;
              }
              // Linked oauth2_subject_mappings rows stay behind by design:
              // a disabled provider's historical links outlive the registry
              // entry (re-creating the key re-attaches them).
              (*sharedCb)(true, "", std::nullopt);
          },
          [sharedCb](const drogon::orm::DrogonDbException &e) {
              LOG_ERROR << "UpstreamProviderRepository delete failed: " << e.base().what();
              (*sharedCb)(false, "db_error", std::nullopt);
          });
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "UpstreamProviderRepository delete mapper failed: " << e.what();
        (*sharedCb)(false, "db_error", std::nullopt);
    }
}

}  // namespace fulla::storage::postgres
