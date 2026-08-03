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
    std::cerr << "QOW-TEST-QRY-013-TABLE-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000e601",
      "019f0000-0000-7200-8000-00000000e602",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000e603",
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
      true,
  };
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
        "019f0000-0000-7200-8000-00000000e604";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000e605";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000e606";
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
                                 const std::string& type_uuid,
                                 const std::string& type_name) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type_name;
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=nullable";
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

api::EngineTypedValue Null(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.is_null = true;
  value.state = api::EngineValueState::sql_null;
  return value;
}

exec::CanonicalTableSubqueryRequest Request() {
  const auto key = Descriptor(
      "019f0000-0000-7200-8000-000000002501",
      "019f0000-0000-7300-8000-000000002502", "int64");
  const auto payload = Descriptor(
      "019f0000-0000-7200-8000-000000002503",
      "019f0000-0000-7300-8000-000000002504", "text");

  exec::CanonicalTableSubqueryRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000002505";
  request.physical_dag.root_physical_node_id = 2502;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000002511"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000002512"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000002513"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000002514"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000002515"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000002516"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000002517"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000002518"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 2501,
       .relational_node_id = 2501,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.table-subquery-input.typed.v1",
       .output_descriptor_ids = {2501, 2502},
       .causal_counter_id = 25001},
      {.physical_node_id = 2502,
       .relational_node_id = 2502,
       .node_kind = exec::PhysicalNodeKind::kSubquery,
       .implementation_id = "subquery.table.materialize.typed.v1",
       .input_physical_node_ids = {2501},
       .output_descriptor_ids = {2501, 2502},
       .causal_counter_id = 25002},
  };
  request.selected_physical_node_id = 2502;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"table_key", key, false, 2501},
       {"table_payload", payload, true, 2502}},
      {{{Value(key, "1"), Value(payload, "alpha")}},
       {{Value(key, "02"), Null(payload)}},
       {{Value(key, "3"), Value(payload, "omega")}}});
  request.maximum_materialized_row_count = 3;
  request.mga_authority = BindPhysicalAbiV2(&request.physical_dag);
  return request;
}

// QOW-TEST-QRY-013-TABLE-V1
bool ValidateTableSubquery() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalTableSubquery(Request());
  passed &= Require(
      result.diagnostic.ok && result.materialized_row_count == 3 &&
          result.output_batch.columns.size() == 2 &&
          result.output_batch.rows.size() == 3 &&
          result.output_batch.rows[1].values[0].encoded_value == "02" &&
          result.output_batch.rows[1].values[1].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[1].values[1].is_null &&
          result.selected_plan_uuid ==
              "019f0000-0000-7200-8000-000000002505" &&
          result.executed_physical_node_id == 2502 &&
          result.causal_counter_id == 25002 &&
          result.mga_statement_context.visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request().mga_authority.statement_context),
      "canonical table subquery did not preserve its typed relational result");

  auto request = Request();
  request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(result.diagnostic.ok &&
                        result.materialized_row_count == 0 &&
                        result.output_batch.columns.size() == 2 &&
                        result.output_batch.rows.empty(),
                    "empty table subquery lost its canonical schema");

  request = Request();
  request.maximum_materialized_row_count = 2;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "QOW-DIAG-QRY-013-TABLE-REFUSAL-V1" &&
                        result.output_batch.rows.empty() &&
                        result.materialized_row_count == 0,
                    "table-subquery row bound published partial output");

  request = Request();
  request.maximum_materialized_row_count = 0;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "zero table-subquery row bound was accepted");

  request = Request();
  request.selected_physical_node_id = 2501;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "non-root table-subquery selection was accepted");

  request = Request();
  request.physical_dag.nodes[1].node_kind =
      exec::PhysicalNodeKind::kProject;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "non-subquery physical route was accepted");

  request = Request();
  request.physical_dag.nodes[1].output_descriptor_ids[1] = 2599;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "table-subquery output descriptor drift was accepted");

  request = Request();
  request.input_batch.rows[0].values[0].descriptor =
      request.input_batch.columns[1].descriptor;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "typed value descriptor drift was accepted");

  request = Request();
  request.input_batch.rows[1].values[1].is_null = false;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "malformed SQL NULL was accepted");

  request = Request();
  request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.materialized_row_count == 0 &&
                        result.selected_plan_uuid.empty() &&
                        result.executed_physical_node_id == 0 &&
                        !exec::PhysicalMgaStatementContextValid(
                            result.mga_statement_context),
                    "missing engine MGA statement context was accepted");

  request = Request();
  request.mga_authority.resolve_current = {};
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "missing current MGA resolver reached table access");

  request = Request();
  request.physical_dag.catalog_epoch_uuid =
      request.physical_dag.mga_statement_context
          .statement_metadata_snapshot_uuid;
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "catalog metadata substitution reached table access");

  request = Request();
  request.physical_dag.local_transaction_id =
      static_cast<std::uint32_t>(kOwnerLocalTransactionId);
  result = exec::ExecuteCanonicalTableSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "narrowed MGA transaction identity reached table access");
  return passed;
}

}  // namespace

int main() {
  return ValidateTableSubquery() ? EXIT_SUCCESS : EXIT_FAILURE;
}
