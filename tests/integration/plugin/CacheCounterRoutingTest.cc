#include <drogon/drogon_test.h>

#include <drogon/drogon.h>
#include <drogon/plugins/PromExporter.h>
#include <drogon/utils/monitoring/Counter.h>
#include <drogon/utils/monitoring/Gauge.h>
#include <fulla/drogon/adapters/DrogonMetrics.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Runs against the configured server: the official configuration registers
// fulla_cache_total as a Counter. Missing/misconfigured collector states are
// covered as unit tests in libs/drogon/test/CacheCounterTest.cc, where the
// reserved family can be exercised without a second server configuration.
namespace
{
namespace monitoring = ::drogon::monitoring;
using Exporter = ::drogon::plugin::PromExporter;
using Counter = monitoring::Collector<monitoring::Counter>;
using Labels = ::fulla::common::ports::MetricLabels;
using Snapshot = std::map<std::string, double>;

std::shared_ptr<monitoring::CollectorBase> lookup(Exporter &exporter, const std::string &name)
{
    try
    {
        return exporter.getCollector(name);
    }
    catch (...)
    {
        return nullptr;
    }
}

Snapshot snapshot(const std::shared_ptr<monitoring::CollectorBase> &collector)
{
    Snapshot result;
    if (!collector)
        return result;
    for (const auto &group : collector->collect())
    {
        std::string key;
        for (const auto &label : group.metric->labels())
            key += label.first + '=' + label.second + ';';
        for (const auto &sample : group.samples)
            result[key] += sample.value;
    }
    return result;
}

bool hasSample(
  const std::string &body,
  const std::string &name,
  const std::vector<std::string> &labels,
  double expected
)
{
    std::istringstream lines(body);
    std::string line;
    while (std::getline(lines, line))
    {
        if (line.rfind(name + '{', 0) != 0)
            continue;
        if (!std::all_of(labels.begin(), labels.end(), [&line](const std::string &label) {
                return line.find(label) != std::string::npos;
            }))
            continue;
        const auto close = line.find('}');
        if (close == std::string::npos)
            return false;
        std::istringstream value(line.substr(close + 1));
        double actual = 0;
        return (value >> actual) && actual >= expected;
    }
    return false;
}
}  // namespace

DROGON_TEST(Integration_P2_Plugin_DrogonMetrics_BoundedCacheRouting)
{
    try
    {
        auto *exporter =
          dynamic_cast<Exporter *>(::drogon::app().getPlugin("drogon::plugin::PromExporter"));
        REQUIRE(exporter != nullptr);
        const auto configured = lookup(*exporter, "fulla_cache_total");
        const auto counter = std::dynamic_pointer_cast<Counter>(configured);
        REQUIRE(counter != nullptr);
        REQUIRE((counter->labelsNames() == std::vector<std::string>{"repo", "outcome"}));

        ::fulla::drogon::adapters::DrogonMetrics metrics;
        const auto original = snapshot(configured);
        for (const Labels &labels : std::vector<Labels>{
               {},
               {{"repo", "token"}},
               {{"repo", "private-user"}, {"outcome", "hit"}},
               {{"repo", "token"}, {"outcome", "private-token"}},
               {{"repo", "token"}, {"outcome", "hit"}, {"client_id", "private-client"}}
             })
            metrics.incrementCounter("fulla_cache_total", labels);
        const Labels valid{{"repo", "owner_status"}, {"outcome", "error"}};
        for (double value :
             {-1.0,
              std::numeric_limits<double>::infinity(),
              std::numeric_limits<double>::quiet_NaN()})
            metrics.incrementCounter("fulla_cache_total", valid, value);
        // The Gauge API must not register or mutate the reserved family.
        metrics.setGauge("fulla_cache_total", {{"arbitrary", "must-not-create"}}, 7);
        metrics.setGauge("fulla_cache_total", valid, 8);
        REQUIRE(lookup(*exporter, "fulla_cache_total") == configured);
        REQUIRE((snapshot(configured) == original));

        const double expectedCache =
          counter->metric({"owner_status", "error"})->collect().at(0).value + 1;
        metrics.incrementCounter("fulla_cache_total", valid);
        REQUIRE(lookup(*exporter, "fulla_cache_total") == configured);
        CHECK(counter->metric({"owner_status", "error"})->collect().at(0).value == expectedCache);
        CHECK(counter->collect().size() <= 9);
        for (const auto &group : counter->collect())
        {
            const auto &labels = group.metric->labels();
            REQUIRE(labels.size() == 2);
            CHECK(labels[0].first == "repo");
            CHECK(labels[1].first == "outcome");
            CHECK(
              (labels[0].second == "token" || labels[0].second == "client" ||
               labels[0].second == "owner_status")
            );
            CHECK((
              labels[1].second == "hit" || labels[1].second == "miss" || labels[1].second == "error"
            ));
        }

        // Preserve the generic export behavior merged by #343.
        const auto suffix =
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto peerCounter = "fulla_test_generic_peer_total_" + suffix;
        const auto peerGauge = "fulla_test_generic_peer_gauge_" + suffix;
        metrics.incrementCounter(peerCounter, {{"kind", "peer"}}, 2);
        ::fulla::drogon::adapters::DrogonMetrics second;
        second.incrementCounter(peerCounter, {{"kind", "peer"}}, 3);
        metrics.setGauge(peerGauge, {{"kind", "peer"}}, 4);
        metrics.setGauge(peerGauge, {{"kind", "peer"}}, 7);
        const auto exportedCounter =
          std::dynamic_pointer_cast<Counter>(lookup(*exporter, peerCounter));
        const auto exportedGauge =
          std::dynamic_pointer_cast<monitoring::Collector<monitoring::Gauge>>(
            lookup(*exporter, peerGauge)
          );
        REQUIRE(exportedCounter != nullptr);
        REQUIRE(exportedGauge != nullptr);
        CHECK(exportedCounter->metric({"peer"})->collect().at(0).value == 5);
        CHECK(exportedGauge->metric({"peer"})->collect().at(0).value == 7);

        // The exporter caches HTTP snapshots for five seconds. Observe reads
        // under one deadline; never alter the interval or replay a metric write.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        auto client =
          ::drogon::HttpClient::newHttpClient("http://127.0.0.1:5555", ::drogon::app().getLoop());
        bool visible = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const double remaining =
              std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0)
                break;
            auto request = ::drogon::HttpRequest::newHttpRequest();
            request->setPath("/metrics");
            const auto response = client->sendRequest(request, (std::min)(remaining, 3.0));
            REQUIRE(response.first == ::drogon::ReqResult::Ok);
            REQUIRE(response.second != nullptr);
            REQUIRE(response.second->getStatusCode() == ::drogon::k200OK);
            const std::string body(response.second->getBody());
            visible = hasSample(body, peerCounter, {"kind=\"peer\""}, 5) &&
                      hasSample(body, peerGauge, {"kind=\"peer\""}, 7) &&
                      hasSample(
                        body,
                        "fulla_cache_total",
                        {"repo=\"owner_status\"", "outcome=\"error\""},
                        expectedCache
                      );
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                visible = false;
                break;
            }
            if (visible)
                break;
            std::this_thread::sleep_until((std::min)(deadline,
                                                     now + std::chrono::milliseconds(50)));
        }
        CHECK(visible);
        std::cout << "[cache-routing] real_collector=true scrape_visible=" << visible << '\n';
    }
    catch (const std::exception &error)
    {
        FAIL(std::string("Cache routing fixture failed: ") + error.what());
    }
}
