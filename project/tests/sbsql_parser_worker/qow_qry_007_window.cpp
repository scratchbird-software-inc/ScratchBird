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
  if (!condition) std::cerr << "QOW-TEST-QRY-007-WINDOW-V1: " << detail << '\n';
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& descriptor_uuid,
                                 const std::string& type_uuid,
                                 const std::string& nullability) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=" + nullability;
  return descriptor;
}

exec::CanonicalDescriptorRowNumberRequest Request() {
  const auto input_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000007501",
      "019f0000-0000-7300-8000-000000007502", "nullable");
  const auto row_number_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000007503",
      "019f0000-0000-7300-8000-000000007504", "non_null");
  const auto value = [&](const std::string& encoded) {
    api::EngineTypedValue typed;
    typed.descriptor = input_descriptor;
    typed.encoded_value = encoded;
    return typed;
  };

  exec::CanonicalDescriptorRowNumberRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000007505";
  request.physical_dag.root_physical_node_id = 753;
  request.physical_dag.local_transaction_id = 754;
  request.physical_dag.statement_snapshot_id = 755;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000007511"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000007512"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000007513"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000007514"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000007515"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000007516"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000007517"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000007518"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 751,
       .relational_node_id = 751,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {751},
       .causal_counter_id = 7501},
      {.physical_node_id = 752,
       .relational_node_id = 752,
       .node_kind = exec::PhysicalNodeKind::kSort,
       .implementation_id = "sort.typed.order-proof.v1",
       .input_physical_node_ids = {751},
       .output_descriptor_ids = {751},
       .causal_counter_id = 7502},
      {.physical_node_id = 753,
       .relational_node_id = 753,
       .node_kind = exec::PhysicalNodeKind::kWindow,
       .implementation_id = "window.row-number.v1",
       .input_physical_node_ids = {752},
       .output_descriptor_ids = {751, 752},
       .causal_counter_id = 7503},
  };
  request.selected_physical_node_id = 753;
  request.ordered_input_batch = exec::MakeDescriptorBatch(
      {{"order_key", input_descriptor, true, 751}},
      {{{value("2")}}, {{value("5")}}});
  request.row_number_column =
      {"row_number", row_number_descriptor, false, 752};
  request.deterministic_order_evidence_uuid =
      "019f0000-0000-7200-8000-000000007519";
  return request;
}

// QOW-TEST-QRY-007-WINDOW-V1
bool ValidatePhysicalRowNumber() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalDescriptorRowNumber(Request());
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 753 &&
                        result.causal_counter_id == 7503,
                    "typed physical ROW_NUMBER node was not executable");
  passed &= Require(
      result.output_batch.rows.size() == 2 &&
          result.output_batch.rows[0].values[0].encoded_value == "2" &&
          result.output_batch.rows[0].values[1].encoded_value == "1" &&
          result.output_batch.rows[1].values[0].encoded_value == "5" &&
          result.output_batch.rows[1].values[1].encoded_value == "2",
      "ROW_NUMBER changed input order or assigned wrong ordinals");
  passed &= Require(result.output_batch.columns.size() == 2 &&
                        result.output_batch.columns[1].descriptor_id == 752 &&
                        !result.output_batch.columns[1].nullable,
                    "ROW_NUMBER lost its bound non-null descriptor");

  auto request = Request();
  request.ordered_input_batch.rows.clear();
  result = exec::ExecuteCanonicalDescriptorRowNumber(request);
  passed &= Require(result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.output_batch.columns.size() == 2,
                    "empty ROW_NUMBER input lost output descriptors");

  request = Request();
  request.deterministic_order_evidence_uuid.clear();
  result = exec::ExecuteCanonicalDescriptorRowNumber(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "ROW_NUMBER invented missing deterministic order");

  request = Request();
  request.physical_dag.nodes[1].node_kind =
      exec::PhysicalNodeKind::kProject;
  result = exec::ExecuteCanonicalDescriptorRowNumber(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "ROW_NUMBER accepted unordered project input");

  request = Request();
  request.row_number_column.nullable = true;
  result = exec::ExecuteCanonicalDescriptorRowNumber(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "nullable ROW_NUMBER result descriptor was accepted");
  return passed;
}

}  // namespace

int main() {
  if (!ValidatePhysicalRowNumber()) return 1;
  std::cout << "QOW-TEST-QRY-007-WINDOW-V1: PASS\n";
  return 0;
}
