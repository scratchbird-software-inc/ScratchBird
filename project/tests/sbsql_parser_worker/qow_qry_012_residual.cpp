// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-012-RESIDUAL-V1: " << detail << '\n';
  }
  return condition;
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

exec::CanonicalJoinResidualRequest Request() {
  const auto left_key = Descriptor(
      "019f0000-0000-7200-8000-000000002101",
      "019f0000-0000-7300-8000-000000002102");
  const auto left_payload = Descriptor(
      "019f0000-0000-7200-8000-000000002103",
      "019f0000-0000-7300-8000-000000002104");
  const auto right_key = Descriptor(
      "019f0000-0000-7200-8000-000000002105",
      "019f0000-0000-7300-8000-000000002106");
  const auto right_payload = Descriptor(
      "019f0000-0000-7200-8000-000000002107",
      "019f0000-0000-7300-8000-000000002108");

  exec::CanonicalJoinResidualRequest request;
  auto& key = request.key_request;
  key.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000002109";
  key.physical_dag.root_physical_node_id = 2103;
  key.physical_dag.local_transaction_id = 2104;
  key.physical_dag.statement_snapshot_id = 2105;
  key.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000002111"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000002112"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000002113"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000002114"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000002115"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000002116"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000002117"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000002118"},
  };
  key.physical_dag.nodes = {
      {.physical_node_id = 2101,
       .relational_node_id = 2101,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.left.typed.v1",
       .output_descriptor_ids = {2101, 2102},
       .causal_counter_id = 21001},
      {.physical_node_id = 2102,
       .relational_node_id = 2102,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.right.typed.v1",
       .output_descriptor_ids = {2103, 2104},
       .causal_counter_id = 21002},
      {.physical_node_id = 2103,
       .relational_node_id = 2103,
       .node_kind = exec::PhysicalNodeKind::kJoin,
       .implementation_id = "join.composite-key-residual.v1",
       .input_physical_node_ids = {2101, 2102},
       .output_descriptor_ids = {2101, 2102, 2103, 2104},
       .causal_counter_id = 21003},
  };
  key.selected_physical_node_id = 2103;
  key.left_batch = exec::MakeDescriptorBatch(
      {{"left_key", left_key, false, 2101},
       {"left_payload", left_payload, false, 2102}},
      {{{Value(left_key, "1"), Value(left_payload, "10")}},
       {{Value(left_key, "01"), Value(left_payload, "11")}},
       {{Value(left_key, "2"), Value(left_payload, "12")}}});
  key.right_batch = exec::MakeDescriptorBatch(
      {{"right_key", right_key, false, 2103},
       {"right_payload", right_payload, false, 2104}},
      {{{Value(right_key, "1"), Value(right_payload, "20")}},
       {{Value(right_key, "2"), Value(right_payload, "21")}},
       {{Value(right_key, "01"), Value(right_payload, "22")}}});
  key.key_terms = {
      {.left_column = 0,
       .left_expression_descriptor_id = 2101,
       .right_column = 0,
       .right_expression_descriptor_id = 2103},
  };
  using Truth = api::EngineSqlTruthValue;
  request.residual_truth_values = {
      Truth::true_value,  Truth::true_value, Truth::false_value,
      Truth::unknown,     Truth::false_value, Truth::true_value,
      Truth::unknown,     Truth::true_value, Truth::unknown,
  };
  return request;
}

bool RowEquals(const exec::DescriptorTuple& row,
               const std::vector<std::string>& expected) {
  if (row.values.size() != expected.size()) return false;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (row.values[index].encoded_value != expected[index]) return false;
  }
  return true;
}

// QOW-TEST-QRY-012-RESIDUAL-V1
bool ValidateJoinResidual() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalJoinResidual(Request());
  passed &= Require(
      result.diagnostic.ok && result.candidate_pair_count == 5 &&
          result.residual_recheck_count == 5 &&
          result.output_batch.rows.size() == 3 &&
          result.executed_physical_node_id == 2103,
      "residual predicate did not recheck the selected key candidates");
  passed &= Require(
      result.output_batch.rows.size() == 3 &&
          RowEquals(result.output_batch.rows[0], {"1", "10", "1", "20"}) &&
          RowEquals(result.output_batch.rows[1], {"01", "11", "01", "22"}) &&
          RowEquals(result.output_batch.rows[2], {"2", "12", "2", "21"}),
      "residual join output lost deterministic pair order");

  auto request = Request();
  request.residual_truth_values[1] =
      static_cast<api::EngineSqlTruthValue>(99);
  result = exec::ExecuteCanonicalJoinResidual(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.candidate_pair_count == 0,
                    "invalid non-candidate residual truth was hidden");

  request = Request();
  request.residual_truth_values.pop_back();
  result = exec::ExecuteCanonicalJoinResidual(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "unbound residual predicate cardinality was accepted");

  request = Request();
  request.maximum_candidate_rechecks = 4;
  result = exec::ExecuteCanonicalJoinResidual(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.residual_recheck_count == 0,
                    "residual candidate recheck bound was exceeded");

  request = Request();
  request.key_request.right_batch.rows[2].values[0].encoded_value =
      "malformed";
  result = exec::ExecuteCanonicalJoinResidual(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "malformed key input reached residual evaluation");

  request = Request();
  request.key_request.physical_dag.statement_snapshot_id = 0;
  result = exec::ExecuteCanonicalJoinResidual(request);
  passed &= Require(!result.diagnostic.ok,
                    "residual join bypassed MGA physical admission");

  request = Request();
  request.key_request.left_batch.rows.clear();
  request.residual_truth_values.clear();
  result = exec::ExecuteCanonicalJoinResidual(request);
  passed &= Require(result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.candidate_pair_count == 0 &&
                        result.residual_recheck_count == 0,
                    "empty join side invented residual candidates");
  return passed;
}

}  // namespace

int main() {
  return ValidateJoinResidual() ? EXIT_SUCCESS : EXIT_FAILURE;
}
