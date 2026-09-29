// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_registry.hpp"

namespace scratchbird::core::metrics {
// Definition data for catalog creation, never a lazy runtime registration.
// The node owner must bind native descriptor/schema/policy/series generations
// before a producer can publish any of these observations.
std::vector<MetricDescriptorDefinition> PageCacheContextMetricDefinitions();
}  // namespace scratchbird::core::metrics
