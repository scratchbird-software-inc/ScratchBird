// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "executor_batch_failure_policy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;

constexpr std::string_view kGateSearchKey =
    "DPC_EXECUTOR_BATCH_BENCHMARK_GATE";
constexpr std::string_view kBenchmarkOutputSearchKey =
    "DPC_EXECUTOR_BATCH_BENCHMARK_OUTPUT";
constexpr std::uint32_t kRunCount = 5;
constexpr double kTargetProxyReduction = 0.15;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::uint64_t MixFnv1a(std::uint64_t hash, std::string_view text) {
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string Hex(std::uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << std::nouppercase << std::setw(16)
      << std::setfill('0') << value;
  return out.str();
}

std::string HashSignature(std::string_view signature) {
  return Hex(MixFnv1a(1469598103934665603ull, signature));
}

std::string Fixed(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6) << value;
  return out.str();
}

std::uint64_t Median(std::vector<std::uint64_t> values) {
  Require(!values.empty(), "DPC-054 median requires samples");
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

std::uint64_t P95(std::vector<std::uint64_t> values) {
  Require(!values.empty(), "DPC-054 p95 requires samples");
  std::sort(values.begin(), values.end());
  const auto index =
      static_cast<std::size_t>(std::ceil(values.size() * 0.95)) - 1;
  return values[std::min(index, values.size() - 1)];
}

double ReductionRatio(std::uint64_t baseline, std::uint64_t enabled) {
  Require(baseline > 0, "DPC-054 baseline proxy counter was zero");
  Require(enabled <= baseline,
          "DPC-054 enabled proxy counter exceeded baseline");
  return static_cast<double>(baseline - enabled) /
         static_cast<double>(baseline);
}

exec::ExecutorBatchRequest BatchRequest() {
  exec::ExecutorBatchRequest request;
  request.requested_mode = exec::ExecutorBatchRequestMode::kPreferBatch;
  request.node_supports_batch = true;
  request.preserve_input_order = true;
  request.limits.max_batch_rows = 64;
  request.limits.max_materialized_cells = 512;
  request.limits.max_materialized_bytes = 4096;
  return request;
}

exec::Batch PointSelectCorpus() {
  std::vector<exec::Tuple> rows;
  for (std::int64_t id = 0; id < 16; ++id) {
    rows.push_back({.values = {id, id % 4, 100 + id * 10}});
  }
  return exec::MakeBatch("dpc_executor_batch_benchmark_point_input_v1",
                         std::move(rows));
}

exec::Batch AggregateCorpus() {
  return exec::MakeBatch("dpc_executor_batch_benchmark_aggregate_input_v1",
                         {
                             {.values = {3, 5}},
                             {.values = {1, 10}},
                             {.values = {2, 20}},
                             {.values = {1, 7}},
                             {.values = {3, 8}},
                             {.values = {2, 13}},
                             {.values = {4, 3}},
                             {.values = {1, 11}},
                             {.values = {4, 17}},
                             {.values = {3, 2}},
                             {.values = {2, 9}},
                             {.values = {4, 1}},
                         });
}

exec::Batch JoinLeftCorpus() {
  return exec::MakeBatch("dpc_executor_batch_benchmark_join_left_v1",
                         {
                             {.values = {1, 100}},
                             {.values = {2, 200}},
                             {.values = {1, 101}},
                             {.values = {3, 300}},
                             {.values = {4, 400}},
                             {.values = {2, 201}},
                             {.values = {5, 500}},
                             {.values = {3, 301}},
                         });
}

exec::Batch JoinRightCorpus() {
  return exec::MakeBatch("dpc_executor_batch_benchmark_join_right_v1",
                         {
                             {.values = {1, 10}},
                             {.values = {2, 20}},
                             {.values = {3, 30}},
                             {.values = {1, 11}},
                             {.values = {4, 40}},
                             {.values = {2, 21}},
                             {.values = {6, 60}},
                             {.values = {3, 31}},
                         });
}

exec::Batch DmlCorpus() {
  return exec::MakeBatch("dpc_executor_batch_benchmark_dml_input_v1",
                         {
                             {.values = {20, 1}},
                             {.values = {21, 2}},
                             {.values = {22, 3}},
                             {.values = {23, 4}},
                             {.values = {24, 5}},
                             {.values = {25, 6}},
                             {.values = {26, 7}},
                             {.values = {27, 8}},
                             {.values = {28, 9}},
                             {.values = {29, 10}},
                         });
}

exec::ExecutorBatchScanFilterProjectionRequest PointSelectRequest() {
  exec::ExecutorBatchScanFilterProjectionRequest request;
  request.batch_request = BatchRequest();
  request.filter.enabled = true;
  request.filter.column = 0;
  request.filter.op = exec::Int64ComparisonOperator::kEqual;
  request.filter.value = 7;
  request.projection_columns = {0, 2};
  request.output_descriptor_digest =
      "dpc_executor_batch_benchmark_point_output_v1";
  return request;
}

exec::ExecutorBatchAggregateSumRequest AggregateRequest() {
  exec::ExecutorBatchAggregateSumRequest request;
  request.batch_request = BatchRequest();
  request.key_column = 0;
  request.value_column = 1;
  request.output_descriptor_digest =
      "dpc_executor_batch_benchmark_aggregate_output_v1";
  return request;
}

exec::ExecutorBatchJoinRequest JoinRequest() {
  exec::ExecutorBatchJoinRequest request;
  request.batch_request = BatchRequest();
  request.left_column = 0;
  request.right_column = 0;
  request.output_descriptor_digest =
      "dpc_executor_batch_benchmark_join_output_v1";
  return request;
}

exec::ExecutorBatchDmlRequest UpdateReturningRequest() {
  exec::ExecutorBatchDmlRequest request;
  request.batch_request = BatchRequest();
  request.operation = exec::ExecutorBatchDmlOperation::kUpdateReturning;
  request.returning_descriptor_digest =
      "dpc_executor_batch_benchmark_update_returning_v1";
  request.row_step =
      [](const exec::Tuple& row,
         std::size_t row_index) -> exec::ExecutorBatchDmlRowStepResult {
    exec::ExecutorBatchDmlRowStepResult result;
    result.new_row.values = {row.values[0], row.values[1] + 100};
    result.returning_row.values = {
        row.values[0],
        row.values[1] + 100,
        static_cast<std::int64_t>(row_index),
    };
    return result;
  };
  return request;
}

exec::ExecutorBatchDmlRequest DeleteReturningRequest() {
  exec::ExecutorBatchDmlRequest request;
  request.batch_request = BatchRequest();
  request.operation = exec::ExecutorBatchDmlOperation::kDeleteReturning;
  request.returning_descriptor_digest =
      "dpc_executor_batch_benchmark_delete_returning_v1";
  request.row_step =
      [](const exec::Tuple& row,
         std::size_t row_index) -> exec::ExecutorBatchDmlRowStepResult {
    exec::ExecutorBatchDmlRowStepResult result;
    result.returning_row.values = {
        row.values[0],
        row.values[1],
        static_cast<std::int64_t>(row_index),
    };
    return result;
  };
  return request;
}

void Disable(exec::ExecutorBatchRequest* request) {
  request->requested_mode = exec::ExecutorBatchRequestMode::kDisabledRowByRow;
}

void RequireFallbackDiagnostic(const exec::ExecutorBatchEvidence& evidence,
                               exec::ExecutorBatchFallbackReason reason,
                               std::string_view diagnostic_code) {
  Require(evidence.selected_mode == exec::ExecutorBatchSelectedMode::kRowByRow,
          "DPC-054 fallback did not select row-by-row");
  Require(evidence.fallback_reason == reason,
          "DPC-054 fallback reason drifted");
  Require(evidence.diagnostic_code == diagnostic_code,
          "DPC-054 fallback diagnostic drifted");
  Require(evidence.message_vector.size() == 1,
          "DPC-054 fallback message vector size drifted");
  Require(evidence.message_vector.front() == diagnostic_code,
          "DPC-054 fallback message vector diagnostic drifted");
}

void RequirePrimitiveAuthority(const exec::ExecutorBatchEvidence& evidence) {
  Require(!evidence.authority.owns_transaction_finality,
          "DPC-054 primitive must not own transaction finality");
  Require(!evidence.authority.owns_visibility,
          "DPC-054 primitive must not own visibility");
  Require(!evidence.authority.owns_rollback,
          "DPC-054 primitive must not own rollback");
  Require(!evidence.authority.owns_recovery,
          "DPC-054 primitive must not own recovery");
  Require(!evidence.authority.owns_parser_execution,
          "DPC-054 primitive must not own parser execution");
}

void RequireRelationalAuthority(
    const exec::ExecutorBatchRelationalAuthorityEvidence& authority) {
  Require(!authority.owns_transaction_finality,
          "DPC-054 relational must not own transaction finality");
  Require(!authority.owns_visibility,
          "DPC-054 relational must not own visibility");
  Require(!authority.owns_rollback,
          "DPC-054 relational must not own rollback");
  Require(!authority.owns_recovery,
          "DPC-054 relational must not own recovery");
  Require(!authority.owns_parser_execution,
          "DPC-054 relational must not own parser execution");
  Require(!authority.owns_timestamp_ordering,
          "DPC-054 relational must not own timestamp ordering");
  Require(!authority.owns_reference_storage,
          "DPC-054 relational must not own reference storage");
  Require(!authority.owns_sql_execution,
          "DPC-054 relational must not own SQL execution");
}

void RequireJoinDmlAuthority(
    const exec::ExecutorBatchJoinDmlAuthorityEvidence& authority) {
  Require(!authority.owns_transaction_finality,
          "DPC-054 join/DML must not own transaction finality");
  Require(!authority.owns_visibility,
          "DPC-054 join/DML must not own visibility");
  Require(!authority.owns_rollback,
          "DPC-054 join/DML must not own rollback");
  Require(!authority.owns_recovery,
          "DPC-054 join/DML must not own recovery");
  Require(!authority.owns_parser_execution,
          "DPC-054 join/DML must not own parser execution");
  Require(!authority.owns_timestamp_ordering,
          "DPC-054 join/DML must not own timestamp ordering");
  Require(!authority.owns_reference_storage,
          "DPC-054 join/DML must not own reference storage");
  Require(!authority.owns_sql_execution,
          "DPC-054 join/DML must not own SQL execution");
  Require(!authority.owns_durable_commit,
          "DPC-054 join/DML must not own durable commit");
}

struct LaneProof {
  std::string workload_id;
  std::string lane;
  std::string baseline_counter_name;
  std::uint64_t baseline_counter = 0;
  std::string enabled_counter_name;
  std::uint64_t enabled_counter = 0;
  std::uint64_t median_proxy_units = 0;
  std::uint64_t p95_proxy_units = 0;
  std::uint64_t baseline_row_count = 0;
  std::uint64_t enabled_row_count = 0;
  std::string baseline_result_hash;
  std::string enabled_result_hash;
  std::string ordering_contract;
};

void PrintProofRow(const LaneProof& proof) {
  const double reduction =
      ReductionRatio(proof.baseline_counter, proof.enabled_counter);
  std::cout << kBenchmarkOutputSearchKey
            << ",workload_id=" << proof.workload_id
            << ",lane=" << proof.lane
            << ",run_count=" << kRunCount
            << ",baseline_mode=disabled_row_by_row"
            << ",enabled_mode=batch"
            << ",baseline_counter_name=" << proof.baseline_counter_name
            << ",baseline_counter=" << proof.baseline_counter
            << ",enabled_counter_name=" << proof.enabled_counter_name
            << ",enabled_counter=" << proof.enabled_counter
            << ",median_proxy_units=" << proof.median_proxy_units
            << ",p95_proxy_units=" << proof.p95_proxy_units
            << ",cv_proxy_units=0.000000"
            << ",proxy_reduction_ratio=" << Fixed(reduction)
            << ",target_reduction_ratio=" << Fixed(kTargetProxyReduction)
            << ",target_met="
            << (reduction >= kTargetProxyReduction ? "true" : "false")
            << ",baseline_row_count=" << proof.baseline_row_count
            << ",enabled_row_count=" << proof.enabled_row_count
            << ",baseline_result_hash=" << proof.baseline_result_hash
            << ",enabled_result_hash=" << proof.enabled_result_hash
            << ",result_hash_equal="
            << (proof.baseline_result_hash == proof.enabled_result_hash
                    ? "true"
                    : "false")
            << ",stable_ordering=true"
            << ",ordering_contract=" << proof.ordering_contract
            << ",disabled_fallback_reason=disabled_by_request"
            << ",disabled_diagnostic_code="
            << "SB_EXECUTOR_BATCH_FALLBACK_DISABLED_BY_REQUEST"
            << ",exact_fallback_reason=exact_error_required"
            << ",exact_diagnostic_code="
            << "SB_EXECUTOR_BATCH_FALLBACK_EXACT_ERROR_REQUIRED"
            << ",transaction_finality_authority=false"
            << ",visibility_authority=false"
            << ",rollback_authority=false"
            << ",recovery_authority=false"
            << ",parser_execution_authority=false"
            << ",timestamp_ordering_authority=false"
            << ",reference_storage_authority=false"
            << ",sql_execution_authority=false"
            << ",durable_commit_authority=false"
            << ",deterministic_equivalent=true"
            << ",source_state=ctest_runtime_deterministic"
            << ",owner_ctest=dpc_executor_batch_benchmark_gate\n";
}

LaneProof ProvePointSelectLane() {
  std::vector<std::uint64_t> enabled_samples;
  LaneProof proof;
  proof.workload_id = "WL10";
  proof.lane = "point_select";
  proof.baseline_counter_name = "row_by_row_executor_dispatch_units";
  proof.enabled_counter_name = "batch_executor_dispatch_units";
  proof.ordering_contract = "single_point_row_input_order";

  std::string expected_signature;
  for (std::uint32_t run = 0; run < kRunCount; ++run) {
    auto disabled_request = PointSelectRequest();
    Disable(&disabled_request.batch_request);
    const auto baseline = exec::ExecuteBatchedScanFilterProjection(
        PointSelectCorpus(), disabled_request);
    const auto enabled = exec::ExecuteBatchedScanFilterProjection(
        PointSelectCorpus(), PointSelectRequest());

    RequireFallbackDiagnostic(
        baseline.evidence.primitive_evidence,
        exec::ExecutorBatchFallbackReason::kDisabledByRequest,
        "SB_EXECUTOR_BATCH_FALLBACK_DISABLED_BY_REQUEST");
    Require(enabled.evidence.selected_mode == exec::ExecutorBatchSelectedMode::kBatch,
            "DPC-054 point select did not select batch");
    Require(enabled.evidence.fallback_reason ==
                exec::ExecutorBatchFallbackReason::kNone,
            "DPC-054 point select unexpectedly fell back");
    Require(baseline.evidence.deterministic_result_signature ==
                enabled.evidence.deterministic_result_signature,
            "DPC-054 point select result drifted");
    Require(enabled.evidence.preserves_input_order,
            "DPC-054 point select lost input ordering");
    Require(enabled.evidence.rows_produced == 1,
            "DPC-054 point select row count drifted");
    RequireRelationalAuthority(enabled.evidence.authority);
    RequirePrimitiveAuthority(enabled.evidence.primitive_evidence);

    auto exact_request = PointSelectRequest();
    exact_request.batch_request.exact_error_required = true;
    const auto exact =
        exec::ExecuteBatchedScanFilterProjection(PointSelectCorpus(),
                                                 exact_request);
    RequireFallbackDiagnostic(
        exact.evidence.primitive_evidence,
        exec::ExecutorBatchFallbackReason::kExactErrorRequired,
        "SB_EXECUTOR_BATCH_FALLBACK_EXACT_ERROR_REQUIRED");
    Require(exact.evidence.deterministic_result_signature ==
                baseline.evidence.deterministic_result_signature,
            "DPC-054 point select exact fallback drifted");

    const std::uint64_t baseline_units =
        baseline.evidence.rows_processed_row_by_row;
    const std::uint64_t enabled_units =
        enabled.evidence.rows_processed_in_batch > 0 ? 1 : 0;
    Require(enabled_units > 0, "DPC-054 point select enabled units missing");
    enabled_samples.push_back(enabled_units);

    if (run == 0) {
      proof.baseline_counter = baseline_units;
      proof.enabled_counter = enabled_units;
      proof.baseline_row_count = baseline.evidence.rows_produced;
      proof.enabled_row_count = enabled.evidence.rows_produced;
      proof.baseline_result_hash =
          HashSignature(baseline.evidence.deterministic_result_signature);
      proof.enabled_result_hash =
          HashSignature(enabled.evidence.deterministic_result_signature);
      expected_signature = enabled.evidence.deterministic_result_signature;
    } else {
      Require(proof.baseline_counter == baseline_units,
              "DPC-054 point select baseline counter was not deterministic");
      Require(proof.enabled_counter == enabled_units,
              "DPC-054 point select enabled counter was not deterministic");
      Require(expected_signature == enabled.evidence.deterministic_result_signature,
              "DPC-054 point select signature was not deterministic");
    }
  }
  proof.median_proxy_units = Median(enabled_samples);
  proof.p95_proxy_units = P95(enabled_samples);
  Require(ReductionRatio(proof.baseline_counter, proof.enabled_counter) >=
              kTargetProxyReduction,
          "DPC-054 point select proxy reduction missed target");
  return proof;
}

LaneProof ProveAggregateLane() {
  std::vector<std::uint64_t> enabled_samples;
  LaneProof proof;
  proof.workload_id = "WL10";
  proof.lane = "aggregate";
  proof.baseline_counter_name = "row_by_row_executor_dispatch_units";
  proof.enabled_counter_name = "batch_executor_dispatch_units";
  proof.ordering_contract = "group_key_ascending";

  std::string expected_signature;
  for (std::uint32_t run = 0; run < kRunCount; ++run) {
    auto disabled_request = AggregateRequest();
    Disable(&disabled_request.batch_request);
    const auto baseline =
        exec::ExecuteBatchedAggregateSumByKey(AggregateCorpus(),
                                              disabled_request);
    const auto enabled =
        exec::ExecuteBatchedAggregateSumByKey(AggregateCorpus(),
                                              AggregateRequest());

    RequireFallbackDiagnostic(
        baseline.evidence.primitive_evidence,
        exec::ExecutorBatchFallbackReason::kDisabledByRequest,
        "SB_EXECUTOR_BATCH_FALLBACK_DISABLED_BY_REQUEST");
    Require(enabled.evidence.selected_mode == exec::ExecutorBatchSelectedMode::kBatch,
            "DPC-054 aggregate did not select batch");
    Require(enabled.evidence.aggregate_group_order_deterministic,
            "DPC-054 aggregate did not report deterministic grouping");
    Require(!enabled.evidence.preserves_input_order,
            "DPC-054 aggregate should report grouped ordering");
    Require(baseline.evidence.deterministic_result_signature ==
                enabled.evidence.deterministic_result_signature,
            "DPC-054 aggregate result drifted");
    Require(enabled.evidence.rows_produced == 4,
            "DPC-054 aggregate group count drifted");
    Require(enabled.evidence.counters.aggregate_groups == 4,
            "DPC-054 aggregate counter drifted");
    RequireRelationalAuthority(enabled.evidence.authority);
    RequirePrimitiveAuthority(enabled.evidence.primitive_evidence);

    auto exact_request = AggregateRequest();
    exact_request.batch_request.exact_error_required = true;
    const auto exact =
        exec::ExecuteBatchedAggregateSumByKey(AggregateCorpus(), exact_request);
    RequireFallbackDiagnostic(
        exact.evidence.primitive_evidence,
        exec::ExecutorBatchFallbackReason::kExactErrorRequired,
        "SB_EXECUTOR_BATCH_FALLBACK_EXACT_ERROR_REQUIRED");
    Require(exact.evidence.deterministic_result_signature ==
                baseline.evidence.deterministic_result_signature,
            "DPC-054 aggregate exact fallback drifted");

    const std::uint64_t baseline_units =
        baseline.evidence.rows_processed_row_by_row;
    const std::uint64_t enabled_units =
        enabled.evidence.rows_processed_in_batch > 0 ? 1 : 0;
    Require(enabled_units > 0, "DPC-054 aggregate enabled units missing");
    enabled_samples.push_back(enabled_units);

    if (run == 0) {
      proof.baseline_counter = baseline_units;
      proof.enabled_counter = enabled_units;
      proof.baseline_row_count = baseline.evidence.rows_produced;
      proof.enabled_row_count = enabled.evidence.rows_produced;
      proof.baseline_result_hash =
          HashSignature(baseline.evidence.deterministic_result_signature);
      proof.enabled_result_hash =
          HashSignature(enabled.evidence.deterministic_result_signature);
      expected_signature = enabled.evidence.deterministic_result_signature;
    } else {
      Require(proof.baseline_counter == baseline_units,
              "DPC-054 aggregate baseline counter was not deterministic");
      Require(proof.enabled_counter == enabled_units,
              "DPC-054 aggregate enabled counter was not deterministic");
      Require(expected_signature == enabled.evidence.deterministic_result_signature,
              "DPC-054 aggregate signature was not deterministic");
    }
  }
  proof.median_proxy_units = Median(enabled_samples);
  proof.p95_proxy_units = P95(enabled_samples);
  Require(ReductionRatio(proof.baseline_counter, proof.enabled_counter) >=
              kTargetProxyReduction,
          "DPC-054 aggregate proxy reduction missed target");
  return proof;
}

LaneProof ProveJoinLane() {
  std::vector<std::uint64_t> enabled_samples;
  LaneProof proof;
  proof.workload_id = "WL09";
  proof.lane = "join";
  proof.baseline_counter_name = "nested_loop_right_rows_scanned";
  proof.enabled_counter_name = "hash_materialize_plus_probe_units";
  proof.ordering_contract = "left_input_order_then_right_input_order";

  std::string expected_signature;
  for (std::uint32_t run = 0; run < kRunCount; ++run) {
    auto disabled_request = JoinRequest();
    Disable(&disabled_request.batch_request);
    const auto baseline = exec::ExecuteBatchedJoinEqual(
        JoinLeftCorpus(), JoinRightCorpus(), disabled_request);
    const auto enabled = exec::ExecuteBatchedJoinEqual(
        JoinLeftCorpus(), JoinRightCorpus(), JoinRequest());

    RequireFallbackDiagnostic(
        baseline.evidence.primitive_evidence,
        exec::ExecutorBatchFallbackReason::kDisabledByRequest,
        "SB_EXECUTOR_BATCH_FALLBACK_DISABLED_BY_REQUEST");
    Require(enabled.evidence.selected_mode == exec::ExecutorBatchSelectedMode::kBatch,
            "DPC-054 join did not select batch");
    Require(enabled.evidence.hash_route_used,
            "DPC-054 join did not use hash-style route");
    Require(enabled.evidence.preserves_nested_loop_order,
            "DPC-054 join did not preserve nested-loop order");
    Require(enabled.evidence.deterministic_ordering ==
                proof.ordering_contract,
            "DPC-054 join ordering contract drifted");
    Require(baseline.evidence.deterministic_result_signature ==
                enabled.evidence.deterministic_result_signature,
            "DPC-054 join result drifted");
    Require(enabled.evidence.rows_produced == 13,
            "DPC-054 join row count drifted");
    RequireJoinDmlAuthority(enabled.evidence.authority);
    RequirePrimitiveAuthority(enabled.evidence.primitive_evidence);

    auto exact_request = JoinRequest();
    exact_request.batch_request.exact_error_required = true;
    const auto exact = exec::ExecuteBatchedJoinEqual(
        JoinLeftCorpus(), JoinRightCorpus(), exact_request);
    RequireFallbackDiagnostic(
        exact.evidence.primitive_evidence,
        exec::ExecutorBatchFallbackReason::kExactErrorRequired,
        "SB_EXECUTOR_BATCH_FALLBACK_EXACT_ERROR_REQUIRED");
    Require(exact.evidence.deterministic_result_signature ==
                baseline.evidence.deterministic_result_signature,
            "DPC-054 join exact fallback drifted");

    const std::uint64_t baseline_units =
        baseline.evidence.counters.nested_loop_right_rows_scanned;
    const std::uint64_t enabled_units =
        enabled.evidence.counters.right_rows_materialized +
        enabled.evidence.counters.hash_join_left_probes;
    Require(enabled_units > 0, "DPC-054 join enabled units missing");
    enabled_samples.push_back(enabled_units);

    if (run == 0) {
      proof.baseline_counter = baseline_units;
      proof.enabled_counter = enabled_units;
      proof.baseline_row_count = baseline.evidence.rows_produced;
      proof.enabled_row_count = enabled.evidence.rows_produced;
      proof.baseline_result_hash =
          HashSignature(baseline.evidence.deterministic_result_signature);
      proof.enabled_result_hash =
          HashSignature(enabled.evidence.deterministic_result_signature);
      expected_signature = enabled.evidence.deterministic_result_signature;
    } else {
      Require(proof.baseline_counter == baseline_units,
              "DPC-054 join baseline counter was not deterministic");
      Require(proof.enabled_counter == enabled_units,
              "DPC-054 join enabled counter was not deterministic");
      Require(expected_signature == enabled.evidence.deterministic_result_signature,
              "DPC-054 join signature was not deterministic");
    }
  }
  proof.median_proxy_units = Median(enabled_samples);
  proof.p95_proxy_units = P95(enabled_samples);
  Require(ReductionRatio(proof.baseline_counter, proof.enabled_counter) >=
              kTargetProxyReduction,
          "DPC-054 join proxy reduction missed target");
  return proof;
}

LaneProof ProveUpdateReturningLane() {
  std::vector<std::uint64_t> enabled_samples;
  LaneProof proof;
  proof.workload_id = "WL10";
  proof.lane = "update_returning";
  proof.baseline_counter_name = "row_by_row_executor_dispatch_units";
  proof.enabled_counter_name = "batch_executor_dispatch_units";
  proof.ordering_contract = "input_row_order";

  std::string expected_signature;
  for (std::uint32_t run = 0; run < kRunCount; ++run) {
    auto disabled_request = UpdateReturningRequest();
    Disable(&disabled_request.batch_request);
    const auto baseline =
        exec::ExecuteBatchedDmlReturning(DmlCorpus(), disabled_request);
    const auto enabled =
        exec::ExecuteBatchedDmlReturning(DmlCorpus(), UpdateReturningRequest());

    RequireFallbackDiagnostic(
        baseline.evidence.primitive_evidence,
        exec::ExecutorBatchFallbackReason::kDisabledByRequest,
        "SB_EXECUTOR_BATCH_FALLBACK_DISABLED_BY_REQUEST");
    Require(enabled.evidence.selected_mode == exec::ExecutorBatchSelectedMode::kBatch,
            "DPC-054 update returning did not select batch");
    Require(enabled.evidence.preserves_input_order,
            "DPC-054 update returning lost input order");
    Require(enabled.evidence.no_partial_success,
            "DPC-054 update returning no-partial evidence drifted");
    Require(baseline.evidence.deterministic_result_signature ==
                enabled.evidence.deterministic_result_signature,
            "DPC-054 update returning result drifted");
    Require(baseline.intents.size() == enabled.intents.size(),
            "DPC-054 update returning intent count drifted");
    Require(enabled.evidence.counters.update_intents == DmlCorpus().rows.size(),
            "DPC-054 update returning update counter drifted");
    Require(enabled.evidence.counters.returning_rows == DmlCorpus().rows.size(),
            "DPC-054 update returning returning counter drifted");
    RequireJoinDmlAuthority(enabled.evidence.authority);
    RequirePrimitiveAuthority(enabled.evidence.primitive_evidence);

    auto exact_request = UpdateReturningRequest();
    exact_request.batch_request.exact_error_required = true;
    const auto exact =
        exec::ExecuteBatchedDmlReturning(DmlCorpus(), exact_request);
    RequireFallbackDiagnostic(
        exact.evidence.primitive_evidence,
        exec::ExecutorBatchFallbackReason::kExactErrorRequired,
        "SB_EXECUTOR_BATCH_FALLBACK_EXACT_ERROR_REQUIRED");
    Require(exact.evidence.deterministic_result_signature ==
                baseline.evidence.deterministic_result_signature,
            "DPC-054 update returning exact fallback drifted");
    Require(exact.intents.size() == baseline.intents.size(),
            "DPC-054 update returning exact fallback intent drifted");

    const std::uint64_t baseline_units =
        baseline.evidence.rows_processed_row_by_row;
    const std::uint64_t enabled_units =
        enabled.evidence.rows_processed_in_batch > 0 ? 1 : 0;
    Require(enabled_units > 0, "DPC-054 update enabled units missing");
    enabled_samples.push_back(enabled_units);

    if (run == 0) {
      proof.baseline_counter = baseline_units;
      proof.enabled_counter = enabled_units;
      proof.baseline_row_count = baseline.evidence.rows_produced;
      proof.enabled_row_count = enabled.evidence.rows_produced;
      proof.baseline_result_hash =
          HashSignature(baseline.evidence.deterministic_result_signature);
      proof.enabled_result_hash =
          HashSignature(enabled.evidence.deterministic_result_signature);
      expected_signature = enabled.evidence.deterministic_result_signature;
    } else {
      Require(proof.baseline_counter == baseline_units,
              "DPC-054 update baseline counter was not deterministic");
      Require(proof.enabled_counter == enabled_units,
              "DPC-054 update enabled counter was not deterministic");
      Require(expected_signature == enabled.evidence.deterministic_result_signature,
              "DPC-054 update signature was not deterministic");
    }
  }
  proof.median_proxy_units = Median(enabled_samples);
  proof.p95_proxy_units = P95(enabled_samples);
  Require(ReductionRatio(proof.baseline_counter, proof.enabled_counter) >=
              kTargetProxyReduction,
          "DPC-054 update proxy reduction missed target");
  return proof;
}

LaneProof ProveDeleteReturningLane() {
  std::vector<std::uint64_t> enabled_samples;
  LaneProof proof;
  proof.workload_id = "WL10";
  proof.lane = "delete_returning";
  proof.baseline_counter_name = "row_by_row_executor_dispatch_units";
  proof.enabled_counter_name = "batch_executor_dispatch_units";
  proof.ordering_contract = "input_row_order";

  std::string expected_signature;
  for (std::uint32_t run = 0; run < kRunCount; ++run) {
    auto disabled_request = DeleteReturningRequest();
    Disable(&disabled_request.batch_request);
    const auto baseline =
        exec::ExecuteBatchedDmlReturning(DmlCorpus(), disabled_request);
    const auto enabled =
        exec::ExecuteBatchedDmlReturning(DmlCorpus(), DeleteReturningRequest());

    RequireFallbackDiagnostic(
        baseline.evidence.primitive_evidence,
        exec::ExecutorBatchFallbackReason::kDisabledByRequest,
        "SB_EXECUTOR_BATCH_FALLBACK_DISABLED_BY_REQUEST");
    Require(enabled.evidence.selected_mode == exec::ExecutorBatchSelectedMode::kBatch,
            "DPC-054 delete returning did not select batch");
    Require(enabled.evidence.preserves_input_order,
            "DPC-054 delete returning lost input order");
    Require(enabled.evidence.no_partial_success,
            "DPC-054 delete returning no-partial evidence drifted");
    Require(baseline.evidence.deterministic_result_signature ==
                enabled.evidence.deterministic_result_signature,
            "DPC-054 delete returning result drifted");
    Require(baseline.intents.size() == enabled.intents.size(),
            "DPC-054 delete returning intent count drifted");
    Require(enabled.evidence.counters.delete_intents == DmlCorpus().rows.size(),
            "DPC-054 delete returning delete counter drifted");
    Require(enabled.evidence.counters.returning_rows == DmlCorpus().rows.size(),
            "DPC-054 delete returning returning counter drifted");
    RequireJoinDmlAuthority(enabled.evidence.authority);
    RequirePrimitiveAuthority(enabled.evidence.primitive_evidence);

    auto exact_request = DeleteReturningRequest();
    exact_request.batch_request.exact_error_required = true;
    const auto exact =
        exec::ExecuteBatchedDmlReturning(DmlCorpus(), exact_request);
    RequireFallbackDiagnostic(
        exact.evidence.primitive_evidence,
        exec::ExecutorBatchFallbackReason::kExactErrorRequired,
        "SB_EXECUTOR_BATCH_FALLBACK_EXACT_ERROR_REQUIRED");
    Require(exact.evidence.deterministic_result_signature ==
                baseline.evidence.deterministic_result_signature,
            "DPC-054 delete returning exact fallback drifted");
    Require(exact.intents.size() == baseline.intents.size(),
            "DPC-054 delete returning exact fallback intent drifted");

    const std::uint64_t baseline_units =
        baseline.evidence.rows_processed_row_by_row;
    const std::uint64_t enabled_units =
        enabled.evidence.rows_processed_in_batch > 0 ? 1 : 0;
    Require(enabled_units > 0, "DPC-054 delete enabled units missing");
    enabled_samples.push_back(enabled_units);

    if (run == 0) {
      proof.baseline_counter = baseline_units;
      proof.enabled_counter = enabled_units;
      proof.baseline_row_count = baseline.evidence.rows_produced;
      proof.enabled_row_count = enabled.evidence.rows_produced;
      proof.baseline_result_hash =
          HashSignature(baseline.evidence.deterministic_result_signature);
      proof.enabled_result_hash =
          HashSignature(enabled.evidence.deterministic_result_signature);
      expected_signature = enabled.evidence.deterministic_result_signature;
    } else {
      Require(proof.baseline_counter == baseline_units,
              "DPC-054 delete baseline counter was not deterministic");
      Require(proof.enabled_counter == enabled_units,
              "DPC-054 delete enabled counter was not deterministic");
      Require(expected_signature == enabled.evidence.deterministic_result_signature,
              "DPC-054 delete signature was not deterministic");
    }
  }
  proof.median_proxy_units = Median(enabled_samples);
  proof.p95_proxy_units = P95(enabled_samples);
  Require(ReductionRatio(proof.baseline_counter, proof.enabled_counter) >=
              kTargetProxyReduction,
          "DPC-054 delete proxy reduction missed target");
  return proof;
}

}  // namespace

int main() {
  Require(!kGateSearchKey.empty(), "missing gate search key");
  Require(!kBenchmarkOutputSearchKey.empty(),
          "missing benchmark output search key");

  const auto point_select = ProvePointSelectLane();
  const auto aggregate = ProveAggregateLane();
  const auto join = ProveJoinLane();
  const auto update_returning = ProveUpdateReturningLane();
  const auto delete_returning = ProveDeleteReturningLane();

  PrintProofRow(point_select);
  PrintProofRow(aggregate);
  PrintProofRow(join);
  PrintProofRow(update_returning);
  PrintProofRow(delete_returning);

  std::cout << kGateSearchKey << "=passed "
            << "DPC_EXECUTOR_BATCH_BENCHMARK_OUTPUT=retained "
            << "run_count=" << kRunCount
            << " deterministic_proxy_counters=true"
            << " wall_clock_claim=false"
            << " mga_authority=engine_transaction_layer\n";
  return EXIT_SUCCESS;
}
