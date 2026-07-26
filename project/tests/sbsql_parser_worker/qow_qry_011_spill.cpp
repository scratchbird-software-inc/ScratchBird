// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

constexpr std::string_view kOwnerUuid =
    "019f0000-0000-7200-8000-000000001801";

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-011-SPILL-V1: " << detail << '\n';
  }
  return condition;
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 const std::string_view expected) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item == expected;
  });
}

api::EngineDescriptor Descriptor(const std::string& descriptor_uuid,
                                 const std::string& type_uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=nullable";
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            const std::string& encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = encoded;
  value.state = api::EngineValueState::value;
  return value;
}

api::EngineTypedValue Null(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.is_null = true;
  value.state = api::EngineValueState::sql_null;
  return value;
}

exec::CanonicalInt64SumSpillRequest Request(
    const std::filesystem::path& root) {
  const auto key_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001802",
      "019f0000-0000-7300-8000-000000001803");
  const auto value_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001804",
      "019f0000-0000-7300-8000-000000001805");
  const auto key_result_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001806",
      "019f0000-0000-7300-8000-000000001807");
  const auto sum_result_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001808",
      "019f0000-0000-7300-8000-000000001809");

  exec::CanonicalInt64SumSpillRequest request;
  auto& aggregate = request.aggregate_request;
  aggregate.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001810";
  aggregate.physical_dag.root_physical_node_id = 1802;
  aggregate.physical_dag.local_transaction_id = 1803;
  aggregate.physical_dag.statement_snapshot_id = 1804;
  aggregate.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001811"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001812"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001813"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001814"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001815"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001816"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001817"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001818"},
  };
  aggregate.physical_dag.nodes = {
      {.physical_node_id = 1801,
       .relational_node_id = 1801,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {1801, 1802},
       .causal_counter_id = 18001},
      {.physical_node_id = 1802,
       .relational_node_id = 1802,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.sum-int64-spill.v1",
       .input_physical_node_ids = {1801},
       .output_descriptor_ids = {1803, 1804},
       .causal_counter_id = 18002},
  };
  aggregate.selected_physical_node_id = 1802;
  aggregate.input_batch = exec::MakeDescriptorBatch(
      {{"group_key", key_descriptor, true, 1801},
       {"amount", value_descriptor, true, 1802}},
      {{{Value(key_descriptor, "1"), Value(value_descriptor, "10")}},
       {{Value(key_descriptor, "2"), Value(value_descriptor, "5")}},
       {{Value(key_descriptor, "01"), Null(value_descriptor)}},
       {{Null(key_descriptor), Value(value_descriptor, "7")}},
       {{Value(key_descriptor, "2"), Value(value_descriptor, "-2")}}});
  aggregate.key_column = 0;
  aggregate.key_expression_descriptor_id = 1801;
  aggregate.value_column = 1;
  aggregate.value_expression_descriptor_id = 1802;
  aggregate.key_result_column =
      {"group_key", key_result_descriptor, true, 1803};
  aggregate.sum_result_column =
      {"sum_amount", sum_result_descriptor, true, 1804};
  aggregate.grouping_set_rule =
      exec::CanonicalInt64GroupingSetRule::key_only;
  request.spill_root = root;
  request.spill_owner_uuid = kOwnerUuid;
  request.runtime_generation = 1805;
  request.memory_quota_bytes = 128;
  return request;
}

bool HasOwnedArtifact(const std::filesystem::path& root) {
  const auto directory = root / kOwnerUuid;
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
  return error ? true : false;
}

// QOW-TEST-QRY-011-SPILL-V1
bool ValidateAggregateSpill(const std::filesystem::path& root) {
  bool passed = true;
  const auto owner_directory = root / kOwnerUuid;
  std::error_code error;
  std::filesystem::create_directories(owner_directory, error);
  const auto sentinel = owner_directory / "unrelated.sentinel";
  {
    std::ofstream output(sentinel);
    output << "preserve";
  }

  auto result = exec::ExecuteCanonicalInt64SumSpill(Request(root));
  passed &= Require(result.diagnostic.ok && result.spilled &&
                        result.spill_reopened && result.cleanup_proven &&
                        result.groups.size() == 3 &&
                        result.groups[0].group_key.encoded_value == "1" &&
                        result.groups[0].sum_state.accumulated_value == 10 &&
                        result.groups[0].sum_state.transition_count == 2 &&
                        result.groups[0].sum_state.non_null_count == 1 &&
                        result.groups[1].sum_state.accumulated_value == 3 &&
                        result.groups[2].group_key.state ==
                            api::EngineValueState::sql_null &&
                        result.groups[2].sum_state.accumulated_value == 7 &&
                        HasEvidence(
                            result.spill_evidence,
                            "temporary_work.spill_payload_checksum=validated") &&
                        HasEvidence(
                            result.spill_evidence,
                            "orh283.temp_metadata.finality_authority=false") &&
                        HasEvidence(
                            result.spill_evidence,
                            "orh283.mga_finality_authority=engine_transaction_inventory") &&
                        HasEvidence(
                            result.spill_evidence,
                            "orh283.temp_live_granted_bytes_after_cleanup=0"),
                    "spilled group states did not exactly merge");
  passed &= Require(!HasOwnedArtifact(root) &&
                        std::filesystem::exists(sentinel),
                    "success cleanup removed unrelated data or left artifact");

  auto request = Request(root);
  request.cancellation_requested = true;
  result = exec::ExecuteCanonicalInt64SumSpill(request);
  passed &= Require(!result.diagnostic.ok && result.cancellation_observed &&
                        result.cleanup_proven && result.groups.empty() &&
                        !HasOwnedArtifact(root),
                    "cancellation did not clean the owned spill artifact");

  request = Request(root);
  request.reopen_runtime_generation = 1806;
  result = exec::ExecuteCanonicalInt64SumSpill(request);
  passed &= Require(!result.diagnostic.ok && result.cleanup_proven &&
                        result.groups.empty() && !HasOwnedArtifact(root),
                    "stale-generation refusal left an owned artifact");

  request = Request(root);
  request.restart_recovery_proof_available = false;
  result = exec::ExecuteCanonicalInt64SumSpill(request);
  passed &= Require(!result.diagnostic.ok && result.cleanup_proven &&
                        !HasOwnedArtifact(root),
                    "missing reopen proof left an owned artifact");

  request = Request(root);
  request.memory_quota_bytes = 1048576;
  result = exec::ExecuteCanonicalInt64SumSpill(request);
  passed &= Require(!result.diagnostic.ok && result.cleanup_proven &&
                        !HasOwnedArtifact(root),
                    "non-spilled refusal left temporary state");

  request = Request(root);
  request.maximum_spill_record_count = 2;
  result = exec::ExecuteCanonicalInt64SumSpill(request);
  passed &= Require(!result.diagnostic.ok && !HasOwnedArtifact(root),
                    "spill record bound was exceeded");

  request = Request(root);
  request.aggregate_request.input_batch.rows[0].values[1].encoded_value =
      "malformed";
  result = exec::ExecuteCanonicalInt64SumSpill(request);
  passed &= Require(!result.diagnostic.ok && !HasOwnedArtifact(root),
                    "malformed typed input reached spill storage");

  request = Request(root);
  request.aggregate_request.grouping_set_rule =
      exec::CanonicalInt64GroupingSetRule::key_and_grand_total;
  result = exec::ExecuteCanonicalInt64SumSpill(request);
  passed &= Require(!result.diagnostic.ok && !HasOwnedArtifact(root),
                    "unadmitted grouping-set spill profile was accepted");

  request = Request(root);
  request.aggregate_request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalInt64SumSpill(request);
  passed &= Require(!result.diagnostic.ok && !HasOwnedArtifact(root),
                    "aggregate spill bypassed MGA physical admission");

  const auto preexisting =
      owner_directory / "orh283_temp_spill-preexisting.sbtmpidx";
  {
    std::ofstream output(preexisting);
    output << "foreign";
  }
  result = exec::ExecuteCanonicalInt64SumSpill(Request(root));
  passed &= Require(!result.diagnostic.ok &&
                        std::filesystem::exists(preexisting),
                    "owner collision overwrote a preexisting artifact");
  std::filesystem::remove(preexisting, error);
  return passed;
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("scratchbird_qow205_aggregate_spill_" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
  std::error_code error;
  std::filesystem::remove_all(root, error);
  const bool passed = ValidateAggregateSpill(root);
  std::filesystem::remove_all(root, error);
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
