#pragma once

// P2-1: adapter-layer Postgres implementation of
// fulla::identity::IUpstreamProviderRepository against the generated
// UpstreamProviders ORM model (upstream_providers table). Same conventions
// as PostgresSocialAccountRepository: async-callback Mapper access, every
// Mapper construction individually guarded, failures surface through the
// callback (never a bare LOG_ERROR + return).

#include <fulla/identity/IUpstreamProviderRepository.h>

#include <drogon/orm/DbClient.h>

#include <memory>

namespace fulla::storage::postgres
{

class PostgresUpstreamProviderRepository
    : public fulla::identity::IUpstreamProviderRepository,
      public std::enable_shared_from_this<PostgresUpstreamProviderRepository>
{
  public:
    explicit PostgresUpstreamProviderRepository(::drogon::orm::DbClientPtr dbClient)
        : dbClient_(std::move(dbClient))
    {
    }

    void listAll(ListCallback &&cb) override;
    void listEnabled(ListCallback &&cb) override;
    void findByKey(const std::string &providerKey, FindCallback &&cb) override;

    void insertRecord(const fulla::identity::UpstreamProviderRecord &record,
                      MutationCallback &&cb) override;
    void updateRecord(const std::string &providerKey,
                      const fulla::identity::UpstreamProviderRecord &record,
                      MutationCallback &&cb) override;
    void deleteByKey(const std::string &providerKey, MutationCallback &&cb) override;

  private:
    ::drogon::orm::DbClientPtr dbClient_;
};

}  // namespace fulla::storage::postgres
