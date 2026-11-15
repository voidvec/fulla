#include "../src/adapters/CacheCounter.h"

#include <fulla/drogon/adapters/DrogonMetrics.h>
#include <drogon/drogon.h>
#include <drogon/utils/monitoring/Gauge.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace
{
using Exporter = ::drogon::plugin::PromExporter;
using Counter = ::drogon::monitoring::Counter;
using Collector = ::drogon::monitoring::Collector<Counter>;
using Labels = ::fulla::common::ports::MetricLabels;
using ::fulla::drogon::adapters::detail::incrementCacheCounter;

std::shared_ptr<Collector> registerCache(Exporter &exporter)
{
    auto collector = std::make_shared<Collector>(
      "fulla_cache_total", "Cache lookup outcomes", std::vector<std::string>{"repo", "outcome"}
    );
    exporter.registerCollector(collector);
    return collector;
}
}  // namespace

TEST(CacheCounter, ExportsBoundedSeriesAndAccumulatesValues)
{
    Exporter exporter;
    auto collector = registerCache(exporter);
    for (const auto *repo : {"token", "client", "owner_status"})
    {
        for (const auto *outcome : {"hit", "miss", "error"})
        {
            const Labels labels{{"repo", repo}, {"outcome", outcome}};
            ASSERT_TRUE(incrementCacheCounter(&exporter, "fulla_cache_total", labels, 1));
            ASSERT_TRUE(incrementCacheCounter(&exporter, "fulla_cache_total", labels, 2.5));
            ASSERT_TRUE(incrementCacheCounter(&exporter, "fulla_cache_total", labels, 0));
            EXPECT_DOUBLE_EQ(collector->metric({repo, outcome})->collect().at(0).value, 3.5);
        }
    }
    EXPECT_EQ(collector->collect().size(), 9u);
}

TEST(CacheCounter, RejectsUnboundedLabelsAndInvalidValuesWithoutCreatingSeries)
{
    Exporter exporter;
    auto collector = registerCache(exporter);
    const Labels valid{{"repo", "token"}, {"outcome", "hit"}};
    EXPECT_FALSE(incrementCacheCounter(&exporter, "other_counter", valid, 1));
    for (const Labels &labels : std::vector<Labels>{
           {},
           {{"repo", "token"}},
           {{"repo", "user-123"}, {"outcome", "hit"}},
           {{"repo", "token"}, {"outcome", "secret-token"}},
           {{"repo", "token"}, {"outcome", "hit"}, {"client_id", "private-client"}},
           {{"other", "token"}, {"outcome", "hit"}},
         })
        EXPECT_FALSE(incrementCacheCounter(&exporter, "fulla_cache_total", labels, 1));
    for (const double value :
         {-1.0,
          std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::quiet_NaN()})
        EXPECT_FALSE(incrementCacheCounter(&exporter, "fulla_cache_total", valid, value));
    EXPECT_TRUE(collector->collect().empty());
}

TEST(CacheCounter, MissingOrMisconfiguredCollectorIsOptional)
{
    const Labels labels{{"repo", "token"}, {"outcome", "hit"}};
    Exporter missing;
    EXPECT_FALSE(incrementCacheCounter(nullptr, "fulla_cache_total", labels, 1));
    EXPECT_FALSE(incrementCacheCounter(&missing, "fulla_cache_total", labels, 1));
    Exporter wrongType;
    wrongType.registerCollector(
      std::make_shared<::drogon::monitoring::Collector<::drogon::monitoring::Gauge>>(
        "fulla_cache_total", "Wrong type", std::vector<std::string>{"repo", "outcome"}
      )
    );
    EXPECT_FALSE(incrementCacheCounter(&wrongType, "fulla_cache_total", labels, 1));
    Exporter wrongLabels;
    auto reversed = std::make_shared<Collector>(
      "fulla_cache_total", "Wrong schema", std::vector<std::string>{"outcome", "repo"}
    );
    wrongLabels.registerCollector(reversed);
    EXPECT_FALSE(incrementCacheCounter(&wrongLabels, "fulla_cache_total", labels, 1));
    EXPECT_TRUE(reversed->collect().empty());
}

TEST(CacheCounter, ConcurrentIncrementsPreserveExactTotals)
{
    Exporter exporter;
    auto collector = registerCache(exporter);
    constexpr int threads = 8;
    constexpr int perThread = 1000;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int i = 0; i != threads; ++i)
        workers.emplace_back([&]() {
            const Labels labels{{"repo", "token"}, {"outcome", "hit"}};
            for (int n = 0; n != perThread; ++n)
                if (!incrementCacheCounter(&exporter, "fulla_cache_total", labels, 1))
                    ++failures;
        });
    for (auto &worker : workers)
        worker.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(collector->collect().size(), 1u);
    EXPECT_DOUBLE_EQ(
      collector->metric({"token", "hit"})->collect().at(0).value, threads * perThread
    );
}

TEST(CacheCounter, ProductionAdapterCanRunBeforeServerStartup)
{
    ASSERT_FALSE(::drogon::app().isRunning());
    ::fulla::drogon::adapters::DrogonMetrics metrics;
    EXPECT_NO_THROW(
      metrics.incrementCounter("fulla_cache_total", {{"repo", "token"}, {"outcome", "hit"}})
    );
    EXPECT_NO_THROW(metrics.incrementCounter("other_counter", {}));
}

TEST(CacheCounter, OfficialProfilesRegisterTheSameUniqueCollector)
{
    const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    for (const auto *file :
         {"config.json", "config.dev.json", "config.ci.json", "config.prod.json"})
    {
        SCOPED_TRACE(file);
        std::ifstream input(root / "apps/server/config" / file);
        ASSERT_TRUE(input.good());
        Json::CharReaderBuilder reader;
        Json::Value config;
        std::string error;
        ASSERT_TRUE(Json::parseFromStream(reader, input, &config, &error)) << error;
        int matched = 0;
        for (const auto &plugin : config["plugins"])
        {
            if (plugin["name"].asString() != "drogon::plugin::PromExporter")
                continue;
            for (const auto &collector : plugin["config"]["collectors"])
            {
                if (collector["name"].asString() != "fulla_cache_total")
                    continue;
                ++matched;
                EXPECT_EQ(collector["type"].asString(), "counter");
                ASSERT_EQ(collector["labels"].size(), 2u);
                EXPECT_EQ(collector["labels"][0].asString(), "repo");
                EXPECT_EQ(collector["labels"][1].asString(), "outcome");
            }
        }
        EXPECT_EQ(matched, 1);
    }
}
