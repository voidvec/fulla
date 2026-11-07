#include <fulla/identity/SocialLoginStateService.h>

#include <algorithm>
#include <exception>

namespace fulla::identity
{

SocialLoginStateService::SocialLoginStateService(
  std::shared_ptr<ISocialLoginStateStore> store,
  std::shared_ptr<common::ports::ICryptoProvider> crypto)
    : store_(std::move(store)), crypto_(std::move(crypto)) {}

bool SocialLoginStateService::validHandle(const std::string &handle)
{
    return handle.size() == 64 && std::all_of(handle.begin(), handle.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::optional<std::string> SocialLoginStateService::newBinding() const
{
    if (!crypto_)
        return std::nullopt;
    unsigned char bytes[32];
    try
    {
        if (!crypto_->secureRandomBytes(bytes, sizeof(bytes)))
            return std::nullopt;
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string value;
    value.reserve(64);
    for (const auto byte : bytes)
    {
        value += hex[byte >> 4];
        value += hex[byte & 15];
    }
    return value;
}

std::string SocialLoginStateService::digest(const std::string &handle) const
{
    if (!crypto_)
        return {};
    try
    {
        auto hash = crypto_->sha256Hex(handle);
        return validHandle(hash) ? hash : std::string{};
    }
    catch (const std::exception &)
    {
        return {};
    }
}

void SocialLoginStateService::issue(const SocialLoginContext &context, IssueCallback &&callback)
{
    if (!store_ || context.provider.empty() || context.clientId.empty() ||
        context.redirectUri.empty() || !validHandle(context.bindingHash) || context.purpose != "login")
    {
        callback(SocialLoginStateStatus::Unavailable, "");
        return;
    }
    issueAttempt(context, std::make_shared<IssueCallback>(std::move(callback)), 3);
}

void SocialLoginStateService::issueAttempt(
  const SocialLoginContext &context, const std::shared_ptr<IssueCallback> &callback, int attempts)
{
    auto handle = newBinding();
    const auto key = handle ? digest(*handle) : std::string{};
    if (key.empty())
    {
        (*callback)(SocialLoginStateStatus::Unavailable, "");
        return;
    }
    auto self = shared_from_this();
    store_->reserve(key, context, [self, context, callback, handle = *handle, attempts](auto status) {
        if (status == SocialLoginStateStatus::Collision && attempts > 1)
        {
            self->issueAttempt(context, callback, attempts - 1);
            return;
        }
        (*callback)(status == SocialLoginStateStatus::Ok ? status : SocialLoginStateStatus::Unavailable,
                    status == SocialLoginStateStatus::Ok ? handle : std::string{});
    });
}

void SocialLoginStateService::consume(
  const std::string &handle, const SocialLoginContext &expected,
  ISocialLoginStateStore::Callback &&callback)
{
    if (!validHandle(handle))
    {
        callback(SocialLoginStateStatus::Invalid);
        return;
    }
    const auto key = digest(handle);
    if (!store_ || key.empty())
    {
        callback(SocialLoginStateStatus::Unavailable);
        return;
    }
    store_->consume(key, expected, std::move(callback));
}

}  // namespace fulla::identity
