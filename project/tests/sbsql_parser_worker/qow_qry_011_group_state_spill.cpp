// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_QRY_011_GROUP_FIXTURE_ONLY
#include "qow_qry_011_group.cpp"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

constexpr std::string_view kGroupedSpillOwner =
    "019f0000-0000-7200-8000-000000001450";

exec::CanonicalGroupedAggregateSetStateSpillRequest GroupedStateSpillRequest(
    const std::filesystem::path& root) {
  exec::CanonicalGroupedAggregateSetStateSpillRequest request;
  request.grouped_request = GroupedAggregateSetRequest();
  auto& first = request.grouped_request.first_aggregate.aggregate_request;
  first.physical_dag.nodes.back().implementation_id =
      "aggregate.registry-grouping-sets-state-spill.v1";
  first.physical_dag.spill_allowed = true;
  first.forced_strategy =
      exec::CanonicalAggregateExecutionStrategy::partitioned_combine;
  first.distinct = true;
  first.aggregate_order_terms = {
      {.column = 2, .expression_descriptor_id = 1423}};
  request.spill_root = root;
  request.spill_owner_uuid = kGroupedSpillOwner;
  request.runtime_generation = 1451;
  request.memory_quota_bytes = 128;
  return request;
}

bool HasGroupedStateSpillArtifact(const std::filesystem::path& root) {
  const auto directory = root / kGroupedSpillOwner;
  std::error_code error;
  if (!std::filesystem::exists(directory, error)) return false;
  for (std::filesystem::directory_iterator iterator(directory, error), end;
       !error && iterator != end; iterator.increment(error)) {
    const auto filename = iterator->path().filename().string();
    if (filename.rfind("orh283_temp_spill-", 0) == 0 &&
        iterator->path().extension() == ".sbtmpidx") {
      return true;
    }
  }
  return static_cast<bool>(error);
}

bool SameGroupedSetOutput(
    const exec::CanonicalGroupedAggregateSetRuntimeResult& left,
    const exec::CanonicalGroupedAggregateSetRuntimeResult& right) {
  if (!left.diagnostic.ok || !right.diagnostic.ok ||
      left.output_batch.columns.size() != right.output_batch.columns.size() ||
      left.output_batch.rows.size() != right.output_batch.rows.size() ||
      left.groups.size() != right.groups.size()) {
    return false;
  }
  for (std::size_t row = 0; row < left.output_batch.rows.size(); ++row) {
    const auto& lhs = left.output_batch.rows[row].values;
    const auto& rhs = right.output_batch.rows[row].values;
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t column = 0; column < lhs.size(); ++column) {
      if (lhs[column].descriptor.descriptor_uuid.canonical !=
              rhs[column].descriptor.descriptor_uuid.canonical ||
          lhs[column].descriptor.canonical_type_name !=
              rhs[column].descriptor.canonical_type_name ||
          lhs[column].descriptor.encoded_descriptor !=
              rhs[column].descriptor.encoded_descriptor ||
          lhs[column].state != rhs[column].state ||
          lhs[column].is_null != rhs[column].is_null ||
          lhs[column].encoded_value != rhs[column].encoded_value ||
          lhs[column].binary_value != rhs[column].binary_value) {
        return false;
      }
    }
    const auto& left_group = left.groups[row];
    const auto& right_group = right.groups[row];
    if (left_group.grouping_set_ordinal !=
            right_group.grouping_set_ordinal ||
        left_group.grouping_id != right_group.grouping_id ||
        left_group.grouping_indicators !=
            right_group.grouping_indicators ||
        left_group.source_row_indices != right_group.source_row_indices ||
        left_group.aggregate_transition_counts !=
            right_group.aggregate_transition_counts ||
        left_group.aggregate_state_bytes !=
            right_group.aggregate_state_bytes) {
      return false;
    }
  }
  return left.aggregate_count == right.aggregate_count &&
         left.aggregate_transition_count ==
             right.aggregate_transition_count &&
         left.aggregate_distinct_tuple_count ==
             right.aggregate_distinct_tuple_count &&
         left.aggregate_order_comparison_count ==
             right.aggregate_order_comparison_count &&
         left.combined_state_bytes == right.combined_state_bytes &&
         left.selected_plan_uuid == right.selected_plan_uuid &&
         left.executed_physical_node_id == right.executed_physical_node_id &&
         left.causal_counter_id == right.causal_counter_id;
}

bool GroupedStateSpillRefused(
    const exec::CanonicalGroupedAggregateSetStateSpillResult& result) {
  return !result.diagnostic.ok && result.grouped_result.groups.empty() &&
         result.grouped_result.output_batch.rows.empty();
}

bool HasGroupedSpillEvidence(const std::vector<std::string>& evidence,
                             const std::string_view expected) {
  return std::find(evidence.begin(), evidence.end(), expected) !=
         evidence.end();
}

bool ValidateGroupedAggregateStateSpill(
    const std::filesystem::path& root) {
  bool passed = true;
  const auto owner_directory = root / kGroupedSpillOwner;
  std::error_code error;
  std::filesystem::create_directories(owner_directory, error);
  const auto sentinel = owner_directory / "unrelated.sentinel";
  {
    std::ofstream output(sentinel);
    output << "preserve";
  }

  auto request = GroupedStateSpillRequest(root);
  auto baseline_request = request.grouped_request;
  baseline_request.first_aggregate.aggregate_request.physical_dag.nodes.back()
      .implementation_id = "aggregate.registry-grouping-sets.v1";
  const auto baseline =
      exec::ExecuteCanonicalGroupedAggregateSetRuntime(baseline_request);
  auto result =
      exec::ExecuteCanonicalGroupedAggregateSetStateSpill(request);
  passed &= Require(
      baseline.diagnostic.ok && result.diagnostic.ok && result.spilled &&
          result.spill_reopened && result.cleanup_proven &&
          result.grouped_result.aggregate_state_spill_required &&
          result.spilled_aggregate_state_count == 16 &&
          result.serialized_aggregate_state_bytes != 0 &&
          result.spilled_aggregate_state_record_count != 0 &&
          SameGroupedSetOutput(baseline, result.grouped_result) &&
          HasGroupedSpillEvidence(
              result.spill_evidence,
              "temporary_work.spill_payload_checksum=validated") &&
          HasGroupedSpillEvidence(
              result.spill_evidence,
              "orh283.temp_metadata.finality_authority=false") &&
          !HasGroupedStateSpillArtifact(root) &&
          std::filesystem::exists(sentinel),
      "grouped AVG/filtered COUNT states did not spill and restore exactly");
  const auto insufficient_serialized_bytes =
      result.serialized_aggregate_state_bytes > 1
          ? result.serialized_aggregate_state_bytes - 1
          : 1;
  const auto insufficient_spill_records =
      result.spilled_aggregate_state_record_count > 1
          ? result.spilled_aggregate_state_record_count - 1
          : 1;

  const auto direct =
      exec::ExecuteCanonicalGroupedAggregateSetRuntime(request.grouped_request);
  passed &= Require(
      !direct.diagnostic.ok && direct.groups.empty(),
      "selected grouped state spill bypassed the spill runtime");

  request = GroupedStateSpillRequest(root);
  request.grouped_request.first_aggregate.aggregate_request.physical_dag.nodes
      .back()
      .implementation_id = "aggregate.registry-grouping-sets.v1";
  result = exec::ExecuteCanonicalGroupedAggregateSetStateSpill(request);
  passed &= Require(
      GroupedStateSpillRefused(result) &&
          !HasGroupedStateSpillArtifact(root),
      "grouped spill payload overrode the optimizer-selected in-memory plan");

  request = GroupedStateSpillRequest(root);
  request.cancellation_requested = true;
  result = exec::ExecuteCanonicalGroupedAggregateSetStateSpill(request);
  passed &= Require(
      GroupedStateSpillRefused(result) && result.cancellation_observed &&
          result.cleanup_proven && !HasGroupedStateSpillArtifact(root),
      "cancelled grouped state spill published or retained state");

  request = GroupedStateSpillRequest(root);
  request.reopen_runtime_generation = request.runtime_generation + 1;
  result = exec::ExecuteCanonicalGroupedAggregateSetStateSpill(request);
  passed &= Require(
      GroupedStateSpillRefused(result) && result.cleanup_proven &&
          !HasGroupedStateSpillArtifact(root),
      "stale grouped state spill generation survived cleanup");

  request = GroupedStateSpillRequest(root);
  request.restart_recovery_proof_available = false;
  result = exec::ExecuteCanonicalGroupedAggregateSetStateSpill(request);
  passed &= Require(
      GroupedStateSpillRefused(result) && result.cleanup_proven &&
          !HasGroupedStateSpillArtifact(root),
      "grouped state reopened without exact recovery proof");

  request = GroupedStateSpillRequest(root);
  request.memory_quota_bytes = 1048576;
  result = exec::ExecuteCanonicalGroupedAggregateSetStateSpill(request);
  passed &= Require(
      GroupedStateSpillRefused(result) && result.cleanup_proven &&
          !HasGroupedStateSpillArtifact(root),
      "non-spilled grouped state published benchmark output");

  request = GroupedStateSpillRequest(root);
  request.maximum_serialized_state_bytes = insufficient_serialized_bytes;
  result = exec::ExecuteCanonicalGroupedAggregateSetStateSpill(request);
  passed &= Require(
      GroupedStateSpillRefused(result) &&
          !HasGroupedStateSpillArtifact(root),
      "grouped state exceeded its combined serialized byte bound");

  request = GroupedStateSpillRequest(root);
  request.maximum_spill_record_count = insufficient_spill_records;
  result = exec::ExecuteCanonicalGroupedAggregateSetStateSpill(request);
  passed &= Require(
      GroupedStateSpillRefused(result) &&
          !HasGroupedStateSpillArtifact(root),
      "grouped state exceeded its combined spill record bound");

  request = GroupedStateSpillRequest(root);
  request.grouped_request.first_aggregate.aggregate_request.physical_dag
      .spill_allowed = false;
  result = exec::ExecuteCanonicalGroupedAggregateSetStateSpill(request);
  passed &= Require(
      GroupedStateSpillRefused(result) && !result.spilled,
      "grouped state spilled without optimizer permission");

  request = GroupedStateSpillRequest(root);
  request.grouped_request.first_aggregate.aggregate_request
      .transaction_finality_claimed = true;
  result = exec::ExecuteCanonicalGroupedAggregateSetStateSpill(request);
  passed &= Require(
      GroupedStateSpillRefused(result) && !result.spilled,
      "grouped state spill claimed transaction finality");

  const auto collision =
      owner_directory / "orh283_temp_spill-preexisting.sbtmpidx";
  {
    std::ofstream output(collision);
    output << "foreign";
  }
  result = exec::ExecuteCanonicalGroupedAggregateSetStateSpill(
      GroupedStateSpillRequest(root));
  passed &= Require(
      GroupedStateSpillRefused(result) &&
          std::filesystem::exists(collision),
      "grouped state spill overwrote a preexisting owner artifact");
  std::filesystem::remove(collision, error);
  return passed;
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("scratchbird_qow213_grouped_state_spill_" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
  std::error_code error;
  std::filesystem::remove_all(root, error);
  const bool passed = ValidateGroupedAggregateStateSpill(root);
  std::filesystem::remove_all(root, error);
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
