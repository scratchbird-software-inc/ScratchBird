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
#include "metric_label_key.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <utility>
#include <stdexcept>

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
    const std::string& operation, MetricsRegistryManagerResult result) {
  auto failure = Refuse("SB_AGENT_METRICS_REGISTRY_MUTATION_FAILED",
                "agents.metrics_registry.mutation_failed",
                operation + ":" + validation.diagnostic_code + ":" +
                    validation.detail);
  result.status = failure.status;
  result.diagnostic = std::move(failure.diagnostic);
  result.fail_closed = true;
  for (auto& field : result.evidence)
    if (field.key == "failed_closed") field.value = "true";
  AddEvidence(&result, "failure_operation", operation);
  return result;
}

metrics::MetricValidationResult PublishSampleToRegistry(
    metrics::MetricRegistry* registry,
    const metrics::MetricDescriptor& descriptor,
    const MetricsRegistryManagerSample& sample,
    metrics::MetricLabelSet labels,
    metrics::MetricValue* published_value) {
  switch (descriptor.type) {
    case metrics::MetricType::counter:
      return registry->IncrementCounter(descriptor.family,
                                        std::move(labels),
                                        sample.sample_count,
                                        descriptor.producer_owner, published_value);
    case metrics::MetricType::gauge:
      return registry->SetGauge(descriptor.family,
                                std::move(labels),
                                sample.numeric_value,
                                descriptor.producer_owner, published_value);
    case metrics::MetricType::histogram:
      return registry->ObserveHistogram(descriptor.family,
                                        std::move(labels),
                                        sample.numeric_value,
                                        descriptor.producer_owner, published_value);
    case metrics::MetricType::state:
      return registry->SetState(descriptor.family,
                                std::move(labels),
                                sample.numeric_value,
                                sample.state_text,
                                descriptor.producer_owner, published_value);
    case metrics::MetricType::derived:
      return metrics::MetricError("SB-METRICS-DERIVED-SAMPLE-READONLY",
                                  descriptor.family);
  }
  return metrics::MetricError("SB-METRICS-TYPE-UNKNOWN", descriptor.family);
}

}  // namespace

MetricExportBuffer::MetricExportBuffer(Uuid database, Uuid node, std::size_t maximum_samples,
                                       std::size_t maximum_bytes)
    : database_(database), node_(node), maximum_samples_(maximum_samples), maximum_bytes_(maximum_bytes) {
  if (!core::uuid::IsEngineIdentityUuid(database) || !core::uuid::IsEngineIdentityUuid(node) ||
      !maximum_samples || maximum_samples > 65536 || !maximum_bytes || maximum_bytes > 64 * 1024 * 1024)
    throw std::invalid_argument("export buffer owner or bounds invalid");
}
bool MetricExportBuffer::OwnerMatches(Uuid database, Uuid node) const noexcept {
  return database_ == database && node_ == node;
}
bool MetricExportBuffer::Enqueue(const metrics::MetricDescriptor& descriptor,
                                 const metrics::MetricSeriesIdentity& series,
                                 const metrics::MetricRawSampleRecord& sample) {
  if (!OwnerMatches(sample.database_uuid, sample.node_uuid) || !sample.cluster_uuid.is_nil()) return false;
  auto encoded = metrics::EncodeMetricRawSample(descriptor, series, sample);
  if (!encoded.ok() || encoded.bytes.size() > maximum_bytes_) return false;
  std::lock_guard lock(mutex_);
  if (entries_.size() >= maximum_samples_ || encoded.bytes.size() > maximum_bytes_ - bytes_ ||
      identities_.contains(sample.sample_uuid)) return false;
  const auto size = encoded.bytes.size();
  entries_.push_back({sample.sample_uuid, std::move(encoded.bytes)});
  try { identities_.insert(sample.sample_uuid); }
  catch (...) { entries_.pop_back(); throw; }
  bytes_ += size;
  return true;
}
std::optional<MetricExportBuffer::Entry> MetricExportBuffer::Take() {
  std::lock_guard lock(mutex_);
  if (entries_.empty()) return std::nullopt;
  auto entry = std::move(entries_.front());
  entries_.pop_front();
  identities_.erase(entry.sample_uuid);
  bytes_ -= entry.bytes.size();
  return entry;
}
MetricExportBuffer::ShedResult MetricExportBuffer::ShedTo(u64 threshold, u64 maximum_to_shed) {
  std::lock_guard lock(mutex_);
  ShedResult result;
  const auto excess = entries_.size() > threshold ? entries_.size() - threshold : 0;
  const auto count = maximum_to_shed ? std::min<u64>(excess, maximum_to_shed) : excess;
  result.removed.reserve(count);
  // All allocating receipt work precedes removal. Container erasure cannot
  // lose the identities of effects already performed.
  for (u64 index = 0; index < count; ++index) result.removed.push_back(entries_[index].sample_uuid);
  for (const auto& id : result.removed) {
    bytes_ -= entries_.front().bytes.size();
    entries_.pop_front();
    identities_.erase(id);
  }
  result.remaining = entries_.size();
  result.ok = true;
  return result;
}
u64 MetricExportBuffer::Depth() const { std::lock_guard lock(mutex_); return entries_.size(); }
u64 MetricExportBuffer::Bytes() const { std::lock_guard lock(mutex_); return bytes_; }

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
  auto observed = request.sample;
  if (request.export_buffer) observed.export_queue_depth = request.export_buffer->Depth();
  auto result = EvaluateMetricsRegistryManagerSample(observed,
                                                     request.policy);
  if (!result.ok()) {
    return result;
  }
  if (request.registry == nullptr) {
    return Refuse("SB_AGENT_METRICS_REGISTRY_REQUIRED",
                  "agents.metrics_registry.registry_required",
                  "metric registry mutation requires a registry handle");
  }
  if (!request.registry->ObservationOwnerMatches(request.database_uuid, request.node_uuid) ||
      (request.export_buffer && !request.export_buffer->OwnerMatches(request.database_uuid, request.node_uuid)))
    return Refuse("SB_AGENT_METRICS_REGISTRY_OWNER_MISMATCH",
                  "agents.metrics_registry.owner_mismatch", "native node owner binding required");
  const auto* descriptor_ptr =
      request.registry->FindDescriptorOrAlias(request.sample.metric_family);
  if (descriptor_ptr == nullptr) {
    return Refuse("SB_AGENT_METRICS_REGISTRY_DESCRIPTOR_UNKNOWN",
                  "agents.metrics_registry.descriptor_unknown",
                  request.sample.metric_family);
  }
  const auto descriptor = *descriptor_ptr;
  if (descriptor.namespace_path != request.sample.namespace_path)
    return Refuse("SB_AGENT_METRICS_REGISTRY_NAMESPACE_MISMATCH",
                  "agents.metrics_registry.namespace_mismatch", descriptor.family);
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
        u64{1},
        "metrics_registry_manager");
    if (!rejected.ok) {
      return FailMutation(rejected, "reject_sample_counter", std::move(result));
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
    const auto before = metrics::LoadMetricHistoryStore(request.history_path);
    if (!before.load_status.ok) return FailMutation(before.load_status, "load_rollups", std::move(result));
    for (const auto& series : before.series)
      if (series.database_uuid != request.database_uuid || series.node_uuid != request.node_uuid ||
          !series.cluster_uuid.is_nil())
        return Refuse("SB_AGENT_METRICS_REGISTRY_OWNER_MISMATCH",
                      "agents.metrics_registry.owner_mismatch", "foreign history series");
    const auto generated =
        metrics::GenerateMetricRollups(request.history_path,
                                       request.rollup_grain, request.actor_uuid, request.transaction_uuid);
    if (!generated.ok) {
      return FailMutation(generated, "generate_rollups", std::move(result));
    }
    result.rollup_written = true;
    const auto after = metrics::LoadMetricHistoryStore(request.history_path);
    if (!after.load_status.ok) return FailMutation(after.load_status, "reopen_rollups", std::move(result));
    result.rollup_rows_created = after.rollups.size() > before.rollups.size()
        ? after.rollups.size() - before.rollups.size() : 0;
    AddEvidence(&result, "rollup_grain",
                metrics::MetricRollupGrainName(request.rollup_grain));
    AddEvidence(&result, "rollup_rows_created",
                std::to_string(result.rollup_rows_created));
    return result;
  }

  if (result.export_shed_requested) {
    if (!request.export_buffer)
      return Refuse("SB_AGENT_METRICS_REGISTRY_EXPORT_BUFFER_REQUIRED",
                    "agents.metrics_registry.export_buffer_required", "actual optional export copies required");
    auto removed = request.export_buffer->ShedTo(request.policy.export_queue_depth_threshold,
                                               request.export_shed_count);
    result.export_samples_removed = std::move(removed.removed);
    const auto shed = static_cast<u64>(result.export_samples_removed.size());
    const auto remaining = removed.remaining;
    result.export_shed_written = removed.ok;
    result.export_queue_depth_after_shed = remaining;
    const auto queue = request.registry->SetGauge(
        "sb_export_adapter_queue_depth",
        {{"component", "core.metrics.export"},
         {"operation", "shed_export"},
         {"metric_family", request.sample.metric_family}},
        remaining,
        "metrics_exporter");
    if (!queue.ok) {
      return FailMutation(queue, "export_queue_depth", std::move(result));
    }
    result.registry_mutation_written = true;
    const auto shed_counter = request.registry->IncrementCounter(
        "sb_metric_export_shed_total",
        {{"metric_family", request.sample.metric_family},
         {"reason", "queue_pressure"}},
        shed,
        "metrics_registry_manager");
    if (!shed_counter.ok) {
      return FailMutation(shed_counter, "export_shed_counter", std::move(result));
    }
    result.registry_mutation_written = true;
    result.export_shed_written = true;
    result.export_queue_depth_after_shed = remaining;
    AddEvidence(&result, "export_shed_count", std::to_string(shed));
    AddEvidence(&result, "export_queue_depth_after_shed",
                std::to_string(remaining));
    return result;
  }

  if (request.history_path.empty() && request.durable_history_required)
    return Refuse("SB_AGENT_METRICS_REGISTRY_HISTORY_REQUIRED",
                  "agents.metrics_registry.history_required", "accepted production samples require persistent metric history");
  std::optional<metrics::MetricSeriesIdentity> history_series;
  if (!request.history_path.empty()) {
    const auto store = metrics::LoadMetricHistoryStore(request.history_path);
    if (!store.load_status.ok) return FailMutation(store.load_status, "history_preflight", std::move(result));
    const auto key = metrics::MakeMetricSeriesKey(descriptor.family, request.labels);
    const auto selected = std::find_if(store.series.begin(), store.series.end(), [&](const auto& series) {
      return metrics::MakeMetricSeriesKey(series.metric_family, series.labels) == key &&
          static_cast<const metrics::MetricDescriptorBinding&>(series) ==
          static_cast<const metrics::MetricDescriptorBinding&>(descriptor);
    });
    if (selected == store.series.end() || selected->database_uuid != request.database_uuid ||
        selected->node_uuid != request.node_uuid || !selected->cluster_uuid.is_nil())
      return Refuse("SB_AGENT_METRICS_REGISTRY_HISTORY_BINDING_REQUIRED",
                    "agents.metrics_registry.history_binding_required", descriptor.family);
    const auto policy = std::find_if(store.policies.begin(), store.policies.end(), [&](const auto& candidate) {
      return candidate.policy_uuid == descriptor.retention_policy_uuid &&
          candidate.generation == descriptor.retention_policy_generation;
    });
    if (policy == store.policies.end() || policy->mode != metrics::MetricRetentionMode::raw_and_rollup)
      return Refuse("SB_AGENT_METRICS_REGISTRY_RAW_HISTORY_POLICY_REQUIRED",
                    "agents.metrics_registry.raw_history_policy_required", descriptor.family);
    history_series = *selected;
  }
  metrics::MetricValue published_value;
  const auto published = PublishSampleToRegistry(request.registry,
                                                descriptor,
                                                request.sample,
                                                request.labels,
                                                history_series ? &published_value : nullptr);
  if (!published.ok) {
    return FailMutation(published, "publish_sample", std::move(result));
  }
  result.registry_mutation_written = true;
  AddEvidence(&result, "registry_mutation", "current_value_written");

  if (!request.history_path.empty()) {
    // Exact successor from this call, not a later concurrent snapshot or a
    // fabricated delta/histogram. No full-registry scan or floating conversion.
    bool written = false;
    const auto history = metrics::AppendMetricRawSample(
        request.history_path,
        descriptor,
        published_value,
        request.observation_time_microseconds, &written, &*history_series);
    if (!history.ok) {
      return FailMutation(history, "append_raw_sample", std::move(result));
    }
    if (!written)
      return FailMutation(metrics::MetricError("SB_AGENT_METRICS_REGISTRY_RAW_HISTORY_POLICY_REQUIRED",
                                               descriptor.family), "append_raw_sample_no_effect", std::move(result));
    result.history_sample_written = true;
    AddEvidence(&result, "history_sample", "raw_sample_written");
  }
  return result;
}

const char* metrics_registry_manager_implementation_anchor() {
  return "metrics_registry_manager";
}

}  // namespace scratchbird::core::agents::implemented_agents
