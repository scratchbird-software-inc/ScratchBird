// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "descriptor_value_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::executor {

struct Tuple {
  std::vector<std::int64_t> values;
};

struct Batch {
  std::string descriptor_digest;
  std::vector<Tuple> rows;
};

struct OperatorDiagnostic {
  bool ok = true;
  std::string diagnostic_code = "SB_EXECUTOR_OK";
};

struct OperatorCatalogEntry {
  std::string operator_id;
  std::string family;
  bool descriptor_required = true;
  bool storage_backed = false;
  bool can_materialize = false;
};

enum class Int64ComparisonOperator {
  kGreaterThan,
  kGreaterThanOrEqual,
  kLessThan,
  kLessThanOrEqual,
  kEqual,
  kNotEqual,
};

enum class CanonicalWindowRankingFunction : std::uint8_t {
  row_number = 1,
  rank,
  dense_rank,
  percent_rank,
  cume_dist,
  ntile,
};

struct CanonicalWindowRankingRequest {
  CanonicalWindowFrameResult frames;
  CanonicalWindowRankingFunction function =
      CanonicalWindowRankingFunction::row_number;
  std::string function_uuid;
  scratchbird::engine::internal_api::EngineDescriptor output_descriptor;
  std::optional<scratchbird::engine::internal_api::EngineTypedValue>
      ntile_bucket_count;
  std::size_t maximum_output_rows = 1048576;
  bool parser_execution_authority_claimed = false;
  bool transaction_finality_claimed = false;
  bool recovery_authority_claimed = false;
};

struct CanonicalWindowRankingResult {
  DescriptorRuntimeDiagnostic diagnostic;
  CanonicalWindowRankingFunction function =
      CanonicalWindowRankingFunction::row_number;
  std::vector<scratchbird::engine::internal_api::EngineTypedValue> values;
  bool frame_and_exclusion_validated_then_ignored = false;
  CanonicalPhysicalDispatchAuthorityEvidence authority;
  std::string window_property_uuid;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

enum class CanonicalWindowValueFunction : std::uint8_t {
  lag = 1,
  lead,
  first_value,
  last_value,
  nth_value,
};

enum class CanonicalWindowNthOrigin : std::uint8_t {
  from_first = 1,
  from_last,
};

enum class CanonicalWindowNullTreatment : std::uint8_t {
  respect_nulls = 1,
  ignore_nulls,
};

struct CanonicalWindowValueRequest {
  CanonicalWindowFrameResult frames;
  CanonicalWindowValueFunction function = CanonicalWindowValueFunction::lag;
  std::string function_uuid;
  std::uint32_t value_expression_descriptor_id = 0;
  ExecutorColumnDescriptor result_column;
  std::optional<
      std::vector<scratchbird::engine::internal_api::EngineTypedValue>>
      offset_values;
  std::optional<
      std::vector<scratchbird::engine::internal_api::EngineTypedValue>>
      default_values;
  std::optional<
      std::vector<scratchbird::engine::internal_api::EngineTypedValue>>
      nth_values;
  std::optional<CanonicalWindowNthOrigin> nth_origin;
  std::optional<CanonicalWindowNullTreatment> null_treatment;
  std::size_t maximum_output_rows = 1048576;
  bool parser_execution_authority_claimed = false;
  bool transaction_finality_claimed = false;
  bool recovery_authority_claimed = false;
};

struct CanonicalWindowValueResult {
  DescriptorRuntimeDiagnostic diagnostic;
  CanonicalWindowValueFunction function = CanonicalWindowValueFunction::lag;
  std::vector<scratchbird::engine::internal_api::EngineTypedValue> values;
  bool frame_and_exclusion_validated = false;
  bool frame_and_exclusion_ignored_for_navigation = false;
  CanonicalPhysicalDispatchAuthorityEvidence authority;
  std::string window_property_uuid;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

enum class CanonicalWindowAggregateFunction : std::uint8_t {
  int64_sum = 1,
};

struct CanonicalWindowAggregateRequest {
  CanonicalWindowFrameResult frames;
  CanonicalWindowAggregateFunction function =
      CanonicalWindowAggregateFunction::int64_sum;
  std::string function_uuid;
  std::uint32_t value_expression_descriptor_id = 0;
  ExecutorColumnDescriptor result_column;
  std::optional<
      std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>>
      filter_truth_values;
  bool distinct = false;
  std::vector<CanonicalDescriptorOrderTerm> aggregate_order_terms;
  std::string deterministic_tie_evidence_uuid;
  std::size_t maximum_output_rows = 1048576;
  std::size_t maximum_transition_count = 1048576;
  std::size_t maximum_distinct_value_count = 1048576;
  std::size_t maximum_pair_comparisons = 1048576;
  bool parser_execution_authority_claimed = false;
  bool transaction_finality_claimed = false;
  bool recovery_authority_claimed = false;
};

struct CanonicalWindowAggregateResult {
  DescriptorRuntimeDiagnostic diagnostic;
  CanonicalWindowAggregateFunction function =
      CanonicalWindowAggregateFunction::int64_sum;
  std::vector<scratchbird::engine::internal_api::EngineTypedValue> values;
  std::vector<std::vector<std::size_t>> transition_row_indices;
  std::size_t transition_count = 0;
  std::size_t distinct_value_count = 0;
  std::size_t pair_comparison_count = 0;
  bool filter_applied_before_transition = false;
  bool distinct_applied_before_transition = false;
  bool aggregate_order_independent_of_window_order = false;
  bool effective_frame_recomputed = false;
  bool shared_aggregate_state_authority_used = false;
  CanonicalPhysicalDispatchAuthorityEvidence authority;
  std::string window_property_uuid;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

enum class CanonicalQueryEvaluationStage : std::uint8_t {
  from = 1,
  where,
  group_and_aggregate,
  having,
  window,
  qualify,
  projection,
  distinct,
  set_operation,
  query_order,
  offset_limit_fetch_top,
};

struct CanonicalWindowMaterialization {
  CanonicalWindowFrameResult frames;
  ExecutorColumnDescriptor result_column;
  std::vector<scratchbird::engine::internal_api::EngineTypedValue> values;
  std::string function_state_uuid;
};

struct CanonicalWindowCompositionRequest {
  DescriptorBatch input_batch;
  std::vector<CanonicalWindowMaterialization> windows;
  std::optional<
      std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>>
      qualify_truth_values;
  std::vector<std::uint32_t> qualify_referenced_window_descriptor_ids;
  std::vector<std::uint32_t> projection_descriptor_ids;
  std::vector<CanonicalDescriptorOrderTerm> query_order_terms;
  std::string query_order_tie_evidence_uuid;
  std::uint64_t offset = 0;
  std::optional<std::uint64_t> row_limit;
  std::optional<TypedPhysicalNodeDag> composition_dag;
  std::vector<PhysicalNodeKind> required_upstream_node_kinds;
  std::size_t maximum_window_count = 256;
  std::size_t maximum_output_rows = 1048576;
  std::size_t maximum_pair_comparisons = 1048576;
  bool shape_specific_parser_route_claimed = false;
  bool shape_specific_execution_route_claimed = false;
  bool parser_execution_authority_claimed = false;
  bool transaction_finality_claimed = false;
  bool recovery_authority_claimed = false;
};

struct CanonicalWindowCompositionResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::vector<std::size_t> source_row_indices;
  std::vector<CanonicalQueryEvaluationStage> stage_trace;
  std::vector<std::uint32_t> materialized_window_descriptor_ids;
  std::size_t shared_materialization_pair_count = 0;
  bool every_function_state_independent = false;
  bool all_windows_materialized_before_qualify = false;
  bool qualify_uses_true_only_3vl = false;
  bool projection_precedes_query_order = false;
  bool query_order_precedes_row_limit = false;
  bool ordinary_physical_nodes_validated = false;
  CanonicalPhysicalDispatchAuthorityEvidence authority;
  std::string selected_plan_uuid;
  std::uint64_t inventory_local_transaction_id = 0;
  std::uint64_t inventory_statement_snapshot_id = 0;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

Batch MakeBatch(std::string descriptor_digest, std::vector<Tuple> rows);
OperatorDiagnostic ValidateBatch(const Batch& batch);
std::int64_t EvalAdd(std::int64_t lhs, std::int64_t rhs);
std::int64_t EvalMultiply(std::int64_t lhs, std::int64_t rhs);
Batch FilterByInt64Comparison(const Batch& input,
                              std::size_t column,
                              Int64ComparisonOperator op,
                              std::int64_t threshold);
Batch FilterGreaterThan(const Batch& input, std::size_t column, std::int64_t threshold);
Batch ProjectColumns(const Batch& input, const std::vector<std::size_t>& columns);
Batch SortByColumn(const Batch& input, std::size_t column, bool ascending);
Batch LimitOffset(const Batch& input, std::size_t limit, std::size_t offset);
Batch AggregateSumByKey(const Batch& input, std::size_t key_column, std::size_t value_column);
Batch NestedLoopJoinEqual(const Batch& left, const Batch& right, std::size_t left_column, std::size_t right_column);
Batch HashJoinEqual(const Batch& left, const Batch& right, std::size_t left_column, std::size_t right_column);
Batch MergeJoinEqual(const Batch& left_sorted, const Batch& right_sorted, std::size_t left_column, std::size_t right_column);
Batch AddRowNumberWindow(const Batch& input, std::size_t order_column);
Batch AddRankWindow(const Batch& input, std::size_t order_column);
Batch AddDenseRankWindow(const Batch& input, std::size_t order_column);
Batch AddPartitionCountWindow(const Batch& input, std::size_t partition_column);
Batch AddNtileWindow(const Batch& input, std::size_t order_column, std::int64_t bucket_count);
Batch AddLagWindow(const Batch& input, std::size_t order_column, std::size_t value_column);
Batch AddLeadWindow(const Batch& input, std::size_t order_column, std::size_t value_column);
Batch AddFirstValueWindow(const Batch& input, std::size_t order_column, std::size_t value_column);
Batch AddLastValueWindow(const Batch& input, std::size_t order_column, std::size_t value_column);
CanonicalWindowRankingResult ExecuteCanonicalWindowRanking(
    const CanonicalWindowRankingRequest& request);
CanonicalWindowValueResult ExecuteCanonicalWindowValue(
    const CanonicalWindowValueRequest& request);
CanonicalWindowAggregateResult ExecuteCanonicalWindowAggregate(
    const CanonicalWindowAggregateRequest& request);
CanonicalWindowCompositionResult ExecuteCanonicalWindowComposition(
    const CanonicalWindowCompositionRequest& request);
Batch MaterializeCte(const Batch& input);
std::int64_t ScalarSubqueryFirstValue(const Batch& input, std::size_t column);
Batch SetUnionDistinct(const Batch& left, const Batch& right);
Batch SetIntersectDistinct(const Batch& left, const Batch& right);
Batch SetExceptDistinct(const Batch& left, const Batch& right);
Batch SetUnionAll(const Batch& left, const Batch& right);
Batch SetIntersectAll(const Batch& left, const Batch& right);
Batch SetExceptAll(const Batch& left, const Batch& right);
std::vector<OperatorCatalogEntry> Stage6OperatorCatalog();
bool ValidateOperatorCatalog(const std::vector<OperatorCatalogEntry>& catalog, std::vector<std::string>* errors);

}  // namespace scratchbird::engine::executor
