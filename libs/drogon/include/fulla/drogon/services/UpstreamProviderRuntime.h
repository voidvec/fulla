// UpstreamProviderRuntime.h
//
// P2-1 glue between the registry storage, the OIDC protocol service and the
// SocialLoginGuard. One instance per process (built by IdentityAssembly in
// postgres deployments):
//
//   * refresh() snapshots the enabled upstream_providers rows, pre-resolves
//     each one's endpoints (endpoint_overrides win; discovery cached inside
//     OidcUpstreamAuthService) and pushes a DynamicProvider set into the
//     guard -- after that, authorize/validate for registry keys ride the
//     exact same state/cookie/origin machinery as google/wechat/github.
//   * A 60s refresh loop (wired via app().getLoop()->runEvery) plus an
//     immediate refresh on every admin mutation keeps the set current.
//   * Login exchange reads the cached record (client secret included) plus
//     the cached endpoints; both stay out of the guard's synchronous layer.
//
// Header-only inline (adapters-layer style): no new .cc to link, wiring is
// IdentityAssembly's single include.

#pragma once

#ifdef WITH_SOCIAL

#include <fulla/drogon/services/SocialLoginGuard.h>
#include <fulla/identity/OidcUpstreamAuthService.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fulla::drogon::services
{

class UpstreamProviderRuntime : public std::enable_shared_from_this<UpstreamProviderRuntime>
{
  public:
    using Record = fulla::identity::UpstreamProviderRecord;
    using RecordCallback = std::function<void(bool ok, Record record)>;

    UpstreamProviderRuntime(std::shared_ptr<fulla::identity::IUpstreamProviderRepository> repo,
                            std::shared_ptr<fulla::identity::OidcUpstreamAuthService> oidcService,
                            std::shared_ptr<SocialLoginGuard> guard,
                            std::function<std::string(const std::string &providerKey)> redirectUriFor)
        : repo_(std::move(repo)),
          oidcService_(std::move(oidcService)),
          guard_(std::move(guard)),
          redirectUriFor_(std::move(redirectUriFor))
    {
    }

    // Re-target the guard that receives dynamic providers. The production
    // wiring sets the app guard at construction; the social mock fixture
    // swaps the controllers onto a test guard and must re-bind here so the
    // registry keeps functioning inside the test process (single guard
    // instance per process either way).
    void setGuardTarget(std::shared_ptr<SocialLoginGuard> guard)
    {
        guard_ = std::move(guard);
        // Push the current snapshot into the new target immediately.
        std::vector<SocialLoginGuard::DynamicProvider> dynamic;
        {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            dynamic = buildDynamic(snapshotRecords_, snapshotEndpoints_);
        }
        if (guard_)
        {
            guard_->setDynamicProviders(std::move(dynamic));
        }
    }

    // Snapshot enabled rows -> resolve endpoints -> guard. Safe to call
    // concurrently; last writer wins.
    void refresh(std::function<void()> &&done = nullptr)
    {
        auto self = shared_from_this();
        auto sharedDone =
          std::make_shared<std::function<void()>>(std::move(done));
        repo_->listEnabled([self, sharedDone](std::vector<Record> records) {
            if (records.empty())
            {
                self->commit({});
                if (*sharedDone)
                    (*sharedDone)();
                return;
            }
            // Resolve endpoints for every record; join by counter.
            struct Snapshot
            {
                std::vector<Record> records;
                std::map<std::string, fulla::identity::OidcUpstreamAuthService::OidcEndpoints>
                  endpoints;
                size_t remaining;
            };
            auto snap = std::make_shared<Snapshot>();
            snap->records = std::move(records);
            snap->remaining = snap->records.size();
            for (const auto &record : snap->records)
            {
                self->oidcService_->resolveEndpoints(
                  record,
                  [self, snap, sharedDone, key = record.providerKey](
                    bool ok,
                    fulla::identity::OidcUpstreamAuthService::OidcEndpoints endpoints) mutable {
                      std::unique_lock<std::mutex> lock(self->snapshotMutex_);
                      if (ok)
                      {
                          (*snap).endpoints[key] = std::move(endpoints);
                      }
                      if (--(*snap).remaining == 0)
                      {
                          // Move out under the last-callback's lock.
                          auto records = std::move((*snap).records);
                          auto endpointsMap = std::move((*snap).endpoints);
                          lock.unlock();
                          self->commit(std::move(records), std::move(endpointsMap));
                          if (*sharedDone)
                              (*sharedDone)();
                      }
                  });
            }
        });
    }

    // Cached lookup for the login-exchange path: the record is returned
    // only when the key is enabled; pair with endpointsFor() for the
    // protocol endpoints of the same refresh cycle.
    void findForLogin(const std::string &providerKey, RecordCallback &&cb)
    {
        {
            std::unique_lock<std::mutex> lock(snapshotMutex_);
            for (const auto &record : snapshotRecords_)
            {
                if (record.providerKey == providerKey)
                {
                    Record recordCopy = record;  // callback runs outside the lock
                    lock.unlock();
                    cb(true, recordCopy);
                    return;
                }
            }
        }
        cb(false, Record{});
    }

    // Endpoints snapshot for a key (empty authorize endpoint = unresolved).
    fulla::identity::OidcUpstreamAuthService::OidcEndpoints endpointsFor(
      const std::string &providerKey) const
    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        auto it = snapshotEndpoints_.find(providerKey);
        return it == snapshotEndpoints_.end()
                 ? fulla::identity::OidcUpstreamAuthService::OidcEndpoints{}
                 : it->second;
    }

    // Login-page listing copy of the enabled records.
    std::vector<Record> enabledSnapshot() const
    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        return snapshotRecords_;
    }

  private:
    // Dynamic-provider projection of a (records, endpoints) snapshot; rows
    // without a resolvable authorize endpoint are never advertised.
    std::vector<SocialLoginGuard::DynamicProvider> buildDynamic(
      const std::vector<Record> &records,
      const std::map<std::string, fulla::identity::OidcUpstreamAuthService::OidcEndpoints>
        &endpoints) const
    {
        std::vector<SocialLoginGuard::DynamicProvider> dynamic;
        for (const auto &record : records)
        {
            auto it = endpoints.find(record.providerKey);
            if (it == endpoints.end() || it->second.authorize.empty())
                continue;
            SocialLoginGuard::DynamicProvider entry;
            entry.key = record.providerKey;
            entry.clientId = record.clientId;
            entry.redirectUri = redirectUriFor_(record.providerKey);
            entry.authorizeEndpoint = it->second.authorize;
            entry.scopes = record.scopes;
            dynamic.push_back(std::move(entry));
        }
        return dynamic;
    }

  public:
    void commit(std::vector<Record> records,
                std::map<std::string, fulla::identity::OidcUpstreamAuthService::OidcEndpoints>
                  endpoints = {})
    {
        auto dynamic = buildDynamic(records, endpoints);
        {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            snapshotRecords_ = std::move(records);
            snapshotEndpoints_ = std::move(endpoints);
        }
        if (guard_)
        {
            guard_->setDynamicProviders(std::move(dynamic));
        }
    }

    std::shared_ptr<fulla::identity::IUpstreamProviderRepository> repo_;
    std::shared_ptr<fulla::identity::OidcUpstreamAuthService> oidcService_;
    std::shared_ptr<SocialLoginGuard> guard_;
    std::function<std::string(const std::string &)> redirectUriFor_;

  public:
    // Admin CRUD path: direct repository access through the single owner.
    std::shared_ptr<fulla::identity::IUpstreamProviderRepository> repository() const
    {
        return repo_;
    }
    // Login-exchange path needs the protocol service directly.
    std::shared_ptr<fulla::identity::OidcUpstreamAuthService> oidcService() const
    {
        return oidcService_;
    }

    mutable std::mutex snapshotMutex_;
    std::vector<Record> snapshotRecords_;
    std::map<std::string, fulla::identity::OidcUpstreamAuthService::OidcEndpoints>
      snapshotEndpoints_;
};

}  // namespace fulla::drogon::services

#endif  // WITH_SOCIAL
