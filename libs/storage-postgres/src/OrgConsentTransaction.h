#pragma once

#include <fulla/storage/postgres/AdvisoryLock.h>

#include <drogon/orm/DbClient.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fulla::storage::postgres::detail
{
using ConsentTransaction = std::shared_ptr<::drogon::orm::Transaction>;

// Private to consent writers: the void-error flavor of withAdvisoryXactLock.
// Guarded continuations, rollback-on-failure and lock ordering live in the
// shared helper (#323 unification); this adapter only drops the string
// detail channel the quota/succession callers keep.
inline void withConsentTransaction(
  const ::drogon::orm::DbClientPtr &db,
  std::vector<std::string> keys,
  std::function<void(const ConsentTransaction &)> locked,
  std::function<void()> failed
)
{
    withAdvisoryXactLock(
      db,
      std::move(keys),
      [locked = std::move(locked)](
        const std::shared_ptr<::drogon::orm::Transaction> &txn
      ) { locked(txn); },
      [failed = std::move(failed)](const std::string &) { failed(); }
    );
}
}  // namespace fulla::storage::postgres::detail
