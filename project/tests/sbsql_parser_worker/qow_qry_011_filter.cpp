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
  if (!condition) std::cerr << "QOW-TEST-QRY-011-FILTER-V1: " << detail << '\n';
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000fb01",
      "019f0000-0000-7200-8000-00000000fb02",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000fb03",
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
        "019f0000-0000-7200-8000-00000000fb04";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000fb05";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000fb06";
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

exec::CanonicalInt64SumFilterRequest Request() {
  const auto value_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001501",
      "019f0000-0000-7300-8000-000000001502");
  const auto result_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001503",
      "019f0000-0000-7300-8000-000000001504");

  exec::CanonicalInt64SumFilterRequest request;
  auto& aggregate = request.aggregate_request;
  aggregate.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001505";
  aggregate.physical_dag.root_physical_node_id = 1502;
  aggregate.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001511"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001512"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001513"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001514"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001515"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001516"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001517"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001518"},
  };
  aggregate.physical_dag.nodes = {
      {.physical_node_id = 1501,
       .relational_node_id = 1501,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {1501},
       .causal_counter_id = 15001},
      {.physical_node_id = 1502,
       .relational_node_id = 1502,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.sum-int64-filter.v1",
       .input_physical_node_ids = {1501},
       .output_descriptor_ids = {1502},
       .causal_counter_id = 15002},
  };
  aggregate.selected_physical_node_id = 1502;
  aggregate.input_batch = exec::MakeDescriptorBatch(
      {{"amount", value_descriptor, true, 1501}},
      {{{Value(value_descriptor, "10")}},
       {{Value(value_descriptor, "100")}},
       {{Value(value_descriptor, "50")}},
       {{Value(value_descriptor, "-3")}},
       {{Null(value_descriptor)}}});
  aggregate.value_expression_descriptor_id = 1501;
  aggregate.result_column = {"sum_amount", result_descriptor, true, 1502};
  request.row_truth_values = {
      api::EngineSqlTruthValue::true_value,
      api::EngineSqlTruthValue::false_value,
      api::EngineSqlTruthValue::unknown,
      api::EngineSqlTruthValue::true_value,
      api::EngineSqlTruthValue::true_value,
  };
  aggregate.mga_authority = BindPhysicalAbiV2(&aggregate.physical_dag);
  return request;
}

// QOW-TEST-QRY-011-FILTER-V1
bool ValidateAggregateFilter() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalInt64SumFilter(Request());
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 1502 &&
                        result.state.transition_count == 3 &&
                        result.state.non_null_count == 2 &&
                        result.state.accumulated_value == 7 &&
                        result.mga_statement_context
                                .visible_committed_high_watermark == 0 &&
                        exec::PhysicalMgaStatementContextEqual(
                            result.mga_statement_context,
                            Request().aggregate_request.mga_authority
                                .statement_context),
                    "aggregate FILTER did not apply TRUE-only 3VL transitions");

  auto request = Request();
  request.row_truth_values = {
      api::EngineSqlTruthValue::false_value,
      api::EngineSqlTruthValue::unknown,
      api::EngineSqlTruthValue::false_value,
      api::EngineSqlTruthValue::unknown,
      api::EngineSqlTruthValue::false_value,
  };
  result = exec::ExecuteCanonicalInt64SumFilter(request);
  passed &= Require(result.diagnostic.ok &&
                        result.state.transition_count == 0 &&
                        !result.state.has_value,
                    "FALSE/UNKNOWN aggregate FILTER rows changed state");

  request = Request();
  request.row_truth_values.pop_back();
  result = exec::ExecuteCanonicalInt64SumFilter(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "FILTER truth cardinality mismatch was accepted");

  request = Request();
  request.row_truth_values[1] =
      static_cast<api::EngineSqlTruthValue>(255);
  result = exec::ExecuteCanonicalInt64SumFilter(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "unbound FILTER truth value was accepted");

  request = Request();
  request.aggregate_request.input_batch.rows[1].values.clear();
  result = exec::ExecuteCanonicalInt64SumFilter(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "FALSE row hid ragged typed aggregate input");

  request = Request();
  request.aggregate_request.input_batch.rows[1].values[0].encoded_value =
      "malformed";
  result = exec::ExecuteCanonicalInt64SumFilter(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "FALSE row hid malformed typed aggregate input");

  request = Request();
  request.aggregate_request.maximum_transition_count = 4;
  result = exec::ExecuteCanonicalInt64SumFilter(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "FILTER scan resource bound was exceeded");

  request = Request();
  request.aggregate_request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalInt64SumFilter(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "aggregate FILTER bypassed MGA physical admission");

  request = Request();
  request.aggregate_request.physical_dag.mga_statement_context.complete =
      false;
  for (auto& node : request.aggregate_request.physical_dag.nodes) {
    node.mga_statement_context.complete = false;
  }
  request.aggregate_request.mga_authority.statement_context.complete = false;
  result = exec::ExecuteCanonicalInt64SumFilter(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "incomplete statement vector reached aggregate FILTER");

  request = Request();
  request.aggregate_request.physical_dag.mga_statement_context
      .inventory_authoritative = false;
  for (auto& node : request.aggregate_request.physical_dag.nodes) {
    node.mga_statement_context.inventory_authoritative = false;
  }
  request.aggregate_request.mga_authority.statement_context
      .inventory_authoritative = false;
  result = exec::ExecuteCanonicalInt64SumFilter(request);
  passed &= Require(!result.diagnostic.ok && !result.state.has_value,
                    "non-inventory statement vector reached aggregate FILTER");
  return passed;
}

}  // namespace

int main() {
  return ValidateAggregateFilter() ? EXIT_SUCCESS : EXIT_FAILURE;
}
