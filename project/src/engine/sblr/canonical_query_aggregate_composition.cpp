// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_aggregate_composition.hpp"

#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_filter_registration.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_relational_expression.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;

namespace {

constexpr std::string_view kValuesImplementationId =
    "values.materialize.canonical.v1";
constexpr std::uint64_t kCanonicalAggregateKernelBaseMemoryBytes = 1024;

}  // namespace

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_AGGREGATE_COMPOSITION_AUTHORITY
// Coordinates already-admitted object-free grouped and global aggregates. It
// consumes engine-selected MGA statement context and cannot create a snapshot,
// access storage, or publish transaction finality.

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeGroupedCountSumQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (root == graph.nodes.end()) {
    return result;
  }
  const bool has_having =
      root->node_kind == plan::CanonicalLogicalRelationalNodeKind::kFilter &&
      IsLiveGroupedHavingProfileForComposition(root->semantic_variant_id);
  auto aggregate_root = root;
  if (has_having) {
    if (root->input_logical_node_ids.size() != 1) return result;
    aggregate_root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
      return node.logical_node_id == root->input_logical_node_ids.front();
    });
  }
  const auto profile =
      aggregate_root == graph.nodes.end()
          ? LiveGroupedCountSumProfile{}
          : MatchLiveGroupedCountSumProfileForComposition(
                aggregate_root->semantic_variant_id);
  if (aggregate_root == graph.nodes.end() ||
      aggregate_root->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kAggregate ||
      (!has_having && root != aggregate_root) || !profile.matched) {
    return result;
  }

  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };

  const auto grouping_projection_count =
      profile.projects_grouping_metadata ? profile.key_count + 1 : 0;
  const auto expected_output_count =
      profile.key_count + 2 + grouping_projection_count;
  if (graph.nodes.size() != (has_having ? 3 : 2) ||
      aggregate_root->input_logical_node_ids.size() != 1 ||
      aggregate_root->bound_expression_ids.size() != expected_output_count ||
      aggregate_root->output_descriptor_ids.size() != expected_output_count ||
      (has_having &&
       (root->bound_expression_ids.size() != 1 ||
        root->output_descriptor_ids != aggregate_root->output_descriptor_ids)) ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-PAYLOAD-V1",
                  "grouped COUNT/SUM root shape is not exact");
  }
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               aggregate_root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == aggregate_root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-PAYLOAD-V1",
                  "grouped COUNT/SUM input is not one literal VALUES leaf");
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return refuse(
          "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-PAYLOAD-V1",
          "grouped COUNT/SUM does not admit object or property authority");
    }
  }
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-ADMISSION-V1",
        "live grouped COUNT/SUM execution lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-PAYLOAD-V1",
                  "grouped COUNT/SUM input VALUES: " + input.detail);
  }
  auto prepared_root = PrepareGroupedCountSumRootForComposition(
      request.relational_dag, *aggregate_root, *input_node, input, profile);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-PAYLOAD-V1",
                  prepared_root.detail);
  }
  PreparedGroupedHavingRoot prepared_having;
  if (has_having) {
    prepared_having = PrepareGroupedHavingRootForComposition(
        request.relational_dag, *root, *aggregate_root, *input_node,
        prepared_root);
    if (!prepared_having.ok) {
      return refuse(
          "QOW-DIAG-RELATIONAL-LIVE-GROUPED-HAVING-PAYLOAD-V1",
          prepared_having.detail);
    }
  }

  const auto input_row_count = input.batch.rows.size();
  if (!BindPreparedGroupedComparisonCeilings(
          &prepared_root, input_row_count) ||
      prepared_root.maximum_combined_grouping_key_comparison_count >
          request.optimizer_request.resource.maximum_candidate_count) {
    return refuse(
        "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
        "live grouped COUNT/SUM comparison work exceeds the admitted bound");
  }
  std::uint64_t input_memory = 1;
  std::uint64_t output_row_bound = 0;
  const auto total_memory =
      request.optimizer_request.resource.memory_budget_bytes;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedMultiply(
          std::max<std::uint64_t>(1, input_row_count),
          static_cast<std::uint64_t>(prepared_root.grouping_sets.size()),
          &output_row_bound) ||
      output_row_bound > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live grouped COUNT/SUM memory size overflowed");
  }
  const auto maximum_output_rows =
      static_cast<std::size_t>(output_row_bound);
  const auto filter_memory = total_memory;

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto aggregate_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "grouped-aggregate.capability");
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "grouped-having.capability");
  std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id, std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues, "canonical.values.materialize.v1",
       input_row_count, input_memory, 0, 0},
      {aggregate_root->logical_node_id, "aggregate.registry-grouping-sets.v1",
       aggregate_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kAggregate,
       exec::PhysicalNodeKind::kAggregate, profile.transformation_id,
       output_row_bound, total_memory, 1, 1}};
  profiles.back().runtime_peak_from_callback_batches = true;
  if (has_having) {
    profiles.push_back(
        {root->logical_node_id, "filter.3vl.row.v1", filter_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kFilter,
         exec::PhysicalNodeKind::kFilter,
         root->semantic_variant_id ==
                 "filter.having-not-not-sum-count-or-gt-int64-literals.v1"
             ? "canonical.filter.having-not-not-sum-count-or-gt-int64-literals.v1"
             : root->semantic_variant_id ==
                 "filter.having-not-not-count-sum-or-gt-int64-literals.v1"
             ? "canonical.filter.having-not-not-count-sum-or-gt-int64-literals.v1"
             : root->semantic_variant_id ==
                 "filter.having-not-not-count-sum-and-gt-int64-literals.v1"
             ? "canonical.filter.having-not-not-count-sum-and-gt-int64-literals.v1"
             : root->semantic_variant_id ==
                 "filter.having-not-not-count-gt-int64-literal.v1"
             ? "canonical.filter.having-not-not-count-gt-int64-literal.v1"
             : root->semantic_variant_id ==
                 "filter.having-not-not-sum-gt-int64-literal.v1"
             ? "canonical.filter.having-not-not-sum-gt-int64-literal.v1"
             : root->semantic_variant_id ==
                 "filter.having-not-count-sum-and-gt-int64-literals.v1"
             ? "canonical.filter.having-not-count-sum-and-gt-int64-literals.v1"
             : root->semantic_variant_id ==
                       "filter.having-not-count-sum-or-gt-int64-literals.v1"
                   ? "canonical.filter.having-not-count-sum-or-gt-int64-literals.v1"
             : root->semantic_variant_id ==
                       "filter.having-not-count-gt-int64-literal.v1"
                   ? "canonical.filter.having-not-count-gt-int64-literal.v1"
             : root->semantic_variant_id ==
                 "filter.having-count-sum-and-gt-int64-literals.v1"
             ? "canonical.filter.having-count-sum-and-gt-int64-literals.v1"
             : (root->semantic_variant_id ==
                        "filter.having-count-sum-or-gt-int64-literals.v1"
                    ? "canonical.filter.having-count-sum-or-gt-int64-literals.v1"
                    : (root->semantic_variant_id ==
                               "filter.having-not-sum-gt-int64-literal.v1"
                           ? "canonical.filter.having-not-sum-gt-int64-literal.v1"
                           : "canonical.filter.having-sum-gt-int64-literal.v1")),
         output_row_bound,
         request.optimizer_request.resource.memory_budget_bytes, 1, 1});
    profiles.back().runtime_peak_from_callback_batches = true;
  }
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "grouped aggregate runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles,
      has_having ? "grouped-having.selected-plan"
                 : "grouped-aggregate.selected-plan",
      has_having ? "GROUPED HAVING" : "GROUPED AGGREGATE");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-VALUES-V1",
      "GROUPED AGGREGATE");

  exec::CanonicalPhysicalExecutorRegistration aggregate_registration;
  aggregate_registration.node_kind = exec::PhysicalNodeKind::kAggregate;
  aggregate_registration.implementation_id =
      "aggregate.registry-grouping-sets.v1";
  aggregate_registration.executor_capability_uuid = aggregate_capability_uuid;
  aggregate_registration.executor_capability_abi_version = 1;
  aggregate_registration.engine_owned = true;
  aggregate_registration.accepts_optimizer_publication_v2 = true;
  aggregate_registration.publishes_runtime_observation_v1 = true;
  aggregate_registration.honors_dispatcher_memory_limit_v1 = true;
  aggregate_registration.execute =
      [prepared_root, input_row_count, maximum_output_rows, has_having,
       mga_context = request.context](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            node.input_physical_node_ids.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "grouped COUNT/SUM executor did not receive one typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (input_batch.rows.size() != input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "grouped COUNT/SUM cardinality differs from selected cost";
          return step;
        }
        std::string key_binding_detail;
        if (!RevalidatePreparedGroupedKeyBindings(
                prepared_root, input_batch, &key_binding_detail)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail = std::move(key_binding_detail);
          return step;
        }

        exec::TypedPhysicalNodeDag scoped_grouped_runtime_dag;
        std::size_t callback_memory_bound = 0;
        std::string scope_detail;
        if (!BuildStrictUnaryOperatorLocalPhysicalDag(
                dag, node, input_batch, 8, 128 * 1024,
                &scoped_grouped_runtime_dag, &callback_memory_bound,
                &scope_detail)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SBLR.PLAN_TREE.RESOURCE_LIMIT";
          step.diagnostic.detail =
              "grouped aggregate " + scope_detail;
          return step;
        }
        const exec::TypedPhysicalNodeDag* grouped_runtime_dag =
            &scoped_grouped_runtime_dag;
        const auto runtime_node = std::ranges::find_if(
            scoped_grouped_runtime_dag.nodes,
            [&](const auto& candidate) {
              return candidate.physical_node_id == node.physical_node_id;
            });
        if (runtime_node == scoped_grouped_runtime_dag.nodes.end()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "grouped aggregate execution view is unresolved";
          return step;
        }
        if (!prepared_root.grouping_projection_columns.empty()) {
          if (runtime_node->output_descriptor_ids.size() !=
                  prepared_root.key_terms.size() + 2 +
                      prepared_root.grouping_projection_columns.size()) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "grouping projection physical descriptor shape drifted";
            return step;
          }
          runtime_node->output_descriptor_ids.resize(
              prepared_root.key_terms.size() + 2);
        }
        const auto aggregate_memory_bound =
            SelectedNodeAggregateMemoryBound(*grouped_runtime_dag,
                                             *runtime_node);
        if (!aggregate_memory_bound.has_value() ||
            *aggregate_memory_bound != callback_memory_bound) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "grouped aggregate finalization memory grant is unresolved";
          return step;
        }
        const auto make_aggregate = [aggregate_memory_bound](
                                        const PreparedGlobalAggregateRoot& prepared) {
          exec::CanonicalAggregateRuntimeRequest aggregate;
          aggregate.descriptor = prepared.aggregate_descriptor;
          aggregate.value_columns = prepared.value_columns;
          aggregate.value_expression_descriptor_ids =
              prepared.value_descriptor_ids;
          aggregate.direct_arguments = prepared.direct_arguments;
          aggregate.result_column = prepared.result_column;
          aggregate.distinct = prepared.distinct;
          aggregate.aggregate_order_terms = prepared.aggregate_order_terms;
          aggregate.aggregate_separator = prepared.aggregate_separator;
          aggregate.listagg_overflow_mode = prepared.listagg_overflow_mode;
          aggregate.listagg_max_output_bytes =
              prepared.listagg_max_output_bytes;
          aggregate.listagg_truncation_indicator =
              prepared.listagg_truncation_indicator;
          aggregate.listagg_with_count = prepared.listagg_with_count;
          aggregate.forced_strategy =
              exec::CanonicalAggregateExecutionStrategy::serial;
          aggregate.maximum_final_output_bytes = *aggregate_memory_bound;
          aggregate.maximum_finalization_workspace_bytes =
              *aggregate_memory_bound;
          return aggregate;
        };

        exec::CanonicalGroupedAggregateSetRuntimeRequest grouped_request;
        auto& first = grouped_request.first_aggregate;
        first.aggregate_request = make_aggregate(prepared_root.count);
        first.aggregate_request.selected_physical_node_id =
            node.physical_node_id;
        first.aggregate_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context,
                                                *grouped_runtime_dag);
        first.group_key_terms = prepared_root.key_terms;
        first.group_result_columns = prepared_root.key_result_columns;
        first.grouping_sets = prepared_root.grouping_sets;
        first.maximum_grouping_key_comparison_count =
            prepared_root.maximum_grouping_key_comparison_count;
        first.maximum_group_count = maximum_output_rows;
        first.maximum_output_rows = maximum_output_rows;
        first.maximum_combined_final_output_bytes =
            *aggregate_memory_bound;
        auto additional_sum = make_aggregate(prepared_root.sum);
        // Grouped-set runtimes deliberately share one borrowed execution DAG,
        // selected node, and input batch. The additional specification still
        // carries the same statement authority without shadow carriers.
        additional_sum.mga_authority = first.aggregate_request.mga_authority;
        grouped_request.additional_aggregates = {std::move(additional_sum)};
        grouped_request.maximum_combined_grouping_key_comparison_count =
            prepared_root.maximum_combined_grouping_key_comparison_count;
        grouped_request.maximum_combined_final_output_bytes =
            *aggregate_memory_bound;

        auto aggregate_result =
            exec::ExecuteCanonicalGroupedAggregateSetRuntime(
                grouped_request, *grouped_runtime_dag, input_batch);
        if (!aggregate_result.diagnostic.ok) {
          step.diagnostic = std::move(aggregate_result.diagnostic);
          return step;
        }
        if (!CanonicalOperatorExecutionReceiptMatches(
                aggregate_result, *grouped_runtime_dag, node,
                first.aggregate_request.mga_authority.statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-EXECUTION-V1";
          step.diagnostic.detail =
              "object-free grouped COUNT/SUM execution receipt changed";
          return step;
        }
        std::uint64_t input_memory_bytes = 0;
        std::uint64_t unprojected_output_memory_bytes = 0;
        std::uint64_t peak_memory_bytes =
            aggregate_result.peak_memory_bytes;
        if (!RuntimeMaterializedBatchMemoryBytes(input_batch,
                                                 &input_memory_bytes) ||
            !RuntimeMaterializedBatchMemoryBytes(
                aggregate_result.output_batch,
                &unprojected_output_memory_bytes) ||
            aggregate_result.input_payload_bytes != input_memory_bytes ||
            aggregate_result.fixed_retained_memory_bytes !=
                input_memory_bytes ||
            aggregate_result.output_payload_bytes !=
                unprojected_output_memory_bytes ||
            aggregate_result.current_memory_bytes !=
                unprojected_output_memory_bytes ||
            aggregate_result.current_memory_bytes >
                aggregate_result.peak_memory_bytes ||
            aggregate_result.peak_memory_bytes > *aggregate_memory_bound) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail =
              "grouped aggregate-set runtime memory receipt is inconsistent";
          return step;
        }
        if (!aggregate_result.group_identity_proven ||
            !aggregate_result.shared_state_authority_used ||
            aggregate_result.aggregate_count != 2 ||
            aggregate_result.groups.size() !=
                aggregate_result.output_batch.rows.size() ||
            aggregate_result.output_batch.rows.size() >
                maximum_output_rows) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "grouped COUNT/SUM runtime did not prove shared group identity";
          return step;
        }
        std::uint64_t observed_set_memory_bytes = 0;
        std::uint64_t expected_indicator_memory_bytes = 0;
        std::uint64_t validation_phase_memory_bytes = 0;
        if (!LogicalBitVectorPayloadBytes(
                prepared_root.grouping_sets.size(),
                &observed_set_memory_bytes) ||
            !LogicalBitVectorPayloadBytes(
                prepared_root.key_terms.size(),
                &expected_indicator_memory_bytes) ||
            !CheckedAdd(aggregate_result.fixed_retained_memory_bytes,
                        unprojected_output_memory_bytes,
                        &validation_phase_memory_bytes) ||
            !CheckedAdd(validation_phase_memory_bytes,
                        observed_set_memory_bytes,
                        &validation_phase_memory_bytes) ||
            !CheckedAdd(validation_phase_memory_bytes,
                        expected_indicator_memory_bytes,
                        &validation_phase_memory_bytes) ||
            validation_phase_memory_bytes > *aggregate_memory_bound) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail =
              "grouped aggregate validation memory exceeded its grant";
          return step;
        }
        peak_memory_bytes =
            std::max(peak_memory_bytes, validation_phase_memory_bytes);
        {
          std::vector<bool> grouping_sets_observed(
              prepared_root.grouping_sets.size(), false);
          for (const auto& group : aggregate_result.groups) {
            if (group.grouping_set_ordinal >=
                    prepared_root.grouping_sets.size() ||
                group.grouping_indicators.size() !=
                    prepared_root.key_terms.size()) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
              step.diagnostic.detail =
                  "grouped COUNT/SUM runtime returned invalid grouping metadata";
              return step;
            }
            grouping_sets_observed[group.grouping_set_ordinal] = true;
            const auto& grouping_set = prepared_root.grouping_sets[
                group.grouping_set_ordinal];
            const auto expected_metadata =
                exec::ComputeCanonicalAggregateGroupingMetadata(
                    prepared_root.key_terms.size(), grouping_set);
            if (!expected_metadata.diagnostic.ok ||
                group.grouping_indicators !=
                    expected_metadata.grouping_indicators ||
                group.grouping_id != expected_metadata.grouping_id) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
              step.diagnostic.detail =
                  "grouped COUNT/SUM grouping metadata identity drifted";
              return step;
            }
          }
          std::size_t empty_grouping_set_count = 0;
          for (std::size_t set_ordinal = 0;
               set_ordinal < prepared_root.grouping_sets.size();
               ++set_ordinal) {
            const bool empty_grouping_set =
                prepared_root.grouping_sets[set_ordinal]
                    .key_term_ordinals.empty();
            const bool expected_observed =
                !input_batch.rows.empty() || empty_grouping_set;
            if (empty_grouping_set) ++empty_grouping_set_count;
            if (grouping_sets_observed[set_ordinal] !=
                expected_observed) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
              step.diagnostic.detail =
                  "grouped COUNT/SUM grouping-set cardinality drifted";
              return step;
            }
          }
          if (input_batch.rows.empty() &&
              aggregate_result.groups.size() !=
                  empty_grouping_set_count) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "empty-input grouped COUNT/SUM cardinality drifted";
            return step;
          }
        }
        std::uint64_t expected_output_memory_bytes =
            unprojected_output_memory_bytes;
        if (!prepared_root.grouping_projection_columns.empty()) {
          if (prepared_root.grouping_projection_columns.size() !=
                  prepared_root.key_terms.size() + 1 ||
              aggregate_result.output_batch.columns.size() !=
                  prepared_root.key_terms.size() + 2) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "grouping projection result descriptor shape drifted";
            return step;
          }
          for (const auto& metadata : aggregate_result.groups) {
            if (metadata.grouping_id >
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()) ||
                !CheckedAdd(expected_output_memory_bytes,
                            prepared_root.key_terms.size(),
                            &expected_output_memory_bytes) ||
                !CheckedAdd(
                    expected_output_memory_bytes,
                    CanonicalUnsignedDecimalWidth(metadata.grouping_id),
                    &expected_output_memory_bytes)) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
              step.diagnostic.detail =
                  "GROUPING projection payload is invalid";
              return step;
            }
          }
          std::uint64_t prospective_projection_phase_memory_bytes = 0;
          if (!CheckedAdd(aggregate_result.fixed_retained_memory_bytes,
                          expected_output_memory_bytes,
                          &prospective_projection_phase_memory_bytes) ||
              prospective_projection_phase_memory_bytes >
                  *aggregate_memory_bound) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-OPT-017-REFUSAL-V1";
            step.diagnostic.detail =
                "grouped aggregate projection exceeded its memory grant";
            return step;
          }
          peak_memory_bytes = std::max(
              peak_memory_bytes,
              prospective_projection_phase_memory_bytes);
          aggregate_result.output_batch.columns.insert(
              aggregate_result.output_batch.columns.end(),
              prepared_root.grouping_projection_columns.begin(),
              prepared_root.grouping_projection_columns.end());
          for (std::size_t group_ordinal = 0;
               group_ordinal < aggregate_result.groups.size();
               ++group_ordinal) {
            auto& output_row =
                aggregate_result.output_batch.rows[group_ordinal];
            const auto& metadata = aggregate_result.groups[group_ordinal];
            for (std::size_t key_ordinal = 0;
                 key_ordinal < prepared_root.key_terms.size(); ++key_ordinal) {
              api::EngineTypedValue indicator;
              indicator.descriptor =
                  prepared_root.grouping_projection_columns[key_ordinal]
                      .descriptor;
              indicator.encoded_value =
                  metadata.grouping_indicators[key_ordinal] ? "1" : "0";
              indicator.state = api::EngineValueState::value;
              output_row.values.push_back(std::move(indicator));
            }
            api::EngineTypedValue grouping_id;
            grouping_id.descriptor =
                prepared_root.grouping_projection_columns.back().descriptor;
            grouping_id.encoded_value =
                std::to_string(metadata.grouping_id);
            grouping_id.state = api::EngineValueState::value;
            output_row.values.push_back(std::move(grouping_id));
          }
          const auto projected_validation =
              exec::ValidateCanonicalDescriptorBatch(
                  aggregate_result.output_batch, node.output_descriptor_ids);
          if (!projected_validation.ok) {
            step.diagnostic = projected_validation;
            return step;
          }
        }
        std::uint64_t output_memory_bytes = 0;
        std::uint64_t projection_phase_memory_bytes = 0;
        if (!RuntimeMaterializedBatchMemoryBytes(
                aggregate_result.output_batch, &output_memory_bytes) ||
            output_memory_bytes != expected_output_memory_bytes ||
            !CheckedAdd(aggregate_result.fixed_retained_memory_bytes,
                        output_memory_bytes,
                        &projection_phase_memory_bytes)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail =
              "grouped aggregate projection memory receipt overflowed";
          return step;
        }
        peak_memory_bytes =
            std::max(peak_memory_bytes, projection_phase_memory_bytes);
        if (output_memory_bytes > peak_memory_bytes ||
            peak_memory_bytes > *aggregate_memory_bound) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail =
              "grouped aggregate projection exceeded its memory grant";
          return step;
        }
        step.authority = aggregate_result.authority;
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = aggregate_result.output_batch.rows.size();
        step.materialized_output_batch =
            std::move(aggregate_result.output_batch);
        PublishRuntimeMemoryObservation(
            &step, output_memory_bytes, peak_memory_bytes);
        step.mga_statement_context =
            std::move(aggregate_result.mga_statement_context);
        return step;
      };

  auto execution_mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  exec::CanonicalPhysicalExecutorRegistration having_registration;
  if (has_having) {
    having_registration = MakeLiveHeapFilterRegistration(
        prepared_having.predicate_expression_id,
        std::move(prepared_having.row_binding), {},
        request.expression_services, filter_capability_uuid,
        maximum_output_rows, {},
        api::EngineCanonicalExpressionConsumer::aggregate,
        api::EnginePredicateConsumer::having, &request.relational_dag,
        &request.context, &execution_mga_authority);
  }

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority = execution_mga_authority;
  execution_request.selected_physical_dag =
      std::move(planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(aggregate_registration));
  if (has_having) {
    execution_request.available_executors.push_back(
        std::move(having_registration));
  }
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          has_having ? "grouped-having.execution-attempt"
                     : "grouped-aggregate.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          has_having ? "grouped-having.transaction-effect-unchanged"
                     : "grouped-aggregate.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      maximum_output_rows;

  const auto execution =
      ExecuteSelectedCanonicalObjectFreeDag(
          request.context, execution_request,
          planning.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live grouped COUNT/SUM selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeGlobalAggregateQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kAggregate ||
      root->input_logical_node_ids.size() != 1 ||
      root->bound_expression_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto unary_aggregate_profile =
      MatchLiveUnaryAggregateExpressionProfileForComposition(root->semantic_variant_id);
  const bool count_star = unary_aggregate_profile.count_star;
  const bool sum_expression =
      unary_aggregate_profile.matched &&
      unary_aggregate_profile.function ==
          exec::CanonicalAggregateFunction::sum;
  const bool avg_expression =
      unary_aggregate_profile.matched &&
      unary_aggregate_profile.function ==
          exec::CanonicalAggregateFunction::avg;
  const bool min_expression =
      unary_aggregate_profile.matched &&
      unary_aggregate_profile.function ==
          exec::CanonicalAggregateFunction::min;
  const bool max_expression =
      unary_aggregate_profile.matched &&
      unary_aggregate_profile.function ==
          exec::CanonicalAggregateFunction::max;
  const bool bool_and_expression =
      unary_aggregate_profile.matched &&
      unary_aggregate_profile.function ==
          exec::CanonicalAggregateFunction::bool_and;
  const bool bool_or_expression =
      unary_aggregate_profile.matched &&
      unary_aggregate_profile.function ==
          exec::CanonicalAggregateFunction::bool_or;
  const bool every_expression =
      unary_aggregate_profile.matched &&
      unary_aggregate_profile.function ==
          exec::CanonicalAggregateFunction::every;
  const auto string_aggregate_profile =
      MatchLiveStringAggregateExpressionProfileForComposition(root->semantic_variant_id);
  const bool unordered_string_agg_expression =
      string_aggregate_profile.matched && !string_aggregate_profile.ordered;
  const bool ordered_string_agg_expression =
      string_aggregate_profile.matched && string_aggregate_profile.ordered;
  const bool string_agg_expression =
      unordered_string_agg_expression || ordered_string_agg_expression;
  const auto listagg_profile =
      MatchLiveListaggExpressionProfileForComposition(root->semantic_variant_id);
  const bool listagg_expression = listagg_profile.matched;
  const auto ordered_single_collection_profile =
      MatchLiveOrderedSingleCollectionExpressionProfileForComposition(
          root->semantic_variant_id);
  const bool array_agg_expression =
      ordered_single_collection_profile.matched &&
      ordered_single_collection_profile.function ==
          exec::CanonicalAggregateFunction::array_agg;
  const bool json_agg_expression =
      ordered_single_collection_profile.matched &&
      ordered_single_collection_profile.function ==
          exec::CanonicalAggregateFunction::json_agg;
  const auto json_object_aggregate_profile =
      MatchLiveJsonObjectAggregateExpressionProfileForComposition(
          root->semantic_variant_id);
  const bool json_object_agg_expression =
      json_object_aggregate_profile.matched;
  const bool ordered_single_collection_expression =
      array_agg_expression || json_agg_expression;
  const bool ordered_collection_expression =
      ordered_single_collection_expression || json_object_agg_expression;
  const bool statistical_expression =
      unary_aggregate_profile.matched &&
      (unary_aggregate_profile.function ==
           exec::CanonicalAggregateFunction::stddev_pop ||
       unary_aggregate_profile.function ==
           exec::CanonicalAggregateFunction::variance_pop ||
       unary_aggregate_profile.function ==
           exec::CanonicalAggregateFunction::stddev ||
       unary_aggregate_profile.function ==
           exec::CanonicalAggregateFunction::variance ||
       unary_aggregate_profile.function ==
           exec::CanonicalAggregateFunction::stddev_samp ||
       unary_aggregate_profile.function ==
           exec::CanonicalAggregateFunction::variance_samp);
  const auto pair_statistical_profile =
      MatchLivePairStatisticalExpressionProfileForComposition(root->semantic_variant_id);
  const bool pair_statistical_expression = pair_statistical_profile.matched;
  const auto ordered_set_profile =
      MatchLiveOrderedSetExpressionProfileForComposition(root->semantic_variant_id);
  const bool ordered_set_expression = ordered_set_profile.matched;
  const auto approximate_profile =
      MatchLiveApproximateExpressionProfileForComposition(root->semantic_variant_id);
  const bool approximate_expression = approximate_profile.matched;
  if (!unary_aggregate_profile.matched &&
      !string_agg_expression && !listagg_expression &&
      !ordered_collection_expression &&
      !statistical_expression &&
      !pair_statistical_expression && !ordered_set_expression &&
      !approximate_expression) {
    return result;
  }
  auto aggregate_function = unary_aggregate_profile.matched
                                ? unary_aggregate_profile.function
                                : exec::CanonicalAggregateFunction::count;
  if (string_agg_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::string_agg;
  }
  if (listagg_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::listagg;
  }
  if (array_agg_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::array_agg;
  }
  if (json_agg_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::json_agg;
  }
  if (json_object_agg_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::json_object_agg;
  }
  if (pair_statistical_expression) {
    aggregate_function = pair_statistical_profile.function;
  }
  if (ordered_set_expression) {
    aggregate_function = ordered_set_profile.function;
  }
  if (approximate_expression) {
    aggregate_function = approximate_profile.function;
  }
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-ADMISSION-V1",
                  "live global aggregate execution lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1",
                  "global aggregate input VALUES: " + input.detail);
  }
  auto prepared_root = PrepareGlobalAggregateRootForComposition(
      request.relational_dag, *root, *input_node, input, aggregate_function,
      count_star,
      unary_aggregate_profile.distinct || pair_statistical_profile.distinct ||
          string_aggregate_profile.distinct ||
          ordered_single_collection_profile.distinct ||
          json_object_aggregate_profile.distinct || listagg_profile.distinct ||
          ordered_set_profile.distinct || approximate_profile.distinct,
      unary_aggregate_profile.has_filter ||
          pair_statistical_profile.has_filter ||
          string_aggregate_profile.has_filter ||
          ordered_single_collection_profile.has_filter ||
          json_object_aggregate_profile.has_filter ||
          listagg_profile.has_filter || ordered_set_profile.has_filter ||
          approximate_profile.has_filter,
      0, false);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1",
                  prepared_root.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  if (count_star &&
      input_row_count >
          static_cast<std::size_t>(
              std::numeric_limits<std::int64_t>::max())) {
    return refuse("QOW-DIAG-QRY-007-AGGREGATE-OVERFLOW-V1",
                  "COUNT(*) exceeds int64 result width");
  }
  std::uint64_t input_memory = 1;
  std::uint64_t total_memory = 0;
  // Signed text width plus the mandatory runtime batch base byte.
  constexpr std::uint64_t kIntegerAggregateResultMemory =
      std::numeric_limits<std::int64_t>::digits10 + 3;
  constexpr std::uint64_t kRealAggregateResultMemory = 64;
  // "false" plus the mandatory runtime batch base byte.
  constexpr std::uint64_t kBooleanAggregateResultMemory = 6;
  const bool pair_real_result =
      pair_statistical_expression &&
      aggregate_function != exec::CanonicalAggregateFunction::regr_count;
  const bool ordered_set_real_result =
      aggregate_function == exec::CanonicalAggregateFunction::percent_rank ||
      aggregate_function == exec::CanonicalAggregateFunction::cume_dist ||
      aggregate_function == exec::CanonicalAggregateFunction::percentile_cont ||
      aggregate_function == exec::CanonicalAggregateFunction::percentile_disc;
  const bool approximate_real_result =
      aggregate_function == exec::CanonicalAggregateFunction::approx_median ||
      aggregate_function ==
          exec::CanonicalAggregateFunction::approx_percentile_cont ||
      aggregate_function ==
          exec::CanonicalAggregateFunction::approx_percentile_disc;
  std::uint64_t aggregate_result_memory =
      count_star
          ? 1
          : ((avg_expression || statistical_expression || pair_real_result ||
              ordered_set_real_result || approximate_real_result)
                 ? kRealAggregateResultMemory
                 : ((bool_and_expression || bool_or_expression ||
                     every_expression)
                        ? kBooleanAggregateResultMemory
                        : kIntegerAggregateResultMemory));
  if (count_star) {
    auto remaining_count = input_row_count;
    do {
      if (!CheckedAdd(aggregate_result_memory, 1,
                      &aggregate_result_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live COUNT(*) result size overflowed");
      }
      remaining_count /= 10;
    } while (remaining_count != 0);
  }
  if (!AddBatchMemoryBytes(input.batch, &input_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live global aggregate input size overflowed");
  }
  if (string_agg_expression || listagg_expression) {
    std::uint64_t separator_memory = 0;
    aggregate_result_memory = input_memory;
    if (!CheckedMultiply(
            static_cast<std::uint64_t>(input_row_count),
            static_cast<std::uint64_t>(
                prepared_root.aggregate_separator.size()),
            &separator_memory) ||
        !CheckedAdd(aggregate_result_memory, separator_memory,
                    &aggregate_result_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live STRING_AGG/LISTAGG result size overflowed");
    }
    if (ordered_string_agg_expression || listagg_expression) {
      std::uint64_t row_overhead_memory = 0;
      if (!CheckedMultiply(static_cast<std::uint64_t>(input_row_count), 64U,
                           &row_overhead_memory) ||
          !CheckedAdd(aggregate_result_memory, row_overhead_memory,
                      &aggregate_result_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live ordered STRING_AGG/LISTAGG state size overflowed");
      }
    }
  }
  std::uint64_t filter_truth_memory_bytes = 0;
  std::size_t admitted_input_row_count = input_row_count;
  std::uint64_t aggregate_workspace_memory = 0;
  std::uint64_t distinct_peak_memory_bytes = 0;
  {
    std::optional<std::vector<api::EngineSqlTruthValue>>
        planning_filter_truth_values;
    if (prepared_root.filter_column.has_value()) {
      if (input_memory >
          request.optimizer_request.resource.memory_budget_bytes) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                      "live aggregate FILTER input exceeds its memory bound");
      }
      std::vector<api::EngineSqlTruthValue> materialized_filter_truth_values;
      std::string filter_detail;
      if (!MaterializeAggregateFilterTruthValues(
              input.batch, *prepared_root.filter_column,
              prepared_root.filter_descriptor_id,
              request.optimizer_request.resource.memory_budget_bytes -
                  input_memory,
              &materialized_filter_truth_values, &filter_truth_memory_bytes,
              &filter_detail)) {
        return refuse(
            "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1",
            std::move(filter_detail));
      }
      planning_filter_truth_values =
          std::move(materialized_filter_truth_values);
    }
    admitted_input_row_count =
        planning_filter_truth_values.has_value()
            ? static_cast<std::size_t>(std::ranges::count(
                  *planning_filter_truth_values,
                  api::EngineSqlTruthValue::true_value))
            : input_row_count;
    std::vector<std::size_t> planning_transition_ordinals;
    if (!count_star) {
      try {
        planning_transition_ordinals.reserve(admitted_input_row_count);
      } catch (const std::bad_alloc&) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
            "live aggregate transition workspace allocation was refused");
      } catch (const std::length_error&) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
            "live aggregate transition workspace capacity overflowed");
      }
    }
    if (!CheckedMultiply(planning_transition_ordinals.capacity(),
                         sizeof(std::size_t),
                         &aggregate_workspace_memory) ||
        input_memory >
            request.optimizer_request.resource.memory_budget_bytes ||
        filter_truth_memory_bytes >
            request.optimizer_request.resource.memory_budget_bytes -
                input_memory ||
        aggregate_workspace_memory >
            request.optimizer_request.resource.memory_budget_bytes -
                input_memory - filter_truth_memory_bytes) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
          "live aggregate transition workspace exceeds its memory bound");
    }
    if (prepared_root.distinct) {
      std::uint64_t distinct_generation_bound = 0;
      std::uint64_t distinct_comparison_bound = 0;
      if (!CheckedMultiply(admitted_input_row_count,
                           prepared_root.value_columns.size(),
                           &distinct_generation_bound) ||
          (admitted_input_row_count > 1 &&
           (!CheckedMultiply(admitted_input_row_count,
                             admitted_input_row_count - 1,
                             &distinct_comparison_bound)))) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
            "live aggregate DISTINCT work or memory bound overflowed");
      }
      distinct_comparison_bound /= 2;
      const auto distinct_memory_bound =
          request.optimizer_request.resource.memory_budget_bytes -
          input_memory - filter_truth_memory_bytes -
          aggregate_workspace_memory;
      std::string distinct_detail;
      if (!MeasureAggregateDistinctPeakMemoryForComposition(
              request.context, input.batch, prepared_root,
              planning_filter_truth_values, admitted_input_row_count,
              distinct_generation_bound, distinct_comparison_bound,
              distinct_memory_bound, &distinct_peak_memory_bytes,
              &distinct_detail)) {
        return refuse(
            "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1",
            "live aggregate DISTINCT: " + distinct_detail);
      }
    }
  }
  if (ordered_collection_expression) {
    std::uint64_t expanded_input_memory = 0;
    std::uint64_t row_overhead_memory = 0;
    const std::uint64_t input_expansion =
        (json_agg_expression || json_object_agg_expression) ? 6U : 1U;
    if (!CheckedMultiply(input_memory, input_expansion,
                         &expanded_input_memory) ||
        !CheckedMultiply(static_cast<std::uint64_t>(input_row_count), 64U,
                         &row_overhead_memory) ||
        !CheckedAdd(expanded_input_memory, row_overhead_memory,
                    &aggregate_result_memory) ||
        !CheckedAdd(aggregate_result_memory, 2U,
                    &aggregate_result_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live ordered collection aggregate result size "
                    "overflowed");
    }
  }
  if (ordered_set_expression) {
    std::uint64_t ordered_state_memory = 0;
    std::uint64_t frequency_payload_memory = 0;
    if (!CheckedMultiply(static_cast<std::uint64_t>(input_row_count), 64U,
                         &ordered_state_memory) ||
        (aggregate_function == exec::CanonicalAggregateFunction::mode &&
         (!CheckedMultiply(input_memory, 2U,
                           &frequency_payload_memory) ||
          !CheckedAdd(ordered_state_memory, frequency_payload_memory,
                      &ordered_state_memory))) ||
        !CheckedAdd(aggregate_result_memory, ordered_state_memory,
                    &aggregate_result_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live ordered-set aggregate state size overflowed");
    }
  }
  if (approximate_expression) {
    std::uint64_t approximate_state_memory = 0;
    std::uint64_t approximate_payload_memory = input_memory;
    std::uint64_t row_overhead_memory = 0;
    if (((aggregate_function ==
              exec::CanonicalAggregateFunction::approx_count_distinct ||
          aggregate_function ==
              exec::CanonicalAggregateFunction::approx_top_k) &&
         !CheckedMultiply(input_memory, 2U,
                          &approximate_payload_memory)) ||
        !CheckedMultiply(static_cast<std::uint64_t>(input_row_count), 64U,
                         &row_overhead_memory) ||
        !CheckedAdd(approximate_payload_memory, row_overhead_memory,
                    &approximate_state_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live approximate aggregate state size overflowed");
    }
    if (aggregate_function ==
        exec::CanonicalAggregateFunction::approx_top_k) {
      std::uint64_t rendered_value_memory = 0;
      if (!CheckedMultiply(input_memory, 6U, &rendered_value_memory) ||
          !CheckedAdd(rendered_value_memory, row_overhead_memory,
                      &rendered_value_memory) ||
          !CheckedAdd(rendered_value_memory, 2U,
                      &aggregate_result_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live approximate top-k result size overflowed");
      }
    }
    if (!CheckedAdd(aggregate_result_memory, approximate_state_memory,
                    &aggregate_result_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live approximate aggregate result size overflowed");
    }
  }
  if (!CheckedAdd(
          aggregate_result_memory,
          count_star ? sizeof(std::int64_t)
                     : kCanonicalAggregateKernelBaseMemoryBytes,
          &aggregate_result_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live aggregate result or state size overflowed");
  }
  const auto aggregate_phase_memory =
      std::max(aggregate_result_memory, distinct_peak_memory_bytes);
  if (!CheckedAdd(input_memory, filter_truth_memory_bytes, &total_memory) ||
      !CheckedAdd(total_memory, aggregate_workspace_memory, &total_memory) ||
      !CheckedAdd(total_memory, aggregate_phase_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live global aggregate input or result size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live global aggregate exceeds the admitted memory budget");
  }

  const std::string aggregate_implementation_id =
      count_star ? "aggregate.count-star.v1" : "aggregate.registry-core.v1";
  std::string aggregate_transformation_id;
  if (unary_aggregate_profile.matched) {
    aggregate_transformation_id = unary_aggregate_profile.transformation_id;
  } else if (string_agg_expression) {
    aggregate_transformation_id =
        string_aggregate_profile.transformation_id;
  } else if (listagg_expression) {
    aggregate_transformation_id = listagg_profile.transformation_id;
  } else if (array_agg_expression) {
    aggregate_transformation_id =
        ordered_single_collection_profile.transformation_id;
  } else if (json_agg_expression) {
    aggregate_transformation_id =
        ordered_single_collection_profile.transformation_id;
  } else if (json_object_agg_expression) {
    aggregate_transformation_id =
        json_object_aggregate_profile.transformation_id;
  } else if (pair_statistical_expression) {
    aggregate_transformation_id = pair_statistical_profile.transformation_id;
  } else if (ordered_set_expression) {
    aggregate_transformation_id = ordered_set_profile.transformation_id;
  } else if (approximate_expression) {
    aggregate_transformation_id = approximate_profile.transformation_id;
  } else {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1",
                  "live global aggregate transformation is unresolved");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto aggregate_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "aggregate.capability");
  std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       aggregate_implementation_id,
       aggregate_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kAggregate,
       exec::PhysicalNodeKind::kAggregate,
       aggregate_transformation_id,
       input_row_count,
       total_memory,
       1,
       1}};
  if (aggregate_implementation_id == "aggregate.registry-core.v1" &&
      !CheckedAdd(filter_truth_memory_bytes, aggregate_workspace_memory,
                  &profiles.back()
                       .runtime_accounted_auxiliary_memory_bytes)) {
    return refuse(
        "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
        "live aggregate runtime auxiliary receipt overflowed");
  }
  if (!CompleteLiveRuntimeMemoryReceipts(
          &profiles,
          {{root->logical_node_id, input_memory}})) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "AGGREGATE runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "aggregate.selected-plan", "AGGREGATE");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-VALUES-V1", "AGGREGATE");

  exec::CanonicalPhysicalExecutorRegistration aggregate_registration;
  aggregate_registration.node_kind = exec::PhysicalNodeKind::kAggregate;
  aggregate_registration.implementation_id = aggregate_implementation_id;
  aggregate_registration.executor_capability_uuid =
      aggregate_capability_uuid;
  aggregate_registration.executor_capability_abi_version = 1;
  aggregate_registration.engine_owned = true;
  aggregate_registration.accepts_optimizer_publication_v2 = true;
  aggregate_registration.publishes_runtime_observation_v1 = true;
  aggregate_registration.execute =
      [aggregate_descriptor = prepared_root.aggregate_descriptor,
       result_column = prepared_root.result_column,
       count_star = prepared_root.count_star,
       distinct = prepared_root.distinct,
       value_columns = prepared_root.value_columns,
       value_descriptor_ids = prepared_root.value_descriptor_ids,
       exact_value_binding_receipts =
           prepared_root.exact_value_binding_receipts,
       direct_arguments = prepared_root.direct_arguments,
       filter_column = prepared_root.filter_column,
       filter_descriptor_id = prepared_root.filter_descriptor_id,
       aggregate_order_terms = prepared_root.aggregate_order_terms,
       aggregate_separator = prepared_root.aggregate_separator,
       listagg_overflow_mode = prepared_root.listagg_overflow_mode,
       listagg_max_output_bytes = prepared_root.listagg_max_output_bytes,
       listagg_truncation_indicator =
           prepared_root.listagg_truncation_indicator,
       listagg_with_count = prepared_root.listagg_with_count,
       maximum_filter_truth_memory_bytes = filter_truth_memory_bytes,
       input_row_count, mga_context = request.context](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            node.input_physical_node_ids.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "global aggregate executor did not receive one typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (input_batch.rows.size() != input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "global aggregate input cardinality differs from its selected cost";
          return step;
        }
        std::string value_binding_detail;
        if (!RevalidatePreparedAggregateValueBindings(
                value_columns, value_descriptor_ids,
                exact_value_binding_receipts, input_batch,
                &value_binding_detail)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail = std::move(value_binding_detail);
          return step;
        }
        const auto aggregate_memory_bound =
            SelectedNodeAggregateMemoryBound(dag, node);
        std::uint64_t input_memory_bytes = 1;
        std::uint64_t input_payload_bytes = 0;
        if (!aggregate_memory_bound.has_value() ||
            !AddBatchMemoryBytes(input_batch, &input_memory_bytes) ||
            !RuntimeMaterializedBatchMemoryBytes(
                input_batch, &input_payload_bytes) ||
            input_memory_bytes > *aggregate_memory_bound ||
            maximum_filter_truth_memory_bytes >
                *aggregate_memory_bound - input_memory_bytes) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "global aggregate FILTER carrier or input exceeds its "
              "selected-node memory grant";
          return step;
        }
        if (!filter_column.has_value() &&
            maximum_filter_truth_memory_bytes != 0) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "global aggregate without FILTER received a truth-carrier "
              "receipt";
          return step;
        }
        std::optional<std::vector<api::EngineSqlTruthValue>>
            filter_truth_values;
        std::uint64_t retained_filter_truth_bytes = 0;
        if (filter_column.has_value()) {
          std::vector<api::EngineSqlTruthValue> materialized_filter;
          std::string filter_detail;
          if (!MaterializeAggregateFilterTruthValues(
                  input_batch, *filter_column, filter_descriptor_id,
                  maximum_filter_truth_memory_bytes,
                  &materialized_filter, &retained_filter_truth_bytes,
                  &filter_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
            step.diagnostic.detail = std::move(filter_detail);
            return step;
          }
          filter_truth_values = std::move(materialized_filter);
        }
        exec::DescriptorBatch output_batch;
        const auto mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);
        if (count_star) {
          exec::CanonicalDescriptorCountRequest aggregate_request;
          aggregate_request.selected_physical_node_id = node.physical_node_id;
          aggregate_request.mga_authority = mga_authority;
          auto aggregate_result = exec::ExecuteCanonicalDescriptorCountStar(
              aggregate_request, dag, input_batch, result_column);
          if (!aggregate_result.diagnostic.ok) {
            step.diagnostic = std::move(aggregate_result.diagnostic);
            return step;
          }
          if (!CanonicalOperatorExecutionReceiptMatches(
                  aggregate_result, dag, node,
                  aggregate_request.mga_authority.statement_context)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-EXECUTION-V1";
            step.diagnostic.detail =
                "global COUNT(*) execution receipt changed";
            return step;
          }
          std::uint64_t output_memory_bytes = 0;
          std::uint64_t expected_peak_memory_bytes = 0;
          if (!RuntimeMaterializedBatchMemoryBytes(
                  aggregate_result.output_batch, &output_memory_bytes) ||
              !CheckedAdd(input_payload_bytes, sizeof(std::int64_t),
                          &expected_peak_memory_bytes) ||
              !CheckedAdd(expected_peak_memory_bytes, output_memory_bytes,
                          &expected_peak_memory_bytes) ||
              aggregate_result.input_payload_bytes != input_payload_bytes ||
              aggregate_result.state_bytes != sizeof(std::int64_t) ||
              aggregate_result.output_payload_bytes != output_memory_bytes ||
              aggregate_result.current_memory_bytes != output_memory_bytes ||
              aggregate_result.current_memory_bytes >
                  aggregate_result.peak_memory_bytes ||
              aggregate_result.peak_memory_bytes !=
                  expected_peak_memory_bytes ||
              aggregate_result.peak_memory_bytes > *aggregate_memory_bound) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code = "QOW-DIAG-OPT-017-REFUSAL-V1";
            step.diagnostic.detail =
                "global COUNT(*) runtime phase memory receipt is inconsistent";
            return step;
          }
          PublishRuntimeMemoryObservation(
              &step, output_memory_bytes, aggregate_result.peak_memory_bytes);
          output_batch = std::move(aggregate_result.output_batch);
        } else {
          exec::CanonicalAggregateRuntimeRequest aggregate_request;
          aggregate_request.selected_physical_node_id = node.physical_node_id;
          aggregate_request.descriptor = aggregate_descriptor;
          aggregate_request.value_columns = value_columns;
          aggregate_request.value_expression_descriptor_ids =
              value_descriptor_ids;
          aggregate_request.direct_arguments = direct_arguments;
          aggregate_request.result_column = result_column;
          aggregate_request.filter_truth_values =
              std::move(filter_truth_values);
          std::uint64_t logical_filter_truth_bytes = 0;
          if ((filter_column.has_value() &&
               !CheckedMultiply(input_batch.rows.size(),
                                sizeof(api::EngineSqlTruthValue),
                                &logical_filter_truth_bytes)) ||
              logical_filter_truth_bytes > retained_filter_truth_bytes) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "global aggregate FILTER truth-carrier receipt is "
                "inconsistent";
            return step;
          }
          aggregate_request.retained_memory_bytes =
              static_cast<std::size_t>(retained_filter_truth_bytes -
                                       logical_filter_truth_bytes);
          aggregate_request.distinct = distinct;
          aggregate_request.aggregate_order_terms = aggregate_order_terms;
          aggregate_request.aggregate_separator = aggregate_separator;
          aggregate_request.listagg_overflow_mode = listagg_overflow_mode;
          aggregate_request.listagg_max_output_bytes =
              listagg_max_output_bytes;
          aggregate_request.listagg_truncation_indicator =
              listagg_truncation_indicator;
          aggregate_request.listagg_with_count = listagg_with_count;
          aggregate_request.forced_strategy =
              exec::CanonicalAggregateExecutionStrategy::serial;
          aggregate_request.maximum_final_output_bytes =
              *aggregate_memory_bound;
          aggregate_request.maximum_finalization_workspace_bytes =
              *aggregate_memory_bound;
          aggregate_request.mga_authority = mga_authority;
          std::string equality_detail;
          if (!BindCanonicalAggregateEqualityTerms(
                  mga_context, input_batch, &aggregate_request,
                  &equality_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-EQUALITY-V1";
            step.diagnostic.detail = std::move(equality_detail);
            return step;
          }
          auto aggregate_result = exec::ExecuteCanonicalAggregateRuntime(
              aggregate_request, dag, input_batch);
          if (!aggregate_result.diagnostic.ok) {
            step.diagnostic = std::move(aggregate_result.diagnostic);
            return step;
          }
          if (!CanonicalOperatorExecutionReceiptMatches(
                  aggregate_result, dag, node,
                  aggregate_request.mga_authority.statement_context)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-EXECUTION-V1";
            step.diagnostic.detail =
                "global aggregate execution receipt changed";
            return step;
          }
          std::uint64_t output_memory_bytes = 0;
          if (!RuntimeMaterializedBatchMemoryBytes(
                  aggregate_result.output_batch, &output_memory_bytes) ||
              aggregate_result.executed_strategy !=
                  exec::CanonicalAggregateExecutionStrategy::serial ||
              aggregate_result.input_payload_bytes != input_payload_bytes ||
              aggregate_result.fixed_retained_memory_bytes !=
                  retained_filter_truth_bytes ||
              aggregate_result.current_memory_bytes != output_memory_bytes ||
              aggregate_result.current_memory_bytes >
                  aggregate_result.peak_memory_bytes ||
              aggregate_result.peak_memory_bytes > *aggregate_memory_bound) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code = "QOW-DIAG-OPT-017-REFUSAL-V1";
            step.diagnostic.detail =
                "global aggregate runtime phase memory receipt is inconsistent";
            return step;
          }
          step.authority = aggregate_result.authority;
          PublishRuntimeMemoryObservation(
              &step, output_memory_bytes, aggregate_result.peak_memory_bytes);
          output_batch = std::move(aggregate_result.output_batch);
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = output_batch.rows.size();
        step.materialized_output_batch = std::move(output_batch);
        step.mga_statement_context = mga_authority.statement_context;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.selected_physical_dag =
      std::move(planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(aggregate_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "aggregate.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "aggregate.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count = 1;

  const auto execution =
      ExecuteSelectedCanonicalObjectFreeDag(
          request.context, execution_request,
          planning.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live global aggregate selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

}  // namespace scratchbird::engine::sblr
