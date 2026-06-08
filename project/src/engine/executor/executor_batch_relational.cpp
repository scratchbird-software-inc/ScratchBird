// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "executor_batch_relational.hpp"

#include <sstream>
#include <string>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

bool HasColumn(const Tuple& tuple, std::size_t column) {
  return column < tuple.values.size();
}

bool MatchesFilter(std::int64_t lhs,
                   Int64ComparisonOperator op,
                   std::int64_t rhs) {
  switch (op) {
    case Int64ComparisonOperator::kGreaterThan:
      return lhs > rhs;
    case Int64ComparisonOperator::kGreaterThanOrEqual:
      return lhs >= rhs;
    case Int64ComparisonOperator::kLessThan:
      return lhs < rhs;
    case Int64ComparisonOperator::kLessThanOrEqual:
      return lhs <= rhs;
    case Int64ComparisonOperator::kEqual:
      return lhs == rhs;
    case Int64ComparisonOperator::kNotEqual:
      return lhs != rhs;
  }
  return false;
}

std::string IndexMessage(std::string prefix, std::size_t value) {
  return prefix + std::to_string(value);
}

ExecutorRowStepResult MissingColumnError(std::string diagnostic_code,
                                         std::size_t row_index,
                                         std::size_t column) {
  ExecutorRowStepResult result;
  result.ok = false;
  result.emit_row = false;
  result.diagnostic_code = std::move(diagnostic_code);
  result.message_vector = {
      result.diagnostic_code,
      IndexMessage("row_index=", row_index),
      IndexMessage("column=", column),
  };
  return result;
}

ExecutorBatchRelationalEvidence BuildEvidence(
    const ExecutorBatchEvidence& primitive_evidence,
    const ExecutorBatchRelationalCounters& counters,
    const Batch& output,
    bool preserves_input_order,
    bool aggregate_group_order_deterministic) {
  ExecutorBatchRelationalEvidence evidence;
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
  evidence.preserves_input_order = preserves_input_order;
  evidence.aggregate_group_order_deterministic =
      aggregate_group_order_deterministic;
  evidence.authority.owns_transaction_finality =
      primitive_evidence.authority.owns_transaction_finality;
  evidence.authority.owns_visibility = primitive_evidence.authority.owns_visibility;
  evidence.authority.owns_rollback = primitive_evidence.authority.owns_rollback;
  evidence.authority.owns_recovery = primitive_evidence.authority.owns_recovery;
  evidence.authority.owns_parser_execution =
      primitive_evidence.authority.owns_parser_execution;
  return evidence;
}

Batch WithDescriptor(Batch batch, const std::string& descriptor_digest) {
  if (!descriptor_digest.empty()) {
    batch.descriptor_digest = descriptor_digest;
  }
  return batch;
}

}  // namespace

std::string DeterministicBatchSignature(const Batch& batch) {
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

ExecutorBatchRelationalResult ExecuteBatchedScanFilterProjection(
    const Batch& input,
    const ExecutorBatchScanFilterProjectionRequest& request) {
  ExecutorBatchRelationalCounters counters;
  const auto row_step =
      [&](const Tuple& row, std::size_t row_index) -> ExecutorRowStepResult {
    ++counters.rows_scanned;

    if (request.filter.enabled) {
      ++counters.rows_filter_evaluated;
      if (!HasColumn(row, request.filter.column)) {
        return MissingColumnError(
            "SB_EXECUTOR_BATCH_RELATIONAL_FILTER_COLUMN_REQUIRED",
            row_index,
            request.filter.column);
      }
      if (!MatchesFilter(row.values[request.filter.column],
                         request.filter.op,
                         request.filter.value)) {
        ExecutorRowStepResult result;
        result.emit_row = false;
        return result;
      }
      ++counters.rows_filter_passed;
    }

    ExecutorRowStepResult result;
    if (request.projection_columns.empty()) {
      result.row = row;
    } else {
      result.row.values.reserve(request.projection_columns.size());
      for (const auto column : request.projection_columns) {
        if (!HasColumn(row, column)) {
          return MissingColumnError(
              "SB_EXECUTOR_BATCH_RELATIONAL_PROJECTION_COLUMN_REQUIRED",
              row_index,
              column);
        }
        result.row.values.push_back(row.values[column]);
      }
    }
    ++counters.rows_projected;
    return result;
  };

  auto primitive =
      ExecuteScopedExecutorBatch(input, request.batch_request, row_step);
  primitive.output =
      WithDescriptor(std::move(primitive.output), request.output_descriptor_digest);

  ExecutorBatchRelationalResult result;
  result.output = std::move(primitive.output);
  result.evidence = BuildEvidence(primitive.evidence,
                                  counters,
                                  result.output,
                                  request.batch_request.preserve_input_order,
                                  false);
  return result;
}

ExecutorBatchRelationalResult ExecuteBatchedAggregateSumByKey(
    const Batch& input,
    const ExecutorBatchAggregateSumRequest& request) {
  ExecutorBatchRelationalCounters counters;
  const auto row_step =
      [&](const Tuple& row, std::size_t row_index) -> ExecutorRowStepResult {
    if (!HasColumn(row, request.key_column)) {
      return MissingColumnError(
          "SB_EXECUTOR_BATCH_RELATIONAL_AGGREGATE_KEY_COLUMN_REQUIRED",
          row_index,
          request.key_column);
    }
    if (!HasColumn(row, request.value_column)) {
      return MissingColumnError(
          "SB_EXECUTOR_BATCH_RELATIONAL_AGGREGATE_VALUE_COLUMN_REQUIRED",
          row_index,
          request.value_column);
    }
    ++counters.aggregate_input_rows;
    ExecutorRowStepResult result;
    result.row = row;
    return result;
  };

  auto primitive =
      ExecuteScopedExecutorBatch(input, request.batch_request, row_step);

  Batch output = primitive.output;
  if (!primitive.evidence.cancelled && !primitive.evidence.error) {
    output = AggregateSumByKey(primitive.output,
                               request.key_column,
                               request.value_column);
    output = WithDescriptor(std::move(output), request.output_descriptor_digest);
    counters.aggregate_groups = output.rows.size();
  }

  ExecutorBatchRelationalResult result;
  result.output = std::move(output);
  result.evidence = BuildEvidence(primitive.evidence,
                                  counters,
                                  result.output,
                                  false,
                                  true);
  return result;
}

}  // namespace scratchbird::engine::executor
