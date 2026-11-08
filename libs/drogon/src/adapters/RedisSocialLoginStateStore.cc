#include <fulla/drogon/adapters/RedisSocialLoginStateStore.h>

#ifdef WITH_SOCIAL
#include <drogon/drogon.h>
#include <atomic>

namespace fulla::drogon::adapters
{
namespace
{
using Status = identity::SocialLoginStateStatus;
using Callback = identity::ISocialLoginStateStore::Callback;

std::string encode(const identity::SocialLoginContext &context)
{
    Json::Value json;
    json["provider"] = context.provider;
    json["client_id"] = context.clientId;
    json["redirect_uri"] = context.redirectUri;
    json["binding_hash"] = context.bindingHash;
    json["purpose"] = context.purpose;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, json);
}

// A disconnected Redis client may buffer commands without invoking its error
// callback. Bound the wait, and suppress late completion after a timeout.
Callback bounded(Callback &&cb)
{
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto callback = std::make_shared<Callback>(std::move(cb));
    Callback finish = [done, callback](Status status) {
        if (!done->exchange(true))
        {
            // A disconnected client can retain queued commands indefinitely.
            // Release request captures at completion even if Redis replies late.
            auto completion = std::move(*callback);
            completion(status);
        }
    };
    ::drogon::app().getLoop()->runAfter(5.0, [finish]() { finish(Status::Unavailable); });
    return finish;
}
}  // namespace

RedisSocialLoginStateStore::RedisSocialLoginStateStore(::drogon::nosql::RedisClientPtr redis)
    : redis_(std::move(redis)) {}

void RedisSocialLoginStateStore::reserve(
  const std::string &key, const identity::SocialLoginContext &context, Callback &&cb)
{
    if (!redis_)
    {
        cb(Status::Unavailable);
        return;
    }
    auto finish = bounded(std::move(cb));
    try
    {
        redis_->execCommandAsync(
          [finish](const ::drogon::nosql::RedisResult &result) {
              if (result.isNil())
                  finish(Status::Collision);
              else if (result.type() == ::drogon::nosql::RedisResultType::kStatus &&
                       result.asString() == "OK")
                  finish(Status::Ok);
              else
                  finish(Status::Unavailable);
          },
          [finish](const ::drogon::nosql::RedisException &) { finish(Status::Unavailable); },
          "SET %s %s NX EX 600", ("fulla:social:login-state:" + key).c_str(), encode(context).c_str());
    }
    catch (const std::exception &)
    {
        finish(Status::Unavailable);
    }
}

void RedisSocialLoginStateStore::consume(
  const std::string &key, const identity::SocialLoginContext &expected, Callback &&cb)
{
    if (!redis_)
    {
        cb(Status::Unavailable);
        return;
    }
    auto finish = bounded(std::move(cb));
    // Compare-and-delete: wrong browsers/providers must not consume a valid
    // transaction. Comparing the canonical record also fails closed on corruption.
    static constexpr char script[] =
      "local value=redis.call('GET',KEYS[1]); "
      "if not value or value~=ARGV[1] then return 0 end; "
      "redis.call('DEL',KEYS[1]); return 1";
    try
    {
        redis_->execCommandAsync(
          [finish](const ::drogon::nosql::RedisResult &result) {
              if (result.type() != ::drogon::nosql::RedisResultType::kInteger)
                  finish(Status::Unavailable);
              else
                  finish(result.asInteger() == 1 ? Status::Ok : Status::Invalid);
          },
          [finish](const ::drogon::nosql::RedisException &) { finish(Status::Unavailable); },
          "EVAL %s 1 %s %s", script, ("fulla:social:login-state:" + key).c_str(), encode(expected).c_str());
    }
    catch (const std::exception &)
    {
        finish(Status::Unavailable);
    }
}

}  // namespace fulla::drogon::adapters
#endif
