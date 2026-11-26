// tests/integration/concurrency/CategoryB_JwkManagerRaceTest.cc
//
// Regression coverage for defect 1.5 (JwkManager init-vs-read data race).
// The legacy case ID is retained for existing test filters.
//
// The production contract is init-before-publish / read-only afterwards:
// OAuth2Plugin publishes the manager as std::shared_ptr<const JwkManager>
// (asserted below). The implementation additionally serializes init() and
// atomically publishes one immutable key snapshot, so readers are safe even
// before or during the first init(), and copy/move construction and assignment
// serialize with init() and with assignment on the other object. The tests
// below cover both the published read-only contract and the internal
// concurrent-init / assignment safety.
//
// Spec: concurrency-lifetime-safety-audit — Task 2 (Category B reproduction).
// Property 2: Bug Condition — Data-Race Freedom (TSan) — covers defect 1.5.
// _Requirements: 2.4, 2.5, 2.7_

#include <drogon/drogon_test.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/common/ports/ILogger.h>
#include <fulla/oauth2/jwk/JwkManager.h>
#include <json/json.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "ConcurrencyRaceSupport.h"

using namespace fulla::test::concurrency;
using fulla::oauth2::JwkManager;

namespace
{
static_assert(std::is_same_v<
              decltype(std::declval<const ::OAuth2Plugin &>().getJwkManager()),
              std::shared_ptr<const JwkManager>>,
              "Production must publish a read-only JWK manager");

struct ReaderResult
{
    bool ok = true;
    int completed = 0;
};

Json::Value samplePayload()
{
    Json::Value payload;
    payload["sub"] = "user-123";
    payload["iss"] = "https://issuer.example";
    payload["aud"] = "test-client";
    payload["iat"] = Json::Int64{1700000000};
    payload["exp"] = Json::Int64{1700003600};
    return payload;
}

class CountingLogger final : public fulla::common::ports::ILogger
{
  public:
    void log(fulla::common::ports::LogLevel, const std::string &) override
    {
        count.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<int> count{0};
};
}  // namespace

DROGON_TEST(Integration_P1_Concurrency_1_5_JwkManager_InitVsSign_DataRace_Repro)
{
    fulla::test::concurrency::InterleavingGenerator generator(0x0B05C0DEu);
    constexpr int kRounds = 8;
    constexpr int kReadsPerThread = 8;
    for (int round = 0; round < kRounds; ++round)
    {
        auto builder = std::make_shared<JwkManager>();
        REQUIRE(builder->init(Json::Value(Json::objectValue)));
        std::shared_ptr<const JwkManager> published = builder;
        const auto claims = samplePayload();
        const auto expectedKid = published->getKeyId();
        const auto expectedJwks = published->getJwks();
        const auto expectedJwt = published->signJwt(claims);
        REQUIRE(published->isInitialized());
        REQUIRE(!expectedKid.empty());
        REQUIRE(!expectedJwt.empty());

        const int readers = generator.threadCount(3, 6);
        auto results = std::make_shared<std::vector<ReaderResult>>(static_cast<size_t>(readers));
        auto start = std::make_shared<std::promise<void>>();
        const auto release = start->get_future().share();
        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(readers));
        std::exception_ptr spawnFailure;
        try
        {
            for (int index = 0; index < readers; ++index)
            {
                threads.emplace_back([published, results, release, claims, expectedKid,
                                      expectedJwks, expectedJwt, index,
                                      readsPerThread = kReadsPerThread]() {
                    auto &result = (*results)[static_cast<size_t>(index)];
                    try
                    {
                        if (release.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
                        {
                            result.ok = false;
                            return;
                        }
                        for (int read = 0; read < readsPerThread; ++read)
                        {
                            const auto jwt = published->signJwt(claims);
                            const auto jwks = published->getJwks();
                            const auto verified = published->verifyJwt(
                              jwt, "https://issuer.example", 1700000001, "test-client"
                            );
                            const bool stable = published->isInitialized() &&
                                                published->getKeyId() == expectedKid &&
                                                jwks == expectedJwks && !jwt.empty() &&
                                                jwt == expectedJwt &&
                                                verified == JwkManager::JwtVerificationResult::Ok;
                            result.ok = result.ok && stable;
                            ++result.completed;
                        }
                    }
                    catch (...)
                    {
                        result.ok = false;
                    }
                });
            }
        }
        catch (...)
        {
            spawnFailure = std::current_exception();
        }
        // Release and join even when only some threads could be constructed.
        start->set_value();
        for (auto &thread : threads)
            thread.join();
        REQUIRE(!spawnFailure);
        for (const auto &result : *results)
        {
            CHECK(result.ok);
            CHECK(result.completed == kReadsPerThread);
        }

        // Reinitialization remains a no-op, after all concurrent reads finish.
        Json::Value replacement;
        replacement["kid"] = "must-not-replace-the-published-key";
        CHECK(builder->init(replacement));
        const bool unchanged = published->getKeyId() == expectedKid &&
                               published->getJwks() == expectedJwks &&
                               published->signJwt(claims) == expectedJwt;
        CHECK(unchanged);
    }
}

DROGON_TEST(Integration_P1_Concurrency_1_5_JwkManager_ConcurrentInitAndReaders_AreConsistent)
{
    constexpr int kRounds = 2;
    constexpr int kWriters = 4;
    constexpr int kReaders = 6;

    for (int round = 0; round < kRounds; ++round)
    {
        JwkManager jwk;
        Json::Value cfg;
        cfg["kid"] = "concurrent-init-key";
        SpinBarrier barrier(kWriters + kReaders + 1);
        std::atomic<int> writersFinished{0};
        std::atomic<int> successfulInits{0};
        std::atomic<int> readersReady{0};
        std::atomic<int> preInitReads{0};
        std::atomic<int> readerCalls{0};
        std::atomic<int> usableSnapshots{0};
        std::vector<std::thread> threads;
        threads.reserve(kWriters + kReaders);

        for (int i = 0; i < kWriters; ++i)
        {
            threads.emplace_back([&]() {
                barrier.arriveAndWait();
                if (jwk.init(cfg))
                    successfulInits.fetch_add(1, std::memory_order_relaxed);
                writersFinished.fetch_add(1, std::memory_order_release);
            });
        }
        for (int i = 0; i < kReaders; ++i)
        {
            threads.emplace_back([&]() {
                const auto payload = samplePayload();
                (void)jwk.signJwt(payload);
                (void)jwk.getJwks();
                (void)jwk.getKeyId();
                (void)jwk.isInitialized();
                preInitReads.fetch_add(1, std::memory_order_release);
                readersReady.fetch_add(1, std::memory_order_release);
                barrier.arriveAndWait();
                readerCalls.fetch_add(1, std::memory_order_relaxed);
                while (writersFinished.load(std::memory_order_acquire) < kWriters)
                {
                    (void)jwk.signJwt(payload);
                    (void)jwk.getJwks();
                    (void)jwk.getKeyId();
                    (void)jwk.isInitialized();
                    readerCalls.fetch_add(1, std::memory_order_relaxed);
                }
                if (!jwk.signJwt(payload).empty())
                    usableSnapshots.fetch_add(1, std::memory_order_relaxed);
            });
        }
        while (readersReady.load(std::memory_order_acquire) < kReaders)
        {
            std::this_thread::yield();
        }
        barrier.arriveAndWait();
        for (auto &thread : threads)
            thread.join();

        CHECK(successfulInits.load(std::memory_order_relaxed) == kWriters);
        CHECK(preInitReads.load(std::memory_order_relaxed) == kReaders);
        CHECK(readerCalls.load(std::memory_order_relaxed) >= kReaders);
        CHECK(usableSnapshots.load(std::memory_order_relaxed) == kReaders);
        CHECK(jwk.getKeyId() == "concurrent-init-key");
        CHECK(jwk.getJwks()["keys"].size() == 1u);

        auto payload = samplePayload();
        const auto nowSecs = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch()
        )
                               .count();
        payload["exp"] = Json::Int64(nowSecs + 300);
        const auto jwt = jwk.signJwt(payload);
        REQUIRE(!jwt.empty());
        CHECK(
          jwk.verifyJwt(jwt, payload["iss"].asString(), nowSecs) ==
          JwkManager::JwtVerificationResult::Ok
        );
    }
}

DROGON_TEST(Integration_P1_Concurrency_1_5_JwkManager_ConcurrentAssignmentAndInit_AreSafe)
{
    CountingLogger firstLogger;
    CountingLogger secondLogger;
    Json::Value firstConfig;
    firstConfig["kid"] = "assignment-key-1";
    Json::Value secondConfig;
    secondConfig["kid"] = "assignment-key-2";

    JwkManager first(&firstLogger);
    JwkManager second(&secondLogger);
    REQUIRE(first.init(firstConfig));
    REQUIRE(second.init(secondConfig));

    JwkManager target;
    SpinBarrier barrier(3);
    std::atomic<bool> start{false};
    std::atomic<int> readerCalls{0};
    constexpr int kAssignmentRounds = 2000;

    std::thread writer([&]() {
        barrier.arriveAndWait();
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        for (int i = 0; i < kAssignmentRounds; ++i)
        {
            if (i % 2 == 0)
            {
                target = first;
                (void)first.init(firstConfig);
            }
            else
            {
                target = second;
                (void)second.init(secondConfig);
            }
        }
    });
    std::thread reader([&]() {
        const auto payload = samplePayload();
        barrier.arriveAndWait();
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        do
        {
            (void)target.init(firstConfig);
            (void)target.signJwt(payload);
            (void)target.getJwks();
            (void)target.isInitialized();
            readerCalls.fetch_add(1, std::memory_order_relaxed);
        } while (start.load(std::memory_order_acquire));
    });

    barrier.arriveAndWait();
    start.store(true, std::memory_order_release);
    writer.join();
    start.store(false, std::memory_order_release);
    reader.join();

    CHECK(readerCalls.load(std::memory_order_relaxed) > 0);
    CHECK(target.isInitialized());
    CHECK(firstLogger.count.load(std::memory_order_relaxed) > 0);
    CHECK(secondLogger.count.load(std::memory_order_relaxed) > 0);
    const auto jwt = target.signJwt(samplePayload());
    REQUIRE(!jwt.empty());
}