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
    std::cerr << "QOW-TEST-QRY-016-NESTING-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {"019f0000-0000-7200-8000-00000000f841",
          "019f0000-0000-7200-8000-00000000f842",
          statement_snapshot_uuid,
          "019f0000-0000-7200-8000-00000000f843",
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
          true};
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
        "019f0000-0000-7200-8000-00000000f844";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000f845";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000f846";
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
  request.mga_authority = BindPhysicalAbiV2(&request.physical_dag);
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
          result.inner_causal_counter_id < result.outer_causal_counter_id &&
          result.mga_statement_context.visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request(exec::CanonicalSetOperationNestingRule::kSqlPrecedence)
                  .inner_request_template.mga_authority.statement_context),
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

  request = Request(exec::CanonicalSetOperationNestingRule::kExplicitRight);
  request.outer_request_template.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalSetOperationNesting(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.inner_physical_node_id == 0 &&
                        result.outer_physical_node_id == 0 &&
                        !exec::PhysicalMgaStatementContextValid(
                            result.mga_statement_context),
                    "missing MGA transaction reached nested access");

  request = Request(exec::CanonicalSetOperationNestingRule::kExplicitRight);
  request.outer_request_template.mga_authority.resolve_current = {};
  result = exec::ExecuteCanonicalSetOperationNesting(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "missing current MGA resolver reached nested access");

  request = Request(exec::CanonicalSetOperationNestingRule::kExplicitRight);
  request.outer_request_template.physical_dag.catalog_epoch_uuid =
      request.outer_request_template.physical_dag.mga_statement_context
          .statement_metadata_snapshot_uuid;
  result = exec::ExecuteCanonicalSetOperationNesting(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "catalog metadata substitution reached nested access");

  request = Request(exec::CanonicalSetOperationNestingRule::kExplicitRight);
  request.outer_request_template.physical_dag.local_transaction_id =
      static_cast<std::uint32_t>(kOwnerLocalTransactionId);
  result = exec::ExecuteCanonicalSetOperationNesting(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "narrowed MGA identity reached nested access");
  return passed;
}

}  // namespace

int main() { return ValidateSetOperationNesting() ? EXIT_SUCCESS
                                                   : EXIT_FAILURE; }
