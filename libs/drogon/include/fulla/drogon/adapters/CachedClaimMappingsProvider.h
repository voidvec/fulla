#pragma once

// P1-5 benchmark follow-up (five-scenario gate): getEnabledMappings used to
// hit Postgres on EVERY userinfo/refresh/code issuance (StorageClaimMappings
// Provider::getEnabledMappings has no cache), which cost the userinfo
// scenario ~18% QPS. This decorator keeps the provider contract while
// caching the mapping list in process memory:
//
//   * invalidate() is called from every claim-mapping write path (the same
//     spots that already refresh the discovery claim-name cache), so CRUD
//     stays immediately visible — the PublicationChain e2e semantics hold;
//   * a long TTL (default 60s) bounds staleness for write paths outside
//     this process (multi-instance), documented as best-effort;
//   * the very first request after boot may still hit the DB once (lazy),
//     which keeps memory-mode deployments wiring-free as before.

#include <fulla/common/ports/IClaimMappingsProvider.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace fulla::drogon::adapters
{

class CachedClaimMappingsProvider : public fulla::common::ports::IClaimMappingsProvider
{
  public:
    CachedClaimMappingsProvider(
      std::shared_ptr<fulla::common::ports::IClaimMappingsProvider> inner,
      std::chrono::steady_clock::duration ttl = std::chrono::seconds(60)
    )
        : inner_(std::move(inner)), ttl_(ttl)
    {
    }

    void getEnabledMappings(MappingsCallback &&cb) override
    {
        std::optional<std::vector<fulla::common::ports::ClaimMappingEntry>> cached;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cache_.has_value() &&
                std::chrono::steady_clock::now() - cachedAt_ < ttl_)
            {
                // Copy under the lock: the callback may run on any thread
                // and outlive the cache slot.
                cached = *cache_;
            }
        }
        if (cached.has_value())
        {
            cb(std::move(*cached));
            return;
        }
        inner_->getEnabledMappings(
          [this, cb = std::make_shared<MappingsCallback>(std::move(cb))](
            std::vector<fulla::common::ports::ClaimMappingEntry> entries) mutable {
              {
                  std::lock_guard<std::mutex> lock(mutex_);
                  cache_ = entries;
                  cachedAt_ = std::chrono::steady_clock::now();
              }
              (*cb)(std::move(entries));
          });
    }

    /// Write-path invalidation: the next read re-queries. Called by the
    /// admin claim-mapping CRUD alongside the discovery cache refresh.
    void invalidate()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.reset();
    }

  private:
    std::shared_ptr<fulla::common::ports::IClaimMappingsProvider> inner_;
    const std::chrono::steady_clock::duration ttl_;
    std::mutex mutex_;
    std::optional<std::vector<fulla::common::ports::ClaimMappingEntry>> cache_;
    std::chrono::steady_clock::time_point cachedAt_{};
};

}  // namespace fulla::drogon::adapters
