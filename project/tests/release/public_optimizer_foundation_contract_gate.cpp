// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cost_model.hpp"
#include "join_planner_full.hpp"
#include "relational_planner.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

constexpr std::uint64_t kOwner = 0xffff'ffff'ffff'ff00ULL;
constexpr std::uint64_t kOldestActive = 0xffff'ffff'ffff'fee8ULL;
constexpr std::uint64_t kHorizon = 0xffff'ffff'ffff'fed0ULL;
constexpr std::uint64_t kInDoubt = 0xffff'ffff'ffff'fef0ULL;
constexpr std::uint64_t kInventoryNext = 0xffff'ffff'ffff'fff0ULL;

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "PUBLIC-OPTIMIZER-FOUNDATION-CONTRACT: " << detail << '\n';
  }
  return condition;
}

std::string Uuid(const std::uint64_t suffix) {
  std::array<char, 37> value{};
  std::snprintf(value.data(), value.size(),
                "019f0000-0000-7500-8000-%012llu",
                static_cast<unsigned long long>(suffix));
  return value.data();
}

plan::CanonicalMgaStatementContext MgaContext() {
  plan::CanonicalMgaStatementContext context;
  context.statement_uuid = Uuid(10);
  context.owning_transaction_uuid = Uuid(11);
  context.statement_snapshot_uuid = Uuid(12);
  context.statement_metadata_snapshot_uuid = Uuid(4);
  context.owning_local_transaction_id = kOwner;
  context.visible_committed_high_watermark = 0;
  context.oldest_active_transaction_id = kOldestActive;
  context.oldest_interesting_transaction_id = kHorizon;
  context.oldest_snapshot_transaction_id = kHorizon;
  context.retention_horizon_transaction_id = kHorizon;
  context.active_excluded_local_transaction_ids = {kOldestActive, kOwner};
  context.in_doubt_excluded_local_transaction_ids = {kInDoubt};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id = kInventoryNext;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

plan::CanonicalLogicalPropertyRecord Property(
    const std::uint64_t ordinal,
    const plan::CanonicalLogicalPropertyKind kind) {
  plan::CanonicalLogicalPropertyRecord property;
  property.property_uuid = Uuid(700 + ordinal);
  property.property_kind = kind;
  property.origin_logical_node_id = 2;
  property.populated_from_bound_sblr = true;
  return property;
}

opt::CanonicalOptimizerAdmissionRequest Request() {
  opt::CanonicalOptimizerAdmissionRequest request;
  auto& graph = request.logical_graph;
  graph.bound_sblr_tree_uuid = Uuid(1);
  graph.catalog_epoch_uuid = Uuid(2);
  graph.security_context_uuid = Uuid(3);
  graph.local_transaction_id = kOwner;
  graph.statement_snapshot_id = 0;
  graph.mga_statement_context = MgaContext();
  graph.root_logical_node_id = 2;
  graph.result_descriptor_ids = {102};

  plan::CanonicalLogicalRelationalNode scan;
  scan.logical_node_id = 1;
  scan.node_kind = plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  scan.output_descriptor_ids = {101};
  scan.origin_relational_node_ids = {1};
  scan.required_object_uuids = {Uuid(9)};
  scan.semantic_variant_id = "relation.source.v1";

  plan::CanonicalLogicalRelationalNode project;
  project.logical_node_id = 2;
  project.node_kind = plan::CanonicalLogicalRelationalNodeKind::kProject;
  project.input_logical_node_ids = {1};
  project.output_descriptor_ids = {102};
  project.bound_expression_ids = {70, 71};
  project.origin_relational_node_ids = {2};
  project.semantic_variant_id = "project.bound-expressions.v1";

  auto& catalog = request.logical_properties;
  catalog.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
  catalog.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  catalog.security_context_uuid = graph.security_context_uuid;
  catalog.local_transaction_id = graph.local_transaction_id;
  catalog.statement_snapshot_id = graph.statement_snapshot_id;
  catalog.mga_statement_context = graph.mga_statement_context;

  auto ordering = Property(1, plan::CanonicalLogicalPropertyKind::kOrdering);
  ordering.ordering_terms = {
      {70, plan::CanonicalLogicalPropertySortDirection::kAscending,
       plan::CanonicalLogicalPropertyNullPlacement::kNullsLast, Uuid(799)}};
  auto grouping = Property(2, plan::CanonicalLogicalPropertyKind::kGrouping);
  grouping.expression_ids = {70};
  auto partitioning =
      Property(3, plan::CanonicalLogicalPropertyKind::kPartitioning);
  partitioning.expression_ids = {70};
  auto window = Property(4, plan::CanonicalLogicalPropertyKind::kWindow);
  window.dependency_property_uuids = {ordering.property_uuid,
                                      partitioning.property_uuid};
  window.window_frame_descriptor_uuid = Uuid(798);
  auto equivalence =
      Property(5, plan::CanonicalLogicalPropertyKind::kExpressionEquivalence);
  equivalence.expression_ids = {70, 71};
  auto distribution =
      Property(6, plan::CanonicalLogicalPropertyKind::kDistribution);
  distribution.expression_ids = {70};
  distribution.distribution_kind =
      plan::CanonicalLogicalDistributionKind::kHashPartitioned;
  auto uniqueness =
      Property(7, plan::CanonicalLogicalPropertyKind::kUniqueness);
  uniqueness.expression_ids = {70};
  auto materialization =
      Property(8, plan::CanonicalLogicalPropertyKind::kMaterialization);
  materialization.materialization_kind =
      plan::CanonicalLogicalMaterializationKind::kStreaming;
  auto rewindability =
      Property(9, plan::CanonicalLogicalPropertyKind::kRewindability);
  rewindability.rewindability_kind =
      plan::CanonicalLogicalRewindabilityKind::kForwardOnly;
  auto vector_ordering =
      Property(10, plan::CanonicalLogicalPropertyKind::kVectorOrdering);
  vector_ordering.ordering_terms = ordering.ordering_terms;
  auto text_ordering =
      Property(11, plan::CanonicalLogicalPropertyKind::kTextScoreOrdering);
  text_ordering.ordering_terms = ordering.ordering_terms;
  auto time_ordering =
      Property(12, plan::CanonicalLogicalPropertyKind::kTimeOrdering);
  time_ordering.ordering_terms = ordering.ordering_terms;
  auto locality = Property(13, plan::CanonicalLogicalPropertyKind::kLocality);
  locality.locality_kind = plan::CanonicalLogicalLocalityKind::kLocalNode;
  locality.locality_uuid = Uuid(797);
  auto visibility =
      Property(14, plan::CanonicalLogicalPropertyKind::kSecurityVisibility);
  visibility.security_visibility_context_uuid = graph.security_context_uuid;
  visibility.security_visibility_generation = 32;
  catalog.properties = {ordering, grouping, partitioning, window, equivalence,
                        distribution, uniqueness, materialization,
                        rewindability, vector_ordering, text_ordering,
                        time_ordering, locality, visibility};
  for (const auto& property : catalog.properties) {
    project.required_property_uuids.push_back(property.property_uuid);
    project.delivered_property_uuids.push_back(property.property_uuid);
  }
  graph.nodes = {scan, project};

  request.catalog.snapshot_uuid = Uuid(4);
  request.catalog.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  request.catalog.catalog_generation = 31;
  request.catalog.object_uuids = {Uuid(9)};
  request.catalog.descriptor_ids = {101, 102};
  request.catalog.engine_owned = true;

  request.security.security_context_uuid = graph.security_context_uuid;
  request.security.security_epoch = 32;
  request.security.policy_epoch = 33;
  request.security.catalog_generation = 31;
  request.security.authorized_object_uuids = {Uuid(9)};
  request.security.engine_owned = true;

  request.mga.local_transaction_id = kOwner;
  request.mga.statement_snapshot_id = 0;
  request.mga.statement_context = MgaContext();
  request.mga.metadata_snapshot_uuid = Uuid(4);
  request.mga.transaction_active = true;
  request.mga.statement_snapshot_fixed = true;
  request.mga.engine_owned = true;

  request.policy_capability.policy_snapshot_uuid = Uuid(3);
  request.policy_capability.policy_epoch = 33;
  request.policy_capability.capability_snapshot_uuid = Uuid(5);
  request.policy_capability.capability_abi_version = 1;
  request.policy_capability.supported_node_kinds = {
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource,
      plan::CanonicalLogicalRelationalNodeKind::kProject};
  request.policy_capability.engine_owned = true;

  request.resource.resource_snapshot_uuid = Uuid(6);
  request.resource.resource_epoch = 34;
  request.resource.memory_budget_bytes = 1'000'000;
  request.resource.maximum_candidate_count = 8;
  request.resource.maximum_memo_groups = 4;
  request.resource.maximum_search_steps = 100;
  request.resource.maximum_planning_time_ns = 1'000'000;
  request.resource.spill_allowed = true;
  request.resource.engine_owned = true;

  request.statistics.statistics_snapshot_uuid = Uuid(7);
  request.statistics.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  request.statistics.statistics_generation = 35;
  request.statistics.admitted_at_monotonic_ns = 700'000;
  request.statistics.captured_before_data_access = true;
  opt::CanonicalOptimizerNodeEstimate scan_estimate;
  scan_estimate.logical_node_id = 1;
  scan_estimate.object_uuid = Uuid(9);
  scan_estimate.state = opt::CanonicalOptimizerStatisticState::kKnown;
  scan_estimate.source = opt::CanonicalOptimizerStatisticSource::kCatalogExact;
  scan_estimate.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  scan_estimate.statistics_snapshot_uuid = Uuid(7);
  scan_estimate.statistics_generation = 35;
  scan_estimate.collected_at_monotonic_ns = 650'000;
  scan_estimate.maximum_age_ns = 100'000;
  scan_estimate.admitted_at_monotonic_ns = 700'000;
  scan_estimate.confidence = opt::CostConfidence::kExact;
  scan_estimate.row_count_present = true;
  scan_estimate.row_count = 100;
  scan_estimate.page_count_present = true;
  scan_estimate.page_count = 10;
  opt::CanonicalOptimizerNodeEstimate project_estimate;
  project_estimate.logical_node_id = 2;
  project_estimate.state = opt::CanonicalOptimizerStatisticState::kUnknown;
  project_estimate.source =
      opt::CanonicalOptimizerStatisticSource::kUnavailable;
  project_estimate.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  project_estimate.statistics_snapshot_uuid = Uuid(7);
  project_estimate.statistics_generation = 35;
  project_estimate.admitted_at_monotonic_ns = 700'000;
  project_estimate.confidence = opt::CostConfidence::kUnknown;
  request.statistics.node_estimates = {scan_estimate, project_estimate};

  request.route.route_snapshot_uuid = Uuid(8);
  request.route.route_epoch = 36;
  request.route.route_generation = 37;
  request.route.operation_id = "query.execute";
  request.route.route_id = "native.sblr.query.execute.v2";
  request.route.native_local_route = true;
  request.route.engine_owned = true;
  request.populated_from_admitted_typed_sblr = true;
  return request;
}

std::vector<opt::CanonicalOptimizerImplementationProfile> Implementations(
    const opt::CanonicalOptimizerAdmissionRequest& request) {
  using Kind = plan::CanonicalLogicalRelationalNodeKind;
  using Physical = exec::PhysicalNodeKind;
  opt::CanonicalOptimizerImplementationProfile heap;
  heap.logical_node_id = 1;
  heap.implementation_id = "scan.heap.v1";
  heap.capability_uuid = Uuid(201);
  heap.logical_node_kind = Kind::kRelationSource;
  heap.physical_node_kind = Physical::kScan;
  heap.transformation_rule_id = "canonical.scan.heap.v1";
  heap.memory_bytes_required = 64;
  heap.page_read_sequential_units = 10;
  heap.mga_visibility_checks_expected = 100;
  heap.storage_read_capable = true;
  heap.mga_visibility_capable = true;
  heap.parallel_safe = true;
  heap.model_family_id = "relational.local.v1";

  auto index = heap;
  index.implementation_id = "scan.index.btree.v1";
  index.capability_uuid = Uuid(202);
  index.transformation_rule_id = "canonical.scan.index.btree.v1";
  index.page_read_sequential_units = 0;
  index.page_read_random_units = 2;
  index.cache_units = 1;

  opt::CanonicalOptimizerImplementationProfile project;
  project.logical_node_id = 2;
  project.implementation_id = "project.direct.v1";
  project.capability_uuid = Uuid(203);
  project.logical_node_kind = Kind::kProject;
  project.physical_node_kind = Physical::kProject;
  project.transformation_rule_id = "canonical.project.direct.v1";
  project.minimum_input_count = 1;
  project.maximum_input_count = 1;
  project.estimated_rows_hint = 7;
  project.memory_bytes_required = 64;
  project.predicate_evaluation_units = 3;
  project.parallel_safe = true;
  project.model_family_id = "relational.local.v1";
  for (const auto& property : request.logical_properties.properties) {
    project.delivered_property_uuids.push_back(property.property_uuid);
    project.supported_property_kinds.push_back(property.property_kind);
  }
  return {heap, index, project};
}

opt::RelationalDagPlanningInput PlanningInput() {
  opt::RelationalDagPlanningInput input;
  input.admission_request = Request();
  input.admission =
      opt::AdmitCanonicalOptimizerPlanningRequest(input.admission_request);
  input.implementations = Implementations(input.admission_request);
  input.search_policy.maximum_exhaustive_plan_count = 16;
  input.search_policy.bounded_beam_width = 4;
  input.search_policy.deterministic_step_cost_ns = 1;
  input.search_policy.engine_owned = true;
  input.publication_identity.selected_plan_uuid = Uuid(300);
  input.publication_identity.first_causal_counter_id = 1;
  input.publication_identity.engine_owned = true;
  input.identity_scope = "public.optimizer.foundation.release.v1";
  input.calibration_profile_uuid = Uuid(301);
  return input;
}

bool ValidateOptimizerOwnedPlanning() {
  auto input = PlanningInput();
  bool passed = Require(input.admission.admitted && input.admission.planning_allowed,
                        "admission fixture was refused");
  const auto first = opt::PlanCanonicalRelationalDag(input);
  std::reverse(input.implementations.begin(), input.implementations.end());
  const auto second = opt::PlanCanonicalRelationalDag(input);
  if (!first.accepted) {
    std::cerr << "PUBLIC-OPTIMIZER-FOUNDATION-CONTRACT: planning_refusal=";
    if (!first.diagnostics.empty()) {
      std::cerr << first.diagnostics.front();
    } else if (!first.factory.issues.empty()) {
      std::cerr << first.factory.issues.front().diagnostic_id << ':'
                << first.factory.issues.front().field_id;
    } else if (!first.search.issues.empty()) {
      std::cerr << first.search.issues.front().diagnostic_id << ':'
                << first.search.issues.front().field_id;
    } else if (!first.publication.issues.empty()) {
      std::cerr << first.publication.issues.front().diagnostic_id << ':'
                << first.publication.issues.front().field_id;
    } else {
      std::cerr << "unknown";
    }
    std::cerr << '\n';
  }
  passed &= Require(
      first.accepted && first.optimizer_owned &&
          first.complete_logical_dag_covered && first.physical_dag_published &&
          !first.data_access_allowed && first.diagnostics.empty() &&
          first.factory.optimizer_owned_enumeration &&
          first.factory.snapshot_derived && first.factory.deterministic &&
          first.factory.inventory.candidate_count == 3 &&
          first.factory.candidates.size() == 3 &&
          first.publication.physical_dag.nodes.size() == 2,
      "optimizer-owned factory/search/publication route was incomplete");
  passed &= Require(
      second.accepted &&
          first.search.selected_plan_signature ==
              second.search.selected_plan_signature,
      "implementation catalog order changed optimizer selection");

  const auto scan_candidate = std::ranges::find_if(
      first.factory.candidates, [](const auto& candidate) {
        return candidate.logical_node_id == 1 &&
               candidate.cost_terms.page_read_random_units == 2;
      });
  passed &= Require(
      scan_candidate != first.factory.candidates.end() &&
          scan_candidate->cost_terms.cpu_units == 100 &&
          scan_candidate->cost_terms.cache_units == 1 &&
          scan_candidate->cost_terms.mga_visibility_checks_expected == 100 &&
          scan_candidate->statistics_snapshot_uuid == Uuid(7) &&
          scan_candidate->statistics_generation == 35,
      "factory did not derive the scan cost from admitted snapshots");
  const auto physical_scan = std::ranges::find_if(
      first.publication.physical_dag.nodes, [](const auto& node) {
        return node.relational_node_id == 1;
      });
  passed &= Require(
      physical_scan != first.publication.physical_dag.nodes.end() &&
          physical_scan->implementation_id == "scan.index.btree.v1" &&
          physical_scan->retained_cost.cpu_units == 100 &&
          physical_scan->retained_cost.page_read_random_units == 2 &&
          physical_scan->retained_cost.cache_units == 1,
      "published DAG lost the selected multidimensional cost vector");
  return passed;
}

bool ValidateCostVectorBookkeeping() {
  opt::CostVector left;
  left.cpu_units = 2;
  left.sequential_io_units = 3;
  left.random_io_units = 5;
  left.page_write_units = 7;
  left.cache_units = 11;
  left.memory_grant_bytes = 13;
  left.spill_units = 17;
  left.network_units = 19;
  left.compression_units = 23;
  left.encryption_units = 29;
  left.predicate_evaluation_units = 31;
  left.vector_distance_units = 37;
  left.text_scoring_units = 41;
  left.spatial_evaluation_units = 43;
  left.udr_invocation_units = 47;
  left.mga_units = 53;
  left.index_maintenance_units = 59;
  left.confidence = opt::CostConfidence::kHigh;
  opt::FinalizeCostVector(&left);
  const auto initial_cpu = left.cpu_units;
  const auto initial_total = left.total_cost;
  opt::FinalizeCostVector(&left);

  opt::CostVector legacy = opt::EstimateNodeCost(plan::MakeLogicalPlanNode(
      plan::LogicalPlanNodeKind::kDmlRead,
      plan::PhysicalAccessKind::kJoinHash,
      "public.optimizer.foundation.legacy-cost",
      "legacy_cost_mutation"));
  const auto legacy_initial_cpu = legacy.cpu_units;
  legacy.row_cost += 100;
  opt::FinalizeCostVector(&legacy);
  const auto legacy_updated_cpu = legacy.cpu_units;
  opt::FinalizeCostVector(&legacy);

  opt::CostVector right;
  right.cpu_units = 61;
  right.random_io_units = 67;
  right.page_write_units = 71;
  right.confidence = opt::CostConfidence::kMedium;
  opt::AccumulateCostVector(&left, right);
  const auto json = opt::SerializeCostVectorToJson(left);
  return Require(
      initial_cpu == 2 && initial_total != 0 &&
          legacy_updated_cpu == legacy_initial_cpu + 100 &&
          legacy.cpu_units == legacy_updated_cpu && left.cpu_units == 63 &&
          left.random_io_units == 72 && left.page_write_units == 78 &&
          left.total_cost > initial_total &&
          json.find("\"page_write_units\": 78") != std::string::npos &&
          json.find("\"vector_distance_units\": 37") != std::string::npos &&
          json.find("\"mga_units\": 53") != std::string::npos,
      "cost dimensions were overwritten, collapsed, or omitted from explain JSON");
}

bool ValidateCrossJoinSemantics() {
  std::vector<opt::JoinRelationNode> relations = {
      {Uuid(401), 6, 64}, {Uuid(402), 7, 64}};
  opt::JoinPredicateEdge cross;
  cross.left_relation_uuid = relations[0].relation_uuid;
  cross.right_relation_uuid = relations[1].relation_uuid;
  cross.predicate_kind = "join.cross";
  cross.semantic_kind = opt::JoinSemanticKind::kCross;
  cross.predicate_count = 0;
  cross.selectivity = 1.0;
  const auto graph = opt::BuildJoinGraph(relations, {cross}, false, false);
  opt::JoinSearchPolicy policy;
  policy.strategy = opt::JoinSearchStrategy::kHypergraphGreedy;
  policy.memory_budget_bytes = 1'000'000;
  const auto result = opt::EnumerateJoinOrderWithPolicy(graph, policy);

  cross.predicate_count = 1;
  const auto forged = opt::BuildJoinGraph(relations, {cross}, false, false);
  const auto refused = opt::EnumerateJoinOrderWithPolicy(forged, policy);
  return Require(
      graph.valid && graph.contains_cross_join &&
          graph.contains_explicit_barrier && !opt::JoinReorderAllowed(graph) &&
          result.ok && result.selected_strategy == opt::JoinSearchStrategy::kInputOrder &&
          result.semantic_order_preserved && !result.reorder_applied &&
          result.method == plan::PhysicalAccessKind::kJoinNestedLoop &&
          result.estimated_rows == 42 && !forged.valid && !refused.ok &&
          refused.fallback_reason ==
              "SB_OPT_JOIN_CROSS_SEMANTIC_PREDICATE_INVALID",
      "CROSS identity, cardinality, method, or reorder barrier was not exact");
}

}  // namespace

int main() {
  const bool passed = ValidateOptimizerOwnedPlanning() &&
                      ValidateCostVectorBookkeeping() &&
                      ValidateCrossJoinSemantics();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
