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
  if (!condition) std::cerr << "QOW-TEST-QRY-007-JOIN-V1: " << detail << '\n';
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

exec::CanonicalDescriptorInnerJoinRequest Request() {
  const auto left_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000007401",
      "019f0000-0000-7300-8000-000000007402");
  const auto right_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000007403",
      "019f0000-0000-7300-8000-000000007404");
  const auto value = [](const api::EngineDescriptor& descriptor,
                        const std::string& encoded) {
    api::EngineTypedValue typed;
    typed.descriptor = descriptor;
    typed.encoded_value = encoded;
    return typed;
  };

  exec::CanonicalDescriptorInnerJoinRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000007405";
  request.physical_dag.root_physical_node_id = 743;
  request.physical_dag.local_transaction_id = 744;
  request.physical_dag.statement_snapshot_id = 745;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000007411"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000007412"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000007413"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000007414"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000007415"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000007416"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000007417"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000007418"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 741,
       .relational_node_id = 741,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.left.typed.v1",
       .output_descriptor_ids = {741},
       .causal_counter_id = 7401},
      {.physical_node_id = 742,
       .relational_node_id = 742,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.right.typed.v1",
       .output_descriptor_ids = {742},
       .causal_counter_id = 7402},
      {.physical_node_id = 743,
       .relational_node_id = 743,
       .node_kind = exec::PhysicalNodeKind::kJoin,
       .implementation_id = "join.inner.3vl.nested.v1",
       .input_physical_node_ids = {741, 742},
       .output_descriptor_ids = {741, 742},
       .causal_counter_id = 7403},
  };
  request.selected_physical_node_id = 743;
  request.left_batch = exec::MakeDescriptorBatch(
      {{"left_id", left_descriptor, true, 741}},
      {{{value(left_descriptor, "1")}}, {{value(left_descriptor, "2")}}});
  request.right_batch = exec::MakeDescriptorBatch(
      {{"right_id", right_descriptor, true, 742}},
      {{{value(right_descriptor, "10")}},
       {{value(right_descriptor, "20")}}});
  request.pair_truth_values = {
      api::EngineSqlTruthValue::true_value,
      api::EngineSqlTruthValue::false_value,
      api::EngineSqlTruthValue::unknown,
      api::EngineSqlTruthValue::true_value,
  };
  return request;
}

// QOW-TEST-QRY-007-JOIN-V1
bool ValidatePhysicalInnerJoin() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalDescriptorInnerJoin(Request());
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 743 &&
                        result.causal_counter_id == 7403,
                    "typed physical join node was not executable");
  passed &= Require(
      result.output_batch.rows.size() == 2 &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[0].values[1].encoded_value == "10" &&
          result.output_batch.rows[1].values[0].encoded_value == "2" &&
          result.output_batch.rows[1].values[1].encoded_value == "20",
      "inner join did not admit TRUE row pairs exclusively");
  passed &= Require(result.output_batch.columns.size() == 2 &&
                        result.output_batch.columns[0].descriptor_id == 741 &&
                        result.output_batch.columns[1].descriptor_id == 742,
                    "join did not concatenate bound output descriptors");

  auto request = Request();
  request.left_batch.rows.clear();
  request.pair_truth_values.clear();
  result = exec::ExecuteCanonicalDescriptorInnerJoin(request);
  passed &= Require(result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.output_batch.columns.size() == 2,
                    "empty typed join input lost output schema");

  request = Request();
  request.pair_truth_values[0] = api::EngineSqlTruthValue::unspecified;
  result = exec::ExecuteCanonicalDescriptorInnerJoin(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "unbound ON truth produced partial joined rows");

  request = Request();
  request.consumer = api::EnginePredicateConsumer::filter;
  result = exec::ExecuteCanonicalDescriptorInnerJoin(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "filter consumer was admitted as join-ON");

  request = Request();
  request.physical_dag.nodes.back().output_descriptor_ids = {742, 741};
  result = exec::ExecuteCanonicalDescriptorInnerJoin(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "reordered unresolved join schema produced data");
  return passed;
}

}  // namespace

int main() {
  if (!ValidatePhysicalInnerJoin()) return 1;
  std::cout << "QOW-TEST-QRY-007-JOIN-V1: PASS\n";
  return 0;
}
