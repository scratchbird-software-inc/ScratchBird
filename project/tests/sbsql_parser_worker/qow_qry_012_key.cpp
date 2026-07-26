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
    std::cerr << "QOW-TEST-QRY-012-KEY-V1: " << detail << '\n';
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

api::EngineTypedValue Null(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.is_null = true;
  value.state = api::EngineValueState::sql_null;
  return value;
}

exec::CanonicalCompositeJoinKeyRequest Request() {
  const auto left_first = Descriptor(
      "019f0000-0000-7200-8000-000000001901",
      "019f0000-0000-7300-8000-000000001902");
  const auto left_second = Descriptor(
      "019f0000-0000-7200-8000-000000001903",
      "019f0000-0000-7300-8000-000000001904");
  const auto right_first = Descriptor(
      "019f0000-0000-7200-8000-000000001905",
      "019f0000-0000-7300-8000-000000001906");
  const auto right_second = Descriptor(
      "019f0000-0000-7200-8000-000000001907",
      "019f0000-0000-7300-8000-000000001908");

  exec::CanonicalCompositeJoinKeyRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001909";
  request.physical_dag.root_physical_node_id = 1903;
  request.physical_dag.local_transaction_id = 1904;
  request.physical_dag.statement_snapshot_id = 1905;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001911"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001912"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001913"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001914"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001915"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001916"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001917"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001918"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 1901,
       .relational_node_id = 1901,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.left.typed.v1",
       .output_descriptor_ids = {1901, 1902},
       .causal_counter_id = 19001},
      {.physical_node_id = 1902,
       .relational_node_id = 1902,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.right.typed.v1",
       .output_descriptor_ids = {1903, 1904},
       .causal_counter_id = 19002},
      {.physical_node_id = 1903,
       .relational_node_id = 1903,
       .node_kind = exec::PhysicalNodeKind::kJoin,
       .implementation_id = "join.composite-int64-key.v1",
       .input_physical_node_ids = {1901, 1902},
       .output_descriptor_ids = {1901, 1902, 1903, 1904},
       .causal_counter_id = 19003},
  };
  request.selected_physical_node_id = 1903;
  request.left_batch = exec::MakeDescriptorBatch(
      {{"left_first", left_first, true, 1901},
       {"left_second", left_second, true, 1902}},
      {{{Value(left_first, "1"), Value(left_second, "10")}},
       {{Value(left_first, "2"), Null(left_second)}},
       {{Value(left_first, "01"), Value(left_second, "20")}}});
  request.right_batch = exec::MakeDescriptorBatch(
      {{"right_first", right_first, true, 1903},
       {"right_second", right_second, true, 1904}},
      {{{Value(right_first, "1"), Value(right_second, "10")}},
       {{Value(right_first, "1"), Null(right_second)}},
       {{Value(right_first, "2"), Value(right_second, "99")}},
       {{Value(right_first, "2"), Null(right_second)}}});
  request.key_terms = {
      {.left_column = 0,
       .left_expression_descriptor_id = 1901,
       .right_column = 0,
       .right_expression_descriptor_id = 1903},
      {.left_column = 1,
       .left_expression_descriptor_id = 1902,
       .right_column = 1,
       .right_expression_descriptor_id = 1904},
  };
  return request;
}

// QOW-TEST-QRY-012-KEY-V1
bool ValidateCompositeJoinKey() {
  using Truth = api::EngineSqlTruthValue;
  bool passed = true;
  auto result = exec::ExecuteCanonicalCompositeJoinKey(Request());
  const std::vector<Truth> expected = {
      Truth::true_value,  Truth::unknown, Truth::false_value,
      Truth::false_value, Truth::false_value, Truth::false_value,
      Truth::unknown,     Truth::unknown, Truth::false_value,
      Truth::unknown,     Truth::false_value, Truth::false_value,
  };
  passed &= Require(result.diagnostic.ok && result.pair_count == 12 &&
                        result.pair_truth_values == expected &&
                        result.executed_physical_node_id == 1903,
                    "composite equality key produced the wrong 3VL matrix");

  auto request = Request();
  request.left_batch.rows.clear();
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(result.diagnostic.ok && result.pair_count == 0 &&
                        result.pair_truth_values.empty(),
                    "empty join side invented key comparisons");

  request = Request();
  request.maximum_key_comparisons = 23;
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok && result.pair_truth_values.empty(),
                    "composite key comparison bound was exceeded");

  request = Request();
  request.key_terms.push_back(request.key_terms.front());
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok && result.pair_truth_values.empty(),
                    "duplicate composite key handle was accepted");

  request = Request();
  request.key_terms[0].left_expression_descriptor_id = 9999;
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok,
                    "unbound composite key handle was accepted");

  request = Request();
  request.left_batch.rows[2].values[0].encoded_value = "malformed";
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok && result.pair_truth_values.empty(),
                    "malformed key operand published a partial matrix");

  request = Request();
  request.right_batch.rows[1].values.pop_back();
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok && result.pair_truth_values.empty(),
                    "ragged join input published a partial matrix");

  request = Request();
  request.physical_dag.nodes.back().output_descriptor_ids =
      {1903, 1904, 1901, 1902};
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok,
                    "unbound composite join output order was accepted");

  request = Request();
  request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok,
                    "composite join key bypassed MGA physical admission");
  return passed;
}

}  // namespace

int main() {
  return ValidateCompositeJoinKey() ? EXIT_SUCCESS : EXIT_FAILURE;
}
