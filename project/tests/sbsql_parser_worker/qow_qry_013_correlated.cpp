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
    std::cerr << "QOW-TEST-QRY-013-CORRELATED-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& descriptor_uuid,
                                 const std::string& type_uuid,
                                 const std::string& type_name) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type_name;
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

exec::CanonicalCorrelatedSubqueryRequest Request() {
  const auto outer_key = Descriptor(
      "019f0000-0000-7200-8000-000000003001",
      "019f0000-0000-7300-8000-000000003002", "int64");
  const auto outer_payload = Descriptor(
      "019f0000-0000-7200-8000-000000003003",
      "019f0000-0000-7300-8000-000000003004", "text");
  const auto inner_key = Descriptor(
      "019f0000-0000-7200-8000-000000003005",
      "019f0000-0000-7300-8000-000000003006", "int64");
  const auto inner_payload = Descriptor(
      "019f0000-0000-7200-8000-000000003007",
      "019f0000-0000-7300-8000-000000003008", "text");

  exec::CanonicalCorrelatedSubqueryRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000003009";
  request.physical_dag.root_physical_node_id = 3003;
  request.physical_dag.local_transaction_id = 3004;
  request.physical_dag.statement_snapshot_id = 3005;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000003011"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000003012"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000003013"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000003014"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000003015"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000003016"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000003017"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000003018"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 3001,
       .relational_node_id = 3001,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.correlated-outer.typed.v1",
       .output_descriptor_ids = {3001, 3002},
       .causal_counter_id = 30001},
      {.physical_node_id = 3002,
       .relational_node_id = 3002,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.correlated-inner.typed.v1",
       .output_descriptor_ids = {3003, 3004},
       .causal_counter_id = 30002},
      {.physical_node_id = 3003,
       .relational_node_id = 3003,
       .node_kind = exec::PhysicalNodeKind::kSubquery,
       .implementation_id = "subquery.correlated.int64-equality.typed.v1",
       .input_physical_node_ids = {3001, 3002},
       .output_descriptor_ids = {3003, 3004},
       .causal_counter_id = 30003},
  };
  request.selected_physical_node_id = 3003;
  request.outer_batch = exec::MakeDescriptorBatch(
      {{"outer_key", outer_key, true, 3001},
       {"outer_payload", outer_payload, false, 3002}},
      {{{Value(outer_key, "1"), Value(outer_payload, "outer-a")}},
       {{Null(outer_key), Value(outer_payload, "outer-null")}},
       {{Value(outer_key, "2"), Value(outer_payload, "outer-b")}},
       {{Value(outer_key, "01"), Value(outer_payload, "outer-alias")}}});
  request.inner_batch = exec::MakeDescriptorBatch(
      {{"inner_key", inner_key, true, 3003},
       {"inner_payload", inner_payload, false, 3004}},
      {{{Value(inner_key, "01"), Value(inner_payload, "inner-a")}},
       {{Value(inner_key, "2"), Value(inner_payload, "inner-b")}},
       {{Value(inner_key, "1"), Value(inner_payload, "inner-c")}},
       {{Null(inner_key), Value(inner_payload, "inner-null")}}});
  request.outer_binding_column = 0;
  request.outer_binding_expression_descriptor_id = 3001;
  request.inner_reference_column = 0;
  request.inner_reference_expression_descriptor_id = 3003;
  request.maximum_scope_execution_count = 4;
  request.maximum_comparison_count = 16;
  request.maximum_result_row_count = 5;
  return request;
}

// QOW-TEST-QRY-013-CORRELATED-V1
bool ValidateCorrelatedSubquery() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalCorrelatedSubquery(Request());
  passed &= Require(
      result.diagnostic.ok && result.scope_execution_count == 4 &&
          result.comparison_count == 16 && result.result_row_count == 5 &&
          result.scopes.size() == 4 &&
          result.scopes[0].outer_row_index == 0 &&
          result.scopes[0].bound_outer_value.encoded_value == "1" &&
          result.scopes[0].output_batch.rows.size() == 2 &&
          result.scopes[0].output_batch.rows[0].values[1].encoded_value ==
              "inner-a" &&
          result.scopes[0].output_batch.rows[1].values[1].encoded_value ==
              "inner-c" &&
          result.scopes[1].output_batch.rows.empty() &&
          result.scopes[2].output_batch.rows.size() == 1 &&
          result.scopes[3].output_batch.rows.size() == 2 &&
          result.selected_plan_uuid ==
              "019f0000-0000-7200-8000-000000003009" &&
          result.executed_physical_node_id == 3003 &&
          result.causal_counter_id == 30003,
      "correlated scopes did not bind and execute in outer-row order");

  auto request = Request();
  request.inner_batch.rows.clear();
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(result.diagnostic.ok &&
                        result.scope_execution_count == 4 &&
                        result.comparison_count == 0 &&
                        result.result_row_count == 0 &&
                        result.scopes.size() == 4 &&
                        result.scopes[0].output_batch.columns.size() == 2 &&
                        result.scopes[0].output_batch.rows.empty(),
                    "empty inner relation lost per-outer typed scopes");

  request = Request();
  request.outer_batch.rows.clear();
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(result.diagnostic.ok && result.scopes.empty() &&
                        result.scope_execution_count == 0 &&
                        result.comparison_count == 0,
                    "empty outer relation invented correlated scopes");

  request = Request();
  request.inner_batch.rows[2].values[0].encoded_value = "bad";
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.scopes.empty() &&
                        result.scope_execution_count == 0 &&
                        result.result_row_count == 0,
                    "malformed later inner key published earlier scopes");

  request = Request();
  request.maximum_scope_execution_count = 3;
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "correlated scope-execution bound was exceeded");

  request = Request();
  request.maximum_comparison_count = 15;
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "correlated comparison bound was exceeded");

  request = Request();
  request.maximum_result_row_count = 4;
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.scopes.empty(),
                    "correlated result bound published partial scopes");

  request = Request();
  request.outer_binding_expression_descriptor_id = 3099;
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "outer correlated binding handle drift was accepted");

  request = Request();
  request.physical_dag.nodes[2].implementation_id =
      "subquery.table.materialize.typed.v1";
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "wrong correlated physical profile was accepted");

  request = Request();
  request.physical_dag.nodes[2].output_descriptor_ids = {3001, 3002};
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "correlated output descriptor drift was accepted");

  request = Request();
  request.physical_dag.statement_snapshot_id = 0;
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "missing engine MGA snapshot was accepted");
  return passed;
}

}  // namespace

int main() {
  return ValidateCorrelatedSubquery() ? EXIT_SUCCESS : EXIT_FAILURE;
}
