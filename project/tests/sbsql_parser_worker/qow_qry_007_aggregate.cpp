// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-007-AGGREGATE-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& descriptor_uuid,
                                 const std::string& type_name,
                                 const std::string& type_uuid,
                                 const std::string& nullability) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type_name;
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=" + nullability;
  return descriptor;
}

exec::CanonicalDescriptorCountRequest Request() {
  const auto input_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000007301", "decimal",
      "019f0000-0000-7300-8000-000000007302", "nullable");
  const auto count_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000007303", "int64",
      "019f0000-0000-7300-8000-000000007304", "non_null");
  api::EngineTypedValue value;
  value.descriptor = input_descriptor;
  value.encoded_value = "3.00";
  api::EngineTypedValue null_value;
  null_value.descriptor = input_descriptor;
  null_value.is_null = true;
  null_value.state = api::EngineValueState::sql_null;

  exec::CanonicalDescriptorCountRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000007305";
  request.physical_dag.root_physical_node_id = 732;
  request.physical_dag.local_transaction_id = 733;
  request.physical_dag.statement_snapshot_id = 734;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000007311"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000007312"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000007313"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000007314"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000007315"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000007316"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000007317"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000007318"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 731,
       .relational_node_id = 731,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {731},
       .causal_counter_id = 7301},
      {.physical_node_id = 732,
       .relational_node_id = 732,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.count-star.v1",
       .input_physical_node_ids = {731},
       .output_descriptor_ids = {732},
       .causal_counter_id = 7302},
  };
  request.selected_physical_node_id = 732;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"amount", input_descriptor, true, 731}},
      {{{value}}, {{null_value}}, {{value}}});
  request.count_column = {"row_count", count_descriptor, false, 732};
  return request;
}

// QOW-TEST-QRY-007-AGGREGATE-V1
bool ValidatePhysicalCountStar() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalDescriptorCountStar(Request());
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 732 &&
                        result.causal_counter_id == 7302,
                    "typed physical aggregate node was not executable");
  passed &= Require(result.output_batch.rows.size() == 1 &&
                        result.output_batch.rows[0].values[0].encoded_value ==
                            "3" &&
                        result.output_batch.rows[0].values[0].state ==
                            api::EngineValueState::value,
                    "COUNT(*) did not count physical rows including NULL");
  passed &= Require(!result.output_batch.columns[0].nullable &&
                        result.output_batch.columns[0].descriptor_id == 732,
                    "COUNT(*) lost its bound non-null result descriptor");

  auto request = Request();
  request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalDescriptorCountStar(request);
  passed &= Require(result.diagnostic.ok &&
                        result.output_batch.rows.size() == 1 &&
                        result.output_batch.rows[0].values[0].encoded_value ==
                            "0",
                    "COUNT(*) on empty input did not return one zero row");

  request = Request();
  request.count_column.nullable = true;
  result = exec::ExecuteCanonicalDescriptorCountStar(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "nullable COUNT(*) descriptor was silently accepted");

  request = Request();
  request.count_column.descriptor_id = 999;
  result = exec::ExecuteCanonicalDescriptorCountStar(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "unresolved aggregate result handle produced data");
  return passed;
}

}  // namespace

int main() {
  if (!ValidatePhysicalCountStar()) return 1;
  std::cout << "QOW-TEST-QRY-007-AGGREGATE-V1: PASS\n";
  return 0;
}
