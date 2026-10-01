// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_concurrency_definitions.hpp"
#include "metric_value_update.hpp"
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace m = scratchbird::core::metrics;
void Check(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}
int main() {
  try {
    m::MetricRegistry registry;
    const std::array<m::u64, 4> bounds{0, 1, 9007199254740993ULL,
                                     std::numeric_limits<m::u64>::max()};
    auto result = m::ConcurrencyWaitMetricDefinitions(bounds);
    Check(result.validation.ok && result.definitions.size() == 4, "complete wait definition set");
    const std::array<const char*, 4> suffixes{
        "latch_waits", "latch_timeouts", "fail_safe_releases", "latch_wait_duration_us"};
    for (std::size_t i = 0; i < result.definitions.size(); ++i) {
      const auto& definition = result.definitions[i];
      Check(definition.family == std::string("sb_mga_concurrency_") + suffixes[i], "exact family");
      Check(definition.namespace_path == "sys.metrics.mga.concurrency" && !definition.cluster_only,
            "local scope");
      Check(definition.aliases == std::vector<std::string>{
          std::string("sys.metrics.mga.concurrency.") + suffixes[i]}, "normative metric alias");
      Check(definition.producer_owner == "mga_concurrency" &&
            definition.security_family == "OBS_METRICS_READ_FAMILY" &&
            definition.visibility == m::MetricVisibilityScope::family, "protected owning producer");
      Check(definition.value_type == m::MetricScalarType::uint64 &&
            m::ValidateStoredMetricValueDescriptor(definition), "exact unsigned descriptor");
      Check(definition.type == (i == 3 ? m::MetricType::histogram : m::MetricType::counter) &&
            definition.unit == (i == 3 ? m::MetricUnit::microseconds : m::MetricUnit::events), "type and unit");
      const std::array<const char*, 3> labels{"primitive_class", "requested_mode", "owner_scope"};
      Check(definition.labels.size() == labels.size(), "bounded label schema");
      for (std::size_t j = 0; j < labels.size(); ++j)
        Check(definition.labels[j].key == labels[j] && definition.labels[j].required &&
              definition.labels[j].value_type == m::MetricLabelType::text, "required typed labels");
      m::MetricDescriptor unbound;
      static_cast<m::MetricDescriptorDefinition&>(unbound) = definition;
      Check(!registry.RegisterDescriptor(unbound).ok, "template cannot activate itself");
      Check(!m::StageMetricValueUpdate(definition, {}, nullptr, m::u64{1}).ok(),
            "missing required labels refused");
      const m::MetricLabelSet values{{"primitive_class", "condition_wait"},
          {"requested_mode", "none"}, {"owner_scope", "database"}};
      const auto staged = m::StageMetricValueUpdate(definition, values, nullptr, m::u64{9007199254740993ULL});
      Check(staged.ok() && std::get<m::u64>(staged.value->value) == 9007199254740993ULL,
            "no binary64 rounding in staging");
    }
    const auto& histogram = result.definitions.back();
    Check(histogram.histogram_cumulative && histogram.histogram_buckets.size() == bounds.size(),
          "caller profile preserved");
    for (std::size_t i = 0; i < bounds.size(); ++i)
      Check(std::get<m::u64>(histogram.histogram_buckets[i]) == bounds[i], "exact histogram bound");
    for (const auto& bad : {std::vector<m::u64>{}, std::vector<m::u64>{1, 1},
                            std::vector<m::u64>{2, 1}}) {
      const auto rejected = m::ConcurrencyWaitMetricDefinitions(bad);
      Check(!rejected.validation.ok && rejected.validation.diagnostic_code == "METRIC.VALUE_INVALID" &&
            rejected.definitions.empty(), "invalid profile has no partial definitions");
    }
    result.definitions.front().aliases.clear();
    Check(m::ConcurrencyWaitMetricDefinitions(bounds).definitions.front().aliases.size() == 1,
          "no shared mutable templates");
    Check(registry.Descriptors().empty() && registry.SnapshotCurrent().empty(),
          "definition enumeration confers no runtime bindings or samples");
    std::cout << "concurrency_metric_definitions=passed activation_claimed=false\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
