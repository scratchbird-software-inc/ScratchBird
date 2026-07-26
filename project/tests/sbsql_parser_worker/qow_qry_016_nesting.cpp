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

constexpr std::string_view kPlanUuid =
    "019f0000-0000-7200-8000-000000004506";

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-016-NESTING-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000004500;"
      "nullability=nullable";
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

exec::DescriptorBatch Batch(const std::string& name,
                            const api::EngineDescriptor& descriptor,
                            const std::uint32_t descriptor_id,
                            const std::initializer_list<std::string> values) {
  exec::DescriptorBatch batch;
  batch.columns = {{name, descriptor, true, descriptor_id}};
  for (const auto& value : values) {
    batch.rows.push_back({{Value(descriptor, value)}});
  }
  return batch;
}

std::string OperationName(const exec::CanonicalSetOperationKind operation) {
  switch (operation) {
    case exec::CanonicalSetOperationKind::kUnion:
      return "union";
    case exec::CanonicalSetOperationKind::kIntersect:
      return "intersect";
    case exec::CanonicalSetOperationKind::kExcept:
      return "except";
  }
  return {};
}

exec::CanonicalSetOperationAllRequest OperationTemplate(
    const exec::DescriptorBatch& left,
    const exec::DescriptorBatch& right,
    exec::ExecutorColumnDescriptor result_column,
    const exec::CanonicalSetOperationKind operation,
    const std::uint64_t root_node_id,
    const std::uint64_t causal_counter_id) {
  exec::CanonicalSetOperationAllRequest request;
  request.physical_dag.selected_plan_uuid = std::string(kPlanUuid);
  request.physical_dag.root_physical_node_id = root_node_id;
  request.physical_dag.local_transaction_id = 4507;
  request.physical_dag.statement_snapshot_id = 4508;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000004511"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000004512"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000004513"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000004514"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000004515"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000004516"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000004517"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000004518"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = root_node_id - 2,
       .relational_node_id = static_cast<std::uint32_t>(root_node_id - 2),
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.set-nested-left.typed.v1",
       .output_descriptor_ids = {left.columns[0].descriptor_id},
       .causal_counter_id = causal_counter_id - 2},
      {.physical_node_id = root_node_id - 1,
       .relational_node_id = static_cast<std::uint32_t>(root_node_id - 1),
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.set-nested-right.typed.v1",
       .output_descriptor_ids = {right.columns[0].descriptor_id},
       .causal_counter_id = causal_counter_id - 1},
      {.physical_node_id = root_node_id,
       .relational_node_id = static_cast<std::uint32_t>(root_node_id),
       .node_kind = exec::PhysicalNodeKind::kSetOperation,
       .implementation_id = "setop." + OperationName(operation) +
                            "-distinct.ordinal.typed.v1",
       .input_physical_node_ids = {root_node_id - 2, root_node_id - 1},
       .output_descriptor_ids = {result_column.descriptor_id},
       .causal_counter_id = causal_counter_id},
  };
  request.selected_physical_node_id = root_node_id;
  request.left_batch = left;
  request.right_batch = right;
  request.result_columns = {std::move(result_column)};
  request.operation = operation;
  request.quantifier = exec::CanonicalSetOperationQuantifier::kDistinct;
  request.maximum_output_row_count = 16;
  return request;
}

exec::CanonicalSetOperationNestingRequest Request(
    const exec::CanonicalSetOperationNestingRule rule,
    const exec::CanonicalSetOperationKind first_operation =
        exec::CanonicalSetOperationKind::kUnion,
    const exec::CanonicalSetOperationKind second_operation =
        exec::CanonicalSetOperationKind::kIntersect) {
  const auto first_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004501");
  const auto second_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004502");
  const auto third_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004503");
  const auto intermediate_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004504");
  const auto result_descriptor =
      Descriptor("019f0000-0000-7200-8000-000000004505");
  const auto first = Batch("n", first_descriptor, 4501, {"1", "2"});
  const auto second = Batch("n", second_descriptor, 4502, {"2", "3"});
  const auto third = Batch("n", third_descriptor, 4503, {"2"});
  exec::DescriptorBatch intermediate_schema;
  intermediate_schema.columns = {
      {"n", intermediate_descriptor, true, 4504}};

  bool right_grouped = rule ==
                       exec::CanonicalSetOperationNestingRule::kExplicitRight;
  if (rule == exec::CanonicalSetOperationNestingRule::kSqlPrecedence) {
    right_grouped =
        second_operation == exec::CanonicalSetOperationKind::kIntersect &&
        first_operation != exec::CanonicalSetOperationKind::kIntersect;
  }

  exec::CanonicalSetOperationNestingRequest request;
  request.first_operand = first;
  request.second_operand = second;
  request.third_operand = third;
  request.first_operation = first_operation;
  request.second_operation = second_operation;
  request.first_quantifier =
      exec::CanonicalSetOperationQuantifier::kDistinct;
  request.second_quantifier =
      exec::CanonicalSetOperationQuantifier::kDistinct;
  request.nesting_rule = rule;
  request.maximum_intermediate_row_count = 8;
  if (right_grouped) {
    request.inner_request_template = OperationTemplate(
        second, third, intermediate_schema.columns[0], second_operation,
        4510, 45100);
    request.outer_request_template = OperationTemplate(
        first, intermediate_schema,
        {"n", result_descriptor, true, 4505}, first_operation, 4520, 45200);
  } else {
    request.inner_request_template = OperationTemplate(
        first, second, intermediate_schema.columns[0], first_operation,
        4510, 45100);
    request.outer_request_template = OperationTemplate(
        intermediate_schema, third,
        {"n", result_descriptor, true, 4505}, second_operation, 4520, 45200);
  }
  return request;
}

// QOW-TEST-QRY-016-NESTING-V1
bool ValidateSetOperationNesting() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalSetOperationNesting(
      Request(exec::CanonicalSetOperationNestingRule::kSqlPrecedence));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 2 &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[1].values[0].encoded_value == "2" &&
          result.resolved_nesting_rule ==
              exec::CanonicalSetOperationNestingRule::kExplicitRight &&
          result.intermediate_row_count == 1 &&
          result.inner_physical_node_id == 4510 &&
          result.outer_physical_node_id == 4520 &&
          result.inner_causal_counter_id < result.outer_causal_counter_id,
      "SQL precedence did not evaluate INTERSECT before UNION");

  result = exec::ExecuteCanonicalSetOperationNesting(
      Request(exec::CanonicalSetOperationNestingRule::kExplicitLeft));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[0].encoded_value == "2" &&
          result.resolved_nesting_rule ==
              exec::CanonicalSetOperationNestingRule::kExplicitLeft &&
          result.intermediate_row_count == 3,
      "explicit left grouping did not override SQL precedence");

  result = exec::ExecuteCanonicalSetOperationNesting(
      Request(exec::CanonicalSetOperationNestingRule::kExplicitRight));
  passed &= Require(result.diagnostic.ok &&
                        result.output_batch.rows.size() == 2,
                    "explicit right grouping was not retained");

  result = exec::ExecuteCanonicalSetOperationNesting(Request(
      exec::CanonicalSetOperationNestingRule::kSqlPrecedence,
      exec::CanonicalSetOperationKind::kExcept,
      exec::CanonicalSetOperationKind::kUnion));
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 2 &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[1].values[0].encoded_value == "2" &&
          result.resolved_nesting_rule ==
              exec::CanonicalSetOperationNestingRule::kExplicitLeft,
      "equal-precedence EXCEPT/UNION expression was not left associative");

  auto request = Request(
      exec::CanonicalSetOperationNestingRule::kExplicitLeft);
  request.maximum_intermediate_row_count = 2;
  result = exec::ExecuteCanonicalSetOperationNesting(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "oversized nested intermediate result was published");

  request = Request(exec::CanonicalSetOperationNestingRule::kExplicitRight);
  request.outer_request_template.physical_dag.statement_snapshot_id = 9999;
  result = exec::ExecuteCanonicalSetOperationNesting(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "nested nodes crossed engine MGA snapshot boundaries");

  request = Request(exec::CanonicalSetOperationNestingRule::kExplicitRight);
  request.outer_request_template.physical_dag.nodes[2].causal_counter_id =
      45000;
  result = exec::ExecuteCanonicalSetOperationNesting(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.inner_physical_node_id == 0 &&
                        result.outer_physical_node_id == 0,
                    "invalid nested causal order retained execution evidence");
  return passed;
}

}  // namespace

int main() { return ValidateSetOperationNesting() ? EXIT_SUCCESS
                                                   : EXIT_FAILURE; }
