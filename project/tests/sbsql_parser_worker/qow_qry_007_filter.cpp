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
  if (!condition) std::cerr << "QOW-TEST-QRY-007-FILTER-V1: " << detail << '\n';
  return condition;
}

api::EngineDescriptor Descriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000007101";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "decimal";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000007102;"
      "nullability=nullable;precision=12;scale=2";
  return descriptor;
}

exec::TypedPhysicalNodeDag Dag() {
  exec::TypedPhysicalNodeDag dag;
  dag.selected_plan_uuid = "019f0000-0000-7200-8000-000000007103";
  dag.root_physical_node_id = 712;
  dag.local_transaction_id = 713;
  dag.statement_snapshot_id = 714;
  dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000007111"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000007112"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000007113"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000007114"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000007115"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000007116"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000007117"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000007118"},
  };
  dag.nodes = {
      {.physical_node_id = 711,
       .relational_node_id = 711,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {711},
       .causal_counter_id = 7101},
      {.physical_node_id = 712,
       .relational_node_id = 712,
       .node_kind = exec::PhysicalNodeKind::kFilter,
       .implementation_id = "filter.3vl.row.v1",
       .input_physical_node_ids = {711},
       .output_descriptor_ids = {711},
       .causal_counter_id = 7102},
  };
  return dag;
}

exec::CanonicalDescriptorFilterRequest Request() {
  const auto descriptor = Descriptor();
  const auto value = [&](const std::string& encoded) {
    api::EngineTypedValue typed;
    typed.descriptor = descriptor;
    typed.encoded_value = encoded;
    return typed;
  };
  exec::CanonicalDescriptorFilterRequest request;
  request.physical_dag = Dag();
  request.selected_physical_node_id = 712;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"amount", descriptor, true, 711}},
      {{{value("1.00")}}, {{value("2.00")}}, {{value("3.00")}}});
  request.row_truth_values = {
      api::EngineSqlTruthValue::true_value,
      api::EngineSqlTruthValue::false_value,
      api::EngineSqlTruthValue::unknown,
  };
  return request;
}

// QOW-TEST-QRY-007-FILTER-V1
bool ValidatePhysicalFilter() {
  bool passed = true;
  const auto result = exec::ExecuteCanonicalDescriptorFilter(Request());
  passed &= Require(result.diagnostic.ok,
                    "typed physical filter node was not executable");
  passed &= Require(result.executed_physical_node_id == 712 &&
                        result.causal_counter_id == 7102,
                    "filter node execution identity was lost");
  passed &= Require(result.output_batch.rows.size() == 1 &&
                        result.output_batch.rows[0].values[0].encoded_value ==
                            "1.00",
                    "filter did not admit TRUE exclusively");
  passed &= Require(result.output_batch.columns[0].descriptor_id == 711,
                    "filter changed the bound output schema");

  auto invalid = Request();
  invalid.row_truth_values[1] = api::EngineSqlTruthValue::unspecified;
  auto refused = exec::ExecuteCanonicalDescriptorFilter(invalid);
  passed &= Require(
      !refused.diagnostic.ok &&
          refused.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-017-3VL-REFUSAL-V1" &&
          refused.output_batch.rows.empty(),
      "unbound truth state reached row admission");

  invalid = Request();
  invalid.consumer = api::EnginePredicateConsumer::join_on;
  refused = exec::ExecuteCanonicalDescriptorFilter(invalid);
  passed &= Require(!refused.diagnostic.ok &&
                        refused.output_batch.rows.empty(),
                    "join-ON consumer was admitted as a filter node");

  invalid = Request();
  invalid.row_truth_values.pop_back();
  refused = exec::ExecuteCanonicalDescriptorFilter(invalid);
  passed &= Require(!refused.diagnostic.ok &&
                        refused.output_batch.rows.empty(),
                    "predicate cardinality mismatch produced partial rows");
  return passed;
}

}  // namespace

int main() {
  if (!ValidatePhysicalFilter()) return 1;
  std::cout << "QOW-TEST-QRY-007-FILTER-V1: PASS\n";
  return 0;
}
