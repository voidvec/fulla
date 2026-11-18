#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <drogon/HttpClient.h>
#include <drogon/plugins/PromExporter.h>
#include <drogon/utils/monitoring/Collector.h>
#include <fulla/drogon/adapters/DrogonMetrics.h>

#include <chrono>
#include <stdexcept>
#include <string>

// Regression for the S07 capacity harness cache-proof gate: the driver
// scrapes fulla_cache_total{repo="token",outcome=...} from /metrics to prove
// the introspection cache path ran. The IMetrics adapter used to be
// log-only, so the counters documented in
// docs/domains/token-refresh-and-revocation.md never reached PromExporter
// and the gate could never pass (row 013-introspect_reuse reported
// cache_path_not_proven despite 100% flow success).

DROGON_TEST(Integration_P2_Plugin_DrogonMetrics_PromExporterExport)
{
    auto plugin = ::drogon::app().getPlugin<::drogon::plugin::PromExporter>();
    REQUIRE(plugin != nullptr);

    const auto suffix = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string counterName = "fulla_test_cache_total_" + suffix;
    const std::string gaugeName = "fulla_test_active_" + suffix;

    fulla::drogon::adapters::DrogonMetrics metrics;
    metrics.incrementCounter(
      counterName, {{"repo", "token"}, {"outcome", "hit"}});
    metrics.incrementCounter(
      counterName, {{"repo", "token"}, {"outcome", "hit"}});
    metrics.incrementCounter(
      counterName, {{"repo", "token"}, {"outcome", "miss"}});

    // A second adapter instance must reuse the registered collector rather
    // than trip PromExporter's duplicate-registration guard.
    fulla::drogon::adapters::DrogonMetrics second;
    second.incrementCounter(
      counterName, {{"repo", "token"}, {"outcome", "hit"}});

    std::shared_ptr<::drogon::monitoring::CollectorBase> collector;
    try
    {
        collector = plugin->getCollector(counterName);
    }
    catch (const std::exception &e)
    {
        FAIL(std::string("counter collector not exported: ") + e.what());
        return;
    }
    REQUIRE(collector != nullptr);
    double hit = -1;
    double miss = -1;
    for (const auto &group : collector->collect())
    {
        std::string outcome;
        for (const auto &kv : group.metric->labels())
        {
            if (kv.first == "outcome")
                outcome = kv.second;
        }
        double sum = 0;
        for (const auto &sample : group.samples)
            sum += sample.value;
        if (outcome == "hit")
            hit = sum;
        if (outcome == "miss")
            miss = sum;
    }
    CHECK(hit == 3.0);
    CHECK(miss == 1.0);

    metrics.setGauge(gaugeName, {{"kind", "active"}}, 42.5);
    metrics.setGauge(gaugeName, {{"kind", "active"}}, 7.0);
    std::shared_ptr<::drogon::monitoring::CollectorBase> gaugeCollector;
    try
    {
        gaugeCollector = plugin->getCollector(gaugeName);
    }
    catch (const std::exception &e)
    {
        FAIL(std::string("gauge collector not exported: ") + e.what());
        return;
    }
    REQUIRE(gaugeCollector != nullptr);
    double gauge = -1;
    for (const auto &group : gaugeCollector->collect())
    {
        for (const auto &sample : group.samples)
            gauge = sample.value;
    }
    CHECK(gauge == 7.0);

    // End-to-end: the Prometheus exposition the S07 driver scrapes must
    // carry the counter with the labels the gate filters on.
    auto client = ::drogon::HttpClient::newHttpClient(
      "http://127.0.0.1:5555", ::drogon::app().getLoop());
    auto req = ::drogon::HttpRequest::newHttpRequest();
    req->setPath("/metrics");
    auto [result, resp] = client->sendRequest(req, 10);
    REQUIRE(result == ::drogon::ReqResult::Ok);
    REQUIRE(resp != nullptr);
    REQUIRE(resp->statusCode() == ::drogon::k200OK);
    const std::string body(resp->body());
    CHECK(body.find(counterName + "{") != std::string::npos);
    CHECK(body.find("repo=\"token\"") != std::string::npos);
    CHECK(body.find("outcome=\"hit\"") != std::string::npos);
}