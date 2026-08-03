// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/plan_api.hpp"
#include "optimizer_contract.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

plan::CanonicalMgaStatementContext MgaContext() {
  plan::CanonicalMgaStatementContext context;
  context.statement_uuid =
      "019f0000-0000-7300-8000-000000015041";
  context.owning_transaction_uuid =
      "019f0000-0000-7300-8000-000000015042";
  context.statement_snapshot_uuid =
      "019f0000-0000-7300-8000-000000015043";
  context.statement_metadata_snapshot_uuid =
      "019f0000-0000-7300-8000-000000015044";
  context.owning_local_transaction_id = 1515;
  context.visible_committed_high_watermark = 1514;
  context.oldest_active_transaction_id = 1515;
  context.oldest_interesting_transaction_id = 1515;
  context.oldest_snapshot_transaction_id = 1515;
  context.retention_horizon_transaction_id = 1515;
  context.active_excluded_local_transaction_ids = {1515};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id = 1516;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-OPT-015-V1: " << detail << '\n';
  return condition;
}

exec::CanonicalExecutionMgaAuthority ClosureAuthority(
    const exec::TypedPhysicalNodeDag& dag) {
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = dag.mga_statement_context;
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  const auto current = authority.statement_context;
  authority.resolve_current = [current] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  return authority;
}

exec::PhysicalMgaStatementContext PhysicalContext(
    const std::uint64_t visible_committed_high_watermark) {
  exec::PhysicalMgaStatementContext context;
  context.statement_uuid =
      "019f0000-0000-7300-8000-000000015041";
  context.owning_transaction_uuid =
      "019f0000-0000-7300-8000-000000015042";
  context.statement_snapshot_uuid =
      "019f0000-0000-7300-8000-000000015043";
  context.statement_metadata_snapshot_uuid =
      "019f0000-0000-7300-8000-000000015044";
  context.owning_local_transaction_id = 1515;
  context.visible_committed_high_watermark =
      visible_committed_high_watermark;
  context.oldest_active_transaction_id = 1515;
  context.oldest_interesting_transaction_id = 1515;
  context.oldest_snapshot_transaction_id = 1515;
  context.retention_horizon_transaction_id = 1515;
  context.active_excluded_local_transaction_ids = {1515};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id = 1516;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

exec::TypedPhysicalNodeDag PhysicalDag(
    const std::uint64_t visible_committed_high_watermark = 1514) {
  exec::TypedPhysicalNodeDag dag;
  dag.abi_version = 2;
  dag.selected_plan_uuid = "019f0000-0000-7300-8000-000000015001";
  dag.root_physical_node_id = 2;
  dag.local_transaction_id = 1515;
  dag.statement_snapshot_id = visible_committed_high_watermark;
  dag.mga_statement_context =
      PhysicalContext(visible_committed_high_watermark);
  dag.bound_sblr_tree_uuid =
      "019f0000-0000-7300-8000-000000015011";
  dag.catalog_epoch_uuid =
      "019f0000-0000-7300-8000-000000015012";
  dag.security_context_uuid =
      "019f0000-0000-7300-8000-000000015013";
  dag.capability_snapshot_uuid =
      "019f0000-0000-7300-8000-000000015014";
  dag.resource_snapshot_uuid =
      "019f0000-0000-7300-8000-000000015015";
  dag.statistics_snapshot_uuid =
      "019f0000-0000-7300-8000-000000015020";
  dag.route_snapshot_uuid =
      "019f0000-0000-7300-8000-000000015017";
  dag.catalog_generation = 31;
  dag.security_epoch = 32;
  dag.policy_epoch = 33;
  dag.resource_epoch = 34;
  dag.statistics_generation = 35;
  dag.route_epoch = 36;
  dag.route_generation = 37;
  dag.memory_budget_bytes = 1 << 20;
  dag.spill_allowed = true;
  dag.optimizer_published = true;
  dag.immutable_node_identity_validated = true;
  dag.capability_validated_before_access = true;
  dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       dag.bound_sblr_tree_uuid},
      {exec::PhysicalAdmissionStage::kCatalogEpoch, dag.catalog_epoch_uuid},
      {exec::PhysicalAdmissionStage::kSecurity, dag.security_context_uuid},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       dag.mga_statement_context.statement_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       dag.capability_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kResource, dag.resource_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       dag.statistics_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       dag.route_snapshot_uuid},
  };
  exec::PhysicalNodeRecord values;
  values.physical_node_id = 1;
  values.relational_node_id = 11;
  values.node_kind = exec::PhysicalNodeKind::kValues;
  values.implementation_id = "values.literal.v1";
  values.output_descriptor_ids = {1};
  values.causal_counter_id = 15101;
  values.selected_alternative_uuid =
      "019f0000-0000-7300-8000-000000015051";
  values.executor_capability_uuid =
      "019f0000-0000-7300-8000-000000015052";
  values.executor_capability_abi_version = 1;
  values.cost_vector_uuid =
      "019f0000-0000-7300-8000-000000015053";
  values.memory_bytes_required = 1024;
  values.engine_capability_validated = true;
  values.mga_statement_context = dag.mga_statement_context;

  exec::PhysicalNodeRecord aggregate;
  aggregate.physical_node_id = 2;
  aggregate.relational_node_id = 12;
  aggregate.node_kind = exec::PhysicalNodeKind::kAggregate;
  aggregate.implementation_id = "aggregate.count.v1";
  aggregate.input_physical_node_ids = {1};
  aggregate.output_descriptor_ids = {2};
  aggregate.causal_counter_id = 15102;
  aggregate.selected_alternative_uuid =
      "019f0000-0000-7300-8000-000000015061";
  aggregate.executor_capability_uuid =
      "019f0000-0000-7300-8000-000000015062";
  aggregate.executor_capability_abi_version = 1;
  aggregate.cost_vector_uuid =
      "019f0000-0000-7300-8000-000000015063";
  aggregate.memory_bytes_required = 2048;
  aggregate.engine_capability_validated = true;
  aggregate.mga_statement_context = dag.mga_statement_context;
  dag.nodes = {std::move(values), std::move(aggregate)};
  return dag;
}

api::CanonicalRuntimeOptimizerStatisticsRequest ActualsRequest(
    const std::uint64_t visible_committed_high_watermark = 1514) {
  api::CanonicalRuntimeOptimizerStatisticsRequest request;
  request.selected_physical_dag =
      PhysicalDag(visible_committed_high_watermark);
  request.pre_access_statistics_snapshot_uuid =
      "019f0000-0000-7300-8000-000000015020";
  request.mga_authority = ClosureAuthority(request.selected_physical_dag);
  request.node_actuals = {
      {1, 11, 15101, 1, 0, 0, 0, 0, 0, true, true, true},
      {2, 12, 15102, 2, 0, 1, 0, 0, 0, true, true, true},
  };
  request.data_access_observed = true;
  request.all_executed_nodes_finished = true;
  request.estimates_frozen_before_access = true;
  request.engine_execution_evidence = true;
  return request;
}

bool ValidatePostExecutionActuals() {
  auto request = ActualsRequest();
  std::swap(request.node_actuals[0], request.node_actuals[1]);
  const auto result = api::BuildRuntimeOptimizerStatistics(request);
  return Require(result.accepted && result.post_execution_actuals &&
                     result.planning_estimates_immutable &&
                     !result.feedback_authorized && result.data_access_observed &&
                     result.issues.empty() && result.node_actuals.size() == 2 &&
                     result.node_actuals[0].physical_node_id == 1 &&
                     result.node_actuals[0].output_row_count == 0 &&
                     result.node_actuals[1].physical_node_id == 2 &&
                     result.selected_plan_uuid ==
                         request.selected_physical_dag.selected_plan_uuid &&
                     result.pre_access_statistics_snapshot_uuid ==
                         request.pre_access_statistics_snapshot_uuid &&
                     exec::PhysicalMgaStatementContextEqual(
                         result.mga_statement_context,
                         request.mga_authority.statement_context) &&
                     exec::PhysicalMgaStatementContextEqual(
                         result.mga_statement_context,
                         request.selected_physical_dag.mga_statement_context),
                 "valid completed-node actuals were not retained separately");
}

bool ValidateZeroHighWaterActuals() {
  const auto request = ActualsRequest(0);
  const auto result = api::BuildRuntimeOptimizerStatistics(request);
  return Require(
      result.accepted && result.issues.empty() &&
          result.mga_statement_context.visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              request.selected_physical_dag.mga_statement_context) &&
          result.node_actuals.size() == request.node_actuals.size(),
      "valid zero-high-water full context was treated as missing");
}

bool ValidatePhaseAndAuthorityRefusals() {
  const auto expect_refusal = [](auto mutation,
                                 const std::string_view diagnostic,
                                 const std::string_view detail) {
    auto request = ActualsRequest();
    mutation(request);
    const auto result = api::BuildRuntimeOptimizerStatistics(request);
    return Require(!result.accepted && !result.post_execution_actuals &&
                       !result.planning_estimates_immutable &&
                       !result.feedback_authorized &&
                       !result.data_access_observed &&
                       result.node_actuals.empty() &&
                       result.issues.size() == 1 &&
                       result.issues.front().diagnostic_id == diagnostic,
                   detail);
  };

  bool passed = true;
  passed &= expect_refusal(
      [](auto& request) { request.data_access_observed = false; },
      "QOW-DIAG-OPTIMIZER-ACTUALS-PHASE-V1",
      "pre-access actuals were accepted");
  passed &= expect_refusal(
      [](auto& request) {
        request.node_actuals[0].counters_captured_after_finish = false;
      },
      "QOW-DIAG-OPTIMIZER-ACTUALS-IDENTITY-V1",
      "counters captured before node finish were accepted");
  passed &= expect_refusal(
      [](auto& request) { request.node_actuals.pop_back(); },
      "QOW-DIAG-OPTIMIZER-ACTUALS-COVERAGE-V1",
      "missing executed-node actual was accepted");
  passed &= expect_refusal(
      [](auto& request) { request.node_actuals[1].causal_counter_id++; },
      "QOW-DIAG-OPTIMIZER-ACTUALS-IDENTITY-V1",
      "actual with a different causal identity was accepted");
  passed &= expect_refusal(
      [](auto& request) {
        request.node_actuals[0].execution_ordinal = 2;
        request.node_actuals[1].execution_ordinal = 1;
      },
      "QOW-DIAG-OPTIMIZER-ACTUALS-ORDER-V1",
      "consumer actual preceding its input was accepted");
  passed &= expect_refusal(
      [](auto& request) { request.parser_actuals_authority_claimed = true; },
      "QOW-DIAG-OPTIMIZER-ACTUALS-AUTHORITY-V1",
      "parser actuals authority was accepted");
  passed &= expect_refusal(
      [](auto& request) { request.transaction_finality_claimed = true; },
      "QOW-DIAG-OPTIMIZER-ACTUALS-AUTHORITY-V1",
      "runtime actuals transaction finality was accepted");
  passed &= expect_refusal(
      [](auto& request) { request.benchmark_authority_claimed = true; },
      "QOW-DIAG-OPTIMIZER-ACTUALS-AUTHORITY-V1",
      "runtime actuals benchmark authority was accepted");
  return passed;
}

bool ValidateActualsCannotBecomePreAccessEstimates() {
  plan::CanonicalLogicalRelationalGraph graph;
  graph.bound_sblr_tree_uuid = "019f0000-0000-7300-8000-000000015031";
  graph.catalog_epoch_uuid = "019f0000-0000-7300-8000-000000015032";
  graph.security_context_uuid = "019f0000-0000-7300-8000-000000015033";
  graph.local_transaction_id = 1515;
  graph.statement_snapshot_id = 1514;
  graph.mga_statement_context = MgaContext();
  graph.root_logical_node_id = 11;
  graph.result_descriptor_ids = {1};
  plan::CanonicalLogicalRelationalNode values;
  values.logical_node_id = 11;
  values.node_kind = plan::CanonicalLogicalRelationalNodeKind::kValues;
  values.output_descriptor_ids = {1};
  values.origin_relational_node_ids = {11};
  values.semantic_variant_id = "values.literal.v1";
  graph.nodes = {values};

  opt::CanonicalOptimizerStatisticsSnapshot snapshot;
  snapshot.statistics_snapshot_uuid =
      "019f0000-0000-7300-8000-000000015034";
  snapshot.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  snapshot.statistics_generation = 15;
  snapshot.admitted_at_monotonic_ns = 1500;
  snapshot.captured_before_data_access = true;
  snapshot.data_access_observed = true;
  snapshot.runtime_actuals_present = true;
  const auto result =
      opt::AdmitCanonicalOptimizerStatisticsBeforeAccess(graph, snapshot);
  return Require(!result.accepted && !result.data_access_allowed &&
                     result.issues.size() == 1 &&
                     result.issues.front().diagnostic_id ==
                         "QOW-DIAG-OPTIMIZER-STATISTICS-PHASE-V1",
                 "post-execution actuals entered pre-access statistics");
}

bool ValidateLegacyPreAccessPlannerDoesNotReadRows() {
  std::ifstream input(SB_QOW_PLAN_API_SOURCE_FILE);
  if (!Require(static_cast<bool>(input), "plan_api.cpp could not be read")) {
    return false;
  }
  const std::string source((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
  const auto begin =
      source.find("BuildLegacyPreAccessOptimizerStatistics(");
  const auto end = source.find("bool StatisticsForcedStale(", begin);
  if (!Require(begin != std::string::npos && end != std::string::npos &&
                   begin < end,
               "legacy pre-access statistics function is not bounded")) {
    return false;
  }
  const auto pre_access_body = source.substr(begin, end - begin);
  constexpr std::string_view kForbiddenPreAccessInputs[] = {
      "relation.rows",
      "VisibleCrudRowsForContext",
      "CountRetainedVersions",
      "CountIndexEntries",
      "CountDistinctIndexKeys",
      "CurrentMetricValue",
      "AddRuntimeStatistic",
  };
  bool passed = true;
  for (const auto token : kForbiddenPreAccessInputs) {
    passed &= Require(pre_access_body.find(token) == std::string::npos,
                      "legacy planner reads runtime rows or actuals");
  }
  return passed;
}

}  // namespace

// QOW-TEST-OPT-015-V1
int main() {
  bool passed = true;
  passed &= ValidatePostExecutionActuals();
  passed &= ValidateZeroHighWaterActuals();
  passed &= ValidatePhaseAndAuthorityRefusals();
  passed &= ValidateActualsCannotBecomePreAccessEstimates();
  passed &= ValidateLegacyPreAccessPlannerDoesNotReadRows();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
