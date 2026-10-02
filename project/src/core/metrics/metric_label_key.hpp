// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "metric_registry.hpp"
#include <algorithm>
#include <string_view>

namespace scratchbird::core::metrics {

inline bool MetricSystemUuidValid(const MetricUuid& value) noexcept {
  return !value.is_nil() && (value.bytes[6] & 0xf0) == 0x70 &&
         (value.bytes[8] & 0xc0) == 0x80;
}

inline bool MetricOwnerLabel(std::string_view key) noexcept {
  return key == "database_uuid" || key == "node_uuid" || key == "cluster_uuid";
}

// Reserved ownership labels repeat native scope. Other labels may describe
// related objects or user UUID data and are not silently reinterpreted here.
inline bool MetricOwnerLabelMatchesScope(std::string_view key,
    const MetricUuid* identity, const MetricUuid& database_uuid,
    const MetricUuid& node_uuid, const MetricUuid& cluster_uuid) noexcept {
  const MetricUuid* expected = nullptr;
  if (key == "database_uuid") expected = &database_uuid;
  else if (key == "node_uuid") expected = &node_uuid;
  else if (key == "cluster_uuid") expected = &cluster_uuid;
  if (!expected) return true;
  return identity && MetricSystemUuidValid(*identity) && *identity == *expected;
}

inline bool MetricOwnerLabelMatchesScope(std::string_view key,
    const MetricLabelValue& value, const MetricUuid& database_uuid,
    const MetricUuid& node_uuid, const MetricUuid& cluster_uuid) noexcept {
  return MetricOwnerLabelMatchesScope(key, std::get_if<MetricUuid>(&value),
      database_uuid, node_uuid, cluster_uuid);
}

inline bool MetricOwnerLabelsMatchScope(const MetricLabelSet& labels,
    const MetricUuid& database_uuid, const MetricUuid& node_uuid,
    const MetricUuid& cluster_uuid) noexcept {
  return std::all_of(labels.begin(), labels.end(), [&](const auto& label) {
    return MetricOwnerLabelMatchesScope(label.key, label.value,
                                       database_uuid, node_uuid, cluster_uuid);
  });
}

// Validation does not mutate any series and does not format or parse UUIDs.
template<class Definition>
inline bool MetricNamespaceMatchesScope(const Definition& definition) noexcept {
  const std::string_view root = definition.cluster_only ? "cluster.sys.metrics." : "sys.metrics.";
  const auto& path = definition.namespace_path;
  return path.size() > root.size() && path.size() <= 4096 &&
      path.starts_with(root) && path.find('\0') == path.npos;
}

template<class Definition>
inline bool MetricDescriptorReferencesValid(const Definition& d,
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
    if (MetricOwnerLabel(value.key) && schema->value_type != MetricLabelType::system_uuid)
      return {false, "SB-METRICS-LABEL-INVALID", descriptor.family + ":" + value.key};
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
