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
constexpr std::uint64_t kOwnerLocalTransactionId =
    0xffff'ffff'ffff'ff00ULL;
constexpr std::uint64_t kOldestActiveLocalTransactionId =
    0xffff'ffff'ffff'fee8ULL;
constexpr std::uint64_t kRetentionHorizonLocalTransactionId =
    0xffff'ffff'ffff'fed0ULL;
constexpr std::uint64_t kInDoubtLocalTransactionId =
    0xffff'ffff'ffff'fef0ULL;
constexpr std::uint64_t kInventoryNextLocalTransactionId =
    0xffff'ffff'ffff'fff0ULL;

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-016-NULL-COLLATION-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {"019f0000-0000-7200-8000-00000000f851",
          "019f0000-0000-7200-8000-00000000f852",
          statement_snapshot_uuid,
          "019f0000-0000-7200-8000-00000000f853",
          kOwnerLocalTransactionId,
          0,
          kOldestActiveLocalTransactionId,
          kRetentionHorizonLocalTransactionId,
          kRetentionHorizonLocalTransactionId,
          kRetentionHorizonLocalTransactionId,
          {kOldestActiveLocalTransactionId, kOwnerLocalTransactionId},
          {kInDoubtLocalTransactionId},
          "statement_stable",
          kInventoryNextLocalTransactionId,
          true,
          true,
          true};
}

exec::CanonicalExecutionMgaAuthority BindPhysicalAbiV2(
    exec::TypedPhysicalNodeDag* dag) {
  dag->abi_version = 2;
  dag->local_transaction_id = kOwnerLocalTransactionId;
  dag->statement_snapshot_id = 0;
  dag->bound_sblr_tree_uuid = dag->admission_evidence.at(0).evidence_uuid;
  dag->catalog_epoch_uuid = dag->admission_evidence.at(1).evidence_uuid;
  dag->security_context_uuid = dag->admission_evidence.at(2).evidence_uuid;
  dag->capability_snapshot_uuid = dag->admission_evidence.at(4).evidence_uuid;
  dag->resource_snapshot_uuid = dag->admission_evidence.at(5).evidence_uuid;
  dag->statistics_snapshot_uuid = dag->admission_evidence.at(6).evidence_uuid;
  dag->route_snapshot_uuid = dag->admission_evidence.at(7).evidence_uuid;
  dag->catalog_generation = 1;
  dag->security_epoch = 1;
  dag->policy_epoch = 1;
  dag->resource_epoch = 1;
  dag->statistics_generation = 1;
  dag->route_epoch = 1;
  dag->route_generation = 1;
  dag->memory_budget_bytes = 4096;
  dag->optimizer_published = true;
  dag->immutable_node_identity_validated = true;
  dag->capability_validated_before_access = true;
  const auto context =
      StatementContext(dag->admission_evidence.at(3).evidence_uuid);
  dag->mga_statement_context = context;
  for (auto& node : dag->nodes) {
    node.mga_statement_context = context;
    node.selected_alternative_uuid =
        "019f0000-0000-7200-8000-00000000f854";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000f855";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000f856";
    node.memory_bytes_required = 1;
    node.engine_capability_validated = true;
  }
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = context;
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  authority.resolve_current = [context] {
    exec::CanonicalMgaCurrentResolution current;
    current.statement_context = context;
    return current;
  };
  return authority;
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
  request.mga_authority = BindPhysicalAbiV2(&request.physical_dag);
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
          result.output_batch.rows[4].values[0].encoded_value == "Beta" &&
          result.mga_statement_context.visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request(exec::CanonicalSetOperationKind::kUnion,
                      exec::CanonicalSetOperationQuantifier::kDistinct)
                  .mga_authority.statement_context),
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

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.executed_physical_node_id == 0 &&
                        !exec::PhysicalMgaStatementContextValid(
                            result.mga_statement_context),
                    "missing MGA transaction reached collated set access");

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  request.mga_authority.resolve_current = {};
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "missing current MGA resolver reached collated set access");

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  request.physical_dag.catalog_epoch_uuid =
      request.physical_dag.mga_statement_context
          .statement_metadata_snapshot_uuid;
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "catalog metadata substitution reached collated set access");

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  request.physical_dag.local_transaction_id =
      static_cast<std::uint32_t>(kOwnerLocalTransactionId);
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "narrowed MGA identity reached collated set access");
  return passed;
}

}  // namespace

int main() { return ValidateSetOperationNullCollation() ? EXIT_SUCCESS
                                                         : EXIT_FAILURE; }
