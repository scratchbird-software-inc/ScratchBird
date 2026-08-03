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
    std::cerr << "QOW-TEST-QRY-013-SCALAR-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000e611",
      "019f0000-0000-7200-8000-00000000e612",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000e613",
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
        "019f0000-0000-7200-8000-00000000e614";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000e615";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000e616";
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
                                 const std::string& type_uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
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

exec::CanonicalScalarSubqueryRequest Request() {
  const auto scalar = Descriptor(
      "019f0000-0000-7200-8000-000000002601",
      "019f0000-0000-7300-8000-000000002602");

  exec::CanonicalScalarSubqueryRequest request;
  auto& table = request.table_request;
  table.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000002603";
  table.physical_dag.root_physical_node_id = 2602;
  table.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000002611"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000002612"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000002613"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000002614"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000002615"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000002616"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000002617"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000002618"},
  };
  table.physical_dag.nodes = {
      {.physical_node_id = 2601,
       .relational_node_id = 2601,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.scalar-subquery-input.typed.v1",
       .output_descriptor_ids = {2601},
       .causal_counter_id = 26001},
      {.physical_node_id = 2602,
       .relational_node_id = 2602,
       .node_kind = exec::PhysicalNodeKind::kSubquery,
       .implementation_id = "subquery.scalar.cardinality.typed.v1",
       .input_physical_node_ids = {2601},
       .output_descriptor_ids = {2601},
       .causal_counter_id = 26002},
  };
  table.selected_physical_node_id = 2602;
  table.input_batch = exec::MakeDescriptorBatch(
      {{"scalar_source", scalar, true, 2601}},
      {{{Value(scalar, "02")}}});
  table.maximum_materialized_row_count = 2;
  table.mga_authority = BindPhysicalAbiV2(&table.physical_dag);
  request.value_expression_descriptor_id = 2601;
  request.result_column = {"scalar_result", scalar, true, 2601};
  return request;
}

// QOW-TEST-QRY-013-SCALAR-V1
bool ValidateScalarSubquery() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalScalarSubquery(Request());
  passed &= Require(
      result.diagnostic.ok && result.source_row_count == 1 &&
          result.output_batch.columns.size() == 1 &&
          result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[0].state ==
              api::EngineValueState::value &&
          result.output_batch.rows[0].values[0].encoded_value == "02" &&
          result.selected_plan_uuid ==
              "019f0000-0000-7200-8000-000000002603" &&
          result.executed_physical_node_id == 2602 &&
          result.causal_counter_id == 26002 &&
          result.mga_statement_context.visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request().table_request.mga_authority.statement_context),
      "one-row scalar subquery did not preserve its typed value");

  auto request = Request();
  request.table_request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(
      result.diagnostic.ok && result.source_row_count == 0 &&
          result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[0].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[0].values[0].is_null &&
          result.output_batch.rows[0].values[0].encoded_value.empty(),
      "zero-row scalar subquery did not produce one typed SQL NULL");

  request = Request();
  request.table_request.input_batch.rows[0].values[0] =
      Null(request.table_request.input_batch.columns[0].descriptor);
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(
      result.diagnostic.ok && result.source_row_count == 1 &&
          result.output_batch.rows[0].values[0].state ==
              api::EngineValueState::sql_null,
      "one-row SQL NULL scalar subquery lost its canonical NULL state");

  request = Request();
  request.table_request.input_batch.rows.push_back(
      request.table_request.input_batch.rows.front());
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-013-SCALAR-REFUSAL-V1" &&
          result.output_batch.rows.empty() && result.source_row_count == 0 &&
          result.selected_plan_uuid.empty() &&
          result.executed_physical_node_id == 0,
      "many-row scalar subquery published a first-row substitute");

  request = Request();
  request.result_column.nullable = false;
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "non-nullable scalar result admitted the zero-row case");

  request = Request();
  request.value_expression_descriptor_id = 2699;
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "unresolved scalar expression handle was accepted");

  request = Request();
  request.result_column.descriptor.encoded_descriptor += ";drift=true";
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "scalar result descriptor drift was accepted");

  request = Request();
  const auto extra = Descriptor(
      "019f0000-0000-7200-8000-000000002621",
      "019f0000-0000-7300-8000-000000002622");
  request.table_request.physical_dag.nodes[0].output_descriptor_ids.push_back(
      2602);
  request.table_request.physical_dag.nodes[1].output_descriptor_ids.push_back(
      2602);
  request.table_request.input_batch.columns.push_back(
      {"extra", extra, true, 2602});
  request.table_request.input_batch.rows[0].values.push_back(Value(extra, "9"));
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "multi-column scalar subquery was accepted");

  request = Request();
  request.table_request.maximum_materialized_row_count = 0;
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "failed table materialization published a scalar");

  request = Request();
  request.table_request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.source_row_count == 0 &&
                        result.selected_plan_uuid.empty() &&
                        result.executed_physical_node_id == 0 &&
                        !exec::PhysicalMgaStatementContextValid(
                            result.mga_statement_context),
                    "missing engine MGA transaction was accepted");

  request = Request();
  std::swap(request.table_request.physical_dag.mga_statement_context
                .statement_snapshot_uuid,
            request.table_request.physical_dag.mga_statement_context
                .statement_metadata_snapshot_uuid);
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "swapped MGA snapshot roles reached scalar access");

  request = Request();
  request.table_request.mga_authority.statement_context.complete = false;
  result = exec::ExecuteCanonicalScalarSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "incomplete MGA authority reached scalar access");
  return passed;
}

}  // namespace

int main() {
  return ValidateScalarSubquery() ? EXIT_SUCCESS : EXIT_FAILURE;
}
