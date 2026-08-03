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
  if (!condition) std::cerr << "QOW-TEST-QRY-011-EMPTY-V1: " << detail << '\n';
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000fc01",
      "019f0000-0000-7200-8000-00000000fc02",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000fc03",
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

void SetStatementContext(
    exec::TypedPhysicalNodeDag* dag,
    const exec::PhysicalMgaStatementContext& context) {
  dag->mga_statement_context = context;
  for (auto& node : dag->nodes) node.mga_statement_context = context;
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
  const auto context = StatementContext(
      dag->admission_evidence.at(3).evidence_uuid);
  SetStatementContext(dag, context);
  for (auto& node : dag->nodes) {
    node.selected_alternative_uuid =
        "019f0000-0000-7200-8000-00000000fc04";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000fc05";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000fc06";
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

exec::CanonicalInt64SumStateRequest StateRequest(
    const std::vector<api::EngineTypedValue>& values) {
  const auto value_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001301",
      "019f0000-0000-7300-8000-000000001302");
  const auto result_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001303",
      "019f0000-0000-7300-8000-000000001304");

  exec::CanonicalInt64SumStateRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001305";
  request.physical_dag.root_physical_node_id = 1302;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001311"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001312"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001313"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001314"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001315"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001316"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001317"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001318"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 1301,
       .relational_node_id = 1301,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {1301},
       .causal_counter_id = 13001},
      {.physical_node_id = 1302,
       .relational_node_id = 1302,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.sum-int64-final.v1",
       .input_physical_node_ids = {1301},
       .output_descriptor_ids = {1302},
       .causal_counter_id = 13002},
  };
  request.selected_physical_node_id = 1302;
  std::vector<exec::DescriptorTuple> rows;
  for (const auto& value : values) rows.push_back({{value}});
  request.input_batch = exec::MakeDescriptorBatch(
      {{"amount", value_descriptor, true, 1301}}, std::move(rows));
  request.value_expression_descriptor_id = 1301;
  request.result_column = {"sum_amount", result_descriptor, true, 1302};
  request.mga_authority = BindPhysicalAbiV2(&request.physical_dag);
  return request;
}

exec::CanonicalInt64SumFinalizeResult Finalize(
    const exec::CanonicalInt64SumStateRequest& state_request,
    exec::CanonicalInt64SumAggregateState state) {
  exec::CanonicalInt64SumFinalizeRequest request;
  request.physical_dag = state_request.physical_dag;
  request.selected_physical_node_id = state_request.selected_physical_node_id;
  request.state = std::move(state);
  request.mga_authority = state_request.mga_authority;
  return exec::ExecuteCanonicalInt64SumFinalize(request);
}

// QOW-TEST-QRY-011-EMPTY-V1
bool ValidateAggregateEmptyFinalization() {
  bool passed = true;
  auto state_request = StateRequest({});
  auto state_result = exec::ExecuteCanonicalInt64SumState(state_request);
  auto result = Finalize(state_request, state_result.state);
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 1302 &&
                        result.output_batch.rows.size() == 1 &&
                        result.mga_statement_context
                                .visible_committed_high_watermark == 0 &&
                        exec::PhysicalMgaStatementContextEqual(
                            result.mga_statement_context,
                            state_request.mga_authority.statement_context),
                    "empty SUM state did not emit one aggregate row");
  const auto& empty_value = result.output_batch.rows[0].values[0];
  passed &= Require(empty_value.state == api::EngineValueState::sql_null &&
                        empty_value.is_null && empty_value.encoded_value.empty() &&
                        empty_value.binary_value.empty() &&
                        result.output_batch.columns[0].descriptor_id == 1302,
                    "empty SUM did not retain canonical typed SQL NULL");

  const auto descriptor = state_request.input_batch.columns[0].descriptor;
  state_request = StateRequest({Null(descriptor)});
  state_result = exec::ExecuteCanonicalInt64SumState(state_request);
  result = Finalize(state_request, state_result.state);
  passed &= Require(result.diagnostic.ok &&
                        result.output_batch.rows[0].values[0].state ==
                            api::EngineValueState::sql_null,
                    "all-NULL SUM did not finalize as SQL NULL");

  state_request = StateRequest({Value(descriptor, "0")});
  state_result = exec::ExecuteCanonicalInt64SumState(state_request);
  result = Finalize(state_request, state_result.state);
  passed &= Require(result.diagnostic.ok &&
                        result.output_batch.rows[0].values[0].state ==
                            api::EngineValueState::value &&
                        result.output_batch.rows[0].values[0].encoded_value ==
                            "0",
                    "real aggregate zero was confused with empty input");

  state_request =
      StateRequest({Value(descriptor, "10"), Null(descriptor),
                    Value(descriptor, "-3")});
  state_result = exec::ExecuteCanonicalInt64SumState(state_request);
  result = Finalize(state_request, state_result.state);
  passed &= Require(result.diagnostic.ok &&
                        result.output_batch.rows[0].values[0].encoded_value ==
                            "7",
                    "nonempty SUM state finalized to the wrong value");

  auto malformed_state = state_result.state;
  malformed_state.has_value = false;
  result = Finalize(state_request, malformed_state);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "contradictory SUM state was finalized");

  malformed_state = state_result.state;
  malformed_state.result_column.nullable = false;
  result = Finalize(state_request, malformed_state);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "non-nullable empty SUM descriptor was accepted");

  malformed_state = state_result.state;
  malformed_state.result_column.descriptor_id = 1399;
  result = Finalize(state_request, malformed_state);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "mismatched SUM output handle was finalized");

  auto final_request = exec::CanonicalInt64SumFinalizeRequest{
      state_request.physical_dag, state_request.selected_physical_node_id,
      state_result.state};
  final_request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalInt64SumFinalize(final_request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "SUM finalization bypassed MGA physical admission");

  final_request = {state_request.physical_dag,
                   state_request.selected_physical_node_id,
                   state_result.state,
                   state_request.mga_authority};
  auto stale_current = final_request.mga_authority.statement_context;
  stale_current.current = false;
  final_request.mga_authority.resolve_current = [stale_current] {
    exec::CanonicalMgaCurrentResolution current;
    current.statement_context = stale_current;
    return current;
  };
  result = exec::ExecuteCanonicalInt64SumFinalize(final_request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "stale current statement context published SUM output");

  final_request = {state_request.physical_dag,
                   state_request.selected_physical_node_id,
                   state_result.state,
                   state_request.mga_authority};
  auto another_statement = final_request.mga_authority.statement_context;
  another_statement.statement_uuid =
      "019f0000-0000-7200-8000-00000000fc07";
  final_request.mga_authority.statement_context = another_statement;
  final_request.mga_authority.resolve_current = [another_statement] {
    exec::CanonicalMgaCurrentResolution current;
    current.statement_context = another_statement;
    return current;
  };
  result = exec::ExecuteCanonicalInt64SumFinalize(final_request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "cross-statement authority published SUM output");
  return passed;
}

}  // namespace

int main() {
  return ValidateAggregateEmptyFinalization() ? EXIT_SUCCESS : EXIT_FAILURE;
}
