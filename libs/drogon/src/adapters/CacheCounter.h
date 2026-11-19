#pragma once

#include <fulla/common/ports/IMetrics.h>
#include <drogon/plugins/PromExporter.h>
#include <drogon/utils/monitoring/Counter.h>

#include <cmath>
#include <string>
#include <vector>

namespace fulla::drogon::adapters::detail
{

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
          !exporter || name != "fulla_cache_total" || labels.size() != 2 || !std::isfinite(value) ||
          value < 0
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

}  // namespace fulla::drogon::adapters::detail
