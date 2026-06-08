// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "executor_batch_failure_policy.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;

constexpr std::string_view kGateSearchKey =
    "DPC_EXECUTOR_BATCH_FAILURE_FALLBACK_GATE";
constexpr std::string_view kImplementationSearchKey =
    "DPC_EXECUTOR_BATCH_FAILURE_POLICY";
constexpr std::string_view kRollbackAuthority =
    "MGA/engine transaction layer";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

exec::Batch RelationalCorpus() {
  return exec::MakeBatch("dpc_executor_batch_failure_rel_v1",
                         {
                             {.values = {0, 2, 5, 1}},
                             {.values = {1, 1, 7, 3}},
                             {.values = {2, 2, 11, 4}},
                             {.values = {3, 3, 13, 2}},
                             {.values = {4, 1, 17, 5}},
                         });
}

exec::Batch LeftJoinCorpus() {
  return exec::MakeBatch("dpc_executor_batch_failure_join_left_v1",
                         {
                             {.values = {1, 100}},
                             {.values = {2, 200}},
                             {.values = {1, 101}},
                             {.values = {3, 300}},
                             {.values = {2, 201}},
                         });
}

exec::Batch RightJoinCorpus() {
  return exec::MakeBatch("dpc_executor_batch_failure_join_right_v1",
                         {
                             {.values = {1, 10}},
                             {.values = {2, 20}},
                             {.values = {1, 11}},
                             {.values = {4, 40}},
                             {.values = {2, 21}},
                         });
}

exec::Batch DmlCorpus() {
  return exec::MakeBatch("dpc_executor_batch_failure_dml_input_v1",
                         {
                             {.values = {10, 1}},
                             {.values = {11, 2}},
                             {.values = {12, 3}},
                             {.values = {13, 4}},
                         });
}

exec::ExecutorBatchRequest BaseBatchRequest() {
  exec::ExecutorBatchRequest request;
  request.requested_mode = exec::ExecutorBatchRequestMode::kPreferBatch;
  request.node_supports_batch = true;
  request.preserve_input_order = true;
  request.limits.max_batch_rows = 16;
  request.limits.max_materialized_cells = 128;
  request.limits.max_materialized_bytes = 1024;
  return request;
}

exec::ExecutorBatchScanFilterProjectionRequest PipelineRequest(
    const exec::ExecutorBatchFailurePolicy& policy = {}) {
  exec::ExecutorBatchScanFilterProjectionRequest request;
  request.batch_request =
      exec::ApplyExecutorBatchFailurePolicy(BaseBatchRequest(), policy);
  request.filter.enabled = true;
  request.filter.column = 3;
  request.filter.op = exec::Int64ComparisonOperator::kGreaterThanOrEqual;
  request.filter.value = 2;
  request.projection_columns = {1, 2, 0};
  request.output_descriptor_digest =
      "dpc_executor_batch_failure_rel_projected_v1";
  return request;
}

exec::ExecutorBatchJoinRequest JoinRequest(
    const exec::ExecutorBatchFailurePolicy& policy = {}) {
  exec::ExecutorBatchJoinRequest request;
  request.batch_request =
      exec::ApplyExecutorBatchFailurePolicy(BaseBatchRequest(), policy);
  request.left_column = 0;
  request.right_column = 0;
  request.output_descriptor_digest = "dpc_executor_batch_failure_join_output_v1";
  return request;
}

exec::ExecutorBatchAggregateSumRequest AggregateRequest(
    const exec::ExecutorBatchFailurePolicy& policy = {}) {
  exec::ExecutorBatchAggregateSumRequest request;
  request.batch_request =
      exec::ApplyExecutorBatchFailurePolicy(BaseBatchRequest(), policy);
  request.key_column = 1;
  request.value_column = 2;
  request.output_descriptor_digest =
      "dpc_executor_batch_failure_rel_aggregate_v1";
  return request;
}

exec::ExecutorBatchDmlRequest UpdateRequest(
    const exec::ExecutorBatchFailurePolicy& policy = {}) {
  exec::ExecutorBatchDmlRequest request;
  request.batch_request =
      exec::ApplyExecutorBatchFailurePolicy(BaseBatchRequest(), policy);
  request.operation = exec::ExecutorBatchDmlOperation::kUpdateReturning;
  request.returning_descriptor_digest =
      "dpc_executor_batch_failure_dml_returning_v1";
  request.row_step =
      [](const exec::Tuple& row,
         std::size_t row_index) -> exec::ExecutorBatchDmlRowStepResult {
    exec::ExecutorBatchDmlRowStepResult result;
    result.new_row.values = {row.values[0], row.values[1] + 10};
    result.returning_row.values = {
        row.values[0],
        row.values[1] + 10,
        static_cast<std::int64_t>(row_index),
    };
    return result;
  };
  return request;
}

void RequireNoAuthorityOwned(
    const exec::ExecutorBatchFailureEvidence& evidence) {
  Require(evidence.rollback_authority_source == kRollbackAuthority,
          "DPC-053 rollback authority source must remain MGA/engine");
  Require(!evidence.authority_flags.owns_transaction_finality,
          "DPC-053 policy must not own transaction finality");
  Require(!evidence.authority_flags.owns_visibility,
          "DPC-053 policy must not own MGA visibility");
  Require(!evidence.authority_flags.owns_rollback,
          "DPC-053 policy must not own rollback");
  Require(!evidence.authority_flags.owns_recovery,
          "DPC-053 policy must not own recovery");
  Require(!evidence.authority_flags.owns_parser_execution,
          "DPC-053 policy must not own parser execution");
  Require(!evidence.authority_flags.owns_timestamp_ordering,
          "DPC-053 policy must not own timestamp ordering");
  Require(!evidence.authority_flags.owns_donor_storage,
          "DPC-053 policy must not own donor storage");
  Require(!evidence.authority_flags.owns_sql_execution,
          "DPC-053 policy must not own SQL execution");
  Require(!evidence.authority_flags.owns_durable_commit,
          "DPC-053 policy must not own durable commit");
}

void RequireFallbackEvidence(
    const exec::ExecutorBatchFailureEvidence& evidence,
    exec::ExecutorBatchFallbackReason reason,
    std::string_view diagnostic_code) {
  Require(evidence.selected_mode == exec::ExecutorBatchSelectedMode::kRowByRow,
          "fallback policy must select row-by-row");
  Require(evidence.fallback_reason == reason, "fallback reason drifted");
  Require(!evidence.cancelled, "fallback success must not report cancelled");
  Require(!evidence.error, "fallback success must not report error");
  Require(evidence.diagnostic_code == diagnostic_code,
          "fallback diagnostic code drifted");
  Require(evidence.message_vector.size() == 1,
          "fallback message vector size drifted");
  Require(evidence.message_vector.front() == diagnostic_code,
          "fallback message vector diagnostic drifted");
  Require(evidence.no_partial_output,
          "fallback success must report no partial output escape");
  Require(evidence.no_partial_intents,
          "fallback success must report no partial intent escape");
  RequireNoAuthorityOwned(evidence);
}

void RequireDiagnosticEvidence(
    const exec::ExecutorBatchFailureEvidence& evidence,
    bool cancelled,
    std::string_view diagnostic_code,
    std::string_view row_message) {
  Require(evidence.cancelled == cancelled, "diagnostic cancelled flag drifted");
  Require(evidence.error != cancelled, "diagnostic error flag drifted");
  Require(evidence.diagnostic_code == diagnostic_code,
          "diagnostic code drifted");
  Require(!evidence.message_vector.empty(),
          "diagnostic message vector must be present");
  Require(evidence.message_vector.front() == diagnostic_code,
          "diagnostic first message drifted");
  if (!row_message.empty()) {
    Require(evidence.message_vector.size() >= 2,
            "diagnostic row message missing");
    Require(evidence.message_vector[1] == row_message,
            "diagnostic row message drifted");
  }
  Require(evidence.output_rows == 0,
          "diagnostic path must not expose partial output rows");
  Require(evidence.no_partial_output,
          "diagnostic path must prove no partial output");
  Require(evidence.no_partial_intents,
          "diagnostic path must prove no partial intents");
  RequireNoAuthorityOwned(evidence);
}

void DisabledFallbackMatchesBaseline() {
  exec::ExecutorBatchFailurePolicy disabled_policy;
  disabled_policy.disabled = true;

  auto disabled_direct = PipelineRequest();
  disabled_direct.batch_request.requested_mode =
      exec::ExecutorBatchRequestMode::kDisabledRowByRow;
  const auto relational_baseline =
      exec::ExecuteBatchedScanFilterProjection(RelationalCorpus(),
                                               disabled_direct);
  const auto relational_policy =
      exec::ExecuteBatchedScanFilterProjection(RelationalCorpus(),
                                               PipelineRequest(disabled_policy));
  Require(relational_policy.evidence.deterministic_result_signature ==
              relational_baseline.evidence.deterministic_result_signature,
          "disabled relational policy must match disabled baseline");
  RequireFallbackEvidence(
      exec::BuildExecutorBatchFailureEvidence(relational_policy),
      exec::ExecutorBatchFallbackReason::kDisabledByRequest,
      "SB_EXECUTOR_BATCH_FALLBACK_DISABLED_BY_REQUEST");

  auto join_disabled_direct = JoinRequest();
  join_disabled_direct.batch_request.requested_mode =
      exec::ExecutorBatchRequestMode::kDisabledRowByRow;
  const auto join_baseline = exec::ExecuteBatchedJoinEqual(
      LeftJoinCorpus(), RightJoinCorpus(), join_disabled_direct);
  const auto join_policy = exec::ExecuteBatchedJoinEqual(
      LeftJoinCorpus(), RightJoinCorpus(), JoinRequest(disabled_policy));
  Require(join_policy.evidence.deterministic_result_signature ==
              join_baseline.evidence.deterministic_result_signature,
          "disabled join policy must match disabled baseline");
  RequireFallbackEvidence(
      exec::BuildExecutorBatchFailureEvidence(join_policy),
      exec::ExecutorBatchFallbackReason::kDisabledByRequest,
      "SB_EXECUTOR_BATCH_FALLBACK_DISABLED_BY_REQUEST");

  auto dml_disabled_direct = UpdateRequest();
  dml_disabled_direct.batch_request.requested_mode =
      exec::ExecutorBatchRequestMode::kDisabledRowByRow;
  const auto dml_baseline =
      exec::ExecuteBatchedDmlReturning(DmlCorpus(), dml_disabled_direct);
  const auto dml_policy =
      exec::ExecuteBatchedDmlReturning(DmlCorpus(), UpdateRequest(disabled_policy));
  Require(dml_policy.evidence.deterministic_result_signature ==
              dml_baseline.evidence.deterministic_result_signature,
          "disabled DML policy must match disabled baseline");
  Require(dml_policy.intents.size() == dml_baseline.intents.size(),
          "disabled DML intent count must match baseline");
  RequireFallbackEvidence(
      exec::BuildExecutorBatchFailureEvidence(dml_policy),
      exec::ExecutorBatchFallbackReason::kDisabledByRequest,
      "SB_EXECUTOR_BATCH_FALLBACK_DISABLED_BY_REQUEST");
}

void SuccessfulFallbackReasonsMatchBaseline() {
  exec::ExecutorBatchFailurePolicy disabled_policy;
  disabled_policy.disabled = true;
  const auto relational_baseline = exec::ExecuteBatchedScanFilterProjection(
      RelationalCorpus(), PipelineRequest(disabled_policy));
  const auto aggregate_baseline = exec::ExecuteBatchedAggregateSumByKey(
      RelationalCorpus(), AggregateRequest(disabled_policy));
  const auto join_baseline = exec::ExecuteBatchedJoinEqual(
      LeftJoinCorpus(), RightJoinCorpus(), JoinRequest(disabled_policy));
  const auto dml_baseline =
      exec::ExecuteBatchedDmlReturning(DmlCorpus(), UpdateRequest(disabled_policy));

  struct Case {
    exec::ExecutorBatchFailurePolicy policy;
    exec::ExecutorBatchFallbackReason reason;
    std::string_view diagnostic_code;
  };

  std::vector<Case> cases;
  cases.push_back({.policy = {.unsupported_node = true},
                   .reason = exec::ExecutorBatchFallbackReason::kUnsupportedNode,
                   .diagnostic_code =
                       "SB_EXECUTOR_BATCH_FALLBACK_UNSUPPORTED_NODE"});
  cases.push_back({.policy = {.resource_limit = true},
                   .reason = exec::ExecutorBatchFallbackReason::kResourceLimit,
                   .diagnostic_code =
                       "SB_EXECUTOR_BATCH_FALLBACK_RESOURCE_LIMIT"});
  cases.push_back({.policy = {.memory_pressure = true},
                   .reason = exec::ExecutorBatchFallbackReason::kMemoryPressure,
                   .diagnostic_code =
                       "SB_EXECUTOR_BATCH_FALLBACK_MEMORY_PRESSURE"});
  cases.push_back({.policy = {.backpressure = true},
                   .reason = exec::ExecutorBatchFallbackReason::kBackpressure,
                   .diagnostic_code =
                       "SB_EXECUTOR_BATCH_FALLBACK_BACKPRESSURE"});
  cases.push_back({.policy = {.exact_error_required = true},
                   .reason =
                       exec::ExecutorBatchFallbackReason::kExactErrorRequired,
                   .diagnostic_code =
                       "SB_EXECUTOR_BATCH_FALLBACK_EXACT_ERROR_REQUIRED"});

  for (const auto& item : cases) {
    const auto relational = exec::ExecuteBatchedScanFilterProjection(
        RelationalCorpus(), PipelineRequest(item.policy));
    Require(relational.evidence.deterministic_result_signature ==
                relational_baseline.evidence.deterministic_result_signature,
            "relational fallback signature must match disabled baseline");
    RequireFallbackEvidence(exec::BuildExecutorBatchFailureEvidence(relational),
                            item.reason,
                            item.diagnostic_code);

    const auto aggregate = exec::ExecuteBatchedAggregateSumByKey(
        RelationalCorpus(), AggregateRequest(item.policy));
    Require(aggregate.evidence.deterministic_result_signature ==
                aggregate_baseline.evidence.deterministic_result_signature,
            "aggregate fallback signature must match disabled baseline");
    RequireFallbackEvidence(exec::BuildExecutorBatchFailureEvidence(aggregate),
                            item.reason,
                            item.diagnostic_code);

    const auto join = exec::ExecuteBatchedJoinEqual(
        LeftJoinCorpus(), RightJoinCorpus(), JoinRequest(item.policy));
    Require(join.evidence.deterministic_result_signature ==
                join_baseline.evidence.deterministic_result_signature,
            "join fallback signature must match disabled baseline");
    RequireFallbackEvidence(exec::BuildExecutorBatchFailureEvidence(join),
                            item.reason,
                            item.diagnostic_code);

    const auto dml =
        exec::ExecuteBatchedDmlReturning(DmlCorpus(), UpdateRequest(item.policy));
    Require(dml.evidence.deterministic_result_signature ==
                dml_baseline.evidence.deterministic_result_signature,
            "DML fallback signature must match disabled baseline");
    Require(dml.intents.size() == dml_baseline.intents.size(),
            "DML fallback intent count must match disabled baseline");
    RequireFallbackEvidence(exec::BuildExecutorBatchFailureEvidence(dml),
                            item.reason,
                            item.diagnostic_code);
  }
}

void CancellationProducesExactEvidence() {
  exec::ExecutorBatchFailurePolicy cancel_policy;
  cancel_policy.cancellation.cancel_before_row = 2;
  cancel_policy.cancellation.diagnostic_code =
      "SB_EXECUTOR_BATCH_DPC053_CANCEL";

  const auto relational = exec::ExecuteBatchedScanFilterProjection(
      RelationalCorpus(), PipelineRequest(cancel_policy));
  RequireDiagnosticEvidence(exec::BuildExecutorBatchFailureEvidence(relational),
                            true,
                            "SB_EXECUTOR_BATCH_DPC053_CANCEL",
                            "");

  const auto join = exec::ExecuteBatchedJoinEqual(
      LeftJoinCorpus(), RightJoinCorpus(), JoinRequest(cancel_policy));
  RequireDiagnosticEvidence(exec::BuildExecutorBatchFailureEvidence(join),
                            true,
                            "SB_EXECUTOR_BATCH_DPC053_CANCEL",
                            "");

  const auto dml =
      exec::ExecuteBatchedDmlReturning(DmlCorpus(), UpdateRequest(cancel_policy));
  const auto dml_evidence = exec::BuildExecutorBatchFailureEvidence(dml);
  Require(dml.intents.empty(), "DML cancellation must clear intents");
  Require(dml_evidence.intent_count == 0,
          "DML cancellation policy evidence must report no intents");
  RequireDiagnosticEvidence(dml_evidence,
                            true,
                            "SB_EXECUTOR_BATCH_DPC053_CANCEL",
                            "");
}

void RowLevelErrorProducesExactEvidence() {
  auto relational_request = PipelineRequest();
  relational_request.projection_columns = {1, 9};
  const auto relational =
      exec::ExecuteBatchedScanFilterProjection(RelationalCorpus(),
                                               relational_request);
  RequireDiagnosticEvidence(
      exec::BuildExecutorBatchFailureEvidence(relational),
      false,
      "SB_EXECUTOR_BATCH_RELATIONAL_PROJECTION_COLUMN_REQUIRED",
      "row_index=1");

  auto join_request = JoinRequest();
  join_request.left_row_validation =
      [](const exec::Tuple& row, std::size_t row_index) {
    exec::ExecutorRowStepResult result;
    result.emit_row = false;
    if (row_index == 2) {
      result.ok = false;
      result.diagnostic_code = "SB_EXECUTOR_BATCH_DPC053_JOIN_ROW_ERROR";
      result.message_vector = {
          "SB_EXECUTOR_BATCH_DPC053_JOIN_ROW_ERROR",
          "row_index=2",
          "join_input_validation",
      };
    }
    return result;
  };
  const auto join = exec::ExecuteBatchedJoinEqual(
      LeftJoinCorpus(), RightJoinCorpus(), join_request);
  RequireDiagnosticEvidence(exec::BuildExecutorBatchFailureEvidence(join),
                            false,
                            "SB_EXECUTOR_BATCH_DPC053_JOIN_ROW_ERROR",
                            "row_index=2");

  auto dml_request = UpdateRequest();
  dml_request.row_step =
      [](const exec::Tuple& row,
         std::size_t row_index) -> exec::ExecutorBatchDmlRowStepResult {
    auto result = UpdateRequest().row_step(row, row_index);
    if (row_index == 3) {
      result.ok = false;
      result.emit_returning = false;
      result.diagnostic_code = "SB_EXECUTOR_BATCH_DPC053_DML_ROW_ERROR";
      result.message_vector = {
          "SB_EXECUTOR_BATCH_DPC053_DML_ROW_ERROR",
          "row_index=3",
          "dml_intent_uncommitted",
      };
    }
    return result;
  };
  const auto dml = exec::ExecuteBatchedDmlReturning(DmlCorpus(), dml_request);
  const auto dml_evidence = exec::BuildExecutorBatchFailureEvidence(dml);
  Require(dml.intents.empty(), "DML row-level error must clear intents");
  Require(dml_evidence.intent_count == 0,
          "DML row-level error policy evidence must report no intents");
  RequireDiagnosticEvidence(dml_evidence,
                            false,
                            "SB_EXECUTOR_BATCH_DPC053_DML_ROW_ERROR",
                            "row_index=3");
}

void PrimitivePolicyEvidenceIsReusable() {
  exec::ExecutorBatchFailurePolicy backpressure_policy;
  backpressure_policy.backpressure = true;
  const auto request =
      exec::ApplyExecutorBatchFailurePolicy(BaseBatchRequest(),
                                            backpressure_policy);
  const auto result = exec::ExecuteScopedExecutorBatch(
      RelationalCorpus(),
      request,
      [](const exec::Tuple& row, std::size_t) {
    exec::ExecutorRowStepResult step;
    step.row = row;
    return step;
  });
  RequireFallbackEvidence(exec::BuildExecutorBatchFailureEvidence(result),
                          exec::ExecutorBatchFallbackReason::kBackpressure,
                          "SB_EXECUTOR_BATCH_FALLBACK_BACKPRESSURE");
}

}  // namespace

int main() {
  Require(!kGateSearchKey.empty(), "missing gate search key");
  Require(!kImplementationSearchKey.empty(), "missing implementation search key");
  DisabledFallbackMatchesBaseline();
  SuccessfulFallbackReasonsMatchBaseline();
  CancellationProducesExactEvidence();
  RowLevelErrorProducesExactEvidence();
  PrimitivePolicyEvidenceIsReusable();
  std::cout << "dpc_executor_batch_failure_fallback_gate=passed\n";
  return EXIT_SUCCESS;
}
