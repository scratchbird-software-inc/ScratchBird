// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_registry.hpp"

namespace scratchbird::core::metrics {
enum class MetricValueUpdateError {
  none, invalid_descriptor, invalid_observation, invalid_current, negative_delta,
  overflow, allocation_failure, arithmetic_failure
};
struct MetricValueUpdateResult {
  MetricValueUpdateError error = MetricValueUpdateError::invalid_observation;
  std::optional<MetricValue> value;
  bool ok() const { return error == MetricValueUpdateError::none && value.has_value(); }
};
bool ValidateMetricHistogramDescriptor(const MetricDescriptorDefinition&) noexcept;
bool ValidateMetricValueDescriptor(const MetricDescriptorDefinition&) noexcept;
bool ValidateMetricValueShape(const MetricDescriptorDefinition&, const MetricValue&);
// Stored shape also admits a source-derived numeric rate. It does not establish
// source/window/reset evidence and never authorizes a raw rate producer update.
bool ValidateStoredMetricValueShape(const MetricDescriptorDefinition&, const MetricValue&);
// Stages the actual next current/history value. Does not mutate previous state,
// persist data, authorize a producer/reset or claim a live catalog binding.
MetricValueUpdateResult StageMetricValueUpdate(const MetricDescriptorDefinition&,
    const MetricLabelSet&, const MetricValue* current, const MetricScalar& observation,
    const std::string& state_text = {});
}  // namespace scratchbird::core::metrics
