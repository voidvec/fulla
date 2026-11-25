// The legacy case ID is retained for existing test filters. This is now a
// regression for the production init-before-publish/read-only contract.
// Concurrent first initialization and reads are not a supported operation.

#include <drogon/drogon_test.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/oauth2/jwk/JwkManager.h>
#include <json/json.h>

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
                                      expectedJwks, expectedJwt, index]() {
                    auto &result = (*results)[static_cast<size_t>(index)];
                    try
                    {
                        if (release.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
                        {
                            result.ok = false;
                            return;
                        }
                        for (int read = 0; read < kReadsPerThread; ++read)
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
