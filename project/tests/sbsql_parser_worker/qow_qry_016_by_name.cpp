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
    std::cerr << "QOW-TEST-QRY-016-BY-NAME-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& uuid,
                                 const std::string& type,
                                 const std::string& type_uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type;
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=nullable";
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

exec::CanonicalSetOperationAllRequest Request(
    const exec::CanonicalSetOperationKind operation) {
  const std::string int_type =
      "019f0000-0000-7300-8000-000000004300";
  const std::string text_type =
      "019f0000-0000-7300-8000-000000004301";
  const auto left_a = Descriptor(
      "019f0000-0000-7200-8000-000000004301", "int64", int_type);
  const auto left_b = Descriptor(
      "019f0000-0000-7200-8000-000000004302", "text", text_type);
  const auto right_b = Descriptor(
      "019f0000-0000-7200-8000-000000004303", "text", text_type);
  const auto right_a = Descriptor(
      "019f0000-0000-7200-8000-000000004304", "int64", int_type);
  const auto result_a = Descriptor(
      "019f0000-0000-7200-8000-000000004305", "int64", int_type);
  const auto result_b = Descriptor(
      "019f0000-0000-7200-8000-000000004306", "text", text_type);

  exec::CanonicalSetOperationAllRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000004307";
  request.physical_dag.root_physical_node_id = 4303;
  request.physical_dag.local_transaction_id = 4304;
  request.physical_dag.statement_snapshot_id = 4305;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000004311"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000004312"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000004313"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000004314"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000004315"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000004316"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000004317"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000004318"},
  };
  std::string implementation;
  switch (operation) {
    case exec::CanonicalSetOperationKind::kUnion:
      implementation = "setop.union-all.by-name.typed.v1";
      break;
    case exec::CanonicalSetOperationKind::kIntersect:
      implementation = "setop.intersect-all.by-name.typed.v1";
      break;
    case exec::CanonicalSetOperationKind::kExcept:
      implementation = "setop.except-all.by-name.typed.v1";
      break;
  }
  request.physical_dag.nodes = {
      {.physical_node_id = 4301,
       .relational_node_id = 4301,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.set-left.typed.v1",
       .output_descriptor_ids = {4301, 4302},
       .causal_counter_id = 43001},
      {.physical_node_id = 4302,
       .relational_node_id = 4302,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.set-right.typed.v1",
       .output_descriptor_ids = {4303, 4304},
       .causal_counter_id = 43002},
      {.physical_node_id = 4303,
       .relational_node_id = 4303,
       .node_kind = exec::PhysicalNodeKind::kSetOperation,
       .implementation_id = implementation,
       .input_physical_node_ids = {4301, 4302},
       .output_descriptor_ids = {4305, 4306},
       .causal_counter_id = 43003},
  };
  request.selected_physical_node_id = 4303;
  request.left_batch = exec::MakeDescriptorBatch(
      {{"a", left_a, true, 4301}, {"b", left_b, true, 4302}},
      {{{Value(left_a, "1"), Value(left_b, "alpha")}},
       {{Value(left_a, "2"), Value(left_b, "beta")}}});
  request.right_batch = exec::MakeDescriptorBatch(
      {{"b", right_b, true, 4303}, {"a", right_a, true, 4304}},
      {{{Value(right_b, "beta"), Value(right_a, "02")}},
       {{Value(right_b, "gamma"), Value(right_a, "3")}}});
  request.result_columns = {
      {"a", result_a, true, 4305}, {"b", result_b, true, 4306}};
  request.operation = operation;
  request.alignment = exec::CanonicalSetOperationAlignment::kByName;
  request.maximum_output_row_count = 8;
  return request;
}

// QOW-TEST-QRY-016-BY-NAME-V1
bool ValidateSetOperationByName() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalSetOperationAll(
      Request(exec::CanonicalSetOperationKind::kUnion));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 4 &&
          result.output_batch.rows[2].values[0].encoded_value == "02" &&
          result.output_batch.rows[2].values[1].encoded_value == "beta" &&
          result.right_to_result_column_indices ==
              std::vector<std::size_t>({1, 0}) &&
          result.implementation_id == "setop.union-all.by-name.typed.v1",
      "UNION ALL BY NAME did not align the reversed bound columns");

  result = exec::ExecuteCanonicalSetOperationAll(
      Request(exec::CanonicalSetOperationKind::kIntersect));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[0].encoded_value == "2" &&
          result.output_batch.rows[0].values[1].encoded_value == "beta",
      "INTERSECT ALL BY NAME did not compare the aligned typed row");

  result = exec::ExecuteCanonicalSetOperationAll(
      Request(exec::CanonicalSetOperationKind::kExcept));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[0].values[1].encoded_value == "alpha",
      "EXCEPT ALL BY NAME did not subtract the aligned typed row");

  auto request = Request(exec::CanonicalSetOperationKind::kUnion);
  request.right_batch.columns[0].stable_name = "a";
  result = exec::ExecuteCanonicalSetOperationAll(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-016-BY-NAME-REFUSAL-V1" &&
          result.output_batch.rows.empty(),
      "duplicate BY NAME operand name was accepted");

  request = Request(exec::CanonicalSetOperationKind::kUnion);
  request.right_batch.columns[0].stable_name = "c";
  result = exec::ExecuteCanonicalSetOperationAll(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-016-BY-NAME-REFUSAL-V1",
      "different BY NAME operand name set was accepted");

  request = Request(exec::CanonicalSetOperationKind::kUnion);
  std::swap(request.result_columns[0], request.result_columns[1]);
  std::swap(request.physical_dag.nodes[2].output_descriptor_ids[0],
            request.physical_dag.nodes[2].output_descriptor_ids[1]);
  result = exec::ExecuteCanonicalSetOperationAll(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-016-BY-NAME-REFUSAL-V1",
      "BY NAME result order drift from the left operand was accepted");

  request = Request(exec::CanonicalSetOperationKind::kUnion);
  request.physical_dag.nodes[2].implementation_id =
      "setop.union-all.ordinal.typed.v1";
  result = exec::ExecuteCanonicalSetOperationAll(request);
  passed &= Require(!result.diagnostic.ok,
                    "BY NAME silently degraded to ordinal execution");
  return passed;
}

}  // namespace

int main() { return ValidateSetOperationByName() ? EXIT_SUCCESS : EXIT_FAILURE; }
