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

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-016-ARITY-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor(const std::uint32_t ordinal) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-0000000042" +
      std::string(ordinal < 10 ? "0" : "") + std::to_string(ordinal);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000004200;"
      "nullability=nullable";
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            std::string encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = std::move(encoded);
  value.state = api::EngineValueState::value;
  return value;
}

exec::CanonicalSetOperationAllRequest Request() {
  const auto left_a = Descriptor(1);
  const auto left_b = Descriptor(2);
  const auto right_a = Descriptor(3);
  const auto right_b = Descriptor(4);
  const auto result_a = Descriptor(5);
  const auto result_b = Descriptor(6);

  exec::CanonicalSetOperationAllRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000004207";
  request.physical_dag.root_physical_node_id = 4203;
  request.physical_dag.local_transaction_id = 4204;
  request.physical_dag.statement_snapshot_id = 4205;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000004211"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000004212"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000004213"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000004214"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000004215"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000004216"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000004217"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000004218"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 4201,
       .relational_node_id = 4201,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.set-left.typed.v1",
       .output_descriptor_ids = {4201, 4202},
       .causal_counter_id = 42001},
      {.physical_node_id = 4202,
       .relational_node_id = 4202,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.set-right.typed.v1",
       .output_descriptor_ids = {4203, 4204},
       .causal_counter_id = 42002},
      {.physical_node_id = 4203,
       .relational_node_id = 4203,
       .node_kind = exec::PhysicalNodeKind::kSetOperation,
       .implementation_id = "setop.union-all.ordinal.typed.v1",
       .input_physical_node_ids = {4201, 4202},
       .output_descriptor_ids = {4205, 4206},
       .causal_counter_id = 42003},
  };
  request.selected_physical_node_id = 4203;
  request.left_batch = exec::MakeDescriptorBatch(
      {{"left_a", left_a, true, 4201}, {"left_b", left_b, true, 4202}},
      {{{Value(left_a, "1"), Value(left_b, "2")}}});
  request.right_batch = exec::MakeDescriptorBatch(
      {{"right_a", right_a, true, 4203},
       {"right_b", right_b, true, 4204}},
      {{{Value(right_a, "3"), Value(right_b, "4")}}});
  request.result_columns = {
      {"a", result_a, true, 4205}, {"b", result_b, true, 4206}};
  request.operation = exec::CanonicalSetOperationKind::kUnion;
  request.maximum_output_row_count = 8;
  return request;
}

// QOW-TEST-QRY-016-ARITY-V1
bool ValidateSetOperationArity() {
  bool passed = true;
  auto request = Request();
  auto result = exec::ExecuteCanonicalSetOperationAll(request);
  passed &= Require(result.diagnostic.ok && result.output_batch.rows.size() == 2,
                    "equal descriptor arity was not admitted");

  request = Request();
  request.right_batch.columns.pop_back();
  request.right_batch.rows[0].values.pop_back();
  request.physical_dag.nodes[1].output_descriptor_ids.pop_back();
  result = exec::ExecuteCanonicalSetOperationAll(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-016-ARITY-REFUSAL-V1" &&
          result.output_batch.rows.empty() &&
          result.executed_physical_node_id == 0,
      "right operand arity mismatch did not fail with the exact diagnostic");

  request = Request();
  request.result_columns.pop_back();
  request.physical_dag.nodes[2].output_descriptor_ids.pop_back();
  result = exec::ExecuteCanonicalSetOperationAll(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-016-ARITY-REFUSAL-V1" &&
          result.output_batch.rows.empty(),
      "result arity mismatch did not fail before publication");

  request = Request();
  request.right_batch.rows[0].values.pop_back();
  result = exec::ExecuteCanonicalSetOperationAll(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-016-ALL-REFUSAL-V1",
      "ragged transport row was confused with descriptor arity");
  return passed;
}

}  // namespace

int main() { return ValidateSetOperationArity() ? EXIT_SUCCESS : EXIT_FAILURE; }
