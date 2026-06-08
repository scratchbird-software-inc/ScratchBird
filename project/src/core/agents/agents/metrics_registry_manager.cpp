// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/metrics_registry_manager.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the local metrics-registry integrity handler.

#include "metric_history.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {

namespace metrics = scratchbird::core::metrics;

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::engine};
}

void AddEvidence(MetricsRegistryManagerResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

MetricsRegistryManagerResult Finish(MetricsRegistryManagerDecisionKind decision,
                                    Status status,
                                    std::string code,
                                    std::string key,
                                    std::string detail,
                                    bool fail_closed) {
  MetricsRegistryManagerResult result;
  result.status = status;
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.sample_accepted = decision == MetricsRegistryManagerDecisionKind::accept_sample;
  result.sample_rejected =
      decision == MetricsRegistryManagerDecisionKind::reject_metric_sample;
  result.rollup_requested =
      decision == MetricsRegistryManagerDecisionKind::rollup_metrics;
  result.export_shed_requested =
      decision == MetricsRegistryManagerDecisionKind::shed_export;
  result.diagnostic = MakeMetricsRegistryManagerDiagnostic(result.status,
                                                           std::move(code),
                                                           std::move(key),
                                                           std::move(detail));
  AddEvidence(&result, "decision",
              MetricsRegistryManagerDecisionKindName(result.decision));
  AddEvidence(&result, "failed_closed", fail_closed ? "true" : "false");
  return result;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

MetricsRegistryManagerResult Refuse(std::string code,
                                    std::string key,
                                    std::string detail) {
  return Finish(MetricsRegistryManagerDecisionKind::refused, ErrorStatus(),
                std::move(code),
                std::move(key),
                std::move(detail),
                true);
}

MetricsRegistryManagerResult FailMutation(
    const metrics::MetricValidationResult& validation,
    const std::string& operation) {
  return Refuse("SB_AGENT_METRICS_REGISTRY_MUTATION_FAILED",
                "agents.metrics_registry.mutation_failed",
                operation + ":" + validation.diagnostic_code + ":" +
                    validation.detail);
}

metrics::MetricValue ValueForSample(const metrics::MetricDescriptor& descriptor,
                                    const MetricsRegistryManagerSample& sample,
                                    metrics::MetricLabelSet labels) {
  metrics::MetricValue value;
  value.family = descriptor.family;
  value.labels = std::move(labels);
  value.type = descriptor.type;
  switch (descriptor.type) {
    case metrics::MetricType::counter:
      value.value = sample.sample_count == 0
                        ? sample.numeric_value
                        : static_cast<double>(sample.sample_count);
      break;
    case metrics::MetricType::gauge:
    case metrics::MetricType::derived:
      value.value = sample.numeric_value;
      break;
    case metrics::MetricType::histogram:
      value.value = sample.numeric_value;
      value.count = 1;
      value.sum = sample.numeric_value;
      break;
    case metrics::MetricType::state:
      value.value = sample.numeric_value;
      value.state_text = sample.state_text;
      break;
  }
  return value;
}

metrics::MetricValidationResult PublishSampleToRegistry(
    metrics::MetricRegistry* registry,
    const metrics::MetricDescriptor& descriptor,
    const MetricsRegistryManagerSample& sample,
    metrics::MetricLabelSet labels) {
  const double numeric = sample.sample_count == 0
                             ? sample.numeric_value
                             : static_cast<double>(sample.sample_count);
  switch (descriptor.type) {
    case metrics::MetricType::counter:
      return registry->IncrementCounter(descriptor.family,
                                        std::move(labels),
                                        numeric,
                                        descriptor.producer_owner);
    case metrics::MetricType::gauge:
      return registry->SetGauge(descriptor.family,
                                std::move(labels),
                                sample.numeric_value,
                                descriptor.producer_owner);
    case metrics::MetricType::histogram:
      return registry->ObserveHistogram(descriptor.family,
                                        std::move(labels),
                                        sample.numeric_value,
                                        descriptor.producer_owner);
    case metrics::MetricType::state:
      return registry->SetState(descriptor.family,
                                std::move(labels),
                                sample.numeric_value,
                                sample.state_text,
                                descriptor.producer_owner);
    case metrics::MetricType::derived:
      return metrics::MetricError("SB-METRICS-DERIVED-SAMPLE-READONLY",
                                  descriptor.family);
  }
  return metrics::MetricError("SB-METRICS-TYPE-UNKNOWN", descriptor.family);
}

u64 CountRollups(const std::string& path) {
  return static_cast<u64>(metrics::LoadMetricHistoryStore(path).rollups.size());
}

}  // namespace

const char* MetricsRegistryManagerDecisionKindName(
    MetricsRegistryManagerDecisionKind decision) {
  switch (decision) {
    case MetricsRegistryManagerDecisionKind::accept_sample:
      return "accept_sample";
    case MetricsRegistryManagerDecisionKind::reject_metric_sample:
      return "reject_metric_sample";
    case MetricsRegistryManagerDecisionKind::rollup_metrics:
      return "rollup_metrics";
    case MetricsRegistryManagerDecisionKind::shed_export:
      return "shed_export";
    case MetricsRegistryManagerDecisionKind::refused:
      return "refused";
  }
  return "refused";
}

DiagnosticRecord MakeMetricsRegistryManagerDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  return scratchbird::core::platform::MakeDiagnostic(
      status.code,
      status.severity,
      status.subsystem,
      std::move(diagnostic_code),
      std::move(message_key),
      {{"detail", std::move(detail)}},
      {},
      "metrics_registry_manager",
      {});
}

MetricsRegistryManagerResult EvaluateMetricsRegistryManagerSample(
    const MetricsRegistryManagerSample& sample,
    const MetricsRegistryManagerPolicy& policy) {
  if (!policy.present || !policy.valid || !policy.scope_compatible) {
    return Finish(MetricsRegistryManagerDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_METRICS_REGISTRY_POLICY_INVALID",
                  "agents.metrics_registry.policy_invalid",
                  "policy missing invalid or outside scope",
                  true);
  }
  if (sample.metric_family.empty() || sample.namespace_path.empty()) {
    return Finish(MetricsRegistryManagerDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_METRICS_REGISTRY_SAMPLE_ID_REQUIRED",
                  "agents.metrics_registry.sample_id_required",
                  "metric family and namespace path are required",
                  true);
  }
  if (sample.cluster_metric_route_requested ||
      StartsWith(sample.namespace_path, "cluster.sys.metrics")) {
    return Finish(MetricsRegistryManagerDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_CLUSTER_PROVIDER_REQUIRED",
                  "agents.metrics_registry.cluster_metric_external_provider_required",
                  "cluster metric registry routes must be supplied by external cluster provider",
                  true);
  }
  if (sample.parser_authority || sample.sidecar_authority ||
      !sample.source_trusted || !sample.scope_compatible ||
      !sample.redaction_policy_valid) {
    return Finish(MetricsRegistryManagerDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_METRICS_REGISTRY_AUTHORITY_UNTRUSTED",
                  "agents.metrics_registry.untrusted_authority",
                  "metric samples require trusted source scope and redaction evidence",
                  true);
  }
  if ((!sample.schema_compatible ||
       sample.label_cardinality > policy.max_label_cardinality) &&
      policy.reject_bad_samples) {
    auto result = Finish(
        MetricsRegistryManagerDecisionKind::reject_metric_sample,
        OkStatus(),
        "SB_AGENT_METRICS_REGISTRY_SAMPLE_REJECTED",
        "agents.metrics_registry.sample_rejected",
        "sample schema or label cardinality violates registry policy",
        false);
    AddEvidence(&result, "metric_family", sample.metric_family);
    AddEvidence(&result, "label_cardinality",
                std::to_string(sample.label_cardinality));
    return result;
  }
  if (sample.export_queue_depth >= policy.export_queue_depth_threshold &&
      policy.export_shed_allowed) {
    auto result = Finish(MetricsRegistryManagerDecisionKind::shed_export,
                         OkStatus(),
                         "SB_AGENT_METRICS_REGISTRY_EXPORT_SHED",
                         "agents.metrics_registry.export_shed",
                         "export queue pressure exceeds policy threshold",
                         false);
    AddEvidence(&result, "export_queue_depth",
                std::to_string(sample.export_queue_depth));
    return result;
  }
  if (sample.rollup_backlog >= policy.rollup_backlog_threshold &&
      policy.rollup_allowed) {
    auto result = Finish(MetricsRegistryManagerDecisionKind::rollup_metrics,
                         OkStatus(),
                         "SB_AGENT_METRICS_REGISTRY_ROLLUP_REQUESTED",
                         "agents.metrics_registry.rollup_requested",
                         "rollup backlog exceeds policy threshold",
                         false);
    AddEvidence(&result, "rollup_backlog",
                std::to_string(sample.rollup_backlog));
    return result;
  }
  auto result = Finish(MetricsRegistryManagerDecisionKind::accept_sample,
                       OkStatus(),
                       "SB_AGENT_METRICS_REGISTRY_SAMPLE_ACCEPTED",
                       "agents.metrics_registry.sample_accepted",
                       "trusted metric sample accepted",
                       false);
  AddEvidence(&result, "metric_family", sample.metric_family);
  AddEvidence(&result, "sample_count", std::to_string(sample.sample_count));
  return result;
}

MetricsRegistryManagerResult ApplyMetricsRegistryManagerAction(
    const MetricsRegistryManagerActionRequest& request) {
  auto result = EvaluateMetricsRegistryManagerSample(request.sample,
                                                     request.policy);
  if (!result.ok()) {
    return result;
  }
  if (request.registry == nullptr) {
    return Refuse("SB_AGENT_METRICS_REGISTRY_REQUIRED",
                  "agents.metrics_registry.registry_required",
                  "metric registry mutation requires a registry handle");
  }
  const auto* descriptor_ptr =
      request.registry->FindDescriptorOrAlias(request.sample.metric_family);
  if (descriptor_ptr == nullptr) {
    return Refuse("SB_AGENT_METRICS_REGISTRY_DESCRIPTOR_UNKNOWN",
                  "agents.metrics_registry.descriptor_unknown",
                  request.sample.metric_family);
  }
  const auto descriptor = *descriptor_ptr;
  if (descriptor.cluster_only ||
      StartsWith(descriptor.namespace_path, "cluster.sys.metrics")) {
    return Refuse("SB_AGENT_CLUSTER_PROVIDER_REQUIRED",
                  "agents.metrics_registry.cluster_metric_external_provider_required",
                  "core metrics registry manager cannot mutate cluster metrics");
  }

  if (result.sample_rejected) {
    const auto rejected = request.registry->IncrementCounter(
        "sb_metric_samples_rejected_total",
        {{"metric_family", request.sample.metric_family},
         {"reason", result.diagnostic.diagnostic_code}},
        1.0,
        "metrics_registry_manager");
    if (!rejected.ok) {
      return FailMutation(rejected, "reject_sample_counter");
    }
    result.registry_mutation_written = true;
    AddEvidence(&result, "registry_mutation", "sample_rejection_counter");
    return result;
  }

  if (result.rollup_requested) {
    if (request.history_path.empty()) {
      return Refuse("SB_AGENT_METRICS_REGISTRY_HISTORY_REQUIRED",
                    "agents.metrics_registry.history_required",
                    "metric rollup requires persistent metric history path");
    }
    const u64 before = CountRollups(request.history_path);
    const auto generated =
        metrics::GenerateMetricRollups(request.history_path,
                                       request.rollup_grain);
    if (!generated.ok) {
      return FailMutation(generated, "generate_rollups");
    }
    const u64 after = CountRollups(request.history_path);
    result.rollup_written = true;
    result.rollup_rows_created = after > before ? after - before : 0;
    AddEvidence(&result, "rollup_grain",
                metrics::MetricRollupGrainName(request.rollup_grain));
    AddEvidence(&result, "rollup_rows_created",
                std::to_string(result.rollup_rows_created));
    return result;
  }

  if (result.export_shed_requested) {
    const u64 threshold = request.policy.export_queue_depth_threshold;
    const u64 excess = request.sample.export_queue_depth > threshold
                           ? request.sample.export_queue_depth - threshold
                           : 0;
    const u64 shed = request.export_shed_count == 0
                         ? excess
                         : std::min(request.export_shed_count, request.sample.export_queue_depth);
    const u64 remaining = request.sample.export_queue_depth > shed
                              ? request.sample.export_queue_depth - shed
                              : 0;
    const auto queue = request.registry->SetGauge(
        "sb_export_adapter_queue_depth",
        {{"component", "core.metrics.export"},
         {"operation", "shed_export"},
         {"metric_family", request.sample.metric_family}},
        static_cast<double>(remaining),
        "metrics_exporter");
    if (!queue.ok) {
      return FailMutation(queue, "export_queue_depth");
    }
    const auto shed_counter = request.registry->IncrementCounter(
        "sb_metric_export_shed_total",
        {{"metric_family", request.sample.metric_family},
         {"reason", "queue_pressure"}},
        static_cast<double>(shed == 0 ? 1 : shed),
        "metrics_registry_manager");
    if (!shed_counter.ok) {
      return FailMutation(shed_counter, "export_shed_counter");
    }
    result.registry_mutation_written = true;
    result.export_shed_written = true;
    result.export_queue_depth_after_shed = remaining;
    AddEvidence(&result, "export_shed_count", std::to_string(shed));
    AddEvidence(&result, "export_queue_depth_after_shed",
                std::to_string(remaining));
    return result;
  }

  const auto published = PublishSampleToRegistry(request.registry,
                                                descriptor,
                                                request.sample,
                                                request.labels);
  if (!published.ok) {
    return FailMutation(published, "publish_sample");
  }
  result.registry_mutation_written = true;
  AddEvidence(&result, "registry_mutation", "current_value_written");

  if (!request.history_path.empty()) {
    const auto history = metrics::AppendMetricRawSample(
        request.history_path,
        descriptor,
        ValueForSample(descriptor, request.sample, request.labels),
        request.observation_time_microseconds);
    if (!history.ok) {
      return FailMutation(history, "append_raw_sample");
    }
    result.history_sample_written = true;
    AddEvidence(&result, "history_sample", "raw_sample_written");
  } else if (request.durable_history_required) {
    return Refuse("SB_AGENT_METRICS_REGISTRY_HISTORY_REQUIRED",
                  "agents.metrics_registry.history_required",
                  "accepted production samples require persistent metric history");
  }
  return result;
}

const char* metrics_registry_manager_implementation_anchor() {
  return "metrics_registry_manager";
}

}  // namespace scratchbird::core::agents::implemented_agents
