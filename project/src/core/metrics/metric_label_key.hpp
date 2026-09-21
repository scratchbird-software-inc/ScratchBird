// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "metric_registry.hpp"
#include <algorithm>

namespace scratchbird::core::metrics {

inline bool MetricSystemUuidValid(const MetricUuid& value) noexcept {
  return !value.is_nil() && (value.bytes[6] & 0xf0) == 0x70 &&
         (value.bytes[8] & 0xc0) == 0x80;
}

// Validation does not mutate any series and does not format or parse UUIDs.
inline bool MetricDescriptorReferencesValid(const MetricDescriptorDefinition& d,
                                            const MetricDescriptorBinding& b) noexcept {
  if (!MetricSystemUuidValid(b.metric_uuid) || !b.descriptor_generation ||
      !MetricSystemUuidValid(b.retention_policy_uuid) || !b.retention_policy_generation ||
      !MetricSystemUuidValid(b.visibility_policy_uuid) || !b.visibility_policy_generation) return false;
  if (!b.label_schema_generation) {
    if (!b.label_schema_uuid.is_nil() || !d.labels.empty()) return false;
  } else if (!MetricSystemUuidValid(b.label_schema_uuid)) return false;
  if (d.type == MetricType::rate)
    return MetricSystemUuidValid(b.rate_source_counter_uuid) && b.rate_source_counter_generation && d.rate_window_nanoseconds;
  return b.rate_source_counter_uuid.is_nil() && !b.rate_source_counter_generation && !d.rate_window_nanoseconds;
}

inline MetricValidationResult ValidateMetricLabelSet(
    const MetricDescriptorDefinition& descriptor, const MetricLabelSet& labels) {
  for (std::size_t i = 0; i < labels.size(); ++i) {
    const auto& value = labels[i];
    if (value.key.empty())
      return {false, "SB-METRICS-LABEL-INVALID", descriptor.family};
    for (std::size_t n = 0; n < i; ++n)
      if (labels[n].key == value.key)
        return {false, "SB-METRICS-LABEL-INVALID", descriptor.family + ":duplicate:" + value.key};
    const auto schema = std::find_if(descriptor.labels.begin(), descriptor.labels.end(),
        [&](const auto& label) { return label.key == value.key; });
    if (schema == descriptor.labels.end())
      return {false, "SB-METRICS-LABEL-UNKNOWN", descriptor.family + ":" + value.key};
    bool valid = false;
    if (schema->value_type == MetricLabelType::text) {
      const auto* text = std::get_if<std::string>(&value.value);
      valid = text && !text->empty();
    } else if (schema->value_type == MetricLabelType::system_uuid) {
      const auto* uuid = std::get_if<MetricUuid>(&value.value);
      valid = uuid && MetricSystemUuidValid(*uuid);
    } else if (schema->value_type == MetricLabelType::uuid_value) {
      // A UUID-valued data label is not a system identity. Older supported
      // UUID versions remain legal data and are never rewritten to v7.
      const auto* uuid = std::get_if<MetricUuid>(&value.value);
      valid = uuid && (uuid->bytes[8] & 0xc0) == 0x80 &&
              (uuid->bytes[6] >> 4) >= 1 && (uuid->bytes[6] >> 4) <= 7;
    }
    if (!valid)
      return {false, "SB-METRICS-LABEL-INVALID", descriptor.family + ":" + value.key};
  }
  for (const auto& schema : descriptor.labels)
    if (schema.required && std::none_of(labels.begin(), labels.end(),
        [&](const auto& label) { return label.key == schema.key; }))
      return {false, "SB-METRICS-LABEL-REQUIRED-MISSING", descriptor.family + ":" + schema.key};
  return {true, {}, {}};
}

inline MetricSeriesKey MakeMetricSeriesKey(const std::string& family,
                                          const MetricLabelSet& labels) {
  MetricSeriesKey key;
  key.first = family;
  key.second.reserve(labels.size());
  for (const auto& label : labels) key.second.emplace_back(label.key, label.value);
  std::sort(key.second.begin(), key.second.end());
  return key;
}

}  // namespace scratchbird::core::metrics
