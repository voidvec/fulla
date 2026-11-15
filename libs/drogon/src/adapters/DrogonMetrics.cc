#include <fulla/drogon/adapters/DrogonMetrics.h>

#include "CacheCounter.h"

#include <drogon/drogon.h>
#include <drogon/plugins/PromExporter.h>
#include <drogon/utils/monitoring/Collector.h>
#include <drogon/utils/monitoring/Counter.h>
#include <drogon/utils/monitoring/Gauge.h>

#include <algorithm>
#include <sstream>
#include <vector>

namespace fulla::drogon::adapters
{

namespace
{
namespace dm = ::drogon::monitoring;

// Format the metric name + labels into the same "[METRIC] <name> k=v ..."
// log line the legacy fulla::drogon::observability::Metrics statics
// produced, so log consumers see no change. value is appended as val=
// (matching observeLatency/updateActiveTokens).
std::string formatMetric(
  const std::string &name,
  const fulla::common::ports::MetricLabels &labels,
  double value
)
{
    std::ostringstream oss;
    oss << "[METRIC] " << name;
    for (const auto &kv : labels)
    {
        oss << " " << kv.first << "=" << kv.second;
    }
    oss << " val=" << value;
    return oss.str();
}

// MetricLabels is an unordered_map; collectors require one stable label
// order per metric name, so sort the keys on every call.
std::vector<std::string> labelNames(
  const fulla::common::ports::MetricLabels &labels)
{
    std::vector<std::string> names;
    names.reserve(labels.size());
    for (const auto &kv : labels)
    {
        names.push_back(kv.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> labelValues(
  const fulla::common::ports::MetricLabels &labels,
  const std::vector<std::string> &names)
{
    std::vector<std::string> values;
    values.reserve(names.size());
    for (const auto &n : names)
    {
        values.push_back(labels.at(n));
    }
    return values;
}
}  // namespace

std::shared_ptr<dm::CollectorBase> DrogonMetrics::resolveCollector(
  const std::string &name,
  const std::function<std::shared_ptr<dm::CollectorBase>()> &create)
{
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = collectors_.find(name);
    if (it != collectors_.end())
    {
        return it->second;
    }
    // The templated getPlugin<T>() asserts before startup and caches a
    // static raw pointer; skip the lookup until the framework is running.
    if (!::drogon::app().isRunning())
    {
        return nullptr;
    }
    auto plugin = ::drogon::app().getPlugin<::drogon::plugin::PromExporter>();
    if (!plugin)
    {
        return nullptr;
    }
    std::shared_ptr<dm::CollectorBase> collector;
    try
    {
        collector = plugin->getCollector(name);
    }
    catch (...)
    {
        collector = create();
        try
        {
            plugin->registerCollector(collector);
        }
        catch (...)
        {
            // Another DrogonMetrics instance registered the same name first.
            collector = plugin->getCollector(name);
        }
    }
    collectors_.emplace(name, collector);
    return collector;
}

void DrogonMetrics::incrementCounter(
  const std::string &name,
  const fulla::common::ports::MetricLabels &labels,
  double value
)
{
    // Export is best-effort: a collector/label mismatch must never break
    // the request path; the [METRIC] log line below remains the fallback.
    if (name == "fulla_cache_total")
    {
        // The bounded cache family goes through the whitelist helper ONLY:
        // the generic path would double-count it and admit unbounded label
        // values. The templated getPlugin<T>() asserts before startup and
        // caches a static raw pointer, so use the running check and the
        // non-template lookup for this family.
        try
        {
            auto &app = ::drogon::app();
            if (app.isRunning())
            {
                auto *exporter = dynamic_cast<::drogon::plugin::PromExporter *>(
                  app.getPlugin("drogon::plugin::PromExporter")
                );
                detail::incrementCacheCounter(exporter, name, labels, value);
            }
        }
        catch (...)
        {
        }
    }
    else
    {
        try
        {
            auto names = labelNames(labels);
            auto collector = resolveCollector(name, [&]() {
                return std::make_shared<dm::Collector<dm::Counter>>(
                  name, name, names);
            });
            if (auto typed =
                  std::dynamic_pointer_cast<dm::Collector<dm::Counter>>(collector))
            {
                typed->metric(labelValues(labels, names))->increment(value);
            }
        }
        catch (...)
        {
        }
    }
    try
    {
        LOG_INFO << formatMetric(name, labels, value);
    }
    catch (...)
    {
    }
}

void DrogonMetrics::setGauge(
  const std::string &name,
  const fulla::common::ports::MetricLabels &labels,
  double value
)
{
    try
    {
        auto names = labelNames(labels);
        auto collector = resolveCollector(name, [&]() {
            return std::make_shared<dm::Collector<dm::Gauge>>(name, name, names);
        });
        if (auto typed =
              std::dynamic_pointer_cast<dm::Collector<dm::Gauge>>(collector))
        {
            typed->metric(labelValues(labels, names))->set(value);
        }
    }
    catch (...)
    {
    }
    try
    {
        LOG_INFO << formatMetric(name, labels, value);
    }
    catch (...)
    {
    }
}

void DrogonMetrics::observeHistogram(
  const std::string &name,
  const fulla::common::ports::MetricLabels &labels,
  double value
)
{
    // Log-only: see the header note on Histogram bucket/loop requirements.
    try
    {
        LOG_INFO << formatMetric(name, labels, value);
    }
    catch (...)
    {
    }
}

}  // namespace fulla::drogon::adapters