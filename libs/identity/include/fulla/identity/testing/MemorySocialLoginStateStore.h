#pragma once

#include <fulla/common/ports/IClock.h>
#include <fulla/identity/ISocialLoginStateStore.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace fulla::identity::testing
{

class MemorySocialLoginStateStore : public ISocialLoginStateStore
{
  public:
    explicit MemorySocialLoginStateStore(std::shared_ptr<common::ports::IClock> clock)
        : clock_(std::move(clock)) {}
    std::atomic<bool> available{true};

    void reserve(const std::string &key, const SocialLoginContext &context, Callback &&cb) override
    {
        if (!available || !clock_)
        {
            cb(SocialLoginStateStatus::Unavailable);
            return;
        }
        auto status = SocialLoginStateStatus::Ok;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = entries_.find(key);
            if (found != entries_.end() && found->second.expires > clock_->nowSeconds())
                status = SocialLoginStateStatus::Collision;
            else
                entries_[key] = {context, clock_->nowSeconds() + 600};
        }
        cb(status);
    }

    void consume(const std::string &key, const SocialLoginContext &expected, Callback &&cb) override
    {
        if (!available || !clock_)
        {
            cb(SocialLoginStateStatus::Unavailable);
            return;
        }
        auto status = SocialLoginStateStatus::Invalid;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = entries_.find(key);
            if (found != entries_.end())
            {
                if (found->second.expires <= clock_->nowSeconds())
                    entries_.erase(found);
                else if (found->second.context == expected)
                {
                    entries_.erase(found);
                    status = SocialLoginStateStatus::Ok;
                }
            }
        }
        cb(status);
    }

  private:
    struct Entry { SocialLoginContext context; int64_t expires; };
    std::shared_ptr<common::ports::IClock> clock_;
    std::mutex mutex_;
    std::map<std::string, Entry> entries_;
};

}  // namespace fulla::identity::testing
