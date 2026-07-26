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
    std::cerr << "QOW-TEST-QRY-014-RESOURCE-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000003501";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000003502;"
      "nullability=not-null";
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            const std::int64_t value) {
  api::EngineTypedValue typed;
  typed.descriptor = descriptor;
  typed.encoded_value = std::to_string(value);
  typed.state = api::EngineValueState::value;
  return typed;
}

exec::CanonicalRecursiveCteResourceRequest Request() {
  const auto descriptor = Descriptor();
  exec::CanonicalRecursiveCteResourceRequest request;
  auto& working = request.working_request;
  working.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000003503";
  working.physical_dag.root_physical_node_id = 3503;
  working.physical_dag.local_transaction_id = 3504;
  working.physical_dag.statement_snapshot_id = 3505;
  working.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000003511"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000003512"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000003513"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000003514"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000003515"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000003516"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000003517"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000003518"},
  };
  working.physical_dag.nodes = {
      {.physical_node_id = 3501,
       .relational_node_id = 3501,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.cte-resource-anchor.typed.v1",
       .output_descriptor_ids = {3501},
       .causal_counter_id = 35001},
      {.physical_node_id = 3502,
       .relational_node_id = 3502,
       .node_kind = exec::PhysicalNodeKind::kCte,
       .implementation_id = "cte.resource-term.typed.v1",
       .output_descriptor_ids = {3501},
       .causal_counter_id = 35002},
      {.physical_node_id = 3503,
       .relational_node_id = 3503,
       .node_kind = exec::PhysicalNodeKind::kRecursiveCte,
       .implementation_id = "cte.recursive.resource-bounded.typed.v1",
       .input_physical_node_ids = {3501, 3502},
       .output_descriptor_ids = {3501},
       .causal_counter_id = 35003},
  };
  working.selected_physical_node_id = 3503;
  working.anchor_batch = exec::MakeDescriptorBatch(
      {{"n", descriptor, false, 3501}}, {{{Value(descriptor, 1)}}});
  working.recursive_step =
      [descriptor](const exec::DescriptorBatch& current, const std::size_t) {
        exec::DescriptorBatch next;
        next.columns = current.columns;
        for (const auto& row : current.rows) {
          const auto value = std::stoll(row.values[0].encoded_value);
          if (value < 4) next.rows.push_back({{Value(descriptor, value + 1)}});
        }
        return next;
      };
  working.maximum_iteration_count = 8;
  working.maximum_working_row_count = 4;
  working.maximum_result_row_count = 8;
  request.memory_grant_evidence_uuid =
      "019f0000-0000-7200-8000-000000003516";
  request.maximum_materialized_value_bytes = 4;
  return request;
}

// QOW-TEST-QRY-014-RESOURCE-V1
bool ValidateResourceBoundary() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalRecursiveCteResource(Request());
  passed &= Require(
      result.working_result.diagnostic.ok &&
          result.working_result.output_batch.rows.size() == 4 &&
          result.materialized_value_bytes == 4 &&
          result.working_state_cleaned &&
          result.memory_grant_evidence_uuid ==
              "019f0000-0000-7200-8000-000000003516" &&
          result.working_result.executed_physical_node_id == 3503,
      "recursive CTE did not charge the exact encoded-value grant");

  auto request = Request();
  request.maximum_materialized_value_bytes = 3;
  result = exec::ExecuteCanonicalRecursiveCteResource(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_result.output_batch.rows.empty() &&
                        result.materialized_value_bytes == 0 &&
                        result.working_state_cleaned,
                    "byte-grant excess published or leaked working state");

  request = Request();
  request.maximum_materialized_value_bytes = 0;
  result = exec::ExecuteCanonicalRecursiveCteResource(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_state_cleaned,
                    "zero recursive memory grant was accepted");

  request = Request();
  request.memory_grant_evidence_uuid =
      "019f0000-0000-7200-8000-000000003599";
  result = exec::ExecuteCanonicalRecursiveCteResource(request);
  passed &= Require(!result.working_result.diagnostic.ok,
                    "resource evidence drift was accepted");

  request = Request();
  request.working_request.physical_dag.nodes[2].implementation_id =
      "cte.recursive.working.typed.v1";
  result = exec::ExecuteCanonicalRecursiveCteResource(request);
  passed &= Require(!result.working_result.diagnostic.ok,
                    "unbounded working profile bypassed resource admission");

  request = Request();
  request.working_request.physical_dag.statement_snapshot_id = 0;
  result = exec::ExecuteCanonicalRecursiveCteResource(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_state_cleaned,
                    "resource route accepted missing MGA snapshot");
  return passed;
}

}  // namespace

int main() { return ValidateResourceBoundary() ? EXIT_SUCCESS : EXIT_FAILURE; }
