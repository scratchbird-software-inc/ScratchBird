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
#include <functional>
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

enum class CanonicalResultKind : std::uint8_t {
  kRows = 1,
  kCommand,
  kEmpty,
  kCursor,
  kExplain,
};

enum class CanonicalResultNullability : std::uint8_t {
  kNonNull = 1,
  kNullable,
  kUnknown,
};

enum class CanonicalResultCursorState : std::uint8_t {
  kClosed = 1,
  kOpen,
  kSuspended,
};

enum class CanonicalResultDiagnosticSeverity : std::uint8_t {
  kInfo = 1,
  kWarning,
  kError,
  kFatal,
};

enum class CanonicalResultDiagnosticPhase : std::uint8_t {
  kParse = 1,
  kBind,
  kVerify,
  kPlan,
  kExecute,
  kFinalize,
};

enum class CanonicalResultTransactionEffect : std::uint8_t {
  kUnchanged = 1,
  kStatementFailedTransactionUsable,
  kEngineMarkedTransactionFailed,
};

enum class CanonicalResultRetryability : std::uint8_t {
  kNotRetryable = 1,
  kRetrySameSnapshot,
  kRetryNewSnapshot,
};

enum class CanonicalResultInvocationMode : std::uint8_t {
  kDirect = 1,
  kPrepared,
};

enum class CanonicalResultDeliveryKind : std::uint8_t {
  kMetadata = 1,
  kRow,
  kDiagnostics,
};

struct CanonicalResultColumnDescriptor {
  std::uint32_t ordinal = 0;
  std::string name_utf8;
  std::string descriptor_uuid;
  std::string type_uuid;
  CanonicalResultNullability nullability =
      CanonicalResultNullability::kUnknown;
  std::optional<std::string> collation_uuid;
  std::optional<std::string> timezone_profile_id;
};

struct CanonicalResultColumnBinding {
  std::size_t physical_column_ordinal = 0;
  bool visible = true;
  std::optional<CanonicalResultColumnDescriptor> published_descriptor;
};

struct CanonicalResultDiagnosticRecord {
  std::string diagnostic_id;
  std::string stable_code;
  CanonicalResultDiagnosticSeverity severity =
      CanonicalResultDiagnosticSeverity::kError;
  std::optional<std::string> sqlstate;
  std::string message_key;
  std::vector<scratchbird::engine::internal_api::EngineTypedValue>
      argument_values;
  CanonicalResultDiagnosticPhase phase =
      CanonicalResultDiagnosticPhase::kExecute;
  std::optional<std::string> record_path;
  std::optional<std::string> field_id;
  std::optional<std::uint64_t> physical_node_id;
  CanonicalResultTransactionEffect transaction_effect =
      CanonicalResultTransactionEffect::kUnchanged;
  CanonicalResultRetryability retryability =
      CanonicalResultRetryability::kNotRetryable;
};

struct CanonicalResultEnvelopeV1 {
  std::uint16_t abi_version = 1;
  std::string statement_uuid;
  std::string execution_attempt_uuid;
  CanonicalResultKind result_kind = CanonicalResultKind::kRows;
  std::vector<CanonicalResultColumnDescriptor> column_descriptors;
  std::string row_stream_format_id;
  std::optional<std::uint64_t> row_count;
  std::optional<std::string> command_tag;
  std::optional<CanonicalResultCursorState> cursor_state;
  std::vector<CanonicalResultDiagnosticRecord> diagnostics;
};

struct CanonicalResultDeliveryRecord {
  CanonicalResultDeliveryKind kind = CanonicalResultDeliveryKind::kMetadata;
  std::optional<std::size_t> row_ordinal;
};

struct CanonicalResultPublicationRequest {
  std::uint16_t abi_version = 1;
  std::string statement_uuid;
  std::string execution_attempt_uuid;
  CanonicalResultKind result_kind = CanonicalResultKind::kRows;
  CanonicalResultInvocationMode invocation_mode =
      CanonicalResultInvocationMode::kDirect;
  DescriptorBatch physical_output_batch;
  std::vector<CanonicalResultColumnBinding> column_bindings;
  std::string row_stream_format_id = "QOW-TYPED-ROW-STREAM-V1";
  std::optional<std::string> command_tag;
  std::optional<CanonicalResultCursorState> cursor_state;
  std::vector<CanonicalResultDiagnosticRecord> diagnostics;
  std::string transaction_effect_evidence_uuid;
  std::size_t maximum_row_count = 1048576;
};

struct CanonicalResultPublicationResult {
  DescriptorRuntimeDiagnostic diagnostic;
  bool published = false;
  CanonicalResultEnvelopeV1 envelope;
  DescriptorBatch row_stream;
  std::vector<CanonicalResultDeliveryRecord> delivery_records;
  std::string canonical_envelope_bytes;
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

struct CanonicalRowSubqueryRequest {
  CanonicalTableSubqueryRequest table_request;
  std::vector<std::uint32_t> row_expression_descriptor_ids;
  std::vector<ExecutorColumnDescriptor> result_columns;
};

struct CanonicalRowSubqueryResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::size_t source_row_count = 0;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalExistsSubqueryRequest {
  CanonicalTableSubqueryRequest table_request;
  std::uint32_t exists_expression_descriptor_id = 0;
  ExecutorColumnDescriptor result_column;
};

struct CanonicalExistsSubqueryResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::size_t source_row_count = 0;
  bool exists = false;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

enum class CanonicalQuantifiedSubqueryQuantifier : std::uint8_t {
  kAny = 1,
  kAll,
};

struct CanonicalQuantifiedSubqueryRequest {
  CanonicalTableSubqueryRequest table_request;
  ExecutorColumnDescriptor left_operand_column;
  scratchbird::engine::internal_api::EngineTypedValue left_value;
  std::uint32_t right_expression_descriptor_id = 0;
  scratchbird::engine::internal_api::EngineComparisonPredicateOperator
      comparison_operator = scratchbird::engine::internal_api::
          EngineComparisonPredicateOperator::unspecified;
  CanonicalQuantifiedSubqueryQuantifier quantifier =
      CanonicalQuantifiedSubqueryQuantifier::kAny;
  std::uint32_t result_expression_descriptor_id = 0;
  ExecutorColumnDescriptor result_column;
  std::size_t maximum_comparison_count = 1048576;
};

struct CanonicalQuantifiedSubqueryResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  scratchbird::engine::internal_api::EngineSqlTruthValue truth_value =
      scratchbird::engine::internal_api::EngineSqlTruthValue::unspecified;
  std::size_t comparison_count = 0;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalCorrelatedSubqueryRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch outer_batch;
  DescriptorBatch inner_batch;
  std::size_t outer_binding_column = 0;
  std::uint32_t outer_binding_expression_descriptor_id = 0;
  std::size_t inner_reference_column = 0;
  std::uint32_t inner_reference_expression_descriptor_id = 0;
  std::size_t maximum_scope_execution_count = 1048576;
  std::size_t maximum_comparison_count = 1048576;
  std::size_t maximum_result_row_count = 1048576;
};

struct CanonicalCorrelatedScopeResult {
  std::size_t outer_row_index = 0;
  scratchbird::engine::internal_api::EngineTypedValue bound_outer_value;
  DescriptorBatch output_batch;
};

struct CanonicalCorrelatedSubqueryResult {
  DescriptorRuntimeDiagnostic diagnostic;
  std::vector<CanonicalCorrelatedScopeResult> scopes;
  std::size_t scope_execution_count = 0;
  std::size_t comparison_count = 0;
  std::size_t result_row_count = 0;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

enum class CanonicalLateralJoinForm : std::uint8_t {
  kInnerLateral = 0,
  kLeftLateral = 1,
  kCrossApply = 2,
  kOuterApply = 3,
};

struct CanonicalLateralSubqueryRequest {
  CanonicalCorrelatedSubqueryRequest correlated_request;
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  CanonicalLateralJoinForm form = CanonicalLateralJoinForm::kInnerLateral;
  std::size_t maximum_output_row_count = 1048576;
};

struct CanonicalLateralSubqueryResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  CanonicalLateralJoinForm form = CanonicalLateralJoinForm::kInnerLateral;
  std::size_t scope_execution_count = 0;
  std::size_t matched_scope_count = 0;
  std::size_t null_extended_outer_row_count = 0;
  std::size_t output_row_count = 0;
  std::string correlated_plan_uuid;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

using CanonicalRecursiveCteStep =
    std::function<DescriptorBatch(const DescriptorBatch&, std::size_t)>;

struct CanonicalRecursiveCteWorkingRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch anchor_batch;
  CanonicalRecursiveCteStep recursive_step;
  std::size_t maximum_iteration_count = 0;
  std::size_t maximum_working_row_count = 0;
  std::size_t maximum_result_row_count = 0;
};

struct CanonicalRecursiveCteIteration {
  std::size_t iteration_ordinal = 0;
  std::size_t working_row_count = 0;
  std::size_t intermediate_row_count = 0;
};

struct CanonicalRecursiveCteWorkingResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::vector<CanonicalRecursiveCteIteration> iterations;
  std::size_t recursive_iteration_count = 0;
  std::size_t maximum_observed_working_row_count = 0;
  bool converged = false;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

enum class CanonicalRecursiveCteUnionMode : std::uint8_t {
  kAll = 1,
  kDistinct,
};

struct CanonicalRecursiveCteUnionRequest {
  CanonicalRecursiveCteWorkingRequest working_request;
  CanonicalRecursiveCteUnionMode union_mode =
      CanonicalRecursiveCteUnionMode::kAll;
};

struct CanonicalRecursiveCteUnionResult {
  CanonicalRecursiveCteWorkingResult working_result;
  CanonicalRecursiveCteUnionMode union_mode =
      CanonicalRecursiveCteUnionMode::kAll;
  std::size_t duplicate_row_count = 0;
};

struct CanonicalRecursiveCteGeneratedBatch {
  DescriptorBatch batch;
  std::vector<std::size_t> parent_working_row_indices;
};

using CanonicalRecursiveCteSearchCycleStep = std::function<
    CanonicalRecursiveCteGeneratedBatch(const DescriptorBatch&, std::size_t)>;

enum class CanonicalRecursiveCteSearchOrder : std::uint8_t {
  kBreadthFirst = 1,
  kDepthFirst,
};

struct CanonicalRecursiveCteSearchCycleRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch anchor_batch;
  CanonicalRecursiveCteSearchCycleStep recursive_step;
  CanonicalRecursiveCteSearchOrder search_order =
      CanonicalRecursiveCteSearchOrder::kBreadthFirst;
  std::size_t cycle_key_column = 0;
  std::uint32_t cycle_key_expression_descriptor_id = 0;
  ExecutorColumnDescriptor search_sequence_column;
  ExecutorColumnDescriptor cycle_mark_column;
  std::size_t maximum_iteration_count = 0;
  std::size_t maximum_working_row_count = 0;
  std::size_t maximum_result_row_count = 0;
};

struct CanonicalRecursiveCteSearchCycleMetadata {
  std::size_t output_row_index = 0;
  std::size_t depth = 0;
  std::uint64_t breadth_first_sequence = 0;
  bool cycle = false;
};

struct CanonicalRecursiveCteSearchCycleResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::vector<CanonicalRecursiveCteSearchCycleMetadata> row_metadata;
  std::size_t recursive_iteration_count = 0;
  std::size_t cycle_row_count = 0;
  bool converged = false;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalRecursiveCteResourceRequest {
  CanonicalRecursiveCteWorkingRequest working_request;
  std::string memory_grant_evidence_uuid;
  std::size_t maximum_materialized_value_bytes = 0;
};

struct CanonicalRecursiveCteResourceResult {
  CanonicalRecursiveCteWorkingResult working_result;
  std::size_t materialized_value_bytes = 0;
  bool working_state_cleaned = false;
  std::string memory_grant_evidence_uuid;
};

using CanonicalRecursiveCteCancellationProbe =
    std::function<bool(std::size_t)>;

struct CanonicalRecursiveCteCancellationRequest {
  CanonicalRecursiveCteWorkingRequest working_request;
  CanonicalRecursiveCteCancellationProbe cancellation_requested;
  std::string cancellation_evidence_uuid;
};

struct CanonicalRecursiveCteCancellationResult {
  CanonicalRecursiveCteWorkingResult working_result;
  bool cancelled = false;
  std::size_t cancellation_iteration_ordinal = 0;
  bool working_state_cleaned = false;
  std::string cancellation_evidence_uuid;
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
  std::string collation_uuid;
  std::uint64_t resource_epoch = 0;
  std::uint64_t collation_epoch = 0;
  scratchbird::core::datatypes::DatatypeTextSeedAuthority text_seed;
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
  kCross = 1,
  kInner,
  kLeftOuter,
  kRightOuter,
  kFullOuter,
  kLeftSemi,
  kLeftAnti,
};

struct CanonicalJoinKindRequest {
  CanonicalJoinResidualRequest residual_request;
  CanonicalAcceptedJoinKind join_kind =
      CanonicalAcceptedJoinKind::kLeftOuter;
  bool conditionless_predicate = false;
  std::size_t maximum_output_rows = 1048576;
};

struct CanonicalJoinKindResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::size_t matched_pair_count = 0;
  std::size_t unmatched_left_row_count = 0;
  std::size_t unmatched_right_row_count = 0;
  std::size_t emitted_left_row_count = 0;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

enum class CanonicalNamedJoinForm : std::uint8_t {
  kUsing = 1,
  kNatural,
};

struct CanonicalNamedJoinBinding {
  std::string normalized_name;
  CanonicalCompositeJoinKeyTerm key_term;
  ExecutorColumnDescriptor result_column;
};

struct CanonicalNamedJoinRequest {
  CanonicalCompositeJoinKeyRequest key_request;
  CanonicalAcceptedJoinKind join_kind = CanonicalAcceptedJoinKind::kInner;
  CanonicalNamedJoinForm form = CanonicalNamedJoinForm::kUsing;
  std::vector<CanonicalNamedJoinBinding> bindings;
  std::string binding_evidence_uuid;
  TypedPhysicalNodeDag projection_dag;
  std::uint64_t selected_projection_node_id = 0;
  std::size_t maximum_binding_count = 64;
  std::size_t maximum_candidate_rechecks = 1048576;
  std::size_t maximum_output_rows = 1048576;
};

struct CanonicalNamedJoinResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  CanonicalNamedJoinForm form = CanonicalNamedJoinForm::kUsing;
  std::size_t binding_count = 0;
  std::size_t matched_pair_count = 0;
  std::size_t unmatched_left_row_count = 0;
  std::size_t unmatched_right_row_count = 0;
  std::string binding_evidence_uuid;
  std::string selected_plan_uuid;
  std::uint64_t executed_join_node_id = 0;
  std::uint64_t join_causal_counter_id = 0;
  std::uint64_t executed_projection_node_id = 0;
  std::uint64_t projection_causal_counter_id = 0;
};

enum class CanonicalJoinStrategyKind : std::uint8_t {
  kHashInnerInt64Equality = 1,
  kNestedLoopInner,
  kMergeInnerInt64Equality,
  kHashTypedCompositeEquality,
  kMergeTypedCompositeEquality,
};

struct CanonicalJoinStrategyRequest {
  CanonicalJoinResidualRequest residual_request;
  CanonicalJoinStrategyKind strategy =
      CanonicalJoinStrategyKind::kHashInnerInt64Equality;
  // The algorithm enum retains its V1 inner-oriented names for ABI stability;
  // this field binds the accepted non-cross join semantics selected for it.
  CanonicalAcceptedJoinKind join_kind =
      CanonicalAcceptedJoinKind::kInner;
  std::size_t maximum_hash_entries = 1048576;
  std::size_t maximum_retained_entries = 1048576;
  std::size_t maximum_candidate_probes = 1048576;
  std::size_t maximum_strategy_key_comparisons = 1048576;
  std::size_t maximum_output_rows = 1048576;
};

struct CanonicalJoinStrategyResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::vector<std::size_t> canonical_pair_indices;
  std::vector<std::size_t> strategy_pair_indices;
  std::size_t hash_entry_count = 0;
  std::size_t retained_entry_count = 0;
  std::size_t candidate_probe_count = 0;
  std::size_t strategy_key_comparison_count = 0;
  bool canonical_multiset_proven = false;
  bool canonical_output_proven = false;
  CanonicalAcceptedJoinKind join_kind =
      CanonicalAcceptedJoinKind::kInner;
  std::size_t matched_pair_count = 0;
  std::size_t unmatched_left_row_count = 0;
  std::size_t unmatched_right_row_count = 0;
  std::size_t emitted_left_row_count = 0;
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

enum class CanonicalScanCandidateSource : std::uint8_t {
  kRelationPage = 1,
  kIndexEntry,
};

struct CanonicalScanCandidateEvidence {
  std::string candidate_uuid;
  std::string record_uuid;
  std::string relation_uuid;
  std::string visibility_decision_uuid;
  std::uint64_t row_version_id = 0;
  std::uint64_t candidate_generation = 0;
  std::uint64_t observed_generation = 0;
  CanonicalScanCandidateSource source =
      CanonicalScanCandidateSource::kRelationPage;
  CanonicalMgaVisibilityDecision visibility =
      CanonicalMgaVisibilityDecision::kIndeterminate;
  CanonicalMgaSecurityDecision security_decision =
      CanonicalMgaSecurityDecision::kIndeterminate;
  scratchbird::engine::internal_api::EngineSqlTruthValue residual_truth =
      scratchbird::engine::internal_api::EngineSqlTruthValue::unspecified;
  bool locator_identity_matches = false;
};

struct CanonicalScanAccessRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  std::string available_implementation_id;
  std::string relation_uuid;
  std::uint64_t inventory_local_transaction_id = 0;
  std::uint64_t inventory_statement_snapshot_id = 0;
  std::uint64_t selected_descriptor_generation = 0;
  std::uint64_t current_descriptor_generation = 0;
  std::vector<CanonicalScanCandidateEvidence> candidates;
  std::size_t maximum_candidate_count = 1048576;
};

struct CanonicalScanAccessCounters {
  std::size_t candidate_count = 0;
  std::size_t visibility_recheck_count = 0;
  std::size_t invisible_filtered_count = 0;
  std::size_t stale_index_filtered_count = 0;
  std::size_t security_filtered_count = 0;
  std::size_t residual_filtered_count = 0;
  std::size_t emitted_count = 0;
};

struct CanonicalScanAccessAuthorityEvidence {
  bool engine_mga_snapshot_bound = false;
  bool visibility_rechecks_complete = false;
  bool owns_transaction_finality = false;
  bool owns_recovery = false;
  bool owns_parser_execution = false;
  bool index_or_cache_is_visibility_authority = false;
  bool wal_is_visibility_or_recovery_authority = false;
};

struct CanonicalScanAccessResult {
  DescriptorRuntimeDiagnostic diagnostic;
  std::vector<std::string> accepted_record_uuids;
  std::vector<std::uint64_t> accepted_row_version_ids;
  CanonicalScanAccessCounters counters;
  CanonicalScanAccessAuthorityEvidence authority;
  bool replan_required = false;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalPhysicalDispatchInput {
  std::uint64_t physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
  std::uint64_t result_handle_id = 0;
  std::vector<std::uint32_t> output_descriptor_ids;
  // A selected physical operator consumes the typed batch produced by each
  // input node.  The stable handle and descriptor identities remain the
  // causal ABI; this payload is the engine-owned value channel and is never
  // reconstructed from parser text or an opaque handle.
  std::optional<DescriptorBatch> materialized_output_batch;
};

struct CanonicalPhysicalDispatchAuthorityEvidence {
  bool engine_mga_snapshot_bound = false;
  bool owns_transaction_finality = false;
  bool owns_recovery = false;
  bool owns_parser_execution = false;
  bool owns_visibility_outside_engine_mga = false;
  bool wal_is_transaction_or_recovery_authority = false;
};

struct CanonicalPhysicalDispatchStepResult {
  DescriptorRuntimeDiagnostic diagnostic;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
  std::uint64_t result_handle_id = 0;
  std::vector<std::uint32_t> output_descriptor_ids;
  CanonicalPhysicalDispatchAuthorityEvidence authority;
  std::size_t execution_ordinal = 0;
  std::uint64_t input_row_count = 0;
  std::uint64_t output_row_count = 0;
  std::uint64_t rows_examined = 0;
  std::uint64_t pages_read = 0;
  std::uint64_t spill_bytes = 0;
  // Only an engine executor may attach a materialized typed batch. The
  // canonical selected-DAG route consumes the root batch at the shared result
  // ABI boundary; the dispatcher never reconstructs it from an opaque handle.
  std::optional<DescriptorBatch> materialized_output_batch;
  bool execution_started = false;
  bool execution_finished = false;
  bool counters_captured_after_finish = false;
};

using CanonicalPhysicalNodeExecutor = std::function<
    CanonicalPhysicalDispatchStepResult(
        const TypedPhysicalNodeDag&,
        const PhysicalNodeRecord&,
        const std::vector<CanonicalPhysicalDispatchInput>&)>;

struct CanonicalPhysicalExecutorRegistration {
  PhysicalNodeKind node_kind = PhysicalNodeKind::kValues;
  std::string implementation_id;
  CanonicalPhysicalNodeExecutor execute;
  std::string executor_capability_uuid;
  std::uint32_t executor_capability_abi_version = 0;
  bool engine_owned = false;
  bool accepts_optimizer_publication_v2 = false;
};

struct CanonicalPhysicalDagDispatchRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t inventory_local_transaction_id = 0;
  std::uint64_t inventory_statement_snapshot_id = 0;
  PhysicalNodeAbiLimits limits;
  std::vector<CanonicalPhysicalExecutorRegistration> available_executors;
};

struct CanonicalPhysicalDagDispatchResult {
  DescriptorRuntimeDiagnostic diagnostic;
  std::vector<CanonicalPhysicalDispatchStepResult> executed_steps;
  std::uint64_t root_result_handle_id = 0;
  std::vector<std::uint32_t> root_output_descriptor_ids;
  CanonicalPhysicalDispatchAuthorityEvidence authority;
  bool replan_required = false;
  bool execution_started = false;
  bool data_access_observed = false;
  std::string selected_plan_uuid;
  std::uint64_t executed_root_physical_node_id = 0;
  std::uint64_t root_causal_counter_id = 0;
};

struct CanonicalRecursiveCteMgaIterationEvidence {
  std::size_t iteration_ordinal = 0;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t statement_snapshot_id = 0;
  CanonicalMgaVisibilityDecision visibility =
      CanonicalMgaVisibilityDecision::kIndeterminate;
  CanonicalMgaSecurityDecision security_decision =
      CanonicalMgaSecurityDecision::kIndeterminate;
  std::string engine_evidence_uuid;
};

struct CanonicalRecursiveCteMgaRequest {
  CanonicalRecursiveCteWorkingRequest working_request;
  std::uint64_t transaction_inventory_id = 0;
  std::uint64_t inventory_local_transaction_id = 0;
  std::uint64_t inventory_statement_snapshot_id = 0;
  std::string transaction_inventory_evidence_uuid;
  std::vector<CanonicalRecursiveCteMgaIterationEvidence> iteration_evidence;
  std::size_t maximum_boundary_rechecks = 1048576;
};

struct CanonicalRecursiveCteMgaResult {
  CanonicalRecursiveCteWorkingResult working_result;
  std::size_t iteration_evidence_count = 0;
  bool mga_boundary_proven = false;
  std::string transaction_inventory_evidence_uuid;
};

enum class CanonicalSetOperationKind : std::uint8_t {
  kUnion = 1,
  kIntersect,
  kExcept,
};

enum class CanonicalSetOperationAlignment : std::uint8_t {
  kOrdinal = 1,
  kByName,
};

enum class CanonicalSetOperationQuantifier : std::uint8_t {
  kAll = 1,
  kDistinct,
};

enum class CanonicalSetOperationEqualityProfile : std::uint8_t {
  kExactTyped = 1,
  kNullEqualBoundCollation,
};

enum class CanonicalSetOperationTypeProfile : std::uint8_t {
  kExact = 1,
  kLosslessImplicit,
};

struct CanonicalSetOperationCollationBinding {
  std::size_t result_column = 0;
  std::string collation_uuid;
  std::uint64_t resource_epoch = 0;
  std::uint64_t collation_epoch = 0;
  scratchbird::core::datatypes::DatatypeTextSeedAuthority text_seed;
};

struct CanonicalSetOperationAllRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch left_batch;
  DescriptorBatch right_batch;
  std::vector<ExecutorColumnDescriptor> result_columns;
  CanonicalSetOperationKind operation = CanonicalSetOperationKind::kUnion;
  CanonicalSetOperationAlignment alignment =
      CanonicalSetOperationAlignment::kOrdinal;
  CanonicalSetOperationQuantifier quantifier =
      CanonicalSetOperationQuantifier::kAll;
  CanonicalSetOperationEqualityProfile equality_profile =
      CanonicalSetOperationEqualityProfile::kExactTyped;
  CanonicalSetOperationTypeProfile type_profile =
      CanonicalSetOperationTypeProfile::kExact;
  std::vector<CanonicalSetOperationCollationBinding> collation_bindings;
  std::size_t maximum_equality_comparison_count = 1048576;
  std::size_t maximum_output_row_count = 1048576;
};

struct CanonicalSetOperationAllResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::size_t left_input_row_count = 0;
  std::size_t right_input_row_count = 0;
  std::size_t consumed_right_multiplicity_count = 0;
  std::size_t eliminated_duplicate_row_count = 0;
  std::size_t equality_comparison_count = 0;
  std::size_t coerced_value_count = 0;
  std::vector<std::string> reconciled_type_names;
  std::vector<std::size_t> right_to_result_column_indices;
  std::string implementation_id;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

enum class CanonicalSetOperationNestingRule : std::uint8_t {
  kSqlPrecedence = 1,
  kExplicitLeft,
  kExplicitRight,
};

struct CanonicalSetOperationNestingRequest {
  DescriptorBatch first_operand;
  DescriptorBatch second_operand;
  DescriptorBatch third_operand;
  CanonicalSetOperationKind first_operation =
      CanonicalSetOperationKind::kUnion;
  CanonicalSetOperationKind second_operation =
      CanonicalSetOperationKind::kUnion;
  CanonicalSetOperationQuantifier first_quantifier =
      CanonicalSetOperationQuantifier::kDistinct;
  CanonicalSetOperationQuantifier second_quantifier =
      CanonicalSetOperationQuantifier::kDistinct;
  CanonicalSetOperationAlignment first_alignment =
      CanonicalSetOperationAlignment::kOrdinal;
  CanonicalSetOperationAlignment second_alignment =
      CanonicalSetOperationAlignment::kOrdinal;
  CanonicalSetOperationNestingRule nesting_rule =
      CanonicalSetOperationNestingRule::kSqlPrecedence;
  CanonicalSetOperationAllRequest inner_request_template;
  CanonicalSetOperationAllRequest outer_request_template;
  std::size_t maximum_intermediate_row_count = 1048576;
};

struct CanonicalSetOperationNestingResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  CanonicalSetOperationNestingRule resolved_nesting_rule =
      CanonicalSetOperationNestingRule::kSqlPrecedence;
  std::size_t intermediate_row_count = 0;
  std::uint64_t inner_physical_node_id = 0;
  std::uint64_t outer_physical_node_id = 0;
  std::uint64_t inner_causal_counter_id = 0;
  std::uint64_t outer_causal_counter_id = 0;
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

struct CanonicalJoinMgaInputRowEvidence {
  std::size_t row_index = 0;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t statement_snapshot_id = 0;
  std::uint64_t row_version_id = 0;
  CanonicalMgaVisibilityDecision visibility =
      CanonicalMgaVisibilityDecision::kIndeterminate;
  CanonicalMgaSecurityDecision security_decision =
      CanonicalMgaSecurityDecision::kIndeterminate;
  std::uint64_t candidate_generation = 0;
  std::uint64_t current_generation = 0;
  std::string engine_evidence_uuid;
};

struct CanonicalJoinMgaRequest {
  CanonicalJoinStrategyRequest strategy_request;
  std::uint64_t transaction_inventory_id = 0;
  std::uint64_t inventory_local_transaction_id = 0;
  std::uint64_t inventory_statement_snapshot_id = 0;
  std::string transaction_inventory_evidence_uuid;
  bool input_row_evidence_profile = false;
  std::vector<CanonicalJoinMgaInputRowEvidence> left_row_evidence;
  std::vector<CanonicalJoinMgaInputRowEvidence> right_row_evidence;
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
  std::size_t visible_left_row_count = 0;
  std::size_t visible_right_row_count = 0;
  std::size_t visibility_filtered_left_row_count = 0;
  std::size_t visibility_filtered_right_row_count = 0;
  std::size_t security_filtered_left_row_count = 0;
  std::size_t security_filtered_right_row_count = 0;
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

enum class CanonicalAggregateFunction : std::uint8_t {
  unknown = 0,
  count,
  sum,
  avg,
  min,
  max,
  bool_and,
  bool_or,
  array_agg,
  string_agg,
  json_agg,
  json_object_agg,
  stddev_pop,
  variance_pop,
  every,
  listagg,
  rank,
  dense_rank,
  percent_rank,
  cume_dist,
  mode,
  percentile_cont,
  percentile_disc,
  approx_count_distinct,
  approx_median,
  approx_percentile_cont,
  approx_percentile_disc,
  approx_top_k,
  stddev,
  variance,
  stddev_samp,
  variance_samp,
  corr,
  covar_pop,
  covar_samp,
  regr_count,
  regr_avgx,
  regr_avgy,
  regr_intercept,
  regr_r2,
  regr_slope,
  regr_sxx,
  regr_sxy,
  regr_syy,
};

enum class CanonicalAggregateExecutionStrategy : std::uint8_t {
  unknown = 0,
  serial,
  partitioned_combine,
};

enum class CanonicalListaggOverflowMode : std::uint8_t {
  none = 0,
  error,
  truncate,
};

struct CanonicalAggregateRegistryEntry {
  std::uint16_t abi_version = 0;
  CanonicalAggregateFunction function = CanonicalAggregateFunction::unknown;
  std::string builtin_id;
  std::string function_uuid;
  bool executable = false;
  bool aggregate_as_window = false;
  bool moving_window_inverse = false;
};

struct CanonicalAggregateDescriptor {
  std::uint16_t abi_version = 0;
  CanonicalAggregateFunction function = CanonicalAggregateFunction::unknown;
  std::string builtin_id;
  std::string function_uuid;
  bool count_star = false;
};

struct CanonicalAggregateRuntimeRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  CanonicalAggregateDescriptor descriptor;
  DescriptorBatch input_batch;
  std::vector<std::size_t> value_columns;
  std::vector<std::uint32_t> value_expression_descriptor_ids;
  std::vector<scratchbird::engine::internal_api::EngineTypedValue>
      direct_arguments;
  ExecutorColumnDescriptor result_column;
  std::optional<
      std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>>
      filter_truth_values;
  bool distinct = false;
  std::vector<CanonicalDescriptorOrderTerm> aggregate_order_terms;
  std::string aggregate_separator = ",";
  CanonicalListaggOverflowMode listagg_overflow_mode =
      CanonicalListaggOverflowMode::none;
  std::size_t listagg_max_output_bytes = 0;
  std::string listagg_truncation_indicator = "...";
  bool listagg_with_count = true;
  CanonicalAggregateExecutionStrategy forced_strategy =
      CanonicalAggregateExecutionStrategy::serial;
  std::size_t maximum_transition_count = 1048576;
  std::size_t maximum_distinct_value_count = 1048576;
  std::size_t maximum_order_comparison_count = 1048576;
  std::size_t maximum_state_bytes = 16777216;
  bool parser_execution_authority_claimed = false;
  bool transaction_finality_claimed = false;
  bool recovery_authority_claimed = false;
};

struct CanonicalAggregateRuntimeResult {
  DescriptorRuntimeDiagnostic diagnostic;
  CanonicalAggregateDescriptor descriptor;
  DescriptorBatch output_batch;
  CanonicalAggregateExecutionStrategy executed_strategy =
      CanonicalAggregateExecutionStrategy::unknown;
  std::size_t input_row_count = 0;
  std::size_t filtered_row_count = 0;
  std::size_t transition_count = 0;
  std::size_t non_null_transition_count = 0;
  std::size_t distinct_tuple_count = 0;
  std::size_t direct_argument_count = 0;
  std::size_t order_comparison_count = 0;
  std::size_t state_bytes = 0;
  bool every_descriptor_field_consumed = false;
  bool filter_applied_before_distinct = false;
  bool aggregate_order_applied = false;
  bool shared_state_authority_used = false;
  CanonicalPhysicalDispatchAuthorityEvidence authority;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalAggregateStateSpillRequest {
  CanonicalAggregateRuntimeRequest aggregate_request;
  std::filesystem::path spill_root;
  std::string spill_owner_uuid;
  std::uint64_t runtime_generation = 0;
  std::uint64_t reopen_runtime_generation = 0;
  std::uint64_t memory_quota_bytes = 0;
  std::size_t maximum_serialized_state_bytes = 16777216;
  std::size_t maximum_spill_record_count = 16777216;
  bool cancellation_requested = false;
  bool cleanup_after_cancellation = true;
  bool restart_recovery_proof_available = true;
};

struct CanonicalAggregateStateSpillResult {
  DescriptorRuntimeDiagnostic diagnostic;
  CanonicalAggregateRuntimeResult aggregate_result;
  std::size_t serialized_state_bytes = 0;
  std::size_t spilled_state_record_count = 0;
  bool state_serialized = false;
  bool spilled = false;
  bool spill_reopened = false;
  bool state_restored = false;
  bool restored_result_equivalent = false;
  bool cleanup_proven = false;
  bool cancellation_observed = false;
  std::vector<std::string> spill_evidence;
};

struct CanonicalAggregateStateExchangeRequest {
  CanonicalAggregateRuntimeRequest aggregate_request;
  std::vector<std::uint32_t> worker_ordinals;
  std::uint64_t exchange_generation = 0;
  std::uint64_t coordinator_exchange_generation = 0;
  std::size_t maximum_partial_state_count = 1024;
  std::size_t maximum_serialized_state_bytes_per_worker = 16777216;
  std::size_t maximum_combined_serialized_state_bytes = 67108864;
  bool cancellation_requested = false;
};

struct CanonicalAggregateStateExchangeResult {
  DescriptorRuntimeDiagnostic diagnostic;
  CanonicalAggregateRuntimeResult aggregate_result;
  std::size_t partial_state_count = 0;
  std::size_t restored_partial_state_count = 0;
  std::size_t merged_partial_state_count = 0;
  std::size_t serialized_state_bytes = 0;
  std::vector<std::size_t> worker_transition_counts;
  std::vector<std::size_t> worker_state_bytes;
  std::vector<std::size_t> worker_serialized_state_bytes;
  bool states_serialized = false;
  bool exchange_identity_proven = false;
  bool all_states_restored = false;
  bool deterministic_merge_order_proven = false;
  bool merged_result_equivalent = false;
  bool cancellation_observed = false;
};

struct CanonicalAggregateMovingRuntimeRequest {
  CanonicalAggregateRuntimeRequest aggregate_request;
  std::vector<std::vector<std::size_t>> effective_frame_row_indices;
  std::size_t maximum_output_rows = 1048576;
  std::size_t maximum_addition_transition_count = 8388608;
  std::size_t maximum_inverse_transition_count = 8388608;
  std::size_t maximum_cumulative_state_bytes = 268435456;
};

struct CanonicalAggregateMovingRuntimeResult {
  DescriptorRuntimeDiagnostic diagnostic;
  CanonicalAggregateDescriptor descriptor;
  std::vector<scratchbird::engine::internal_api::EngineTypedValue> values;
  std::size_t addition_transition_count = 0;
  std::size_t inverse_transition_count = 0;
  std::size_t cumulative_state_bytes = 0;
  std::size_t maximum_retained_state_bytes = 0;
  bool moving_inverse_state_used = false;
  bool frame_recomputation_used = false;
  CanonicalPhysicalDispatchAuthorityEvidence authority;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalAggregateGroupingSet {
  // Strictly increasing ordinals into group_key_terms.  An empty vector is
  // the grand-total grouping set.
  std::vector<std::size_t> key_term_ordinals;
};

struct CanonicalGroupedAggregateRuntimeRequest {
  CanonicalAggregateRuntimeRequest aggregate_request;
  std::vector<CanonicalDescriptorOrderTerm> group_key_terms;
  std::vector<ExecutorColumnDescriptor> group_result_columns;
  std::vector<CanonicalAggregateGroupingSet> grouping_sets;
  std::size_t maximum_group_count = 65536;
  std::size_t maximum_grouping_key_comparison_count = 1048576;
  std::size_t maximum_grouping_set_transition_count = 1048576;
  std::size_t maximum_combined_distinct_tuple_count = 1048576;
  std::size_t maximum_combined_order_comparison_count = 1048576;
  std::size_t maximum_combined_state_bytes = 67108864;
  std::size_t maximum_output_rows = 65536;
};

struct CanonicalGroupedAggregateMetadata {
  std::uint32_t grouping_set_ordinal = 0;
  std::uint64_t grouping_id = 0;
  std::vector<bool> grouping_indicators;
  std::vector<std::size_t> source_row_indices;
  std::size_t source_row_count = 0;
  std::size_t aggregate_transition_count = 0;
  std::size_t aggregate_state_bytes = 0;
};

struct CanonicalGroupedAggregateRuntimeResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::vector<CanonicalGroupedAggregateMetadata> groups;
  std::size_t grouping_set_count = 0;
  std::size_t grouping_key_comparison_count = 0;
  std::size_t grouping_set_transition_count = 0;
  std::size_t aggregate_transition_count = 0;
  std::size_t aggregate_distinct_tuple_count = 0;
  std::size_t aggregate_order_comparison_count = 0;
  std::size_t combined_state_bytes = 0;
  bool aggregate_state_spill_required = false;
  bool shared_state_authority_used = false;
  CanonicalPhysicalDispatchAuthorityEvidence authority;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalGroupedAggregateSetRuntimeRequest {
  CanonicalGroupedAggregateRuntimeRequest first_aggregate;
  // Additional aggregate specifications must not carry a second physical DAG,
  // selected node, or input batch; those authorities are shared from first.
  std::vector<CanonicalAggregateRuntimeRequest> additional_aggregates;
  std::size_t maximum_aggregate_count = 64;
  std::size_t maximum_combined_grouping_set_transition_count = 8388608;
  std::size_t maximum_combined_grouping_key_comparison_count = 8388608;
  std::size_t maximum_combined_aggregate_transition_count = 8388608;
  std::size_t maximum_combined_distinct_tuple_count = 8388608;
  std::size_t maximum_combined_order_comparison_count = 8388608;
  std::size_t maximum_combined_state_bytes = 268435456;
};

struct CanonicalGroupedAggregateSetMetadata {
  std::uint32_t grouping_set_ordinal = 0;
  std::uint64_t grouping_id = 0;
  std::vector<bool> grouping_indicators;
  std::vector<std::size_t> source_row_indices;
  std::size_t source_row_count = 0;
  std::vector<std::size_t> aggregate_transition_counts;
  std::vector<std::size_t> aggregate_state_bytes;
};

struct CanonicalGroupedAggregateSetRuntimeResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::vector<CanonicalGroupedAggregateSetMetadata> groups;
  std::size_t aggregate_count = 0;
  std::size_t grouping_set_transition_count = 0;
  std::size_t grouping_key_comparison_count = 0;
  std::size_t aggregate_transition_count = 0;
  std::size_t aggregate_distinct_tuple_count = 0;
  std::size_t aggregate_order_comparison_count = 0;
  std::size_t combined_state_bytes = 0;
  bool group_identity_proven = false;
  bool aggregate_state_spill_required = false;
  bool shared_state_authority_used = false;
  CanonicalPhysicalDispatchAuthorityEvidence authority;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

struct CanonicalGroupedAggregateSetStateSpillRequest {
  CanonicalGroupedAggregateSetRuntimeRequest grouped_request;
  std::filesystem::path spill_root;
  std::string spill_owner_uuid;
  std::uint64_t runtime_generation = 0;
  std::uint64_t reopen_runtime_generation = 0;
  std::uint64_t memory_quota_bytes = 0;
  std::size_t maximum_serialized_state_bytes = 67108864;
  std::size_t maximum_spill_record_count = 67108864;
  bool cancellation_requested = false;
  bool cleanup_after_cancellation = true;
  bool restart_recovery_proof_available = true;
};

struct CanonicalGroupedAggregateSetStateSpillResult {
  DescriptorRuntimeDiagnostic diagnostic;
  CanonicalGroupedAggregateSetRuntimeResult grouped_result;
  std::size_t spilled_aggregate_state_count = 0;
  std::size_t serialized_aggregate_state_bytes = 0;
  std::size_t spilled_aggregate_state_record_count = 0;
  bool spilled = false;
  bool spill_reopened = false;
  bool cleanup_proven = false;
  bool cancellation_observed = false;
  std::vector<std::string> spill_evidence;
};

struct CanonicalDescriptorOrderComparisonResult {
  DescriptorRuntimeDiagnostic diagnostic;
  int comparison = 0;
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

struct CanonicalWindowPartitionTerm {
  std::size_t column = 0;
  std::uint32_t expression_descriptor_id = 0;
  std::string collation_uuid;
  std::uint64_t resource_epoch = 0;
  std::uint64_t collation_epoch = 0;
  scratchbird::core::datatypes::DatatypeTextSeedAuthority text_seed;
};

struct CanonicalWindowRowPeerMetadata {
  std::size_t source_row_index = 0;
  std::size_t ordered_row_index = 0;
  std::optional<std::size_t> partition_id;
  std::optional<std::size_t> peer_group_id;
  std::size_t partition_begin = 0;
  std::size_t partition_end_exclusive = 0;
  std::size_t peer_begin = 0;
  std::size_t peer_end_exclusive = 0;
};

struct CanonicalWindowPartitionOrderRequest {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  DescriptorBatch input_batch;
  std::vector<CanonicalWindowPartitionTerm> partition_terms;
  std::vector<CanonicalDescriptorOrderTerm> order_terms;
  std::string window_property_uuid;
  std::string partition_property_uuid;
  std::string ordering_property_uuid;
  std::uint64_t inventory_local_transaction_id = 0;
  std::uint64_t inventory_statement_snapshot_id = 0;
  std::size_t maximum_term_count = 64;
  std::size_t maximum_pair_comparisons = 1048576;
  bool parser_execution_authority_claimed = false;
  bool transaction_finality_claimed = false;
  bool recovery_authority_claimed = false;
};

struct CanonicalWindowPartitionOrderResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch ordered_batch;
  std::vector<CanonicalWindowRowPeerMetadata> row_metadata;
  std::vector<CanonicalDescriptorOrderTerm> order_terms;
  std::size_t partition_count = 0;
  std::size_t peer_group_count = 0;
  std::string window_property_uuid;
  std::string partition_property_uuid;
  std::string ordering_property_uuid;
  bool explicit_peer_metadata = false;
  bool weaker_peer_recomputation_forbidden = false;
  bool final_query_order_guaranteed = false;
  CanonicalPhysicalDispatchAuthorityEvidence authority;
  std::uint64_t inventory_local_transaction_id = 0;
  std::uint64_t inventory_statement_snapshot_id = 0;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
};

enum class CanonicalWindowFrameUnit : std::uint8_t {
  rows = 1,
  range,
  groups,
};

enum class CanonicalWindowFrameBoundKind : std::uint8_t {
  unbounded_preceding = 1,
  offset_preceding,
  current_row,
  offset_following,
  unbounded_following,
};

enum class CanonicalWindowFrameExclusion : std::uint8_t {
  no_others = 1,
  current_row,
  group,
  ties,
};

enum class CanonicalWindowFrameState : std::uint8_t {
  nonempty = 1,
  empty,
  reversed_to_empty,
};

struct CanonicalWindowFrameBound {
  CanonicalWindowFrameBoundKind kind =
      CanonicalWindowFrameBoundKind::current_row;
  std::optional<scratchbird::engine::internal_api::EngineTypedValue> offset;
};

struct CanonicalWindowFrameDescriptor {
  std::string frame_descriptor_uuid;
  bool frame_specified = false;
  CanonicalWindowFrameUnit unit = CanonicalWindowFrameUnit::rows;
  std::optional<CanonicalWindowFrameBound> start;
  std::optional<CanonicalWindowFrameBound> end;
  CanonicalWindowFrameExclusion exclusion =
      CanonicalWindowFrameExclusion::no_others;
};

struct CanonicalWindowEffectiveFrame {
  std::size_t ordered_row_index = 0;
  std::optional<std::size_t> partition_id;
  CanonicalWindowFrameState base_state =
      CanonicalWindowFrameState::empty;
  std::optional<std::size_t> base_begin;
  std::optional<std::size_t> base_end_exclusive;
  std::vector<std::size_t> effective_row_indices;
  bool exclusion_applied = false;
};

struct CanonicalWindowFrameRequest {
  CanonicalWindowPartitionOrderResult partition_order;
  CanonicalWindowFrameDescriptor frame;
  std::size_t maximum_effective_row_references = 1048576;
  bool parser_execution_authority_claimed = false;
  bool transaction_finality_claimed = false;
  bool recovery_authority_claimed = false;
};

struct CanonicalWindowFrameResult {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch ordered_batch;
  std::vector<CanonicalWindowRowPeerMetadata> row_metadata;
  std::vector<CanonicalWindowEffectiveFrame> effective_frames;
  CanonicalWindowFrameDescriptor resolved_frame;
  std::string window_property_uuid;
  std::string ordering_property_uuid;
  bool defaulted_with_order = false;
  bool defaulted_without_order = false;
  bool every_frame_operand_consumed = false;
  bool empty_state_uses_optional_bounds = false;
  CanonicalPhysicalDispatchAuthorityEvidence authority;
  std::uint64_t inventory_local_transaction_id = 0;
  std::uint64_t inventory_statement_snapshot_id = 0;
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
CanonicalResultPublicationResult PublishCanonicalResultEnvelope(
    const CanonicalResultPublicationRequest& request);
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
CanonicalRowSubqueryResult ExecuteCanonicalRowSubquery(
    const CanonicalRowSubqueryRequest& request);
CanonicalExistsSubqueryResult ExecuteCanonicalExistsSubquery(
    const CanonicalExistsSubqueryRequest& request);
CanonicalQuantifiedSubqueryResult ExecuteCanonicalQuantifiedSubquery(
    const CanonicalQuantifiedSubqueryRequest& request);
CanonicalCorrelatedSubqueryResult ExecuteCanonicalCorrelatedSubquery(
    const CanonicalCorrelatedSubqueryRequest& request);
CanonicalLateralSubqueryResult ExecuteCanonicalLateralSubquery(
    const CanonicalLateralSubqueryRequest& request);
CanonicalRecursiveCteWorkingResult ExecuteCanonicalRecursiveCteWorking(
    const CanonicalRecursiveCteWorkingRequest& request);
CanonicalRecursiveCteUnionResult ExecuteCanonicalRecursiveCteUnion(
    const CanonicalRecursiveCteUnionRequest& request);
CanonicalRecursiveCteSearchCycleResult
ExecuteCanonicalRecursiveCteSearchCycle(
    const CanonicalRecursiveCteSearchCycleRequest& request);
CanonicalRecursiveCteResourceResult ExecuteCanonicalRecursiveCteResource(
    const CanonicalRecursiveCteResourceRequest& request);
CanonicalRecursiveCteCancellationResult
ExecuteCanonicalRecursiveCteCancellation(
    const CanonicalRecursiveCteCancellationRequest& request);
CanonicalRecursiveCteMgaResult ExecuteCanonicalRecursiveCteMgaBoundary(
    const CanonicalRecursiveCteMgaRequest& request);
CanonicalSetOperationAllResult ExecuteCanonicalSetOperationAll(
    const CanonicalSetOperationAllRequest& request);
CanonicalSetOperationAllResult ExecuteCanonicalSetOperationDistinct(
    const CanonicalSetOperationAllRequest& request);
CanonicalSetOperationNestingResult ExecuteCanonicalSetOperationNesting(
    const CanonicalSetOperationNestingRequest& request);
CanonicalDescriptorFetchProfileResult ExecuteCanonicalDescriptorFetchProfile(
    const CanonicalDescriptorFetchProfileRequest& request);
CanonicalDescriptorCountResult ExecuteCanonicalDescriptorCountStar(
    const CanonicalDescriptorCountRequest& request);
std::vector<CanonicalAggregateRegistryEntry>
CanonicalAggregateRuntimeRegistryV1();
CanonicalAggregateRuntimeResult ExecuteCanonicalAggregateRuntime(
    const CanonicalAggregateRuntimeRequest& request);
CanonicalAggregateStateSpillResult ExecuteCanonicalAggregateStateSpill(
    const CanonicalAggregateStateSpillRequest& request);
CanonicalAggregateStateExchangeResult ExecuteCanonicalAggregateStateExchange(
    const CanonicalAggregateStateExchangeRequest& request);
CanonicalAggregateMovingRuntimeResult ExecuteCanonicalAggregateMovingRuntime(
    const CanonicalAggregateMovingRuntimeRequest& request);
CanonicalGroupedAggregateRuntimeResult ExecuteCanonicalGroupedAggregateRuntime(
    const CanonicalGroupedAggregateRuntimeRequest& request);
CanonicalGroupedAggregateSetRuntimeResult
ExecuteCanonicalGroupedAggregateSetRuntime(
    const CanonicalGroupedAggregateSetRuntimeRequest& request);
CanonicalGroupedAggregateSetStateSpillResult
ExecuteCanonicalGroupedAggregateSetStateSpill(
    const CanonicalGroupedAggregateSetStateSpillRequest& request);
CanonicalScanAccessResult ExecuteCanonicalSelectedScanAccess(
    const CanonicalScanAccessRequest& request);
CanonicalPhysicalDagDispatchResult ExecuteCanonicalPhysicalDag(
    const CanonicalPhysicalDagDispatchRequest& request);
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
CanonicalNamedJoinResult ExecuteCanonicalNamedJoin(
    const CanonicalNamedJoinRequest& request);
CanonicalJoinStrategyResult ExecuteCanonicalJoinStrategy(
    const CanonicalJoinStrategyRequest& request);
CanonicalJoinMgaResult ExecuteCanonicalJoinMgaBoundary(
    const CanonicalJoinMgaRequest& request);
CanonicalDescriptorRowNumberResult ExecuteCanonicalDescriptorRowNumber(
    const CanonicalDescriptorRowNumberRequest& request);
CanonicalDescriptorSortResult ExecuteCanonicalDescriptorSort(
    const CanonicalDescriptorSortRequest& request);
DescriptorRuntimeDiagnostic ValidateCanonicalDescriptorOrderTerm(
    const CanonicalDescriptorOrderTerm& term,
    const ExecutorColumnDescriptor& column);
CanonicalDescriptorOrderComparisonResult CompareCanonicalDescriptorOrderValues(
    const scratchbird::engine::internal_api::EngineTypedValue& left,
    const scratchbird::engine::internal_api::EngineTypedValue& right,
    const CanonicalDescriptorOrderTerm& term);
CanonicalWindowPartitionOrderResult ExecuteCanonicalWindowPartitionOrder(
    const CanonicalWindowPartitionOrderRequest& request);
CanonicalWindowFrameResult ExecuteCanonicalWindowFrames(
    const CanonicalWindowFrameRequest& request);
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
