// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "executor_batching.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

constexpr std::size_t kInt64CellBytes = sizeof(std::int64_t);

std::size_t SaturatingAdd(std::size_t lhs, std::size_t rhs) {
  const auto max = std::numeric_limits<std::size_t>::max();
  if (rhs > max - lhs) {
    return max;
  }
  return lhs + rhs;
}

std::size_t SaturatingMul(std::size_t lhs, std::size_t rhs) {
  if (lhs == 0 || rhs == 0) {
    return 0;
  }
  const auto max = std::numeric_limits<std::size_t>::max();
  if (lhs > max / rhs) {
    return max;
  }
  return lhs * rhs;
}

struct BatchEnvelope {
  std::size_t rows = 0;
  std::size_t cells = 0;
  std::size_t bytes = 0;
};

BatchEnvelope MeasureEnvelope(const Batch& input) {
  BatchEnvelope envelope;
  envelope.rows = input.rows.size();
  for (const auto& row : input.rows) {
    envelope.cells = SaturatingAdd(envelope.cells, row.values.size());
  }
  envelope.bytes = SaturatingMul(envelope.cells, kInt64CellBytes);
  return envelope;
}

void ObserveEnvelope(ExecutorBatchResourceCounters& counters,
                     std::size_t rows,
                     std::size_t cells,
                     std::size_t bytes) {
  counters.max_observed_batch_rows =
      std::max(counters.max_observed_batch_rows, rows);
  counters.max_observed_materialized_cells =
      std::max(counters.max_observed_materialized_cells, cells);
  counters.max_observed_materialized_bytes =
      std::max(counters.max_observed_materialized_bytes, bytes);
}

bool ExceedsEnvelope(const BatchEnvelope& envelope,
                     const ExecutorBatchResourceLimits& limits) {
  return envelope.rows > limits.max_batch_rows ||
         envelope.cells > limits.max_materialized_cells ||
         envelope.bytes > limits.max_materialized_bytes ||
         limits.max_batch_rows == 0;
}

ExecutorBatchFallbackReason SelectFallbackReason(
    const BatchEnvelope& envelope,
    const ExecutorBatchRequest& request) {
  if (request.requested_mode == ExecutorBatchRequestMode::kDisabledRowByRow) {
    return ExecutorBatchFallbackReason::kDisabledByRequest;
  }
  if (!request.node_supports_batch) {
    return ExecutorBatchFallbackReason::kUnsupportedNode;
  }
  if (request.backpressure_active) {
    return ExecutorBatchFallbackReason::kBackpressure;
  }
  if (request.exact_error_required) {
    return ExecutorBatchFallbackReason::kExactErrorRequired;
  }
  if (request.limits.memory_pressure) {
    return ExecutorBatchFallbackReason::kMemoryPressure;
  }
  if (ExceedsEnvelope(envelope, request.limits)) {
    return ExecutorBatchFallbackReason::kResourceLimit;
  }
  return ExecutorBatchFallbackReason::kNone;
}

std::string FallbackDiagnosticCode(ExecutorBatchFallbackReason reason) {
  switch (reason) {
    case ExecutorBatchFallbackReason::kNone:
      return "SB_EXECUTOR_OK";
    case ExecutorBatchFallbackReason::kDisabledByRequest:
      return "SB_EXECUTOR_BATCH_FALLBACK_DISABLED_BY_REQUEST";
    case ExecutorBatchFallbackReason::kUnsupportedNode:
      return "SB_EXECUTOR_BATCH_FALLBACK_UNSUPPORTED_NODE";
    case ExecutorBatchFallbackReason::kResourceLimit:
      return "SB_EXECUTOR_BATCH_FALLBACK_RESOURCE_LIMIT";
    case ExecutorBatchFallbackReason::kMemoryPressure:
      return "SB_EXECUTOR_BATCH_FALLBACK_MEMORY_PRESSURE";
    case ExecutorBatchFallbackReason::kBackpressure:
      return "SB_EXECUTOR_BATCH_FALLBACK_BACKPRESSURE";
    case ExecutorBatchFallbackReason::kExactErrorRequired:
      return "SB_EXECUTOR_BATCH_FALLBACK_EXACT_ERROR_REQUIRED";
  }
  return "SB_EXECUTOR_BATCH_FALLBACK_UNKNOWN";
}

void FinishWithDiagnostic(ExecutorBatchResult& result,
                          bool cancelled,
                          bool error,
                          std::size_t row_index,
                          std::string diagnostic_code,
                          std::vector<std::string> message_vector) {
  result.output.rows.clear();
  result.evidence.rows_produced = 0;
  result.evidence.cancelled = cancelled;
  result.evidence.error = error;
  result.evidence.diagnostic_row_index = row_index;
  result.evidence.diagnostic_code = std::move(diagnostic_code);
  result.evidence.message_vector = std::move(message_vector);
}

void ProcessRows(const Batch& input,
                 const ExecutorBatchRequest& request,
                 const ExecutorRowStep& row_step,
                 ExecutorBatchResult& result) {
  if (!row_step) {
    FinishWithDiagnostic(result,
                         false,
                         true,
                         0,
                         "SB_EXECUTOR_BATCH_ROW_STEP_REQUIRED",
                         {"SB_EXECUTOR_BATCH_ROW_STEP_REQUIRED"});
    return;
  }

  if (request.cancellation.cancel_before_start ||
      request.cancellation.cancel_before_row == 0) {
    FinishWithDiagnostic(result,
                         true,
                         false,
                         0,
                         request.cancellation.diagnostic_code,
                         {request.cancellation.diagnostic_code});
    return;
  }

  const bool use_batch =
      result.evidence.selected_mode == ExecutorBatchSelectedMode::kBatch;
  const std::size_t batch_width =
      use_batch ? input.rows.size() : std::min<std::size_t>(1, input.rows.size());
  const std::size_t batch_cells =
      use_batch ? MeasureEnvelope(input).cells
                : (input.rows.empty() ? 0 : input.rows.front().values.size());
  ObserveEnvelope(result.evidence.resource_counters,
                  batch_width,
                  batch_cells,
                  SaturatingMul(batch_cells, kInt64CellBytes));

  for (std::size_t row_index = 0; row_index < input.rows.size(); ++row_index) {
    if (request.cancellation.cancel_before_row == row_index) {
      FinishWithDiagnostic(result,
                           true,
                           false,
                           row_index,
                           request.cancellation.diagnostic_code,
                           {request.cancellation.diagnostic_code});
      return;
    }

    const auto step = row_step(input.rows[row_index], row_index);
    if (use_batch) {
      ++result.evidence.rows_processed_in_batch;
    } else {
      ++result.evidence.rows_processed_row_by_row;
      ObserveEnvelope(result.evidence.resource_counters,
                      1,
                      input.rows[row_index].values.size(),
                      SaturatingMul(input.rows[row_index].values.size(),
                                    kInt64CellBytes));
    }

    if (!step.ok) {
      auto messages = step.message_vector;
      if (messages.empty()) {
        messages.push_back(step.diagnostic_code);
      }
      FinishWithDiagnostic(result,
                           false,
                           true,
                           row_index,
                           step.diagnostic_code,
                           std::move(messages));
      return;
    }
    if (step.emit_row) {
      result.output.rows.push_back(step.row);
    }
  }

  result.evidence.rows_produced = result.output.rows.size();
}

}  // namespace

ExecutorBatchResult ExecuteScopedExecutorBatch(const Batch& input,
                                               const ExecutorBatchRequest& request,
                                               const ExecutorRowStep& row_step) {
  ExecutorBatchResult result;
  result.output.descriptor_digest = input.descriptor_digest;
  result.evidence.rows_requested = input.rows.size();

  const auto validation = ValidateBatch(input);
  if (!validation.ok) {
    FinishWithDiagnostic(result,
                         false,
                         true,
                         0,
                         validation.diagnostic_code,
                         {validation.diagnostic_code});
    return result;
  }

  const auto envelope = MeasureEnvelope(input);
  const auto fallback_reason = SelectFallbackReason(envelope, request);
  result.evidence.fallback_reason = fallback_reason;
  result.evidence.selected_mode =
      fallback_reason == ExecutorBatchFallbackReason::kNone
          ? ExecutorBatchSelectedMode::kBatch
          : ExecutorBatchSelectedMode::kRowByRow;
  if (fallback_reason != ExecutorBatchFallbackReason::kNone) {
    result.evidence.diagnostic_code = FallbackDiagnosticCode(fallback_reason);
    result.evidence.message_vector = {result.evidence.diagnostic_code};
  }

  ProcessRows(input, request, row_step, result);
  return result;
}

std::string_view ToString(ExecutorBatchSelectedMode mode) {
  switch (mode) {
    case ExecutorBatchSelectedMode::kRowByRow:
      return "row_by_row";
    case ExecutorBatchSelectedMode::kBatch:
      return "batch";
  }
  return "unknown";
}

std::string_view ToString(ExecutorBatchFallbackReason reason) {
  switch (reason) {
    case ExecutorBatchFallbackReason::kNone:
      return "none";
    case ExecutorBatchFallbackReason::kDisabledByRequest:
      return "disabled_by_request";
    case ExecutorBatchFallbackReason::kUnsupportedNode:
      return "unsupported_node";
    case ExecutorBatchFallbackReason::kResourceLimit:
      return "resource_limit";
    case ExecutorBatchFallbackReason::kMemoryPressure:
      return "memory_pressure";
    case ExecutorBatchFallbackReason::kBackpressure:
      return "backpressure";
    case ExecutorBatchFallbackReason::kExactErrorRequired:
      return "exact_error_required";
  }
  return "unknown";
}

}  // namespace scratchbird::engine::executor
