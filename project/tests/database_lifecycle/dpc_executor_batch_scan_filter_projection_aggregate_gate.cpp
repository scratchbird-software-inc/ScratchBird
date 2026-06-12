// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "executor_batch_relational.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace exec = scratchbird::engine::executor;

constexpr std::string_view kGateSearchKey =
    "DPC_EXECUTOR_BATCH_RELATIONAL_GATE";
constexpr std::string_view kImplementationSearchKey =
    "DPC_EXECUTOR_BATCH_RELATIONAL";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

exec::Batch Corpus() {
  return exec::MakeBatch("dpc_executor_batch_relational_tuple_v1",
                         {
                             {.values = {0, 2, 5, 1}},
                             {.values = {1, 1, 7, 3}},
                             {.values = {2, 2, 11, 4}},
                             {.values = {3, 3, 13, 2}},
                             {.values = {4, 1, 17, 5}},
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

exec::ExecutorBatchScanFilterProjectionRequest PipelineRequest() {
  exec::ExecutorBatchScanFilterProjectionRequest request;
  request.batch_request = BatchRequest();
  request.filter.enabled = true;
  request.filter.column = 3;
  request.filter.op = exec::Int64ComparisonOperator::kGreaterThanOrEqual;
  request.filter.value = 2;
  request.projection_columns = {1, 2, 0};
  request.output_descriptor_digest = "dpc_executor_batch_relational_projected_v1";
  return request;
}

exec::ExecutorBatchAggregateSumRequest AggregateRequest() {
  exec::ExecutorBatchAggregateSumRequest request;
  request.batch_request = BatchRequest();
  request.key_column = 1;
  request.value_column = 2;
  request.output_descriptor_digest = "dpc_executor_batch_relational_aggregate_v1";
  return request;
}

void RequireNoAuthorityOwned(const exec::ExecutorBatchRelationalEvidence& evidence) {
  Require(!evidence.authority.owns_transaction_finality,
          "DPC-051 helper must not own transaction finality");
  Require(!evidence.authority.owns_visibility,
          "DPC-051 helper must not own MGA visibility");
  Require(!evidence.authority.owns_rollback,
          "DPC-051 helper must not own rollback");
  Require(!evidence.authority.owns_recovery,
          "DPC-051 helper must not own recovery");
  Require(!evidence.authority.owns_parser_execution,
          "DPC-051 helper must not own parser execution");
  Require(!evidence.authority.owns_timestamp_ordering,
          "DPC-051 helper must not own timestamp ordering");
  Require(!evidence.authority.owns_reference_storage,
          "DPC-051 helper must not own reference storage");
  Require(!evidence.authority.owns_sql_execution,
          "DPC-051 helper must not own SQL execution");
}

void RequireSameSignature(const exec::ExecutorBatchRelationalResult& left,
                          const exec::ExecutorBatchRelationalResult& right,
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

void ScanFilterProjectionEquivalence() {
  auto disabled_request = PipelineRequest();
  disabled_request.batch_request.requested_mode =
      exec::ExecutorBatchRequestMode::kDisabledRowByRow;

  const auto row_by_row =
      exec::ExecuteBatchedScanFilterProjection(Corpus(), disabled_request);
  const auto batched =
      exec::ExecuteBatchedScanFilterProjection(Corpus(), PipelineRequest());

  Require(row_by_row.evidence.selected_mode ==
              exec::ExecutorBatchSelectedMode::kRowByRow,
          "disabled pipeline must select row-by-row");
  Require(row_by_row.evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kDisabledByRequest,
          "disabled pipeline fallback reason drifted");
  Require(batched.evidence.selected_mode ==
              exec::ExecutorBatchSelectedMode::kBatch,
          "enabled pipeline must select batch mode");
  Require(batched.evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kNone,
          "enabled pipeline must not report fallback");
  RequireSameSignature(row_by_row, batched, "pipeline enabled/disabled");
  Require(batched.evidence.deterministic_result_signature ==
              "dpc_executor_batch_relational_projected_v1|"
              "[1,7,1][2,11,2][3,13,3][1,17,4]",
          "pipeline result signature drifted");
  Require(batched.evidence.preserves_input_order,
          "scan/filter/projection must report input-order preservation");
  Require(batched.evidence.rows_requested == 5,
          "pipeline rows requested evidence drifted");
  Require(batched.evidence.rows_produced == 4,
          "pipeline rows produced evidence drifted");
  Require(batched.evidence.rows_processed_in_batch == 5,
          "pipeline batch processed counter drifted");
  Require(batched.evidence.counters.rows_scanned == 5,
          "pipeline scanned counter drifted");
  Require(batched.evidence.counters.rows_filter_evaluated == 5,
          "pipeline filter evaluated counter drifted");
  Require(batched.evidence.counters.rows_filter_passed == 4,
          "pipeline filter passed counter drifted");
  Require(batched.evidence.counters.rows_projected == 4,
          "pipeline projected counter drifted");
  RequireNoAuthorityOwned(batched.evidence);
}

void AggregateEquivalenceAndOrdering() {
  auto disabled_request = AggregateRequest();
  disabled_request.batch_request.requested_mode =
      exec::ExecutorBatchRequestMode::kDisabledRowByRow;

  const auto row_by_row =
      exec::ExecuteBatchedAggregateSumByKey(Corpus(), disabled_request);
  const auto batched =
      exec::ExecuteBatchedAggregateSumByKey(Corpus(), AggregateRequest());

  RequireSameSignature(row_by_row, batched, "aggregate enabled/disabled");
  Require(batched.evidence.selected_mode ==
              exec::ExecutorBatchSelectedMode::kBatch,
          "enabled aggregate must select batch mode");
  Require(batched.evidence.aggregate_group_order_deterministic,
          "aggregate must report deterministic grouping order");
  Require(!batched.evidence.preserves_input_order,
          "aggregate output is grouped order, not input order");
  Require(batched.evidence.deterministic_result_signature ==
              "dpc_executor_batch_relational_aggregate_v1|"
              "[1,24][2,16][3,13]",
          "aggregate grouped signature drifted");
  Require(batched.evidence.rows_requested == 5,
          "aggregate rows requested evidence drifted");
  Require(batched.evidence.rows_produced == 3,
          "aggregate rows produced must report final group rows");
  Require(batched.evidence.primitive_evidence.rows_produced == 5,
          "aggregate primitive evidence must retain scanned row count");
  Require(batched.evidence.counters.aggregate_input_rows == 5,
          "aggregate input counter drifted");
  Require(batched.evidence.counters.aggregate_groups == 3,
          "aggregate group counter drifted");
  RequireNoAuthorityOwned(batched.evidence);
}

void ResourceAndUnsupportedFallbacks() {
  auto limited = PipelineRequest();
  limited.batch_request.limits.max_batch_rows = 2;
  const auto resource =
      exec::ExecuteBatchedScanFilterProjection(Corpus(), limited);

  auto disabled = PipelineRequest();
  disabled.batch_request.requested_mode =
      exec::ExecutorBatchRequestMode::kDisabledRowByRow;
  const auto row_by_row =
      exec::ExecuteBatchedScanFilterProjection(Corpus(), disabled);

  Require(resource.evidence.selected_mode ==
              exec::ExecutorBatchSelectedMode::kRowByRow,
          "resource fallback must select row-by-row");
  Require(resource.evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kResourceLimit,
          "resource fallback reason drifted");
  Require(resource.evidence.primitive_evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kResourceLimit,
          "resource fallback must retain primitive evidence");
  RequireSameSignature(resource, row_by_row, "resource fallback");
  RequireNoAuthorityOwned(resource.evidence);

  auto unsupported = PipelineRequest();
  unsupported.batch_request.node_supports_batch = false;
  const auto unsupported_result =
      exec::ExecuteBatchedScanFilterProjection(Corpus(), unsupported);
  Require(unsupported_result.evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kUnsupportedNode,
          "unsupported fallback reason drifted");
  RequireSameSignature(unsupported_result, row_by_row, "unsupported fallback");

  auto pressured = AggregateRequest();
  pressured.batch_request.limits.memory_pressure = true;
  const auto pressured_result =
      exec::ExecuteBatchedAggregateSumByKey(Corpus(), pressured);
  auto aggregate_disabled = AggregateRequest();
  aggregate_disabled.batch_request.requested_mode =
      exec::ExecutorBatchRequestMode::kDisabledRowByRow;
  const auto aggregate_row_by_row =
      exec::ExecuteBatchedAggregateSumByKey(Corpus(), aggregate_disabled);
  Require(pressured_result.evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kMemoryPressure,
          "memory-pressure fallback reason drifted");
  RequireSameSignature(pressured_result,
                       aggregate_row_by_row,
                       "memory-pressure aggregate fallback");
  RequireNoAuthorityOwned(pressured_result.evidence);
}

void CancellationAndRowErrorPropagation() {
  auto cancel_request = PipelineRequest();
  cancel_request.batch_request.cancellation.cancel_before_row = 2;
  cancel_request.batch_request.cancellation.diagnostic_code =
      "SB_EXECUTOR_BATCH_RELATIONAL_CANCEL_TEST";
  const auto cancelled =
      exec::ExecuteBatchedScanFilterProjection(Corpus(), cancel_request);

  Require(cancelled.evidence.primitive_evidence.cancelled,
          "cancellation must propagate primitive cancelled evidence");
  Require(!cancelled.evidence.primitive_evidence.error,
          "cancellation must not report error");
  Require(cancelled.evidence.primitive_evidence.diagnostic_code ==
              "SB_EXECUTOR_BATCH_RELATIONAL_CANCEL_TEST",
          "cancellation diagnostic code drifted");
  Require(cancelled.evidence.primitive_evidence.message_vector.size() == 1 &&
              cancelled.evidence.primitive_evidence.message_vector.front() ==
                  "SB_EXECUTOR_BATCH_RELATIONAL_CANCEL_TEST",
          "cancellation message vector drifted");
  Require(cancelled.output.rows.empty(),
          "cancellation must not expose partial output");
  Require(cancelled.evidence.rows_produced == 0,
          "cancellation must not expose partial success count");
  RequireNoAuthorityOwned(cancelled.evidence);

  auto error_request = PipelineRequest();
  error_request.projection_columns = {1, 9};
  const auto error =
      exec::ExecuteBatchedScanFilterProjection(Corpus(), error_request);
  Require(error.evidence.primitive_evidence.error,
          "projection column miss must report primitive error");
  Require(!error.evidence.primitive_evidence.cancelled,
          "projection column miss must not report cancelled");
  Require(error.evidence.primitive_evidence.diagnostic_code ==
              "SB_EXECUTOR_BATCH_RELATIONAL_PROJECTION_COLUMN_REQUIRED",
          "projection diagnostic code drifted");
  Require(error.evidence.primitive_evidence.message_vector.size() == 3,
          "projection error message vector size drifted");
  Require(error.evidence.primitive_evidence.message_vector[1] == "row_index=1",
          "projection error row index drifted");
  Require(error.evidence.primitive_evidence.message_vector[2] == "column=9",
          "projection error column drifted");
  Require(error.output.rows.empty(),
          "projection row-level error must not expose partial output");
  Require(error.evidence.rows_produced == 0,
          "projection row-level error must not expose partial success count");
  RequireNoAuthorityOwned(error.evidence);
}

}  // namespace

int main() {
  Require(!kGateSearchKey.empty(), "missing gate search key");
  Require(!kImplementationSearchKey.empty(), "missing implementation search key");
  ScanFilterProjectionEquivalence();
  AggregateEquivalenceAndOrdering();
  ResourceAndUnsupportedFallbacks();
  CancellationAndRowErrorPropagation();
  std::cout << "dpc_executor_batch_scan_filter_projection_aggregate_gate=passed\n";
  return EXIT_SUCCESS;
}
