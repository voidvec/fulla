#pragma once

// M8 Task 40 (fulla-sdk-refactor, design.md §5.2 decision b):
// Adapter-side default implementation of fulla::common::ports::IMetrics,
// the Domain-facing metrics port. Mirrors DrogonAuditSink's shape/rationale:
// Drogon-layer controllers (and any future Domain service) emit metrics through
// the IMetrics port (getMetrics()) instead of calling the fulla::drogon::
// observability::Metrics statics directly, keeping the call sites decoupled
// from the concrete log-based emitter.
//
// The port is generic (incrementCounter(name, labels, value)); this adapter
// exports counters and gauges through drogon::plugin::PromExporter (the
// production backend named in the IMetrics port contract) AND retains the
// "[METRIC] <name> key=value ..." log line the legacy Metrics::* statics
// produced, so log-scraping consumers are unchanged. When no PromExporter
// plugin is configured the adapter degrades to log-only.
// (The legacy Metrics class is retained for now as the OperationTimer RAII
// helper's backing; this adapter is the new primary emission path.)
//
// observeHistogram stays log-only: drogon's monitoring Histogram demands
// explicit bucket boundaries and spins an event-loop thread per metric,
// and no production call site observes histograms through this port yet.

#include <fulla/common/ports/IMetrics.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace drogon::monitoring
{
class CollectorBase;
}

namespace fulla::drogon::adapters
{

/**
 * @brief Exports IMetrics counters/gauges via PromExporter and forwards
 * every call to Drogon's LOG_ emitter as a "[METRIC] <name> k=v ..." line.
 * One instance may be shared across threads; the collector cache is
 * mutex-guarded and the underlying collectors are thread-safe.
 */
class DrogonMetrics : public fulla::common::ports::IMetrics
{
  public:
    void incrementCounter(
      const std::string &name,
      const fulla::common::ports::MetricLabels &labels,
      double value = 1.0
    ) override;

    void setGauge(
      const std::string &name,
      const fulla::common::ports::MetricLabels &labels,
      double value
    ) override;

    void observeHistogram(
      const std::string &name,
      const fulla::common::ports::MetricLabels &labels,
      double value
    ) override;

  private:
    std::shared_ptr<::drogon::monitoring::CollectorBase> resolveCollector(
      const std::string &name,
      const std::function<
        std::shared_ptr<::drogon::monitoring::CollectorBase>()> &create);

    std::mutex mutex_;
    std::unordered_map<
      std::string,
      std::shared_ptr<::drogon::monitoring::CollectorBase>>
      collectors_;
};

}  // namespace fulla::drogon::adapters
