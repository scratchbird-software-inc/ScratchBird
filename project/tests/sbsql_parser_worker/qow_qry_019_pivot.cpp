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

constexpr std::uint64_t kOwner = 0xffff'ffff'ffff'ff00ULL;
constexpr std::uint64_t kMemoryBudgetBytes = 32ULL * 1024ULL * 1024ULL;

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-QRY-019-PIVOT-V1: " << detail << '\n';
  return condition;
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
                            std::string encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = std::move(encoded);
  value.state = api::EngineValueState::value;
  return value;
}

exec::CanonicalExecutionMgaAuthority Bind(exec::TypedPhysicalNodeDag* dag) {
  dag->admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000019011"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000019012"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000019013"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000019003"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000019014"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000019015"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000019016"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000019017"},
  };
  const exec::PhysicalMgaStatementContext context{
      "019f0000-0000-7200-8000-000000019001",
      "019f0000-0000-7200-8000-000000019002",
      "019f0000-0000-7200-8000-000000019003",
      "019f0000-0000-7200-8000-000000019004",
      kOwner,
      0,
      kOwner - 8,
      kOwner - 16,
      kOwner - 16,
      kOwner - 16,
      {kOwner - 8, kOwner},
      {},
      "statement_stable",
      kOwner + 1,
      true,
      true,
      true};
  dag->abi_version = 2;
  dag->local_transaction_id = kOwner;
  dag->bound_sblr_tree_uuid =
      "019f0000-0000-7200-8000-000000019011";
  dag->catalog_epoch_uuid =
      "019f0000-0000-7200-8000-000000019012";
  dag->security_context_uuid =
      "019f0000-0000-7200-8000-000000019013";
  dag->capability_snapshot_uuid =
      "019f0000-0000-7200-8000-000000019014";
  dag->resource_snapshot_uuid =
      "019f0000-0000-7200-8000-000000019015";
  dag->statistics_snapshot_uuid =
      "019f0000-0000-7200-8000-000000019016";
  dag->route_snapshot_uuid =
      "019f0000-0000-7200-8000-000000019017";
  dag->catalog_generation = dag->security_epoch = dag->policy_epoch = 1;
  dag->resource_epoch = dag->statistics_generation = dag->route_epoch = 1;
  dag->route_generation = 1;
  dag->memory_budget_bytes = kMemoryBudgetBytes;
  dag->optimizer_published = true;
  dag->immutable_node_identity_validated = true;
  dag->capability_validated_before_access = true;
  dag->mga_statement_context = context;
  for (auto& node : dag->nodes) {
    node.mga_statement_context = context;
    node.selected_alternative_uuid =
        "019f0000-0000-7200-8000-000000019021";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-000000019022";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-000000019023";
    node.memory_bytes_required = kMemoryBudgetBytes;
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

exec::CanonicalPivotRequest Request() {
  const auto group = Descriptor(
      "019f0000-0000-7300-8000-000000019101",
      "019f0000-0000-7400-8000-000000019101", "int64");
  const auto for_key = Descriptor(
      "019f0000-0000-7300-8000-000000019102",
      "019f0000-0000-7400-8000-000000019102", "int64");
  const auto amount = Descriptor(
      "019f0000-0000-7300-8000-000000019103",
      "019f0000-0000-7400-8000-000000019103", "int64");
  const auto result_group = group;
  const auto first_sum = Descriptor(
      "019f0000-0000-7300-8000-000000019105",
      "019f0000-0000-7400-8000-000000019103", "int64");
  const auto second_sum = Descriptor(
      "019f0000-0000-7300-8000-000000019106",
      "019f0000-0000-7400-8000-000000019103", "int64");

  exec::CanonicalPivotRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000019030";
  request.physical_dag.root_physical_node_id = 1902;
  request.physical_dag.nodes = {
      {.physical_node_id = 1901,
       .relational_node_id = 1901,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.materialize.canonical.v1",
       .output_descriptor_ids = {1, 2, 3},
       .causal_counter_id = 19001},
      {.physical_node_id = 1902,
       .relational_node_id = 1902,
       .node_kind = exec::PhysicalNodeKind::kPivot,
       .implementation_id = "pivot.canonical.exclude-nulls.typed.v1",
       .input_physical_node_ids = {1901},
       .output_descriptor_ids = {4, 5, 6},
       .causal_counter_id = 19002},
  };
  request.selected_physical_node_id = 1902;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"group_key", group, true, 1},
       {"for_key", for_key, true, 2},
       {"amount", amount, true, 3}},
      {{{Value(group, "1"), Value(for_key, "1"), Value(amount, "10")}},
       {{Value(group, "1"), Value(for_key, "2"), Value(amount, "20")}},
       {{Value(group, "1"), Value(for_key, "1"), Value(amount, "5")}},
       {{Value(group, "2"), Value(for_key, "2"), Value(amount, "7")}},
       {{Value(group, "2"), Value(for_key, "3"), Value(amount, "100")}}});
  request.group_key_terms = {{.column = 0, .expression_descriptor_id = 1}};
  request.for_key_terms = {{.column = 1, .expression_descriptor_id = 2}};
  request.in_items = {{{Value(for_key, "1")}}, {{Value(for_key, "2")}}};
  request.result_columns = {
      {"group_key", result_group, true, 4},
      {"first_sum", first_sum, true, 5},
      {"second_sum", second_sum, true, 6},
  };
  const auto* sum = exec::LookupCanonicalAggregateByFunctionV1(
      exec::CanonicalAggregateFunction::sum);
  if (sum == nullptr) std::abort();
  exec::CanonicalPivotAggregateBinding aggregate;
  aggregate.aggregate_template.descriptor =
      {sum->abi_version, sum->function, sum->builtin_id,
       sum->function_uuid, false};
  aggregate.aggregate_template.value_columns = {2};
  aggregate.aggregate_template.value_expression_descriptor_ids = {3};
  aggregate.aggregate_template.forced_strategy =
      exec::CanonicalAggregateExecutionStrategy::serial;
  aggregate.aggregate_template.maximum_transition_count = 64;
  aggregate.aggregate_template.maximum_state_bytes = 4096;
  aggregate.result_columns_by_item = {
      {"first_sum", first_sum, true, 5},
      {"second_sum", second_sum, true, 6},
  };
  request.aggregates = {std::move(aggregate)};
  request.maximum_key_comparison_count = 128;
  request.maximum_total_aggregate_transition_count = 64;
  request.maximum_output_row_count = 8;
  request.maximum_output_cell_count = 64;
  request.mga_authority = Bind(&request.physical_dag);
  return request;
}

bool ValidatePivot() {
  auto request = Request();
  const auto first = exec::ExecuteCanonicalPivot(request);
  const auto replay = exec::ExecuteCanonicalPivot(request);
  if (!first.diagnostic.ok) {
    std::cerr << "QOW-TEST-QRY-019-PIVOT-V1: first diagnostic "
              << first.diagnostic.diagnostic_code << ": "
              << first.diagnostic.detail << '\n';
  }
  bool passed = true;
  passed &= Require(
      first.diagnostic.ok && first.output_batch.rows.size() == 2 &&
          first.output_batch.rows[0].values[0].encoded_value == "1" &&
          first.output_batch.rows[0].values[1].encoded_value == "15" &&
          first.output_batch.rows[0].values[2].encoded_value == "20" &&
          first.output_batch.rows[1].values[0].encoded_value == "2" &&
          first.output_batch.rows[1].values[1].state ==
              api::EngineValueState::sql_null &&
          first.output_batch.rows[1].values[2].encoded_value == "7" &&
          first.group_count == 2 && first.in_item_count == 2 &&
          first.aggregate_count == 1 && first.matched_input_row_count == 4 &&
          first.aggregate_transition_count == 4 &&
          first.executed_physical_node_id == 1902 &&
          first.causal_counter_id == 19002,
      "PIVOT did not group, match fixed IN keys, or use canonical SUM semantics");
  bool replay_matches =
      replay.diagnostic.ok &&
      replay.output_batch.rows.size() == first.output_batch.rows.size();
  if (replay_matches) {
    for (std::size_t row_index = 0;
         row_index < first.output_batch.rows.size() && replay_matches;
         ++row_index) {
      const auto& expected = first.output_batch.rows[row_index].values;
      const auto& actual = replay.output_batch.rows[row_index].values;
      replay_matches = actual.size() == expected.size();
      for (std::size_t column_index = 0;
           column_index < expected.size() && replay_matches;
           ++column_index) {
        replay_matches =
            actual[column_index].state == expected[column_index].state &&
            actual[column_index].is_null == expected[column_index].is_null &&
            actual[column_index].encoded_value ==
                expected[column_index].encoded_value &&
            actual[column_index].descriptor.descriptor_uuid.canonical ==
                expected[column_index].descriptor.descriptor_uuid.canonical;
      }
    }
  }
  passed &= Require(replay_matches, "PIVOT replay changed typed output");

  request.maximum_total_aggregate_transition_count = 3;
  const auto bounded = exec::ExecuteCanonicalPivot(request);
  passed &= Require(
      !bounded.diagnostic.ok && bounded.output_batch.rows.empty() &&
          bounded.diagnostic.diagnostic_code ==
              "SBLR.PLAN_TREE.RESOURCE_LIMIT",
      "PIVOT aggregate transition exhaustion published a partial result");

  request = Request();
  request.in_items[1] = request.in_items[0];
  const auto duplicate = exec::ExecuteCanonicalPivot(request);
  passed &= Require(
      !duplicate.diagnostic.ok && duplicate.output_batch.rows.empty() &&
          duplicate.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-019-PIVOT-IN-V1",
      "PIVOT admitted overlapping fixed IN keys");
  return passed;
}

}  // namespace

int main() { return ValidatePivot() ? EXIT_SUCCESS : EXIT_FAILURE; }
