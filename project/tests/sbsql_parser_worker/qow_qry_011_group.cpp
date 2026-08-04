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
#include <utility>
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
  if (!condition) std::cerr << "QOW-TEST-QRY-011-GROUP-V1: " << detail << '\n';
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000f901",
      "019f0000-0000-7200-8000-00000000f902",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000f903",
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
        "019f0000-0000-7200-8000-00000000f904";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000f905";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000f906";
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
                                 const std::string& nullability,
                                 const std::string& canonical_type_name =
                                     "int64") {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = canonical_type_name;
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

exec::CanonicalInt64SumGroupRequest Request() {
  const auto key_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001401",
      "019f0000-0000-7300-8000-000000001402", "nullable");
  const auto value_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001403",
      "019f0000-0000-7300-8000-000000001404", "nullable");
  const auto key_result_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001405",
      "019f0000-0000-7300-8000-000000001402", "nullable");
  const auto sum_result_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001407",
      "019f0000-0000-7300-8000-000000001408", "nullable");

  exec::CanonicalInt64SumGroupRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001409";
  request.physical_dag.root_physical_node_id = 1402;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001411"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001412"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001413"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001414"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001415"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001416"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001417"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001418"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 1401,
       .relational_node_id = 1401,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {1401, 1402},
       .causal_counter_id = 14001},
      {.physical_node_id = 1402,
       .relational_node_id = 1402,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.sum-int64-grouping-sets.v1",
       .input_physical_node_ids = {1401},
       .output_descriptor_ids = {1403, 1404},
       .causal_counter_id = 14002},
  };
  request.selected_physical_node_id = 1402;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"group_key", key_descriptor, true, 1401},
       {"amount", value_descriptor, true, 1402}},
      {{{Value(key_descriptor, "1"), Value(value_descriptor, "10")}},
       {{Value(key_descriptor, "2"), Value(value_descriptor, "5")}},
       {{Value(key_descriptor, "1"), Null(value_descriptor)}},
       {{Null(key_descriptor), Value(value_descriptor, "7")}},
       {{Value(key_descriptor, "2"), Value(value_descriptor, "-2")}}});
  request.key_column = 0;
  request.key_expression_descriptor_id = 1401;
  request.value_column = 1;
  request.value_expression_descriptor_id = 1402;
  request.key_result_column =
      {"group_key", key_result_descriptor, true, 1403};
  request.sum_result_column =
      {"sum_amount", sum_result_descriptor, true, 1404};
  request.grouping_set_rule =
      exec::CanonicalInt64GroupingSetRule::key_and_grand_total;
  request.mga_authority = BindPhysicalAbiV2(&request.physical_dag);
  return request;
}

exec::CanonicalGroupedAggregateRuntimeRequest GroupedRegistryRequest() {
  const auto key_a = Descriptor(
      "019f0000-0000-7200-8000-000000001421",
      "019f0000-0000-7300-8000-000000001422", "nullable");
  const auto key_b = Descriptor(
      "019f0000-0000-7200-8000-000000001423",
      "019f0000-0000-7300-8000-000000001424", "nullable");
  const auto amount = Descriptor(
      "019f0000-0000-7200-8000-000000001425",
      "019f0000-0000-7300-8000-000000001426", "nullable");
  const auto result_a = Descriptor(
      "019f0000-0000-7200-8000-000000001427",
      "019f0000-0000-7300-8000-000000001422", "nullable");
  const auto result_b = Descriptor(
      "019f0000-0000-7200-8000-000000001429",
      "019f0000-0000-7300-8000-000000001424", "nullable");
  const auto average = Descriptor(
      "019f0000-0000-7200-8000-000000001431",
      "019f0000-0000-7300-8000-000000001432", "nullable", "real64");

  exec::CanonicalGroupedAggregateRuntimeRequest request;
  auto& aggregate = request.aggregate_request;
  aggregate.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001433";
  aggregate.physical_dag.root_physical_node_id = 1422;
  aggregate.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001434"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001435"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001436"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001437"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001438"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001439"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001440"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001441"},
  };
  aggregate.physical_dag.nodes = {
      {.physical_node_id = 1421,
       .relational_node_id = 1421,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {1421, 1422, 1423},
       .causal_counter_id = 14201},
      {.physical_node_id = 1422,
       .relational_node_id = 1422,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.registry-grouping-sets.v1",
       .input_physical_node_ids = {1421},
       .output_descriptor_ids = {1424, 1425, 1426},
       .causal_counter_id = 14202},
  };
  aggregate.selected_physical_node_id = 1422;
  aggregate.descriptor =
      {.abi_version = 1,
       .function = exec::CanonicalAggregateFunction::avg,
       .builtin_id = "sb.aggregate.avg",
       .function_uuid = "019de5fc-2400-78ac-b50c-45b832831004"};
  aggregate.input_batch = exec::MakeDescriptorBatch(
      {{"key_a", key_a, true, 1421},
       {"key_b", key_b, true, 1422},
       {"amount", amount, true, 1423}},
      {{{Value(key_a, "1"), Value(key_b, "10"), Value(amount, "2")}},
       {{Value(key_a, "1"), Value(key_b, "11"), Value(amount, "4")}},
       {{Value(key_a, "01"), Value(key_b, "10"), Value(amount, "6")}},
       {{Value(key_a, "2"), Value(key_b, "20"), Null(amount)}},
       {{Null(key_a), Value(key_b, "30"), Value(amount, "8")}}});
  aggregate.value_columns = {2};
  aggregate.value_expression_descriptor_ids = {1423};
  aggregate.result_column = {"average_amount", average, true, 1426};
  request.group_key_terms = {
      {.column = 0, .expression_descriptor_id = 1421},
      {.column = 1, .expression_descriptor_id = 1422},
  };
  request.group_result_columns = {
      {"key_a", result_a, true, 1424},
      {"key_b", result_b, true, 1425},
  };
  request.grouping_sets = {
      {.key_term_ordinals = {0, 1}},
      {.key_term_ordinals = {0}},
      {.key_term_ordinals = {}},
  };
  aggregate.mga_authority = BindPhysicalAbiV2(&aggregate.physical_dag);
  return request;
}

exec::CanonicalGroupedAggregateSetRuntimeRequest GroupedAggregateSetRequest() {
  exec::CanonicalGroupedAggregateSetRuntimeRequest request;
  request.first_aggregate = GroupedRegistryRequest();
  const auto count_result = Descriptor(
      "019f0000-0000-7200-8000-000000001442",
      "019f0000-0000-7300-8000-000000001443", "required");
  request.first_aggregate.aggregate_request.physical_dag.nodes.back()
      .output_descriptor_ids.push_back(1427);

  exec::CanonicalAggregateRuntimeRequest count;
  count.descriptor =
      {.abi_version = 1,
       .function = exec::CanonicalAggregateFunction::count,
       .builtin_id = "sb.aggregate.count",
       .function_uuid = "019de5fc-2400-784a-9aec-371f8b95b7ea",
       .count_star = true};
  count.result_column = {"filtered_count", count_result, false, 1427};
  using Truth = api::EngineSqlTruthValue;
  count.filter_truth_values = std::vector<Truth>{
      Truth::true_value, Truth::true_value, Truth::false_value,
      Truth::true_value, Truth::false_value};
  count.mga_authority =
      request.first_aggregate.aggregate_request.mga_authority;
  request.additional_aggregates = {std::move(count)};
  return request;
}

bool SameGroupedOutput(const exec::CanonicalGroupedAggregateRuntimeResult& left,
                       const exec::CanonicalGroupedAggregateRuntimeResult& right) {
  if (!left.diagnostic.ok || !right.diagnostic.ok ||
      left.output_batch.rows.size() != right.output_batch.rows.size()) {
    return false;
  }
  for (std::size_t row = 0; row < left.output_batch.rows.size(); ++row) {
    const auto& left_values = left.output_batch.rows[row].values;
    const auto& right_values = right.output_batch.rows[row].values;
    if (left_values.size() != right_values.size()) return false;
    for (std::size_t column = 0; column < left_values.size(); ++column) {
      if (left_values[column].state != right_values[column].state ||
          left_values[column].encoded_value !=
              right_values[column].encoded_value) {
        return false;
      }
    }
  }
  return true;
}

bool ValidateGroupedRegistryState() {
  bool passed = true;
  auto request = GroupedRegistryRequest();
  auto result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(result.diagnostic.ok &&
                        result.shared_state_authority_used &&
                        result.grouping_set_count == 3 &&
                        result.groups.size() == 8 &&
                        result.output_batch.rows.size() == 8 &&
                        result.grouping_set_transition_count == 15 &&
                        result.executed_physical_node_id == 1422 &&
                        result.mga_statement_context
                                .visible_committed_high_watermark == 0 &&
                        exec::PhysicalMgaStatementContextEqual(
                            result.mga_statement_context,
                            request.aggregate_request.mga_authority
                                .statement_context),
                    "multi-column registry grouping sets did not execute");
  passed &= Require(
      result.groups[0].grouping_id == 0 &&
          result.groups[0].grouping_indicators ==
              std::vector<bool>({false, false}) &&
          result.groups[0].source_row_count == 2 &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[0].values[1].encoded_value == "10" &&
          result.output_batch.rows[0].values[2].encoded_value == "4",
      "full composite group did not preserve decoded equality and AVG state");
  passed &= Require(
      result.output_batch.rows[2].values[2].state ==
          api::EngineValueState::sql_null &&
          result.groups[4].grouping_id == 1 &&
          result.groups[4].grouping_indicators ==
              std::vector<bool>({false, true}) &&
          result.output_batch.rows[4].values[1].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[4].values[2].encoded_value == "4",
      "ROLLUP-level groups or empty AVG state are wrong");
  passed &= Require(
      result.groups[6].grouping_id == 1 &&
          !result.groups[6].grouping_indicators[0] &&
          result.output_batch.rows[6].values[0].state ==
              api::EngineValueState::sql_null &&
          result.groups[7].grouping_id == 3 &&
          result.groups[7].grouping_indicators ==
              std::vector<bool>({true, true}) &&
          result.output_batch.rows[7].values[2].encoded_value == "5",
      "data NULL and grouping NULL are not distinguishable");

  request.aggregate_request.forced_strategy =
      exec::CanonicalAggregateExecutionStrategy::partitioned_combine;
  const auto partitioned =
      exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(SameGroupedOutput(result, partitioned),
                    "grouped serial/combine aggregate results diverged");

  request = GroupedRegistryRequest();
  request.aggregate_request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(result.diagnostic.ok && result.groups.size() == 1 &&
                        result.groups[0].grouping_id == 3 &&
                        result.output_batch.rows[0].values[2].state ==
                            api::EngineValueState::sql_null,
                    "empty grouping sets lost the grand-total aggregate row");

  request = GroupedRegistryRequest();
  request.maximum_group_count = 7;
  result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "grouped aggregate exceeded its group bound");

  request = GroupedRegistryRequest();
  request.maximum_grouping_set_transition_count = 14;
  result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "grouping-set transition bound was exceeded");

  request = GroupedRegistryRequest();
  request.maximum_grouping_key_comparison_count = 1;
  result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "grouping-key comparison bound was exceeded");

  request = GroupedRegistryRequest();
  request.maximum_combined_state_bytes = 1;
  result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "combined grouped state bound was exceeded");

  request = GroupedRegistryRequest();
  request.aggregate_request.distinct = true;
  request.maximum_combined_distinct_tuple_count = 1;
  result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "combined grouped DISTINCT bound was exceeded");

  request = GroupedRegistryRequest();
  request.aggregate_request.input_batch.rows[0].values[0].encoded_value =
      "malformed";
  result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "malformed composite grouping key was accepted");

  request = GroupedRegistryRequest();
  request.grouping_sets[0].key_term_ordinals = {1, 0};
  result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "noncanonical grouping-set ordinals were accepted");

  request = GroupedRegistryRequest();
  request.aggregate_request.physical_dag.nodes.back().implementation_id =
      "aggregate.registry.v1";
  result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "grouped aggregate implementation drift was accepted");

  request = GroupedRegistryRequest();
  request.aggregate_request.input_batch.rows.clear();
  request.grouping_sets = {{.key_term_ordinals = {0}}};
  request.aggregate_request.descriptor.builtin_id = "sb.aggregate.sum";
  result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "empty keyed input bypassed aggregate registry preflight");

  request = GroupedRegistryRequest();
  request.aggregate_request.filter_truth_values =
      std::vector<api::EngineSqlTruthValue>(4,
                                           api::EngineSqlTruthValue::true_value);
  result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "grouped aggregate FILTER cardinality drift was accepted");

  request = GroupedRegistryRequest();
  request.aggregate_request.mga_authority.statement_context.statement_uuid =
      "019f0000-0000-7200-8000-00000000f907";
  result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty() &&
                        result.output_batch.rows.empty(),
                    "cross-statement authority reached grouped aggregation");
  return passed;
}

// RCP-026-TEST-ORDINARY-GROUP-BY-IDENTITY-V1
bool ValidateOrdinaryGroupByIdentity() {
  bool passed = true;
  auto request = GroupedRegistryRequest();
  request.grouping_sets = {{.key_term_ordinals = {0, 1}}};
  const auto rows = request.aggregate_request.input_batch.rows;
  auto null_peer = rows[4];
  null_peer.values[2] = Value(
      request.aggregate_request.input_batch.columns[2].descriptor, "4");
  request.aggregate_request.input_batch.rows = {
      rows[0], rows[2], rows[4], std::move(null_peer)};

  auto result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(
      result.diagnostic.ok && result.grouping_set_count == 1 &&
          result.grouping_set_transition_count == 4 &&
          result.groups.size() == 2 && result.output_batch.rows.size() == 2 &&
          result.groups[0].source_row_count == 2 &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[0].values[1].encoded_value == "10" &&
          result.output_batch.rows[0].values[2].encoded_value == "4" &&
          result.groups[1].source_row_count == 2 &&
          result.output_batch.rows[1].values[0].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[1].values[1].encoded_value == "30" &&
          result.output_batch.rows[1].values[2].encoded_value == "6",
      "ordinary composite GROUP BY did not use decoded or SQL NULL equality");

  request = GroupedRegistryRequest();
  request.grouping_sets = {{.key_term_ordinals = {0, 1}}};
  request.aggregate_request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(result.diagnostic.ok && result.groups.empty() &&
                        result.output_batch.rows.empty(),
                    "empty ordinary GROUP BY invented a group");

  request = GroupedRegistryRequest();
  request.grouping_sets = {{.key_term_ordinals = {0, 1}}};
  request.group_result_columns[0].descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000001499;nullability=nullable";
  result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty() &&
                        result.output_batch.rows.empty(),
                    "group output type UUID drift was accepted");

  request = GroupedRegistryRequest();
  request.grouping_sets = {{.key_term_ordinals = {0, 1}}};
  request.group_result_columns[0].nullable = false;
  request.group_result_columns[0].descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000001422;nullability=non_null";
  result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty() &&
                        result.output_batch.rows.empty(),
                    "nullable group key was published as non-nullable");

  request = GroupedRegistryRequest();
  request.grouping_sets = {{.key_term_ordinals = {0, 1}}};
  request.group_result_columns[0].descriptor.descriptor_kind = "tuple";
  result = exec::ExecuteCanonicalGroupedAggregateRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty() &&
                        result.output_batch.rows.empty(),
                    "group output descriptor-kind drift was accepted");
  return passed;
}

bool ValidateGroupedAggregateSetState() {
  bool passed = true;
  auto request = GroupedAggregateSetRequest();
  auto result = exec::ExecuteCanonicalGroupedAggregateSetRuntime(request);
  passed &= Require(result.diagnostic.ok && result.aggregate_count == 2 &&
                        result.group_identity_proven &&
                        result.shared_state_authority_used &&
                        result.groups.size() == 8 &&
                        result.output_batch.columns.size() == 4 &&
                        result.output_batch.rows.size() == 8,
                    "multiple grouped registry aggregates did not execute");
  passed &= Require(
      result.output_batch.rows[0].values[2].encoded_value == "4" &&
          result.output_batch.rows[0].values[3].encoded_value == "1" &&
          result.output_batch.rows[3].values[3].encoded_value == "0" &&
          result.output_batch.rows[7].values[2].encoded_value == "5" &&
          result.output_batch.rows[7].values[3].encoded_value == "3" &&
          result.groups[7].aggregate_transition_counts ==
              std::vector<std::size_t>({5, 3}) &&
          result.groups[7].aggregate_state_bytes.size() == 2,
      "independent AVG/filtered COUNT states or evidence are wrong");

  request = GroupedAggregateSetRequest();
  request.maximum_aggregate_count = 1;
  result = exec::ExecuteCanonicalGroupedAggregateSetRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "aggregate-count bound was exceeded");

  request = GroupedAggregateSetRequest();
  request.maximum_combined_state_bytes = 1;
  result = exec::ExecuteCanonicalGroupedAggregateSetRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "aggregate-set combined state bound was exceeded");

  request = GroupedAggregateSetRequest();
  request.additional_aggregates[0].result_column.descriptor_id = 1426;
  result = exec::ExecuteCanonicalGroupedAggregateSetRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "duplicate aggregate result handle was accepted");

  request = GroupedAggregateSetRequest();
  request.additional_aggregates[0].physical_dag.root_physical_node_id = 99;
  result = exec::ExecuteCanonicalGroupedAggregateSetRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "additional aggregate shadow authority was accepted");

  request = GroupedAggregateSetRequest();
  std::swap(request.first_aggregate.aggregate_request.physical_dag.nodes.back()
                .output_descriptor_ids[2],
            request.first_aggregate.aggregate_request.physical_dag.nodes.back()
                .output_descriptor_ids[3]);
  result = exec::ExecuteCanonicalGroupedAggregateSetRuntime(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "aggregate physical result order drift was accepted");
  return passed;
}

// QOW-TEST-QRY-011-GROUP-V1
bool ValidateTypedGroupingState() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalInt64SumGroups(Request());
  passed &= Require(result.diagnostic.ok && result.groups.size() == 4 &&
                        result.executed_physical_node_id == 1402 &&
                        result.mga_statement_context
                                .visible_committed_high_watermark == 0 &&
                        exec::PhysicalMgaStatementContextEqual(
                            result.mga_statement_context,
                            Request().mga_authority.statement_context),
                    "typed grouping sets did not execute four states");
  passed &= Require(!result.groups[0].is_grand_total &&
                        result.groups[0].group_key.encoded_value == "1" &&
                        result.groups[0].sum_state.accumulated_value == 10 &&
                        result.groups[0].sum_state.transition_count == 2 &&
                        result.groups[0].sum_state.non_null_count == 1,
                    "group key 1 state is wrong");
  passed &= Require(!result.groups[1].is_grand_total &&
                        result.groups[1].group_key.encoded_value == "2" &&
                        result.groups[1].sum_state.accumulated_value == 3,
                    "group key 2 state is wrong");
  passed &= Require(!result.groups[2].is_grand_total &&
                        result.groups[2].group_key.state ==
                            api::EngineValueState::sql_null &&
                        result.groups[2].sum_state.accumulated_value == 7,
                    "actual SQL NULL key group was lost");
  passed &= Require(result.groups[3].is_grand_total &&
                        result.groups[3].grouping_set_ordinal == 1 &&
                        result.groups[3].group_key.state ==
                            api::EngineValueState::sql_null &&
                        result.groups[3].sum_state.accumulated_value == 20 &&
                        result.groups[3].sum_state.transition_count == 5 &&
                        result.groups[3].sum_state.non_null_count == 4,
                    "grand-total grouping set is wrong or ambiguous");

  auto request = Request();
  request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalInt64SumGroups(request);
  passed &= Require(result.diagnostic.ok && result.groups.size() == 1 &&
                        result.groups[0].is_grand_total &&
                        !result.groups[0].sum_state.has_value,
                    "empty grouping sets lost the grand-total state");

  request = Request();
  request.input_batch.rows.clear();
  request.grouping_set_rule = exec::CanonicalInt64GroupingSetRule::key_only;
  result = exec::ExecuteCanonicalInt64SumGroups(request);
  passed &= Require(result.diagnostic.ok && result.groups.empty(),
                    "empty ordinary GROUP BY invented a group");

  request = Request();
  request.grouping_set_rule = exec::CanonicalInt64GroupingSetRule::key_only;
  request.input_batch.columns[0].nullable = false;
  request.input_batch.columns[0].descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000001402;nullability=non_null";
  request.input_batch.rows.erase(request.input_batch.rows.begin() + 3);
  for (auto& row : request.input_batch.rows) {
    row.values[0].descriptor = request.input_batch.columns[0].descriptor;
  }
  request.key_result_column.nullable = false;
  request.key_result_column.descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000001402;nullability=non_null";
  result = exec::ExecuteCanonicalInt64SumGroups(request);
  passed &= Require(result.diagnostic.ok && result.groups.size() == 2,
                    "non-null ordinary GROUP BY did not derive a non-null key output" +
                        (result.diagnostic.ok
                             ? std::string{}
                             : ": " + result.diagnostic.diagnostic_code +
                                   ": " + result.diagnostic.detail));

  request = Request();
  request.maximum_group_count = 3;
  result = exec::ExecuteCanonicalInt64SumGroups(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "group resource limit was exceeded");

  request = Request();
  request.maximum_transition_count = 9;
  result = exec::ExecuteCanonicalInt64SumGroups(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "grouping-set transition limit ignored duplicate states");

  request = Request();
  request.input_batch.rows[0].values[0].encoded_value = "bad-key";
  result = exec::ExecuteCanonicalInt64SumGroups(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "malformed typed grouping key was accepted");

  request = Request();
  const auto& value_descriptor = request.input_batch.columns[1].descriptor;
  request.input_batch.rows =
      {{{Value(request.input_batch.columns[0].descriptor, "1"),
         Value(value_descriptor,
               std::to_string(std::numeric_limits<std::int64_t>::max()))}},
       {{Value(request.input_batch.columns[0].descriptor, "1"),
         Value(value_descriptor, "1")}}};
  result = exec::ExecuteCanonicalInt64SumGroups(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "grouped SUM overflow published partial groups");

  request = Request();
  request.grouping_set_rule =
      static_cast<exec::CanonicalInt64GroupingSetRule>(255);
  result = exec::ExecuteCanonicalInt64SumGroups(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "unknown grouping-set rule was accepted");

  request = Request();
  request.key_result_column.descriptor_id = 1499;
  result = exec::ExecuteCanonicalInt64SumGroups(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "mismatched grouped output handle was accepted");

  request = Request();
  request.key_result_column.descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000001499;nullability=nullable";
  result = exec::ExecuteCanonicalInt64SumGroups(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "specialized group output type UUID drift was accepted");

  request = Request();
  request.key_result_column.nullable = false;
  request.key_result_column.descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000001402;nullability=non_null";
  result = exec::ExecuteCanonicalInt64SumGroups(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "specialized group output nullability drift was accepted");

  request = Request();
  request.key_result_column.descriptor.descriptor_kind = "tuple";
  result = exec::ExecuteCanonicalInt64SumGroups(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "specialized group output descriptor kind drift was accepted");

  request = Request();
  request.mga_authority.origin = exec::CanonicalMgaAuthorityOrigin::kMissing;
  result = exec::ExecuteCanonicalInt64SumGroups(request);
  passed &= Require(!result.diagnostic.ok && result.groups.empty(),
                    "missing statement authority reached grouped aggregation");
  return passed;
}

}  // namespace

#ifndef QOW_QRY_011_GROUP_FIXTURE_ONLY
int main() {
  return ValidateTypedGroupingState() && ValidateGroupedRegistryState() &&
                 ValidateGroupedAggregateSetState() &&
                 ValidateOrdinaryGroupByIdentity()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
#endif
