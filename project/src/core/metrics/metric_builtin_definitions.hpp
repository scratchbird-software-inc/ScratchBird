// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_registry.hpp"

namespace scratchbird::core::metrics {
// Compiled definitions only: no runtime identities, policy binding, readiness,
// registry mutation or successful observation is implied by enumeration.
std::vector<MetricDescriptorDefinition> BuiltinMetricDescriptorDefinitions();
}  // namespace scratchbird::core::metrics
