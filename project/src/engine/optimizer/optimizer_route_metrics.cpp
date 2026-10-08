// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_route_metrics.hpp"
#include "metric_bound_definition.hpp"
#include "uuid.hpp"

#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

namespace metrics = scratchbird::core::metrics;

using metrics::MetricDescriptor;
using metrics::MetricLabelDescriptor;
using metrics::MetricReadiness;
using metrics::MetricType;
using metrics::MetricUnit;
using metrics::MetricValidationResult;

metrics::MetricDescriptorDefinition Descriptor(std::string family,
                            MetricType type,
                            MetricUnit unit,
                            std::string producer_owner,
                            std::string help) {
  metrics::MetricDescriptorDefinition descriptor;
  descriptor.family = std::move(family);
  descriptor.type = type;
  descriptor.unit = unit;
  descriptor.namespace_path = "sys.metrics.optimizer.enterprise";
  descriptor.help = std::move(help);
  descriptor.producer_owner = std::move(producer_owner);
  descriptor.security_family = "OPTIMIZER_METRICS";
  descriptor.value_type = type == MetricType::state ? metrics::MetricScalarType::enumeration : metrics::MetricScalarType::uint64;
  if (type == MetricType::state) descriptor.enum_values = {kOptimizerRouteObservationPresent};
  descriptor.labels = {MetricLabelDescriptor{"scope_uuid", true, false, metrics::MetricLabelType::system_uuid},
                       MetricLabelDescriptor{"route_label", true, false},
                       MetricLabelDescriptor{"plan_node_id", false, false},
                       MetricLabelDescriptor{"metric_family", true, false},
                       MetricLabelDescriptor{"result", false, false},
                       MetricLabelDescriptor{"source_generation", true, false},
                       MetricLabelDescriptor{"evidence_digest", true, true}};
  return descriptor;
}

bool UnsafeAuthority(const OptimizerRouteMetricAuthority& authority) {
  return authority.parser_or_reference_authority ||
         authority.client_finality_or_visibility_authority ||
         authority.metric_visibility_or_finality_authority ||
         authority.metric_recovery_authority ||
         authority.wal_or_redo_authority ||
         authority.cluster_authority ||
         authority.benchmark_authority;
}

void AddEvidence(OptimizerRouteMetricPublishResult* result,
                 std::string evidence) {
  if (result != nullptr) {
    result->evidence.push_back(std::move(evidence));
  }
}

OptimizerRouteMetricPublishResult Refuse(const OptimizerRouteMetricSample& sample,
                                         std::string code,
                                         std::string detail) {
  OptimizerRouteMetricPublishResult result;
  result.ok = false;
  result.scope_uuid = sample.scope_uuid;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  AddEvidence(&result, "OEIC_ROUTE_DRIVER_OPTIMIZER_METRICS");
  AddEvidence(&result, "optimizer.route_metrics.fail_closed=true");
  AddEvidence(&result, "optimizer.route_metrics.route_kind=" +
                           sample.route_kind);
  AddEvidence(&result, "optimizer.route_metrics.route_label=" +
                           sample.route_label);
  AddEvidence(&result, "optimizer.route_metrics.refused=" +
                           result.diagnostic_code);
  return result;
}

bool EmptyRequiredField(const OptimizerRouteMetricSample& sample,
                        std::string* field) {
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(sample.scope_uuid)) {
    if (field != nullptr) *field = "scope_uuid";
    return true;
  }
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(sample.database_uuid) ||
      !scratchbird::core::uuid::IsEngineIdentityUuid(sample.node_uuid)) {
    if (field != nullptr) *field = "observation_owner";
    return true;
  }
  if (sample.route_kind.empty()) {
    if (field != nullptr) *field = "route_kind";
    return true;
  }
  if (sample.route_label.empty()) {
    if (field != nullptr) *field = "route_label";
    return true;
  }
  if (sample.plan_hash.empty()) {
    if (field != nullptr) *field = "plan_hash";
    return true;
  }
  if (sample.result_hash.empty()) {
    if (field != nullptr) *field = "result_hash";
    return true;
  }
  if (sample.explain_digest.empty()) {
    if (field != nullptr) *field = "explain_digest";
    return true;
  }
  if (sample.result_contract_hash.empty()) {
    if (field != nullptr) *field = "result_contract_hash";
    return true;
  }
  if (sample.redaction_digest.empty()) {
    if (field != nullptr) *field = "redaction_digest";
    return true;
  }
  if (sample.diagnostic_code.empty()) {
    if (field != nullptr) *field = "diagnostic_code";
    return true;
  }
  if (sample.evidence_digest.empty()) {
    if (field != nullptr) *field = "evidence_digest";
    return true;
  }
  return false;
}

metrics::MetricLabelSet LabelsFor(const OptimizerRouteMetricSample& sample,
                                  std::string metric_family,
                                  std::string result_label = {}) {
  metrics::MetricLabelSet labels = {
      {"scope_uuid", sample.scope_uuid},
      {"route_label", sample.route_label},
      {"metric_family", std::move(metric_family)},
      {"source_generation", std::to_string(sample.source_generation)},
      {"evidence_digest", sample.evidence_digest}};
  if (!sample.plan_node_id.empty()) {
    labels.push_back({"plan_node_id", sample.plan_node_id});
  }
  if (!result_label.empty()) {
    labels.push_back({"result", std::move(result_label)});
  }
  return labels;
}

void Push(OptimizerRouteMetricPublishResult* result,
          MetricValidationResult metric_result) {
  if (!metric_result.ok && result != nullptr) {
    result->ok = false;
    if (result->diagnostic_code.empty()) {
      result->diagnostic_code = metric_result.diagnostic_code;
      result->detail = metric_result.detail;
    }
  }
  if (result != nullptr) {
    result->metric_results.push_back(std::move(metric_result));
  }
}

void State(OptimizerRouteMetricPublishResult* result,
           const OptimizerRouteMetricSample& sample,
           const std::string& family,
           const std::string& metric_family,
           std::string state_text,
           const std::string& producer_owner,
           std::string result_label = "ok") {
  Push(result,
       metrics::DefaultMetricRegistry().SetState(
           family, LabelsFor(sample, metric_family, std::move(result_label)),
           metrics::MetricEnumValue{kOptimizerRouteObservationPresent}, std::move(state_text), producer_owner));
}

void Gauge(OptimizerRouteMetricPublishResult* result,
           const OptimizerRouteMetricSample& sample,
           const std::string& family,
           const std::string& metric_family,
           std::uint64_t value,
           const std::string& producer_owner) {
  Push(result,
       metrics::DefaultMetricRegistry().SetGauge(
           family, LabelsFor(sample, metric_family), value, producer_owner));
}

}  // namespace

const std::vector<metrics::MetricDescriptorDefinition>&
OptimizerRouteMetricDescriptorDefinitions() {
  static const std::vector<metrics::MetricDescriptorDefinition> descriptors = {
      Descriptor("sb_optimizer_route_plan_hash", MetricType::state,
                 MetricUnit::none, "optimizer_explain",
                 "Driver-visible optimizer route plan hash."),
      Descriptor("sb_optimizer_route_result_hash", MetricType::state,
                 MetricUnit::none, "route_executor",
                 "Driver-visible optimizer route result hash."),
      Descriptor("sb_optimizer_explain_digest", MetricType::state,
                 MetricUnit::none, "optimizer_explain",
                 "Driver-visible optimizer explain digest."),
      Descriptor("sb_optimizer_route_equivalence_status", MetricType::state,
                 MetricUnit::none, "route_executor",
                 "Cross-route optimizer equivalence status."),
      Descriptor("sb_optimizer_driver_visible_route_count", MetricType::gauge,
                 MetricUnit::none, "route_executor",
                 "Driver-visible route count in optimizer evidence.")};
  return descriptors;
}

metrics::MetricValidationResult EnsureOptimizerRouteMetricDescriptors(
    metrics::MetricRegistry* registry) {
  for (const auto& descriptor : OptimizerRouteMetricDescriptorDefinitions()) {
    const auto result = metrics::ValidateBoundMetricDefinition(
        registry ? *registry : metrics::DefaultMetricRegistry(), descriptor);
    if (!result.ok) {
      return result;
    }
  }
  return metrics::MetricOk();
}

OptimizerRouteMetricPublishResult PublishOptimizerRouteMetrics(
    const OptimizerRouteMetricSample& sample) {
  std::string missing_field;
  if (EmptyRequiredField(sample, &missing_field)) {
    return Refuse(sample,
                  "SB_OPTIMIZER_ROUTE_METRICS.MISSING_SCOPE",
                  "optimizer.route_metrics.required_field_missing:" +
                      missing_field);
  }
  if (sample.source_generation == 0) {
    return Refuse(sample,
                  "SB_OPTIMIZER_ROUTE_METRICS.GENERATION_REQUIRED",
                  "optimizer.route_metrics.generation_required");
  }
  if (sample.freshness_microseconds > sample.max_freshness_microseconds) {
    return Refuse(sample,
                  "SB_OPTIMIZER_ROUTE_METRICS.STALE",
                  "optimizer.route_metrics.stale");
  }
  if (!sample.authority.route_executor_authoritative ||
      !sample.authority.optimizer_explain_authoritative ||
      !sample.authority.result_contract_authoritative ||
      !sample.authority.driver_surface_authoritative ||
      !sample.authority.route_equivalence_validated ||
      !sample.authority.engine_scope_bound ||
      !sample.authority.exact_diagnostics_preserved ||
      !sample.authority.redaction_applied) {
    return Refuse(sample,
                  "SB_OPTIMIZER_ROUTE_METRICS.ROUTE_AUTHORITY_REQUIRED",
                  "optimizer.route_metrics.route_authority_required");
  }
  if (UnsafeAuthority(sample.authority)) {
    return Refuse(sample,
                  "SB_OPTIMIZER_ROUTE_METRICS.UNSAFE_AUTHORITY",
                  "optimizer.route_metrics.unsafe_authority");
  }
  if (sample.driver_routes.empty() || sample.required_driver_routes.empty()) {
    return Refuse(sample,
                  "SB_OPTIMIZER_ROUTE_METRICS.DRIVER_ROUTES_REQUIRED",
                  "optimizer.route_metrics.driver_routes_required");
  }
  const auto route_validation = ValidateDriverVisibleExplainRouteEquivalence(
      sample.driver_routes, sample.required_driver_routes);
  if (!route_validation.ok) {
    return Refuse(sample,
                  "SB_OPTIMIZER_ROUTE_METRICS.ROUTE_EQUIVALENCE_FAILED",
                  route_validation.diagnostic_code);
  }

  // The validated routes must describe this sample, not a separate internally
  // consistent route set. Plan hash and plan-evidence digest are distinct data.
  bool selected_route = false;
  for (const auto& route : sample.driver_routes) {
    if (route.route_kind == sample.route_kind) selected_route = true;
    if (route.route_label != sample.route_label || route.result_hash != sample.result_hash ||
        route.explain_digest != sample.explain_digest || route.redaction_digest != sample.redaction_digest ||
        route.diagnostic_code != sample.diagnostic_code)
      return Refuse(sample, "SB_OPTIMIZER_ROUTE_METRICS.ROUTE_EQUIVALENCE_FAILED", "route sample differs from supplied route evidence");
  }
  if (!selected_route)
    return Refuse(sample, "SB_OPTIMIZER_ROUTE_METRICS.ROUTE_EQUIVALENCE_FAILED", "selected route is not in the supplied evidence");
  if (!metrics::DefaultMetricRegistry().ObservationOwnerMatches(sample.database_uuid, sample.node_uuid))
    return Refuse(sample, "METRIC.OBSERVATION_SOURCE_UNAVAILABLE", "route sample owner does not match the observation source");
  const auto descriptors = EnsureOptimizerRouteMetricDescriptors();
  if (!descriptors.ok) return Refuse(sample, descriptors.diagnostic_code, descriptors.detail);

  OptimizerRouteMetricPublishResult result;
  result.ok = true;
  result.scope_uuid = sample.scope_uuid;
  result.metric_results.reserve(5);
  result.diagnostic_code = "SB_OPTIMIZER_ROUTE_METRICS.OK";
  AddEvidence(&result, "OEIC_ROUTE_DRIVER_OPTIMIZER_METRICS");
  AddEvidence(&result, "optimizer.route_metrics.fail_closed=false");
  AddEvidence(&result, "optimizer.route_metrics.advisory_only=true");
  AddEvidence(&result, "optimizer.route_metrics.finality_authority=false");
  AddEvidence(&result, "optimizer.route_metrics.visibility_authority=false");
  AddEvidence(&result, "optimizer.route_metrics.security_authority=false");
  AddEvidence(&result, "optimizer.route_metrics.recovery_authority=false");
  AddEvidence(&result, "optimizer.route_metrics.wal_redo_authority=false");
  AddEvidence(&result, "optimizer.route_metrics.cluster_authority=false");

  State(&result, sample, "sb_optimizer_route_plan_hash", "route_plan_hash",
        sample.plan_hash, "optimizer_explain");
  State(&result, sample, "sb_optimizer_route_result_hash",
        "route_result_hash", sample.result_hash, "route_executor");
  State(&result, sample, "sb_optimizer_explain_digest", "explain_digest",
        sample.explain_digest, "optimizer_explain");
  State(&result, sample, "sb_optimizer_route_equivalence_status",
        "route_equivalence_status", route_validation.diagnostic_code,
        "route_executor");
  Gauge(&result, sample, "sb_optimizer_driver_visible_route_count",
        "driver_visible_route_count",
        sample.driver_routes.size(), "route_executor");

  if (!result.ok &&
      result.diagnostic_code == "SB_OPTIMIZER_ROUTE_METRICS.OK") {
    result.diagnostic_code =
        "SB_OPTIMIZER_ROUTE_METRICS.METRIC_PUBLISH_FAILED";
    result.detail = "optimizer.route_metrics.metric_publish_failed";
  }
  return result;
}

}  // namespace scratchbird::engine::optimizer
