// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_history.hpp"
#include "metric_label_key.hpp"
#include "metric_value_update.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <utility>

namespace scratchbird::core::metrics {
namespace {
using E = MetricHistoryRecordError;
bool BindingValid(const MetricDescriptor& d, const MetricHistoryBinding& b) {
  if (!MetricDescriptorReferencesValid(d, d) ||
      static_cast<const MetricDescriptorBinding&>(d) != static_cast<const MetricDescriptorBinding&>(b)) return false;
  if (!MetricSystemUuidValid(b.metric_uuid) || !b.descriptor_generation ||
      !MetricSystemUuidValid(b.retention_policy_uuid) || !b.retention_policy_generation ||
      !MetricSystemUuidValid(b.visibility_policy_uuid) || !b.visibility_policy_generation ||
      !MetricSystemUuidValid(b.database_uuid) || !MetricSystemUuidValid(b.node_uuid)) return false;
  if (d.labels.empty()) {
    if (b.label_schema_generation == 0) {
      if (!b.label_schema_uuid.is_nil()) return false;
    } else if (!MetricSystemUuidValid(b.label_schema_uuid)) return false;
  } else if (!MetricSystemUuidValid(b.label_schema_uuid) || !b.label_schema_generation) return false;
  return d.cluster_only ? MetricSystemUuidValid(b.cluster_uuid) : b.cluster_uuid.is_nil();
}
MetricHistorySeriesKey Key(const MetricHistoryBinding& b, const MetricLabelSet& labels) {
  std::vector<std::pair<std::string, MetricLabelValue>> values;
  values.reserve(labels.size());
  for (const auto& label : labels) values.emplace_back(label.key, label.value);
  std::sort(values.begin(), values.end());
  return {b.database_uuid, b.node_uuid, b.cluster_uuid, b.metric_uuid, b.label_schema_uuid, std::move(values)};
}
bool Text(const std::string& s) { return !s.empty() && s.find('\0') == std::string::npos; }
}  // namespace

MetricHistoryRecordResult<MetricSeriesIdentity> MakeMetricSeriesIdentity(
    const MetricDescriptor& descriptor, MetricLabelSet labels, const MetricRetentionPolicy& policy,
    const MetricHistoryBinding& binding, const MetricUuid& series_uuid, u64 series_generation) {
  if (!MetricSystemUuidValid(series_uuid)) return {E::invalid_identity, {}};
  if (!series_generation || !ValidateStoredMetricValueDescriptor(descriptor) ||
      !BindingValid(descriptor, binding) || !ValidateMetricRetentionPolicy(policy).ok ||
      policy.policy_uuid != binding.retention_policy_uuid || policy.generation != binding.retention_policy_generation ||
      (policy.scope == "cluster") != descriptor.cluster_only) return {E::invalid_binding, {}};
  if (!ValidateMetricLabelSet(descriptor, labels).ok) return {E::invalid_labels, {}};
  MetricSeriesIdentity result;
  static_cast<MetricHistoryBinding&>(result) = binding;
  result.series_uuid = series_uuid;
  result.series_definition_generation = series_generation;
  result.series_key = Key(binding, labels);
  result.metric_family = descriptor.family;
  result.namespace_path = descriptor.namespace_path;
  result.producer_owner = descriptor.producer_owner;
  result.scope_class = descriptor.cluster_only ? "cluster" : "local";
  result.labels = std::move(labels);
  result.redaction_class = std::any_of(descriptor.labels.begin(), descriptor.labels.end(),
      [](const auto& label) { return label.sensitive; }) ? "contains_sensitive_labels" : "none";
  return {E::none, std::move(result)};
}

MetricHistoryRecordResult<MetricRawSampleRecord> MakeMetricRawSampleRecord(
    const MetricDescriptor& descriptor, const MetricSeriesIdentity& series, const MetricValue& value,
    u64 observed, u64 collected, u64 sequence) {
  if (!MetricSystemUuidValid(series.series_uuid)) return {E::invalid_identity, {}};
  if (!series.series_definition_generation || !BindingValid(descriptor, series) || series.series_key != Key(series, series.labels) ||
      series.metric_family != descriptor.family || series.scope_class != (descriptor.cluster_only ? "cluster" : "local"))
    return {E::invalid_binding, {}};
  if (!ValidateMetricLabelSet(descriptor, series.labels).ok || !ValidateMetricLabelSet(descriptor, value.labels).ok ||
      Key(series, value.labels) != series.series_key) return {E::invalid_labels, {}};
  if (!observed || !collected || !sequence || value.family != descriptor.family || value.type != descriptor.type ||
      !ValidateMetricValueShape(descriptor, value))
    return {E::invalid_observation, {}};
  MetricRawSampleRecord result;
  static_cast<MetricHistoryBinding&>(result) = series;
  result.series_uuid = series.series_uuid;
  result.series_definition_generation = series.series_definition_generation;
  result.metric_family = series.metric_family;
  result.labels = series.labels;
  result.value = value;
  result.sample_time_utc_ns = observed;
  result.collection_time_utc_ns = collected;
  result.source_sequence = sequence;
  result.revision = 1;
  // Allocate/copy first; identity is issued only for a complete observation.
  const auto identity = uuid::IssueRuntimeIdentityV7();
  if (!identity) return {E::identity_issuance_failed, {}};
  result.sample_uuid = *identity;
  return {E::none, std::move(result)};
}

MetricHistoryRecordResult<MetricRetentionEvidenceRecord> MakeMetricRetentionEvidenceRecord(
    MetricRetentionEvidenceRecord requested) {
  if (!requested.evidence_uuid.is_nil() || !MetricSystemUuidValid(requested.policy_uuid) ||
      !MetricSystemUuidValid(requested.actor_uuid) || !MetricSystemUuidValid(requested.transaction_uuid) ||
      (!requested.series_uuid.is_nil() && !MetricSystemUuidValid(requested.series_uuid)))
    return {E::invalid_identity, {}};
  if (!Text(requested.operation) || !Text(requested.decision)) return {E::invalid_observation, {}};
  const auto identity = uuid::IssueRuntimeIdentityV7();
  if (!identity) return {E::identity_issuance_failed, {}};
  requested.evidence_uuid = *identity;
  return {E::none, std::move(requested)};
}
}  // namespace scratchbird::core::metrics
