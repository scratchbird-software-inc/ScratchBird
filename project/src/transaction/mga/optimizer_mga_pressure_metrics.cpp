// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_mga_pressure_metrics.hpp"

#include "metric_producer.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace scratchbird::transaction::mga {
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

bool UnsafeAuthority(const OptimizerMgaPressureAuthority& authority) {
  return authority.parser_or_donor_authority ||
         authority.client_finality_or_visibility_authority ||
         authority.metric_visibility_or_finality_authority ||
         authority.metric_recovery_authority ||
         authority.external_log_replay_authority ||
         authority.benchmark_authority;
}

void AddEvidence(OptimizerMgaPressurePublishResult* result,
                 std::string evidence) {
  if (result != nullptr) {
    result->evidence.push_back(std::move(evidence));
  }
}

OptimizerMgaPressurePublishResult Refuse(const OptimizerMgaPressureSample& sample,
                                         std::string code,
                                         std::string detail) {
  OptimizerMgaPressurePublishResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  AddEvidence(&result, "OEIC_MGA_PRESSURE_OPTIMIZER_METRICS");
  AddEvidence(&result, "optimizer.mga_pressure.fail_closed=true");
  AddEvidence(&result, "optimizer.mga_pressure.relation_uuid=" +
                           sample.relation_uuid);
  AddEvidence(&result, "optimizer.mga_pressure.refused=" +
                           result.diagnostic_code);
  return result;
}

bool EmptyRequiredField(const OptimizerMgaPressureSample& sample,
                        std::string* field) {
  if (sample.scope_uuid.empty()) {
    if (field != nullptr) *field = "scope_uuid";
    return true;
  }
  if (sample.route_label.empty()) {
    if (field != nullptr) *field = "route_label";
    return true;
  }
  if (sample.relation_uuid.empty()) {
    if (field != nullptr) *field = "relation_uuid";
    return true;
  }
  if (sample.page_class.empty()) {
    if (field != nullptr) *field = "page_class";
    return true;
  }
  if (sample.evidence_digest.empty()) {
    if (field != nullptr) *field = "evidence_digest";
    return true;
  }
  return false;
}

metrics::MetricLabelSet LabelsFor(const OptimizerMgaPressureSample& sample,
                                  std::string metric_family) {
  return {{"scope_uuid", sample.scope_uuid},
          {"route_label", sample.route_label},
          {"metric_family", std::move(metric_family)},
          {"source_generation", std::to_string(sample.source_generation)},
          {"evidence_digest", sample.evidence_digest}};
}

void Push(OptimizerMgaPressurePublishResult* result,
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

void Gauge(OptimizerMgaPressurePublishResult* result,
           const OptimizerMgaPressureSample& sample,
           const std::string& family,
           const std::string& metric_family,
           double value,
           const std::string& producer_owner) {
  Push(result, metrics::DefaultMetricRegistry().SetGauge(
                   family, LabelsFor(sample, metric_family), value,
                   producer_owner));
}

}  // namespace

metrics::MetricValidationResult EnsureOptimizerMgaPressureMetricDescriptors(
    metrics::MetricRegistry* registry) {
  const MetricDescriptor descriptors[] = {
      Descriptor("sb_optimizer_mga_cleanup_debt", MetricType::gauge,
                 MetricUnit::bytes, "transaction_mga_cleanup",
                 "MGA cleanup debt bytes visible to optimizer costing."),
      Descriptor("sb_optimizer_mga_retained_dead_bytes", MetricType::gauge,
                 MetricUnit::bytes, "transaction_mga_cleanup",
                 "MGA retained dead bytes visible to optimizer costing."),
      Descriptor("sb_optimizer_mga_chain_depth", MetricType::gauge,
                 MetricUnit::count, "row_version_runtime",
                 "MGA row-version chain depth bucket."),
      Descriptor("sb_optimizer_mga_chain_scatter", MetricType::gauge,
                 MetricUnit::count, "row_version_runtime",
                 "MGA row-version chain scatter bucket."),
      Descriptor("sb_optimizer_same_page_update_ratio", MetricType::gauge,
                 MetricUnit::ratio, "row_version_runtime",
                 "MGA same-page update ratio."),
      Descriptor("sb_optimizer_commit_fence_pressure", MetricType::gauge,
                 MetricUnit::count, "transaction_manager",
                 "Commit fence pressure visible to optimizer costing.")};
  for (const auto& descriptor : descriptors) {
    const auto result = RegisterIfMissing(registry, descriptor);
    if (!result.ok) {
      return result;
    }
  }
  return metrics::MetricOk();
}

OptimizerMgaPressurePublishResult PublishOptimizerMgaPressureMetrics(
    const OptimizerMgaPressureSample& sample) {
  std::string missing_field;
  if (EmptyRequiredField(sample, &missing_field)) {
    return Refuse(sample,
                  "SB_OPTIMIZER_MGA_PRESSURE.MISSING_SCOPE",
                  "optimizer.mga_pressure.required_field_missing:" +
                      missing_field);
  }
  if (sample.source_generation == 0 ||
      sample.authoritative_cleanup_horizon_local_transaction_id == 0) {
    return Refuse(sample,
                  "SB_OPTIMIZER_MGA_PRESSURE.GENERATION_REQUIRED",
                  "optimizer.mga_pressure.generation_required");
  }
  if (sample.freshness_microseconds > sample.max_freshness_microseconds) {
    return Refuse(sample,
                  "SB_OPTIMIZER_MGA_PRESSURE.STALE",
                  "optimizer.mga_pressure.stale");
  }
  if (!sample.authority.transaction_inventory_authoritative ||
      !sample.authority.cleanup_horizon_authoritative ||
      !sample.authority.row_version_runtime_authoritative ||
      !sample.authority.engine_scope_bound) {
    return Refuse(sample,
                  "SB_OPTIMIZER_MGA_PRESSURE.MGA_AUTHORITY_REQUIRED",
                  "optimizer.mga_pressure.mga_authority_required");
  }
  if (UnsafeAuthority(sample.authority)) {
    return Refuse(sample,
                  "SB_OPTIMIZER_MGA_PRESSURE.UNSAFE_AUTHORITY",
                  "optimizer.mga_pressure.unsafe_authority");
  }
  if (sample.same_page_update_ratio < 0.0 ||
      sample.same_page_update_ratio > 1.0) {
    return Refuse(sample,
                  "SB_OPTIMIZER_MGA_PRESSURE.RATIO_INVALID",
                  "optimizer.mga_pressure.same_page_ratio_invalid");
  }

  OptimizerMgaPressurePublishResult result;
  result.ok = true;
  result.diagnostic_code = "SB_OPTIMIZER_MGA_PRESSURE.OK";
  AddEvidence(&result, "OEIC_MGA_PRESSURE_OPTIMIZER_METRICS");
  AddEvidence(&result, "optimizer.mga_pressure.fail_closed=false");
  AddEvidence(&result, "optimizer.mga_pressure.advisory_only=true");
  AddEvidence(&result, "optimizer.mga_pressure.finality_authority=false");
  AddEvidence(&result, "optimizer.mga_pressure.visibility_authority=false");
  AddEvidence(&result, "optimizer.mga_pressure.security_authority=false");
  AddEvidence(&result, "optimizer.mga_pressure.recovery_authority=false");
  AddEvidence(&result, "optimizer.mga_pressure.external_log_replay_authority=false");

  Push(&result, EnsureOptimizerMgaPressureMetricDescriptors());
  Gauge(&result, sample, "sb_optimizer_mga_cleanup_debt",
        "mga_cleanup_debt", static_cast<double>(sample.cleanup_debt_bytes),
        "transaction_mga_cleanup");
  Gauge(&result, sample, "sb_optimizer_mga_retained_dead_bytes",
        "mga_retained_dead_bytes",
        static_cast<double>(sample.retained_dead_bytes),
        "transaction_mga_cleanup");
  Gauge(&result, sample, "sb_optimizer_mga_chain_depth", "mga_chain_depth",
        static_cast<double>(sample.chain_depth_bucket), "row_version_runtime");
  Gauge(&result, sample, "sb_optimizer_mga_chain_scatter", "mga_chain_scatter",
        static_cast<double>(sample.chain_scatter_bucket), "row_version_runtime");
  Gauge(&result, sample, "sb_optimizer_same_page_update_ratio",
        "same_page_update_ratio", sample.same_page_update_ratio,
        "row_version_runtime");
  Gauge(&result, sample, "sb_optimizer_commit_fence_pressure",
        "commit_fence_pressure",
        static_cast<double>(sample.commit_fence_backlog),
        "transaction_manager");

  Push(&result, metrics::SetGauge(
                    "sb_mga_cleanup_horizon_local_transaction_id",
                    metrics::Labels({{"component", "transaction.mga.cleanup"},
                                     {"authority", "local_inventory"}}),
                    static_cast<double>(
                        sample.authoritative_cleanup_horizon_local_transaction_id),
                    "transaction_mga_cleanup"));
  Push(&result, metrics::SetGauge(
                    "sb_mga_cleanup_retained_row_versions",
                    metrics::Labels({{"component", "transaction.mga.cleanup"},
                                     {"authority", "local_inventory"}}),
                    static_cast<double>(sample.chain_depth_bucket),
                    "transaction_mga_cleanup"));

  if (!result.ok &&
      result.diagnostic_code == "SB_OPTIMIZER_MGA_PRESSURE.OK") {
    result.diagnostic_code =
        "SB_OPTIMIZER_MGA_PRESSURE.METRIC_PUBLISH_FAILED";
    result.detail = "optimizer.mga_pressure.metric_publish_failed";
  }
  return result;
}

}  // namespace scratchbird::transaction::mga
