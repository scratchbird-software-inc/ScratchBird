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
    std::cerr << "QOW-TEST-QRY-014-WORKING-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Int64Descriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000003201";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000003202;"
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

exec::CanonicalRecursiveCteWorkingRequest Request() {
  const auto descriptor = Int64Descriptor();
  exec::CanonicalRecursiveCteWorkingRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000003203";
  request.physical_dag.root_physical_node_id = 3203;
  request.physical_dag.local_transaction_id = 3204;
  request.physical_dag.statement_snapshot_id = 3205;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000003211"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000003212"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000003213"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000003214"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000003215"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000003216"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000003217"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000003218"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 3201,
       .relational_node_id = 3201,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.cte-anchor.typed.v1",
       .output_descriptor_ids = {3201},
       .causal_counter_id = 32001},
      {.physical_node_id = 3202,
       .relational_node_id = 3202,
       .node_kind = exec::PhysicalNodeKind::kCte,
       .implementation_id = "cte.recursive-term.typed.v1",
       .output_descriptor_ids = {3201},
       .causal_counter_id = 32002},
      {.physical_node_id = 3203,
       .relational_node_id = 3203,
       .node_kind = exec::PhysicalNodeKind::kRecursiveCte,
       .implementation_id = "cte.recursive.working.typed.v1",
       .input_physical_node_ids = {3201, 3202},
       .output_descriptor_ids = {3201},
       .causal_counter_id = 32003},
  };
  request.selected_physical_node_id = 3203;
  request.anchor_batch = exec::MakeDescriptorBatch(
      {{"n", descriptor, false, 3201}}, {{{Value(descriptor, 1)}}});
  request.recursive_step = [descriptor](const exec::DescriptorBatch& working,
                                        const std::size_t) {
    exec::DescriptorBatch next;
    next.columns = working.columns;
    for (const auto& row : working.rows) {
      const auto value = std::stoll(row.values[0].encoded_value);
      if (value < 4) next.rows.push_back({{Value(descriptor, value + 1)}});
    }
    return next;
  };
  request.maximum_iteration_count = 8;
  request.maximum_working_row_count = 2;
  request.maximum_result_row_count = 8;
  return request;
}

// QOW-TEST-QRY-014-WORKING-V1
bool ValidateWorkingTransition() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalRecursiveCteWorking(Request());
  passed &= Require(
      result.diagnostic.ok && result.converged &&
          result.recursive_iteration_count == 4 &&
          result.iterations.size() == 4 &&
          result.iterations[0].working_row_count == 1 &&
          result.iterations[0].intermediate_row_count == 1 &&
          result.iterations[3].working_row_count == 1 &&
          result.iterations[3].intermediate_row_count == 0 &&
          result.maximum_observed_working_row_count == 1 &&
          result.output_batch.rows.size() == 4 &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[3].values[0].encoded_value == "4" &&
          result.selected_plan_uuid ==
              "019f0000-0000-7200-8000-000000003203" &&
          result.executed_physical_node_id == 3203 &&
          result.causal_counter_id == 32003,
      "working/intermediate transitions did not converge canonically");

  auto request = Request();
  request.anchor_batch.rows.clear();
  std::size_t calls = 0;
  request.recursive_step = [&calls](const exec::DescriptorBatch& batch,
                                    const std::size_t) {
    ++calls;
    return batch;
  };
  result = exec::ExecuteCanonicalRecursiveCteWorking(request);
  passed &= Require(result.diagnostic.ok && result.converged && calls == 0 &&
                        result.output_batch.columns.size() == 1 &&
                        result.output_batch.rows.empty(),
                    "empty anchor did not retain typed converged schema");

  request = Request();
  request.maximum_iteration_count = 2;
  result = exec::ExecuteCanonicalRecursiveCteWorking(request);
  passed &= Require(!result.diagnostic.ok && !result.converged &&
                        result.output_batch.rows.empty() &&
                        result.iterations.empty() &&
                        result.executed_physical_node_id == 0,
                    "non-convergence published partial CTE state");

  request = Request();
  request.recursive_step = [](const exec::DescriptorBatch& working,
                              const std::size_t) {
    auto malformed = working;
    malformed.rows[0].values[0].encoded_value = "not-int64";
    return malformed;
  };
  result = exec::ExecuteCanonicalRecursiveCteWorking(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "malformed intermediate relation was accepted");

  request = Request();
  request.physical_dag.nodes[2].implementation_id =
      "cte.recursive.fixed-static-union.v1";
  result = exec::ExecuteCanonicalRecursiveCteWorking(request);
  passed &= Require(!result.diagnostic.ok,
                    "fixed static recursive route was accepted");

  request = Request();
  request.physical_dag.nodes[1].output_descriptor_ids = {3299};
  result = exec::ExecuteCanonicalRecursiveCteWorking(request);
  passed &= Require(!result.diagnostic.ok,
                    "recursive descriptor drift was accepted");

  request = Request();
  request.physical_dag.statement_snapshot_id = 0;
  result = exec::ExecuteCanonicalRecursiveCteWorking(request);
  passed &= Require(!result.diagnostic.ok,
                    "missing engine MGA snapshot was accepted");
  return passed;
}

}  // namespace

int main() {
  return ValidateWorkingTransition() ? EXIT_SUCCESS : EXIT_FAILURE;
}
