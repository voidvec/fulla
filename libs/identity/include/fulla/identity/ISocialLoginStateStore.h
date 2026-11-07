#pragma once

#include <functional>
#include <string>

namespace fulla::identity
{

enum class SocialLoginStateStatus { Ok, Invalid, Unavailable, Collision };

struct SocialLoginContext
{
    std::string provider;
    std::string clientId;
    std::string redirectUri;
    std::string bindingHash;
    std::string purpose = "login";

    bool operator==(const SocialLoginContext &other) const
    {
        return provider == other.provider && clientId == other.clientId &&
               redirectUri == other.redirectUri && bindingHash == other.bindingHash &&
               purpose == other.purpose;
    }
};

// Keys are SHA-256 digests. Implementations must never store raw handles.
// consume() compares BEFORE deleting, atomically; a mismatch leaves the key.
class ISocialLoginStateStore
{
  public:
    using Callback = std::function<void(SocialLoginStateStatus)>;
    virtual ~ISocialLoginStateStore() = default;
    virtual void reserve(const std::string &key, const SocialLoginContext &context,
                         Callback &&callback) = 0;
    virtual void consume(const std::string &key, const SocialLoginContext &expected,
                         Callback &&callback) = 0;
};

}  // namespace fulla::identity
