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
    std::cerr << "QOW-TEST-QRY-016-ALL-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000004100;"
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

api::EngineTypedValue Null(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.is_null = true;
  value.state = api::EngineValueState::sql_null;
  return value;
}

exec::CanonicalSetOperationAllRequest Request(
    const exec::CanonicalSetOperationKind operation) {
  const auto left_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004101");
  const auto right_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004102");
  const auto result_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004103");

  exec::CanonicalSetOperationAllRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000004104";
  request.physical_dag.root_physical_node_id = 4103;
  request.physical_dag.local_transaction_id = 4104;
  request.physical_dag.statement_snapshot_id = 4105;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000004111"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000004112"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000004113"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000004114"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000004115"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000004116"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000004117"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000004118"},
  };
  std::string implementation;
  switch (operation) {
    case exec::CanonicalSetOperationKind::kUnion:
      implementation = "setop.union-all.ordinal.typed.v1";
      break;
    case exec::CanonicalSetOperationKind::kIntersect:
      implementation = "setop.intersect-all.ordinal.typed.v1";
      break;
    case exec::CanonicalSetOperationKind::kExcept:
      implementation = "setop.except-all.ordinal.typed.v1";
      break;
  }
  request.physical_dag.nodes = {
      {.physical_node_id = 4101,
       .relational_node_id = 4101,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.set-left.typed.v1",
       .output_descriptor_ids = {4101},
       .causal_counter_id = 41001},
      {.physical_node_id = 4102,
       .relational_node_id = 4102,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.set-right.typed.v1",
       .output_descriptor_ids = {4102},
       .causal_counter_id = 41002},
      {.physical_node_id = 4103,
       .relational_node_id = 4103,
       .node_kind = exec::PhysicalNodeKind::kSetOperation,
       .implementation_id = implementation,
       .input_physical_node_ids = {4101, 4102},
       .output_descriptor_ids = {4103},
       .causal_counter_id = 41003},
  };
  request.selected_physical_node_id = 4103;
  request.left_batch = exec::MakeDescriptorBatch(
      {{"left_n", left_descriptor, true, 4101}},
      {{{Value(left_descriptor, "1")}},
       {{Value(left_descriptor, "01")}},
       {{Value(left_descriptor, "2")}},
       {{Null(left_descriptor)}}});
  request.right_batch = exec::MakeDescriptorBatch(
      {{"right_n", right_descriptor, true, 4102}},
      {{{Value(right_descriptor, "1")}},
       {{Value(right_descriptor, "3")}},
       {{Null(right_descriptor)}}});
  request.result_columns = {
      {"n", result_descriptor, true, 4103},
  };
  request.operation = operation;
  request.maximum_output_row_count = 16;
  return request;
}

// QOW-TEST-QRY-016-ALL-V1
bool ValidateSetOperationAll() {
  bool passed = true;

  auto result = exec::ExecuteCanonicalSetOperationAll(
      Request(exec::CanonicalSetOperationKind::kUnion));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 7 &&
          result.left_input_row_count == 4 &&
          result.right_input_row_count == 3 &&
          result.consumed_right_multiplicity_count == 0 &&
          result.implementation_id == "setop.union-all.ordinal.typed.v1" &&
          result.executed_physical_node_id == 4103 &&
          result.causal_counter_id == 41003 &&
          result.output_batch.rows[0].values[0].descriptor.descriptor_uuid
                  .canonical ==
              "019f0000-0000-7200-8000-000000004103",
      "UNION ALL did not preserve every typed row or result descriptor");

  result = exec::ExecuteCanonicalSetOperationAll(
      Request(exec::CanonicalSetOperationKind::kIntersect));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 2 &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[1].values[0].state ==
              api::EngineValueState::sql_null &&
          result.consumed_right_multiplicity_count == 2,
      "INTERSECT ALL did not apply minimum typed multiplicities");

  result = exec::ExecuteCanonicalSetOperationAll(
      Request(exec::CanonicalSetOperationKind::kExcept));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 2 &&
          result.output_batch.rows[0].values[0].encoded_value == "01" &&
          result.output_batch.rows[1].values[0].encoded_value == "2" &&
          result.consumed_right_multiplicity_count == 2,
      "EXCEPT ALL did not subtract typed right-side multiplicities");

  auto request = Request(exec::CanonicalSetOperationKind::kUnion);
  request.maximum_output_row_count = 6;
  result = exec::ExecuteCanonicalSetOperationAll(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "UNION ALL published rows beyond its resource bound");

  request = Request(exec::CanonicalSetOperationKind::kIntersect);
  request.right_batch.rows[0].values[0].encoded_value = "malformed";
  result = exec::ExecuteCanonicalSetOperationAll(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "malformed typed multiplicity input was hidden");

  request = Request(exec::CanonicalSetOperationKind::kExcept);
  request.physical_dag.nodes[2].implementation_id =
      "setop.union-all.ordinal.typed.v1";
  result = exec::ExecuteCanonicalSetOperationAll(request);
  passed &= Require(!result.diagnostic.ok,
                    "set-operation kind/profile drift was accepted");

  request = Request(exec::CanonicalSetOperationKind::kUnion);
  request.physical_dag.statement_snapshot_id = 0;
  result = exec::ExecuteCanonicalSetOperationAll(request);
  passed &= Require(!result.diagnostic.ok,
                    "set operation escaped its MGA statement boundary");
  return passed;
}

}  // namespace

int main() { return ValidateSetOperationAll() ? EXIT_SUCCESS : EXIT_FAILURE; }
