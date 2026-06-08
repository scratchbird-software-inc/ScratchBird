// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "executor_operator_metrics.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

namespace metrics = scratchbird::core::metrics;

using metrics::MetricDescriptor;
using metrics::MetricLabelDescriptor;
using metrics::MetricReadiness;
using metrics::MetricType;
using metrics::MetricUnit;
using metrics::MetricValidationResult;

constexpr std::uint64_t kMaxMetricValue =
    std::numeric_limits<std::uint64_t>::max() / 4;

MetricDescriptor Descriptor(std::string family,
                            MetricType type,
                            MetricUnit unit,
                            std::string help) {
  MetricDescriptor descriptor;
  descriptor.family = std::move(family);
  descriptor.type = type;
  descriptor.unit = unit;
  descriptor.namespace_path = "sys.metrics.optimizer.enterprise";
  descriptor.help = std::move(help);
  descriptor.producer_owner = "executor_runtime";
  descriptor.security_family = "OPTIMIZER_METRICS";
  descriptor.readiness = MetricReadiness::implemented;
  descriptor.labels = {MetricLabelDescriptor{"scope_uuid", true, false},
                       MetricLabelDescriptor{"route_label", true, false},
                       MetricLabelDescriptor{"plan_node_id", true, false},
                       MetricLabelDescriptor{"metric_family", true, false},
                       MetricLabelDescriptor{"source_generation", true, false},
                       MetricLabelDescriptor{"evidence_digest", true, true}};
  if (type == MetricType::histogram) {
    descriptor.histogram_buckets = {1, 10, 100, 1000, 10000, 100000,
                                    1000000, 10000000};
  }
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

bool UnsafeAuthority(const ExecutorOperatorMetricAuthority& authority) {
  return authority.parser_or_donor_authority ||
         authority.client_supplied_finality ||
         authority.metric_visibility_or_finality_authority ||
         authority.metric_recovery_authority ||
         authority.benchmark_authority;
}

void AddEvidence(ExecutorOperatorMetricPublishResult* result,
                 std::string evidence) {
  if (result != nullptr) {
    result->evidence.push_back(std::move(evidence));
  }
}

ExecutorOperatorMetricPublishResult Refuse(const ExecutorOperatorActualsSample& sample,
                                           std::string code,
                                           std::string detail) {
  ExecutorOperatorMetricPublishResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  AddEvidence(&result, "OEIC_EXECUTOR_OPERATOR_ACTUALS_METRICS");
  AddEvidence(&result, "executor.operator_actuals.fail_closed=true");
  AddEvidence(&result, "executor.operator_actuals.operator_family=" +
                           sample.operator_family);
  AddEvidence(&result, "executor.operator_actuals.plan_node_id=" +
                           sample.plan_node_id);
  AddEvidence(&result, "executor.operator_actuals.refused=" +
                           result.diagnostic_code);
  return result;
}

bool EmptyRequiredField(const ExecutorOperatorActualsSample& sample,
                        std::string* field) {
  if (sample.scope_uuid.empty()) {
    if (field != nullptr) *field = "scope_uuid";
    return true;
  }
  if (sample.route_label.empty()) {
    if (field != nullptr) *field = "route_label";
    return true;
  }
  if (sample.plan_node_id.empty()) {
    if (field != nullptr) *field = "plan_node_id";
    return true;
  }
  if (sample.operator_family.empty()) {
    if (field != nullptr) *field = "operator_family";
    return true;
  }
  if (sample.plan_shape.empty()) {
    if (field != nullptr) *field = "plan_shape";
    return true;
  }
  if (sample.evidence_digest.empty()) {
    if (field != nullptr) *field = "evidence_digest";
    return true;
  }
  return false;
}

bool ValueOverflowRisk(const ExecutorOperatorActualsSample& sample) {
  const std::uint64_t values[] = {sample.estimated_rows,
                                  sample.actual_rows,
                                  sample.rows_examined,
                                  sample.rows_filtered,
                                  sample.loop_count,
                                  sample.estimated_pages,
                                  sample.actual_pages,
                                  sample.estimated_io_operations,
                                  sample.actual_io_operations,
                                  sample.estimated_visibility_recheck_rows,
                                  sample.actual_visibility_recheck_rows,
                                  sample.estimated_spill_bytes,
                                  sample.actual_spill_bytes,
                                  sample.spill_passes,
                                  sample.memory_grant_bytes,
                                  sample.peak_memory_bytes,
                                  sample.estimated_latency_microseconds,
                                  sample.actual_latency_microseconds,
                                  sample.cpu_time_microseconds,
                                  sample.estimated_resource_units,
                                  sample.actual_resource_units};
  return std::any_of(std::begin(values), std::end(values), [](std::uint64_t value) {
    return value > kMaxMetricValue;
  });
}

metrics::MetricLabelSet LabelsFor(const ExecutorOperatorActualsSample& sample,
                                  std::string metric_family) {
  return {{"scope_uuid", sample.scope_uuid},
          {"route_label", sample.route_label},
          {"plan_node_id", sample.plan_node_id},
          {"metric_family", std::move(metric_family)},
          {"source_generation", std::to_string(sample.source_generation)},
          {"evidence_digest", sample.evidence_digest}};
}

void Push(ExecutorOperatorMetricPublishResult* result,
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

void Gauge(ExecutorOperatorMetricPublishResult* result,
           const ExecutorOperatorActualsSample& sample,
           std::string family,
           std::string metric_family,
           std::uint64_t value) {
  Push(result,
       metrics::DefaultMetricRegistry().SetGauge(
           family,
           LabelsFor(sample, std::move(metric_family)),
           static_cast<double>(value),
           "executor_runtime"));
}

void Histogram(ExecutorOperatorMetricPublishResult* result,
               const ExecutorOperatorActualsSample& sample,
               std::string family,
               std::string metric_family,
               std::uint64_t value) {
  Push(result,
       metrics::DefaultMetricRegistry().ObserveHistogram(
           family,
           LabelsFor(sample, std::move(metric_family)),
           static_cast<double>(value),
           "executor_runtime"));
}

}  // namespace

metrics::MetricValidationResult EnsureExecutorOperatorActualsMetricDescriptors(
    metrics::MetricRegistry* registry) {
  const MetricDescriptor descriptors[] = {
      Descriptor("sb_optimizer_operator_actual_rows",
                 MetricType::gauge,
                 MetricUnit::count,
                 "Executor operator actual output rows."),
      Descriptor("sb_optimizer_operator_rows_examined",
                 MetricType::gauge,
                 MetricUnit::count,
                 "Executor operator rows examined."),
      Descriptor("sb_optimizer_operator_rows_filtered",
                 MetricType::gauge,
                 MetricUnit::count,
                 "Executor operator rows filtered."),
      Descriptor("sb_optimizer_operator_loop_count",
                 MetricType::gauge,
                 MetricUnit::count,
                 "Executor operator loop count."),
      Descriptor("sb_optimizer_operator_cpu_time",
                 MetricType::histogram,
                 MetricUnit::microseconds,
                 "Executor operator CPU time in microseconds."),
      Descriptor("sb_optimizer_spill_passes",
                 MetricType::gauge,
                 MetricUnit::count,
                 "Executor operator spill passes.")};
  for (const auto& descriptor : descriptors) {
    const auto result = RegisterIfMissing(registry, descriptor);
    if (!result.ok) {
      return result;
    }
  }
  return metrics::MetricOk();
}

ExecutorOperatorMetricPublishResult PublishExecutorOperatorActuals(
    const ExecutorOperatorActualsSample& sample) {
  std::string missing_field;
  if (EmptyRequiredField(sample, &missing_field)) {
    return Refuse(sample,
                  "SB_EXECUTOR_OPERATOR_ACTUALS.MISSING_SCOPE",
                  "executor.operator_actuals.required_field_missing:" +
                      missing_field);
  }
  if (sample.source_generation == 0) {
    return Refuse(sample,
                  "SB_EXECUTOR_OPERATOR_ACTUALS.SOURCE_GENERATION_REQUIRED",
                  "executor.operator_actuals.source_generation_required");
  }
  if (sample.loop_count == 0) {
    return Refuse(sample,
                  "SB_EXECUTOR_OPERATOR_ACTUALS.LOOP_COUNT_REQUIRED",
                  "executor.operator_actuals.loop_count_required");
  }
  if (sample.rows_filtered > sample.rows_examined) {
    return Refuse(sample,
                  "SB_EXECUTOR_OPERATOR_ACTUALS.ROWS_FILTERED_INVALID",
                  "executor.operator_actuals.rows_filtered_exceeds_examined");
  }
  if (ValueOverflowRisk(sample)) {
    return Refuse(sample,
                  "SB_EXECUTOR_OPERATOR_ACTUALS.VALUE_RANGE_INVALID",
                  "executor.operator_actuals.value_range_invalid");
  }
  if (sample.freshness_microseconds > sample.max_freshness_microseconds) {
    return Refuse(sample,
                  "SB_EXECUTOR_OPERATOR_ACTUALS.STALE",
                  "executor.operator_actuals.stale");
  }
  if (!sample.authority.engine_mga_snapshot_bound ||
      !sample.authority.transaction_inventory_authoritative ||
      !sample.authority.security_recheck_preserved) {
    return Refuse(sample,
                  "SB_EXECUTOR_OPERATOR_ACTUALS.MGA_SECURITY_EVIDENCE_REQUIRED",
                  "executor.operator_actuals.mga_security_evidence_required");
  }
  if (UnsafeAuthority(sample.authority)) {
    return Refuse(sample,
                  "SB_EXECUTOR_OPERATOR_ACTUALS.UNSAFE_AUTHORITY",
                  "executor.operator_actuals.unsafe_authority");
  }

  ExecutorOperatorMetricPublishResult result;
  result.ok = true;
  result.diagnostic_code = "SB_EXECUTOR_OPERATOR_ACTUALS.OK";
  AddEvidence(&result, "OEIC_EXECUTOR_OPERATOR_ACTUALS_METRICS");
  AddEvidence(&result, "executor.operator_actuals.fail_closed=false");
  AddEvidence(&result, "executor.operator_actuals.advisory_only=true");
  AddEvidence(&result, "executor.operator_actuals.mga_finality_authority=engine_transaction_inventory");
  AddEvidence(&result, "executor.operator_actuals.visibility_authority=false");
  AddEvidence(&result, "executor.operator_actuals.security_authority=false");
  AddEvidence(&result, "executor.operator_actuals.recovery_authority=false");
  AddEvidence(&result, "executor.operator_actuals.benchmark_authority=false");

  Push(&result, EnsureExecutorOperatorActualsMetricDescriptors());
  Gauge(&result, sample, "sb_optimizer_operator_actual_rows",
        "operator_actual_rows", sample.actual_rows);
  Gauge(&result, sample, "sb_optimizer_operator_rows_examined",
        "operator_rows_examined", sample.rows_examined);
  Gauge(&result, sample, "sb_optimizer_operator_rows_filtered",
        "operator_rows_filtered", sample.rows_filtered);
  Gauge(&result, sample, "sb_optimizer_operator_loop_count",
        "operator_loop_count", sample.loop_count);
  Histogram(&result, sample, "sb_optimizer_operator_cpu_time",
            "operator_cpu_time", sample.cpu_time_microseconds);
  Gauge(&result, sample, "sb_optimizer_spill_passes", "spill_passes",
        sample.spill_passes);

  metrics::OptimizerRuntimeFeedbackMetricSample feedback;
  feedback.estimated_rows = static_cast<double>(sample.estimated_rows);
  feedback.actual_rows = static_cast<double>(sample.actual_rows);
  feedback.estimated_pages = static_cast<double>(sample.estimated_pages);
  feedback.actual_pages = static_cast<double>(sample.actual_pages);
  feedback.estimated_io_operations =
      static_cast<double>(sample.estimated_io_operations);
  feedback.actual_io_operations = static_cast<double>(sample.actual_io_operations);
  feedback.estimated_visibility_recheck_rows =
      static_cast<double>(sample.estimated_visibility_recheck_rows);
  feedback.actual_visibility_recheck_rows =
      static_cast<double>(sample.actual_visibility_recheck_rows);
  feedback.estimated_spill_bytes =
      static_cast<double>(sample.estimated_spill_bytes);
  feedback.actual_spill_bytes = static_cast<double>(sample.actual_spill_bytes);
  feedback.memory_grant_bytes = static_cast<double>(sample.memory_grant_bytes);
  feedback.peak_memory_bytes = static_cast<double>(sample.peak_memory_bytes);
  feedback.recommended_memory_grant_bytes =
      static_cast<double>(std::max(sample.memory_grant_bytes,
                                   sample.peak_memory_bytes));
  feedback.estimated_latency_microseconds =
      static_cast<double>(sample.estimated_latency_microseconds);
  feedback.actual_latency_microseconds =
      static_cast<double>(sample.actual_latency_microseconds);
  feedback.estimated_resource_units =
      static_cast<double>(sample.estimated_resource_units);
  feedback.actual_resource_units =
      static_cast<double>(sample.actual_resource_units);
  Push(&result, metrics::PublishOptimizerRuntimeFeedbackSample(
                    feedback, sample.operator_family, sample.plan_shape));

  if (!result.ok && result.diagnostic_code == "SB_EXECUTOR_OPERATOR_ACTUALS.OK") {
    result.diagnostic_code =
        "SB_EXECUTOR_OPERATOR_ACTUALS.METRIC_PUBLISH_FAILED";
    result.detail = "executor.operator_actuals.metric_publish_failed";
  }
  return result;
}

}  // namespace scratchbird::engine::executor
