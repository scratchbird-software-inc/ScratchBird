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
    std::cerr << "QOW-TEST-QRY-013-TABLE-V1: " << detail << '\n';
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

exec::CanonicalTableSubqueryRequest Request() {
  const auto key = Descriptor(
      "019f0000-0000-7200-8000-000000002501",
      "019f0000-0000-7300-8000-000000002502", "int64");
  const auto payload = Descriptor(
      "019f0000-0000-7200-8000-000000002503",
      "019f0000-0000-7300-8000-000000002504", "text");

  exec::CanonicalTableSubqueryRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000002505";
  request.physical_dag.root_physical_node_id = 2502;
  request.physical_dag.local_transaction_id = 2503;
  request.physical_dag.statement_snapshot_id = 2504;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000002511"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000002512"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000002513"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000002514"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000002515"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000002516"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000002517"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000002518"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 2501,
       .relational_node_id = 2501,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.table-subquery-input.typed.v1",
       .output_descriptor_ids = {2501, 2502},
       .causal_counter_id = 25001},
      {.physical_node_id = 2502,
       .relational_node_id = 2502,
       .node_kind = exec::PhysicalNodeKind::kSubquery,
       .implementation_id = "subquery.table.materialize.typed.v1",
       .input_physical_node_ids = {2501},
       .output_descriptor_ids = {2501, 2502},
       .causal_counter_id = 25002},
  };
  request.selected_physical_node_id = 2502;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"table_key", key, false, 2501},
       {"table_payload", payload, true, 2502}},
      {{{Value(key, "1"), Value(payload, "alpha")}},
       {{Value(key, "02"), Null(payload)}},
       {{Value(key, "3"), Value(payload, "omega")}}});
  request.maximum_materialized_row_count = 3;
  return request;
}

// QOW-TEST-QRY-013-TABLE-V1
bool ValidateTableSubquery() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalTableSubquery(Request());
  passed &= Require(
      result.diagnostic.ok && result.materialized_row_count == 3 &&
          result.output_batch.columns.size() == 2 &&
          result.output_batch.rows.size() == 3 &&
          result.output_batch.rows[1].values[0].encoded_value == "02" &&
          result.output_batch.rows[1].values[1].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[1].values[1].is_null &&
          result.selected_plan_uuid ==
              "019f0000-0000-7200-8000-000000002505" &&
          result.executed_physical_node_id == 2502 &&
          result.causal_counter_id == 25002,
      "canonical table subquery did not preserve its typed relational result");

  auto request = Request();
  request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(result.diagnostic.ok &&
                        result.materialized_row_count == 0 &&
                        result.output_batch.columns.size() == 2 &&
                        result.output_batch.rows.empty(),
                    "empty table subquery lost its canonical schema");

  request = Request();
  request.maximum_materialized_row_count = 2;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "QOW-DIAG-QRY-013-TABLE-REFUSAL-V1" &&
                        result.output_batch.rows.empty() &&
                        result.materialized_row_count == 0,
                    "table-subquery row bound published partial output");

  request = Request();
  request.maximum_materialized_row_count = 0;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "zero table-subquery row bound was accepted");

  request = Request();
  request.selected_physical_node_id = 2501;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "non-root table-subquery selection was accepted");

  request = Request();
  request.physical_dag.nodes[1].node_kind =
      exec::PhysicalNodeKind::kProject;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "non-subquery physical route was accepted");

  request = Request();
  request.physical_dag.nodes[1].output_descriptor_ids[1] = 2599;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "table-subquery output descriptor drift was accepted");

  request = Request();
  request.input_batch.rows[0].values[0].descriptor =
      request.input_batch.columns[1].descriptor;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "typed value descriptor drift was accepted");

  request = Request();
  request.input_batch.rows[1].values[1].is_null = false;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "malformed SQL NULL was accepted");

  request = Request();
  request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "missing engine MGA statement context was accepted");
  return passed;
}

}  // namespace

int main() {
  return ValidateTableSubquery() ? EXIT_SUCCESS : EXIT_FAILURE;
}
