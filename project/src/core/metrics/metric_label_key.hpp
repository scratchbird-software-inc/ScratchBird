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

namespace detail {
enum class LabelIssue { none, empty_key, duplicate, unknown, invalid, missing };
struct LabelValidation { LabelIssue issue = LabelIssue::none; std::string_view key; };
inline bool LabelTextPresent(const MetricLabelValue& value) {
  const auto* text = std::get_if<std::string>(&value);
  return text && !text->empty();
}
inline bool LabelTextPresent(const std::variant<std::string_view, MetricUuid>& value) {
  const auto* text = std::get_if<std::string_view>(&value);
  return text && !text->empty();
}
template<class Definition, class Labels>
LabelValidation ValidateLabels(const Definition& descriptor, const Labels& labels) {
  for (auto at = labels.begin(); at != labels.end(); ++at) {
    const auto& value = *at;
    if (value.key.empty())
      return {LabelIssue::empty_key, {}};
    for (auto previous = labels.begin(); previous != at; ++previous)
      if ((*previous).key == value.key)
        return {LabelIssue::duplicate, value.key};
    const auto schema = std::find_if(descriptor.labels.begin(), descriptor.labels.end(),
        [&](const auto& label) { return label.key == value.key; });
    if (schema == descriptor.labels.end())
      return {LabelIssue::unknown, value.key};
    const auto& definition = *schema;
    if (MetricOwnerLabel(value.key) && definition.value_type != MetricLabelType::system_uuid)
      return {LabelIssue::invalid, value.key};
    bool valid = false;
    if (definition.value_type == MetricLabelType::text) {
      valid = LabelTextPresent(value.value);
    } else if (definition.value_type == MetricLabelType::system_uuid) {
      const auto* uuid = std::get_if<MetricUuid>(&value.value);
      valid = uuid && MetricSystemUuidValid(*uuid);
    } else if (definition.value_type == MetricLabelType::uuid_value) {
      // A UUID-valued data label is not a system identity. Older supported
      // UUID versions remain legal data and are never rewritten to v7.
      const auto* uuid = std::get_if<MetricUuid>(&value.value);
      valid = uuid && (uuid->bytes[8] & 0xc0) == 0x80 &&
              (uuid->bytes[6] >> 4) >= 1 && (uuid->bytes[6] >> 4) <= 7;
    }
    if (!valid)
      return {LabelIssue::invalid, value.key};
  }
  for (const auto& schema : descriptor.labels)
    if (schema.required && std::none_of(labels.begin(), labels.end(),
        [&](const auto& label) { return label.key == schema.key; }))
      return {LabelIssue::missing, schema.key};
  return {};
}
}  // namespace detail
inline MetricValidationResult ValidateMetricLabelSet(
    const MetricDescriptorDefinition& descriptor, const MetricLabelSet& labels) {
  const auto result = detail::ValidateLabels(descriptor, labels);
  using I = detail::LabelIssue;
  if (result.issue == I::none) return {true, {}, {}};
  if (result.issue == I::empty_key) return {false, "SB-METRICS-LABEL-INVALID", descriptor.family};
  const char* code = result.issue == I::unknown ? "SB-METRICS-LABEL-UNKNOWN" :
      result.issue == I::missing ? "SB-METRICS-LABEL-REQUIRED-MISSING" : "SB-METRICS-LABEL-INVALID";
  return {false, code, descriptor.family + (result.issue == I::duplicate ? ":duplicate:" : ":") + std::string(result.key)};
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
