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
  if (!condition) std::cerr << "QOW-TEST-QRY-011-EMPTY-V1: " << detail << '\n';
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

exec::CanonicalInt64SumStateRequest StateRequest(
    const std::vector<api::EngineTypedValue>& values) {
  const auto value_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001301",
      "019f0000-0000-7300-8000-000000001302");
  const auto result_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001303",
      "019f0000-0000-7300-8000-000000001304");

  exec::CanonicalInt64SumStateRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001305";
  request.physical_dag.root_physical_node_id = 1302;
  request.physical_dag.local_transaction_id = 1303;
  request.physical_dag.statement_snapshot_id = 1304;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001311"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001312"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001313"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001314"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001315"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001316"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001317"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001318"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 1301,
       .relational_node_id = 1301,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {1301},
       .causal_counter_id = 13001},
      {.physical_node_id = 1302,
       .relational_node_id = 1302,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.sum-int64-final.v1",
       .input_physical_node_ids = {1301},
       .output_descriptor_ids = {1302},
       .causal_counter_id = 13002},
  };
  request.selected_physical_node_id = 1302;
  std::vector<exec::DescriptorTuple> rows;
  for (const auto& value : values) rows.push_back({{value}});
  request.input_batch = exec::MakeDescriptorBatch(
      {{"amount", value_descriptor, true, 1301}}, std::move(rows));
  request.value_expression_descriptor_id = 1301;
  request.result_column = {"sum_amount", result_descriptor, true, 1302};
  return request;
}

exec::CanonicalInt64SumFinalizeResult Finalize(
    const exec::CanonicalInt64SumStateRequest& state_request,
    exec::CanonicalInt64SumAggregateState state) {
  exec::CanonicalInt64SumFinalizeRequest request;
  request.physical_dag = state_request.physical_dag;
  request.selected_physical_node_id = state_request.selected_physical_node_id;
  request.state = std::move(state);
  return exec::ExecuteCanonicalInt64SumFinalize(request);
}

// QOW-TEST-QRY-011-EMPTY-V1
bool ValidateAggregateEmptyFinalization() {
  bool passed = true;
  auto state_request = StateRequest({});
  auto state_result = exec::ExecuteCanonicalInt64SumState(state_request);
  auto result = Finalize(state_request, state_result.state);
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 1302 &&
                        result.output_batch.rows.size() == 1,
                    "empty SUM state did not emit one aggregate row");
  const auto& empty_value = result.output_batch.rows[0].values[0];
  passed &= Require(empty_value.state == api::EngineValueState::sql_null &&
                        empty_value.is_null && empty_value.encoded_value.empty() &&
                        empty_value.binary_value.empty() &&
                        result.output_batch.columns[0].descriptor_id == 1302,
                    "empty SUM did not retain canonical typed SQL NULL");

  const auto descriptor = state_request.input_batch.columns[0].descriptor;
  state_request = StateRequest({Null(descriptor)});
  state_result = exec::ExecuteCanonicalInt64SumState(state_request);
  result = Finalize(state_request, state_result.state);
  passed &= Require(result.diagnostic.ok &&
                        result.output_batch.rows[0].values[0].state ==
                            api::EngineValueState::sql_null,
                    "all-NULL SUM did not finalize as SQL NULL");

  state_request = StateRequest({Value(descriptor, "0")});
  state_result = exec::ExecuteCanonicalInt64SumState(state_request);
  result = Finalize(state_request, state_result.state);
  passed &= Require(result.diagnostic.ok &&
                        result.output_batch.rows[0].values[0].state ==
                            api::EngineValueState::value &&
                        result.output_batch.rows[0].values[0].encoded_value ==
                            "0",
                    "real aggregate zero was confused with empty input");

  state_request =
      StateRequest({Value(descriptor, "10"), Null(descriptor),
                    Value(descriptor, "-3")});
  state_result = exec::ExecuteCanonicalInt64SumState(state_request);
  result = Finalize(state_request, state_result.state);
  passed &= Require(result.diagnostic.ok &&
                        result.output_batch.rows[0].values[0].encoded_value ==
                            "7",
                    "nonempty SUM state finalized to the wrong value");

  auto malformed_state = state_result.state;
  malformed_state.has_value = false;
  result = Finalize(state_request, malformed_state);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "contradictory SUM state was finalized");

  malformed_state = state_result.state;
  malformed_state.result_column.nullable = false;
  result = Finalize(state_request, malformed_state);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "non-nullable empty SUM descriptor was accepted");

  malformed_state = state_result.state;
  malformed_state.result_column.descriptor_id = 1399;
  result = Finalize(state_request, malformed_state);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "mismatched SUM output handle was finalized");

  auto final_request = exec::CanonicalInt64SumFinalizeRequest{
      state_request.physical_dag, state_request.selected_physical_node_id,
      state_result.state};
  final_request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalInt64SumFinalize(final_request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "SUM finalization bypassed MGA physical admission");
  return passed;
}

}  // namespace

int main() {
  return ValidateAggregateEmptyFinalization() ? EXIT_SUCCESS : EXIT_FAILURE;
}
