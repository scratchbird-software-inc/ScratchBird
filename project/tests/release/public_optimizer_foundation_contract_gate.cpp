// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cost_model.hpp"
#include "join_planner_full.hpp"
#include "model_family_profile_factory.hpp"
#include "optimizer_prepare_metric_collector.hpp"
#include "query/plan_api.hpp"
#include "relational_planner.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
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
          scan_candidate->cost_terms.cache_miss_units == 1 &&
          scan_candidate->cost_terms.memory_allocation_units == 64 &&
          scan_candidate->cost_terms.memory_grant_opportunity_units == 64 &&
          scan_candidate->cost_terms.mga_version_traversal_units == 100 &&
          scan_candidate->cost_terms.mga_visibility_check_units == 100 &&
          scan_candidate->cost_terms.complete_dimension_vector &&
          scan_candidate->cost_terms.scalarization_policy_id ==
              "canonical.optimizer.complete-unit-sum-minus-cache-benefit.v1" &&
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
          physical_scan->retained_cost.cache_units == 1 &&
          physical_scan->retained_cost.cache_miss_units == 1 &&
          physical_scan->retained_cost.memory_allocation_units == 64 &&
          physical_scan->retained_cost.mga_visibility_check_units == 100 &&
          physical_scan->retained_cost.complete_dimension_vector &&
          physical_scan->retained_cost.scalarization_policy_id ==
              "canonical.optimizer.complete-unit-sum-minus-cache-benefit.v1",
      "published DAG lost the selected multidimensional cost vector");
  return passed;
}

opt::CanonicalPlannerContextAuthority PlanningContextAuthority(
    const std::uint64_t identity, const std::string& invalid_state_behavior) {
  opt::CanonicalPlannerContextAuthority authority;
  authority.context_uuid = Uuid(identity);
  authority.generation = identity + 1;
  authority.authority_uuid = Uuid(identity + 2);
  authority.authority_generation = identity + 3;
  authority.confidence_basis_points = 10'000;
  authority.dependency_signature = std::string(64, 'd');
  authority.invalid_state_behavior_id = invalid_state_behavior;
  authority.engine_owned = true;
  return authority;
}

bool ValidateContinuationAndWhatIfPlanningContexts() {
  auto continuation_input = PlanningInput();
  const auto ordering = std::ranges::find_if(
      continuation_input.admission_request.logical_properties.properties,
      [](const auto& property) {
        return property.property_kind ==
               plan::CanonicalLogicalPropertyKind::kOrdering;
      });
  const auto materialization = std::ranges::find_if(
      continuation_input.admission_request.logical_properties.properties,
      [](const auto& property) {
        return property.property_kind ==
               plan::CanonicalLogicalPropertyKind::kMaterialization;
      });
  const auto rewindability = std::ranges::find_if(
      continuation_input.admission_request.logical_properties.properties,
      [](const auto& property) {
        return property.property_kind ==
               plan::CanonicalLogicalPropertyKind::kRewindability;
      });
  if (ordering ==
          continuation_input.admission_request.logical_properties.properties
              .end() ||
      materialization ==
          continuation_input.admission_request.logical_properties.properties
              .end() ||
      rewindability ==
          continuation_input.admission_request.logical_properties.properties
              .end()) {
    return Require(false, "resumable property fixture is incomplete");
  }
  materialization->materialization_kind =
      plan::CanonicalLogicalMaterializationKind::kMaterialized;
  rewindability->rewindability_kind =
      plan::CanonicalLogicalRewindabilityKind::kRewindable;
  continuation_input.admission = opt::AdmitCanonicalOptimizerPlanningRequest(
      continuation_input.admission_request);
  continuation_input.executor_availability =
      ExecutorAvailability(continuation_input.admission_request);

  opt::CanonicalPlannerContinuationContext continuation;
  continuation.authority = PlanningContextAuthority(
      810, "reject_continuation_plan");
  continuation.prepared_statement_uuid = Uuid(813);
  continuation.prepared_statement_generation = 814;
  continuation.cursor_uuid = Uuid(815);
  continuation.cursor_generation = 816;
  continuation.continuation_token_uuid = Uuid(817);
  continuation.continuation_token_generation = 818;
  continuation.resume_boundary_uuid = Uuid(819);
  continuation.resume_boundary_generation = 820;
  continuation.result_schema_uuid = Uuid(821);
  continuation.required_ordering_property_uuid = ordering->property_uuid;
  continuation.required_materialization_property_uuid =
      materialization->property_uuid;
  continuation.required_rewindability_property_uuid =
      rewindability->property_uuid;
  continuation.cursor_mode = opt::CanonicalPlannerCursorMode::kScrollable;
  continuation.holdability =
      opt::CanonicalPlannerCursorHoldability::kHoldable;
  continuation.continuation_requested = true;
  continuation_input.continuation_context = continuation;

  const auto continued = opt::PlanCanonicalRelationalDag(continuation_input);
  if (!continued.accepted || !continued.physical_dag_published ||
      !continued.planning_context.continuation_receipt.has_value() ||
      (continued.planning_context.continuation_receipt.has_value() &&
       !continued.planning_context.continuation_receipt
            ->physical_root_delivery_validated)) {
    std::cerr << "PUBLIC-OPTIMIZER-FOUNDATION-CONTRACT: continuation_refusal=";
    if (!continued.planning_context.issues.empty()) {
      std::cerr << continued.planning_context.issues.front().diagnostic_id << ':'
                << continued.planning_context.issues.front().field_id;
    } else if (!continued.factory.issues.empty()) {
      std::cerr << continued.factory.issues.front().diagnostic_id << ':'
                << continued.factory.issues.front().field_id;
    } else if (!continued.search.issues.empty()) {
      std::cerr << continued.search.issues.front().diagnostic_id << ':'
                << continued.search.issues.front().field_id;
    } else if (!continued.publication.issues.empty()) {
      std::cerr << continued.publication.issues.front().diagnostic_id << ':'
                << continued.publication.issues.front().field_id;
    } else if (!continued.diagnostics.empty()) {
      std::cerr << continued.diagnostics.front();
    } else {
      std::cerr << "accepted=" << continued.accepted
                << ",published=" << continued.physical_dag_published
                << ",receipt="
                << continued.planning_context.continuation_receipt.has_value();
    }
    std::cerr << '\n';
  }
  bool passed = Require(
      continued.accepted && continued.optimizer_owned &&
          continued.physical_dag_published &&
          continued.cache_admission_allowed && continued.execution_allowed &&
          !continued.data_access_allowed &&
          continued.planning_context.continuation_planning &&
          continued.planning_context.continuation_receipt.has_value() &&
          continued.planning_context.continuation_receipt
              ->physical_root_delivery_validated &&
          continued.planning_context.continuation_receipt->resumable_plan,
      "continuation planning did not enforce and publish resumable properties");

  auto missing_physical_delivery = continued.publication.physical_dag;
  auto continuation_receipt =
      *continued.planning_context.continuation_receipt;
  const auto physical_root = std::ranges::find_if(
      missing_physical_delivery.nodes, [&](const auto& node) {
        return node.physical_node_id ==
               missing_physical_delivery.root_physical_node_id;
      });
  if (physical_root != missing_physical_delivery.nodes.end()) {
    std::erase(physical_root->delivered_property_uuids,
               continuation.required_rewindability_property_uuid);
  }
  passed &= Require(
      !opt::ValidateCanonicalContinuationPhysicalRoot(
          &continuation_receipt, missing_physical_delivery),
      "continuation physical root omitted a resumable delivery");

  auto missing_property = continuation_input;
  missing_property.continuation_context->required_rewindability_property_uuid =
      Uuid(899);
  const auto missing = opt::PlanCanonicalRelationalDag(missing_property);
  passed &= Require(
      !missing.accepted && !missing.planning_context.accepted &&
          !missing.planning_context.issues.empty() &&
          missing.planning_context.issues.front().field_id ==
              "continuation_resumable_properties",
      "missing continuation property did not fail closed before enumeration");

  auto normal_input = PlanningInput();
  const auto normal_before = opt::PlanCanonicalRelationalDag(normal_input);
  auto what_if_input = normal_input;
  opt::CanonicalPlannerWhatIfContext what_if;
  what_if.authority = PlanningContextAuthority(
      830, "advisory_only_no_normal_plan_influence");
  what_if.policy_uuid = Uuid(833);
  what_if.policy_generation = 834;
  what_if.hypotheses = {
      {opt::CanonicalPlannerWhatIfHypothesisKind::kIndex, Uuid(835), 836,
       std::string(64, '1')},
      {opt::CanonicalPlannerWhatIfHypothesisKind::kStatistics, Uuid(837), 838,
       std::string(64, '2')},
      {opt::CanonicalPlannerWhatIfHypothesisKind::kPolicy, Uuid(839), 840,
       std::string(64, '3')},
  };
  what_if.enabled = true;
  what_if_input.what_if_context = what_if;
  const auto advisory = opt::PlanCanonicalRelationalDag(what_if_input);
  const auto normal_after = opt::PlanCanonicalRelationalDag(normal_input);
  passed &= Require(
      normal_before.accepted && advisory.accepted && advisory.optimizer_owned &&
          advisory.complete_logical_dag_covered && advisory.search.accepted &&
          advisory.search.selected && advisory.diagnostics.empty() &&
          !advisory.physical_dag_published &&
          !advisory.cache_admission_allowed && !advisory.execution_allowed &&
          !advisory.data_access_allowed &&
          advisory.planning_context.what_if_planning &&
          advisory.planning_context.what_if_receipt.has_value() &&
          advisory.planning_context.what_if_receipt->advisory_plan_only &&
          advisory.planning_context.what_if_receipt
              ->physical_publication_forbidden &&
          advisory.planning_context.what_if_receipt
              ->cache_admission_forbidden &&
          advisory.planning_context.what_if_receipt->execution_forbidden &&
          normal_after.accepted &&
          normal_before.search.selected_plan_signature ==
              normal_after.search.selected_plan_signature,
      "what-if planning escaped advisory isolation or influenced normal planning");

  auto forged_what_if = what_if_input;
  forged_what_if.what_if_context->normal_plan_influence_permitted = true;
  const auto forged = opt::PlanCanonicalRelationalDag(forged_what_if);
  passed &= Require(
      !forged.accepted && !forged.planning_context.accepted &&
          !forged.planning_context.issues.empty() &&
          forged.planning_context.issues.front().field_id ==
              "what_if_isolation",
      "what-if normal-plan influence carrier was admitted");

  auto coexistent = what_if_input;
  coexistent.continuation_context = continuation;
  const auto refused_coexistence = opt::PlanCanonicalRelationalDag(coexistent);
  passed &= Require(
      !refused_coexistence.accepted &&
          !refused_coexistence.planning_context.issues.empty() &&
          refused_coexistence.planning_context.issues.front().field_id ==
              "continuation_what_if_isolation",
      "continuation and what-if contexts were admitted together");
  return passed;
}

bool ValidateCompleteKindFactoryCoverage() {
  const auto input = CompleteKindPlanningInput();
  const auto result = opt::PlanCanonicalRelationalDag(input);
  bool has_match = false;
  bool has_table_function = false;
  bool complete_cost_vectors = true;
  for (const auto& candidate : result.factory.candidates) {
    has_match |= candidate.logical_node_id == 16 &&
                 candidate.cost_terms.predicate_evaluation_units != 0;
    has_table_function |= candidate.logical_node_id == 15 &&
                          candidate.cost_terms.udr_invocation_units != 0;
    complete_cost_vectors &=
        candidate.cost_terms.complete_dimension_vector;
  }
  if (!input.admission.admitted || !result.accepted ||
      result.factory.inventory.candidate_count != 17 ||
      result.factory.candidates.size() != 17 ||
      result.publication.physical_dag.nodes.size() != 17 || !has_match ||
      !has_table_function || !complete_cost_vectors) {
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
                << ",table_function=" << has_table_function
                << ",complete_cost_vectors=" << complete_cost_vectors;
    }
    std::cerr << '\n';
  }
  return Require(
      input.admission.admitted && result.accepted &&
          result.factory.optimizer_owned_enumeration &&
          result.factory.inventory.candidate_count == 17 &&
          result.factory.candidates.size() == 17 &&
          result.publication.physical_dag.nodes.size() == 17 && has_match &&
          has_table_function && complete_cost_vectors &&
          std::ranges::all_of(result.publication.physical_dag.nodes,
                              [](const auto& node) {
                                return node.retained_cost
                                    .complete_dimension_vector;
                              }),
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
  left.cache_residency_benefit_units = 1;
  left.spill_read_units = 61;
  left.temp_space_pressure_units = 67;
  left.decompression_units = 71;
  left.decryption_units = 73;
  left.expression_evaluation_units = 79;
  left.domain_cast_units = 83;
  left.datatype_conversion_units = 89;
  left.collation_comparison_units = 97;
  left.mga_version_traversal_units = 101;
  left.archive_fetch_units = 103;
  left.garbage_retention_pressure_units = 107;
  left.lock_latch_wait_risk_units = 109;
  left.network_latency_units = 113;
  left.remote_execution_startup_units = 127;
  left.cluster_coordination_units = 131;
  left.repartition_units = 137;
  left.broadcast_units = 139;
  left.replica_staleness_risk_units = 149;
  left.quorum_availability_risk_units = 151;
  left.donor_compatibility_enforcement_units = 157;
  left.result_ordering_enforcement_units = 163;
  left.plan_instability_penalty = 167;
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
  right.expression_evaluation_units = 73;
  right.plan_instability_penalty = 79;
  right.confidence = opt::CostConfidence::kMedium;
  opt::AccumulateCostVector(&left, right);
  const auto json = opt::SerializeCostVectorToJson(left);
  exec::PhysicalCostVectorReceipt valid_physical_cost;
  valid_physical_cost.scalarization_policy_id =
      "canonical.optimizer.complete-unit-sum-minus-cache-benefit.v1";
  valid_physical_cost.cpu_units = 5;
  valid_physical_cost.cache_residency_benefit_units = 2;
  valid_physical_cost.complete_dimension_vector = true;
  std::uint64_t physical_scalar = 0;
  const bool valid_physical_scalar = exec::ComputePhysicalCostVectorScalarScore(
      valid_physical_cost, &physical_scalar);
  const auto valid_physical_scalar_value = physical_scalar;
  auto unknown_policy_cost = valid_physical_cost;
  unknown_policy_cost.scalarization_policy_id = "unknown.policy.v1";
  const bool unknown_policy_refused =
      !exec::ComputePhysicalCostVectorScalarScore(unknown_policy_cost,
                                                  &physical_scalar);
  auto overflowing_cost = valid_physical_cost;
  overflowing_cost.cache_residency_benefit_units = 0;
  overflowing_cost.cpu_units = std::numeric_limits<std::uint64_t>::max();
  overflowing_cost.page_read_sequential_units = 1;
  const bool overflow_refused =
      !exec::ComputePhysicalCostVectorScalarScore(overflowing_cost,
                                                  &physical_scalar);
  return Require(
      initial_cpu == 2 && initial_total != 0 &&
          legacy_updated_cpu == legacy_initial_cpu + 100 &&
          legacy.cpu_units == legacy_updated_cpu && left.cpu_units == 63 &&
          left.random_io_units == 72 && left.page_write_units == 78 &&
          left.expression_evaluation_units == 152 &&
          left.plan_instability_penalty == 246 &&
          left.complete_dimension_vector &&
          left.total_cost > initial_total &&
          valid_physical_scalar && valid_physical_scalar_value == 3 &&
          unknown_policy_refused && overflow_refused &&
          json.find("\"page_write_units\": 78") != std::string::npos &&
          json.find("\"vector_distance_units\": 37") != std::string::npos &&
          json.find("\"mga_units\": 53") != std::string::npos &&
          json.find("\"garbage_retention_pressure_units\": 107") !=
              std::string::npos &&
          json.find("\"result_ordering_enforcement_units\": 163") !=
              std::string::npos &&
          json.find("\"plan_instability_penalty\": 246") !=
              std::string::npos &&
          json.find("\"complete_dimension_vector\": true") !=
              std::string::npos &&
          json.find(
              "\"scalarization_policy_id\": \"optimizer.complete-unit-sum-minus-cache-benefit.v1\"") !=
              std::string::npos,
      "cost dimensions were overwritten, collapsed, or omitted from explain JSON");
}

exec::PhysicalMgaStatementContext ModelFamilyMgaContext(
    const bool timestamp_required) {
  exec::PhysicalMgaStatementContext context;
  context.statement_uuid = Uuid(1400);
  context.owning_transaction_uuid = Uuid(1401);
  context.statement_snapshot_uuid = Uuid(1402);
  context.statement_metadata_snapshot_uuid = Uuid(1403);
  context.owning_local_transaction_id = kOwner;
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
  if (timestamp_required) {
    context.statement_timestamp = "2026-08-24T12:00:00Z";
  }
  return context;
}

bool ValidateModelFamilyOptimizerOwnedProfiles() {
  struct FamilyCase {
    std::string family_id;
    std::vector<std::string> operation_ids;
    std::string operation_id;
    std::string logical_operator_id;
  };
  const std::vector<FamilyCase> families = {
      {"document", {}, "DOCUMENT_FIND", "LOGICAL_DOCUMENT_SOURCE_V1"},
      {"graph", {}, "GRAPH_MATCH", "LOGICAL_GRAPH_SOURCE_V1"},
      {"key_value", {}, "KEY_VALUE_GET", "LOGICAL_KEY_VALUE_SOURCE_V1"},
      {"time_series", {}, "TIME_SERIES_RANGE_READ",
       "LOGICAL_TIME_SERIES_SOURCE_V1"},
      {"vector", {}, "VECTOR_EXACT_SEARCH", "LOGICAL_VECTOR_SOURCE_V1"},
      {"search", {}, "SEARCH_RANKED_QUERY", "LOGICAL_SEARCH_SOURCE_V1"},
      {"spatial", {"SPATIAL_SOURCE"}, "SPATIAL_SOURCE",
       "LOGICAL_SPATIAL_SOURCE_V1"},
      {"columnar", {"COLUMNAR_SOURCE"}, "COLUMNAR_SOURCE",
       "LOGICAL_COLUMNAR_SOURCE_V1"},
  };
  bool passed = true;
  for (std::size_t index = 0; index < families.size(); ++index) {
    const auto& family = families[index];
    opt::ModelFamilyCoordinatorRequestV1 logical;
    logical.family_id = family.family_id;
    logical.operation_ids = family.operation_ids;
    logical.operation_id = family.operation_id;
    logical.logical_operator_id = family.logical_operator_id;
    logical.logical_node_id = static_cast<std::uint32_t>(index + 1);
    logical.object_uuid = Uuid(1500 + index);
    logical.output_descriptor_ids = {
        static_cast<std::uint32_t>(1600 + index)};
    const bool timestamp_required = family.family_id != "document" &&
                                    family.family_id != "graph";
    logical.mga_statement_context =
        ModelFamilyMgaContext(timestamp_required);
    logical.bound_sblr_tree_uuid = Uuid(1700 + index);
    logical.catalog_epoch_uuid = Uuid(1800 + index);
    logical.security_context_uuid = Uuid(1900 + index);
    logical.capability_snapshot_uuid = Uuid(2000 + index);
    logical.resource_snapshot_uuid = Uuid(2100 + index);
    logical.statistics_snapshot_uuid = Uuid(2200 + index);
    logical.route_snapshot_uuid = Uuid(2300 + index);
    logical.catalog_generation = logical.current_catalog_generation = 7;
    logical.security_epoch = 8;
    logical.policy_epoch = 9;
    logical.resource_epoch = 10;
    logical.statistics_generation = 11;
    logical.route_epoch = 12;
    logical.route_generation = 13;
    logical.memory_budget_bytes = 4096;
    logical.security_admitted = true;

    const auto capability = [&](const bool fallback, const bool available,
                                const std::uint64_t identity) {
      opt::ModelFamilyCapabilitySnapshotV1 snapshot;
      snapshot.route_class =
          fallback
              ? opt::ModelFamilyAlternativeRouteClassV1::kExactCollectionFallback
              : opt::ModelFamilyAlternativeRouteClassV1::kNative;
      snapshot.provider_uuid = Uuid(identity);
      snapshot.capability_uuid = Uuid(identity + 1);
      snapshot.provider_generation = 14;
      snapshot.available = available;
      snapshot.metrics.statistics_snapshot_uuid =
          logical.statistics_snapshot_uuid;
      snapshot.metrics.property_snapshot_uuid = Uuid(identity + 2);
      snapshot.metrics.calibration_profile_uuid = Uuid(identity + 3);
      snapshot.metrics.statistics_generation = logical.statistics_generation;
      snapshot.metrics.confidence_basis_points = fallback ? 8000 : 9500;
      snapshot.metrics.startup_events = 1;
      snapshot.metrics.estimated_rows = fallback ? 20 : 2;
      snapshot.metrics.sequential_pages = fallback ? 20 : 1;
      snapshot.metrics.working_set_bytes = fallback ? 1024 : 512;
      snapshot.metrics.memory_grant_units = fallback ? 1024 : 512;
      snapshot.metrics.predicate_evaluations = fallback ? 20 : 2;
      snapshot.metrics.mga_rechecks = fallback ? 20 : 2;
      if (family.family_id == "vector" && !fallback) {
        snapshot.metrics.vector_distance_evaluations = 2;
      }
      if (family.family_id == "search" && !fallback) {
        snapshot.metrics.text_score_evaluations = 2;
      }
      if (family.family_id == "spatial" && !fallback) {
        snapshot.metrics.spatial_evaluations = 2;
      }
      return snapshot;
    };

    opt::ModelFamilyProfileFactoryRequestV1 request;
    request.identity_scope = "public.optimizer.model-family." + family.family_id;
    request.logical_request = logical;
    request.capability_snapshots = {
        capability(false, true, 2400 + index * 20),
        capability(true, true, 2410 + index * 20),
    };
    const auto inventory = opt::BuildModelFamilyAlternativeProfilesV1(request);
    const auto native = opt::PlanOptimizerOwnedModelFamilySourceV1(request);
    auto reordered_request = request;
    std::ranges::reverse(reordered_request.capability_snapshots);
    const auto reordered_inventory =
        opt::BuildModelFamilyAlternativeProfilesV1(reordered_request);
    auto changed_metric_request = request;
    ++changed_metric_request.capability_snapshots.front()
          .metrics.predicate_evaluations;
    const auto changed_metric_inventory =
        opt::BuildModelFamilyAlternativeProfilesV1(changed_metric_request);
    std::uint64_t retained_model_score = 0;
    const bool retained_model_cost_exact =
        native.physical_dag.nodes.size() == 1 &&
        exec::ComputePhysicalCostVectorScalarScore(
            native.physical_dag.nodes.front().retained_cost,
            &retained_model_score) &&
        retained_model_score == native.selected_candidate.cost.scalar_score;
    passed &= Require(
        inventory.accepted && inventory.optimizer_owned_enumeration &&
            !inventory.data_access_allowed && inventory.candidates.size() == 2 &&
            inventory.native_alternative_count == 1 &&
            inventory.exact_fallback_alternative_count == 1 &&
            native.accepted && native.selected &&
            native.optimizer_owned_enumeration &&
            !native.exact_fallback_selected &&
            native.selected_candidate.route_class ==
                opt::ModelFamilyAlternativeRouteClassV1::kNative &&
            native.selected_candidate.cost.vector_distance_units ==
                (family.family_id == "vector" ? 2 : 0) &&
            native.selected_candidate.cost.text_scoring_units ==
                (family.family_id == "search" ? 2 : 0) &&
            native.selected_candidate.cost.spatial_evaluation_units ==
                (family.family_id == "spatial" ? 2 : 0) &&
            native.selected_candidate.cost.mga_units == 2 &&
            native.selected_candidate.cost.memory_grant_units == 512 &&
            native.selected_candidate.cost.memory_allocation_units == 512 &&
            native.selected_candidate.cost.memory_grant_opportunity_units ==
                512 &&
            native.selected_candidate.cost.mga_visibility_check_units == 2 &&
            native.selected_candidate.cost.complete_dimension_vector &&
            native.selected_candidate.cost.scalar_score != 0 &&
            native.physical_dag.nodes.size() == 1 &&
            native.physical_dag.nodes.front()
                .retained_cost.complete_dimension_vector &&
            native.physical_dag.nodes.front()
                    .retained_cost.memory_allocation_units == 512 &&
            native.physical_dag.nodes.front().retained_cost.scalar_score ==
                native.selected_candidate.cost.scalar_score &&
            retained_model_cost_exact &&
            native.selected_candidate.cost.scalarization_policy_id ==
                "model-family.complete-unit-sum-minus-cache-benefit.v1" &&
            native.selected_candidate.cost.calibration_profile_uuid ==
                request.capability_snapshots.front()
                    .metrics.calibration_profile_uuid &&
            native.selected_cost_explain_json.find(
                "\"memory_grant_units\":512") != std::string::npos &&
            native.selected_cost_explain_json.find(
                "\"memory_allocation_units\":512") != std::string::npos &&
            native.selected_cost_explain_json.find(
                "\"complete_dimension_vector\":true") !=
                std::string::npos &&
            native.selected_cost_explain_json.find(
                "\"scalarization_policy_id\":\"model-family.complete-unit-sum-minus-cache-benefit.v1\"") !=
                std::string::npos &&
            native.candidate_inventory_receipt_uuid ==
                inventory.candidate_inventory_receipt_uuid &&
            reordered_inventory.accepted &&
            reordered_inventory.candidate_inventory_receipt_uuid ==
                inventory.candidate_inventory_receipt_uuid &&
            changed_metric_inventory.accepted &&
            changed_metric_inventory.candidate_inventory_receipt_uuid !=
                inventory.candidate_inventory_receipt_uuid,
        "model-family native inventory or full cost vector was not optimizer-owned: " +
            family.family_id);

    auto fallback_request = request;
    fallback_request.identity_scope += ".fallback-only";
    fallback_request.capability_snapshots.erase(
        fallback_request.capability_snapshots.begin());
    const auto fallback =
        opt::PlanOptimizerOwnedModelFamilySourceV1(fallback_request);
    passed &= Require(
        fallback.accepted && fallback.selected &&
            fallback.optimizer_owned_enumeration &&
            fallback.exact_fallback_selected &&
            fallback.selected_candidate.route_class ==
                opt::ModelFamilyAlternativeRouteClassV1::kExactCollectionFallback,
        "model-family exact fallback was absent or mislabeled as native: " +
            family.family_id);

    auto unavailable_request = request;
    unavailable_request.identity_scope += ".unavailable";
    unavailable_request.capability_snapshots.resize(1);
    unavailable_request.capability_snapshots.front().available = false;
    const auto unavailable =
        opt::PlanOptimizerOwnedModelFamilySourceV1(unavailable_request);
    passed &= Require(
        !unavailable.accepted && !unavailable.data_access_allowed &&
            !unavailable.optimizer_owned_enumeration &&
            unavailable.physical_dag.nodes.empty(),
        "model-family unavailable native route silently became executable: " +
            family.family_id);
  }
  return passed;
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

opt::CanonicalPreparePhysicalPlanRequest PreparedRequest(
    const exec::TypedPhysicalNodeDag& dag, const std::uint64_t identity) {
  opt::CanonicalPreparePhysicalPlanRequest request;
  request.prepared_plan_uuid = Uuid(identity);
  request.prepare_generation = identity;
  request.parameter_shape_uuid = Uuid(identity + 1);
  request.result_schema_uuid = Uuid(identity + 2);
  request.selected_physical_dag = dag;
  const auto root = std::ranges::find_if(dag.nodes, [&](const auto& node) {
    return node.physical_node_id == dag.root_physical_node_id;
  });
  if (root != dag.nodes.end()) {
    for (std::size_t index = 0; index < root->output_descriptor_ids.size();
         ++index) {
      opt::CanonicalPreparedPlanResultDescriptor descriptor;
      descriptor.ordinal = static_cast<std::uint32_t>(index + 1);
      descriptor.descriptor_id = root->output_descriptor_ids[index];
      descriptor.name_utf8 = "result_" + std::to_string(index + 1);
      descriptor.descriptor_uuid = Uuid(identity + 10 + index);
      descriptor.type_uuid = Uuid(identity + 30 + index);
      descriptor.type_modifier_digest = std::string(64, 'a');
      request.result_descriptors.push_back(std::move(descriptor));
    }
  }
  request.dependencies = {
      {opt::CanonicalPreparedPlanDependencyKind::kObject, Uuid(identity + 50),
       identity + 50, std::string(64, 'b')},
      {opt::CanonicalPreparedPlanDependencyKind::kDatatype,
       Uuid(identity + 51), identity + 51, std::string(64, 'c')},
  };
  request.engine_prepare_authorized = true;
  return request;
}

opt::CanonicalPrepareMetricLegRequest MetricLeg(
    const std::uint64_t identity, std::string family,
    std::vector<std::string> dependencies,
    std::atomic<std::uint64_t>* collector_calls,
    std::atomic<std::uint64_t>* planner_calls,
    std::atomic<std::uint64_t>* cleanup_calls,
    std::atomic<bool>* dependency_proof = nullptr) {
  opt::CanonicalPrepareMetricLegRequest leg;
  leg.leg_uuid = Uuid(identity);
  leg.family_id = std::move(family);
  leg.dependency_leg_uuids = std::move(dependencies);
  std::ranges::sort(leg.dependency_leg_uuids);
  leg.required_metric_ids =
      opt::CanonicalRequiredPrepareMetricIdsForFamily(leg.family_id);
  leg.collect_metrics = [identity, collector_calls](const auto& context) {
    ++*collector_calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    opt::CanonicalPrepareMetricCollectionOutput output;
    if (context.cancellation_requested()) return output;
    output.collected = true;
    output.metric_snapshot_uuid = Uuid(identity + 100);
    output.metric_snapshot_generation = identity + 100;
    for (std::size_t index = 0;
         index < context.required_metric_ids.size(); ++index) {
      output.metrics.push_back(
          {context.required_metric_ids[index], "canonical_units",
           identity + index, Uuid(identity + 200 + index),
           identity + 200 + index});
    }
    return output;
  };
  leg.plan_leg = [identity, planner_calls, dependency_proof](
                     const auto& context) {
    ++*planner_calls;
    if (dependency_proof != nullptr) {
      dependency_proof->store(
          context.completed_dependency_plans.size() == 2 &&
          std::ranges::all_of(context.completed_dependency_plans,
                              [](const auto& receipt) {
                                return receipt.planned &&
                                       !receipt.planning_receipt_uuid.empty();
                              }));
    }
    opt::CanonicalPrepareLegPlanningOutput output;
    output.planned = true;
    output.selected_leg_plan_uuid = Uuid(identity + 300);
    output.selected_alternative_uuid = Uuid(identity + 301);
    output.family_local_cost_vector_uuid = Uuid(identity + 302);
    output.retained_alternative_uuids = {Uuid(identity + 301),
                                         Uuid(identity + 303)};
    output.estimated_output_rows = identity;
    return output;
  };
  leg.cleanup_transient_state = [cleanup_calls] {
    ++*cleanup_calls;
    return true;
  };
  return leg;
}

opt::CanonicalPrepareWithMetricCollectionRequest MetricPrepareRequest(
    opt::CanonicalPreparePhysicalPlanRequest prepared,
    std::atomic<std::uint64_t>* collector_calls,
    std::atomic<std::uint64_t>* planner_calls,
    std::atomic<std::uint64_t>* cleanup_calls,
    std::atomic<std::uint64_t>* assembler_calls,
    std::atomic<bool>* dependency_proof) {
  opt::CanonicalPrepareWithMetricCollectionRequest request;
  request.coordinator_policy_uuid = Uuid(900);
  request.coordinator_policy_generation = 41;
  request.bound_sblr_tree_uuid =
      prepared.selected_physical_dag.bound_sblr_tree_uuid;
  request.route_snapshot_uuid =
      prepared.selected_physical_dag.route_snapshot_uuid;
  request.route_epoch = prepared.selected_physical_dag.route_epoch;
  request.route_generation =
      prepared.selected_physical_dag.route_generation;
  request.cluster_scope_id = "local_only";
  request.metric_thread_budget = 2;
  request.timeout_ns = 2'000'000'000ULL;
  request.legs = {
      MetricLeg(910, "relational", {}, collector_calls, planner_calls,
                cleanup_calls),
      MetricLeg(920, "vector", {}, collector_calls, planner_calls,
                cleanup_calls),
      MetricLeg(930, "graph", {Uuid(910), Uuid(920)}, collector_calls,
                planner_calls, cleanup_calls, dependency_proof),
  };
  request.assemble_selected_plan =
      [prepared = std::move(prepared), assembler_calls](
          const auto&, const auto& metrics, const auto& plans) mutable {
        ++*assembler_calls;
        if (metrics.size() != 3 || plans.size() != 3) {
          prepared.engine_prepare_authorized = false;
        }
        return prepared;
      };
  request.engine_prepare_authorized = true;
  request.global_security_admitted = true;
  request.global_mga_admitted = true;
  return request;
}

bool ValidatePrepareMetricCollectionOrchestration() {
  const auto planned = opt::PlanCanonicalRelationalDag(PlanningInput());
  if (!planned.accepted) {
    return Require(false, "metric PREPARE fixture could not publish a DAG");
  }
  std::atomic<std::uint64_t> collector_calls{0};
  std::atomic<std::uint64_t> planner_calls{0};
  std::atomic<std::uint64_t> cleanup_calls{0};
  std::atomic<std::uint64_t> assembler_calls{0};
  std::atomic<bool> dependency_proof{false};
  auto request = MetricPrepareRequest(
      PreparedRequest(planned.publication.physical_dag, 1000),
      &collector_calls, &planner_calls, &cleanup_calls, &assembler_calls,
      &dependency_proof);
  opt::CanonicalPreparedPlanStore store;
  const auto first = opt::PrepareCanonicalPhysicalPlanWithMetricCollection(
      request, &store);
  const auto stored = store.Find(Uuid(1000));
  const auto calls_before_execute = collector_calls.load();
  std::vector<opt::CanonicalPreparedPhysicalNode> execute_nodes;
  if (stored) execute_nodes = stored->nodes;
  bool passed = Require(
      first.accepted && first.metrics_collected && first.legs_planned &&
          first.prepared && first.persisted && first.all_workers_joined &&
          first.transient_state_cleaned &&
          first.maximum_observed_concurrency == 2 &&
          first.metric_collector_invocation_count == 3 &&
          first.leg_planner_invocation_count == 3 &&
          first.cleanup_invocation_count == 3 && collector_calls == 3 &&
          planner_calls == 3 && cleanup_calls == 3 && assembler_calls == 1 &&
          dependency_proof && stored &&
          stored->prepare_metric_receipts_retained &&
          stored->prepare_metric_coordinator_receipt.has_value() &&
          stored->prepare_metric_collection_receipts.size() == 3 &&
          stored->prepare_leg_plan_receipts.size() == 3 &&
          stored->prepare_metric_coordinator_receipt
              ->execute_metric_recollection_forbidden &&
          execute_nodes.size() == stored->nodes.size() &&
          collector_calls.load() == calls_before_execute,
      "bounded PREPARE metrics were not retained or EXECUTE recollected them");

  auto replay_request = MetricPrepareRequest(
      PreparedRequest(planned.publication.physical_dag, 1100),
      &collector_calls, &planner_calls, &cleanup_calls, &assembler_calls,
      &dependency_proof);
  opt::CanonicalPreparedPlanStore replay_store;
  const auto replay = opt::PrepareCanonicalPhysicalPlanWithMetricCollection(
      replay_request, &replay_store);
  passed &= Require(
      replay.accepted &&
          replay.metric_receipts.size() == first.metric_receipts.size() &&
          replay.leg_plan_receipts.size() == first.leg_plan_receipts.size() &&
          std::ranges::equal(
              replay.metric_receipts, first.metric_receipts,
              {}, &opt::CanonicalPreparedMetricCollectionReceipt::collection_receipt_uuid,
              &opt::CanonicalPreparedMetricCollectionReceipt::collection_receipt_uuid) &&
          std::ranges::equal(
              replay.leg_plan_receipts, first.leg_plan_receipts,
              {}, &opt::CanonicalPreparedLegPlanReceipt::planning_receipt_uuid,
              &opt::CanonicalPreparedLegPlanReceipt::planning_receipt_uuid),
      "identical PREPARE inputs produced nondeterministic metric receipts");

  std::atomic<bool> cancel{false};
  std::atomic<std::uint64_t> cancelled_cleanup{0};
  auto cancelled_request = MetricPrepareRequest(
      PreparedRequest(planned.publication.physical_dag, 1200),
      &collector_calls, &planner_calls, &cancelled_cleanup, &assembler_calls,
      &dependency_proof);
  cancelled_request.legs.resize(1);
  cancelled_request.legs.front().collect_metrics = [](const auto& context) {
    while (!context.cancellation_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return opt::CanonicalPrepareMetricCollectionOutput{};
  };
  cancelled_request.cancellation_requested = [&] { return cancel.load(); };
  opt::CanonicalPreparedPlanStore cancelled_store;
  std::thread cancel_thread([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    cancel = true;
  });
  const auto cancelled =
      opt::PrepareCanonicalPhysicalPlanWithMetricCollection(
          cancelled_request, &cancelled_store);
  cancel_thread.join();
  passed &= Require(
      !cancelled.accepted && cancelled.cancelled &&
          cancelled.all_workers_joined && cancelled.transient_state_cleaned &&
          cancelled_cleanup == 1 && cancelled_store.Size() == 0,
      "cancelled PREPARE leaked a worker, transient state, or plan");

  std::atomic<std::uint64_t> timeout_cleanup{0};
  auto timeout_request = MetricPrepareRequest(
      PreparedRequest(planned.publication.physical_dag, 1300),
      &collector_calls, &planner_calls, &timeout_cleanup, &assembler_calls,
      &dependency_proof);
  timeout_request.legs.resize(1);
  timeout_request.timeout_ns = 2'000'000;
  timeout_request.legs.front().collect_metrics = [](const auto& context) {
    while (!context.cancellation_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return opt::CanonicalPrepareMetricCollectionOutput{};
  };
  opt::CanonicalPreparedPlanStore timeout_store;
  const auto timed_out = opt::PrepareCanonicalPhysicalPlanWithMetricCollection(
      timeout_request, &timeout_store);
  passed &= Require(
      !timed_out.accepted && timed_out.timed_out &&
          timed_out.all_workers_joined && timed_out.transient_state_cleaned &&
          timeout_cleanup == 1 && timeout_store.Size() == 0,
      "timed-out PREPARE leaked a worker, transient state, or plan");

  auto cleanup_refusal_request = MetricPrepareRequest(
      PreparedRequest(planned.publication.physical_dag, 1350),
      &collector_calls, &planner_calls, &cleanup_calls, &assembler_calls,
      &dependency_proof);
  cleanup_refusal_request.legs.resize(1);
  cleanup_refusal_request.legs.front().cleanup_transient_state = [] {
    return false;
  };
  opt::CanonicalPreparedPlanStore cleanup_refusal_store;
  const auto cleanup_refused =
      opt::PrepareCanonicalPhysicalPlanWithMetricCollection(
          cleanup_refusal_request, &cleanup_refusal_store);
  passed &= Require(
      !cleanup_refused.accepted && cleanup_refused.all_workers_joined &&
          !cleanup_refused.transient_state_cleaned &&
          cleanup_refused.cleanup_invocation_count == 1 &&
          !cleanup_refused.issues.empty() &&
          cleanup_refused.issues.front().field_id ==
              "prepare_metric_cleanup" &&
          cleanup_refusal_store.Size() == 0,
      "failed PREPARE cleanup was accepted or reported complete");

  auto cyclic_request = request;
  cyclic_request.legs[0].dependency_leg_uuids = {Uuid(930)};
  opt::CanonicalPreparedPlanStore cyclic_store;
  const auto cyclic = opt::PrepareCanonicalPhysicalPlanWithMetricCollection(
      cyclic_request, &cyclic_store);
  passed &= Require(
      !cyclic.accepted && !cyclic.issues.empty() &&
          cyclic.issues.front().field_id ==
              "prepare_metric_dependency_chain" &&
          cyclic.metric_collector_invocation_count == 0 &&
          cyclic_store.Size() == 0,
      "cyclic PREPARE dependency chain reached a collector");
  return passed;
}

}  // namespace

int main() {
  const bool passed = ValidateOptimizerOwnedPlanning() &&
                      ValidateContinuationAndWhatIfPlanningContexts() &&
                      ValidateCompleteKindFactoryCoverage() &&
                      ValidateCostVectorBookkeeping() &&
                      ValidateModelFamilyOptimizerOwnedProfiles() &&
                      ValidateCrossJoinSemantics() &&
                      ValidateTypedPhysicalPropertyCarrier() &&
                      ValidatePrepareMetricCollectionOrchestration();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
