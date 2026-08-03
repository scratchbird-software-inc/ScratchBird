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
#include <vector>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

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
    std::cerr << "QOW-TEST-QRY-016-TYPE-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {"019f0000-0000-7200-8000-00000000f861",
          "019f0000-0000-7200-8000-00000000f862",
          statement_snapshot_uuid,
          "019f0000-0000-7200-8000-00000000f863",
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
        "019f0000-0000-7200-8000-00000000f864";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000f865";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000f866";
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

api::EngineDescriptor Descriptor(const std::string& descriptor_uuid,
                                 const std::string_view type_name,
                                 const std::string_view type_uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::string(type_name);
  descriptor.encoded_descriptor = "type_uuid=" + std::string(type_uuid) +
                                  ";nullability=nullable";
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

std::string OperationName(const exec::CanonicalSetOperationKind operation) {
  switch (operation) {
    case exec::CanonicalSetOperationKind::kUnion:
      return "union";
    case exec::CanonicalSetOperationKind::kIntersect:
      return "intersect";
    case exec::CanonicalSetOperationKind::kExcept:
      return "except";
  }
  return {};
}

exec::CanonicalSetOperationAllRequest Request(
    const exec::CanonicalSetOperationKind operation,
    const exec::CanonicalSetOperationQuantifier quantifier) {
  const auto left_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000004701", "int32",
      "019f0000-0000-7300-8000-000000004701");
  const auto right_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000004702", "int64",
      "019f0000-0000-7300-8000-000000004702");
  const auto result_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000004703", "int64",
      "019f0000-0000-7300-8000-000000004702");

  exec::CanonicalSetOperationAllRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000004704";
  request.physical_dag.root_physical_node_id = 4703;
  request.physical_dag.local_transaction_id = 4704;
  request.physical_dag.statement_snapshot_id = 4705;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000004711"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000004712"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000004713"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000004714"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000004715"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000004716"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000004717"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000004718"},
  };
  const std::string quantifier_name =
      quantifier == exec::CanonicalSetOperationQuantifier::kAll
          ? "all"
          : "distinct";
  request.physical_dag.nodes = {
      {.physical_node_id = 4701,
       .relational_node_id = 4701,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.set-left.typed.v1",
       .output_descriptor_ids = {4701},
       .causal_counter_id = 47001},
      {.physical_node_id = 4702,
       .relational_node_id = 4702,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.set-right.typed.v1",
       .output_descriptor_ids = {4702},
       .causal_counter_id = 47002},
      {.physical_node_id = 4703,
       .relational_node_id = 4703,
       .node_kind = exec::PhysicalNodeKind::kSetOperation,
       .implementation_id = "setop." + OperationName(operation) + "-" +
                            quantifier_name +
                            ".ordinal.type-reconciled.typed.v1",
       .input_physical_node_ids = {4701, 4702},
       .output_descriptor_ids = {4703},
       .causal_counter_id = 47003},
  };
  request.selected_physical_node_id = 4703;
  request.left_batch = exec::MakeDescriptorBatch(
      {{"n", left_descriptor, true, 4701}},
      {{{Value(left_descriptor, "01")}},
       {{Value(left_descriptor, "2")}},
       {{Null(left_descriptor)}},
       {{Value(left_descriptor, "2")}}});
  request.right_batch = exec::MakeDescriptorBatch(
      {{"n", right_descriptor, true, 4702}},
      {{{Value(right_descriptor, "1")}},
       {{Value(right_descriptor, "3")}},
       {{Null(right_descriptor)}}});
  request.result_columns = {{"n", result_descriptor, true, 4703}};
  request.operation = operation;
  request.quantifier = quantifier;
  request.type_profile =
      exec::CanonicalSetOperationTypeProfile::kLosslessImplicit;
  request.maximum_output_row_count = 16;
  request.mga_authority = BindPhysicalAbiV2(&request.physical_dag);
  return request;
}

void ReplaceBatchDescriptor(exec::DescriptorBatch* batch,
                            const api::EngineDescriptor& descriptor) {
  if (batch == nullptr || batch->columns.empty()) return;
  batch->columns[0].descriptor = descriptor;
  for (auto& row : batch->rows) {
    row.values[0].descriptor = descriptor;
  }
}

// QOW-TEST-QRY-016-TYPE-V1
bool ValidateSetOperationTypeReconciliation() {
  bool passed = true;
  auto request = Request(exec::CanonicalSetOperationKind::kUnion,
                         exec::CanonicalSetOperationQuantifier::kDistinct);
  auto result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 4 &&
          result.eliminated_duplicate_row_count == 3 &&
          result.coerced_value_count == 4 &&
          result.reconciled_type_names ==
              std::vector<std::string>{"int64"} &&
          result.implementation_id ==
              "setop.union-distinct.ordinal.type-reconciled.typed.v1" &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[1].values[0].encoded_value == "2" &&
          result.output_batch.rows[2].values[0].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[3].values[0].encoded_value == "3" &&
          result.output_batch.rows[0].values[0]
                  .descriptor.descriptor_uuid.canonical ==
              request.result_columns[0].descriptor.descriptor_uuid.canonical &&
          result.mga_statement_context.visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              request.mga_authority.statement_context),
      "UNION DISTINCT did not reconcile, canonicalize, and retag values");

  result = exec::ExecuteCanonicalSetOperationDistinct(Request(
      exec::CanonicalSetOperationKind::kIntersect,
      exec::CanonicalSetOperationQuantifier::kDistinct));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 2 &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[1].values[0].state ==
              api::EngineValueState::sql_null,
      "INTERSECT DISTINCT did not compare reconciled typed membership");

  result = exec::ExecuteCanonicalSetOperationDistinct(Request(
      exec::CanonicalSetOperationKind::kExcept,
      exec::CanonicalSetOperationQuantifier::kDistinct));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[0].encoded_value == "2" &&
          result.eliminated_duplicate_row_count == 1,
      "EXCEPT DISTINCT did not use reconciled membership");

  result = exec::ExecuteCanonicalSetOperationAll(Request(
      exec::CanonicalSetOperationKind::kUnion,
      exec::CanonicalSetOperationQuantifier::kAll));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 7 &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[3].values[0].encoded_value == "2" &&
          result.coerced_value_count == 4,
      "UNION ALL did not publish reconciled multiplicities");

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  const auto narrow_result = Descriptor(
      request.result_columns[0].descriptor.descriptor_uuid.canonical, "int32",
      "019f0000-0000-7300-8000-000000004701");
  request.result_columns[0].descriptor = narrow_result;
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-016-TYPE-REFUSAL-V1" &&
          result.output_batch.rows.empty(),
      "narrowing reconciliation published rows");

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  const auto unsigned_left = Descriptor(
      request.left_batch.columns[0].descriptor.descriptor_uuid.canonical,
      "uint32", "019f0000-0000-7300-8000-000000004721");
  ReplaceBatchDescriptor(&request.left_batch, unsigned_left);
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-016-TYPE-REFUSAL-V1",
      "explicit-only signedness crossing was accepted");

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  request.left_batch.rows[0].values[0].encoded_value = "2147483648";
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-016-TYPE-REFUSAL-V1" &&
          result.output_batch.rows.empty(),
      "out-of-range source value reached set equality");

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  const auto unknown_left = Descriptor(
      request.left_batch.columns[0].descriptor.descriptor_uuid.canonical,
      "qow_unknown_integer",
      "019f0000-0000-7300-8000-000000004722");
  ReplaceBatchDescriptor(&request.left_batch, unknown_left);
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-016-TYPE-REFUSAL-V1",
      "unknown canonical type was accepted");

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  request.physical_dag.nodes[2].implementation_id =
      "setop.union-distinct.ordinal.typed.v1";
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-016-TYPE-REFUSAL-V1",
      "physical implementation omitted the reconciled type profile");

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  request.type_profile = exec::CanonicalSetOperationTypeProfile::kExact;
  request.physical_dag.nodes[2].implementation_id =
      "setop.union-distinct.ordinal.typed.v1";
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-016-DISTINCT-REFUSAL-V1",
      "exact profile silently reconciled mismatched descriptors");

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.executed_physical_node_id == 0 &&
                        !exec::PhysicalMgaStatementContextValid(
                            result.mga_statement_context),
                    "missing MGA transaction reached reconciled set access");

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  request.mga_authority.resolve_current = {};
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "missing current MGA resolver reached reconciled set access");

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  request.physical_dag.catalog_epoch_uuid =
      request.physical_dag.mga_statement_context
          .statement_metadata_snapshot_uuid;
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "catalog metadata substitution reached reconciled set access");

  request = Request(exec::CanonicalSetOperationKind::kUnion,
                    exec::CanonicalSetOperationQuantifier::kDistinct);
  request.physical_dag.local_transaction_id =
      static_cast<std::uint32_t>(kOwnerLocalTransactionId);
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "narrowed MGA identity reached reconciled set access");
  return passed;
}

}  // namespace

int main() {
  return ValidateSetOperationTypeReconciliation() ? EXIT_SUCCESS
                                                   : EXIT_FAILURE;
}
