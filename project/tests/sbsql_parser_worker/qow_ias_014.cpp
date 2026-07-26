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
  if (!condition) std::cerr << "QOW-TEST-IAS-014-V1: " << detail << '\n';
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "decimal";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000014001;"
      "nullability=nullable;precision=12;scale=2";
  return descriptor;
}

exec::TypedPhysicalNodeDag Dag(const std::string& strategy) {
  exec::TypedPhysicalNodeDag dag;
  dag.selected_plan_uuid = "019f0000-0000-7200-8000-000000014002";
  dag.root_physical_node_id = 1402;
  dag.local_transaction_id = 1403;
  dag.statement_snapshot_id = 1404;
  dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000014011"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000014012"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000014013"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000014014"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000014015"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000014016"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000014017"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000014018"},
  };
  dag.nodes = {
      {.physical_node_id = 1401,
       .relational_node_id = 141,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {141},
       .causal_counter_id = 14001},
      {.physical_node_id = 1402,
       .relational_node_id = 142,
       .node_kind = exec::PhysicalNodeKind::kProject,
       .implementation_id = strategy,
       .input_physical_node_ids = {1401},
       .output_descriptor_ids = {141},
       .causal_counter_id = 14002},
  };
  return dag;
}

exec::CanonicalDescriptorProjectionRequest Request(
    const std::string& strategy) {
  const auto descriptor =
      Descriptor("019f0000-0000-7200-8000-000000014021");
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = "14.00";
  value.state = api::EngineValueState::value;

  exec::CanonicalDescriptorProjectionRequest request;
  request.physical_dag = Dag(strategy);
  request.selected_physical_node_id = 1402;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"amount", descriptor, true, 141}}, {{{value}}});
  request.projected_columns = {0};
  return request;
}

// QOW-TEST-IAS-014-V1
bool ValidateCanonicalAdoption() {
  bool passed = true;
  const auto row = exec::ExecuteCanonicalDescriptorProjection(
      Request("project.typed.row.v1"));
  const auto vector = exec::ExecuteCanonicalDescriptorProjection(
      Request("project.typed.vector.v1"));
  passed &= Require(row.diagnostic.ok && vector.diagnostic.ok,
                    "canonical physical strategies did not reach helper");
  passed &= Require(row.executed_physical_node_id == 1402 &&
                        row.causal_counter_id == 14002 &&
                        row.output_batch.rows.size() == 1 &&
                        row.output_batch.rows[0].values[0].encoded_value ==
                            "14.00",
                    "physical execution evidence or typed data was lost");
  passed &= Require(
      vector.output_batch.rows[0].values[0].encoded_value ==
              row.output_batch.rows[0].values[0].encoded_value &&
          vector.output_batch.rows[0]
                  .values[0]
                  .descriptor.descriptor_uuid.canonical ==
              row.output_batch.rows[0]
                  .values[0]
                  .descriptor.descriptor_uuid.canonical,
      "forced strategies diverged in value or descriptor identity");

  const auto direct = exec::ExecuteCanonicalDescriptorProjection({});
  passed &= Require(!direct.diagnostic.ok &&
                        direct.output_batch.columns.empty() &&
                        direct.output_batch.rows.empty(),
                    "plan-free request reached descriptor semantics");

  auto invalid = Request("project.typed.row.v1");
  invalid.physical_dag.admission_evidence.pop_back();
  const auto refused = exec::ExecuteCanonicalDescriptorProjection(invalid);
  passed &= Require(!refused.diagnostic.ok &&
                        refused.output_batch.rows.empty(),
                    "incomplete canonical admission reached helper");
  return passed;
}

}  // namespace

int main() {
  if (!ValidateCanonicalAdoption()) return 1;
  std::cout << "QOW-TEST-IAS-014-V1: PASS\n";
  return 0;
}
