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
    std::cerr << "QOW-TEST-QRY-014-UNION-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000003301";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000003302;"
      "nullability=nullable";
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            std::string encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = std::move(encoded);
  value.state = api::EngineValueState::value;
  return value;
}

exec::CanonicalRecursiveCteUnionRequest Request(
    const exec::CanonicalRecursiveCteUnionMode mode) {
  const auto descriptor = Descriptor();
  exec::CanonicalRecursiveCteUnionRequest request;
  request.union_mode = mode;
  auto& working = request.working_request;
  working.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000003303";
  working.physical_dag.root_physical_node_id = 3303;
  working.physical_dag.local_transaction_id = 3304;
  working.physical_dag.statement_snapshot_id = 3305;
  working.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000003311"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000003312"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000003313"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000003314"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000003315"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000003316"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000003317"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000003318"},
  };
  const std::string root_profile =
      mode == exec::CanonicalRecursiveCteUnionMode::kAll
          ? "cte.recursive.union-all.typed.v1"
          : "cte.recursive.union-distinct-int64.typed.v1";
  working.physical_dag.nodes = {
      {.physical_node_id = 3301,
       .relational_node_id = 3301,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.cte-union-anchor.typed.v1",
       .output_descriptor_ids = {3301},
       .causal_counter_id = 33001},
      {.physical_node_id = 3302,
       .relational_node_id = 3302,
       .node_kind = exec::PhysicalNodeKind::kCte,
       .implementation_id = "cte.recursive-union-term.typed.v1",
       .output_descriptor_ids = {3301},
       .causal_counter_id = 33002},
      {.physical_node_id = 3303,
       .relational_node_id = 3303,
       .node_kind = exec::PhysicalNodeKind::kRecursiveCte,
       .implementation_id = root_profile,
       .input_physical_node_ids = {3301, 3302},
       .output_descriptor_ids = {3301},
       .causal_counter_id = 33003},
  };
  working.selected_physical_node_id = 3303;
  working.anchor_batch = exec::MakeDescriptorBatch(
      {{"n", descriptor, true, 3301}},
      {{{Value(descriptor, "1")}}, {{Value(descriptor, "01")}}});
  working.recursive_step =
      [descriptor](const exec::DescriptorBatch& current, const std::size_t) {
        exec::DescriptorBatch next;
        next.columns = current.columns;
        for (const auto& row : current.rows) {
          const auto value = std::stoll(row.values[0].encoded_value);
          next.rows.push_back({{Value(descriptor, std::to_string(value))}});
          if (value < 3) {
            next.rows.push_back(
                {{Value(descriptor, std::to_string(value + 1))}});
          }
        }
        return next;
      };
  working.maximum_iteration_count = 8;
  working.maximum_working_row_count = 16;
  working.maximum_result_row_count = 64;
  return request;
}

// QOW-TEST-QRY-014-UNION-V1
bool ValidateUnionModes() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalRecursiveCteUnion(
      Request(exec::CanonicalRecursiveCteUnionMode::kDistinct));
  passed &= Require(
      result.working_result.diagnostic.ok &&
          result.working_result.converged &&
          result.working_result.recursive_iteration_count == 3 &&
          result.working_result.output_batch.rows.size() == 3 &&
          result.working_result.output_batch.rows[0].values[0].encoded_value ==
              "1" &&
          result.working_result.output_batch.rows[1].values[0].encoded_value ==
              "2" &&
          result.working_result.output_batch.rows[2].values[0].encoded_value ==
              "3" &&
          result.duplicate_row_count == 4 &&
          result.working_result.executed_physical_node_id == 3303,
      "UNION DISTINCT did not remove typed duplicates across iterations");

  auto request = Request(exec::CanonicalRecursiveCteUnionMode::kAll);
  request.working_request.recursive_step =
      [](const exec::DescriptorBatch& current, const std::size_t iteration) {
        exec::DescriptorBatch next;
        next.columns = current.columns;
        if (iteration == 1) next.rows = current.rows;
        return next;
      };
  result = exec::ExecuteCanonicalRecursiveCteUnion(request);
  passed &= Require(result.working_result.diagnostic.ok &&
                        result.working_result.output_batch.rows.size() == 4 &&
                        result.duplicate_row_count == 0,
                    "UNION ALL did not preserve duplicate rows");

  request = Request(exec::CanonicalRecursiveCteUnionMode::kDistinct);
  request.working_request.physical_dag.nodes[2].implementation_id =
      "cte.recursive.union-all.typed.v1";
  result = exec::ExecuteCanonicalRecursiveCteUnion(request);
  passed &= Require(!result.working_result.diagnostic.ok,
                    "UNION mode/profile drift was accepted");

  request = Request(exec::CanonicalRecursiveCteUnionMode::kDistinct);
  request.working_request.recursive_step =
      [](const exec::DescriptorBatch& current, const std::size_t) {
        auto malformed = current;
        malformed.rows[0].values[0].encoded_value = "bad";
        return malformed;
      };
  result = exec::ExecuteCanonicalRecursiveCteUnion(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_result.output_batch.rows.empty() &&
                        result.duplicate_row_count == 0,
                    "malformed DISTINCT row published prior state");

  request = Request(exec::CanonicalRecursiveCteUnionMode::kDistinct);
  request.working_request.anchor_batch.columns[0]
      .descriptor.canonical_type_name = "text";
  request.working_request.anchor_batch.rows[0]
      .values[0].descriptor.canonical_type_name = "text";
  request.working_request.anchor_batch.rows[1]
      .values[0].descriptor.canonical_type_name = "text";
  result = exec::ExecuteCanonicalRecursiveCteUnion(request);
  passed &= Require(!result.working_result.diagnostic.ok,
                    "unsupported DISTINCT descriptor profile was accepted");
  return passed;
}

}  // namespace

int main() { return ValidateUnionModes() ? EXIT_SUCCESS : EXIT_FAILURE; }
