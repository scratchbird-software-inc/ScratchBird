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
    std::cerr << "QOW-TEST-QRY-016-DISTINCT-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000004400;"
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
    const exec::CanonicalSetOperationKind operation,
    const exec::CanonicalSetOperationAlignment alignment =
        exec::CanonicalSetOperationAlignment::kOrdinal) {
  const auto left_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004401");
  const auto right_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004402");
  const auto result_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004403");

  exec::CanonicalSetOperationAllRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000004404";
  request.physical_dag.root_physical_node_id = 4403;
  request.physical_dag.local_transaction_id = 4404;
  request.physical_dag.statement_snapshot_id = 4405;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000004411"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000004412"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000004413"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000004414"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000004415"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000004416"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000004417"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000004418"},
  };
  std::string operation_name;
  switch (operation) {
    case exec::CanonicalSetOperationKind::kUnion:
      operation_name = "union";
      break;
    case exec::CanonicalSetOperationKind::kIntersect:
      operation_name = "intersect";
      break;
    case exec::CanonicalSetOperationKind::kExcept:
      operation_name = "except";
      break;
  }
  const std::string alignment_name =
      alignment == exec::CanonicalSetOperationAlignment::kByName
          ? "by-name"
          : "ordinal";
  request.physical_dag.nodes = {
      {.physical_node_id = 4401,
       .relational_node_id = 4401,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.set-left.typed.v1",
       .output_descriptor_ids = {4401},
       .causal_counter_id = 44001},
      {.physical_node_id = 4402,
       .relational_node_id = 4402,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.set-right.typed.v1",
       .output_descriptor_ids = {4402},
       .causal_counter_id = 44002},
      {.physical_node_id = 4403,
       .relational_node_id = 4403,
       .node_kind = exec::PhysicalNodeKind::kSetOperation,
       .implementation_id = "setop." + operation_name +
                            "-distinct." + alignment_name + ".typed.v1",
       .input_physical_node_ids = {4401, 4402},
       .output_descriptor_ids = {4403},
       .causal_counter_id = 44003},
  };
  request.selected_physical_node_id = 4403;
  request.left_batch = exec::MakeDescriptorBatch(
      {{"n", left_descriptor, true, 4401}},
      {{{Value(left_descriptor, "1")}},
       {{Value(left_descriptor, "01")}},
       {{Value(left_descriptor, "2")}},
       {{Value(left_descriptor, "2")}},
       {{Null(left_descriptor)}},
       {{Null(left_descriptor)}},
       {{Value(left_descriptor, "4")}}});
  request.right_batch = exec::MakeDescriptorBatch(
      {{"n", right_descriptor, true, 4402}},
      {{{Value(right_descriptor, "1")}},
       {{Value(right_descriptor, "3")}},
       {{Value(right_descriptor, "3")}},
       {{Null(right_descriptor)}},
       {{Value(right_descriptor, "5")}}});
  request.result_columns = {{"n", result_descriptor, true, 4403}};
  request.operation = operation;
  request.alignment = alignment;
  request.quantifier = exec::CanonicalSetOperationQuantifier::kDistinct;
  request.maximum_output_row_count = 16;
  return request;
}

// QOW-TEST-QRY-016-DISTINCT-V1
bool ValidateSetOperationDistinct() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalSetOperationDistinct(
      Request(exec::CanonicalSetOperationKind::kUnion));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 6 &&
          result.eliminated_duplicate_row_count == 6 &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[1].values[0].encoded_value == "2" &&
          result.output_batch.rows[2].values[0].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[3].values[0].encoded_value == "4" &&
          result.output_batch.rows[4].values[0].encoded_value == "3" &&
          result.output_batch.rows[5].values[0].encoded_value == "5",
      "UNION DISTINCT did not emit first typed representatives in order");

  result = exec::ExecuteCanonicalSetOperationDistinct(
      Request(exec::CanonicalSetOperationKind::kIntersect));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 2 &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[1].values[0].state ==
              api::EngineValueState::sql_null &&
          result.eliminated_duplicate_row_count == 2,
      "INTERSECT DISTINCT did not emit one row per shared typed key");

  result = exec::ExecuteCanonicalSetOperationDistinct(
      Request(exec::CanonicalSetOperationKind::kExcept));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 2 &&
          result.output_batch.rows[0].values[0].encoded_value == "2" &&
          result.output_batch.rows[1].values[0].encoded_value == "4" &&
          result.eliminated_duplicate_row_count == 1,
      "EXCEPT DISTINCT used multiset subtraction instead of membership");

  result = exec::ExecuteCanonicalSetOperationDistinct(Request(
      exec::CanonicalSetOperationKind::kUnion,
      exec::CanonicalSetOperationAlignment::kByName));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 6 &&
          result.implementation_id ==
              "setop.union-distinct.by-name.typed.v1",
      "DISTINCT BY NAME did not use the admitted aligned profile");

  auto request = Request(exec::CanonicalSetOperationKind::kUnion);
  request.maximum_output_row_count = 5;
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "DISTINCT resource excess published a partial set");

  request = Request(exec::CanonicalSetOperationKind::kUnion);
  request.left_batch.rows[1].values[0].encoded_value = "malformed";
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "duplicate position hid malformed typed input");

  request = Request(exec::CanonicalSetOperationKind::kUnion);
  result = exec::ExecuteCanonicalSetOperationAll(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-016-ALL-REFUSAL-V1",
      "DISTINCT request entered the ALL execution boundary");

  request = Request(exec::CanonicalSetOperationKind::kExcept);
  request.physical_dag.nodes[2].implementation_id =
      "setop.except-all.ordinal.typed.v1";
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok,
                    "DISTINCT silently degraded to ALL execution");
  return passed;
}

}  // namespace

int main() { return ValidateSetOperationDistinct() ? EXIT_SUCCESS
                                                    : EXIT_FAILURE; }
