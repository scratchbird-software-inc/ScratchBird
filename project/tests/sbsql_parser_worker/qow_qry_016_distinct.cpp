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
    std::cerr << "QOW-TEST-QRY-016-DISTINCT-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {"019f0000-0000-7200-8000-00000000f831",
          "019f0000-0000-7200-8000-00000000f832",
          statement_snapshot_uuid,
          "019f0000-0000-7200-8000-00000000f833",
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
        "019f0000-0000-7200-8000-00000000f834";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000f835";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000f836";
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

api::EngineDescriptor Descriptor(const std::string& uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000004400;"
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

api::EngineTypedValue Null(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.is_null = true;
  value.state = api::EngineValueState::sql_null;
  return value;
}

exec::CanonicalSetOperationAllRequest Request(
    const exec::CanonicalSetOperationKind operation,
    const exec::CanonicalSetOperationAlignment alignment =
        exec::CanonicalSetOperationAlignment::kOrdinal) {
  const auto left_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004401");
  const auto right_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004402");
  const auto result_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004403");

  exec::CanonicalSetOperationAllRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000004404";
  request.physical_dag.root_physical_node_id = 4403;
  request.physical_dag.local_transaction_id = 4404;
  request.physical_dag.statement_snapshot_id = 4405;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000004411"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000004412"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000004413"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000004414"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000004415"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000004416"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000004417"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000004418"},
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
  const std::string alignment_name =
      alignment == exec::CanonicalSetOperationAlignment::kByName
          ? "by-name"
          : "ordinal";
  request.physical_dag.nodes = {
      {.physical_node_id = 4401,
       .relational_node_id = 4401,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.set-left.typed.v1",
       .output_descriptor_ids = {4401},
       .causal_counter_id = 44001},
      {.physical_node_id = 4402,
       .relational_node_id = 4402,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.set-right.typed.v1",
       .output_descriptor_ids = {4402},
       .causal_counter_id = 44002},
      {.physical_node_id = 4403,
       .relational_node_id = 4403,
       .node_kind = exec::PhysicalNodeKind::kSetOperation,
       .implementation_id = "setop." + operation_name +
                            "-distinct." + alignment_name + ".typed.v1",
       .input_physical_node_ids = {4401, 4402},
       .output_descriptor_ids = {4403},
       .causal_counter_id = 44003},
  };
  request.selected_physical_node_id = 4403;
  request.left_batch = exec::MakeDescriptorBatch(
      {{"n", left_descriptor, true, 4401}},
      {{{Value(left_descriptor, "1")}},
       {{Value(left_descriptor, "01")}},
       {{Value(left_descriptor, "2")}},
       {{Value(left_descriptor, "2")}},
       {{Null(left_descriptor)}},
       {{Null(left_descriptor)}},
       {{Value(left_descriptor, "4")}}});
  request.right_batch = exec::MakeDescriptorBatch(
      {{"n", right_descriptor, true, 4402}},
      {{{Value(right_descriptor, "1")}},
       {{Value(right_descriptor, "3")}},
       {{Value(right_descriptor, "3")}},
       {{Null(right_descriptor)}},
       {{Value(right_descriptor, "5")}}});
  request.result_columns = {{"n", result_descriptor, true, 4403}};
  request.operation = operation;
  request.alignment = alignment;
  request.quantifier = exec::CanonicalSetOperationQuantifier::kDistinct;
  request.maximum_output_row_count = 16;
  request.mga_authority = BindPhysicalAbiV2(&request.physical_dag);
  return request;
}

// QOW-TEST-QRY-016-DISTINCT-V1
bool ValidateSetOperationDistinct() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalSetOperationDistinct(
      Request(exec::CanonicalSetOperationKind::kUnion));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 6 &&
          result.eliminated_duplicate_row_count == 6 &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[1].values[0].encoded_value == "2" &&
          result.output_batch.rows[2].values[0].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[3].values[0].encoded_value == "4" &&
          result.output_batch.rows[4].values[0].encoded_value == "3" &&
          result.output_batch.rows[5].values[0].encoded_value == "5" &&
          result.mga_statement_context.visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request(exec::CanonicalSetOperationKind::kUnion)
                  .mga_authority.statement_context),
      "UNION DISTINCT did not emit first typed representatives in order");

  result = exec::ExecuteCanonicalSetOperationDistinct(
      Request(exec::CanonicalSetOperationKind::kIntersect));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 2 &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[1].values[0].state ==
              api::EngineValueState::sql_null &&
          result.eliminated_duplicate_row_count == 2,
      "INTERSECT DISTINCT did not emit one row per shared typed key");

  result = exec::ExecuteCanonicalSetOperationDistinct(
      Request(exec::CanonicalSetOperationKind::kExcept));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 2 &&
          result.output_batch.rows[0].values[0].encoded_value == "2" &&
          result.output_batch.rows[1].values[0].encoded_value == "4" &&
          result.eliminated_duplicate_row_count == 1,
      "EXCEPT DISTINCT used multiset subtraction instead of membership");

  result = exec::ExecuteCanonicalSetOperationDistinct(Request(
      exec::CanonicalSetOperationKind::kUnion,
      exec::CanonicalSetOperationAlignment::kByName));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 6 &&
          result.implementation_id ==
              "setop.union-distinct.by-name.typed.v1",
      "DISTINCT BY NAME did not use the admitted aligned profile");

  auto request = Request(exec::CanonicalSetOperationKind::kUnion);
  request.maximum_output_row_count = 5;
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "DISTINCT resource excess published a partial set");

  request = Request(exec::CanonicalSetOperationKind::kUnion);
  request.left_batch.rows[1].values[0].encoded_value = "malformed";
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "duplicate position hid malformed typed input");

  request = Request(exec::CanonicalSetOperationKind::kUnion);
  result = exec::ExecuteCanonicalSetOperationAll(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-016-ALL-REFUSAL-V1",
      "DISTINCT request entered the ALL execution boundary");

  request = Request(exec::CanonicalSetOperationKind::kExcept);
  request.physical_dag.nodes[2].implementation_id =
      "setop.except-all.ordinal.typed.v1";
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok,
                    "DISTINCT silently degraded to ALL execution");

  request = Request(exec::CanonicalSetOperationKind::kUnion);
  request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.executed_physical_node_id == 0 &&
                        !exec::PhysicalMgaStatementContextValid(
                            result.mga_statement_context),
                    "missing MGA transaction reached DISTINCT access");

  request = Request(exec::CanonicalSetOperationKind::kUnion);
  request.mga_authority.resolve_current = {};
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "missing current MGA resolver reached DISTINCT access");

  request = Request(exec::CanonicalSetOperationKind::kUnion);
  request.physical_dag.catalog_epoch_uuid =
      request.physical_dag.mga_statement_context
          .statement_metadata_snapshot_uuid;
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "catalog metadata substitution reached DISTINCT access");

  request = Request(exec::CanonicalSetOperationKind::kUnion);
  request.physical_dag.local_transaction_id =
      static_cast<std::uint32_t>(kOwnerLocalTransactionId);
  result = exec::ExecuteCanonicalSetOperationDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "narrowed MGA identity reached DISTINCT access");
  return passed;
}

}  // namespace

int main() { return ValidateSetOperationDistinct() ? EXIT_SUCCESS
                                                    : EXIT_FAILURE; }
