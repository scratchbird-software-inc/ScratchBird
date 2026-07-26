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
#include <limits>
#include <string>
#include <string_view>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-010-FETCH-TOP-PROFILE-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000001101";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000001102;"
      "nullability=non_null";
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            const std::string& encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = encoded;
  value.state = api::EngineValueState::value;
  return value;
}

exec::CanonicalDescriptorFetchProfileRequest Request() {
  const auto descriptor = Descriptor();
  exec::CanonicalDescriptorFetchProfileRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001103";
  request.physical_dag.root_physical_node_id = 1102;
  request.physical_dag.local_transaction_id = 1103;
  request.physical_dag.statement_snapshot_id = 1104;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001111"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001112"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001113"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001114"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001115"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001116"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001117"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001118"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 1101,
       .relational_node_id = 1101,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {1101},
       .causal_counter_id = 11001},
      {.physical_node_id = 1102,
       .relational_node_id = 1102,
       .node_kind = exec::PhysicalNodeKind::kLimit,
       .implementation_id = "fetch.native.rows-only.v1",
       .input_physical_node_ids = {1101},
       .output_descriptor_ids = {1101},
       .causal_counter_id = 11002},
  };
  request.selected_physical_node_id = 1102;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"value", descriptor, false, 1101}},
      {{{Value(descriptor, "1")}},
       {{Value(descriptor, "2")}},
       {{Value(descriptor, "3")}},
       {{Value(descriptor, "4")}}});
  request.form = exec::CanonicalFetchTopProfileForm::fetch_first_rows_only;
  request.row_count = 2;
  request.offset = 1;
  request.row_count_is_bound = true;
  return request;
}

// QOW-TEST-QRY-010-FETCH-TOP-PROFILE-V1
bool ValidateFetchFirstProfile() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalDescriptorFetchProfile(Request());
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 1102 &&
                        result.causal_counter_id == 11002,
                    "native FETCH FIRST profile did not execute limit node");
  passed &= Require(result.output_batch.rows.size() == 2 &&
                        result.output_batch.rows[0].values[0].encoded_value ==
                            "2" &&
                        result.output_batch.rows[1].values[0].encoded_value ==
                            "3",
                    "FETCH FIRST count or preceding offset was not preserved");

  auto request = Request();
  request.row_count = 0;
  result = exec::ExecuteCanonicalDescriptorFetchProfile(request);
  passed &= Require(result.diagnostic.ok && result.output_batch.rows.empty(),
                    "FETCH FIRST zero returned rows");

  request = Request();
  request.row_count = std::numeric_limits<std::uint64_t>::max();
  result = exec::ExecuteCanonicalDescriptorFetchProfile(request);
  passed &= Require(result.diagnostic.ok &&
                        result.output_batch.rows.size() == 3,
                    "maximum FETCH FIRST count overflowed row bounds");

  request = Request();
  request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalDescriptorFetchProfile(request);
  passed &= Require(result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.output_batch.columns.size() == 1,
                    "empty FETCH FIRST input lost its descriptor table");

  for (const auto form : {
           exec::CanonicalFetchTopProfileForm::fetch_first_rows_with_ties,
           exec::CanonicalFetchTopProfileForm::top_rows,
           exec::CanonicalFetchTopProfileForm::top_percent,
           exec::CanonicalFetchTopProfileForm::top_rows_with_ties,
           static_cast<exec::CanonicalFetchTopProfileForm>(255),
       }) {
    request = Request();
    request.form = form;
    result = exec::ExecuteCanonicalDescriptorFetchProfile(request);
    passed &= Require(
        !result.diagnostic.ok && result.output_batch.rows.empty() &&
            result.diagnostic.diagnostic_code ==
                "QOW-DIAG-QRY-010-FETCH-TOP-PROFILE-REFUSAL-V1",
        "omitted FETCH/TOP form did not use canonical refusal");
  }

  request = Request();
  request.row_count_is_bound = false;
  result = exec::ExecuteCanonicalDescriptorFetchProfile(request);
  passed &= Require(
      !result.diagnostic.ok && result.output_batch.rows.empty() &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-010-FETCH-TOP-PROFILE-REFUSAL-V1",
      "unbound FETCH FIRST count was accepted");
  return passed;
}

}  // namespace

int main() {
  return ValidateFetchFirstProfile() ? EXIT_SUCCESS : EXIT_FAILURE;
}
