// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_explain.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OEIC runtime payload gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool ContainsText(const std::string& value, const std::string& expected) {
  return value.find(expected) != std::string::npos;
}

plan::LogicalPlan Logical() {
  plan::LogicalPlan logical;
  logical.ok = true;
  logical.plan_id = "logical:oeic050";
  logical.optimizer_policy.optimizer_policy_metadata_present = true;
  logical.optimizer_policy.policy_source_kind = "sblr_api";
  logical.optimizer_policy.policy_epoch = 50;
  logical.optimizer_policy.normalized_controls.safe_control_ids = {
      "join.frontier_width.16",
      "memory.feedback.trusted"};
  logical.optimizer_policy.safe_control_ids = {
      "driver.explain.redacted",
      "route.equivalence.required"};
  return logical;
}

opt::BoundOptimizerRequest Request() {
  // SEARCH_KEY: OEIC_RUNTIME_PAYLOAD_EXPLAIN_ENTERPRISE
  opt::BoundOptimizerRequest request;
  request.context.request_uuid = "request:oeic050";
  request.context.operation_id = "dml.select_rows";
  request.context.sblr_digest = "sblr:oeic050";
  request.context.descriptor_set_digest = "descriptor:oeic050";
  request.context.statistics_snapshot_id = "stats:oeic050";
  request.context.metric_snapshot_id = "metrics:oeic050";
  request.context.executor_capability_set_id = "executor:local:oeic050";
  request.context.catalog_epoch = 10;
  request.context.stats_epoch = 11;
  request.context.security_epoch = 12;
  request.context.redaction_epoch = 13;
  request.context.policy_epoch = 14;
  request.context.resource_epoch = 15;
  request.context.name_resolution_epoch = 16;
  request.context.memory_policy_epoch = 17;
  request.context.memory_feedback_generation = 18;
  request.context.route_epoch = 19;
  request.context.security_context_present = true;
  request.context.transaction_context_present = true;
  request.logical_plan = Logical();
  return request;
}

opt::BoundOptimizerResult Result() {
  opt::BoundOptimizerResult result;
  result.ok = true;
  result.diagnostic_code = "SB_OPT_OK";
  result.plan_id = "plan:oeic050";
  result.optimizer_profile = "enterprise:oeic050";

  opt::PlanCandidate refused;
  refused.candidate_id = "candidate.scan";
  refused.access_kind = plan::PhysicalAccessKind::kTableScan;
  refused.cost.selectable = false;
  refused.cost.rejection_reason = "route_benchmark_clean_required";
  refused.refusal_reasons.push_back("missing_metric_snapshot");
  result.candidates.push_back(refused);

  opt::PlanCandidate selected;
  selected.candidate_id = "candidate.join";
  selected.access_kind = plan::PhysicalAccessKind::kJoinHash;
  selected.selected = true;
  selected.cost.selectable = true;
  selected.cost.total_cost = 42;
  selected.runtime_evidence = {
      "join_strategy=bounded_dp",
      "SB_OPT_JOIN_FRONTIER_PROPERTY_RETENTION",
      "adaptive_feedback.feedback_generation=18",
      "runtime_feedback.feedback_uuid=feedback:oeic050",
      "optimizer_feedback.spill_observed",
      "actual_rows=1000",
      "runtime_actual.rows_examined=1200",
      "runtime_actual.loop_count=2",
      "enterprise_relational_operator.kind=aggregate",
      "enterprise_relational_operator.memory_budget_bytes=4194304",
      "optimizer_memory_feedback.spill_passes=1",
      "enterprise_relational_operator.feedback_applied=true",
      "enterprise_relational_operator.route_label=embedded:oeic050",
      "enterprise_relational_operator.plan_node_id=plan-node-oeic050",
      "enterprise_relational_operator.result_contract_hash=result-contract-oeic050",
      "optimizer_explain.protected_material_redacted=true"};
  result.candidates.push_back(selected);
  return result;
}

bool DriverVisiblePayloadContainsEnterpriseFields() {
  const auto document = opt::BuildOptimizerExplainDocument(Request(), Result());
  const auto json = opt::RenderOptimizerExplainJson(document);

  return Require(!document.plan_hash.empty(), "plan hash missing") &&
         Require(Has(document.invalidation_dependencies, "catalog_epoch=10"),
                 "catalog epoch dependency missing") &&
         Require(Has(document.invalidation_dependencies, "stats_epoch=11"),
                 "stats epoch dependency missing") &&
         Require(Has(document.invalidation_dependencies, "redaction_epoch=13"),
                 "redaction epoch dependency missing") &&
         Require(Has(document.invalidation_dependencies, "memory_feedback_generation=18"),
                 "memory feedback generation dependency missing") &&
         Require(Has(document.statistics_provenance, "statistics_snapshot_id=stats:oeic050"),
                 "statistics snapshot provenance missing") &&
         Require(Has(document.statistics_provenance, "metric_snapshot_id=metrics:oeic050"),
                 "metric snapshot provenance missing") &&
         Require(Has(document.optimizer_controls,
                     "normalized_safe_control_id=join.frontier_width.16"),
                 "normalized optimizer control missing") &&
         Require(Has(document.optimizer_controls,
                     "safe_control_id=driver.explain.redacted"),
                 "driver explain control missing") &&
         Require(Has(document.candidate_refusals,
                     "candidate_refusal=candidate.scan:missing_metric_snapshot"),
                 "candidate refusal missing") &&
         Require(!document.join_search_telemetry.empty(),
                 "join search telemetry missing") &&
         Require(!document.adaptive_feedback_evidence.empty(),
                 "feedback evidence missing") &&
         Require(!document.runtime_actuals.empty(),
                 "runtime actuals missing") &&
         Require(!document.memory_metric_evidence.empty(),
                 "memory metric evidence missing") &&
         Require(Has(document.route_evidence,
                     "enterprise_relational_operator.route_label=embedded:oeic050"),
                 "route evidence missing") &&
         Require(Has(document.route_evidence,
                     "enterprise_relational_operator.result_contract_hash=result-contract-oeic050"),
                 "result contract evidence missing") &&
         Require(Has(document.redactions,
                     "optimizer_explain.protected_material_redacted=true"),
                 "redaction evidence missing") &&
         Require(ContainsText(json, "\"memory_metric_evidence\""),
                 "JSON omitted memory metric evidence") &&
         Require(ContainsText(json, "\"route_evidence\""),
                 "JSON omitted route evidence") &&
         Require(!ContainsText(json, "SELECT "),
                 "driver-visible explain leaked SQL text");
}

}  // namespace

int main() {
  if (!DriverVisiblePayloadContainsEnterpriseFields()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
