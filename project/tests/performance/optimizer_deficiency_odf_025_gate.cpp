// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_cost_full.hpp"
#include "optimizer_feedback.hpp"
#include "metric_contracts.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace metrics = scratchbird::core::metrics;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

opt::OptimizerRuntimeFeedback BaseFeedback() {
  opt::OptimizerRuntimeFeedback feedback;
  feedback.operator_family = "table_scan";
  feedback.plan_shape = "rel.customer.scan";
  feedback.cost_profile_id = "odf025.local";
  feedback.estimated_rows = 100;
  feedback.actual_rows = 105;
  feedback.estimated_pages = 16;
  feedback.actual_pages = 16;
  feedback.estimated_io_operations = 32;
  feedback.actual_io_operations = 34;
  feedback.estimated_visibility_recheck_rows = 8;
  feedback.actual_visibility_recheck_rows = 8;
  feedback.memory_grant_bytes = 1024 * 1024;
  feedback.peak_memory_bytes = 512 * 1024;
  feedback.estimated_latency_microseconds = 1000;
  feedback.actual_latency_microseconds = 1000;
  feedback.estimated_resource_units = 100;
  feedback.actual_resource_units = 100;
  return feedback;
}

opt::CostVector BaseCost() {
  opt::CostVector cost;
  cost.startup_cost = 100;
  cost.row_cost = 1000;
  cost.io_cost = 2000;
  cost.memory_cost = 400;
  cost.uncertainty_cost = 10;
  cost.total_cost = 3510;
  cost.reason = "odf025_base";
  cost.confidence = opt::CostConfidence::kMedium;
  cost.selectable = true;
  return cost;
}

bool DiagnosticsCoverRequiredCases() {
  {
    const auto status = opt::EvaluateOptimizerRuntimeFeedback(BaseFeedback());
    if (!Require(status.ok, "OK feedback was rejected") ||
        !Require(status.applied, "OK feedback was not applied") ||
        !Require(status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.OK",
                 "OK diagnostic mismatch: " + status.diagnostic_code) ||
        !Require(Has(status.evidence, "ok_feedback"), "OK evidence missing") ||
        !Require(Has(status.evidence, "feedback_advisory_only=true"),
                 "advisory-only evidence missing") ||
        !Require(Has(status.evidence, "mga_visibility_recheck=preserved"),
                 "MGA visibility preservation evidence missing") ||
        !Require(Has(status.evidence, "mga_finality_authority=engine_transaction_inventory"),
                 "MGA finality authority evidence missing")) {
      return false;
    }
  }
  {
    auto feedback = BaseFeedback();
    feedback.estimated_rows = 10;
    feedback.actual_rows = 500;
    const auto status = opt::EvaluateOptimizerRuntimeFeedback(feedback);
    if (!Require(status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.HIGH_MISESTIMATE",
                 "high misestimate diagnostic mismatch: " + status.diagnostic_code) ||
        !Require(Has(status.evidence, "high_misestimate"),
                 "high misestimate evidence missing")) {
      return false;
    }
  }
  {
    auto feedback = BaseFeedback();
    feedback.operator_family.clear();
    const auto status = opt::EvaluateOptimizerRuntimeFeedback(feedback);
    if (!Require(!status.ok, "missing labels feedback was accepted") ||
        !Require(status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.MISSING_LABELS",
                 "missing labels diagnostic mismatch: " + status.diagnostic_code) ||
        !Require(Has(status.evidence, "missing_labels"), "missing labels evidence missing")) {
      return false;
    }
  }
  {
    auto feedback = BaseFeedback();
    feedback.memory_grant_bytes = 256 * 1024;
    feedback.peak_memory_bytes = 768 * 1024;
    const auto status = opt::EvaluateOptimizerRuntimeFeedback(feedback);
    if (!Require(status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.MEMORY_UNDERGRANT",
                 "memory undergrant diagnostic mismatch: " + status.diagnostic_code) ||
        !Require(Has(status.evidence, "memory_undergrant"), "memory undergrant evidence missing") ||
        !Require(status.memory_grant.recommended_grant_bytes >= feedback.peak_memory_bytes,
                 "memory undergrant recommendation did not cover observed peak")) {
      return false;
    }
  }
  {
    auto feedback = BaseFeedback();
    feedback.memory_grant_bytes = 2 * 1024 * 1024;
    feedback.peak_memory_bytes = 128 * 1024;
    const auto status = opt::EvaluateOptimizerRuntimeFeedback(feedback);
    if (!Require(status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.MEMORY_OVERGRANT",
                 "memory overgrant diagnostic mismatch: " + status.diagnostic_code) ||
        !Require(Has(status.evidence, "memory_overgrant"), "memory overgrant evidence missing") ||
        !Require(status.memory_grant.recommended_grant_bytes < feedback.memory_grant_bytes,
                 "memory overgrant recommendation did not reduce grant")) {
      return false;
    }
  }
  {
    auto feedback = BaseFeedback();
    feedback.actual_spill_bytes = 24 * 1024;
    const auto status = opt::EvaluateOptimizerRuntimeFeedback(feedback);
    if (!Require(status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.SPILL_OBSERVED",
                 "spill diagnostic mismatch: " + status.diagnostic_code) ||
        !Require(Has(status.evidence, "spill_observed"), "spill evidence missing") ||
        !Require(status.cost_profile.spill_penalty_pages == 6,
                 "spill penalty pages were not deterministic")) {
      return false;
    }
  }
  {
    auto feedback = BaseFeedback();
    feedback.estimated_pages = 8;
    feedback.actual_pages = 40;
    feedback.estimated_io_operations = 10;
    feedback.actual_io_operations = 60;
    const auto status = opt::EvaluateOptimizerRuntimeFeedback(feedback);
    if (!Require(status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.IO_PAGE_DRIFT",
                 "IO/page drift diagnostic mismatch: " + status.diagnostic_code) ||
        !Require(Has(status.evidence, "io_page_drift"), "IO/page drift evidence missing")) {
      return false;
    }
  }
  {
    auto feedback = BaseFeedback();
    feedback.freshness_microseconds = feedback.max_freshness_microseconds + 1;
    const auto status = opt::EvaluateOptimizerRuntimeFeedback(feedback);
    if (!Require(!status.applied, "stale feedback was applied") ||
        !Require(status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.STALE",
                 "stale feedback diagnostic mismatch: " + status.diagnostic_code) ||
        !Require(Has(status.evidence, "stale_feedback"), "stale feedback evidence missing")) {
      return false;
    }
  }
  {
    auto feedback = BaseFeedback();
    feedback.policy_allowed = false;
    const auto status = opt::EvaluateOptimizerRuntimeFeedback(feedback);
    if (!Require(!status.applied, "policy-disabled feedback was applied") ||
        !Require(status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.POLICY_DISABLED",
                 "policy-disabled diagnostic mismatch: " + status.diagnostic_code) ||
        !Require(Has(status.evidence, "policy_disabled_feedback"),
                 "policy-disabled feedback evidence missing")) {
      return false;
    }
  }
  {
    auto feedback = BaseFeedback();
    feedback.parser_or_donor_authority = true;
    feedback.transaction_finality_authority = "parser_worker";
    const auto status = opt::EvaluateOptimizerRuntimeFeedback(feedback);
    if (!Require(!status.ok, "unsafe feedback was accepted") ||
        !Require(status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.REJECTED_UNSAFE",
                 "unsafe feedback diagnostic mismatch: " + status.diagnostic_code) ||
        !Require(Has(status.evidence, "rejected_unsafe_feedback"),
                 "unsafe feedback evidence missing") ||
        !Require(Has(status.evidence,
                     "mga_finality_authority_required=engine_transaction_inventory"),
                 "unsafe feedback did not require MGA finality authority")) {
      return false;
    }
  }
  return true;
}

bool FeedbackAdjustsCostWithoutChangingSemantics() {
  auto feedback = BaseFeedback();
  feedback.estimated_rows = 100;
  feedback.actual_rows = 600;
  feedback.estimated_pages = 10;
  feedback.actual_pages = 30;
  feedback.estimated_io_operations = 20;
  feedback.actual_io_operations = 80;
  feedback.memory_grant_bytes = 256 * 1024;
  feedback.peak_memory_bytes = 512 * 1024;
  feedback.actual_spill_bytes = 8192;
  feedback.estimated_latency_microseconds = 1000;
  feedback.actual_latency_microseconds = 3000;

  const auto base = BaseCost();
  const auto adjusted = opt::ApplyOptimizerRuntimeFeedbackCost(base, feedback);
  if (!Require(adjusted.selectable == base.selectable,
               "feedback changed plan selectability/result semantics") ||
      !Require(adjusted.rejection_reason == base.rejection_reason,
               "feedback changed rejection semantics") ||
      !Require(adjusted.row_cost > base.row_cost, "row cost did not increase from feedback") ||
      !Require(adjusted.io_cost > base.io_cost, "IO cost did not increase from feedback") ||
      !Require(adjusted.memory_cost > base.memory_cost, "memory cost did not increase from feedback") ||
      !Require(adjusted.uncertainty_cost > base.uncertainty_cost,
               "uncertainty did not include feedback drift/spill evidence") ||
      !Require(adjusted.reason.find("feedback=calibrated:") != std::string::npos,
               "calibrated profile reason missing")) {
    return false;
  }

  auto unsafe = feedback;
  unsafe.parser_or_donor_authority = true;
  const auto rejected = opt::ApplyOptimizerRuntimeFeedbackCost(base, unsafe);
  if (!Require(rejected.row_cost == base.row_cost, "unsafe feedback changed row cost") ||
      !Require(rejected.io_cost == base.io_cost, "unsafe feedback changed IO cost") ||
      !Require(rejected.memory_cost == base.memory_cost, "unsafe feedback changed memory cost") ||
      !Require(rejected.reason.find("feedback_rejected=SB_OPTIMIZER_FEEDBACK.REJECTED_UNSAFE") !=
                   std::string::npos,
               "unsafe feedback rejection reason missing")) {
    return false;
  }

  std::vector<opt::OptimizerMetricCostInput> metrics;
  const auto add = [&](std::string name, std::uint64_t value) {
    opt::OptimizerMetricCostInput metric;
    metric.metric_name = std::move(name);
    metric.value = static_cast<double>(value);
    metric.policy_allowed = true;
    metric.operator_family = "table_scan";
    metric.plan_shape = "rel.customer.scan";
    metric.cost_profile_id = "odf025.metric";
    metrics.push_back(std::move(metric));
  };
  add("feedback.estimated_rows", 100);
  add("feedback.actual_rows", 400);
  add("feedback.estimated_pages", 10);
  add("feedback.actual_pages", 20);
  add("feedback.estimated_io_operations", 20);
  add("feedback.actual_io_operations", 40);
  add("feedback.estimated_visibility_recheck_rows", 5);
  add("feedback.actual_visibility_recheck_rows", 10);
  add("feedback.memory_grant_bytes", 256 * 1024);
  add("feedback.peak_memory_bytes", 512 * 1024);
  add("feedback.estimated_latency_microseconds", 1000);
  add("feedback.actual_latency_microseconds", 2000);
  add("feedback.estimated_resource_units", 100);
  add("feedback.actual_resource_units", 200);
  const auto metric_adjusted = opt::ApplyMetricFeedbackCost(base, metrics);
  return Require(metric_adjusted.total_cost > base.total_cost,
                 "runtime metric feedback did not feed cost adjustment") &&
         Require(metric_adjusted.selectable == base.selectable,
                 "runtime metric feedback changed result semantics");
}

bool CalibrationIsBoundedAndDeterministic() {
  auto feedback = BaseFeedback();
  feedback.estimated_rows = 1;
  feedback.actual_rows = 1000000000;
  feedback.estimated_pages = 1;
  feedback.actual_pages = 1000000000;
  feedback.estimated_io_operations = 1;
  feedback.actual_io_operations = 1000000000;
  feedback.estimated_visibility_recheck_rows = 1;
  feedback.actual_visibility_recheck_rows = 1000000000;
  feedback.estimated_latency_microseconds = 1;
  feedback.actual_latency_microseconds = 1000000000;
  feedback.estimated_resource_units = 1;
  feedback.actual_resource_units = 1000000000;

  const auto first = opt::EvaluateOptimizerRuntimeFeedback(feedback);
  const auto second = opt::EvaluateOptimizerRuntimeFeedback(feedback);
  return Require(first.cost_profile.row_cost_multiplier == 4.0,
                 "row multiplier was not bounded") &&
         Require(first.cost_profile.io_cost_multiplier == 4.0,
                 "IO multiplier was not bounded") &&
         Require(first.cost_profile.latency_cost_multiplier == 4.0,
                 "latency multiplier was not bounded") &&
         Require(first.cost_profile.profile_id == second.cost_profile.profile_id,
                 "profile id was not deterministic") &&
         Require(first.cost_profile.uncertainty_penalty == second.cost_profile.uncertainty_penalty,
                 "uncertainty penalty was not deterministic");
}

bool ObservabilityMetricContractsAreImplemented() {
  const std::vector<std::string> required_families = {
      "sb_optimizer_plan_estimate_error_ratio",
      "sb_optimizer_feedback_estimated_rows",
      "sb_optimizer_feedback_actual_rows",
      "sb_optimizer_feedback_estimated_pages",
      "sb_optimizer_feedback_actual_pages",
      "sb_optimizer_feedback_estimated_io_operations",
      "sb_optimizer_feedback_actual_io_operations",
      "sb_optimizer_feedback_estimated_visibility_recheck_rows",
      "sb_optimizer_feedback_actual_visibility_recheck_rows",
      "sb_optimizer_feedback_estimated_spill_bytes",
      "sb_optimizer_feedback_actual_spill_bytes",
      "sb_optimizer_feedback_memory_grant_bytes",
      "sb_optimizer_feedback_peak_memory_bytes",
      "sb_optimizer_feedback_recommended_memory_grant_bytes",
      "sb_optimizer_feedback_estimated_latency_microseconds",
      "sb_optimizer_feedback_actual_latency_microseconds",
      "sb_optimizer_feedback_estimated_resource_units",
      "sb_optimizer_feedback_actual_resource_units",
  };
  for (const auto& family : required_families) {
    const auto* descriptor = metrics::DefaultMetricRegistry().FindDescriptor(family);
    if (!Require(descriptor != nullptr, "optimizer feedback metric descriptor missing: " + family) ||
        !Require(descriptor->producer_owner == "optimizer_executor_feedback",
                 "optimizer feedback metric producer mismatch: " + family) ||
        !Require(descriptor->readiness == metrics::MetricReadiness::implemented,
                 "optimizer feedback metric is not implemented: " + family)) {
      return false;
    }
  }

  metrics::OptimizerRuntimeFeedbackMetricSample sample;
  sample.estimated_rows = 100;
  sample.actual_rows = 105;
  sample.estimated_pages = 16;
  sample.actual_pages = 16;
  sample.estimated_io_operations = 32;
  sample.actual_io_operations = 34;
  sample.estimated_visibility_recheck_rows = 8;
  sample.actual_visibility_recheck_rows = 8;
  sample.estimated_spill_bytes = 0;
  sample.actual_spill_bytes = 24 * 1024;
  sample.memory_grant_bytes = 1024 * 1024;
  sample.peak_memory_bytes = 512 * 1024;
  sample.recommended_memory_grant_bytes = 768 * 1024;
  sample.estimated_latency_microseconds = 1000;
  sample.actual_latency_microseconds = 1200;
  sample.estimated_resource_units = 100;
  sample.actual_resource_units = 110;
  const auto published = metrics::PublishOptimizerRuntimeFeedbackSample(sample,
                                                                        "table_scan",
                                                                        "rel.customer.scan");
  return Require(published.ok,
                 "optimizer feedback metric sample publish failed: " + published.diagnostic_code);
}

}  // namespace

int main() {
  if (!DiagnosticsCoverRequiredCases()) return 1;
  if (!FeedbackAdjustsCostWithoutChangingSemantics()) return 1;
  if (!CalibrationIsBoundedAndDeterministic()) return 1;
  if (!ObservabilityMetricContractsAreImplemented()) return 1;
  return 0;
}
