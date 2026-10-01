// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_concurrency_definitions.hpp"

namespace scratchbird::core::metrics {
// SEARCH_KEY: SB_MGA_CONCURRENCY_WAIT_METRIC_DEFINITIONS
ConcurrencyMetricDefinitionsResult ConcurrencyWaitMetricDefinitions(
    std::span<const u64> duration_bounds_us) {
  if (duration_bounds_us.empty())
    return {MetricError("METRIC.VALUE_INVALID", "missing concurrency duration profile"), {}};
  for (std::size_t i = 1; i < duration_bounds_us.size(); ++i) {
    if (duration_bounds_us[i] <= duration_bounds_us[i - 1])
      return {MetricError("METRIC.VALUE_INVALID", "unordered concurrency duration profile"), {}};
  }
  struct Row { const char* suffix; const char* help; };
  static constexpr Row rows[] = {
      {"latch_waits", "Registered wait calls; repeated wakes do not add waits."},
      {"latch_timeouts", "Wait calls selecting timed_out; drain deadlines are not wait timeouts."},
      {"fail_safe_releases", "Fail-safe occurrences; not successful release or drain receipts."},
      {"latch_wait_duration_us", "Registered-call duration through terminal selection in microseconds."},
  };
  ConcurrencyMetricDefinitionsResult result{MetricOk(), {}};
  result.definitions.reserve(std::size(rows));
  for (std::size_t i = 0; i < std::size(rows); ++i) {
    MetricDescriptorDefinition definition;
    definition.family = std::string("sb_mga_concurrency_") + rows[i].suffix;
    definition.namespace_path = "sys.metrics.mga.concurrency";
    definition.aliases = {definition.namespace_path + "." + rows[i].suffix};
    definition.help = rows[i].help;
    definition.producer_owner = "mga_concurrency";
    definition.security_family = "OBS_METRICS_READ_FAMILY";
    definition.visibility = MetricVisibilityScope::family;
    definition.value_type = MetricScalarType::uint64;
    definition.type = i == 3 ? MetricType::histogram : MetricType::counter;
    definition.unit = i == 3 ? MetricUnit::microseconds : MetricUnit::events;
    definition.labels = {{"primitive_class", true}, {"requested_mode", true}, {"owner_scope", true}};
    if (i == 3) {
      definition.histogram_buckets.reserve(duration_bounds_us.size());
      for (auto bound : duration_bounds_us) definition.histogram_buckets.emplace_back(bound);
    }
    result.definitions.push_back(std::move(definition));
  }
  return result;
}
}  // namespace scratchbird::core::metrics
