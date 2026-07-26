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
#include <limits>
#include <string>
#include <string_view>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-QRY-011-STATE-V1: " << detail << '\n';
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& descriptor_uuid,
                                 const std::string& type_uuid,
                                 const std::string& nullability) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=" + nullability;
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

exec::CanonicalInt64SumStateRequest Request() {
  const auto value_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001201",
      "019f0000-0000-7300-8000-000000001202", "nullable");
  const auto result_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001203",
      "019f0000-0000-7300-8000-000000001204", "nullable");

  exec::CanonicalInt64SumStateRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001205";
  request.physical_dag.root_physical_node_id = 1202;
  request.physical_dag.local_transaction_id = 1203;
  request.physical_dag.statement_snapshot_id = 1204;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001211"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001212"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001213"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001214"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001215"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001216"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001217"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001218"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 1201,
       .relational_node_id = 1201,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {1201},
       .causal_counter_id = 12001},
      {.physical_node_id = 1202,
       .relational_node_id = 1202,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.sum-int64-state.v1",
       .input_physical_node_ids = {1201},
       .output_descriptor_ids = {1202},
       .causal_counter_id = 12002},
  };
  request.selected_physical_node_id = 1202;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"amount", value_descriptor, true, 1201}},
      {{{Value(value_descriptor, "10")}},
       {{Null(value_descriptor)}},
       {{Value(value_descriptor, "-3")}}});
  request.value_column = 0;
  request.value_expression_descriptor_id = 1201;
  request.result_column = {"sum_amount", result_descriptor, true, 1202};
  return request;
}

// QOW-TEST-QRY-011-STATE-V1
bool ValidateAggregateTransitionState() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalInt64SumState(Request());
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 1202 &&
                        result.causal_counter_id == 12002,
                    "typed physical SUM state did not execute");
  passed &= Require(result.state.transition_count == 3 &&
                        result.state.non_null_count == 2 &&
                        result.state.accumulated_value == 7 &&
                        result.state.has_value &&
                        result.state.value_expression_descriptor_id == 1201 &&
                        result.state.result_column.descriptor_id == 1202,
                    "SUM transition state lost rows, NULL semantics, or handles");

  auto request = Request();
  request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(result.diagnostic.ok &&
                        result.state.transition_count == 0 &&
                        result.state.non_null_count == 0 &&
                        !result.state.has_value,
                    "empty SUM state invented a value");

  request = Request();
  request.input_batch.rows = {{{Null(request.input_batch.columns[0].descriptor)}}};
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(result.diagnostic.ok &&
                        result.state.transition_count == 1 &&
                        result.state.non_null_count == 0 &&
                        !result.state.has_value,
                    "all-NULL SUM state invented a numeric transition");

  request = Request();
  const auto& descriptor = request.input_batch.columns[0].descriptor;
  request.input_batch.rows =
      {{{Value(descriptor,
               std::to_string(std::numeric_limits<std::int64_t>::max()))}},
       {{Value(descriptor, "1")}}};
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value &&
                        result.state.transition_count == 0,
                    "SUM overflow published a partial transition state");

  request = Request();
  request.input_batch.rows[0].values[0].encoded_value = "not-an-int64";
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "malformed SUM operand was accepted");

  request = Request();
  request.value_expression_descriptor_id = 1202;
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "mismatched aggregate expression handle was accepted");

  request = Request();
  request.result_column.nullable = false;
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "non-nullable SUM result descriptor was accepted");

  request = Request();
  request.maximum_transition_count = 2;
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "SUM transition resource bound was exceeded");
  return passed;
}

}  // namespace

int main() {
  return ValidateAggregateTransitionState() ? EXIT_SUCCESS : EXIT_FAILURE;
}
