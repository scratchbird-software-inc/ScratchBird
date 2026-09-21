// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_registry.hpp"
#include <span>

namespace scratchbird::core::metrics {
struct MetricExportContext {
  MetricUuid export_profile_uuid, source_scope_uuid, redaction_policy_uuid;
  u64 schema_version=0, observation_time_utc_ns=0, export_time_utc_ns=0;
  std::string residency_decision;
};
struct MetricExportLabelRule {
  std::string source_key, export_key;
  bool omit=false;
  bool operator==(const MetricExportLabelRule&) const = default;
};
struct MetricExportSample {
  MetricDescriptor descriptor;
  MetricValue value;
  std::string export_name;
  std::vector<MetricExportLabelRule> label_rules;
};
enum class MetricExportError {
  none, invalid_context, invalid_projection, invalid_value, name_collision,
  duplicate_series, size_limit, resource_exhausted, numeric_failure
};
struct MetricExportResult {
  MetricExportError error=MetricExportError::invalid_projection;
  std::string text;
  bool ok() const noexcept {return error==MetricExportError::none&&!text.empty();}
};
inline constexpr std::size_t kMetricExportMaximumBytes=16*1024*1024;
// Pure presentation of the owning adapter's already authorized, committed
// projection. Does not acquire grants, visibility, profile/residency authority,
// read a live registry, record observations, persist data or send a response.
MetricExportResult RenderOpenMetricsProjection(const MetricExportContext&,
    std::span<const MetricExportSample>,std::size_t maximum_bytes=kMetricExportMaximumBytes) noexcept;
} // namespace scratchbird::core::metrics
