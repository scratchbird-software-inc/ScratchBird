// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_registry.hpp"
#include <span>

namespace scratchbird::core::metrics {
struct ConcurrencyMetricDefinitionsResult {
  MetricValidationResult validation;
  std::vector<MetricDescriptorDefinition> definitions;
};
// Definition templates only, not catalog bindings or producer activation.
// The owning profile supplies finite, strictly increasing microsecond bounds;
// the metric service adds its terminal unbounded histogram bucket.
// No identities, readiness, generations or default timing policy are invented.
ConcurrencyMetricDefinitionsResult ConcurrencyWaitMetricDefinitions(
    std::span<const u64> duration_bounds_us);
}  // namespace scratchbird::core::metrics
