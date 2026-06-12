// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "executor_batch_join_dml.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace exec = scratchbird::engine::executor;

constexpr std::string_view kGateSearchKey =
    "DPC_EXECUTOR_BATCH_JOIN_DML_GATE";
constexpr std::string_view kImplementationSearchKey =
    "DPC_EXECUTOR_BATCH_JOIN_DML";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

exec::Batch LeftJoinCorpus() {
  return exec::MakeBatch("dpc_executor_batch_join_left_v1",
                         {
                             {.values = {1, 100}},
                             {.values = {2, 200}},
                             {.values = {1, 101}},
                             {.values = {3, 300}},
                             {.values = {2, 201}},
                         });
}

exec::Batch RightJoinCorpus() {
  return exec::MakeBatch("dpc_executor_batch_join_right_v1",
                         {
                             {.values = {1, 10}},
                             {.values = {2, 20}},
                             {.values = {1, 11}},
                             {.values = {4, 40}},
                             {.values = {2, 21}},
                         });
}

exec::Batch DmlCorpus() {
  return exec::MakeBatch("dpc_executor_batch_dml_input_v1",
                         {
                             {.values = {10, 1}},
                             {.values = {11, 2}},
                             {.values = {12, 3}},
                             {.values = {13, 4}},
                         });
}

exec::ExecutorBatchRequest BatchRequest() {
  exec::ExecutorBatchRequest request;
  request.requested_mode = exec::ExecutorBatchRequestMode::kPreferBatch;
  request.node_supports_batch = true;
  request.preserve_input_order = true;
  request.limits.max_batch_rows = 16;
  request.limits.max_materialized_cells = 128;
  request.limits.max_materialized_bytes = 1024;
  return request;
}

exec::ExecutorBatchJoinRequest JoinRequest() {
  exec::ExecutorBatchJoinRequest request;
  request.batch_request = BatchRequest();
  request.left_column = 0;
  request.right_column = 0;
  request.output_descriptor_digest = "dpc_executor_batch_join_output_v1";
  return request;
}

exec::ExecutorBatchDmlRequest UpdateRequest() {
  exec::ExecutorBatchDmlRequest request;
  request.batch_request = BatchRequest();
  request.operation = exec::ExecutorBatchDmlOperation::kUpdateReturning;
  request.returning_descriptor_digest = "dpc_executor_batch_dml_update_returning_v1";
  request.row_step =
      [](const exec::Tuple& row,
         std::size_t row_index) -> exec::ExecutorBatchDmlRowStepResult {
    exec::ExecutorBatchDmlRowStepResult result;
    result.new_row.values = {row.values[0], row.values[1] + 10};
    result.returning_row.values = {
        row.values[0],
        row.values[1] + 10,
        static_cast<std::int64_t>(row_index),
    };
    return result;
  };
  return request;
}

exec::ExecutorBatchDmlRequest DeleteRequest() {
  exec::ExecutorBatchDmlRequest request;
  request.batch_request = BatchRequest();
  request.operation = exec::ExecutorBatchDmlOperation::kDeleteReturning;
  request.returning_descriptor_digest = "dpc_executor_batch_dml_delete_returning_v1";
  request.row_step =
      [](const exec::Tuple& row,
         std::size_t row_index) -> exec::ExecutorBatchDmlRowStepResult {
    exec::ExecutorBatchDmlRowStepResult result;
    result.returning_row.values = {
        row.values[0],
        row.values[1],
        static_cast<std::int64_t>(row_index),
    };
    return result;
  };
  return request;
}

void RequireNoAuthorityOwned(const exec::ExecutorBatchJoinDmlAuthorityEvidence& authority) {
  Require(!authority.owns_transaction_finality,
          "DPC-052 helper must not own transaction finality");
  Require(!authority.owns_visibility,
          "DPC-052 helper must not own MGA visibility");
  Require(!authority.owns_rollback,
          "DPC-052 helper must not own rollback");
  Require(!authority.owns_recovery,
          "DPC-052 helper must not own recovery");
  Require(!authority.owns_parser_execution,
          "DPC-052 helper must not own parser execution");
  Require(!authority.owns_timestamp_ordering,
          "DPC-052 helper must not own timestamp ordering");
  Require(!authority.owns_reference_storage,
          "DPC-052 helper must not own reference storage");
  Require(!authority.owns_sql_execution,
          "DPC-052 helper must not own SQL execution");
  Require(!authority.owns_durable_commit,
          "DPC-052 helper must not own durable commit");
}

void RequireSameJoinSignature(const exec::ExecutorBatchJoinResult& left,
                              const exec::ExecutorBatchJoinResult& right,
                              std::string_view context) {
  if (left.evidence.deterministic_result_signature !=
      right.evidence.deterministic_result_signature) {
    std::cerr << context
              << " left=" << left.evidence.deterministic_result_signature
              << " right=" << right.evidence.deterministic_result_signature
              << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void RequireSameDmlSignature(const exec::ExecutorBatchDmlResult& left,
                             const exec::ExecutorBatchDmlResult& right,
                             std::string_view context) {
  if (left.evidence.deterministic_result_signature !=
      right.evidence.deterministic_result_signature) {
    std::cerr << context
              << " left=" << left.evidence.deterministic_result_signature
              << " right=" << right.evidence.deterministic_result_signature
              << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void JoinEnabledDisabledEquivalenceAndOrdering() {
  auto disabled_request = JoinRequest();
  disabled_request.batch_request.requested_mode =
      exec::ExecutorBatchRequestMode::kDisabledRowByRow;

  const auto row_by_row = exec::ExecuteBatchedJoinEqual(
      LeftJoinCorpus(), RightJoinCorpus(), disabled_request);
  const auto batched = exec::ExecuteBatchedJoinEqual(
      LeftJoinCorpus(), RightJoinCorpus(), JoinRequest());

  Require(row_by_row.evidence.selected_mode ==
              exec::ExecutorBatchSelectedMode::kRowByRow,
          "disabled join must select row-by-row");
  Require(row_by_row.evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kDisabledByRequest,
          "disabled join fallback reason drifted");
  Require(batched.evidence.selected_mode == exec::ExecutorBatchSelectedMode::kBatch,
          "enabled join must select batch");
  Require(batched.evidence.fallback_reason == exec::ExecutorBatchFallbackReason::kNone,
          "enabled join must not report fallback");
  Require(batched.evidence.hash_route_used,
          "enabled join must use a hash-style batch route");
  Require(batched.evidence.preserves_nested_loop_order,
          "join must preserve nested-loop output order");
  Require(batched.evidence.deterministic_ordering ==
              "left_input_order_then_right_input_order",
          "join deterministic ordering evidence drifted");
  RequireSameJoinSignature(row_by_row, batched, "join enabled/disabled");
  Require(batched.evidence.deterministic_result_signature ==
              "dpc_executor_batch_join_output_v1|"
              "[1,100,1,10][1,100,1,11]"
              "[2,200,2,20][2,200,2,21]"
              "[1,101,1,10][1,101,1,11]"
              "[2,201,2,20][2,201,2,21]",
          "join deterministic signature drifted");
  Require(batched.evidence.left_rows_requested == 5,
          "join left rows requested evidence drifted");
  Require(batched.evidence.right_rows_requested == 5,
          "join right rows requested evidence drifted");
  Require(batched.evidence.rows_produced == 8,
          "join rows produced evidence drifted");
  Require(batched.evidence.rows_processed_in_batch == 5,
          "join batch processed counter drifted");
  Require(batched.evidence.counters.right_rows_materialized == 5,
          "join right materialization counter drifted");
  Require(batched.evidence.counters.hash_join_left_probes == 5,
          "join hash probe counter drifted");
  Require(batched.evidence.counters.join_matches == 8,
          "join match counter drifted");
  RequireNoAuthorityOwned(batched.evidence.authority);
}

void JoinFallbackEquivalence() {
  auto disabled = JoinRequest();
  disabled.batch_request.requested_mode =
      exec::ExecutorBatchRequestMode::kDisabledRowByRow;
  const auto row_by_row =
      exec::ExecuteBatchedJoinEqual(LeftJoinCorpus(), RightJoinCorpus(), disabled);

  auto limited = JoinRequest();
  limited.batch_request.limits.max_batch_rows = 2;
  const auto resource =
      exec::ExecuteBatchedJoinEqual(LeftJoinCorpus(), RightJoinCorpus(), limited);
  Require(resource.evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kResourceLimit,
          "join resource fallback reason drifted");
  Require(resource.evidence.primitive_evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kResourceLimit,
          "join resource fallback must retain primitive evidence");
  RequireSameJoinSignature(resource, row_by_row, "join resource fallback");
  RequireNoAuthorityOwned(resource.evidence.authority);

  auto unsupported = JoinRequest();
  unsupported.batch_request.node_supports_batch = false;
  const auto unsupported_result =
      exec::ExecuteBatchedJoinEqual(LeftJoinCorpus(), RightJoinCorpus(), unsupported);
  Require(unsupported_result.evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kUnsupportedNode,
          "join unsupported fallback reason drifted");
  RequireSameJoinSignature(unsupported_result, row_by_row, "join unsupported fallback");

  auto pressured = JoinRequest();
  pressured.batch_request.limits.memory_pressure = true;
  const auto pressured_result =
      exec::ExecuteBatchedJoinEqual(LeftJoinCorpus(), RightJoinCorpus(), pressured);
  Require(pressured_result.evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kMemoryPressure,
          "join memory-pressure fallback reason drifted");
  RequireSameJoinSignature(pressured_result, row_by_row, "join memory fallback");
  RequireNoAuthorityOwned(pressured_result.evidence.authority);
}

void DmlUpdateDeleteReturningEquivalence() {
  auto disabled_update = UpdateRequest();
  disabled_update.batch_request.requested_mode =
      exec::ExecutorBatchRequestMode::kDisabledRowByRow;

  const auto update_row_by_row =
      exec::ExecuteBatchedDmlReturning(DmlCorpus(), disabled_update);
  const auto update_batched =
      exec::ExecuteBatchedDmlReturning(DmlCorpus(), UpdateRequest());
  RequireSameDmlSignature(update_row_by_row,
                          update_batched,
                          "DML update enabled/disabled");
  Require(update_batched.evidence.selected_mode == exec::ExecutorBatchSelectedMode::kBatch,
          "DML update must select batch");
  Require(update_batched.evidence.preserves_input_order,
          "DML update must preserve input order");
  Require(update_batched.evidence.no_partial_success,
          "DML update success evidence drifted");
  Require(update_batched.evidence.deterministic_result_signature ==
              "dpc_executor_batch_dml_update_returning_v1|"
              "[10,11,0][11,12,1][12,13,2][13,14,3]",
          "DML update returning signature drifted");
  Require(update_batched.intents.size() == 4,
          "DML update intent envelope count drifted");
  Require(update_batched.evidence.counters.update_intents == 4,
          "DML update intent counter drifted");
  Require(update_batched.evidence.counters.returning_rows == 4,
          "DML update returning counter drifted");
  Require(update_batched.intents[2].new_row.values[1] == 13,
          "DML update new-row intent drifted");
  RequireNoAuthorityOwned(update_batched.evidence.authority);

  auto disabled_delete = DeleteRequest();
  disabled_delete.batch_request.requested_mode =
      exec::ExecutorBatchRequestMode::kDisabledRowByRow;
  const auto delete_row_by_row =
      exec::ExecuteBatchedDmlReturning(DmlCorpus(), disabled_delete);
  const auto delete_batched =
      exec::ExecuteBatchedDmlReturning(DmlCorpus(), DeleteRequest());
  RequireSameDmlSignature(delete_row_by_row,
                          delete_batched,
                          "DML delete enabled/disabled");
  Require(delete_batched.evidence.deterministic_result_signature ==
              "dpc_executor_batch_dml_delete_returning_v1|"
              "[10,1,0][11,2,1][12,3,2][13,4,3]",
          "DML delete returning signature drifted");
  Require(delete_batched.evidence.counters.delete_intents == 4,
          "DML delete intent counter drifted");
  Require(delete_batched.intents[1].operation ==
              exec::ExecutorBatchDmlOperation::kDeleteReturning,
          "DML delete operation envelope drifted");
  RequireNoAuthorityOwned(delete_batched.evidence.authority);
}

void DmlFallbackEquivalence() {
  auto disabled = UpdateRequest();
  disabled.batch_request.requested_mode =
      exec::ExecutorBatchRequestMode::kDisabledRowByRow;
  const auto row_by_row = exec::ExecuteBatchedDmlReturning(DmlCorpus(), disabled);

  auto limited = UpdateRequest();
  limited.batch_request.limits.max_batch_rows = 2;
  const auto resource = exec::ExecuteBatchedDmlReturning(DmlCorpus(), limited);
  Require(resource.evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kResourceLimit,
          "DML resource fallback reason drifted");
  Require(resource.evidence.primitive_evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kResourceLimit,
          "DML resource fallback must retain primitive evidence");
  RequireSameDmlSignature(resource, row_by_row, "DML resource fallback");

  auto unsupported = UpdateRequest();
  unsupported.batch_request.node_supports_batch = false;
  const auto unsupported_result =
      exec::ExecuteBatchedDmlReturning(DmlCorpus(), unsupported);
  Require(unsupported_result.evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kUnsupportedNode,
          "DML unsupported fallback reason drifted");
  RequireSameDmlSignature(unsupported_result, row_by_row, "DML unsupported fallback");

  auto pressured = UpdateRequest();
  pressured.batch_request.limits.memory_pressure = true;
  const auto pressured_result =
      exec::ExecuteBatchedDmlReturning(DmlCorpus(), pressured);
  Require(pressured_result.evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kMemoryPressure,
          "DML memory-pressure fallback reason drifted");
  RequireSameDmlSignature(pressured_result, row_by_row, "DML memory fallback");
  RequireNoAuthorityOwned(pressured_result.evidence.authority);
}

void JoinCancellationAndRowErrorPropagation() {
  auto cancel_request = JoinRequest();
  cancel_request.batch_request.cancellation.cancel_before_row = 2;
  cancel_request.batch_request.cancellation.diagnostic_code =
      "SB_EXECUTOR_BATCH_JOIN_CANCEL_TEST";
  const auto cancelled = exec::ExecuteBatchedJoinEqual(
      LeftJoinCorpus(), RightJoinCorpus(), cancel_request);
  Require(cancelled.evidence.primitive_evidence.cancelled,
          "join cancellation must propagate cancelled evidence");
  Require(cancelled.evidence.primitive_evidence.diagnostic_code ==
              "SB_EXECUTOR_BATCH_JOIN_CANCEL_TEST",
          "join cancellation diagnostic code drifted");
  Require(cancelled.evidence.primitive_evidence.message_vector.size() == 1 &&
              cancelled.evidence.primitive_evidence.message_vector.front() ==
                  "SB_EXECUTOR_BATCH_JOIN_CANCEL_TEST",
          "join cancellation message vector drifted");
  Require(cancelled.output.rows.empty(),
          "join cancellation must not expose partial output");
  Require(cancelled.evidence.rows_produced == 0,
          "join cancellation must not expose partial row count");
  RequireNoAuthorityOwned(cancelled.evidence.authority);

  auto error_request = JoinRequest();
  error_request.left_row_validation =
      [](const exec::Tuple& row, std::size_t row_index) {
    exec::ExecutorRowStepResult result;
    result.emit_row = false;
    if (row_index == 2) {
      result.ok = false;
      result.diagnostic_code = "SB_EXECUTOR_BATCH_JOIN_ROW_ERROR_TEST";
      result.message_vector = {
          "SB_EXECUTOR_BATCH_JOIN_ROW_ERROR_TEST",
          "row_index=2",
          "join_input_validation",
      };
    }
    return result;
  };
  const auto error = exec::ExecuteBatchedJoinEqual(
      LeftJoinCorpus(), RightJoinCorpus(), error_request);
  Require(error.evidence.primitive_evidence.error,
          "join row-level error must propagate primitive error");
  Require(error.evidence.primitive_evidence.diagnostic_code ==
              "SB_EXECUTOR_BATCH_JOIN_ROW_ERROR_TEST",
          "join row-level diagnostic code drifted");
  Require(error.evidence.primitive_evidence.message_vector.size() == 3,
          "join row-level message vector size drifted");
  Require(error.evidence.primitive_evidence.message_vector[1] == "row_index=2",
          "join row-level message row index drifted");
  Require(error.output.rows.empty(),
          "join row-level error must not expose partial output");
  Require(error.evidence.rows_produced == 0,
          "join row-level error must not expose partial row count");
  RequireNoAuthorityOwned(error.evidence.authority);
}

void DmlCancellationAndRowErrorPropagation() {
  auto cancel_request = UpdateRequest();
  cancel_request.batch_request.cancellation.cancel_before_row = 2;
  cancel_request.batch_request.cancellation.diagnostic_code =
      "SB_EXECUTOR_BATCH_DML_CANCEL_TEST";
  const auto cancelled =
      exec::ExecuteBatchedDmlReturning(DmlCorpus(), cancel_request);
  Require(cancelled.evidence.primitive_evidence.cancelled,
          "DML cancellation must propagate primitive cancelled evidence");
  Require(cancelled.evidence.primitive_evidence.diagnostic_code ==
              "SB_EXECUTOR_BATCH_DML_CANCEL_TEST",
          "DML cancellation diagnostic code drifted");
  Require(cancelled.evidence.primitive_evidence.message_vector.size() == 1 &&
              cancelled.evidence.primitive_evidence.message_vector.front() ==
                  "SB_EXECUTOR_BATCH_DML_CANCEL_TEST",
          "DML cancellation message vector drifted");
  Require(cancelled.returning_rows.rows.empty(),
          "DML cancellation must not expose partial returning rows");
  Require(cancelled.intents.empty(),
          "DML cancellation must not expose partial intent envelopes");
  Require(cancelled.evidence.no_partial_success,
          "DML cancellation no-partial-success evidence drifted");
  RequireNoAuthorityOwned(cancelled.evidence.authority);

  auto error_request = UpdateRequest();
  error_request.row_step =
      [](const exec::Tuple& row,
         std::size_t row_index) -> exec::ExecutorBatchDmlRowStepResult {
    auto result = UpdateRequest().row_step(row, row_index);
    if (row_index == 3) {
      result.ok = false;
      result.emit_returning = false;
      result.diagnostic_code = "SB_EXECUTOR_BATCH_DML_ROW_ERROR_TEST";
      result.message_vector = {
          "SB_EXECUTOR_BATCH_DML_ROW_ERROR_TEST",
          "row_index=3",
          "dml_intent_uncommitted",
      };
    }
    return result;
  };
  const auto error = exec::ExecuteBatchedDmlReturning(DmlCorpus(), error_request);
  Require(error.evidence.primitive_evidence.error,
          "DML row-level error must propagate primitive error");
  Require(error.evidence.primitive_evidence.diagnostic_code ==
              "SB_EXECUTOR_BATCH_DML_ROW_ERROR_TEST",
          "DML row-level diagnostic code drifted");
  Require(error.evidence.primitive_evidence.message_vector.size() == 3,
          "DML row-level message vector size drifted");
  Require(error.evidence.primitive_evidence.message_vector[1] == "row_index=3",
          "DML row-level message row index drifted");
  Require(error.returning_rows.rows.empty(),
          "DML row-level error must not expose partial returning rows");
  Require(error.intents.empty(),
          "DML row-level error must not expose partial intent envelopes");
  Require(error.evidence.no_partial_success,
          "DML row-level error no-partial-success evidence drifted");
  RequireNoAuthorityOwned(error.evidence.authority);
}

}  // namespace

int main() {
  Require(!kGateSearchKey.empty(), "missing gate search key");
  Require(!kImplementationSearchKey.empty(), "missing implementation search key");
  JoinEnabledDisabledEquivalenceAndOrdering();
  JoinFallbackEquivalence();
  DmlUpdateDeleteReturningEquivalence();
  DmlFallbackEquivalence();
  JoinCancellationAndRowErrorPropagation();
  DmlCancellationAndRowErrorPropagation();
  std::cout << "dpc_executor_batch_join_dml_gate=passed\n";
  return EXIT_SUCCESS;
}
