#pragma once

#ifdef WITH_SOCIAL
#include <fulla/identity/ISocialLoginStateStore.h>
#include <drogon/nosql/RedisClient.h>

namespace fulla::drogon::adapters
{

class RedisSocialLoginStateStore : public identity::ISocialLoginStateStore
{
  public:
    explicit RedisSocialLoginStateStore(::drogon::nosql::RedisClientPtr redis);
    void reserve(const std::string &key, const identity::SocialLoginContext &context,
                 Callback &&callback) override;
    void consume(const std::string &key, const identity::SocialLoginContext &expected,
                 Callback &&callback) override;

  private:
    ::drogon::nosql::RedisClientPtr redis_;
};

}  // namespace fulla::drogon::adapters
#endif
