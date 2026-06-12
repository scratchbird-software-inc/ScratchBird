// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "executor_batching.hpp"
#include "executor_foundation.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

// DPC_EXECUTOR_BATCH_RELATIONAL: scan/filter/projection/aggregate batch routes.
struct ExecutorBatchRelationalFilter {
  bool enabled = false;
  std::size_t column = 0;
  Int64ComparisonOperator op = Int64ComparisonOperator::kEqual;
  std::int64_t value = 0;
};

struct ExecutorBatchScanFilterProjectionRequest {
  ExecutorBatchRequest batch_request;
  ExecutorBatchRelationalFilter filter;
  std::vector<std::size_t> projection_columns;
  std::string output_descriptor_digest;
};

struct ExecutorBatchAggregateSumRequest {
  ExecutorBatchRequest batch_request;
  std::size_t key_column = 0;
  std::size_t value_column = 1;
  std::string output_descriptor_digest;
};

struct ExecutorBatchRelationalCounters {
  std::size_t rows_scanned = 0;
  std::size_t rows_filter_evaluated = 0;
  std::size_t rows_filter_passed = 0;
  std::size_t rows_projected = 0;
  std::size_t aggregate_input_rows = 0;
  std::size_t aggregate_groups = 0;
};

struct ExecutorBatchRelationalAuthorityEvidence {
  bool owns_transaction_finality = false;
  bool owns_visibility = false;
  bool owns_rollback = false;
  bool owns_recovery = false;
  bool owns_parser_execution = false;
  bool owns_timestamp_ordering = false;
  bool owns_reference_storage = false;
  bool owns_sql_execution = false;
};

struct ExecutorBatchRelationalEvidence {
  ExecutorBatchEvidence primitive_evidence;
  ExecutorBatchSelectedMode selected_mode = ExecutorBatchSelectedMode::kRowByRow;
  ExecutorBatchFallbackReason fallback_reason = ExecutorBatchFallbackReason::kNone;
  std::size_t rows_requested = 0;
  std::size_t rows_produced = 0;
  std::size_t rows_processed_row_by_row = 0;
  std::size_t rows_processed_in_batch = 0;
  ExecutorBatchRelationalCounters counters;
  std::string deterministic_result_signature;
  bool preserves_input_order = true;
  bool aggregate_group_order_deterministic = false;
  ExecutorBatchRelationalAuthorityEvidence authority;
};

struct ExecutorBatchRelationalResult {
  Batch output;
  ExecutorBatchRelationalEvidence evidence;
};

ExecutorBatchRelationalResult ExecuteBatchedScanFilterProjection(
    const Batch& input,
    const ExecutorBatchScanFilterProjectionRequest& request);

ExecutorBatchRelationalResult ExecuteBatchedAggregateSumByKey(
    const Batch& input,
    const ExecutorBatchAggregateSumRequest& request);

std::string DeterministicBatchSignature(const Batch& batch);

}  // namespace scratchbird::engine::executor
