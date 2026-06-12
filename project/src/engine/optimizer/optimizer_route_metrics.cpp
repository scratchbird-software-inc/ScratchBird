// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_route_metrics.hpp"

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

MetricDescriptor Descriptor(std::string family,
                            MetricType type,
                            MetricUnit unit,
                            std::string producer_owner,
                            std::string help) {
  MetricDescriptor descriptor;
  descriptor.family = std::move(family);
  descriptor.type = type;
  descriptor.unit = unit;
  descriptor.namespace_path = "sys.metrics.optimizer.enterprise";
  descriptor.help = std::move(help);
  descriptor.producer_owner = std::move(producer_owner);
  descriptor.security_family = "OPTIMIZER_METRICS";
  descriptor.readiness = MetricReadiness::implemented;
  descriptor.labels = {MetricLabelDescriptor{"scope_uuid", true, false},
                       MetricLabelDescriptor{"route_label", true, false},
                       MetricLabelDescriptor{"plan_node_id", false, false},
                       MetricLabelDescriptor{"metric_family", true, false},
                       MetricLabelDescriptor{"result", false, false},
                       MetricLabelDescriptor{"source_generation", true, false},
                       MetricLabelDescriptor{"evidence_digest", true, true}};
  return descriptor;
}

MetricValidationResult RegisterIfMissing(metrics::MetricRegistry* registry,
                                         MetricDescriptor descriptor) {
  auto& target = registry == nullptr ? metrics::DefaultMetricRegistry() : *registry;
  if (target.FindDescriptor(descriptor.family) != nullptr) {
    return metrics::MetricOk();
  }
  return target.RegisterDescriptor(std::move(descriptor));
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
  if (sample.scope_uuid.empty()) {
    if (field != nullptr) *field = "scope_uuid";
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
           1.0, std::move(state_text), producer_owner));
}

void Gauge(OptimizerRouteMetricPublishResult* result,
           const OptimizerRouteMetricSample& sample,
           const std::string& family,
           const std::string& metric_family,
           double value,
           const std::string& producer_owner) {
  Push(result,
       metrics::DefaultMetricRegistry().SetGauge(
           family, LabelsFor(sample, metric_family), value, producer_owner));
}

}  // namespace

metrics::MetricValidationResult EnsureOptimizerRouteMetricDescriptors(
    metrics::MetricRegistry* registry) {
  const MetricDescriptor descriptors[] = {
      Descriptor("sb_optimizer_route_plan_hash", MetricType::state,
                 MetricUnit::state, "optimizer_explain",
                 "Driver-visible optimizer route plan hash."),
      Descriptor("sb_optimizer_route_result_hash", MetricType::state,
                 MetricUnit::state, "route_executor",
                 "Driver-visible optimizer route result hash."),
      Descriptor("sb_optimizer_explain_digest", MetricType::state,
                 MetricUnit::state, "optimizer_explain",
                 "Driver-visible optimizer explain digest."),
      Descriptor("sb_optimizer_route_equivalence_status", MetricType::state,
                 MetricUnit::state, "route_executor",
                 "Cross-route optimizer equivalence status."),
      Descriptor("sb_optimizer_driver_visible_route_count", MetricType::gauge,
                 MetricUnit::count, "route_executor",
                 "Driver-visible route count in optimizer evidence.")};
  for (const auto& descriptor : descriptors) {
    const auto result = RegisterIfMissing(registry, descriptor);
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

  OptimizerRouteMetricPublishResult result;
  result.ok = true;
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

  Push(&result, EnsureOptimizerRouteMetricDescriptors());
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
        static_cast<double>(sample.driver_routes.size()), "route_executor");

  if (!result.ok &&
      result.diagnostic_code == "SB_OPTIMIZER_ROUTE_METRICS.OK") {
    result.diagnostic_code =
        "SB_OPTIMIZER_ROUTE_METRICS.METRIC_PUBLISH_FAILED";
    result.detail = "optimizer.route_metrics.metric_publish_failed";
  }
  return result;
}

}  // namespace scratchbird::engine::optimizer
