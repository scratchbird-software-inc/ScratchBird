// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "executor_batching.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;

constexpr std::string_view kGateSearchKey = "DPC_EXECUTOR_BATCH_PRIMITIVE_GATE";
constexpr std::string_view kImplementationSearchKey = "DPC_EXECUTOR_BATCH";

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
  return exec::MakeBatch("dpc_executor_batch_tuple_v1",
                         {
                             {.values = {0, 30}},
                             {.values = {1, 10}},
                             {.values = {2, 20}},
                             {.values = {3, 40}},
                             {.values = {4, 50}},
                         });
}

exec::ExecutorRowStepResult TransformFilterStep(const exec::Tuple& row,
                                                std::size_t row_index) {
  exec::ExecutorRowStepResult result;
  if (row.values.empty() || row.values.front() % 2 != 0) {
    result.emit_row = false;
    return result;
  }
  result.row.values = {
      static_cast<std::int64_t>(row_index),
      row.values[0],
      row.values[1] + 7,
  };
  return result;
}

std::string Signature(const exec::Batch& batch) {
  std::ostringstream out;
  out << batch.descriptor_digest << '|';
  for (const auto& row : batch.rows) {
    out << '[';
    for (std::size_t i = 0; i < row.values.size(); ++i) {
      if (i > 0) {
        out << ',';
      }
      out << row.values[i];
    }
    out << ']';
  }
  return out.str();
}

exec::ExecutorBatchRequest BatchRequest() {
  exec::ExecutorBatchRequest request;
  request.requested_mode = exec::ExecutorBatchRequestMode::kPreferBatch;
  request.node_supports_batch = true;
  request.preserve_input_order = true;
  request.limits.max_batch_rows = 16;
  request.limits.max_materialized_cells = 64;
  request.limits.max_materialized_bytes = 512;
  return request;
}

void RequireNoAuthorityOwned(const exec::ExecutorBatchEvidence& evidence) {
  Require(!evidence.authority.owns_transaction_finality,
          "DPC-050 primitive must not own transaction finality");
  Require(!evidence.authority.owns_visibility,
          "DPC-050 primitive must not own MGA visibility");
  Require(!evidence.authority.owns_rollback,
          "DPC-050 primitive must not own rollback");
  Require(!evidence.authority.owns_recovery,
          "DPC-050 primitive must not own recovery");
  Require(!evidence.authority.owns_parser_execution,
          "DPC-050 primitive must not own parser execution");
}

void RequireEquivalentRows(const exec::ExecutorBatchResult& left,
                           const exec::ExecutorBatchResult& right,
                           std::string_view context) {
  if (Signature(left.output) != Signature(right.output)) {
    std::cerr << context << " left=" << Signature(left.output)
              << " right=" << Signature(right.output) << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void BatchEnabledAndDisabledEquivalence() {
  auto disabled = BatchRequest();
  disabled.requested_mode = exec::ExecutorBatchRequestMode::kDisabledRowByRow;

  const auto row_by_row =
      exec::ExecuteScopedExecutorBatch(Corpus(), disabled, TransformFilterStep);
  const auto batched =
      exec::ExecuteScopedExecutorBatch(Corpus(), BatchRequest(), TransformFilterStep);

  Require(row_by_row.evidence.selected_mode ==
              exec::ExecutorBatchSelectedMode::kRowByRow,
          "disabled request must select row-by-row");
  Require(row_by_row.evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kDisabledByRequest,
          "disabled request must report disabled fallback");
  Require(batched.evidence.selected_mode == exec::ExecutorBatchSelectedMode::kBatch,
          "enabled request must select batch");
  Require(batched.evidence.fallback_reason == exec::ExecutorBatchFallbackReason::kNone,
          "enabled request must not report fallback");
  RequireEquivalentRows(row_by_row, batched, "enabled/disabled equivalence");
  Require(Signature(batched.output) == "dpc_executor_batch_tuple_v1|[0,0,37][2,2,27][4,4,57]",
          "batch output order/signature drifted");
  Require(batched.evidence.rows_requested == 5, "rows requested evidence drifted");
  Require(batched.evidence.rows_produced == 3, "rows produced evidence drifted");
  Require(batched.evidence.rows_processed_in_batch == 5,
          "batch processed counter drifted");
  Require(batched.evidence.rows_processed_row_by_row == 0,
          "batch row-by-row counter must be zero");
  RequireNoAuthorityOwned(batched.evidence);
}

void ResourceFallbackEquivalence() {
  auto limited = BatchRequest();
  limited.limits.max_batch_rows = 2;

  auto disabled = BatchRequest();
  disabled.requested_mode = exec::ExecutorBatchRequestMode::kDisabledRowByRow;

  const auto fallback =
      exec::ExecuteScopedExecutorBatch(Corpus(), limited, TransformFilterStep);
  const auto row_by_row =
      exec::ExecuteScopedExecutorBatch(Corpus(), disabled, TransformFilterStep);
  Require(fallback.evidence.selected_mode == exec::ExecutorBatchSelectedMode::kRowByRow,
          "resource envelope overflow must select row-by-row");
  Require(fallback.evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kResourceLimit,
          "resource envelope overflow must report resource fallback");
  RequireEquivalentRows(fallback, row_by_row, "resource fallback equivalence");
  Require(fallback.evidence.resource_counters.max_observed_batch_rows == 1,
          "row-by-row fallback must keep observed batch rows bounded");
  Require(fallback.evidence.resource_counters.max_observed_materialized_cells <= 2,
          "row-by-row fallback materialized cells must be deterministic");
  RequireNoAuthorityOwned(fallback.evidence);
}

void UnsupportedAndMemoryPressureFallbacks() {
  auto unsupported = BatchRequest();
  unsupported.node_supports_batch = false;
  const auto unsupported_result =
      exec::ExecuteScopedExecutorBatch(Corpus(), unsupported, TransformFilterStep);
  Require(unsupported_result.evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kUnsupportedNode,
          "unsupported node fallback reason drifted");
  Require(unsupported_result.evidence.rows_processed_row_by_row == 5,
          "unsupported node must process row-by-row");

  auto pressured = BatchRequest();
  pressured.limits.memory_pressure = true;
  const auto pressured_result =
      exec::ExecuteScopedExecutorBatch(Corpus(), pressured, TransformFilterStep);
  Require(pressured_result.evidence.fallback_reason ==
              exec::ExecutorBatchFallbackReason::kMemoryPressure,
          "memory pressure fallback reason drifted");
  RequireEquivalentRows(unsupported_result,
                        pressured_result,
                        "unsupported/memory fallback row equivalence");
  RequireNoAuthorityOwned(pressured_result.evidence);
}

void CancellationSemantics() {
  auto before = BatchRequest();
  before.cancellation.cancel_before_start = true;
  before.cancellation.diagnostic_code = "SB_EXECUTOR_BATCH_CANCEL_BEFORE_TEST";
  const auto before_result =
      exec::ExecuteScopedExecutorBatch(Corpus(), before, TransformFilterStep);
  Require(before_result.evidence.cancelled, "cancel-before must report cancelled");
  Require(!before_result.evidence.error, "cancel-before must not report error");
  Require(before_result.evidence.rows_produced == 0,
          "cancel-before must not expose partial output");
  Require(before_result.evidence.message_vector.size() == 1 &&
              before_result.evidence.message_vector.front() ==
                  "SB_EXECUTOR_BATCH_CANCEL_BEFORE_TEST",
          "cancel-before message vector drifted");
  RequireNoAuthorityOwned(before_result.evidence);

  auto within = BatchRequest();
  within.cancellation.cancel_before_row = 2;
  within.cancellation.diagnostic_code = "SB_EXECUTOR_BATCH_CANCEL_WITHIN_TEST";
  const auto within_result =
      exec::ExecuteScopedExecutorBatch(Corpus(), within, TransformFilterStep);
  Require(within_result.evidence.cancelled, "cancel-within must report cancelled");
  Require(within_result.evidence.diagnostic_row_index == 2,
          "cancel-within row index drifted");
  Require(within_result.evidence.rows_processed_in_batch == 2,
          "cancel-within processed counter drifted");
  Require(within_result.evidence.rows_produced == 0,
          "cancel-within must not expose partial output");
  Require(within_result.output.rows.empty(),
          "cancel-within output must be empty");
  RequireNoAuthorityOwned(within_result.evidence);
}

void RowLevelErrorSemantics() {
  const auto error_step = [](const exec::Tuple& row, std::size_t row_index) {
    auto result = TransformFilterStep(row, row_index);
    if (row_index == 2) {
      result.ok = false;
      result.emit_row = false;
      result.diagnostic_code = "SB_EXECUTOR_BATCH_ROW_ERROR_TEST";
      result.message_vector = {
          "SB_EXECUTOR_BATCH_ROW_ERROR_TEST",
          "row_index=2",
          "statement_effects_uncommitted",
      };
    }
    return result;
  };

  const auto result =
      exec::ExecuteScopedExecutorBatch(Corpus(), BatchRequest(), error_step);
  Require(result.evidence.error, "row-level error must report error");
  Require(!result.evidence.cancelled, "row-level error must not report cancelled");
  Require(result.evidence.diagnostic_code == "SB_EXECUTOR_BATCH_ROW_ERROR_TEST",
          "row-level diagnostic code drifted");
  Require(result.evidence.diagnostic_row_index == 2,
          "row-level diagnostic row index drifted");
  Require(result.evidence.message_vector.size() == 3,
          "row-level message vector size drifted");
  Require(result.evidence.rows_processed_in_batch == 3,
          "row-level processed counter must include failing row");
  Require(result.evidence.rows_produced == 0,
          "row-level error must not expose partial success");
  Require(result.output.rows.empty(), "row-level error output must be empty");
  RequireNoAuthorityOwned(result.evidence);
}

void ResourceCountersAreBounded() {
  const auto result =
      exec::ExecuteScopedExecutorBatch(Corpus(), BatchRequest(), TransformFilterStep);
  Require(result.evidence.resource_counters.max_observed_batch_rows == 5,
          "batch high-water row counter drifted");
  Require(result.evidence.resource_counters.max_observed_materialized_cells == 10,
          "batch high-water cell counter drifted");
  Require(result.evidence.resource_counters.max_observed_materialized_bytes == 80,
          "batch high-water byte counter drifted");
  Require(result.evidence.resource_counters.max_observed_batch_rows <=
              BatchRequest().limits.max_batch_rows,
          "observed rows exceeded request limit");
  Require(result.evidence.resource_counters.max_observed_materialized_cells <=
              BatchRequest().limits.max_materialized_cells,
          "observed cells exceeded request limit");
  Require(result.evidence.resource_counters.max_observed_materialized_bytes <=
              BatchRequest().limits.max_materialized_bytes,
          "observed bytes exceeded request limit");
}

}  // namespace

int main() {
  Require(!kGateSearchKey.empty(), "missing gate search key");
  Require(!kImplementationSearchKey.empty(), "missing implementation search key");
  BatchEnabledAndDisabledEquivalence();
  ResourceFallbackEquivalence();
  UnsupportedAndMemoryPressureFallbacks();
  CancellationSemantics();
  RowLevelErrorSemantics();
  ResourceCountersAreBounded();
  std::cout << "dpc_executor_batch_primitive_gate=passed\n";
  return EXIT_SUCCESS;
}
