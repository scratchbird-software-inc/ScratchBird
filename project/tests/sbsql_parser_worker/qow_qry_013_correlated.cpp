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
    std::cerr << "QOW-TEST-QRY-013-CORRELATED-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000e651",
      "019f0000-0000-7200-8000-00000000e652",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000e653",
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
  dag->memory_budget_bytes = 32ULL * 1024ULL * 1024ULL;
  dag->optimizer_published = true;
  dag->immutable_node_identity_validated = true;
  dag->capability_validated_before_access = true;
  const auto context =
      StatementContext(dag->admission_evidence.at(3).evidence_uuid);
  dag->mga_statement_context = context;
  for (auto& node : dag->nodes) {
    node.mga_statement_context = context;
    node.selected_alternative_uuid =
        "019f0000-0000-7200-8000-00000000e654";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000e655";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000e656";
    node.memory_bytes_required = 32ULL * 1024ULL * 1024ULL;
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

exec::CanonicalCorrelatedSubqueryRequest Request() {
  const auto outer_key = Descriptor(
      "019f0000-0000-7200-8000-000000003001",
      "019f0000-0000-7300-8000-000000003002", "int64");
  const auto outer_payload = Descriptor(
      "019f0000-0000-7200-8000-000000003003",
      "019f0000-0000-7300-8000-000000003004", "text");
  const auto inner_key = Descriptor(
      "019f0000-0000-7200-8000-000000003005",
      "019f0000-0000-7300-8000-000000003006", "int64");
  const auto inner_payload = Descriptor(
      "019f0000-0000-7200-8000-000000003007",
      "019f0000-0000-7300-8000-000000003008", "text");

  exec::CanonicalCorrelatedSubqueryRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000003009";
  request.physical_dag.root_physical_node_id = 3003;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000003011"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000003012"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000003013"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000003014"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000003015"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000003016"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000003017"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000003018"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 3001,
       .relational_node_id = 3001,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.correlated-outer.typed.v1",
       .output_descriptor_ids = {3001, 3002},
       .causal_counter_id = 30001},
      {.physical_node_id = 3002,
       .relational_node_id = 3002,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.correlated-inner.typed.v1",
       .output_descriptor_ids = {3003, 3004},
       .causal_counter_id = 30002},
      {.physical_node_id = 3003,
       .relational_node_id = 3003,
       .node_kind = exec::PhysicalNodeKind::kSubquery,
       .implementation_id = "subquery.correlated.int64-equality.typed.v1",
       .input_physical_node_ids = {3001, 3002},
       .output_descriptor_ids = {3003, 3004},
       .causal_counter_id = 30003},
  };
  request.selected_physical_node_id = 3003;
  request.outer_batch = exec::MakeDescriptorBatch(
      {{"outer_key", outer_key, true, 3001},
       {"outer_payload", outer_payload, true, 3002}},
      {{{Value(outer_key, "1"), Value(outer_payload, "outer-a")}},
       {{Null(outer_key), Value(outer_payload, "outer-null")}},
       {{Value(outer_key, "2"), Value(outer_payload, "outer-b")}},
       {{Value(outer_key, "01"), Value(outer_payload, "outer-alias")}}});
  request.inner_batch = exec::MakeDescriptorBatch(
      {{"inner_key", inner_key, true, 3003},
       {"inner_payload", inner_payload, true, 3004}},
      {{{Value(inner_key, "01"), Value(inner_payload, "inner-a")}},
       {{Value(inner_key, "2"), Value(inner_payload, "inner-b")}},
       {{Value(inner_key, "1"), Value(inner_payload, "inner-c")}},
       {{Null(inner_key), Value(inner_payload, "inner-null")}}});
  request.outer_binding_column = 0;
  request.outer_binding_expression_descriptor_id = 3001;
  request.inner_reference_column = 0;
  request.inner_reference_expression_descriptor_id = 3003;
  request.maximum_scope_execution_count = 4;
  request.maximum_comparison_count = 16;
  request.maximum_result_row_count = 5;
  request.mga_authority = BindPhysicalAbiV2(&request.physical_dag);
  return request;
}

exec::CanonicalCorrelatedSubqueryRequest Real64Request() {
  auto request = Request();
  const auto outer_key = Descriptor(
      "019f0000-0000-7200-8000-000000003021",
      "019f0000-0000-7300-8000-000000003022", "real64");
  const auto inner_key = Descriptor(
      "019f0000-0000-7200-8000-000000003023",
      "019f0000-0000-7300-8000-000000003024", "real64");
  request.outer_batch.columns[0].descriptor = outer_key;
  request.inner_batch.columns[0].descriptor = inner_key;
  request.outer_batch.rows[0].values[0] = Value(outer_key, "1.5");
  request.outer_batch.rows[1].values[0] = Null(outer_key);
  request.outer_batch.rows[2].values[0] = Value(outer_key, "2.5");
  request.outer_batch.rows[3].values[0] = Value(outer_key, "1.50");
  request.inner_batch.rows[0].values[0] = Value(inner_key, "1.50");
  request.inner_batch.rows[1].values[0] = Value(inner_key, "2.5");
  request.inner_batch.rows[2].values[0] = Value(inner_key, "1.5");
  request.inner_batch.rows[3].values[0] = Null(inner_key);
  request.physical_dag.nodes[2].implementation_id =
      "subquery.correlated.equality.typed.v1";
  return request;
}

// QOW-TEST-QRY-013-CORRELATED-V1
bool ValidateCorrelatedSubquery() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalCorrelatedSubquery(Request());
  passed &= Require(
      result.diagnostic.ok && result.scope_execution_count == 4 &&
          result.comparison_count == 9 && result.result_row_count == 5 &&
          result.scopes.size() == 4 &&
          result.scopes[0].outer_row_index == 0 &&
          result.scopes[0].bound_outer_value.encoded_value == "1" &&
          result.scopes[0].output_batch.rows.size() == 2 &&
          result.scopes[0].output_batch.rows[0].values[1].encoded_value ==
              "inner-a" &&
          result.scopes[0].output_batch.rows[1].values[1].encoded_value ==
              "inner-c" &&
          result.scopes[1].output_batch.rows.empty() &&
          result.scopes[2].output_batch.rows.size() == 1 &&
          result.scopes[3].output_batch.rows.size() == 2 &&
          result.selected_plan_uuid ==
              "019f0000-0000-7200-8000-000000003009" &&
          result.executed_physical_node_id == 3003 &&
          result.causal_counter_id == 30003 &&
          result.mga_statement_context.visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request().mga_authority.statement_context),
      "correlated scopes did not bind and execute in outer-row order");

  auto request = Request();
  request.inner_batch.rows.clear();
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(result.diagnostic.ok &&
                        result.scope_execution_count == 4 &&
                        result.comparison_count == 0 &&
                        result.result_row_count == 0 &&
                        result.scopes.size() == 4 &&
                        result.scopes[0].output_batch.columns.size() == 2 &&
                        result.scopes[0].output_batch.rows.empty(),
                    "empty inner relation lost per-outer typed scopes");

  request = Request();
  request.outer_batch.rows.clear();
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(result.diagnostic.ok && result.scopes.empty() &&
                        result.scope_execution_count == 0 &&
                        result.comparison_count == 0,
                    "empty outer relation invented correlated scopes");

  request = Request();
  request.inner_batch.rows[2].values[0].encoded_value = "bad";
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.scopes.empty() &&
                        result.scope_execution_count == 0 &&
                        result.result_row_count == 0,
                    "malformed later inner key published earlier scopes");

  request = Real64Request();
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(
      result.diagnostic.ok && result.scope_execution_count == 4 &&
          result.comparison_count == 9 && result.result_row_count == 5 &&
          result.scopes[0].output_batch.rows.size() == 2 &&
          result.scopes[2].output_batch.rows.size() == 1 &&
          result.scopes[3].output_batch.rows.size() == 2,
      "descriptor-compatible real64 correlated equality was refused");

  request = Real64Request();
  request.inner_batch.columns[0].descriptor.canonical_type_name = "int64";
  request.inner_batch.rows[0].values[0].descriptor.canonical_type_name =
      "int64";
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.scopes.empty(),
                    "descriptor-incompatible correlation was accepted");

  request = Request();
  request.maximum_scope_execution_count = 3;
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "correlated scope-execution bound was exceeded");

  request = Request();
  request.maximum_comparison_count = 15;
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "correlated comparison bound was exceeded");

  request = Request();
  request.maximum_result_row_count = 4;
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.scopes.empty(),
                    "correlated result bound published partial scopes");

  request = Request();
  request.outer_binding_expression_descriptor_id = 3099;
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "outer correlated binding handle drift was accepted");

  request = Request();
  request.physical_dag.nodes[2].implementation_id =
      "subquery.table.materialize.typed.v1";
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "wrong correlated physical profile was accepted");

  request = Request();
  request.physical_dag.nodes[2].output_descriptor_ids = {3001, 3002};
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "correlated output descriptor drift was accepted");

  request = Request();
  request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.scopes.empty() &&
                        result.scope_execution_count == 0 &&
                        result.result_row_count == 0 &&
                        result.selected_plan_uuid.empty() &&
                        !exec::PhysicalMgaStatementContextValid(
                            result.mga_statement_context),
                    "missing engine MGA transaction was accepted");

  request = Request();
  request.physical_dag.mga_statement_context
      .statement_metadata_snapshot_uuid =
      request.physical_dag.catalog_epoch_uuid;
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.scopes.empty(),
                    "catalog metadata alias reached correlated access");

  request = Request();
  request.physical_dag.nodes[1].mga_statement_context.statement_uuid =
      "019f0000-0000-7200-8000-00000000e657";
  result = exec::ExecuteCanonicalCorrelatedSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.scopes.empty(),
                    "node-local cross-statement context reached correlated access");
  return passed;
}

}  // namespace

int main() {
  return ValidateCorrelatedSubquery() ? EXIT_SUCCESS : EXIT_FAILURE;
}
