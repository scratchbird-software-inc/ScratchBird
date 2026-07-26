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
#include <stdexcept>
#include <string>
#include <string_view>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-014-CANCELLATION-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000003601";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000003602;"
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

exec::CanonicalRecursiveCteCancellationRequest Request() {
  const auto descriptor = Descriptor();
  exec::CanonicalRecursiveCteCancellationRequest request;
  auto& working = request.working_request;
  working.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000003603";
  working.physical_dag.root_physical_node_id = 3603;
  working.physical_dag.local_transaction_id = 3604;
  working.physical_dag.statement_snapshot_id = 3605;
  working.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000003611"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000003612"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000003613"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000003614"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000003615"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000003616"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000003617"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000003618"},
  };
  working.physical_dag.nodes = {
      {.physical_node_id = 3601,
       .relational_node_id = 3601,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.cte-cancellation-anchor.typed.v1",
       .output_descriptor_ids = {3601},
       .causal_counter_id = 36001},
      {.physical_node_id = 3602,
       .relational_node_id = 3602,
       .node_kind = exec::PhysicalNodeKind::kCte,
       .implementation_id = "cte.cancellation-term.typed.v1",
       .output_descriptor_ids = {3601},
       .causal_counter_id = 36002},
      {.physical_node_id = 3603,
       .relational_node_id = 3603,
       .node_kind = exec::PhysicalNodeKind::kRecursiveCte,
       .implementation_id = "cte.recursive.cancellable.typed.v1",
       .input_physical_node_ids = {3601, 3602},
       .output_descriptor_ids = {3601},
       .causal_counter_id = 36003},
  };
  working.selected_physical_node_id = 3603;
  working.anchor_batch = exec::MakeDescriptorBatch(
      {{"n", descriptor, false, 3601}}, {{{Value(descriptor, 1)}}});
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
  request.cancellation_requested = [](const std::size_t) { return false; };
  request.cancellation_evidence_uuid =
      "019f0000-0000-7200-8000-000000003615";
  return request;
}

// QOW-TEST-QRY-014-CANCELLATION-V1
bool ValidateCancellationBoundary() {
  bool passed = true;
  auto request = Request();
  request.cancellation_requested =
      [](const std::size_t iteration) { return iteration == 2; };
  auto result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(
      !result.working_result.diagnostic.ok && result.cancelled &&
          result.cancellation_iteration_ordinal == 2 &&
          result.working_result.output_batch.rows.empty() &&
          result.working_result.iterations.empty() &&
          result.working_state_cleaned &&
          result.cancellation_evidence_uuid ==
              "019f0000-0000-7200-8000-000000003615",
      "mid-recursion cancellation published or retained private state");

  result = exec::ExecuteCanonicalRecursiveCteCancellation(Request());
  passed &= Require(
      result.working_result.diagnostic.ok && !result.cancelled &&
          result.working_result.output_batch.rows.size() == 4 &&
          result.working_result.recursive_iteration_count == 4 &&
          result.working_state_cleaned &&
          result.working_result.executed_physical_node_id == 3603,
      "uncancelled recursive CTE did not complete through shared working state");

  request = Request();
  request.cancellation_requested =
      [](const std::size_t iteration) { return iteration == 0; };
  result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(!result.working_result.diagnostic.ok && result.cancelled &&
                        result.cancellation_iteration_ordinal == 0 &&
                        result.working_result.output_batch.rows.empty() &&
                        result.working_state_cleaned,
                    "pre-anchor cancellation was not an atomic cleanup");

  request = Request();
  request.cancellation_evidence_uuid =
      "019f0000-0000-7200-8000-000000003699";
  result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(!result.working_result.diagnostic.ok && !result.cancelled &&
                        result.cancellation_evidence_uuid.empty(),
                    "drifted cancellation evidence was accepted");

  request = Request();
  request.cancellation_requested = {};
  result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_state_cleaned,
                    "missing cancellation probe was accepted");

  request = Request();
  request.cancellation_requested = [](const std::size_t iteration) {
    if (iteration == 1) throw std::runtime_error("probe transport failed");
    return false;
  };
  result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(!result.working_result.diagnostic.ok && !result.cancelled &&
                        result.working_result.output_batch.rows.empty() &&
                        result.working_state_cleaned,
                    "probe failure was misreported as cancellation or published");

  request = Request();
  request.working_request.physical_dag.nodes[2].implementation_id =
      "cte.recursive.working.typed.v1";
  result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(!result.working_result.diagnostic.ok,
                    "uncancellable physical profile bypassed admission");

  request = Request();
  request.working_request.physical_dag.statement_snapshot_id = 0;
  result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_state_cleaned,
                    "cancellation route accepted missing MGA snapshot");
  return passed;
}

}  // namespace

int main() {
  return ValidateCancellationBoundary() ? EXIT_SUCCESS : EXIT_FAILURE;
}
