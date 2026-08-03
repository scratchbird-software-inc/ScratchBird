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
  if (!condition) std::cerr << "QOW-TEST-QRY-011-STATE-V1: " << detail << '\n';
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000fd01",
      "019f0000-0000-7200-8000-00000000fd02",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000fd03",
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
        "019f0000-0000-7200-8000-00000000fd04";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000fd05";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000fd06";
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
                                 const std::string& nullability) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=" + nullability;
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

exec::CanonicalInt64SumStateRequest Request() {
  const auto value_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001201",
      "019f0000-0000-7300-8000-000000001202", "nullable");
  const auto result_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001203",
      "019f0000-0000-7300-8000-000000001204", "nullable");

  exec::CanonicalInt64SumStateRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001205";
  request.physical_dag.root_physical_node_id = 1202;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001211"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001212"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001213"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001214"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001215"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001216"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001217"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001218"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 1201,
       .relational_node_id = 1201,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {1201},
       .causal_counter_id = 12001},
      {.physical_node_id = 1202,
       .relational_node_id = 1202,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.sum-int64-state.v1",
       .input_physical_node_ids = {1201},
       .output_descriptor_ids = {1202},
       .causal_counter_id = 12002},
  };
  request.selected_physical_node_id = 1202;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"amount", value_descriptor, true, 1201}},
      {{{Value(value_descriptor, "10")}},
       {{Null(value_descriptor)}},
       {{Value(value_descriptor, "-3")}}});
  request.value_column = 0;
  request.value_expression_descriptor_id = 1201;
  request.result_column = {"sum_amount", result_descriptor, true, 1202};
  request.mga_authority = BindPhysicalAbiV2(&request.physical_dag);
  return request;
}

// QOW-TEST-QRY-011-STATE-V1
bool ValidateAggregateTransitionState() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalInt64SumState(Request());
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 1202 &&
                        result.causal_counter_id == 12002,
                    "typed physical SUM state did not execute");
  passed &= Require(result.state.transition_count == 3 &&
                        result.state.non_null_count == 2 &&
                        result.state.accumulated_value == 7 &&
                        result.state.has_value &&
                        result.state.value_expression_descriptor_id == 1201 &&
                        result.state.result_column.descriptor_id == 1202 &&
                        result.mga_statement_context
                                .visible_committed_high_watermark == 0 &&
                        exec::PhysicalMgaStatementContextEqual(
                            result.mga_statement_context,
                            Request().mga_authority.statement_context),
                    "SUM transition state lost rows, NULL semantics, or handles");

  auto request = Request();
  request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(result.diagnostic.ok &&
                        result.state.transition_count == 0 &&
                        result.state.non_null_count == 0 &&
                        !result.state.has_value,
                    "empty SUM state invented a value");

  request = Request();
  request.input_batch.rows = {{{Null(request.input_batch.columns[0].descriptor)}}};
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(result.diagnostic.ok &&
                        result.state.transition_count == 1 &&
                        result.state.non_null_count == 0 &&
                        !result.state.has_value,
                    "all-NULL SUM state invented a numeric transition");

  request = Request();
  const auto& descriptor = request.input_batch.columns[0].descriptor;
  request.input_batch.rows =
      {{{Value(descriptor,
               std::to_string(std::numeric_limits<std::int64_t>::max()))}},
       {{Value(descriptor, "1")}}};
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value &&
                        result.state.transition_count == 0,
                    "SUM overflow published a partial transition state");

  request = Request();
  request.input_batch.rows[0].values[0].encoded_value = "not-an-int64";
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "malformed SUM operand was accepted");

  request = Request();
  request.value_expression_descriptor_id = 1202;
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "mismatched aggregate expression handle was accepted");

  request = Request();
  request.result_column.nullable = false;
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "non-nullable SUM result descriptor was accepted");

  request = Request();
  request.maximum_transition_count = 2;
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "SUM transition resource bound was exceeded");

  request = Request();
  auto unsorted = request.physical_dag.mga_statement_context;
  std::swap(unsorted.active_excluded_local_transaction_ids[0],
            unsorted.active_excluded_local_transaction_ids[1]);
  SetStatementContext(&request.physical_dag, unsorted);
  request.mga_authority.statement_context = unsorted;
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "unordered active exclusions reached aggregate state");

  request = Request();
  auto overlap = request.physical_dag.mga_statement_context;
  overlap.in_doubt_excluded_local_transaction_ids =
      {kOwnerLocalTransactionId};
  SetStatementContext(&request.physical_dag, overlap);
  request.mga_authority.statement_context = overlap;
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "overlapping active/in-doubt exclusions reached aggregate state");

  request = Request();
  auto truncated = request.physical_dag.mga_statement_context;
  truncated.publication_inventory_next_local_transaction_id =
      static_cast<std::uint32_t>(kInventoryNextLocalTransactionId);
  SetStatementContext(&request.physical_dag, truncated);
  request.mga_authority.statement_context = truncated;
  result = exec::ExecuteCanonicalInt64SumState(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "truncated inventory ceiling reached aggregate state");
  return passed;
}

}  // namespace

int main() {
  return ValidateAggregateTransitionState() ? EXIT_SUCCESS : EXIT_FAILURE;
}
