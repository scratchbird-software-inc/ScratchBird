// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cost_model.hpp"
#include "join_planner_full.hpp"
#include "query/plan_api.hpp"
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
namespace api = scratchbird::engine::internal_api;
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

opt::CanonicalOptimizerExecutorAvailability ExecutorAvailability(
    const opt::CanonicalOptimizerAdmissionRequest& request) {
  using Kind = plan::CanonicalLogicalRelationalNodeKind;
  using Physical = exec::PhysicalNodeKind;
  opt::CanonicalOptimizerExecutorAvailability availability;
  availability.engine_owned = true;
  availability.capability_catalog.capability_snapshot_uuid = Uuid(5);
  availability.capability_catalog.policy_epoch = 33;
  availability.capability_catalog.engine_owned = true;

  opt::CanonicalExecutorCapabilityRecord heap;
  heap.capability_abi_version = 1;
  heap.implementation_id = "scan.heap.v1";
  heap.capability_uuid = Uuid(201);
  heap.logical_node_kind = Kind::kRelationSource;
  heap.physical_node_kind = Physical::kScan;
  heap.maximum_memory_bytes = request.resource.memory_budget_bytes;
  heap.storage_read_capable = true;
  heap.mga_visibility_capable = true;
  heap.available = true;
  heap.engine_owned = true;

  auto index = heap;
  index.implementation_id = "scan.index.btree.v1";
  index.capability_uuid = Uuid(202);

  opt::CanonicalExecutorCapabilityRecord project;
  project.capability_abi_version = 1;
  project.implementation_id = "project.direct.v1";
  project.capability_uuid = Uuid(203);
  project.logical_node_kind = Kind::kProject;
  project.physical_node_kind = Physical::kProject;
  project.minimum_input_count = 1;
  project.maximum_input_count = 1;
  project.maximum_memory_bytes = request.resource.memory_budget_bytes;
  project.available = true;
  project.engine_owned = true;
  for (const auto& property : request.logical_properties.properties) {
    project.supported_property_kinds.push_back(property.property_kind);
  }
  availability.capability_catalog.capabilities = {heap, index, project};
  availability.node_bindings = {
      {1, heap.capability_uuid, 64, true, {}},
      {1, index.capability_uuid, 64, true, {}},
      {2, project.capability_uuid, 64, true, {}},
  };
  return availability;
}

opt::RelationalDagPlanningInput PlanningInput() {
  opt::RelationalDagPlanningInput input;
  input.admission_request = Request();
  input.admission =
      opt::AdmitCanonicalOptimizerPlanningRequest(input.admission_request);
  input.executor_availability =
      ExecutorAvailability(input.admission_request);
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

opt::RelationalDagPlanningInput CompleteKindPlanningInput() {
  auto input = PlanningInput();
  auto& request = input.admission_request;
  auto& graph = request.logical_graph;
  const auto node = [](const std::uint32_t id,
                       const plan::CanonicalLogicalRelationalNodeKind kind,
                       std::vector<std::uint32_t> inputs,
                       std::string semantic) {
    plan::CanonicalLogicalRelationalNode value;
    value.logical_node_id = id;
    value.node_kind = kind;
    value.input_logical_node_ids = std::move(inputs);
    value.output_descriptor_ids = {100 + id};
    value.bound_expression_ids = {1000 + id};
    value.origin_relational_node_ids = {id};
    value.semantic_variant_id = std::move(semantic);
    return value;
  };
  graph.root_logical_node_id = 17;
  graph.result_descriptor_ids = {117};
  graph.nodes = {
      node(1, plan::CanonicalLogicalRelationalNodeKind::kRelationSource, {},
           "source.bound-relation.v1"),
      node(2, plan::CanonicalLogicalRelationalNodeKind::kValues, {},
           "values.literal-table.v1"),
      node(3, plan::CanonicalLogicalRelationalNodeKind::kJoin, {1, 2},
           "join.inner.v1"),
      node(4, plan::CanonicalLogicalRelationalNodeKind::kFilter, {3},
           "filter.where.v1"),
      node(5, plan::CanonicalLogicalRelationalNodeKind::kProject, {4},
           "project.select-list.v1"),
      node(6, plan::CanonicalLogicalRelationalNodeKind::kAggregate, {5},
           "aggregate.grouped.v1"),
      node(7, plan::CanonicalLogicalRelationalNodeKind::kSort, {6},
           "sort.required-order.v1"),
      node(8, plan::CanonicalLogicalRelationalNodeKind::kWindow, {7},
           "window.bound-spec.v1"),
      node(9, plan::CanonicalLogicalRelationalNodeKind::kLimit, {8},
           "limit.bound-count.v1"),
      node(10, plan::CanonicalLogicalRelationalNodeKind::kSubquery, {9},
           "subquery.table.v1"),
      node(11, plan::CanonicalLogicalRelationalNodeKind::kCte, {10},
           "cte.bound.v1"),
      node(12, plan::CanonicalLogicalRelationalNodeKind::kRecursiveCte,
           {11, 2}, "recursive-cte.union-all.v1"),
      node(13, plan::CanonicalLogicalRelationalNodeKind::kPivot, {12},
           "pivot.bound.v1"),
      node(14, plan::CanonicalLogicalRelationalNodeKind::kUnpivot, {13},
           "unpivot.bound.v1"),
      node(15,
           plan::CanonicalLogicalRelationalNodeKind::kTableFunctionInvoke,
           {}, "table-function.bound.v1"),
      node(16, plan::CanonicalLogicalRelationalNodeKind::kMatchRecognize,
           {15}, "match-recognize.bound.v1"),
      node(17, plan::CanonicalLogicalRelationalNodeKind::kSetOperation,
           {16, 14}, "set-operation.union-all.v1"),
  };
  graph.nodes[1].shareable = true;
  graph.nodes[0].required_object_uuids = {Uuid(601)};
  graph.nodes[14].required_object_uuids = {Uuid(602)};
  graph.nodes[14].argument_expression_ids = {2015, 2016};
  auto sort_ordering =
      Property(30, plan::CanonicalLogicalPropertyKind::kOrdering);
  sort_ordering.origin_logical_node_id = 7;
  sort_ordering.ordering_terms = {
      {1007, plan::CanonicalLogicalPropertySortDirection::kAscending,
       plan::CanonicalLogicalPropertyNullPlacement::kNullsLast, {}}};
  auto window_property =
      Property(32, plan::CanonicalLogicalPropertyKind::kWindow);
  window_property.origin_logical_node_id = 8;
  window_property.dependency_property_uuids = {sort_ordering.property_uuid};
  window_property.window_frame_descriptor_uuid = Uuid(755);
  request.logical_properties.properties = {sort_ordering, window_property};
  graph.nodes[6].delivered_property_uuids = {sort_ordering.property_uuid};
  graph.nodes[7].required_property_uuids = {sort_ordering.property_uuid};
  graph.nodes[7].delivered_property_uuids = {window_property.property_uuid};
  request.catalog.object_uuids = {Uuid(601), Uuid(602)};
  request.catalog.descriptor_ids.clear();
  for (std::uint32_t descriptor_id = 101; descriptor_id <= 117;
       ++descriptor_id) {
    request.catalog.descriptor_ids.push_back(descriptor_id);
  }
  request.security.authorized_object_uuids = request.catalog.object_uuids;
  request.policy_capability.supported_node_kinds.clear();
  request.statistics.node_estimates.clear();
  for (const auto& logical : graph.nodes) {
    request.policy_capability.supported_node_kinds.push_back(logical.node_kind);
    opt::CanonicalOptimizerNodeEstimate estimate;
    estimate.logical_node_id = logical.logical_node_id;
    estimate.catalog_epoch_uuid = graph.catalog_epoch_uuid;
    estimate.statistics_snapshot_uuid =
        request.statistics.statistics_snapshot_uuid;
    estimate.statistics_generation = request.statistics.statistics_generation;
    estimate.admitted_at_monotonic_ns = 700'000;
    if (logical.node_kind ==
        plan::CanonicalLogicalRelationalNodeKind::kRelationSource) {
      estimate.object_uuid = Uuid(601);
      estimate.state = opt::CanonicalOptimizerStatisticState::kKnown;
      estimate.source = opt::CanonicalOptimizerStatisticSource::kCatalogExact;
      estimate.collected_at_monotonic_ns = 650'000;
      estimate.maximum_age_ns = 100'000;
      estimate.confidence = opt::CostConfidence::kExact;
      estimate.row_count_present = true;
      estimate.row_count = 16;
      estimate.page_count_present = true;
      estimate.page_count = 2;
    } else if (logical.node_kind ==
               plan::CanonicalLogicalRelationalNodeKind::kValues) {
      estimate.state =
          opt::CanonicalOptimizerStatisticState::kNotApplicable;
      estimate.source = opt::CanonicalOptimizerStatisticSource::kUnavailable;
      estimate.confidence = opt::CostConfidence::kUnknown;
    } else {
      estimate.state = opt::CanonicalOptimizerStatisticState::kUnknown;
      estimate.source = opt::CanonicalOptimizerStatisticSource::kUnavailable;
      estimate.confidence = opt::CostConfidence::kUnknown;
    }
    request.statistics.node_estimates.push_back(std::move(estimate));
  }
  request.resource.maximum_candidate_count = 32;
  request.resource.maximum_memo_groups = 32;
  request.resource.maximum_search_steps = 512;
  input.admission = opt::AdmitCanonicalOptimizerPlanningRequest(request);

  const auto physical_kind = [](const auto kind) {
    using Logical = plan::CanonicalLogicalRelationalNodeKind;
    using Physical = exec::PhysicalNodeKind;
    switch (kind) {
      case Logical::kRelationSource: return Physical::kScan;
      case Logical::kValues: return Physical::kValues;
      case Logical::kJoin: return Physical::kJoin;
      case Logical::kFilter: return Physical::kFilter;
      case Logical::kProject: return Physical::kProject;
      case Logical::kAggregate: return Physical::kAggregate;
      case Logical::kSort: return Physical::kSort;
      case Logical::kWindow: return Physical::kWindow;
      case Logical::kLimit: return Physical::kLimit;
      case Logical::kSubquery: return Physical::kSubquery;
      case Logical::kCte: return Physical::kCte;
      case Logical::kRecursiveCte: return Physical::kRecursiveCte;
      case Logical::kPivot: return Physical::kPivot;
      case Logical::kUnpivot: return Physical::kUnpivot;
      case Logical::kMatchRecognize: return Physical::kMatchRecognize;
      case Logical::kTableFunctionInvoke:
        return Physical::kTableFunctionInvoke;
      case Logical::kSetOperation: return Physical::kSetOperation;
    }
    return Physical::kValues;
  };
  auto& availability = input.executor_availability;
  availability = {};
  availability.engine_owned = true;
  availability.capability_catalog.capability_snapshot_uuid = Uuid(5);
  availability.capability_catalog.policy_epoch = 33;
  availability.capability_catalog.engine_owned = true;
  for (const auto& logical : graph.nodes) {
    opt::CanonicalExecutorCapabilityRecord capability;
    capability.capability_abi_version = 1;
    capability.implementation_id =
        "complete." +
        std::string(plan::CanonicalLogicalRelationalNodeKindName(
            logical.node_kind)) +
        ".v1";
    capability.capability_uuid = Uuid(620 + logical.logical_node_id);
    capability.logical_node_kind = logical.node_kind;
    capability.physical_node_kind = physical_kind(logical.node_kind);
    capability.minimum_input_count = logical.input_logical_node_ids.size();
    capability.maximum_input_count = logical.input_logical_node_ids.size();
    capability.maximum_memory_bytes = request.resource.memory_budget_bytes;
    capability.supported_property_kinds = {
        plan::CanonicalLogicalPropertyKind::kOrdering,
        plan::CanonicalLogicalPropertyKind::kPartitioning,
        plan::CanonicalLogicalPropertyKind::kWindow};
    capability.storage_read_capable =
        logical.node_kind ==
        plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
    capability.mga_visibility_capable = capability.storage_read_capable;
    capability.available = true;
    capability.engine_owned = true;
    availability.capability_catalog.capabilities.push_back(capability);
    availability.node_bindings.push_back(
        {logical.logical_node_id, capability.capability_uuid, 64, true, {}});
  }
  input.search_policy.maximum_exhaustive_plan_count = 64;
  input.search_policy.bounded_beam_width = 32;
  return input;
}

bool ValidateOptimizerOwnedPlanning() {
  auto input = PlanningInput();
  bool passed = Require(input.admission.admitted && input.admission.planning_allowed,
                        "admission fixture was refused");
  const auto first = opt::PlanCanonicalRelationalDag(input);
  std::reverse(input.executor_availability.node_bindings.begin(),
               input.executor_availability.node_bindings.end());
  std::reverse(
      input.executor_availability.capability_catalog.capabilities.begin(),
      input.executor_availability.capability_catalog.capabilities.end());
  const auto second = opt::PlanCanonicalRelationalDag(input);
  if (!first.accepted) {
    std::cerr << "PUBLIC-OPTIMIZER-FOUNDATION-CONTRACT: planning_refusal=";
    if (!first.diagnostics.empty()) {
      std::cerr << first.diagnostics.front();
      if (!first.factory.issues.empty()) {
        std::cerr << ':' << first.factory.issues.front().logical_node_id << ':'
                  << first.factory.issues.front().implementation_id << ':'
                  << first.factory.issues.front().field_id;
      }
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

bool ValidateCompleteKindFactoryCoverage() {
  const auto input = CompleteKindPlanningInput();
  const auto result = opt::PlanCanonicalRelationalDag(input);
  bool has_match = false;
  bool has_table_function = false;
  for (const auto& candidate : result.factory.candidates) {
    has_match |= candidate.logical_node_id == 16 &&
                 candidate.cost_terms.predicate_evaluation_units != 0;
    has_table_function |= candidate.logical_node_id == 15 &&
                          candidate.cost_terms.udr_invocation_units != 0;
  }
  if (!input.admission.admitted || !result.accepted ||
      result.factory.inventory.candidate_count != 17 ||
      result.factory.candidates.size() != 17 ||
      result.publication.physical_dag.nodes.size() != 17 || !has_match ||
      !has_table_function) {
    std::cerr << "PUBLIC-OPTIMIZER-FOUNDATION-CONTRACT: complete_kind_refusal=";
    if (!input.admission.issues.empty()) {
      const auto& issue = input.admission.issues.front();
      std::cerr << issue.diagnostic_id << ':' << issue.field_id;
    } else if (!result.factory.issues.empty()) {
      const auto& issue = result.factory.issues.front();
      std::cerr << issue.diagnostic_id << ':' << issue.logical_node_id << ':'
                << issue.implementation_id << ':' << issue.field_id;
    } else if (!result.search.issues.empty()) {
      const auto& issue = result.search.issues.front();
      std::cerr << issue.diagnostic_id << ':' << issue.logical_node_id << ':'
                << issue.field_id;
    } else if (!result.publication.issues.empty()) {
      const auto& issue = result.publication.issues.front();
      std::cerr << issue.diagnostic_id << ':' << issue.logical_node_id << ':'
                << issue.field_id;
    } else if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front();
    } else {
      std::cerr << "accepted=" << result.accepted
                << ",candidates=" << result.factory.candidates.size()
                << ",inventory=" << result.factory.inventory.candidate_count
                << ",published="
                << result.publication.physical_dag.nodes.size()
                << ",match=" << has_match
                << ",table_function=" << has_table_function;
    }
    std::cerr << '\n';
  }
  return Require(
      input.admission.admitted && result.accepted &&
          result.factory.optimizer_owned_enumeration &&
          result.factory.inventory.candidate_count == 17 &&
          result.factory.candidates.size() == 17 &&
          result.publication.physical_dag.nodes.size() == 17 && has_match &&
          has_table_function,
      "optimizer factory did not enumerate/cost every logical node family");
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

bool ValidateTypedPhysicalPropertyCarrier() {
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid = Uuid(501);
  dag.bound_catalog_epoch_uuid = Uuid(502);
  dag.bound_security_context_uuid = Uuid(503);
  dag.statement_uuid = Uuid(504);
  dag.owning_transaction_uuid = Uuid(505);
  dag.statement_snapshot_uuid = Uuid(506);
  dag.statement_metadata_snapshot_uuid = Uuid(507);
  dag.local_transaction_id = kOwner;
  dag.root_node_id = 1;
  dag.descriptors = {
      {1, Uuid(508), Uuid(509), api::RelationalNullability::kNonNull},
  };
  api::RelationalExpressionRecord literal;
  literal.expression_id = 1;
  literal.expression_kind = api::RelationalExpressionKind::kLiteral;
  literal.result_descriptor_id = 1;
  literal.literal_kind = api::RelationalLiteralKind::kNumeric;
  literal.literal_or_parameter_ref = "1";
  dag.expressions.push_back(std::move(literal));
  dag.outputs = {{1, 1, 1, "value", 1, false, 0}};
  dag.values_rows = {{1, {1}}};
  api::RelationalDagNode node;
  node.node_id = 1;
  node.node_kind = api::RelationalDagNodeKind::kValues;
  node.output_descriptor_ids = {1};
  node.values_row_ids = {1};
  node.bound_expression_ids = {1};
  node.semantic_variant_id = "values.literal-table.v1";

  const auto property = [&](const std::uint64_t ordinal,
                            const api::RelationalPropertyKind kind) {
    api::RelationalPropertyRecord record;
    record.property_uuid = Uuid(520 + ordinal);
    record.property_kind = kind;
    record.origin_node_id = 1;
    return record;
  };
  auto distribution =
      property(1, api::RelationalPropertyKind::kDistribution);
  distribution.expression_ids = {1};
  distribution.distribution_kind =
      api::RelationalPropertyDistributionKind::kHashPartitioned;
  auto uniqueness = property(2, api::RelationalPropertyKind::kUniqueness);
  uniqueness.expression_ids = {1};
  auto materialization =
      property(3, api::RelationalPropertyKind::kMaterialization);
  materialization.materialization_kind =
      api::RelationalPropertyMaterializationKind::kMaterialized;
  auto rewindability =
      property(4, api::RelationalPropertyKind::kRewindability);
  rewindability.rewindability_kind =
      api::RelationalPropertyRewindabilityKind::kRewindable;
  const api::RelationalPropertyOrderingTerm order{
      1, api::RelationalPropertySortDirection::kAscending,
      api::RelationalPropertyNullPlacement::kNullsLast, {}};
  auto vector_ordering =
      property(5, api::RelationalPropertyKind::kVectorOrdering);
  vector_ordering.ordering_terms = {order};
  auto text_ordering =
      property(6, api::RelationalPropertyKind::kTextScoreOrdering);
  text_ordering.ordering_terms = {order};
  auto time_ordering =
      property(7, api::RelationalPropertyKind::kTimeOrdering);
  time_ordering.ordering_terms = {order};
  auto locality = property(8, api::RelationalPropertyKind::kLocality);
  locality.locality_kind = api::RelationalPropertyLocalityKind::kLocalNode;
  locality.locality_uuid = Uuid(540);
  auto visibility =
      property(9, api::RelationalPropertyKind::kSecurityVisibility);
  visibility.security_visibility_context_uuid =
      dag.bound_security_context_uuid;
  visibility.security_visibility_generation = 11;
  dag.properties = {distribution, uniqueness, materialization, rewindability,
                    vector_ordering, text_ordering, time_ordering, locality,
                    visibility};
  for (const auto& item : dag.properties) {
    node.delivered_property_uuids.push_back(item.property_uuid);
  }
  dag.nodes.push_back(std::move(node));

  const auto accepted = api::ValidateTypedRelationalDag(dag);
  auto forged = dag;
  forged.properties.back().security_visibility_context_uuid = Uuid(541);
  const auto refused = api::ValidateTypedRelationalDag(forged);
  return Require(
      accepted.accepted && accepted.validated_node_count == 1 &&
          dag.properties.size() == 9 &&
          !refused.accepted && !refused.issues.empty() &&
          refused.issues.front().diagnostic_id ==
              "QOW-DIAG-LOGICAL-PROPERTY-SHAPE-V1",
      "typed SBLR property carrier omitted or weakened a physical domain");
}

}  // namespace

int main() {
  const bool passed = ValidateOptimizerOwnedPlanning() &&
                      ValidateCompleteKindFactoryCoverage() &&
                      ValidateCostVectorBookkeeping() &&
                      ValidateCrossJoinSemantics() &&
                      ValidateTypedPhysicalPropertyCarrier();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
