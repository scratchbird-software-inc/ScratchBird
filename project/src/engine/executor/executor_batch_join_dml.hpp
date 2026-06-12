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
#include <functional>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

// DPC_EXECUTOR_BATCH_JOIN_DML: join and DML-facing executor batch routes.
struct ExecutorBatchJoinRequest {
  ExecutorBatchRequest batch_request;
  std::size_t left_column = 0;
  std::size_t right_column = 0;
  std::string output_descriptor_digest;
  ExecutorRowStep left_row_validation;
};

struct ExecutorBatchJoinCounters {
  std::size_t left_rows_scanned = 0;
  std::size_t right_rows_materialized = 0;
  std::size_t right_hash_buckets = 0;
  std::size_t nested_loop_right_rows_scanned = 0;
  std::size_t hash_join_left_probes = 0;
  std::size_t join_matches = 0;
};

enum class ExecutorBatchDmlOperation {
  kUpdateReturning,
  kDeleteReturning,
};

struct ExecutorBatchDmlRowStepResult {
  bool ok = true;
  Tuple new_row;
  Tuple returning_row;
  bool emit_returning = true;
  std::string diagnostic_code = "SB_EXECUTOR_OK";
  std::vector<std::string> message_vector;
};

struct ExecutorBatchDmlIntent {
  ExecutorBatchDmlOperation operation = ExecutorBatchDmlOperation::kUpdateReturning;
  std::size_t input_row_index = 0;
  Tuple old_row;
  Tuple new_row;
  Tuple returning_row;
  bool emits_returning = true;
};

using ExecutorBatchDmlRowStep =
    std::function<ExecutorBatchDmlRowStepResult(const Tuple& row,
                                                std::size_t row_index)>;

struct ExecutorBatchDmlRequest {
  ExecutorBatchRequest batch_request;
  ExecutorBatchDmlOperation operation = ExecutorBatchDmlOperation::kUpdateReturning;
  std::string returning_descriptor_digest;
  ExecutorBatchDmlRowStep row_step;
};

struct ExecutorBatchDmlCounters {
  std::size_t rows_matched = 0;
  std::size_t update_intents = 0;
  std::size_t delete_intents = 0;
  std::size_t returning_rows = 0;
  std::size_t intent_envelopes = 0;
};

struct ExecutorBatchJoinDmlAuthorityEvidence {
  bool owns_transaction_finality = false;
  bool owns_visibility = false;
  bool owns_rollback = false;
  bool owns_recovery = false;
  bool owns_parser_execution = false;
  bool owns_timestamp_ordering = false;
  bool owns_reference_storage = false;
  bool owns_sql_execution = false;
  bool owns_durable_commit = false;
};

struct ExecutorBatchJoinEvidence {
  ExecutorBatchEvidence primitive_evidence;
  ExecutorBatchSelectedMode selected_mode = ExecutorBatchSelectedMode::kRowByRow;
  ExecutorBatchFallbackReason fallback_reason = ExecutorBatchFallbackReason::kNone;
  std::size_t left_rows_requested = 0;
  std::size_t right_rows_requested = 0;
  std::size_t rows_produced = 0;
  std::size_t rows_processed_row_by_row = 0;
  std::size_t rows_processed_in_batch = 0;
  ExecutorBatchJoinCounters counters;
  std::string deterministic_result_signature;
  std::string deterministic_ordering;
  bool preserves_nested_loop_order = true;
  bool hash_route_used = false;
  bool hash_candidate_matched_nested_loop_order = false;
  ExecutorBatchJoinDmlAuthorityEvidence authority;
};

struct ExecutorBatchJoinResult {
  Batch output;
  ExecutorBatchJoinEvidence evidence;
};

struct ExecutorBatchDmlEvidence {
  ExecutorBatchEvidence primitive_evidence;
  ExecutorBatchSelectedMode selected_mode = ExecutorBatchSelectedMode::kRowByRow;
  ExecutorBatchFallbackReason fallback_reason = ExecutorBatchFallbackReason::kNone;
  std::size_t rows_requested = 0;
  std::size_t rows_produced = 0;
  std::size_t rows_processed_row_by_row = 0;
  std::size_t rows_processed_in_batch = 0;
  ExecutorBatchDmlCounters counters;
  std::string deterministic_result_signature;
  bool preserves_input_order = true;
  bool no_partial_success = true;
  ExecutorBatchJoinDmlAuthorityEvidence authority;
};

struct ExecutorBatchDmlResult {
  Batch returning_rows;
  std::vector<ExecutorBatchDmlIntent> intents;
  ExecutorBatchDmlEvidence evidence;
};

ExecutorBatchJoinResult ExecuteBatchedJoinEqual(
    const Batch& left,
    const Batch& right,
    const ExecutorBatchJoinRequest& request);

ExecutorBatchDmlResult ExecuteBatchedDmlReturning(
    const Batch& input,
    const ExecutorBatchDmlRequest& request);

}  // namespace scratchbird::engine::executor
