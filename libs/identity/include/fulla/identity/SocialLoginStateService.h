#pragma once

#include <fulla/common/ports/ICryptoProvider.h>
#include <fulla/identity/ISocialLoginStateStore.h>

#include <memory>
#include <optional>

namespace fulla::identity
{

class SocialLoginStateService : public std::enable_shared_from_this<SocialLoginStateService>
{
  public:
    using IssueCallback = std::function<void(SocialLoginStateStatus, const std::string &)>;
    SocialLoginStateService(std::shared_ptr<ISocialLoginStateStore> store,
                            std::shared_ptr<common::ports::ICryptoProvider> crypto);
    static bool validHandle(const std::string &handle);
    std::optional<std::string> newBinding() const;
    std::string digest(const std::string &handle) const;
    void issue(const SocialLoginContext &context, IssueCallback &&callback);
    void consume(const std::string &handle, const SocialLoginContext &expected,
                 ISocialLoginStateStore::Callback &&callback);

  private:
    void issueAttempt(const SocialLoginContext &context,
                      const std::shared_ptr<IssueCallback> &callback, int attempts);
    std::shared_ptr<ISocialLoginStateStore> store_;
    std::shared_ptr<common::ports::ICryptoProvider> crypto_;
};

}  // namespace fulla::identity
