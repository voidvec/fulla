#ifdef WITH_SOCIAL
#include <drogon/drogon_test.h>
#include <fulla/drogon/adapters/RedisSocialLoginStateStore.h>
#include "contract/ContractFixtures.h"
#include <atomic>
#include <future>

using LoginStateStatus = fulla::identity::SocialLoginStateStatus;
using LoginStateStore = fulla::drogon::adapters::RedisSocialLoginStateStore;
using fulla::test::contract::getRedisClientOrNull;
using fulla::test::contract::waitForValue;

DROGON_TEST(Integration_P0_SocialLogin_RedisAtomicCompareAndDelete)
{
    auto redis = getRedisClientOrNull();
    if (!redis) return;
    LoginStateStore store(redis);
    const auto digest = drogon::utils::getSha256(drogon::utils::getUuid());
    fulla::identity::SocialLoginContext context{"github", "client", "https://portal.test/callback/github", std::string(64, 'a'), "login"};
    auto result = waitForValue<LoginStateStatus>([&](auto cb) { store.reserve(digest, context, std::move(cb)); });
    CHECK(result == LoginStateStatus::Ok);
    result = waitForValue<LoginStateStatus>([&](auto cb) { store.reserve(digest, context, std::move(cb)); });
    CHECK(result == LoginStateStatus::Collision);
    auto ttl = redis->execCommandSync(
      [](const drogon::nosql::RedisResult &reply) { return reply.asInteger(); },
      "TTL %s", ("fulla:social:login-state:" + digest).c_str());
    CHECK(ttl > 590);
    CHECK(ttl <= 600);
    auto wrong = context;
    wrong.bindingHash = std::string(64, 'b');
    result = waitForValue<LoginStateStatus>([&](auto cb) { store.consume(digest, wrong, std::move(cb)); });
    CHECK(result == LoginStateStatus::Invalid);
    std::vector<std::future<LoginStateStatus>> results;
    for (int i = 0; i < 16; ++i)
    {
        auto promise = std::make_shared<std::promise<LoginStateStatus>>();
        results.push_back(promise->get_future());
        store.consume(digest, context, [promise](auto status) { promise->set_value(status); });
    }
    int winners = 0;
    for (auto &pending : results)
    {
        REQUIRE(pending.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        if (pending.get() == LoginStateStatus::Ok) ++winners;
    }
    CHECK(winners == 1);
    result = waitForValue<LoginStateStatus>([&](auto cb) { store.consume(digest, context, std::move(cb)); });
    CHECK(result == LoginStateStatus::Invalid);
    // Expiry and corruption both fail closed without requiring a ten-minute wait.
    redis->execCommandSync([](const drogon::nosql::RedisResult &reply) { return reply.asString(); },
      "SET %s corrupt EX 600", ("fulla:social:login-state:" + digest).c_str());
    result = waitForValue<LoginStateStatus>([&](auto cb) { store.consume(digest, context, std::move(cb)); });
    CHECK(result == LoginStateStatus::Invalid);
    redis->execCommandSync([](const drogon::nosql::RedisResult &reply) { return reply.asInteger(); },
      "DEL %s", ("fulla:social:login-state:" + digest).c_str());
    result = waitForValue<LoginStateStatus>([&](auto cb) { store.reserve(digest, context, std::move(cb)); });
    CHECK(result == LoginStateStatus::Ok);
    redis->execCommandSync([](const drogon::nosql::RedisResult &reply) { return reply.asInteger(); },
      "PEXPIREAT %s 1", ("fulla:social:login-state:" + digest).c_str());
    result = waitForValue<LoginStateStatus>([&](auto cb) { store.consume(digest, context, std::move(cb)); });
    CHECK(result == LoginStateStatus::Invalid);
}

DROGON_TEST(Integration_P0_SocialLogin_NullRedisFailsClosed)
{
    LoginStateStore store(nullptr);
    fulla::identity::SocialLoginContext context;
    auto result = waitForValue<LoginStateStatus>([&](auto cb) { store.reserve("key", context, std::move(cb)); });
    CHECK(result == LoginStateStatus::Unavailable);
    result = waitForValue<LoginStateStatus>([&](auto cb) { store.consume("key", context, std::move(cb)); });
    CHECK(result == LoginStateStatus::Unavailable);
}
#endif

#ifdef WITH_SOCIAL
DROGON_TEST(Integration_P0_SocialLogin_DisconnectedRedisHasBoundedCompletion)
{
    // No service listens on this reserved local port in the test environment.
    auto disconnected = drogon::nosql::RedisClient::newRedisClient(
      trantor::InetAddress("127.0.0.1", 1), 1, "");
    auto store = std::make_shared<LoginStateStore>(disconnected);
    // RedisClient owns an I/O thread. Keep it alive until process teardown;
    // releasing its last reference inside an I/O callback would join itself.
    static std::vector<std::shared_ptr<void>> keepAlive;
    keepAlive.push_back(disconnected);
    keepAlive.push_back(store);
    auto promise = std::make_shared<std::promise<LoginStateStatus>>();
    auto future = promise->get_future();
    auto callbacks = std::make_shared<std::atomic<int>>(0);
    const auto started = std::chrono::steady_clock::now();
    fulla::identity::SocialLoginContext context;
    store->reserve("unavailable", context, [promise, callbacks](auto status) {
        if (++*callbacks == 1) promise->set_value(status);
    });
    REQUIRE(future.wait_for(std::chrono::seconds(8)) == std::future_status::ready);
    CHECK(future.get() == LoginStateStatus::Unavailable);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(8));
    CHECK(callbacks->load() == 1);
}
#endif
