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
  if (!condition) std::cerr << "QOW-TEST-QRY-011-FILTER-V1: " << detail << '\n';
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

exec::CanonicalInt64SumFilterRequest Request() {
  const auto value_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001501",
      "019f0000-0000-7300-8000-000000001502");
  const auto result_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001503",
      "019f0000-0000-7300-8000-000000001504");

  exec::CanonicalInt64SumFilterRequest request;
  auto& aggregate = request.aggregate_request;
  aggregate.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001505";
  aggregate.physical_dag.root_physical_node_id = 1502;
  aggregate.physical_dag.local_transaction_id = 1503;
  aggregate.physical_dag.statement_snapshot_id = 1504;
  aggregate.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001511"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001512"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001513"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001514"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001515"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001516"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001517"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001518"},
  };
  aggregate.physical_dag.nodes = {
      {.physical_node_id = 1501,
       .relational_node_id = 1501,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {1501},
       .causal_counter_id = 15001},
      {.physical_node_id = 1502,
       .relational_node_id = 1502,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.sum-int64-filter.v1",
       .input_physical_node_ids = {1501},
       .output_descriptor_ids = {1502},
       .causal_counter_id = 15002},
  };
  aggregate.selected_physical_node_id = 1502;
  aggregate.input_batch = exec::MakeDescriptorBatch(
      {{"amount", value_descriptor, true, 1501}},
      {{{Value(value_descriptor, "10")}},
       {{Value(value_descriptor, "100")}},
       {{Value(value_descriptor, "50")}},
       {{Value(value_descriptor, "-3")}},
       {{Null(value_descriptor)}}});
  aggregate.value_expression_descriptor_id = 1501;
  aggregate.result_column = {"sum_amount", result_descriptor, true, 1502};
  request.row_truth_values = {
      api::EngineSqlTruthValue::true_value,
      api::EngineSqlTruthValue::false_value,
      api::EngineSqlTruthValue::unknown,
      api::EngineSqlTruthValue::true_value,
      api::EngineSqlTruthValue::true_value,
  };
  return request;
}

// QOW-TEST-QRY-011-FILTER-V1
bool ValidateAggregateFilter() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalInt64SumFilter(Request());
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 1502 &&
                        result.state.transition_count == 3 &&
                        result.state.non_null_count == 2 &&
                        result.state.accumulated_value == 7,
                    "aggregate FILTER did not apply TRUE-only 3VL transitions");

  auto request = Request();
  request.row_truth_values = {
      api::EngineSqlTruthValue::false_value,
      api::EngineSqlTruthValue::unknown,
      api::EngineSqlTruthValue::false_value,
      api::EngineSqlTruthValue::unknown,
      api::EngineSqlTruthValue::false_value,
  };
  result = exec::ExecuteCanonicalInt64SumFilter(request);
  passed &= Require(result.diagnostic.ok &&
                        result.state.transition_count == 0 &&
                        !result.state.has_value,
                    "FALSE/UNKNOWN aggregate FILTER rows changed state");

  request = Request();
  request.row_truth_values.pop_back();
  result = exec::ExecuteCanonicalInt64SumFilter(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "FILTER truth cardinality mismatch was accepted");

  request = Request();
  request.row_truth_values[1] =
      static_cast<api::EngineSqlTruthValue>(255);
  result = exec::ExecuteCanonicalInt64SumFilter(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "unbound FILTER truth value was accepted");

  request = Request();
  request.aggregate_request.input_batch.rows[1].values.clear();
  result = exec::ExecuteCanonicalInt64SumFilter(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "FALSE row hid ragged typed aggregate input");

  request = Request();
  request.aggregate_request.input_batch.rows[1].values[0].encoded_value =
      "malformed";
  result = exec::ExecuteCanonicalInt64SumFilter(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "FALSE row hid malformed typed aggregate input");

  request = Request();
  request.aggregate_request.maximum_transition_count = 4;
  result = exec::ExecuteCanonicalInt64SumFilter(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "FILTER scan resource bound was exceeded");

  request = Request();
  request.aggregate_request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalInt64SumFilter(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "aggregate FILTER bypassed MGA physical admission");
  return passed;
}

}  // namespace

int main() {
  return ValidateAggregateFilter() ? EXIT_SUCCESS : EXIT_FAILURE;
}
