#pragma once
// libs/drogon/src/UserReadCache.h — src-internal (deliberately NOT under
// include/fulla: public-SDK surface would drift api-diff).
//
// Wave-2 P1 (docs/performance-optimization/optimization-wave-2-plan.md):
// Redis cache-aside for the two S6 userinfo read funnels —
//   OAuth2Plugin::getUserRoles  (roles, JSON array of role names)
//   OAuth2Plugin::getUserInfo   (profile JSON {id,username,email,email_verified})
// eliminating the 3 serial PG round-trips per userinfo request (~54% of its
// end-to-end latency per the instrumented hotspot report).
//
// Semantics (aligned with the RedisCached* decorators):
//   - miss → run the caller's fetch (the uncached path), fire-and-forget fill;
//   - soft-fail: any Redis error → fetch (never a wrong answer);
//   - negative cache: a profile miss (user absent) is cached 60s under a
//     "__user_absent__" marker (documented deviation from the client-repo
//     "don't cache nullopt" rule — public_sub is minted at registration and
//     userinfo is a read path, so the shadow window is harmless);
//   - roles may legitimately be an empty array — cached as "[]";
//   - key = the reader's subject string AS GIVEN (numeric id or public_sub
//     form). Write-path invalidation passes every form it knows (see
//     UserCacheInvalidator); a form that no write path can reach is bounded
//     by the TTL (120s roles / 300s profile).
//
// Singletons (one OAuth2Plugin per process): UserReadCache::instance() is
// configured by the plugin's cache branch; before configuration (or when the
// cache is disabled / Redis unavailable) it is disabled and every read goes
// straight to the fetch path.

#include <drogon/drogon.h>
#include <drogon/nosql/RedisClient.h>
#include <json/json.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fulla::drogon
{

namespace userreadcache
{
inline constexpr int kDefaultProfileTtlSeconds = 300;
inline constexpr int kDefaultRolesTtlSeconds = 120;
inline constexpr int kNegativeTtlSeconds = 60;
inline const std::string kAbsentMarker = "{\"__user_absent__\":true}";

inline std::string profileKey(const std::string &subject)
{
    return "fulla:cache:user:profile:" + subject;
}
inline std::string rolesKey(const std::string &subject)
{
    return "fulla:cache:user:roles:" + subject;
}
}  // namespace userreadcache

// ---------------------------------------------------------------------------
// Write-path invalidation registry (same pattern as ClientCacheInvalidator).
// The plugin registers a DEL hook when the cache is active; user/role write
// paths call invalidateUser(subject[, publicSub]) after a successful write.
// ---------------------------------------------------------------------------
class UserCacheInvalidator
{
  public:
    using Hook = std::function<void(const std::string &subject)>;

    static UserCacheInvalidator &instance()
    {
        static UserCacheInvalidator inst;
        return inst;
    }

    void registerHook(Hook hook)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hook_ = std::make_shared<Hook>(std::move(hook));
    }

    // DEL both cache kinds for every known subject form of the user.
    void invalidateUser(const std::string &subject, const std::string &publicSub = "")
    {
        std::shared_ptr<Hook> hook;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hook = hook_;
        }
        if (hook && *hook)
        {
            if (!subject.empty())
                (*hook)(subject);
            if (!publicSub.empty() && publicSub != subject)
                (*hook)(publicSub);
        }
    }

  private:
    UserCacheInvalidator() = default;
    std::mutex mutex_;
    std::shared_ptr<Hook> hook_;
};

// ---------------------------------------------------------------------------
// The cache-aside read cache itself.
// ---------------------------------------------------------------------------
class UserReadCache
{
  public:
    using RolesCallback = std::function<void(std::vector<std::string>)>;
    using ProfileCallback = std::function<void(std::optional<Json::Value>)>;

    static UserReadCache &instance()
    {
        static UserReadCache inst;
        return inst;
    }

    void configure(
      ::drogon::nosql::RedisClientPtr redisClient,
      int profileTtlSeconds = userreadcache::kDefaultProfileTtlSeconds,
      int rolesTtlSeconds = userreadcache::kDefaultRolesTtlSeconds
    )
    {
        redisClient_ = std::move(redisClient);
        profileTtl_ = profileTtlSeconds > 0 ? profileTtlSeconds : userreadcache::kDefaultProfileTtlSeconds;
        rolesTtl_ = rolesTtlSeconds > 0 ? rolesTtlSeconds : userreadcache::kDefaultRolesTtlSeconds;
    }

    bool enabled() const
    {
        return redisClient_ != nullptr;
    }

    // Clears the cached RedisClientPtr and in-process memo during plugin shutdown.
    void reset()
    {
        redisClient_.reset();
        std::lock_guard<std::mutex> lock(memoMutex_);
        memo_.clear();
    }

    // Called by the write-path invalidation hook (in-process, synchronous)
    // so a revoked user's memo never outlives the DEL.
    void dropMemo(const std::string &subject)
    {
        std::lock_guard<std::mutex> lock(memoMutex_);
        memo_.erase(subject);
    }

    using RolesFetch = std::function<void(const std::string &, RolesCallback)>;
    using ProfileFetch = std::function<void(const std::string &, ProfileCallback)>;

    // Roles: hit → parsed JSON array; miss/error → fetch (result filled back).
    // Wave-2 P4: the MGET piggybacks the PROFILE key so the follow-up
    // getProfile call (userinfo reads roles first, then the profile) is
    // served from an in-process memo without a second Redis round-trip.
    void getRoles(const std::string &subject, RolesCallback &&cb, RolesFetch fetch)
    {
        if (!enabled())
        {
            fetch(subject, std::move(cb));
            return;
        }
        auto sharedCb = std::make_shared<RolesCallback>(std::move(cb));
        auto sharedFetch = std::make_shared<RolesFetch>(std::move(fetch));
        auto redis = redisClient_;
        std::string rKey = userreadcache::rolesKey(subject);
        std::string pKey = userreadcache::profileKey(subject);
        redis->execCommandAsync(
          [this, sharedCb, subject, sharedFetch](const ::drogon::nosql::RedisResult &r) {
              if (r.type() == ::drogon::nosql::RedisResultType::kArray)
              {
                  const auto arr = r.asArray();
                  if (!arr.empty() && arr[0].type() == ::drogon::nosql::RedisResultType::kString)
                  {
                      // Piggyback: memo the profile payload for the follow-up
                      // getProfile call (one-shot, TTL-bounded, cleared by the
                      // invalidation hook).
                      if (arr.size() > 1 && arr[1].type() == ::drogon::nosql::RedisResultType::kString)
                          memoProfilePayload(subject, arr[1].asString());
                      Json::Value root;
                      Json::CharReaderBuilder rb;
                      std::string errs;
                      std::istringstream is(arr[0].asString());
                      if (Json::parseFromStream(rb, is, &root, &errs) && root.isArray())
                      {
                          std::vector<std::string> roles;
                          for (const auto &v : root)
                              roles.push_back(v.asString());
                          (*sharedCb)(std::move(roles));
                          return;
                      }
                      // Corrupt entry → fall through to fetch.
                  }
              }
              fetchAndFillRoles(subject, std::move(*sharedCb), *sharedFetch);
          },
          [this, sharedCb, subject, sharedFetch](const ::drogon::nosql::RedisException &) {
              // Soft-fail: Redis unhealthy → uncached path.
              fetchAndFillRoles(subject, std::move(*sharedCb), *sharedFetch);
          },
          "MGET %s %s",
          rKey.c_str(),
          pKey.c_str()
        );
    }

    // Profile: hit → parsed JSON object (absent marker → nullopt);
    // miss/error → fetch (result filled back; nullopt filled negatively).
    // A fresh piggyback memo (primed by getRoles' MGET) is consumed first —
    // no Redis round-trip for the common userinfo sequence.
    void getProfile(const std::string &subject, ProfileCallback &&cb, ProfileFetch fetch)
    {
        if (!enabled())
        {
            fetch(subject, std::move(cb));
            return;
        }
        std::string memoized;
        if (takeMemoizedProfile(subject, memoized))
        {
            if (memoized == userreadcache::kAbsentMarker)
            {
                cb(std::nullopt);
                return;
            }
            Json::Value root;
            Json::CharReaderBuilder rb;
            std::string errs;
            std::istringstream is(memoized);
            if (Json::parseFromStream(rb, is, &root, &errs) && root.isObject())
            {
                cb(std::move(root));
                return;
            }
            // Corrupt memo → fall through to Redis.
        }
        auto sharedCb = std::make_shared<ProfileCallback>(std::move(cb));
        auto sharedFetch = std::make_shared<ProfileFetch>(std::move(fetch));
        auto redis = redisClient_;
        std::string key = userreadcache::profileKey(subject);
        redis->execCommandAsync(
          [this, sharedCb, subject, sharedFetch](const ::drogon::nosql::RedisResult &r) {
              if (r.type() == ::drogon::nosql::RedisResultType::kString)
              {
                  if (r.asString() == userreadcache::kAbsentMarker)
                  {
                      (*sharedCb)(std::nullopt);
                      return;
                  }
                  Json::Value root;
                  Json::CharReaderBuilder rb;
                  std::string errs;
                  std::istringstream is(r.asString());
                  if (Json::parseFromStream(rb, is, &root, &errs) && root.isObject())
                  {
                      (*sharedCb)(std::move(root));
                      return;
                  }
                  // Corrupt entry → fall through to fetch.
              }
              fetchAndFillProfile(subject, std::move(*sharedCb), *sharedFetch);
          },
          [this, sharedCb, subject, sharedFetch](const ::drogon::nosql::RedisException &) {
              fetchAndFillProfile(subject, std::move(*sharedCb), *sharedFetch);
          },
          "GET %s",
          key.c_str()
        );
    }

  private:
    UserReadCache() = default;

    // --- piggyback memo (Wave-2 P4) -------------------------------------
    // One-shot, TTL-bounded in-process memo of the profile payload, primed
    // by getRoles' MGET and consumed by the immediately-following getProfile
    // (the userinfo read sequence). Cleared synchronously by the write-path
    // invalidation hook (dropMemo); kMemoTtl bounds a lost-clear.
    static constexpr auto kMemoTtl = std::chrono::seconds(2);

    void memoProfilePayload(const std::string &subject, const std::string &payload)
    {
        std::lock_guard<std::mutex> lock(memoMutex_);
        if (memo_.size() > 1024)
            pruneExpiredMemoLocked();
        memo_[subject] = {payload, std::chrono::steady_clock::now()};
    }

    bool takeMemoizedProfile(const std::string &subject, std::string &payloadOut)
    {
        std::lock_guard<std::mutex> lock(memoMutex_);
        auto it = memo_.find(subject);
        if (it == memo_.end())
            return false;
        const auto entry = it->second;
        memo_.erase(it);  // one-shot: consume on first read
        if (std::chrono::steady_clock::now() - entry.second > kMemoTtl)
            return false;
        payloadOut = entry.first;
        return true;
    }

    void pruneExpiredMemoLocked()
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = memo_.begin(); it != memo_.end();)
        {
            if (now - it->second.second > kMemoTtl)
                it = memo_.erase(it);
            else
                ++it;
        }
    }

    std::mutex memoMutex_;
    std::unordered_map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>>
      memo_;

    void fetchAndFillRoles(const std::string &subject, RolesCallback cb, const RolesFetch &fetch)
    {
        fetch(
          subject,
          [this, subject, cb = std::make_shared<RolesCallback>(std::move(cb))](
            std::vector<std::string> roles) mutable {
              fillRoles(subject, roles);
              (*cb)(std::move(roles));
          }
        );
    }

    void fetchAndFillProfile(
      const std::string &subject,
      ProfileCallback cb,
      const ProfileFetch &fetch
    )
    {
        fetch(
          subject,
          [this, subject, cb = std::make_shared<ProfileCallback>(std::move(cb))](
            std::optional<Json::Value> profile) mutable {
              fillProfile(subject, profile);
              (*cb)(std::move(profile));
          }
        );
    }

    void fillRoles(const std::string &subject, const std::vector<std::string> &roles)
    {
        if (!enabled())
            return;
        Json::Value arr(Json::arrayValue);
        for (const auto &r : roles)
            arr.append(r);
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        postFill(userreadcache::rolesKey(subject), Json::writeString(wb, arr), rolesTtl_);
    }

    void fillProfile(const std::string &subject, const std::optional<Json::Value> &profile)
    {
        if (!enabled())
            return;
        if (!profile)
        {
            // Negative cache (user absent).
            postFill(userreadcache::profileKey(subject), userreadcache::kAbsentMarker,
                     userreadcache::kNegativeTtlSeconds);
            return;
        }
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        postFill(userreadcache::profileKey(subject), Json::writeString(wb, *profile), profileTtl_);
    }

    void postFill(const std::string &key, const std::string &payload, int ttl)
    {
        auto redis = redisClient_;
        if (!redis)
            return;
        std::string k = key;
        redis->execCommandAsync(
          [](const ::drogon::nosql::RedisResult &) {},
          [k](const ::drogon::nosql::RedisException &e) {
              LOG_DEBUG << "UserReadCache: fill SET failed for " << k << ": " << e.what();
          },
          "SET %s %s EX %d",
          k.c_str(),
          payload.c_str(),
          ttl
        );
    }

    ::drogon::nosql::RedisClientPtr redisClient_;
    int profileTtl_ = userreadcache::kDefaultProfileTtlSeconds;
    int rolesTtl_ = userreadcache::kDefaultRolesTtlSeconds;
};

}  // namespace fulla::drogon
