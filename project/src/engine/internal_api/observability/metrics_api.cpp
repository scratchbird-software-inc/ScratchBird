// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/metrics_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "crud_support/crud_store.hpp"
#include "metric_history.hpp"
#include "metric_registry.hpp"
#include "metric_retention_policy.hpp"
#include "security/security_model.hpp"

#include <map>
#include <set>
#include <sstream>

namespace scratchbird::engine::internal_api {
namespace {

using scratchbird::core::metrics::MetricDescriptor;
using scratchbird::core::metrics::MetricLabelSet;
using scratchbird::core::metrics::MetricReadinessName;
using scratchbird::core::metrics::MetricType;
using scratchbird::core::metrics::MetricRetentionPolicy;
using scratchbird::core::metrics::MetricTypeName;
using scratchbird::core::metrics::MetricUnit;
using scratchbird::core::metrics::MetricUnitName;
using scratchbird::core::metrics::MetricValue;

bool HasMetricsReadRight(const EngineRequestContext& context) {
  return SecurityContextHasRight(context, "OBS_METRICS_READ_ALL") ||
         SecurityContextHasRight(context, "OBS_METRICS_READ_FAMILY") ||
         SecurityContextHasRight(context, "MGA_METRICS_READ") ||
         SecurityContextHasRight(context, "OBS_METRICS_READ_SELF") ||
         SecurityContextHasRight(context, "OBS_METRICS_READ_DATABASE") ||
         SecurityContextHasRight(context, "OBS_METRICS_READ_NODE") ||
         SecurityContextHasRight(context, "OBS_METRICS_READ_CLUSTER");
}

void AddPublicExactMetricsEvidence(EngineApiResult* result, const EngineApiRequest& request) {
  const auto result_shape = SecurityOptionValue(request, "result_shape_contract:");
  if (result_shape.empty()) return;
  AddApiBehaviorEvidence(result, "public_sbsql_operation", "observability.show_metrics");
  AddApiBehaviorEvidence(result, "engine_api_function", "EngineInspectShowOperation");
  AddApiBehaviorEvidence(result, "result_shape_contract", result_shape);
  result->result_shape.result_kind = result_shape;
}

bool HasSensitiveMetricRight(const EngineRequestContext& context) {
  return SecurityContextHasRight(context, "OBS_METRICS_READ_ALL") ||
         SecurityContextHasRight(context, "OBS_METRICS_EXPORT") ||
         SecurityContextHasTag(context, "security.bootstrap");
}

bool HasMetricsRetentionControlRight(const EngineRequestContext& context) {
  return SecurityContextHasRight(context, "OBS_METRICS_RETENTION_CONTROL") ||
         SecurityContextHasRight(context, "OBS_METRICS_EXPORT_CONTROL") ||
         SecurityContextHasTag(context, "security.bootstrap");
}

bool DescriptorMatches(const EngineApiRequest& request, const MetricDescriptor& descriptor, bool cluster_surface) {
  if (cluster_surface != descriptor.cluster_only) {
    return false;
  }
  const auto family = SecurityOptionValue(request, "family:");
  if (!family.empty() && family != descriptor.family) {
    return false;
  }
  const auto namespace_prefix = SecurityOptionValue(request, "namespace:");
  if (!namespace_prefix.empty() && descriptor.namespace_path.rfind(namespace_prefix, 0) != 0) {
    return false;
  }
  const auto producer = SecurityOptionValue(request, "producer:");
  if (!producer.empty() && producer != descriptor.producer_owner) {
    return false;
  }
  const auto type = SecurityOptionValue(request, "type:");
  if (!type.empty() && type != MetricTypeName(descriptor.type)) {
    return false;
  }
  return true;
}

std::string LabelsToText(const MetricLabelSet& labels) {
  std::ostringstream out;
  bool first = true;
  for (const auto& label : labels) {
    if (!first) {
      out << ",";
    }
    out << label.key << "=" << label.value;
    first = false;
  }
  return out.str();
}

EngineApiU64 ParseApiU64(const std::string& value, EngineApiU64 fallback = 0) {
  if (value.empty()) {
    return fallback;
  }
  try {
    return static_cast<EngineApiU64>(std::stoull(value));
  } catch (...) {
    return fallback;
  }
}

std::string MetricHistoryPath(const EngineApiRequest& request) {
  const auto explicit_path = SecurityOptionValue(request, "history_path:");
  if (!explicit_path.empty()) {
    return explicit_path;
  }
  const auto configured = scratchbird::core::metrics::ConfiguredMetricHistoryPath();
  if (!configured.empty()) {
    return configured;
  }
  if (!request.context.database_path.empty()) {
    return request.context.database_path + ".metrics.history";
  }
  return {};
}

template <typename TResult>
TResult MetricsSecurityFailure(const EngineApiRequest& request, const std::string& operation_id) {
  return MakeApiBehaviorDiagnostic<TResult>(
      request.context,
      operation_id,
      MakeSecurityContextRequiredDiagnostic(operation_id));
}

template <typename TResult>
TResult ClusterMetricsUnavailable(const EngineApiRequest& request, const std::string& operation_id) {
  auto result = MakeApiBehaviorDiagnostic<TResult>(
      request.context,
      operation_id,
      MakeClusterAuthorityUnavailableDiagnostic(operation_id));
  result.cluster_authority_required = true;
  AddApiBehaviorEvidence(&result, "cluster_metrics", "cluster_authority_unavailable");
  return result;
}

void AddDescriptorRow(EngineApiResult* result, const MetricDescriptor& descriptor) {
  AddApiBehaviorRow(result,
                    {{"metric", descriptor.family},
                     {"namespace", descriptor.namespace_path},
                     {"type", MetricTypeName(descriptor.type)},
                     {"unit", MetricUnitName(descriptor.unit)},
                     {"producer_owner", descriptor.producer_owner},
                     {"readiness", MetricReadinessName(descriptor.readiness)},
                     {"security_family", descriptor.security_family},
                     {"cluster_only", descriptor.cluster_only ? "true" : "false"}});
}

void AddValueRow(EngineApiResult* result,
                 const MetricDescriptor& descriptor,
                 MetricValue value,
                 bool allow_sensitive_labels) {
  value = scratchbird::core::metrics::RedactSensitiveMetricValue(descriptor, std::move(value), allow_sensitive_labels);
  AddApiBehaviorRow(result,
                    {{"metric", value.family},
                     {"namespace", descriptor.namespace_path},
                     {"type", MetricTypeName(value.type)},
                     {"value", std::to_string(value.value)},
                     {"count", std::to_string(value.count)},
                     {"sum", std::to_string(value.sum)},
                     {"state_text", value.state_text},
                     {"labels", LabelsToText(value.labels)}});
}

void AddSeriesRow(EngineApiResult* result,
                  const MetricDescriptor& descriptor,
                  const scratchbird::core::metrics::MetricSeriesIdentity& series,
                  bool allow_sensitive_labels) {
  const auto labels = scratchbird::core::metrics::RedactSensitiveLabels(descriptor, series.labels, allow_sensitive_labels);
  const bool redact_series_identity = !allow_sensitive_labels && series.redaction_class != "none";
  AddApiBehaviorRow(result,
                    {{"series_uuid", series.series_uuid},
                     {"series_key", redact_series_identity ? "<redacted>" : series.series_key},
                     {"metric", series.metric_family},
                     {"namespace", series.namespace_path},
                     {"producer_owner", series.producer_owner},
                     {"scope_class", series.scope_class},
                     {"database_uuid", series.database_uuid},
                     {"node_uuid", series.node_uuid},
                     {"cluster_uuid", series.cluster_uuid},
                     {"label_hash", series.label_hash},
                     {"labels", LabelsToText(labels)},
                     {"redaction_class", series.redaction_class},
                     {"retention_policy_uuid", series.retention_policy_uuid}});
}

void AddRawHistoryRow(EngineApiResult* result,
                      const MetricDescriptor& descriptor,
                      scratchbird::core::metrics::MetricRawSampleRecord sample,
                      bool allow_sensitive_labels) {
  sample.value.labels = sample.labels;
  sample.value = scratchbird::core::metrics::RedactSensitiveMetricValue(
      descriptor,
      std::move(sample.value),
      allow_sensitive_labels);
  AddApiBehaviorRow(result,
                    {{"sample_uuid", sample.sample_uuid},
                     {"series_uuid", sample.series_uuid},
                     {"metric", sample.metric_family},
                     {"namespace", descriptor.namespace_path},
                     {"type", MetricTypeName(sample.value.type)},
                     {"observation_time_microseconds", std::to_string(sample.observation_time_microseconds)},
                     {"collection_time_microseconds", std::to_string(sample.collection_time_microseconds)},
                     {"publish_time_microseconds", std::to_string(sample.publish_time_microseconds)},
                     {"source_sequence", std::to_string(sample.source_sequence)},
                     {"clock_quality", sample.clock_quality},
                     {"freshness_class", sample.freshness_class},
                     {"value", std::to_string(sample.value.value)},
                     {"count", std::to_string(sample.value.count)},
                     {"sum", std::to_string(sample.value.sum)},
                     {"state_text", sample.value.state_text},
                     {"labels", LabelsToText(sample.value.labels)},
                     {"evidence_uuid", sample.evidence_uuid}});
}

void AddRollupRow(EngineApiResult* result, const scratchbird::core::metrics::MetricRollupRecord& rollup) {
  AddApiBehaviorRow(result,
                    {{"rollup_uuid", rollup.rollup_uuid},
                     {"series_uuid", rollup.series_uuid},
                     {"metric", rollup.metric_family},
                     {"rollup_grain", scratchbird::core::metrics::MetricRollupGrainName(rollup.grain)},
                     {"window_start_microseconds", std::to_string(rollup.window_start_microseconds)},
                     {"window_end_microseconds", std::to_string(rollup.window_end_microseconds)},
                     {"sample_count", std::to_string(rollup.sample_count)},
                     {"min_value", std::to_string(rollup.min_value)},
                     {"max_value", std::to_string(rollup.max_value)},
                     {"avg_value", std::to_string(rollup.avg_value)},
                     {"last_value", std::to_string(rollup.last_value)},
                     {"sum_value", std::to_string(rollup.sum_value)},
                     {"histogram_count", std::to_string(rollup.histogram_count)},
                     {"histogram_sum", std::to_string(rollup.histogram_sum)},
                     {"last_state_text", rollup.last_state_text},
                     {"evidence_uuid", rollup.evidence_uuid}});
}

void AddRetentionPolicyRow(EngineApiResult* result, const MetricRetentionPolicy& policy, bool editable) {
  AddApiBehaviorRow(result,
                    {{"policy_uuid", policy.policy_uuid},
                     {"policy_name", policy.policy_name},
                     {"scope", policy.scope},
                     {"mode", scratchbird::core::metrics::MetricRetentionModeName(policy.mode)},
                     {"raw_retention_seconds", std::to_string(policy.raw_retention_seconds)},
                     {"rollup_retention_seconds", std::to_string(policy.rollup_retention_seconds)},
                     {"purge_batch_limit", std::to_string(policy.purge_batch_limit)},
                     {"max_cardinality", std::to_string(policy.max_cardinality)},
                     {"overflow_behavior", policy.overflow_behavior},
                     {"edit_right", policy.edit_right},
                     {"default_admin_group", policy.default_admin_group},
                     {"evidence_required", policy.evidence_required ? "true" : "false"},
                     {"editable", editable ? "true" : "false"}});
}

void EnsureLifecycleMetricDescriptor(const std::string& family,
                                     MetricType type,
                                     MetricUnit unit,
                                     const std::string& namespace_path,
                                     const std::string& help) {
  auto& registry = scratchbird::core::metrics::DefaultMetricRegistry();
  if (registry.FindDescriptor(family) != nullptr) return;
  MetricDescriptor descriptor;
  descriptor.family = family;
  descriptor.type = type;
  descriptor.unit = unit;
  descriptor.namespace_path = namespace_path;
  descriptor.help = help;
  descriptor.producer_owner = "database_lifecycle_observability";
  descriptor.security_family = "OBS_METRICS_READ_FAMILY";
  descriptor.labels = {
      {"operation", false, false},
      {"result", false, false},
      {"reason", false, false},
      {"component", false, false},
      {"database_uuid", false, false},
      {"session_uuid", false, true},
      {"principal_uuid", false, true},
      {"diagnostic_code", false, false},
      {"cache_family", false, false},
      {"route_class", false, false}};
  (void)registry.RegisterDescriptor(std::move(descriptor));
}

void EnsureLifecycleMetricDescriptors() {
  EnsureLifecycleMetricDescriptor("sb_lifecycle_operation_total",
                                  MetricType::counter,
                                  MetricUnit::count,
                                  "sys.metrics.lifecycle",
                                  "Lifecycle operations by route and result.");
  EnsureLifecycleMetricDescriptor("sb_lifecycle_diagnostic_total",
                                  MetricType::counter,
                                  MetricUnit::count,
                                  "sys.metrics.lifecycle.diagnostics",
                                  "Lifecycle diagnostics emitted by canonical message vector code.");
  EnsureLifecycleMetricDescriptor("sb_lifecycle_cache_invalidation_total",
                                  MetricType::counter,
                                  MetricUnit::count,
                                  "sys.metrics.lifecycle.cache",
                                  "Lifecycle cache invalidation markers by family and reason.");
  EnsureLifecycleMetricDescriptor("sb_lifecycle_audit_event_total",
                                  MetricType::counter,
                                  MetricUnit::count,
                                  "sys.metrics.lifecycle.audit",
                                  "Lifecycle audit evidence emitted before visible route completion.");
}

template <typename TResult>
TResult RegistrySurface(const EngineApiRequest& request, const std::string& operation_id, bool cluster_surface) {
  if (!HasMetricsReadRight(request.context)) {
    return MetricsSecurityFailure<TResult>(request, operation_id);
  }
  if (cluster_surface && !request.context.cluster_authority_available) {
    return ClusterMetricsUnavailable<TResult>(request, operation_id);
  }
  auto result = MakeApiBehaviorSuccess<TResult>(request.context, operation_id);
  for (const auto& descriptor : scratchbird::core::metrics::DefaultMetricRegistry().Descriptors(true)) {
    if (DescriptorMatches(request, descriptor, cluster_surface)) {
      AddDescriptorRow(&result, descriptor);
    }
  }
  AddApiBehaviorEvidence(&result, "metrics_surface", cluster_surface ? "cluster.sys.metrics.registry" : "sys.metrics.registry");
  return result;
}

template <typename TResult>
TResult CurrentSurface(const EngineApiRequest& request, const std::string& operation_id, bool cluster_surface) {
  if (!HasMetricsReadRight(request.context)) {
    return MetricsSecurityFailure<TResult>(request, operation_id);
  }
  if (cluster_surface && !request.context.cluster_authority_available) {
    return ClusterMetricsUnavailable<TResult>(request, operation_id);
  }
  auto result = MakeApiBehaviorSuccess<TResult>(request.context, operation_id);
  const bool allow_sensitive = HasSensitiveMetricRight(request.context);
  const auto descriptors = scratchbird::core::metrics::DefaultMetricRegistry().Descriptors(true);
  const auto values = scratchbird::core::metrics::DefaultMetricRegistry().SnapshotCurrent(true);
  std::map<std::string, MetricDescriptor> by_family;
  for (const auto& descriptor : descriptors) {
    by_family[descriptor.family] = descriptor;
  }
  for (const auto& value : values) {
    const auto descriptor = by_family.find(value.family);
    if (descriptor == by_family.end()) {
      continue;
    }
    if (DescriptorMatches(request, descriptor->second, cluster_surface)) {
      AddValueRow(&result, descriptor->second, value, allow_sensitive);
    }
  }
  AddApiBehaviorEvidence(&result, "metrics_surface", cluster_surface ? "cluster.sys.metrics.current" : "sys.metrics.current");
  AddApiBehaviorEvidence(&result, "redaction", allow_sensitive ? "sensitive_visible" : "sensitive_redacted");
  return result;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_OBSERVABILITY_METRICS_API_BEHAVIOR
EngineShowMetricsResult EngineShowMetrics(const EngineShowMetricsRequest& request) {
  if (!HasMetricsReadRight(request.context)) {
    return MetricsSecurityFailure<EngineShowMetricsResult>(request, "observability.show_metrics");
  }
  for (const auto& option : request.option_envelopes) {
    if ((option == "require_metrics_right" || option == "family:restricted") && !request.context.security_context_present) {
      return MakeApiBehaviorDiagnostic<EngineShowMetricsResult>(
          request.context,
          "observability.show_metrics",
          MakeSecurityContextRequiredDiagnostic("observability.show_metrics"));
    }
  }
  const bool include_cluster = SecurityOptionPresent(request, "include_cluster") ||
                               SecurityOptionBool(request, "include_cluster:", false);
  if (include_cluster && !request.context.cluster_authority_available) {
    return ClusterMetricsUnavailable<EngineShowMetricsResult>(request, "observability.show_metrics");
  }
  auto result = MakeApiBehaviorSuccess<EngineShowMetricsResult>(request.context, "observability.show_metrics");
  const auto descriptors = scratchbird::core::metrics::DefaultMetricRegistry().Descriptors(include_cluster);
  const auto current = scratchbird::core::metrics::DefaultMetricRegistry().SnapshotCurrent(include_cluster);
  for (const auto& descriptor : descriptors) {
    if (!DescriptorMatches(request, descriptor, descriptor.cluster_only)) {
      continue;
    }
    std::string value = "";
    std::string count = "";
    std::string sum = "";
    for (const auto& sample : current) {
      if (sample.family == descriptor.family) {
        value = std::to_string(sample.value);
        count = std::to_string(sample.count);
        sum = std::to_string(sample.sum);
        break;
      }
    }
    AddApiBehaviorRow(&result,
                      {{"metric", descriptor.family},
                       {"namespace", descriptor.namespace_path},
                       {"type", scratchbird::core::metrics::MetricTypeName(descriptor.type)},
                       {"unit", scratchbird::core::metrics::MetricUnitName(descriptor.unit)},
                       {"producer_owner", descriptor.producer_owner},
                       {"readiness", scratchbird::core::metrics::MetricReadinessName(descriptor.readiness)},
                       {"value", value},
                       {"count", count},
                       {"sum", sum}});
  }
  AddApiBehaviorEvidence(&result, "metrics_registry", "local_node");
  AddApiBehaviorEvidence(&result, "metrics_source", "core_metrics_registry");
  AddApiBehaviorEvidence(&result, "metrics_scope", request.context.security_context_present ? "permission_checked" : "baseline");
  AddPublicExactMetricsEvidence(&result, request);
  return result;
}

EngineSysMetricsRegistryResult EngineSysMetricsRegistry(const EngineSysMetricsRegistryRequest& request) {
  return RegistrySurface<EngineSysMetricsRegistryResult>(request, "observability.sys_metrics.registry", false);
}

EngineSysMetricsCurrentResult EngineSysMetricsCurrent(const EngineSysMetricsCurrentRequest& request) {
  return CurrentSurface<EngineSysMetricsCurrentResult>(request, "observability.sys_metrics.current", false);
}

EngineSysMetricsHistoryResult EngineSysMetricsHistory(const EngineSysMetricsHistoryRequest& request) {
  if (!HasMetricsReadRight(request.context)) {
    return MetricsSecurityFailure<EngineSysMetricsHistoryResult>(request, "observability.sys_metrics.history");
  }
  auto result = MakeApiBehaviorSuccess<EngineSysMetricsHistoryResult>(request.context, "observability.sys_metrics.history");
  const bool allow_sensitive = HasSensitiveMetricRight(request.context);
  const auto descriptors = scratchbird::core::metrics::DefaultMetricRegistry().Descriptors(false);
  std::map<std::string, MetricDescriptor> by_family;
  for (const auto& descriptor : descriptors) {
    by_family[descriptor.family] = descriptor;
  }
  const auto path = MetricHistoryPath(request);
  if (!path.empty()) {
    const auto store = scratchbird::core::metrics::LoadMetricHistoryStore(path);
    for (auto sample : store.raw_samples) {
      const auto descriptor = by_family.find(sample.metric_family);
      if (descriptor != by_family.end() && DescriptorMatches(request, descriptor->second, false)) {
        AddRawHistoryRow(&result, descriptor->second, std::move(sample), allow_sensitive);
      }
    }
    AddApiBehaviorEvidence(&result, "metrics_surface", "sys.metrics.history");
    AddApiBehaviorEvidence(&result, "metrics_history_source", "persistent_history_store");
    return result;
  }
  const auto values = scratchbird::core::metrics::DefaultMetricRegistry().SnapshotHistory(false, 1024);
  for (const auto& value : values) {
    const auto descriptor = by_family.find(value.family);
    if (descriptor != by_family.end() && DescriptorMatches(request, descriptor->second, false)) {
      AddValueRow(&result, descriptor->second, value, allow_sensitive);
    }
  }
  AddApiBehaviorEvidence(&result, "metrics_surface", "sys.metrics.history");
  AddApiBehaviorEvidence(&result, "metrics_history_source", "bounded_runtime_history");
  return result;
}

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_METRICS_HISTORY_RETENTION
EngineSysMetricsPersistentHistoryResult EngineSysMetricsPersistentHistory(
    const EngineSysMetricsPersistentHistoryRequest& request) {
  EngineSysMetricsHistoryRequest history_request;
  history_request.context = request.context;
  history_request.operation_id = request.operation_id;
  history_request.target_database = request.target_database;
  history_request.target_schema = request.target_schema;
  history_request.target_object = request.target_object;
  history_request.related_objects = request.related_objects;
  history_request.option_envelopes = request.option_envelopes;
  auto history = EngineSysMetricsHistory(history_request);
  EngineSysMetricsPersistentHistoryResult result;
  static_cast<EngineApiResult&>(result) = std::move(static_cast<EngineApiResult&>(history));
  result.operation_id = "observability.sys_metrics.persistent_history";
  AddApiBehaviorEvidence(&result, "metrics_surface", "sys.metrics.persistent_history");
  return result;
}

EngineSysMetricsRollupsResult EngineSysMetricsRollups(const EngineSysMetricsRollupsRequest& request) {
  if (!HasMetricsReadRight(request.context)) {
    return MetricsSecurityFailure<EngineSysMetricsRollupsResult>(request, "observability.sys_metrics.rollups");
  }
  auto result = MakeApiBehaviorSuccess<EngineSysMetricsRollupsResult>(request.context, "observability.sys_metrics.rollups");
  const auto path = MetricHistoryPath(request);
  if (path.empty()) {
    AddApiBehaviorEvidence(&result, "metrics_history", "not_configured");
    return result;
  }
  const auto store = scratchbird::core::metrics::LoadMetricHistoryStore(path);
  const auto descriptors = scratchbird::core::metrics::DefaultMetricRegistry().Descriptors(false);
  std::map<std::string, MetricDescriptor> by_family;
  for (const auto& descriptor : descriptors) {
    by_family[descriptor.family] = descriptor;
  }
  for (const auto& rollup : store.rollups) {
    const auto descriptor = by_family.find(rollup.metric_family);
    if (descriptor != by_family.end() && DescriptorMatches(request, descriptor->second, false)) {
      AddRollupRow(&result, rollup);
    }
  }
  AddApiBehaviorEvidence(&result, "metrics_surface", "sys.metrics.rollups");
  return result;
}

EngineSysMetricsSeriesResult EngineSysMetricsSeries(const EngineSysMetricsSeriesRequest& request) {
  if (!HasMetricsReadRight(request.context)) {
    return MetricsSecurityFailure<EngineSysMetricsSeriesResult>(request, "observability.sys_metrics.series");
  }
  auto result = MakeApiBehaviorSuccess<EngineSysMetricsSeriesResult>(request.context, "observability.sys_metrics.series");
  const auto path = MetricHistoryPath(request);
  if (path.empty()) {
    AddApiBehaviorEvidence(&result, "metrics_history", "not_configured");
    return result;
  }
  const bool allow_sensitive = HasSensitiveMetricRight(request.context);
  const auto store = scratchbird::core::metrics::LoadMetricHistoryStore(path);
  const auto descriptors = scratchbird::core::metrics::DefaultMetricRegistry().Descriptors(false);
  std::map<std::string, MetricDescriptor> by_family;
  for (const auto& descriptor : descriptors) {
    by_family[descriptor.family] = descriptor;
  }
  for (const auto& series : store.series) {
    const auto descriptor = by_family.find(series.metric_family);
    if (descriptor != by_family.end() && DescriptorMatches(request, descriptor->second, false)) {
      AddSeriesRow(&result, descriptor->second, series, allow_sensitive);
    }
  }
  AddApiBehaviorEvidence(&result, "metrics_surface", "sys.metrics.series");
  AddApiBehaviorEvidence(&result, "redaction", allow_sensitive ? "sensitive_visible" : "sensitive_redacted");
  return result;
}

EngineSysMetricsRetentionPoliciesResult EngineSysMetricsRetentionPolicies(
    const EngineSysMetricsRetentionPoliciesRequest& request) {
  if (!HasMetricsReadRight(request.context) && !HasMetricsRetentionControlRight(request.context)) {
    return MetricsSecurityFailure<EngineSysMetricsRetentionPoliciesResult>(
        request,
        "observability.sys_metrics.retention_policies");
  }
  auto result = MakeApiBehaviorSuccess<EngineSysMetricsRetentionPoliciesResult>(
      request.context,
      "observability.sys_metrics.retention_policies");
  auto policies = scratchbird::core::metrics::BaselineMetricRetentionPolicies();
  const auto path = MetricHistoryPath(request);
  if (!path.empty()) {
    const auto store = scratchbird::core::metrics::LoadMetricHistoryStore(path);
    if (!store.policies.empty()) {
      policies = store.policies;
    }
  }
  const bool editable = HasMetricsRetentionControlRight(request.context);
  for (const auto& policy : policies) {
    const auto name = SecurityOptionValue(request, "policy_name:");
    if (!name.empty() && name != policy.policy_name && name != policy.policy_uuid) {
      continue;
    }
    AddRetentionPolicyRow(&result, policy, editable);
  }
  AddApiBehaviorEvidence(&result, "metrics_surface", "sys.metrics.retention_policies");
  return result;
}

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_METRICS_RETENTION_POLICY_EDIT
EngineAlterMetricRetentionPolicyResult EngineAlterMetricRetentionPolicy(
    const EngineAlterMetricRetentionPolicyRequest& request) {
  if (!HasMetricsRetentionControlRight(request.context)) {
    return MetricsSecurityFailure<EngineAlterMetricRetentionPolicyResult>(
        request,
        "observability.alter_metric_retention_policy");
  }
  const auto path = MetricHistoryPath(request);
  if (path.empty()) {
    return MakeApiBehaviorDiagnostic<EngineAlterMetricRetentionPolicyResult>(
        request.context,
        "observability.alter_metric_retention_policy",
        MakeInvalidRequestDiagnostic("observability.alter_metric_retention_policy", "history_path_required"));
  }
  MetricRetentionPolicy policy;
  policy.policy_name = SecurityOptionValue(request, "policy_name:");
  if (policy.policy_name.empty()) {
    return MakeApiBehaviorDiagnostic<EngineAlterMetricRetentionPolicyResult>(
        request.context,
        "observability.alter_metric_retention_policy",
        MakeInvalidRequestDiagnostic("observability.alter_metric_retention_policy", "policy_name_required"));
  }
  policy.policy_uuid = SecurityOptionValue(request, "policy_uuid:");
  if (policy.policy_uuid.empty()) {
    policy.policy_uuid = scratchbird::core::metrics::StableV7LikeMetricUuid("retention-policy:" + policy.policy_name);
  }
  const auto mode = SecurityOptionValue(request, "mode:");
  policy.mode = scratchbird::core::metrics::MetricRetentionModeFromName(mode.empty() ? "current_only" : mode);
  policy.scope = SecurityOptionValue(request, "scope:").empty() ? "local" : SecurityOptionValue(request, "scope:");
  policy.raw_retention_seconds = ParseApiU64(SecurityOptionValue(request, "raw_retention_seconds:"), 0);
  policy.rollup_retention_seconds = ParseApiU64(SecurityOptionValue(request, "rollup_retention_seconds:"), 0);
  policy.purge_batch_limit = ParseApiU64(SecurityOptionValue(request, "purge_batch_limit:"), 1024);
  policy.max_cardinality = ParseApiU64(SecurityOptionValue(request, "max_cardinality:"), 4096);
  policy.overflow_behavior = SecurityOptionValue(request, "overflow_behavior:").empty()
      ? "reject_and_evidence"
      : SecurityOptionValue(request, "overflow_behavior:");
  policy.edit_right = SecurityOptionValue(request, "edit_right:").empty()
      ? "OBS_METRICS_RETENTION_CONTROL"
      : SecurityOptionValue(request, "edit_right:");
  policy.default_admin_group = SecurityOptionValue(request, "default_admin_group:").empty()
      ? "OPS"
      : SecurityOptionValue(request, "default_admin_group:");
  policy.evidence_required = true;
  const auto rollup_grain = SecurityOptionValue(request, "rollup_grain:");
  if (!rollup_grain.empty()) {
    policy.rollup_grains.push_back(scratchbird::core::metrics::MetricRollupGrainFromName(rollup_grain));
  }
  const auto persisted = scratchbird::core::metrics::UpsertMetricRetentionPolicy(
      path,
      policy,
      request.context.principal_uuid.canonical.empty() ? "system.metrics_admin" : request.context.principal_uuid.canonical,
      request.context.transaction_uuid.canonical);
  if (!persisted.ok) {
    return MakeApiBehaviorDiagnostic<EngineAlterMetricRetentionPolicyResult>(
        request.context,
        "observability.alter_metric_retention_policy",
        MakeInvalidRequestDiagnostic("observability.alter_metric_retention_policy", persisted.diagnostic_code + ":" + persisted.detail));
  }
  auto result = MakeApiBehaviorSuccess<EngineAlterMetricRetentionPolicyResult>(
      request.context,
      "observability.alter_metric_retention_policy");
  AddRetentionPolicyRow(&result, policy, true);
  AddApiBehaviorEvidence(&result, "metric_retention_policy", policy.policy_name);
  AddApiBehaviorEvidence(&result, "evidence_before_success", "policy_upsert");
  return result;
}

EngineSysMetricsLabelsResult EngineSysMetricsLabels(const EngineSysMetricsLabelsRequest& request) {
  if (!HasMetricsReadRight(request.context)) {
    return MetricsSecurityFailure<EngineSysMetricsLabelsResult>(request, "observability.sys_metrics.labels");
  }
  auto result = MakeApiBehaviorSuccess<EngineSysMetricsLabelsResult>(request.context, "observability.sys_metrics.labels");
  for (const auto& descriptor : scratchbird::core::metrics::DefaultMetricRegistry().Descriptors(false)) {
    if (!DescriptorMatches(request, descriptor, false)) {
      continue;
    }
    for (const auto& label : descriptor.labels) {
      AddApiBehaviorRow(&result,
                        {{"metric", descriptor.family},
                         {"label", label.key},
                         {"required", label.required ? "true" : "false"},
                         {"sensitive", label.sensitive ? "true" : "false"}});
    }
  }
  AddApiBehaviorEvidence(&result, "metrics_surface", "sys.metrics.labels");
  return result;
}

EngineSysMetricsProducersResult EngineSysMetricsProducers(const EngineSysMetricsProducersRequest& request) {
  if (!HasMetricsReadRight(request.context)) {
    return MetricsSecurityFailure<EngineSysMetricsProducersResult>(request, "observability.sys_metrics.producers");
  }
  auto result = MakeApiBehaviorSuccess<EngineSysMetricsProducersResult>(request.context, "observability.sys_metrics.producers");
  std::map<std::string, std::pair<int, int>> counts;
  for (const auto& descriptor : scratchbird::core::metrics::DefaultMetricRegistry().Descriptors(false)) {
    auto& count = counts[descriptor.producer_owner];
    ++count.first;
    if (descriptor.readiness == scratchbird::core::metrics::MetricReadiness::contract_ready_unwired) {
      ++count.second;
    }
  }
  for (const auto& [producer, count] : counts) {
    AddApiBehaviorRow(&result,
                      {{"producer_owner", producer},
                       {"families", std::to_string(count.first)},
                       {"contract_ready_unwired", std::to_string(count.second)}});
  }
  AddApiBehaviorEvidence(&result, "metrics_surface", "sys.metrics.producers");
  return result;
}

EngineClusterSysMetricsRegistryResult EngineClusterSysMetricsRegistry(
    const EngineClusterSysMetricsRegistryRequest& request) {
  return RegistrySurface<EngineClusterSysMetricsRegistryResult>(request, "observability.cluster_sys_metrics.registry", true);
}

EngineClusterSysMetricsCurrentResult EngineClusterSysMetricsCurrent(
    const EngineClusterSysMetricsCurrentRequest& request) {
  return CurrentSurface<EngineClusterSysMetricsCurrentResult>(request, "observability.cluster_sys_metrics.current", true);
}

EngineClusterSysMetricsHistoryResult EngineClusterSysMetricsHistory(
    const EngineClusterSysMetricsHistoryRequest& request) {
  if (!HasMetricsReadRight(request.context)) {
    return MetricsSecurityFailure<EngineClusterSysMetricsHistoryResult>(
        request,
        "observability.cluster_sys_metrics.history");
  }
  if (!request.context.cluster_authority_available) {
    return ClusterMetricsUnavailable<EngineClusterSysMetricsHistoryResult>(
        request,
        "observability.cluster_sys_metrics.history");
  }
  auto result = MakeApiBehaviorSuccess<EngineClusterSysMetricsHistoryResult>(
      request.context,
      "observability.cluster_sys_metrics.history");
  const auto path = MetricHistoryPath(request);
  if (path.empty()) {
    AddApiBehaviorEvidence(&result, "metrics_history", "not_configured");
    return result;
  }
  const bool allow_sensitive = HasSensitiveMetricRight(request.context);
  const auto store = scratchbird::core::metrics::LoadMetricHistoryStore(path);
  const auto descriptors = scratchbird::core::metrics::DefaultMetricRegistry().Descriptors(true);
  std::map<std::string, MetricDescriptor> by_family;
  for (const auto& descriptor : descriptors) {
    by_family[descriptor.family] = descriptor;
  }
  for (auto sample : store.raw_samples) {
    const auto descriptor = by_family.find(sample.metric_family);
    if (descriptor != by_family.end() && DescriptorMatches(request, descriptor->second, true)) {
      AddRawHistoryRow(&result, descriptor->second, std::move(sample), allow_sensitive);
    }
  }
  AddApiBehaviorEvidence(&result, "metrics_surface", "cluster.sys.metrics.history");
  AddApiBehaviorEvidence(&result, "cluster_authority", "available");
  return result;
}

EngineInspectMetricAdapterContractResult EngineInspectMetricAdapterContract(
    const EngineInspectMetricAdapterContractRequest& request) {
  if (!SecurityContextHasRight(request.context, "OBS_METRICS_EXPORT") &&
      !SecurityContextHasRight(request.context, "OBS_METRICS_EXPORT_CONTROL")) {
    return MetricsSecurityFailure<EngineInspectMetricAdapterContractResult>(request, "observability.metric_adapter.contract");
  }
  const std::string format = SecurityOptionValue(request, "format:").empty()
      ? "openmetrics"
      : SecurityOptionValue(request, "format:");
  if (format != "openmetrics" && format != "prometheus" && format != "json_snapshot") {
    return MakeApiBehaviorDiagnostic<EngineInspectMetricAdapterContractResult>(
        request.context,
        "observability.metric_adapter.contract",
        MakeInvalidRequestDiagnostic("observability.metric_adapter.contract", "unsupported_adapter_format:" + format));
  }
  auto result = MakeApiBehaviorSuccess<EngineInspectMetricAdapterContractResult>(
      request.context,
      "observability.metric_adapter.contract");
  AddApiBehaviorRow(&result,
                    {{"format", format},
                     {"authority", "core_metrics_registry"},
                     {"security_context_required", "true"},
                     {"redaction", "registry_label_policy"},
                     {"retention_limit_source", "metric_descriptor_policy"},
                     {"failure_behavior", "fail_closed_without_independent_authority"}});
  AddApiBehaviorEvidence(&result, "metrics_adapter_contract", format);
  return result;
}

EngineRecordLifecycleMetricResult EngineRecordLifecycleMetric(
    const EngineRecordLifecycleMetricRequest& request) {
  if (!HasMetricsReadRight(request.context)) {
    return MetricsSecurityFailure<EngineRecordLifecycleMetricResult>(
        request,
        "observability.lifecycle.record_metric");
  }
  EnsureLifecycleMetricDescriptors();
  const std::string operation = request.operation_key.empty()
      ? SecurityOptionValue(request, "operation_key:")
      : request.operation_key;
  if (operation.empty()) {
    return MakeApiBehaviorDiagnostic<EngineRecordLifecycleMetricResult>(
        request.context,
        "observability.lifecycle.record_metric",
        MakeInvalidRequestDiagnostic("observability.lifecycle.record_metric", "operation_key_required"));
  }
  const std::string outcome = request.outcome.empty()
      ? SecurityOptionValue(request, "outcome:")
      : request.outcome;
  const std::string result_class =
      outcome == "refused" || outcome == "failed" ? "failure" : "success";
  const std::string route = request.route_family.empty()
      ? SecurityOptionValue(request, "route_family:")
      : request.route_family;
  auto& registry = scratchbird::core::metrics::DefaultMetricRegistry();
  (void)registry.IncrementCounter(
      "sb_lifecycle_operation_total",
      {{"operation", operation},
       {"result", result_class},
       {"route_class", route.empty() ? "engine_internal" : route},
       {"database_uuid", request.context.database_uuid.canonical},
       {"session_uuid", request.context.session_uuid.canonical}},
      1,
      "database_lifecycle_observability");
  if (!request.diagnostic_code.empty()) {
    (void)registry.IncrementCounter(
        "sb_lifecycle_diagnostic_total",
        {{"operation", operation},
         {"diagnostic_code", request.diagnostic_code},
         {"result", result_class}},
        1,
        "database_lifecycle_observability");
  }
  (void)registry.IncrementCounter(
      "sb_lifecycle_audit_event_total",
      {{"operation", operation},
       {"result", result_class},
       {"route_class", route.empty() ? "engine_internal" : route},
       {"database_uuid", request.context.database_uuid.canonical},
       {"session_uuid", request.context.session_uuid.canonical},
       {"diagnostic_code", request.diagnostic_code.empty() ? "none" : request.diagnostic_code}},
      1,
      "database_lifecycle_observability");
  EngineRecordLifecycleMetricResult result =
      MakeApiBehaviorSuccess<EngineRecordLifecycleMetricResult>(
          request.context,
          "observability.lifecycle.record_metric");
  result.metric_recorded = true;
  AddApiBehaviorEvidence(&result, "lifecycle_metric", operation + ":" + result_class);
  AddApiBehaviorEvidence(&result,
                         "lifecycle_audit_event",
                         operation + ":" + result_class);
  const bool invalidate = request.cache_invalidation_required ||
                          SecurityOptionBool(request, "cache_invalidation_required:", false);
  if (invalidate) {
    const std::string family = request.cache_family.empty()
        ? SecurityOptionValue(request, "cache_family:")
        : request.cache_family;
    const std::string reason = request.cache_reason.empty()
        ? SecurityOptionValue(request, "cache_reason:")
        : request.cache_reason;
    (void)registry.IncrementCounter(
        "sb_lifecycle_cache_invalidation_total",
        {{"operation", operation},
         {"cache_family", family.empty() ? "lifecycle_metadata" : family},
         {"reason", reason.empty() ? operation : reason}},
        1,
        "database_lifecycle_observability");
    result.cache_invalidation_recorded = true;
    AddApiBehaviorEvidence(&result,
                           "lifecycle_cache_invalidation",
                           (family.empty() ? "lifecycle_metadata" : family) + ":" +
                               (reason.empty() ? operation : reason));
  }
  AddApiBehaviorRow(&result,
                    {{"operation_key", operation},
                     {"outcome", outcome.empty() ? result_class : outcome},
                     {"result_class", result_class},
                     {"route_family", route.empty() ? "engine_internal" : route},
                     {"diagnostic_code", request.diagnostic_code},
                     {"cache_invalidation_recorded",
                      result.cache_invalidation_recorded ? "true" : "false"},
                     {"parser_finality_authority", "false"},
                     {"reference_finality_authority", "false"}});
  return result;
}

}  // namespace scratchbird::engine::internal_api
