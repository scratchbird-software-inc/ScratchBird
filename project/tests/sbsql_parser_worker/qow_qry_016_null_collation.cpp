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
namespace dt = scratchbird::core::datatypes;

namespace {

constexpr std::string_view kCollationUuid =
    "019f0000-0000-7400-8000-000000004601";

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-016-NULL-COLLATION-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& uuid,
                                 const std::string_view collation_uuid =
                                     kCollationUuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "text";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000004600;"
      "nullability=nullable;collation_uuid=" +
      std::string(collation_uuid);
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

api::EngineTypedValue Null(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.is_null = true;
  value.state = api::EngineValueState::sql_null;
  return value;
}

dt::DatatypeTextSeedAuthority CollationAuthority() {
  dt::DatatypeTextSeedAuthority authority;
  authority.active = true;
  authority.seed_pack_name = "qow_core_resource_catalog";
  authority.seed_pack_version = "2026.07";
  authority.charset_name = "UTF-8";
  authority.collation_name = "unicode_ci_ai";
  authority.collation_case_insensitive = true;
  authority.collation_accent_insensitive = true;
  return authority;
}

exec::CanonicalSetOperationAllRequest Request(
    const exec::CanonicalSetOperationKind operation,
    const exec::CanonicalSetOperationQuantifier quantifier) {
  const auto left_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004602");
  const auto right_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004603");
  const auto result_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004604");

  exec::CanonicalSetOperationAllRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000004605";
  request.physical_dag.root_physical_node_id = 4603;
  request.physical_dag.local_transaction_id = 4604;
  request.physical_dag.statement_snapshot_id = 4605;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000004611"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000004612"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000004613"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000004614"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000004615"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000004616"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000004617"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000004618"},
  };
  std::string operation_name;
  switch (operation) {
    case exec::CanonicalSetOperationKind::kUnion:
      operation_name = "union";
      break;
    case exec::CanonicalSetOperationKind::kIntersect:
      operation_name = "intersect";
      break;
    case exec::CanonicalSetOperationKind::kExcept:
      operation_name = "except";
      break;
  }
  const std::string quantifier_name =
      quantifier == exec::CanonicalSetOperationQuantifier::kAll
          ? "all"
          : "distinct";
  request.physical_dag.nodes = {
      {.physical_node_id = 4601,
       .relational_node_id = 4601,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.set-left.typed.v1",
       .output_descriptor_ids = {4601},
       .causal_counter_id = 46001},
      {.physical_node_id = 4602,
       .relational_node_id = 4602,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.set-right.typed.v1",
       .output_descriptor_ids = {4602},
       .causal_counter_id = 46002},
      {.physical_node_id = 4603,
       .relational_node_id = 4603,
       .node_kind = exec::PhysicalNodeKind::kSetOperation,
       .implementation_id = "setop." + operation_name + "-" +
                            quantifier_name +
                            ".ordinal.null-collation.typed.v1",
       .input_physical_node_ids = {4601, 4602},
       .output_descriptor_ids = {4603},
       .causal_counter_id = 46003},
  };
  request.selected_physical_node_id = 4603;
  request.left_batch = exec::MakeDescriptorBatch(
      {{"label", left_descriptor, true, 4601}},
      {{{Value(left_descriptor, "R\xC3\xA9sum\xC3\xA9")}},
       {{Value(left_descriptor, "ALPHA")}},
       {{Null(left_descriptor)}},
       {{Null(left_descriptor)}},
       {{Value(left_descriptor, "Gamma")}}});
  request.right_batch = exec::MakeDescriptorBatch(
      {{"label", right_descriptor, true, 4602}},
      {{{Value(right_descriptor, "resume")}},
       {{Value(right_descriptor, "alpha")}},
       {{Value(right_descriptor, "Beta")}},
       {{Null(right_descriptor)}}});
  request.result_columns = {{"label", result_descriptor, true, 4603}};
  request.operation = operation;
  request.quantifier = quantifier;
  request.equality_profile =
      exec::CanonicalSetOperationEqualityProfile::kNullEqualBoundCollation;
  request.collation_bindings = {
      {.result_column = 0,
       .collation_uuid = std::string(kCollationUuid),
       .resource_epoch = 46,
       .collation_epoch = 17,
       .text_seed = CollationAuthority()},
  };
  request.maximum_equality_comparison_count = 128;
  request.maximum_output_row_count = 16;
  return request;
}

// QOW-TEST-QRY-016-NULL-COLLATION-V1
bool ValidateSetOperationNullCollation() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalSetOperationDistinct(Request(
      exec::CanonicalSetOperationKind::kUnion,
      exec::CanonicalSetOperationQuantifier::kDistinct));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 5 &&
          result.eliminated_duplicate_row_count == 4 &&
          result.equality_comparison_count != 0 &&
          result.output_batch.rows[0].values[0].encoded_value ==
              "R\xC3\xA9sum\xC3\xA9" &&
          result.output_batch.rows[1].values[0].encoded_value == "ALPHA" &&
          result.output_batch.rows[2].values[0].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[3].values[0].encoded_value == "Gamma" &&
          result.output_batch.rows[4].values[0].encoded_value == "Beta",
      "UNION DISTINCT did not apply bound collation and NULL equality");

  result = exec::ExecuteCanonicalSetOperationDistinct(Request(
      exec::CanonicalSetOperationKind::kIntersect,
      exec::CanonicalSetOperationQuantifier::kDistinct));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 3 &&
          result.output_batch.rows[0].values[0].encoded_value ==
              "R\xC3\xA9sum\xC3\xA9" &&
          result.output_batch.rows[1].values[0].encoded_value == "ALPHA" &&
          result.output_batch.rows[2].values[0].state ==
              api::EngineValueState::sql_null,
      "INTERSECT DISTINCT did not use collated membership");

  result = exec::ExecuteCanonicalSetOperationDistinct(Request(
      exec::CanonicalSetOperationKind::kExcept,
      exec::CanonicalSetOperationQuantifier::kDistinct));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[0].encoded_value == "Gamma",
      "EXCEPT DISTINCT did not exclude collated and NULL matches");

  result = exec::ExecuteCanonicalSetOperationAll(Request(
      exec::CanonicalSetOperationKind::kIntersect,
      exec::CanonicalSetOperationQuantifier::kAll));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 3 &&
          result.output_batch.rows[2].values[0].state ==
              api::EngineValueState::sql_null,
      "INTERSECT ALL did not consume one matching NULL multiplicity");

  auto request = Request(exec::CanonicalSetOperationKind::kUnion,
                         exec::CanonicalSetOperationQuantifier::kDistinct);
  request.collation_bindings.clear();
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-016-NULL-COLLATION-REFUSAL-V1" &&
          result.output_batch.rows.empty(),
      "missing bound collation authority was accepted");

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  request.collation_bindings[0].text_seed.seed_pack_version.clear();
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "incomplete collation seed authority was accepted");

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  request.maximum_equality_comparison_count = 1;
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "collation comparison resource excess published rows");

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  const auto mismatched = Descriptor(
      "019f0000-0000-7200-8000-000000004603",
      "019f0000-0000-7400-8000-000000004699");
  request.right_batch.columns[0].descriptor = mismatched;
  for (auto& row : request.right_batch.rows) {
    row.values[0].descriptor = mismatched;
  }
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-016-NULL-COLLATION-REFUSAL-V1",
      "mismatched bound collation descriptor was accepted");
  return passed;
}

}  // namespace

int main() { return ValidateSetOperationNullCollation() ? EXIT_SUCCESS
                                                         : EXIT_FAILURE; }
