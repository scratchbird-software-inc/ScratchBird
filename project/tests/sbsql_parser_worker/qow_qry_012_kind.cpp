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
    std::cerr << "QOW-TEST-QRY-012-KIND-V1: " << detail << '\n';
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

exec::CanonicalJoinKindRequest Request() {
  const auto left_key = Descriptor(
      "019f0000-0000-7200-8000-000000002201",
      "019f0000-0000-7300-8000-000000002202");
  const auto left_payload = Descriptor(
      "019f0000-0000-7200-8000-000000002203",
      "019f0000-0000-7300-8000-000000002204");
  const auto right_key = Descriptor(
      "019f0000-0000-7200-8000-000000002205",
      "019f0000-0000-7300-8000-000000002206");
  const auto right_payload = Descriptor(
      "019f0000-0000-7200-8000-000000002207",
      "019f0000-0000-7300-8000-000000002208");

  exec::CanonicalJoinKindRequest request;
  auto& residual = request.residual_request;
  auto& key = residual.key_request;
  key.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000002209";
  key.physical_dag.root_physical_node_id = 2203;
  key.physical_dag.local_transaction_id = 2204;
  key.physical_dag.statement_snapshot_id = 2205;
  key.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000002211"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000002212"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000002213"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000002214"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000002215"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000002216"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000002217"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000002218"},
  };
  key.physical_dag.nodes = {
      {.physical_node_id = 2201,
       .relational_node_id = 2201,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.left.typed.v1",
       .output_descriptor_ids = {2201, 2202},
       .causal_counter_id = 22001},
      {.physical_node_id = 2202,
       .relational_node_id = 2202,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.right.typed.v1",
       .output_descriptor_ids = {2203, 2204},
       .causal_counter_id = 22002},
      {.physical_node_id = 2203,
       .relational_node_id = 2203,
       .node_kind = exec::PhysicalNodeKind::kJoin,
       .implementation_id = "join.left-outer.residual.v1",
       .input_physical_node_ids = {2201, 2202},
       .output_descriptor_ids = {2201, 2202, 2203, 2204},
       .causal_counter_id = 22003},
  };
  key.selected_physical_node_id = 2203;
  key.left_batch = exec::MakeDescriptorBatch(
      {{"left_key", left_key, false, 2201},
       {"left_payload", left_payload, false, 2202}},
      {{{Value(left_key, "1"), Value(left_payload, "10")}},
       {{Value(left_key, "01"), Value(left_payload, "11")}},
       {{Value(left_key, "2"), Value(left_payload, "12")}},
       {{Value(left_key, "3"), Value(left_payload, "13")}}});
  key.right_batch = exec::MakeDescriptorBatch(
      {{"right_key", right_key, false, 2203},
       {"right_payload", right_payload, false, 2204}},
      {{{Value(right_key, "1"), Value(right_payload, "20")}},
       {{Value(right_key, "2"), Value(right_payload, "21")}},
       {{Value(right_key, "01"), Value(right_payload, "22")}}});
  key.key_terms = {
      {.left_column = 0,
       .left_expression_descriptor_id = 2201,
       .right_column = 0,
       .right_expression_descriptor_id = 2203},
  };
  using Truth = api::EngineSqlTruthValue;
  residual.residual_truth_values = {
      Truth::true_value, Truth::true_value,  Truth::false_value,
      Truth::unknown,    Truth::false_value, Truth::true_value,
      Truth::unknown,    Truth::true_value,  Truth::unknown,
      Truth::true_value, Truth::true_value,  Truth::true_value,
  };
  return request;
}

bool ValueEquals(const api::EngineTypedValue& value,
                 const std::string_view expected) {
  return value.state == api::EngineValueState::value && !value.is_null &&
         value.encoded_value == expected;
}

bool IsNull(const api::EngineTypedValue& value) {
  return value.state == api::EngineValueState::sql_null && value.is_null &&
         value.encoded_value.empty() && value.binary_value.empty();
}

// QOW-TEST-QRY-012-KIND-V1
bool ValidateJoinKind() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalJoinKind(Request());
  passed &= Require(
      result.diagnostic.ok && result.matched_pair_count == 3 &&
          result.unmatched_left_row_count == 1 &&
          result.output_batch.rows.size() == 4 &&
          result.output_batch.columns[2].nullable &&
          result.output_batch.columns[3].nullable &&
          result.executed_physical_node_id == 2203,
      "left outer join produced the wrong cardinality or nullability");
  passed &= Require(
      ValueEquals(result.output_batch.rows[0].values[1], "10") &&
          ValueEquals(result.output_batch.rows[0].values[3], "20") &&
          ValueEquals(result.output_batch.rows[1].values[1], "11") &&
          ValueEquals(result.output_batch.rows[1].values[3], "22") &&
          ValueEquals(result.output_batch.rows[2].values[1], "12") &&
          ValueEquals(result.output_batch.rows[2].values[3], "21") &&
          ValueEquals(result.output_batch.rows[3].values[0], "3") &&
          ValueEquals(result.output_batch.rows[3].values[1], "13") &&
          IsNull(result.output_batch.rows[3].values[2]) &&
          IsNull(result.output_batch.rows[3].values[3]),
      "left outer join lost pair order or canonical unmatched NULLs");

  auto request = Request();
  request.join_kind = exec::CanonicalAcceptedJoinKind::kInner;
  result = exec::ExecuteCanonicalJoinKind(request);
  passed &= Require(result.diagnostic.ok && result.matched_pair_count == 3 &&
                        result.output_batch.rows.size() == 3 &&
                        result.output_batch.columns.size() == 4,
                    "inner join did not preserve accepted pair multiplicity");

  request = Request();
  request.join_kind = exec::CanonicalAcceptedJoinKind::kCross;
  result = exec::ExecuteCanonicalJoinKind(request);
  passed &= Require(result.diagnostic.ok && result.matched_pair_count == 12 &&
                        result.output_batch.rows.size() == 12,
                    "cross join did not emit the Cartesian product");

  request = Request();
  request.join_kind = exec::CanonicalAcceptedJoinKind::kLeftSemi;
  result = exec::ExecuteCanonicalJoinKind(request);
  passed &= Require(result.diagnostic.ok && result.matched_pair_count == 3 &&
                        result.emitted_left_row_count == 3 &&
                        result.output_batch.columns.size() == 2 &&
                        result.output_batch.rows.size() == 3 &&
                        ValueEquals(result.output_batch.rows[2].values[1],
                                    "12"),
                    "left semi join did not emit each matching left row once");

  request = Request();
  request.join_kind = exec::CanonicalAcceptedJoinKind::kLeftAnti;
  result = exec::ExecuteCanonicalJoinKind(request);
  passed &= Require(result.diagnostic.ok &&
                        result.unmatched_left_row_count == 1 &&
                        result.emitted_left_row_count == 1 &&
                        result.output_batch.columns.size() == 2 &&
                        result.output_batch.rows.size() == 1 &&
                        ValueEquals(result.output_batch.rows[0].values[1],
                                    "13"),
                    "left anti join did not emit only unmatched left rows");

  request = Request();
  request.join_kind = exec::CanonicalAcceptedJoinKind::kRightOuter;
  request.residual_request.key_request.left_batch.rows.clear();
  request.residual_request.residual_truth_values.clear();
  result = exec::ExecuteCanonicalJoinKind(request);
  passed &= Require(result.diagnostic.ok && result.matched_pair_count == 0 &&
                        result.unmatched_right_row_count == 3 &&
                        result.output_batch.rows.size() == 3 &&
                        result.output_batch.columns[0].nullable &&
                        result.output_batch.columns[1].nullable &&
                        IsNull(result.output_batch.rows[0].values[0]) &&
                        ValueEquals(result.output_batch.rows[2].values[3],
                                    "22"),
                    "right outer join did not preserve an unmatched right side");

  request = Request();
  request.join_kind = exec::CanonicalAcceptedJoinKind::kFullOuter;
  result = exec::ExecuteCanonicalJoinKind(request);
  passed &= Require(result.diagnostic.ok && result.matched_pair_count == 3 &&
                        result.unmatched_left_row_count == 1 &&
                        result.unmatched_right_row_count == 0 &&
                        result.output_batch.rows.size() == 4 &&
                        result.output_batch.columns[0].nullable &&
                        result.output_batch.columns[3].nullable,
                    "full outer join did not preserve both nullable sides");

  request = Request();
  request.join_kind = static_cast<exec::CanonicalAcceptedJoinKind>(99);
  result = exec::ExecuteCanonicalJoinKind(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "unaccepted join kind was executed");

  request = Request();
  request.maximum_output_rows = 3;
  result = exec::ExecuteCanonicalJoinKind(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.matched_pair_count == 0,
                    "left outer output bound was exceeded");

  request = Request();
  request.residual_request.key_request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalJoinKind(request);
  passed &= Require(!result.diagnostic.ok,
                    "join kind bypassed MGA physical admission");

  request = Request();
  request.residual_request.key_request.right_batch.rows.clear();
  request.residual_request.residual_truth_values.clear();
  result = exec::ExecuteCanonicalJoinKind(request);
  passed &= Require(
      result.diagnostic.ok && result.matched_pair_count == 0 &&
          result.unmatched_left_row_count == 4 &&
          result.output_batch.rows.size() == 4 &&
          IsNull(result.output_batch.rows[0].values[2]) &&
          IsNull(result.output_batch.rows[3].values[3]),
      "empty right side did not preserve every left row");

  request = Request();
  request.residual_request.key_request.left_batch.rows.clear();
  request.residual_request.residual_truth_values.clear();
  result = exec::ExecuteCanonicalJoinKind(request);
  passed &= Require(result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.unmatched_left_row_count == 0,
                    "empty left side invented outer rows");
  return passed;
}

}  // namespace

int main() {
  return ValidateJoinKind() ? EXIT_SUCCESS : EXIT_FAILURE;
}
