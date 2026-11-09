#pragma once

#include <drogon/orm/DbClient.h>
#include <functional>
#include <string>
#include <vector>

namespace fulla::storage::postgres::detail
{
using ConsentTransaction = std::shared_ptr<::drogon::orm::Transaction>;

// Private to consent writers. Every asynchronous lock continuation has its
// own synchronous-exception guard; unrelated quota/succession callers keep
// their existing helper and behavior.
void withConsentTransaction(
  const ::drogon::orm::DbClientPtr &db,
  std::vector<std::string> keys,
  std::function<void(const ConsentTransaction &)> locked,
  std::function<void()> failed
);
}  // namespace fulla::storage::postgres::detail
