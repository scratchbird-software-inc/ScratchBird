// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "executor_batch_join_dml.hpp"

#include "executor_batch_relational.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

bool HasColumn(const Tuple& tuple, std::size_t column) {
  return column < tuple.values.size();
}

std::vector<std::int64_t> ConcatValues(const Tuple& left, const Tuple& right) {
  std::vector<std::int64_t> values = left.values;
  values.insert(values.end(), right.values.begin(), right.values.end());
  return values;
}

Batch WithDescriptor(Batch batch, const std::string& descriptor_digest) {
  if (!descriptor_digest.empty()) {
    batch.descriptor_digest = descriptor_digest;
  }
  return batch;
}

std::string JoinDescriptor(const Batch& left, const Batch& right) {
  return left.descriptor_digest + "+" + right.descriptor_digest;
}

Batch DeterministicHashJoinEqual(const Batch& left,
                                 const Batch& right,
                                 std::size_t left_column,
                                 std::size_t right_column,
                                 ExecutorBatchJoinCounters* counters) {
  std::unordered_map<std::int64_t, std::vector<std::size_t>> hash;
  for (std::size_t right_index = 0; right_index < right.rows.size(); ++right_index) {
    const auto& right_row = right.rows[right_index];
    if (!HasColumn(right_row, right_column)) {
      continue;
    }
    hash[right_row.values[right_column]].push_back(right_index);
  }

  if (counters != nullptr) {
    counters->right_rows_materialized = right.rows.size();
    counters->right_hash_buckets = hash.size();
  }

  std::vector<Tuple> rows;
  for (const auto& left_row : left.rows) {
    if (!HasColumn(left_row, left_column)) {
      continue;
    }
    if (counters != nullptr) {
      ++counters->hash_join_left_probes;
    }
    const auto bucket = hash.find(left_row.values[left_column]);
    if (bucket == hash.end()) {
      continue;
    }
    for (const auto right_index : bucket->second) {
      rows.push_back({.values = ConcatValues(left_row, right.rows[right_index])});
    }
  }
  return MakeBatch(JoinDescriptor(left, right), std::move(rows));
}

ExecutorBatchJoinEvidence BuildJoinEvidence(
    const ExecutorBatchEvidence& primitive_evidence,
    const Batch& output,
    const ExecutorBatchJoinCounters& counters,
    std::size_t left_rows_requested,
    std::size_t right_rows_requested,
    bool hash_route_used,
    bool hash_candidate_matched_nested_loop_order) {
  ExecutorBatchJoinEvidence evidence;
  evidence.primitive_evidence = primitive_evidence;
  evidence.selected_mode = primitive_evidence.selected_mode;
  evidence.fallback_reason = primitive_evidence.fallback_reason;
  evidence.left_rows_requested = left_rows_requested;
  evidence.right_rows_requested = right_rows_requested;
  evidence.rows_produced = output.rows.size();
  evidence.rows_processed_row_by_row =
      primitive_evidence.rows_processed_row_by_row;
  evidence.rows_processed_in_batch = primitive_evidence.rows_processed_in_batch;
  evidence.counters = counters;
  evidence.counters.join_matches = output.rows.size();
  evidence.deterministic_result_signature = DeterministicBatchSignature(output);
  evidence.deterministic_ordering = "left_input_order_then_right_input_order";
  evidence.preserves_nested_loop_order = true;
  evidence.hash_route_used = hash_route_used;
  evidence.hash_candidate_matched_nested_loop_order =
      hash_candidate_matched_nested_loop_order;
  return evidence;
}

ExecutorBatchDmlEvidence BuildDmlEvidence(
    const ExecutorBatchEvidence& primitive_evidence,
    const Batch& output,
    const ExecutorBatchDmlCounters& counters) {
  ExecutorBatchDmlEvidence evidence;
  evidence.primitive_evidence = primitive_evidence;
  evidence.selected_mode = primitive_evidence.selected_mode;
  evidence.fallback_reason = primitive_evidence.fallback_reason;
  evidence.rows_requested = primitive_evidence.rows_requested;
  evidence.rows_produced = output.rows.size();
  evidence.rows_processed_row_by_row =
      primitive_evidence.rows_processed_row_by_row;
  evidence.rows_processed_in_batch = primitive_evidence.rows_processed_in_batch;
  evidence.counters = counters;
  evidence.deterministic_result_signature = DeterministicBatchSignature(output);
  evidence.preserves_input_order = true;
  evidence.no_partial_success = true;
  return evidence;
}

}  // namespace

ExecutorBatchJoinResult ExecuteBatchedJoinEqual(
    const Batch& left,
    const Batch& right,
    const ExecutorBatchJoinRequest& request) {
  ExecutorBatchJoinCounters counters;
  const auto row_step =
      [&](const Tuple& row, std::size_t row_index) -> ExecutorRowStepResult {
    ++counters.left_rows_scanned;
    if (request.left_row_validation) {
      auto validation = request.left_row_validation(row, row_index);
      validation.emit_row = false;
      return validation;
    }
    ExecutorRowStepResult result;
    result.emit_row = false;
    return result;
  };

  auto primitive = ExecuteScopedExecutorBatch(left, request.batch_request, row_step);

  Batch output = MakeBatch(JoinDescriptor(left, right), {});
  bool hash_route_used = false;
  bool hash_candidate_matched_nested_loop_order = false;
  if (!primitive.evidence.cancelled && !primitive.evidence.error) {
    if (primitive.evidence.selected_mode == ExecutorBatchSelectedMode::kBatch) {
      hash_route_used = true;
      auto deterministic_hash = DeterministicHashJoinEqual(left,
                                                          right,
                                                          request.left_column,
                                                          request.right_column,
                                                          &counters);
      const auto nested_loop = NestedLoopJoinEqual(left,
                                                   right,
                                                   request.left_column,
                                                   request.right_column);
      const auto existing_hash = HashJoinEqual(left,
                                               right,
                                               request.left_column,
                                               request.right_column);
      hash_candidate_matched_nested_loop_order =
          DeterministicBatchSignature(existing_hash) ==
          DeterministicBatchSignature(nested_loop);
      output = DeterministicBatchSignature(deterministic_hash) ==
                       DeterministicBatchSignature(nested_loop)
                   ? std::move(deterministic_hash)
                   : nested_loop;
    } else {
      counters.nested_loop_right_rows_scanned =
          left.rows.size() * right.rows.size();
      output = NestedLoopJoinEqual(left,
                                   right,
                                   request.left_column,
                                   request.right_column);
    }
    output = WithDescriptor(std::move(output), request.output_descriptor_digest);
  }

  ExecutorBatchJoinResult result;
  result.output = std::move(output);
  result.evidence = BuildJoinEvidence(primitive.evidence,
                                      result.output,
                                      counters,
                                      left.rows.size(),
                                      right.rows.size(),
                                      hash_route_used,
                                      hash_candidate_matched_nested_loop_order);
  return result;
}

ExecutorBatchDmlResult ExecuteBatchedDmlReturning(
    const Batch& input,
    const ExecutorBatchDmlRequest& request) {
  ExecutorBatchDmlCounters counters;
  std::vector<ExecutorBatchDmlIntent> intents;

  const auto row_step =
      [&](const Tuple& row, std::size_t row_index) -> ExecutorRowStepResult {
    if (!request.row_step) {
      ExecutorRowStepResult result;
      result.ok = false;
      result.emit_row = false;
      result.diagnostic_code = "SB_EXECUTOR_BATCH_DML_ROW_STEP_REQUIRED";
      result.message_vector = {result.diagnostic_code};
      return result;
    }

    const auto dml_step = request.row_step(row, row_index);
    if (!dml_step.ok) {
      ExecutorRowStepResult result;
      result.ok = false;
      result.emit_row = false;
      result.diagnostic_code = dml_step.diagnostic_code;
      result.message_vector = dml_step.message_vector;
      return result;
    }

    ++counters.rows_matched;
    ExecutorBatchDmlIntent intent;
    intent.operation = request.operation;
    intent.input_row_index = row_index;
    intent.old_row = row;
    intent.new_row = dml_step.new_row;
    intent.returning_row = dml_step.returning_row;
    intent.emits_returning = dml_step.emit_returning;
    intents.push_back(std::move(intent));
    ++counters.intent_envelopes;
    if (request.operation == ExecutorBatchDmlOperation::kUpdateReturning) {
      ++counters.update_intents;
    } else {
      ++counters.delete_intents;
    }
    if (dml_step.emit_returning) {
      ++counters.returning_rows;
    }

    ExecutorRowStepResult result;
    result.emit_row = dml_step.emit_returning;
    result.row = dml_step.returning_row;
    return result;
  };

  auto primitive = ExecuteScopedExecutorBatch(input, request.batch_request, row_step);
  primitive.output =
      WithDescriptor(std::move(primitive.output), request.returning_descriptor_digest);

  if (primitive.evidence.cancelled || primitive.evidence.error) {
    intents.clear();
    counters.intent_envelopes = 0;
    counters.update_intents = 0;
    counters.delete_intents = 0;
    counters.returning_rows = 0;
  }

  ExecutorBatchDmlResult result;
  result.returning_rows = std::move(primitive.output);
  result.intents = std::move(intents);
  result.evidence = BuildDmlEvidence(primitive.evidence,
                                     result.returning_rows,
                                     counters);
  return result;
}

}  // namespace scratchbird::engine::executor
