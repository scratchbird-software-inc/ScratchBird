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
    std::cerr << "QOW-TEST-QRY-007-PROJECTION-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& descriptor_uuid,
                                 const std::string& type_uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "decimal";
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=nullable;precision=12;scale=2";
  return descriptor;
}

exec::CanonicalDescriptorProjectionRequest Request() {
  const auto first = Descriptor(
      "019f0000-0000-7200-8000-000000007011",
      "019f0000-0000-7300-8000-000000007012");
  const auto second = Descriptor(
      "019f0000-0000-7200-8000-000000007013",
      "019f0000-0000-7300-8000-000000007014");
  api::EngineTypedValue first_value;
  first_value.descriptor = first;
  first_value.encoded_value = "7.00";
  api::EngineTypedValue second_null;
  second_null.descriptor = second;
  second_null.is_null = true;
  second_null.state = api::EngineValueState::sql_null;

  exec::CanonicalDescriptorProjectionRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000007001";
  request.physical_dag.root_physical_node_id = 702;
  request.physical_dag.local_transaction_id = 703;
  request.physical_dag.statement_snapshot_id = 704;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000007021"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000007022"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000007023"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000007024"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000007025"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000007026"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000007027"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000007028"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 701,
       .relational_node_id = 71,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {71, 72},
       .causal_counter_id = 7001},
      {.physical_node_id = 702,
       .relational_node_id = 72,
       .node_kind = exec::PhysicalNodeKind::kProject,
       .implementation_id = "project.typed.row.v1",
       .input_physical_node_ids = {701},
       .output_descriptor_ids = {72, 71},
       .causal_counter_id = 7002},
  };
  request.selected_physical_node_id = 702;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"first", first, true, 71}, {"second", second, true, 72}},
      {{{first_value, second_null}}});
  request.projected_columns = {1, 0};
  return request;
}

// QOW-TEST-QRY-007-PROJECTION-V1
bool ValidatePhysicalProjection() {
  bool passed = true;
  const auto result = exec::ExecuteCanonicalDescriptorProjection(Request());
  passed &= Require(result.diagnostic.ok,
                    "typed physical project node was not executable");
  passed &= Require(result.executed_physical_node_id == 702 &&
                        result.causal_counter_id == 7002,
                    "executed node identity was not retained");
  passed &= Require(result.output_batch.columns.size() == 2 &&
                        result.output_batch.columns[0].descriptor_id == 72 &&
                        result.output_batch.columns[1].descriptor_id == 71,
                    "project output handles were not applied in bound order");
  passed &= Require(
      result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[0].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[0].values[0].encoded_value.empty() &&
          result.output_batch.rows[0].values[1].encoded_value == "7.00",
      "typed NULL or scalar value changed during projection");

  auto invalid = Request();
  invalid.physical_dag.nodes.back().node_kind =
      exec::PhysicalNodeKind::kSort;
  const auto refused = exec::ExecuteCanonicalDescriptorProjection(invalid);
  passed &= Require(!refused.diagnostic.ok &&
                        refused.output_batch.rows.empty(),
                    "source-layout-only non-project route produced data");
  return passed;
}

}  // namespace

int main() {
  if (!ValidatePhysicalProjection()) return 1;
  std::cout << "QOW-TEST-QRY-007-PROJECTION-V1: PASS\n";
  return 0;
}
