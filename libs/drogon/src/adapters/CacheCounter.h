#pragma once

#include <fulla/common/ports/IMetrics.h>
#include <drogon/plugins/PromExporter.h>
#include <drogon/utils/monitoring/Collector.h>
#include <drogon/utils/monitoring/Counter.h>
#include <drogon/utils/monitoring/Gauge.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fulla::drogon::adapters::detail
{

// The bounded cache family is registered as a Counter by configuration and
// updated only through incrementCacheCounter(). The IMetrics Gauge API must
// neither register it nor mutate an existing collector, so a misconfigured or
// missing collector cannot turn the counter family into a gauge family.
inline constexpr char kCacheCounterName[] = "fulla_cache_total";

inline bool isReservedCacheFamily(std::string_view name) noexcept
{
    return name == kCacheCounterName;
}

// MetricLabels is an unordered_map; collectors require one stable label
// order per metric name, so sort the keys on every call.
inline std::vector<std::string> labelNames(const ::fulla::common::ports::MetricLabels &labels)
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

inline std::vector<std::string> labelValues(
  const ::fulla::common::ports::MetricLabels &labels,
  const std::vector<std::string> &names
)
{
    std::vector<std::string> values;
    values.reserve(names.size());
    for (const auto &n : names)
    {
        values.push_back(labels.at(n));
    }
    return values;
}

// Only the bounded cache family is exported. Other IMetrics calls retain
// their existing logging behavior; arbitrary labels must not create series.
inline bool incrementCacheCounter(
  ::drogon::plugin::PromExporter *exporter,
  const std::string &name,
  const ::fulla::common::ports::MetricLabels &labels,
  double value
) noexcept
{
    try
    {
        if (
          !exporter || !isReservedCacheFamily(name) || labels.size() != 2 ||
          !std::isfinite(value) || value < 0
        )
            return false;
        const auto repo = labels.find("repo");
        const auto outcome = labels.find("outcome");
        if (repo == labels.end() || outcome == labels.end())
            return false;
        if (repo->second != "token" && repo->second != "client" && repo->second != "owner_status")
            return false;
        if (outcome->second != "hit" && outcome->second != "miss" && outcome->second != "error")
            return false;
        auto collector = exporter->getCollector<::drogon::monitoring::Counter>(name);
        if (!collector || collector->labelsNames() != std::vector<std::string>{"repo", "outcome"})
            return false;
        collector->metric({repo->second, outcome->second})->increment(value);
        return true;
    }
    catch (...)
    {
        // Missing/misconfigured exporters must never fail an OAuth operation.
        return false;
    }
}

// Applies one IMetrics gauge sample. The reserved cache family is rejected
// before any lookup, so a missing collector is never registered and an
// existing collector of any type is never mutated.
inline bool setGaugeSample(
  ::drogon::plugin::PromExporter *exporter,
  const std::string &name,
  const ::fulla::common::ports::MetricLabels &labels,
  double value
) noexcept
{
    try
    {
        if (!exporter || isReservedCacheFamily(name))
            return false;
        auto names = labelNames(labels);
        auto collector = [&]() -> std::shared_ptr<::drogon::monitoring::CollectorBase> {
            try
            {
                return exporter->getCollector(name);
            }
            catch (...)
            {
                auto created =
                  std::make_shared<::drogon::monitoring::Collector<::drogon::monitoring::Gauge>>(
                    name, name, names
                  );
                try
                {
                    exporter->registerCollector(created);
                    return created;
                }
                catch (...)
                {
                    // Another adapter registered the same name first.
                    return exporter->getCollector(name);
                }
            }
        }();
        if (
          auto typed =
            std::dynamic_pointer_cast<::drogon::monitoring::Collector<::drogon::monitoring::Gauge>>(
              collector
            )
        )
        {
            typed->metric(labelValues(labels, names))->set(value);
            return true;
        }
        return false;
    }
    catch (...)
    {
        return false;
    }
}

}  // namespace fulla::drogon::adapters::detail
