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
    std::cerr << "QOW-TEST-QRY-011-ORDERED-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000f801",
      "019f0000-0000-7200-8000-00000000f802",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000f803",
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
        "019f0000-0000-7200-8000-00000000f804";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000f805";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000f806";
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

exec::CanonicalInt64SumOrderedRequest Request() {
  const auto value_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001701",
      "019f0000-0000-7300-8000-000000001702");
  const auto order_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001703",
      "019f0000-0000-7300-8000-000000001704");
  const auto result_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001705",
      "019f0000-0000-7300-8000-000000001706");

  exec::CanonicalInt64SumOrderedRequest request;
  auto& aggregate = request.aggregate_request;
  aggregate.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001707";
  aggregate.physical_dag.root_physical_node_id = 1703;
  aggregate.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001711"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001712"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001713"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001714"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001715"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001716"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001717"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001718"},
  };
  aggregate.physical_dag.nodes = {
      {.physical_node_id = 1702,
       .relational_node_id = 1702,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {1701, 1702},
       .causal_counter_id = 17002},
      {.physical_node_id = 1703,
       .relational_node_id = 1703,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.sum-int64-ordered.v1",
       .input_physical_node_ids = {1702},
       .output_descriptor_ids = {1703},
       .causal_counter_id = 17003},
  };
  aggregate.selected_physical_node_id = 1703;
  aggregate.input_batch = exec::MakeDescriptorBatch(
      {{"amount", value_descriptor, true, 1701},
       {"order_key", order_descriptor, true, 1702}},
      {{{Value(value_descriptor, "10"), Value(order_descriptor, "2")}},
       {{Value(value_descriptor, "20"), Value(order_descriptor, "1")}},
       {{Value(value_descriptor, "30"), Value(order_descriptor, "1")}},
       {{Value(value_descriptor, "40"), Null(order_descriptor)}},
       {{Null(value_descriptor), Value(order_descriptor, "0")}}});
  aggregate.value_expression_descriptor_id = 1701;
  aggregate.result_column = {"sum_amount", result_descriptor, true, 1703};
  request.order_column = 1;
  request.order_expression_descriptor_id = 1702;
  request.deterministic_tie_evidence_uuid =
      "019f0000-0000-7200-8000-000000001719";
  aggregate.mga_authority = BindPhysicalAbiV2(&aggregate.physical_dag);
  return request;
}

// QOW-TEST-QRY-011-ORDERED-V1
bool ValidateAggregateOrderedArguments() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalInt64SumOrdered(Request());
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 1703 &&
                        result.ordered_input_row_indices ==
                            std::vector<std::size_t>({4, 1, 2, 0, 3}) &&
                        result.state.transition_count == 5 &&
                        result.state.non_null_count == 4 &&
                        result.state.accumulated_value == 100 &&
                        result.mga_statement_context
                                .visible_committed_high_watermark == 0 &&
                        exec::PhysicalMgaStatementContextEqual(
                            result.mga_statement_context,
                            Request().aggregate_request.mga_authority
                                .statement_context),
                    "ascending ordered arguments lost NULL or stable-tie order");

  auto request = Request();
  request.direction = exec::CanonicalDescriptorOrderDirection::descending;
  request.null_placement = exec::CanonicalDescriptorNullPlacement::first;
  result = exec::ExecuteCanonicalInt64SumOrdered(request);
  passed &= Require(result.diagnostic.ok &&
                        result.ordered_input_row_indices ==
                            std::vector<std::size_t>({3, 0, 1, 2, 4}),
                    "descending ordered arguments changed explicit NULL order");

  request = Request();
  request.aggregate_request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalInt64SumOrdered(request);
  passed &= Require(result.diagnostic.ok &&
                        result.ordered_input_row_indices.empty() &&
                        result.state.transition_count == 0 &&
                        !result.state.has_value,
                    "empty ordered aggregate changed SUM empty state");

  request = Request();
  request.aggregate_request.input_batch.rows[1].values[1].encoded_value =
      "malformed";
  result = exec::ExecuteCanonicalInt64SumOrdered(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.ordered_input_row_indices.empty(),
                    "malformed ordered key published transition order");

  request = Request();
  request.aggregate_request.input_batch.rows[1].values[0].encoded_value =
      "malformed";
  result = exec::ExecuteCanonicalInt64SumOrdered(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "malformed aggregate value published ordered state");

  request = Request();
  request.aggregate_request.input_batch.rows[1].values.pop_back();
  result = exec::ExecuteCanonicalInt64SumOrdered(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.ordered_input_row_indices.empty(),
                    "ragged input published ordered transition evidence");

  request = Request();
  request.direction =
      static_cast<exec::CanonicalDescriptorOrderDirection>(255);
  result = exec::ExecuteCanonicalInt64SumOrdered(request);
  passed &= Require(!result.diagnostic.ok,
                    "unbound ordered-argument direction was accepted");

  request = Request();
  request.deterministic_tie_evidence_uuid.clear();
  result = exec::ExecuteCanonicalInt64SumOrdered(request);
  passed &= Require(!result.diagnostic.ok,
                    "missing ordered-argument tie evidence was accepted");

  request = Request();
  request.maximum_pair_comparisons = 24;
  result = exec::ExecuteCanonicalInt64SumOrdered(request);
  passed &= Require(!result.diagnostic.ok,
                    "ordered-argument comparison bound was exceeded");

  request = Request();
  request.aggregate_request.maximum_transition_count = 4;
  result = exec::ExecuteCanonicalInt64SumOrdered(request);
  passed &= Require(!result.diagnostic.ok,
                    "ordered-argument transition bound was exceeded");

  request = Request();
  request.order_expression_descriptor_id = 1701;
  result = exec::ExecuteCanonicalInt64SumOrdered(request);
  passed &= Require(!result.diagnostic.ok,
                    "unbound ordered-argument key handle was accepted");

  request = Request();
  request.aggregate_request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalInt64SumOrdered(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "ordered aggregate bypassed MGA physical admission");

  request = Request();
  auto swapped =
      request.aggregate_request.physical_dag.mga_statement_context;
  std::swap(swapped.statement_snapshot_uuid,
            swapped.statement_metadata_snapshot_uuid);
  SetStatementContext(&request.aggregate_request.physical_dag, swapped);
  result = exec::ExecuteCanonicalInt64SumOrdered(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value &&
                        result.ordered_input_row_indices.empty(),
                    "swapped snapshot identities published ordered state");

  request = Request();
  request.aggregate_request.physical_dag.statement_snapshot_id = 1;
  result = exec::ExecuteCanonicalInt64SumOrdered(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value &&
                        result.ordered_input_row_indices.empty(),
                    "widened zero-high-water alias published ordered state");
  return passed;
}

}  // namespace

int main() {
  return ValidateAggregateOrderedArguments() ? EXIT_SUCCESS : EXIT_FAILURE;
}
