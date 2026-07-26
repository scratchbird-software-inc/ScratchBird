// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "datatype_operations.hpp"
#include "physical_node_abi.hpp"
#include "query/expression_api.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

// SEARCH_KEY: SB_EXEC_DESCRIPTOR_VALUE_RUNTIME_AUTHORITY
// Descriptor-bound tuple/batch runtime used by the executor. SQL names and
// parser syntax are not authority here; descriptors and encoded values are.

struct ExecutorColumnDescriptor {
  std::string stable_name;
  scratchbird::engine::internal_api::EngineDescriptor descriptor;
  bool nullable = true;
  std::uint32_t descriptor_id = 0;
};

struct DescriptorTuple {
  std::vector<scratchbird::engine::internal_api::EngineTypedValue> values;
};

struct DescriptorBatch {
  std::vector<ExecutorColumnDescriptor> columns;
  std::vector<DescriptorTuple> rows;
};

struct DescriptorRuntimeDiagnostic {
  bool ok = true;
  std::string diagnostic_code = "SB_EXECUTOR_OK";
  std::string detail;
  std::size_t row_index = 0;
  std::size_t column_index = 0;
};

struct CanonicalDescriptorProjectionRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch input_batch;
  std::vector<std::size_t> projected_columns;
};

struct CanonicalDescriptorProjectionResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalDescriptorFilterRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch input_batch;
  std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>
      row_truth_values;
  scratchbird::engine::internal_api::EnginePredicateConsumer consumer =
      scratchbird::engine::internal_api::EnginePredicateConsumer::filter;
};

struct CanonicalDescriptorFilterResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalDescriptorLimitRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch input_batch;
  std::uint64_t limit = 0;
  std::uint64_t offset = 0;
};

struct CanonicalDescriptorLimitResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalTableSubqueryRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch input_batch;
  std::size_t maximum_materialized_row_count = 1048576;
};

struct CanonicalTableSubqueryResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::size_t materialized_row_count = 0;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalScalarSubqueryRequest {
  CanonicalTableSubqueryRequest table_request;
  std::uint32_t value_expression_descriptor_id = 0;
  ExecutorColumnDescriptor result_column;
};

struct CanonicalScalarSubqueryResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::size_t source_row_count = 0;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

enum class CanonicalFetchTopProfileForm : std::uint8_t {
  fetch_first_rows_only = 1,
  fetch_first_rows_with_ties,
  top_rows,
  top_percent,
  top_rows_with_ties,
};

struct CanonicalDescriptorFetchProfileRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch input_batch;
  CanonicalFetchTopProfileForm form =
      CanonicalFetchTopProfileForm::fetch_first_rows_only;
  std::uint64_t row_count = 0;
  std::uint64_t offset = 0;
  bool row_count_is_bound = false;
};

struct CanonicalDescriptorFetchProfileResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalDescriptorCountRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch input_batch;
  ExecutorColumnDescriptor count_column;
};

struct CanonicalDescriptorCountResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalInt64SumAggregateState {
  std::uint32_t value_expression_descriptor_id = 0;
  ExecutorColumnDescriptor result_column;
  std::uint64_t transition_count = 0;
  std::uint64_t non_null_count = 0;
  std::int64_t accumulated_value = 0;
  bool has_value = false;
};

struct CanonicalInt64SumStateRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch input_batch;
  std::size_t value_column = 0;
  std::uint32_t value_expression_descriptor_id = 0;
  ExecutorColumnDescriptor result_column;
  std::size_t maximum_transition_count = 1048576;
};

struct CanonicalInt64SumStateResult {
  DescriptorRuntimeDiagnostic diagnostic;
  CanonicalInt64SumAggregateState state;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalInt64SumFinalizeRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  CanonicalInt64SumAggregateState state;
};

struct CanonicalInt64SumFinalizeResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

enum class CanonicalInt64GroupingSetRule : std::uint8_t {
  key_only = 1,
  key_and_grand_total,
};

struct CanonicalInt64SumGroupState {
  std::uint32_t grouping_set_ordinal = 0;
  bool is_grand_total = false;
  scratchbird::engine::internal_api::EngineTypedValue group_key;
  CanonicalInt64SumAggregateState sum_state;
};

struct CanonicalInt64SumGroupRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch input_batch;
  std::size_t key_column = 0;
  std::uint32_t key_expression_descriptor_id = 0;
  std::size_t value_column = 0;
  std::uint32_t value_expression_descriptor_id = 0;
  ExecutorColumnDescriptor key_result_column;
  ExecutorColumnDescriptor sum_result_column;
  CanonicalInt64GroupingSetRule grouping_set_rule =
      CanonicalInt64GroupingSetRule::key_only;
  std::size_t maximum_group_count = 65536;
  std::size_t maximum_transition_count = 1048576;
};

struct CanonicalInt64SumGroupResult {
  DescriptorRuntimeDiagnostic diagnostic;
  std::vector<CanonicalInt64SumGroupState> groups;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalInt64SumFilterRequest {
  CanonicalInt64SumStateRequest aggregate_request;
  std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>
      row_truth_values;
};

struct CanonicalInt64SumFilterResult {
  DescriptorRuntimeDiagnostic diagnostic;
  CanonicalInt64SumAggregateState state;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalInt64SumDistinctRequest {
  CanonicalInt64SumStateRequest aggregate_request;
  std::size_t maximum_distinct_value_count = 1048576;
};

struct CanonicalInt64SumDistinctResult {
  DescriptorRuntimeDiagnostic diagnostic;
  CanonicalInt64SumAggregateState state;
  std::size_t distinct_value_count = 0;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalInt64SumSpillRequest {
  CanonicalInt64SumGroupRequest aggregate_request;
  std::filesystem::path spill_root;
  std::string spill_owner_uuid;
  std::uint64_t runtime_generation = 1;
  std::uint64_t reopen_runtime_generation = 0;
  std::uint64_t memory_quota_bytes = 4096;
  std::size_t maximum_spill_record_count = 3145728;
  bool cancellation_requested = false;
  bool restart_recovery_proof_available = true;
};

struct CanonicalInt64SumSpillResult {
  DescriptorRuntimeDiagnostic diagnostic;
  std::vector<CanonicalInt64SumGroupState> groups;
  bool spilled = false;
  bool spill_reopened = false;
  bool cleanup_proven = false;
  bool cancellation_observed = false;
  std::vector<std::string> spill_evidence;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalDescriptorInnerJoinRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch left_batch;
  DescriptorBatch right_batch;
  std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>
      pair_truth_values;
  scratchbird::engine::internal_api::EnginePredicateConsumer consumer =
      scratchbird::engine::internal_api::EnginePredicateConsumer::join_on;
};

struct CanonicalDescriptorInnerJoinResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalCompositeJoinKeyTerm {
  std::size_t left_column = 0;
  std::uint32_t left_expression_descriptor_id = 0;
  std::size_t right_column = 0;
  std::uint32_t right_expression_descriptor_id = 0;
};

struct CanonicalCompositeJoinKeyRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch left_batch;
  DescriptorBatch right_batch;
  std::vector<CanonicalCompositeJoinKeyTerm> key_terms;
  std::size_t maximum_key_term_count = 64;
  std::size_t maximum_key_comparisons = 1048576;
};

struct CanonicalCompositeJoinKeyResult {
  DescriptorRuntimeDiagnostic diagnostic;
  std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>
      pair_truth_values;
  std::size_t pair_count = 0;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalJoinResidualRequest {
  CanonicalCompositeJoinKeyRequest key_request;
  std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>
      residual_truth_values;
  std::size_t maximum_candidate_rechecks = 1048576;
};

struct CanonicalJoinResidualResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::vector<std::size_t> accepted_pair_indices;
  std::size_t candidate_pair_count = 0;
  std::size_t residual_recheck_count = 0;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

enum class CanonicalAcceptedJoinKind : std::uint8_t {
  kLeftOuter = 1,
};

struct CanonicalJoinKindRequest {
  CanonicalJoinResidualRequest residual_request;
  CanonicalAcceptedJoinKind join_kind =
      CanonicalAcceptedJoinKind::kLeftOuter;
  std::size_t maximum_output_rows = 1048576;
};

struct CanonicalJoinKindResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::size_t matched_pair_count = 0;
  std::size_t unmatched_left_row_count = 0;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

enum class CanonicalJoinStrategyKind : std::uint8_t {
  kHashInnerInt64Equality = 1,
};

struct CanonicalJoinStrategyRequest {
  CanonicalJoinResidualRequest residual_request;
  CanonicalJoinStrategyKind strategy =
      CanonicalJoinStrategyKind::kHashInnerInt64Equality;
  std::size_t maximum_hash_entries = 1048576;
  std::size_t maximum_candidate_probes = 1048576;
  std::size_t maximum_output_rows = 1048576;
};

struct CanonicalJoinStrategyResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::vector<std::size_t> canonical_pair_indices;
  std::vector<std::size_t> strategy_pair_indices;
  std::size_t hash_entry_count = 0;
  std::size_t candidate_probe_count = 0;
  bool canonical_multiset_proven = false;
  std::string strategy_id;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

enum class CanonicalMgaVisibilityDecision : std::uint8_t {
  kVisible = 1,
  kInvisible,
  kIndeterminate,
};

enum class CanonicalMgaSecurityDecision : std::uint8_t {
  kAllowed = 1,
  kDenied,
  kIndeterminate,
};

struct CanonicalJoinMgaCandidateEvidence {
  std::size_t pair_index = 0;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t statement_snapshot_id = 0;
  std::uint64_t left_row_version_id = 0;
  std::uint64_t right_row_version_id = 0;
  CanonicalMgaVisibilityDecision left_visibility =
      CanonicalMgaVisibilityDecision::kIndeterminate;
  CanonicalMgaVisibilityDecision right_visibility =
      CanonicalMgaVisibilityDecision::kIndeterminate;
  CanonicalMgaSecurityDecision security_decision =
      CanonicalMgaSecurityDecision::kIndeterminate;
  std::uint64_t index_candidate_generation = 0;
  std::uint64_t current_index_generation = 0;
  scratchbird::engine::internal_api::EngineSqlTruthValue exact_key_recheck =
      scratchbird::engine::internal_api::EngineSqlTruthValue::unknown;
  std::string engine_evidence_uuid;
};

struct CanonicalJoinMgaRequest {
  CanonicalJoinStrategyRequest strategy_request;
  std::uint64_t transaction_inventory_id = 0;
  std::uint64_t inventory_local_transaction_id = 0;
  std::uint64_t inventory_statement_snapshot_id = 0;
  std::string transaction_inventory_evidence_uuid;
  std::vector<CanonicalJoinMgaCandidateEvidence> candidate_evidence;
  std::size_t maximum_boundary_rechecks = 1048576;
};

struct CanonicalJoinMgaResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::size_t candidate_pair_count = 0;
  std::size_t visible_pair_count = 0;
  std::size_t visibility_filtered_pair_count = 0;
  std::size_t security_filtered_pair_count = 0;
  bool mga_boundary_proven = false;
  std::string transaction_inventory_evidence_uuid;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalDescriptorRowNumberRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch ordered_input_batch;
  ExecutorColumnDescriptor row_number_column;
  std::string deterministic_order_evidence_uuid;
};

struct CanonicalDescriptorRowNumberResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

enum class CanonicalDescriptorOrderDirection : std::uint8_t {
  ascending = 1,
  descending,
};

enum class CanonicalDescriptorNullPlacement : std::uint8_t {
  first = 1,
  last,
};

struct CanonicalDescriptorOrderTerm {
  std::size_t column = 0;
  std::uint32_t expression_descriptor_id = 0;
  CanonicalDescriptorOrderDirection direction =
      CanonicalDescriptorOrderDirection::ascending;
  CanonicalDescriptorNullPlacement null_placement =
      CanonicalDescriptorNullPlacement::last;
  std::string collation_uuid;
  std::uint64_t resource_epoch = 0;
  std::uint64_t collation_epoch = 0;
  scratchbird::core::datatypes::DatatypeTextSeedAuthority text_seed;
};

struct CanonicalDescriptorSortRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch input_batch;
  std::vector<CanonicalDescriptorOrderTerm> order_terms;
  std::string deterministic_tie_evidence_uuid;
  std::size_t maximum_pair_comparisons = 1048576;
};

struct CanonicalDescriptorSortResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalInt64SumOrderedRequest {
  CanonicalInt64SumStateRequest aggregate_request;
  std::size_t order_column = 0;
  std::uint32_t order_expression_descriptor_id = 0;
  CanonicalDescriptorOrderDirection direction =
      CanonicalDescriptorOrderDirection::ascending;
  CanonicalDescriptorNullPlacement null_placement =
      CanonicalDescriptorNullPlacement::last;
  std::string deterministic_tie_evidence_uuid;
  std::size_t maximum_pair_comparisons = 1048576;
};

struct CanonicalInt64SumOrderedResult {
  DescriptorRuntimeDiagnostic diagnostic;
  CanonicalInt64SumAggregateState state;
  std::vector<std::size_t> ordered_input_row_indices;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct Int64DecodeResult {
  DescriptorRuntimeDiagnostic diagnostic;
  std::int64_t value = 0;

  bool ok() const { return diagnostic.ok; }
};

struct BoolDecodeResult {
  DescriptorRuntimeDiagnostic diagnostic;
  bool value = false;

  bool ok() const { return diagnostic.ok; }
};

struct Real64DecodeResult {
  DescriptorRuntimeDiagnostic diagnostic;
  double value = 0.0;

  bool ok() const { return diagnostic.ok; }
};

enum class DescriptorExpressionOperator {
  kInt64Add,
  kInt64Subtract,
  kInt64Multiply,
  kInt64Divide,
  kInt64Equal,
  kInt64GreaterThan,
  kReal64Add,
  kReal64Subtract,
  kReal64Multiply,
  kReal64Divide,
  kReal64Equal,
  kReal64GreaterThan,
  kBoolAnd,
  kBoolOr,
  kTextConcat,
  kTextEqual,
};

enum class DescriptorComparisonOperator {
  kEqual,
  kGreaterThan,
};

enum class DescriptorDomainMaskKind {
  kNone,
  kNull,
  kFixedText,
  kRevealLast4,
};

struct DescriptorRuntimeVariable {
  std::string stable_name;
  scratchbird::engine::internal_api::EngineTypedValue value;
};

struct DescriptorRuntimeSetScope {
  std::vector<DescriptorRuntimeVariable> variables;
};

struct DescriptorDomainPolicy {
  std::string domain_stable_name;
  scratchbird::engine::internal_api::EngineDescriptor base_descriptor;
  bool nullable = true;
  std::optional<std::int64_t> min_int64;
  std::optional<std::int64_t> max_int64;
  std::optional<std::size_t> max_text_bytes;
  DescriptorDomainMaskKind mask_kind = DescriptorDomainMaskKind::kNone;
  std::string fixed_mask_text;
  std::string required_security_token;
};

scratchbird::engine::internal_api::EngineDescriptor MakeExecutorDescriptor(std::string canonical_type_name,
                                                                           std::string encoded_descriptor = {});
scratchbird::engine::internal_api::EngineTypedValue MakeExecutorValue(
    const scratchbird::engine::internal_api::EngineDescriptor& descriptor,
    std::string encoded_value,
    bool is_null = false);
DescriptorBatch MakeDescriptorBatch(std::vector<ExecutorColumnDescriptor> columns,
                                    std::vector<DescriptorTuple> rows);
std::string DescriptorFingerprint(const std::vector<ExecutorColumnDescriptor>& columns);
bool DescriptorMatches(const scratchbird::engine::internal_api::EngineDescriptor& expected,
                       const scratchbird::engine::internal_api::EngineDescriptor& actual);
DescriptorRuntimeDiagnostic ValidateDescriptorBatch(const DescriptorBatch& batch);
DescriptorRuntimeDiagnostic ValidateCanonicalDescriptorBatch(
    const DescriptorBatch& batch,
    const std::vector<std::uint32_t>& output_descriptor_ids);
CanonicalDescriptorProjectionResult ExecuteCanonicalDescriptorProjection(
    const CanonicalDescriptorProjectionRequest& request);
CanonicalDescriptorFilterResult ExecuteCanonicalDescriptorFilter(
    const CanonicalDescriptorFilterRequest& request);
CanonicalDescriptorLimitResult ExecuteCanonicalDescriptorLimit(
    const CanonicalDescriptorLimitRequest& request);
CanonicalTableSubqueryResult ExecuteCanonicalTableSubquery(
    const CanonicalTableSubqueryRequest& request);
CanonicalScalarSubqueryResult ExecuteCanonicalScalarSubquery(
    const CanonicalScalarSubqueryRequest& request);
CanonicalDescriptorFetchProfileResult ExecuteCanonicalDescriptorFetchProfile(
    const CanonicalDescriptorFetchProfileRequest& request);
CanonicalDescriptorCountResult ExecuteCanonicalDescriptorCountStar(
    const CanonicalDescriptorCountRequest& request);
CanonicalInt64SumStateResult ExecuteCanonicalInt64SumState(
    const CanonicalInt64SumStateRequest& request);
CanonicalInt64SumFinalizeResult ExecuteCanonicalInt64SumFinalize(
    const CanonicalInt64SumFinalizeRequest& request);
CanonicalInt64SumGroupResult ExecuteCanonicalInt64SumGroups(
    const CanonicalInt64SumGroupRequest& request);
CanonicalInt64SumFilterResult ExecuteCanonicalInt64SumFilter(
    const CanonicalInt64SumFilterRequest& request);
CanonicalInt64SumDistinctResult ExecuteCanonicalInt64SumDistinct(
    const CanonicalInt64SumDistinctRequest& request);
CanonicalInt64SumSpillResult ExecuteCanonicalInt64SumSpill(
    const CanonicalInt64SumSpillRequest& request);
CanonicalDescriptorInnerJoinResult ExecuteCanonicalDescriptorInnerJoin(
    const CanonicalDescriptorInnerJoinRequest& request);
CanonicalCompositeJoinKeyResult ExecuteCanonicalCompositeJoinKey(
    const CanonicalCompositeJoinKeyRequest& request);
CanonicalJoinResidualResult ExecuteCanonicalJoinResidual(
    const CanonicalJoinResidualRequest& request);
CanonicalJoinKindResult ExecuteCanonicalJoinKind(
    const CanonicalJoinKindRequest& request);
CanonicalJoinStrategyResult ExecuteCanonicalJoinStrategy(
    const CanonicalJoinStrategyRequest& request);
CanonicalJoinMgaResult ExecuteCanonicalJoinMgaBoundary(
    const CanonicalJoinMgaRequest& request);
CanonicalDescriptorRowNumberResult ExecuteCanonicalDescriptorRowNumber(
    const CanonicalDescriptorRowNumberRequest& request);
CanonicalDescriptorSortResult ExecuteCanonicalDescriptorSort(
    const CanonicalDescriptorSortRequest& request);
CanonicalInt64SumOrderedResult ExecuteCanonicalInt64SumOrdered(
    const CanonicalInt64SumOrderedRequest& request);
std::optional<std::size_t> FindColumnByStableName(const DescriptorBatch& batch, const std::string& stable_name);
DescriptorBatch ProjectDescriptorBatch(const DescriptorBatch& input, const std::vector<std::size_t>& columns);
DescriptorBatch FilterDescriptorInt64GreaterThan(const DescriptorBatch& input,
                                                 std::size_t column,
                                                 std::int64_t threshold,
                                                 DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch FilterDescriptorBatchByComparison(
    const DescriptorBatch& input,
    std::size_t column,
    DescriptorComparisonOperator op,
    const scratchbird::engine::internal_api::EngineTypedValue& bound_value,
    DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch SortDescriptorBatchByColumn(const DescriptorBatch& input,
                                            std::size_t column,
                                            bool ascending,
                                            DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch LimitOffsetDescriptorBatch(const DescriptorBatch& input,
                                           std::size_t limit,
                                           std::size_t offset);
DescriptorBatch SetUnionDistinctDescriptorBatch(const DescriptorBatch& left,
                                                const DescriptorBatch& right,
                                                DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch SetIntersectDistinctDescriptorBatch(const DescriptorBatch& left,
                                                    const DescriptorBatch& right,
                                                    DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch SetExceptDistinctDescriptorBatch(const DescriptorBatch& left,
                                                 const DescriptorBatch& right,
                                                 DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch JoinDescriptorBatchesOnInt64(const DescriptorBatch& left,
                                             const DescriptorBatch& right,
                                             std::size_t left_column,
                                             std::size_t right_column,
                                             DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch JoinDescriptorBatchesOnEqual(const DescriptorBatch& left,
                                             const DescriptorBatch& right,
                                             std::size_t left_column,
                                             std::size_t right_column,
                                             DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch AggregateDescriptorCountByInt64(const DescriptorBatch& input,
                                                std::size_t group_column,
                                                std::string count_stable_name,
                                                DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch AggregateDescriptorCountByKey(const DescriptorBatch& input,
                                              std::size_t group_column,
                                              std::string count_stable_name,
                                              DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch WindowDescriptorRowNumberByInt64(const DescriptorBatch& input,
                                                 std::size_t order_column,
                                                 std::string row_number_stable_name,
                                                 bool ascending,
                                                 DescriptorRuntimeDiagnostic* diagnostic = nullptr);
scratchbird::engine::internal_api::EngineTypedValue EvaluateDescriptorExpression(
    DescriptorExpressionOperator op,
    const scratchbird::engine::internal_api::EngineTypedValue& left,
    const scratchbird::engine::internal_api::EngineTypedValue& right,
    DescriptorRuntimeDiagnostic* diagnostic = nullptr);
scratchbird::engine::internal_api::EngineTypedValue EvaluateDescriptorCoalesce(
    const std::vector<scratchbird::engine::internal_api::EngineTypedValue>& values,
    DescriptorRuntimeDiagnostic* diagnostic = nullptr);
scratchbird::engine::internal_api::EngineTypedValue CastDescriptorValue(
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    const scratchbird::engine::internal_api::EngineDescriptor& target_descriptor,
    DescriptorRuntimeDiagnostic* diagnostic = nullptr);
scratchbird::engine::internal_api::EngineTypedValue ExtractDescriptorField(
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    const std::string& field_name,
    DescriptorRuntimeDiagnostic* diagnostic = nullptr);
void SetDescriptorRuntimeVariable(DescriptorRuntimeSetScope* scope,
                                  std::string stable_name,
                                  scratchbird::engine::internal_api::EngineTypedValue value);
std::optional<scratchbird::engine::internal_api::EngineTypedValue> GetDescriptorRuntimeVariable(
    const DescriptorRuntimeSetScope& scope,
    const std::string& stable_name);
DescriptorRuntimeDiagnostic ValidateDescriptorDomainValue(
    const DescriptorDomainPolicy& policy,
    const scratchbird::engine::internal_api::EngineTypedValue& value);
scratchbird::engine::internal_api::EngineTypedValue ApplyDescriptorDomainMask(
    const DescriptorDomainPolicy& policy,
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    const std::string& security_token,
    DescriptorRuntimeDiagnostic* diagnostic = nullptr);
scratchbird::engine::internal_api::EngineTypedValue EvaluateDescriptorDomainMethod(
    const DescriptorDomainPolicy& policy,
    const std::string& method_name,
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    const std::string& security_token,
    DescriptorRuntimeDiagnostic* diagnostic = nullptr);
Int64DecodeResult DecodeInt64Value(const scratchbird::engine::internal_api::EngineTypedValue& value);
BoolDecodeResult DecodeBoolValue(const scratchbird::engine::internal_api::EngineTypedValue& value);
Real64DecodeResult DecodeReal64Value(const scratchbird::engine::internal_api::EngineTypedValue& value);
scratchbird::engine::internal_api::EngineTypedValue EncodeInt64Value(std::int64_t value);
scratchbird::engine::internal_api::EngineTypedValue EncodeBoolValue(bool value);
scratchbird::engine::internal_api::EngineTypedValue EncodeReal64Value(double value);
scratchbird::engine::internal_api::EngineTypedValue EncodeTextValue(std::string value);

}  // namespace scratchbird::engine::executor
