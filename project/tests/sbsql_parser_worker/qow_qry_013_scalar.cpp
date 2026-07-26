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
    std::cerr << "QOW-TEST-QRY-013-SCALAR-V1: " << detail << '\n';
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

exec::CanonicalScalarSubqueryRequest Request() {
  const auto scalar = Descriptor(
      "019f0000-0000-7200-8000-000000002601",
      "019f0000-0000-7300-8000-000000002602");

  exec::CanonicalScalarSubqueryRequest request;
  auto& table = request.table_request;
  table.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000002603";
  table.physical_dag.root_physical_node_id = 2602;
  table.physical_dag.local_transaction_id = 2603;
  table.physical_dag.statement_snapshot_id = 2604;
  table.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000002611"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000002612"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000002613"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000002614"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000002615"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000002616"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000002617"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000002618"},
  };
  table.physical_dag.nodes = {
      {.physical_node_id = 2601,
       .relational_node_id = 2601,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.scalar-subquery-input.typed.v1",
       .output_descriptor_ids = {2601},
       .causal_counter_id = 26001},
      {.physical_node_id = 2602,
       .relational_node_id = 2602,
       .node_kind = exec::PhysicalNodeKind::kSubquery,
       .implementation_id = "subquery.scalar.cardinality.typed.v1",
       .input_physical_node_ids = {2601},
       .output_descriptor_ids = {2601},
       .causal_counter_id = 26002},
  };
  table.selected_physical_node_id = 2602;
  table.input_batch = exec::MakeDescriptorBatch(
      {{"scalar_source", scalar, true, 2601}},
      {{{Value(scalar, "02")}}});
  table.maximum_materialized_row_count = 2;
  request.value_expression_descriptor_id = 2601;
  request.result_column = {"scalar_result", scalar, true, 2601};
  return request;
}

// QOW-TEST-QRY-013-SCALAR-V1
bool ValidateScalarSubquery() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalScalarSubquery(Request());
  passed &= Require(
      result.diagnostic.ok && result.source_row_count == 1 &&
          result.output_batch.columns.size() == 1 &&
          result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[0].state ==
              api::EngineValueState::value &&
          result.output_batch.rows[0].values[0].encoded_value == "02" &&
          result.selected_plan_uuid ==
              "019f0000-0000-7200-8000-000000002603" &&
          result.executed_physical_node_id == 2602 &&
          result.causal_counter_id == 26002,
      "one-row scalar subquery did not preserve its typed value");

  auto request = Request();
  request.table_request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(
      result.diagnostic.ok && result.source_row_count == 0 &&
          result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[0].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[0].values[0].is_null &&
          result.output_batch.rows[0].values[0].encoded_value.empty(),
      "zero-row scalar subquery did not produce one typed SQL NULL");

  request = Request();
  request.table_request.input_batch.rows[0].values[0] =
      Null(request.table_request.input_batch.columns[0].descriptor);
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(
      result.diagnostic.ok && result.source_row_count == 1 &&
          result.output_batch.rows[0].values[0].state ==
              api::EngineValueState::sql_null,
      "one-row SQL NULL scalar subquery lost its canonical NULL state");

  request = Request();
  request.table_request.input_batch.rows.push_back(
      request.table_request.input_batch.rows.front());
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-013-SCALAR-REFUSAL-V1" &&
          result.output_batch.rows.empty() && result.source_row_count == 0 &&
          result.selected_plan_uuid.empty() &&
          result.executed_physical_node_id == 0,
      "many-row scalar subquery published a first-row substitute");

  request = Request();
  request.result_column.nullable = false;
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "non-nullable scalar result admitted the zero-row case");

  request = Request();
  request.value_expression_descriptor_id = 2699;
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "unresolved scalar expression handle was accepted");

  request = Request();
  request.result_column.descriptor.encoded_descriptor += ";drift=true";
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "scalar result descriptor drift was accepted");

  request = Request();
  const auto extra = Descriptor(
      "019f0000-0000-7200-8000-000000002621",
      "019f0000-0000-7300-8000-000000002622");
  request.table_request.physical_dag.nodes[0].output_descriptor_ids.push_back(
      2602);
  request.table_request.physical_dag.nodes[1].output_descriptor_ids.push_back(
      2602);
  request.table_request.input_batch.columns.push_back(
      {"extra", extra, true, 2602});
  request.table_request.input_batch.rows[0].values.push_back(Value(extra, "9"));
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "multi-column scalar subquery was accepted");

  request = Request();
  request.table_request.maximum_materialized_row_count = 0;
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "failed table materialization published a scalar");

  request = Request();
  request.table_request.physical_dag.statement_snapshot_id = 0;
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "missing engine MGA snapshot was accepted");
  return passed;
}

}  // namespace

int main() {
  return ValidateScalarSubquery() ? EXIT_SUCCESS : EXIT_FAILURE;
}
