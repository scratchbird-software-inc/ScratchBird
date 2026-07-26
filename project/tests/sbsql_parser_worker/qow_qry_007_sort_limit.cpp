// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-007-SORT-LIMIT-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000007201";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "decimal";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000007202;"
      "nullability=nullable;precision=12;scale=2";
  return descriptor;
}

exec::CanonicalDescriptorLimitRequest Request() {
  const auto descriptor = Descriptor();
  const auto value = [&](const std::string& encoded) {
    api::EngineTypedValue typed;
    typed.descriptor = descriptor;
    typed.encoded_value = encoded;
    return typed;
  };
  exec::CanonicalDescriptorLimitRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000007203";
  request.physical_dag.root_physical_node_id = 722;
  request.physical_dag.local_transaction_id = 723;
  request.physical_dag.statement_snapshot_id = 724;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000007211"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000007212"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000007213"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000007214"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000007215"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000007216"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000007217"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000007218"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 721,
       .relational_node_id = 721,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {721},
       .causal_counter_id = 7201},
      {.physical_node_id = 722,
       .relational_node_id = 722,
       .node_kind = exec::PhysicalNodeKind::kLimit,
       .implementation_id = "limit.typed.v1",
       .input_physical_node_ids = {721},
       .output_descriptor_ids = {721},
       .causal_counter_id = 7202},
  };
  request.selected_physical_node_id = 722;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"amount", descriptor, true, 721}},
      {{{value("1.00")}},
       {{value("2.00")}},
       {{value("3.00")}},
       {{value("4.00")}}});
  request.limit = 2;
  request.offset = 1;
  return request;
}

// QOW-TEST-QRY-007-SORT-LIMIT-V1
bool ValidatePhysicalLimit() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalDescriptorLimit(Request());
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 722 &&
                        result.causal_counter_id == 7202,
                    "typed physical limit node was not executable");
  passed &= Require(result.output_batch.rows.size() == 2 &&
                        result.output_batch.rows[0].values[0].encoded_value ==
                            "2.00" &&
                        result.output_batch.rows[1].values[0].encoded_value ==
                            "3.00",
                    "LIMIT/OFFSET did not preserve input order and bounds");
  passed &= Require(result.output_batch.columns[0].descriptor_id == 721,
                    "LIMIT changed the output descriptor handle");

  auto request = Request();
  request.offset = std::numeric_limits<std::uint64_t>::max();
  request.limit = std::numeric_limits<std::uint64_t>::max();
  result = exec::ExecuteCanonicalDescriptorLimit(request);
  passed &= Require(result.diagnostic.ok && result.output_batch.rows.empty(),
                    "maximum OFFSET overflowed into visible rows");

  request = Request();
  request.offset = 3;
  request.limit = std::numeric_limits<std::uint64_t>::max();
  result = exec::ExecuteCanonicalDescriptorLimit(request);
  passed &= Require(result.diagnostic.ok &&
                        result.output_batch.rows.size() == 1 &&
                        result.output_batch.rows[0].values[0].encoded_value ==
                            "4.00",
                    "maximum LIMIT overflowed end-of-input arithmetic");

  request = Request();
  request.limit = 0;
  result = exec::ExecuteCanonicalDescriptorLimit(request);
  passed &= Require(result.diagnostic.ok && result.output_batch.rows.empty(),
                    "LIMIT zero returned data");

  request = Request();
  request.physical_dag.nodes.back().node_kind =
      exec::PhysicalNodeKind::kSort;
  result = exec::ExecuteCanonicalDescriptorLimit(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "unimplemented sort route fell back to integer ordering");
  return passed;
}

}  // namespace

int main() {
  if (!ValidatePhysicalLimit()) return 1;
  std::cout << "QOW-TEST-QRY-007-SORT-LIMIT-V1: PASS\n";
  return 0;
}
