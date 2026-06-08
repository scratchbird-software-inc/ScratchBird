// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "logical_plan.hpp"
#include "optimizer_request.hpp"
#include "statistics_catalog.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OPCH-011 gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool Contains(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

plan::LogicalPlan SafeLogicalPlan() {
  auto logical = plan::BuildQueryShapePlan({plan::QueryShapeKind::kPointLookup});
  logical.optimizer_policy.optimizer_policy_metadata_present = true;
  logical.optimizer_policy.policy_source_kind = "sblr_api";
  logical.optimizer_policy.policy_epoch = 101;
  logical.optimizer_policy.safe_control_ids = {
      "join_search_budget:bounded_dp",
      "memory_grant_policy:governed",
      "plan_profile:diagnostic"};
  return logical;
}

opt::BoundOptimizerRequest SafeRequest() {
  opt::BoundOptimizerRequest request;
  request.context.request_uuid = "opch011.request";
  request.context.operation_id = "public_sql.select.point_lookup";
  request.context.sblr_digest = "sblr:opch011-safe-policy";
  request.context.descriptor_set_digest = "desc:opch011";
  request.context.statistics_snapshot_id = "stats:opch011";
  request.context.metric_snapshot_id = "metrics:opch011";
  request.context.executor_capability_set_id = "executor:opch011";
  request.context.catalog_epoch = 17;
  request.context.security_epoch = 19;
  request.context.policy_epoch = 101;
  request.context.security_context_present = true;
  request.context.transaction_context_present = true;
  request.logical_plan = SafeLogicalPlan();
  request.statistics.Add(opt::MakeStatistic("row_count",
                                            "relation",
                                            "rel.opch011",
                                            10.0,
                                            opt::StatisticSource::kCatalogExact,
                                            17,
                                            0,
                                            opt::CostConfidence::kHigh));
  return request;
}

bool AcceptsOnlySafeSblrApiLogicalPlanPolicy() {
  const auto validation = opt::ValidateBoundOptimizerRequest(SafeRequest());
  return Require(validation.ok, "safe SBLR/API policy metadata was rejected") &&
         Require(std::any_of(validation.authority_facts.begin(),
                             validation.authority_facts.end(),
                             [](const opt::OptimizerAuthorityFact& fact) {
                               return fact.fact_name == "optimizer_policy_metadata" &&
                                      fact.status == opt::OptimizerAuthorityStatus::kPresent &&
                                      fact.detail == "OPCH_ENGINE_BOUNDARY_PARSER_SAFE_CONTROLS";
                             }),
                 "safe optimizer policy boundary fact missing");
}

bool RejectsRawSqlTextPolicyAuthority() {
  auto request = SafeRequest();
  request.logical_plan.optimizer_policy.raw_sql_text_present = true;

  const auto validation = opt::ValidateBoundOptimizerRequest(request);
  return Require(!validation.ok, "raw SQL text policy claim was accepted") &&
         Require(Contains(validation.diagnostics,
                          "SB_OPT_AUTHORITY_REJECTED.optimizer_policy_raw_sql_text"),
                 "raw SQL text rejection diagnostic missing");
}

bool RejectsParserExecutionPolicyAuthority() {
  auto request = SafeRequest();
  request.logical_plan.optimizer_policy.parser_execution_authority_claimed = true;
  request.logical_plan.optimizer_policy.parser_session_directives_unbound = true;

  const auto result = opt::OptimizeBoundRequest(request);
  return Require(!result.ok, "parser execution policy claim was optimized") &&
         Require(result.diagnostic_code == "SB_OPT_REQUEST_REFUSED",
                 "parser execution claim did not refuse at optimizer boundary") &&
         Require(Contains(result.diagnostics,
                          "SB_OPT_AUTHORITY_REJECTED.optimizer_policy_parser_execution"),
                 "parser execution rejection diagnostic missing") &&
         Require(Contains(result.diagnostics,
                          "SB_OPT_AUTHORITY_REJECTED.optimizer_policy_unbound_parser_directive"),
                 "unbound parser directive rejection diagnostic missing");
}

bool RejectsDonorLegacyPolicyAuthority() {
  auto request = SafeRequest();
  request.logical_plan.optimizer_policy.policy_source_kind = "legacy_sql_text";
  request.logical_plan.optimizer_policy.donor_or_legacy_policy_authority_claimed = true;

  const auto validation = opt::ValidateBoundOptimizerRequest(request);
  return Require(!validation.ok, "donor/legacy policy authority was accepted") &&
         Require(Contains(validation.diagnostics,
                          "SB_OPT_AUTHORITY_REJECTED.optimizer_policy_donor_or_legacy_authority"),
                 "donor/legacy policy authority rejection diagnostic missing");
}

}  // namespace

int main() {
  // SEARCH_KEY: OPCH_ENGINE_BOUNDARY_PARSER_SAFE_CONTROLS
  if (!AcceptsOnlySafeSblrApiLogicalPlanPolicy()) return 1;
  if (!RejectsRawSqlTextPolicyAuthority()) return 1;
  if (!RejectsParserExecutionPolicyAuthority()) return 1;
  if (!RejectsDonorLegacyPolicyAuthority()) return 1;
  return 0;
}
