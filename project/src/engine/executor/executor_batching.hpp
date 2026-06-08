// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "executor_foundation.hpp"

#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::executor {

// DPC_EXECUTOR_BATCH: scoped executor batch primitive foundation.
enum class ExecutorBatchRequestMode {
  kDisabledRowByRow,
  kPreferBatch,
};

enum class ExecutorBatchSelectedMode {
  kRowByRow,
  kBatch,
};

enum class ExecutorBatchFallbackReason {
  kNone,
  kDisabledByRequest,
  kUnsupportedNode,
  kResourceLimit,
  kMemoryPressure,
  kBackpressure,
  kExactErrorRequired,
};

struct ExecutorBatchResourceLimits {
  std::size_t max_batch_rows = 1024;
  std::size_t max_materialized_cells = 16384;
  std::size_t max_materialized_bytes = 1024 * 1024;
  bool memory_pressure = false;
};

struct ExecutorBatchCancellation {
  bool cancel_before_start = false;
  std::size_t cancel_before_row = std::numeric_limits<std::size_t>::max();
  std::string diagnostic_code = "SB_EXECUTOR_BATCH_CANCELLED";
};

struct ExecutorBatchRequest {
  ExecutorBatchRequestMode requested_mode = ExecutorBatchRequestMode::kPreferBatch;
  bool node_supports_batch = true;
  bool preserve_input_order = true;
  bool backpressure_active = false;
  bool exact_error_required = false;
  ExecutorBatchResourceLimits limits;
  ExecutorBatchCancellation cancellation;
};

struct ExecutorRowStepResult {
  bool ok = true;
  bool emit_row = true;
  Tuple row;
  std::string diagnostic_code = "SB_EXECUTOR_OK";
  std::vector<std::string> message_vector;
};

struct ExecutorBatchResourceCounters {
  std::size_t max_observed_batch_rows = 0;
  std::size_t max_observed_materialized_cells = 0;
  std::size_t max_observed_materialized_bytes = 0;
};

struct ExecutorBatchAuthorityEvidence {
  bool owns_transaction_finality = false;
  bool owns_visibility = false;
  bool owns_rollback = false;
  bool owns_recovery = false;
  bool owns_parser_execution = false;
};

struct ExecutorBatchEvidence {
  ExecutorBatchSelectedMode selected_mode = ExecutorBatchSelectedMode::kRowByRow;
  ExecutorBatchFallbackReason fallback_reason = ExecutorBatchFallbackReason::kNone;
  std::size_t rows_requested = 0;
  std::size_t rows_produced = 0;
  std::size_t rows_processed_row_by_row = 0;
  std::size_t rows_processed_in_batch = 0;
  ExecutorBatchResourceCounters resource_counters;
  bool cancelled = false;
  bool error = false;
  std::size_t diagnostic_row_index = std::numeric_limits<std::size_t>::max();
  std::string diagnostic_code = "SB_EXECUTOR_OK";
  std::vector<std::string> message_vector;
  ExecutorBatchAuthorityEvidence authority;
};

struct ExecutorBatchResult {
  Batch output;
  ExecutorBatchEvidence evidence;
};

using ExecutorRowStep =
    std::function<ExecutorRowStepResult(const Tuple& row, std::size_t row_index)>;

ExecutorBatchResult ExecuteScopedExecutorBatch(const Batch& input,
                                               const ExecutorBatchRequest& request,
                                               const ExecutorRowStep& row_step);

std::string_view ToString(ExecutorBatchSelectedMode mode);
std::string_view ToString(ExecutorBatchFallbackReason reason);

}  // namespace scratchbird::engine::executor
