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

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-QRY-011-GROUP-V1: " << detail << '\n';
  return condition;
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

exec::CanonicalInt64SumGroupRequest Request() {
  const auto key_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001401",
      "019f0000-0000-7300-8000-000000001402", "nullable");
  const auto value_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001403",
      "019f0000-0000-7300-8000-000000001404", "nullable");
  const auto key_result_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001405",
      "019f0000-0000-7300-8000-000000001406", "nullable");
  const auto sum_result_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001407",
      "019f0000-0000-7300-8000-000000001408", "nullable");

  exec::CanonicalInt64SumGroupRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001409";
  request.physical_dag.root_physical_node_id = 1402;
  request.physical_dag.local_transaction_id = 1403;
  request.physical_dag.statement_snapshot_id = 1404;
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
  return request;
}

// QOW-TEST-QRY-011-GROUP-V1
bool ValidateTypedGroupingState() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalInt64SumGroups(Request());
  passed &= Require(result.diagnostic.ok && result.groups.size() == 4 &&
                        result.executed_physical_node_id == 1402,
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
  return passed;
}

}  // namespace

int main() {
  return ValidateTypedGroupingState() ? EXIT_SUCCESS : EXIT_FAILURE;
}
