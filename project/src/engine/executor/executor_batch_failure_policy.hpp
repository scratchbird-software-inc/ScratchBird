// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "executor_batch_join_dml.hpp"
#include "executor_batch_relational.hpp"
#include "executor_batching.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::executor {

// DPC_EXECUTOR_BATCH_FAILURE_POLICY: executor batch fallback/cancellation evidence.
enum class ExecutorBatchEvidencePath {
  kPrimitive,
  kRelational,
  kJoin,
  kDml,
};

struct ExecutorBatchFailurePolicy {
  bool disabled = false;
  bool unsupported_node = false;
  bool resource_limit = false;
  bool memory_pressure = false;
  bool backpressure = false;
  bool exact_error_required = false;
  ExecutorBatchCancellation cancellation;
};

struct ExecutorBatchFailureAuthorityFlags {
  bool owns_transaction_finality = false;
  bool owns_visibility = false;
  bool owns_rollback = false;
  bool owns_recovery = false;
  bool owns_parser_execution = false;
  bool owns_timestamp_ordering = false;
  bool owns_donor_storage = false;
  bool owns_sql_execution = false;
  bool owns_durable_commit = false;
};

struct ExecutorBatchFailureEvidence {
  ExecutorBatchEvidencePath path = ExecutorBatchEvidencePath::kPrimitive;
  ExecutorBatchSelectedMode selected_mode = ExecutorBatchSelectedMode::kRowByRow;
  ExecutorBatchFallbackReason fallback_reason = ExecutorBatchFallbackReason::kNone;
  bool cancelled = false;
  bool error = false;
  std::string diagnostic_code = "SB_EXECUTOR_OK";
  std::vector<std::string> message_vector;
  bool no_partial_output = true;
  bool no_partial_intents = true;
  std::size_t output_rows = 0;
  std::size_t intent_count = 0;
  std::string deterministic_result_signature;
  std::string rollback_authority_source = "MGA/engine transaction layer";
  ExecutorBatchFailureAuthorityFlags authority_flags;
};

ExecutorBatchRequest ApplyExecutorBatchFailurePolicy(
    ExecutorBatchRequest request,
    const ExecutorBatchFailurePolicy& policy);

ExecutorBatchFailureEvidence BuildExecutorBatchFailureEvidence(
    const ExecutorBatchResult& result);

ExecutorBatchFailureEvidence BuildExecutorBatchFailureEvidence(
    const ExecutorBatchRelationalResult& result);

ExecutorBatchFailureEvidence BuildExecutorBatchFailureEvidence(
    const ExecutorBatchJoinResult& result);

ExecutorBatchFailureEvidence BuildExecutorBatchFailureEvidence(
    const ExecutorBatchDmlResult& result);

std::string_view ToString(ExecutorBatchEvidencePath path);

}  // namespace scratchbird::engine::executor
