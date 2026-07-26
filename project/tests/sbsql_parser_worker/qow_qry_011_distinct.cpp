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
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-011-DISTINCT-V1: " << detail << '\n';
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

exec::CanonicalInt64SumDistinctRequest Request() {
  const auto value_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001601",
      "019f0000-0000-7300-8000-000000001602");
  const auto result_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001603",
      "019f0000-0000-7300-8000-000000001604");

  exec::CanonicalInt64SumDistinctRequest request;
  auto& aggregate = request.aggregate_request;
  aggregate.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001605";
  aggregate.physical_dag.root_physical_node_id = 1602;
  aggregate.physical_dag.local_transaction_id = 1603;
  aggregate.physical_dag.statement_snapshot_id = 1604;
  aggregate.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001611"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001612"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001613"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001614"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001615"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001616"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001617"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001618"},
  };
  aggregate.physical_dag.nodes = {
      {.physical_node_id = 1601,
       .relational_node_id = 1601,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {1601},
       .causal_counter_id = 16001},
      {.physical_node_id = 1602,
       .relational_node_id = 1602,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.sum-int64-distinct.v1",
       .input_physical_node_ids = {1601},
       .output_descriptor_ids = {1602},
       .causal_counter_id = 16002},
  };
  aggregate.selected_physical_node_id = 1602;
  aggregate.input_batch = exec::MakeDescriptorBatch(
      {{"amount", value_descriptor, true, 1601}},
      {{{Value(value_descriptor, "10")}},
       {{Value(value_descriptor, "10")}},
       {{Value(value_descriptor, "01")}},
       {{Value(value_descriptor, "1")}},
       {{Null(value_descriptor)}},
       {{Value(value_descriptor, "-3")}},
       {{Value(value_descriptor, "-3")}}});
  aggregate.value_expression_descriptor_id = 1601;
  aggregate.result_column = {"sum_amount", result_descriptor, true, 1602};
  return request;
}

// QOW-TEST-QRY-011-DISTINCT-V1
bool ValidateAggregateDistinct() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalInt64SumDistinct(Request());
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 1602 &&
                        result.distinct_value_count == 3 &&
                        result.state.transition_count == 3 &&
                        result.state.non_null_count == 3 &&
                        result.state.accumulated_value == 8 &&
                        result.state.has_value,
                    "numeric DISTINCT aliases or duplicate values changed SUM");

  auto request = Request();
  const auto& descriptor = request.aggregate_request.input_batch.columns[0]
                               .descriptor;
  request.aggregate_request.input_batch.rows = {
      {{Null(descriptor)}}, {{Null(descriptor)}}};
  result = exec::ExecuteCanonicalInt64SumDistinct(request);
  passed &= Require(result.diagnostic.ok &&
                        result.distinct_value_count == 0 &&
                        result.state.transition_count == 0 &&
                        !result.state.has_value,
                    "SQL NULL entered the DISTINCT set or SUM state");

  request = Request();
  request.maximum_distinct_value_count = 2;
  result = exec::ExecuteCanonicalInt64SumDistinct(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value &&
                        result.distinct_value_count == 0,
                    "DISTINCT value resource bound was exceeded");

  request = Request();
  request.aggregate_request.maximum_transition_count = 6;
  result = exec::ExecuteCanonicalInt64SumDistinct(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "DISTINCT scan resource bound was exceeded");

  request = Request();
  request.aggregate_request.input_batch.rows[1].values[0].encoded_value =
      "malformed";
  result = exec::ExecuteCanonicalInt64SumDistinct(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "duplicate position hid malformed typed input");

  request = Request();
  request.aggregate_request.input_batch.rows[1].values.clear();
  result = exec::ExecuteCanonicalInt64SumDistinct(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "duplicate position hid ragged typed input");

  request = Request();
  request.aggregate_request.input_batch.rows = {
      {{Value(request.aggregate_request.input_batch.columns[0].descriptor,
              std::to_string(std::numeric_limits<std::int64_t>::max()))}},
      {{Value(request.aggregate_request.input_batch.columns[0].descriptor,
              "1")}}};
  result = exec::ExecuteCanonicalInt64SumDistinct(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "DISTINCT SUM overflow published partial state");

  request = Request();
  request.maximum_distinct_value_count = 0;
  result = exec::ExecuteCanonicalInt64SumDistinct(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "zero DISTINCT resource bound was accepted");

  request = Request();
  request.aggregate_request.result_column.descriptor_id = 9999;
  result = exec::ExecuteCanonicalInt64SumDistinct(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "unbound DISTINCT result handle was accepted");

  request = Request();
  request.aggregate_request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalInt64SumDistinct(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "DISTINCT aggregate bypassed MGA physical admission");
  return passed;
}

}  // namespace

int main() {
  return ValidateAggregateDistinct() ? EXIT_SUCCESS : EXIT_FAILURE;
}
