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
    std::cerr << "QOW-TEST-QRY-013-ROW-V1: " << detail << '\n';
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

exec::CanonicalRowSubqueryRequest Request() {
  const auto key = Descriptor(
      "019f0000-0000-7200-8000-000000002701",
      "019f0000-0000-7300-8000-000000002702", "int64");
  const auto payload = Descriptor(
      "019f0000-0000-7200-8000-000000002703",
      "019f0000-0000-7300-8000-000000002704", "text");

  exec::CanonicalRowSubqueryRequest request;
  auto& table = request.table_request;
  table.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000002705";
  table.physical_dag.root_physical_node_id = 2702;
  table.physical_dag.local_transaction_id = 2703;
  table.physical_dag.statement_snapshot_id = 2704;
  table.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000002711"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000002712"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000002713"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000002714"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000002715"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000002716"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000002717"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000002718"},
  };
  table.physical_dag.nodes = {
      {.physical_node_id = 2701,
       .relational_node_id = 2701,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.row-subquery-input.typed.v1",
       .output_descriptor_ids = {2701, 2702},
       .causal_counter_id = 27001},
      {.physical_node_id = 2702,
       .relational_node_id = 2702,
       .node_kind = exec::PhysicalNodeKind::kSubquery,
       .implementation_id = "subquery.row.cardinality.typed.v1",
       .input_physical_node_ids = {2701},
       .output_descriptor_ids = {2701, 2702},
       .causal_counter_id = 27002},
  };
  table.selected_physical_node_id = 2702;
  table.input_batch = exec::MakeDescriptorBatch(
      {{"row_key_source", key, false, 2701},
       {"row_payload_source", payload, true, 2702}},
      {{{Value(key, "07"), Null(payload)}}});
  table.maximum_materialized_row_count = 2;
  request.row_expression_descriptor_ids = {2701, 2702};
  request.result_columns = {
      {"row_key_result", key, true, 2701},
      {"row_payload_result", payload, true, 2702},
  };
  return request;
}

// QOW-TEST-QRY-013-ROW-V1
bool ValidateRowSubquery() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalRowSubquery(Request());
  passed &= Require(
      result.diagnostic.ok && result.source_row_count == 1 &&
          result.output_batch.columns.size() == 2 &&
          result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[0].encoded_value == "07" &&
          result.output_batch.rows[0].values[1].state ==
              api::EngineValueState::sql_null &&
          result.selected_plan_uuid ==
              "019f0000-0000-7200-8000-000000002705" &&
          result.executed_physical_node_id == 2702 &&
          result.causal_counter_id == 27002,
      "one-row subquery did not preserve every typed field");

  auto request = Request();
  request.table_request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalRowSubquery(request);
  passed &= Require(
      result.diagnostic.ok && result.source_row_count == 0 &&
          result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values.size() == 2 &&
          result.output_batch.rows[0].values[0].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[0].values[1].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[0].values[0].is_null &&
          result.output_batch.rows[0].values[1].is_null,
      "zero-row subquery did not produce one typed all-NULL row");

  request = Request();
  request.table_request.input_batch.rows.push_back(
      {{Value(request.table_request.input_batch.columns[0].descriptor, "8"),
        Value(request.table_request.input_batch.columns[1].descriptor,
              "second")}});
  result = exec::ExecuteCanonicalRowSubquery(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-013-ROW-REFUSAL-V1" &&
          result.output_batch.rows.empty() && result.source_row_count == 0 &&
          result.selected_plan_uuid.empty(),
      "many-row subquery published a first-row substitute");

  request = Request();
  request.row_expression_descriptor_ids.pop_back();
  result = exec::ExecuteCanonicalRowSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "partially bound row expression was accepted");

  request = Request();
  request.row_expression_descriptor_ids = {2702, 2701};
  result = exec::ExecuteCanonicalRowSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "reordered row expression handles were accepted");

  request = Request();
  request.result_columns[0].nullable = false;
  result = exec::ExecuteCanonicalRowSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "non-nullable zero-row result field was accepted");

  request = Request();
  request.result_columns[1].descriptor.descriptor_uuid.canonical =
      request.result_columns[0].descriptor.descriptor_uuid.canonical;
  result = exec::ExecuteCanonicalRowSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "row result descriptor drift was accepted");

  request = Request();
  request.table_request.input_batch.rows[0].values.pop_back();
  result = exec::ExecuteCanonicalRowSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "ragged table input published a row value");

  request = Request();
  request.table_request.maximum_materialized_row_count = 0;
  result = exec::ExecuteCanonicalRowSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "failed table resource admission published a row value");

  request = Request();
  request.table_request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalRowSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "missing engine MGA transaction was accepted");
  return passed;
}

}  // namespace

int main() {
  return ValidateRowSubquery() ? EXIT_SUCCESS : EXIT_FAILURE;
}
