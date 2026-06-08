// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "executor_batch_failure_policy.hpp"

#include <limits>

namespace scratchbird::engine::executor {
namespace {

constexpr std::size_t kForcedResourceLimitRows = 0;

void CopyPrimitiveEvidence(const ExecutorBatchEvidence& source,
                           ExecutorBatchFailureEvidence& target) {
  target.selected_mode = source.selected_mode;
  target.fallback_reason = source.fallback_reason;
  target.cancelled = source.cancelled;
  target.error = source.error;
  target.diagnostic_code = source.diagnostic_code;
  target.message_vector = source.message_vector;
}

bool NoPartialRowsEscaped(const ExecutorBatchEvidence& evidence,
                          const Batch& output) {
  if (!evidence.cancelled && !evidence.error) {
    return true;
  }
  return evidence.rows_produced == 0 && output.rows.empty();
}

}  // namespace

ExecutorBatchRequest ApplyExecutorBatchFailurePolicy(
    ExecutorBatchRequest request,
    const ExecutorBatchFailurePolicy& policy) {
  if (policy.disabled) {
    request.requested_mode = ExecutorBatchRequestMode::kDisabledRowByRow;
  }
  if (policy.unsupported_node) {
    request.node_supports_batch = false;
  }
  if (policy.resource_limit) {
    request.limits.max_batch_rows = kForcedResourceLimitRows;
  }
  if (policy.memory_pressure) {
    request.limits.memory_pressure = true;
  }
  if (policy.backpressure) {
    request.backpressure_active = true;
  }
  if (policy.exact_error_required) {
    request.exact_error_required = true;
  }
  if (policy.cancellation.cancel_before_start ||
      policy.cancellation.cancel_before_row !=
          std::numeric_limits<std::size_t>::max()) {
    request.cancellation = policy.cancellation;
  }
  return request;
}

ExecutorBatchFailureEvidence BuildExecutorBatchFailureEvidence(
    const ExecutorBatchResult& result) {
  ExecutorBatchFailureEvidence evidence;
  evidence.path = ExecutorBatchEvidencePath::kPrimitive;
  CopyPrimitiveEvidence(result.evidence, evidence);
  evidence.no_partial_output =
      NoPartialRowsEscaped(result.evidence, result.output);
  evidence.output_rows = result.output.rows.size();
  evidence.deterministic_result_signature =
      DeterministicBatchSignature(result.output);
  return evidence;
}

ExecutorBatchFailureEvidence BuildExecutorBatchFailureEvidence(
    const ExecutorBatchRelationalResult& result) {
  ExecutorBatchFailureEvidence evidence;
  evidence.path = ExecutorBatchEvidencePath::kRelational;
  CopyPrimitiveEvidence(result.evidence.primitive_evidence, evidence);
  evidence.no_partial_output =
      NoPartialRowsEscaped(result.evidence.primitive_evidence, result.output);
  evidence.output_rows = result.output.rows.size();
  evidence.deterministic_result_signature =
      result.evidence.deterministic_result_signature;
  return evidence;
}

ExecutorBatchFailureEvidence BuildExecutorBatchFailureEvidence(
    const ExecutorBatchJoinResult& result) {
  ExecutorBatchFailureEvidence evidence;
  evidence.path = ExecutorBatchEvidencePath::kJoin;
  CopyPrimitiveEvidence(result.evidence.primitive_evidence, evidence);
  evidence.no_partial_output =
      NoPartialRowsEscaped(result.evidence.primitive_evidence, result.output);
  evidence.output_rows = result.output.rows.size();
  evidence.deterministic_result_signature =
      result.evidence.deterministic_result_signature;
  return evidence;
}

ExecutorBatchFailureEvidence BuildExecutorBatchFailureEvidence(
    const ExecutorBatchDmlResult& result) {
  ExecutorBatchFailureEvidence evidence;
  evidence.path = ExecutorBatchEvidencePath::kDml;
  CopyPrimitiveEvidence(result.evidence.primitive_evidence, evidence);
  evidence.no_partial_output =
      NoPartialRowsEscaped(result.evidence.primitive_evidence,
                           result.returning_rows);
  evidence.no_partial_intents =
      (!evidence.cancelled && !evidence.error) || result.intents.empty();
  evidence.output_rows = result.returning_rows.rows.size();
  evidence.intent_count = result.intents.size();
  evidence.deterministic_result_signature =
      result.evidence.deterministic_result_signature;
  return evidence;
}

std::string_view ToString(ExecutorBatchEvidencePath path) {
  switch (path) {
    case ExecutorBatchEvidencePath::kPrimitive:
      return "primitive";
    case ExecutorBatchEvidencePath::kRelational:
      return "relational";
    case ExecutorBatchEvidencePath::kJoin:
      return "join";
    case ExecutorBatchEvidencePath::kDml:
      return "dml";
  }
  return "unknown";
}

}  // namespace scratchbird::engine::executor
