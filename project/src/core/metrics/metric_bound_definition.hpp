// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "metric_registry.hpp"
#include <algorithm>

namespace scratchbird::core::metrics {
// Verify the emitting component's contract against an immutable, previously
// admitted registry row. Definitions do not create UUIDs, activate families or
// admit series. Help text and aliases are annotations, not emission authority.
inline MetricValidationResult ValidateBoundMetricDefinition(
    const MetricRegistry& registry, const MetricDescriptorDefinition& expected) {
  const auto* actual = registry.FindDescriptor(expected.family);
  if (!actual) return MetricError("METRIC.VALUE_INVALID", "metric descriptor not bound: " + expected.family);
  if (actual->type != expected.type || actual->unit != expected.unit ||
      actual->namespace_path != expected.namespace_path || actual->producer_owner != expected.producer_owner ||
      actual->security_family != expected.security_family || actual->visibility != expected.visibility ||
      actual->cluster_only != expected.cluster_only || actual->value_type != expected.value_type ||
      actual->histogram_buckets != expected.histogram_buckets || actual->histogram_cumulative != expected.histogram_cumulative ||
      actual->min_value != expected.min_value || actual->max_value != expected.max_value ||
      actual->enum_values != expected.enum_values || actual->rate_window_nanoseconds != expected.rate_window_nanoseconds ||
      actual->readiness != MetricReadiness::implemented ||
      !std::equal(actual->labels.begin(), actual->labels.end(), expected.labels.begin(), expected.labels.end(),
          [](const auto& a, const auto& b) { return a.key == b.key && a.required == b.required &&
              a.sensitive == b.sensitive && a.value_type == b.value_type; }))
    return MetricError("METRIC.VALUE_INVALID", "metric descriptor differs: " + expected.family);
  // Registration validated this retained immutable row. Avoid rebuilding its
  // label-key set and revalidating native identity on every observation.
  return MetricOk();
}
}  // namespace scratchbird::core::metrics
