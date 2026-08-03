// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/plan_api.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

bool HasIssue(const api::RelationalDagValidationResult& result,
              const std::string_view diagnostic_id,
              const std::string_view field_id) {
  for (const auto& issue : result.issues) {
    if (issue.diagnostic_id == diagnostic_id && issue.field_id == field_id) {
      return true;
    }
  }
  return false;
}

api::TypedRelationalDag SharedDag() {
  api::TypedRelationalDag dag;
  dag.root_node_id = 4;
  dag.descriptors = {
      {1, "019f0000-0000-7200-8000-000000000201",
       "019f0000-0000-7300-8000-000000000211",
       api::RelationalNullability::kNonNull},
      {2, "019f0000-0000-7200-8000-000000000202",
       "019f0000-0000-7300-8000-000000000212",
       api::RelationalNullability::kNonNull},
      {3, "019f0000-0000-7200-8000-000000000203",
       "019f0000-0000-7300-8000-000000000213",
       api::RelationalNullability::kNonNull},
  };
  api::RelationalExpressionRecord literal;
  literal.expression_id = 1;
  literal.expression_kind = api::RelationalExpressionKind::kLiteral;
  literal.result_descriptor_id = 1;
  literal.literal_kind = api::RelationalLiteralKind::kNumeric;
  literal.literal_or_parameter_ref = "1";
  dag.expressions.push_back(std::move(literal));
  dag.outputs = {{1, 1, 1, "column_1", 1, true, 0}};
  dag.values_rows = {{1, {1}}};
  dag.nodes = {
      {1, api::RelationalDagNodeKind::kValues, {}, {1}, true, {1}},
      {2, api::RelationalDagNodeKind::kFilter, {1}, {1}, false},
      {3, api::RelationalDagNodeKind::kProject, {1}, {2}, false},
      {4, api::RelationalDagNodeKind::kSetOperation, {2, 3}, {3}, false},
  };
  return dag;
}

api::TypedRelationalDag GroupingSetDag() {
  auto dag = SharedDag();
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid = "019f0000-0000-7000-8000-000000000281";
  dag.bound_catalog_epoch_uuid = "019f0000-0000-7000-8000-000000000282";
  dag.bound_security_context_uuid = "019f0000-0000-7000-8000-000000000283";
  dag.statement_uuid = "019f0000-0000-7000-8000-000000000284";
  dag.owning_transaction_uuid = "019f0000-0000-7000-8000-000000000285";
  dag.statement_snapshot_uuid = "019f0000-0000-7000-8000-000000000286";
  dag.statement_metadata_snapshot_uuid =
      "019f0000-0000-7000-8000-000000000287";
  dag.local_transaction_id = 0xffff'ffff'ffff'ff00ULL;
  dag.snapshot_visible_through_local_transaction_id = 0;
  for (auto& node : dag.nodes) {
    node.semantic_variant_id = "relational.contract-node.v1";
  }
  dag.nodes.back().node_kind = api::RelationalDagNodeKind::kAggregate;
  dag.nodes.back().bound_expression_ids = {1};
  dag.grouping_sets = {
      {4, 0, {1}},
      {4, 1, {}},
      {4, 2, {1}},
  };
  return dag;
}

bool ValidateAcceptedSharedDag() {
  const auto result = api::ValidateTypedRelationalDag(SharedDag());
  bool passed = true;
  passed &= Require(result.accepted, "valid shared relational DAG was refused");
  passed &= Require(result.issues.empty(), "valid relational DAG emitted issues");
  passed &= Require(result.validated_node_count == 4,
                    "validated relational node count differs");
  passed &= Require(result.maximum_observed_depth == 3,
                    "observed relational DAG depth differs");
  return passed;
}

bool ValidateRootRefusal() {
  auto dag = SharedDag();
  dag.package_root = static_cast<api::RelationalPackageRoot>(99);
  const auto result = api::ValidateTypedRelationalDag(dag);
  bool passed = true;
  passed &= Require(!result.accepted, "noncanonical relational root was accepted");
  passed &= Require(HasIssue(result,
                             "QOW-DIAG-RELATIONAL-ROOT-NONCANONICAL",
                             "package_root"),
                    "noncanonical root diagnostic differs");
  return passed;
}

bool ValidateVersionAndKindRefusal() {
  auto version = SharedDag();
  version.wire_version = 3;
  const auto version_result = api::ValidateTypedRelationalDag(version);

  auto unknown_kind = SharedDag();
  unknown_kind.nodes[0].node_kind =
      static_cast<api::RelationalDagNodeKind>(255);
  const auto kind_result = api::ValidateTypedRelationalDag(unknown_kind);

  bool passed = true;
  passed &= Require(!version_result.accepted,
                    "unknown relational wire version was accepted");
  passed &= Require(HasIssue(version_result, "SBLR.PLAN_TREE.INVALID_VERSION",
                             "wire_version"),
                    "wire-version diagnostic differs");
  passed &= Require(!kind_result.accepted,
                    "unknown relational node kind was accepted");
  passed &= Require(HasIssue(kind_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                             "node_id_or_kind"),
                    "node-kind diagnostic differs");
  return passed;
}

bool ValidateDescriptorHandleRefusal() {
  auto zero = SharedDag();
  zero.nodes[0].output_descriptor_ids = {0};
  const auto zero_result = api::ValidateTypedRelationalDag(zero);

  auto duplicate = SharedDag();
  duplicate.nodes[0].output_descriptor_ids = {1, 1};
  const auto duplicate_result = api::ValidateTypedRelationalDag(duplicate);

  bool passed = true;
  passed &= Require(!zero_result.accepted,
                    "zero output descriptor handle was accepted");
  passed &= Require(HasIssue(zero_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                             "output_descriptor_ids"),
                    "zero descriptor-handle diagnostic differs");
  passed &= Require(!duplicate_result.accepted,
                    "duplicate output descriptor handle was accepted");
  passed &= Require(HasIssue(duplicate_result,
                             "SBLR.PLAN_TREE.INVALID_HANDLE",
                             "output_descriptor_ids"),
                    "duplicate descriptor-handle diagnostic differs");
  return passed;
}

bool ValidateDuplicateAndDanglingRefusal() {
  auto duplicate = SharedDag();
  duplicate.nodes[1].node_id = 1;
  const auto duplicate_result = api::ValidateTypedRelationalDag(duplicate);

  auto dangling = SharedDag();
  dangling.nodes[3].input_node_ids[1] = 99;
  const auto dangling_result = api::ValidateTypedRelationalDag(dangling);

  bool passed = true;
  passed &= Require(!duplicate_result.accepted,
                    "duplicate relational node handle was accepted");
  passed &= Require(HasIssue(duplicate_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                             "node_id_or_kind"),
                    "duplicate handle diagnostic differs");
  passed &= Require(!dangling_result.accepted,
                    "dangling relational input handle was accepted");
  passed &= Require(HasIssue(dangling_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                             "input_node_ids"),
                    "dangling handle diagnostic differs");
  return passed;
}

bool ValidateCycleRefusal() {
  api::TypedRelationalDag dag;
  dag.descriptors = {
      {1, "019f0000-0000-7200-8000-000000000221",
       "019f0000-0000-7300-8000-000000000231",
       api::RelationalNullability::kNonNull}};
  dag.root_node_id = 1;
  dag.nodes = {
      {1, api::RelationalDagNodeKind::kProject, {2}, {1}, false},
      {2, api::RelationalDagNodeKind::kFilter, {1}, {1}, false},
  };
  const auto result = api::ValidateTypedRelationalDag(dag);
  bool passed = true;
  passed &= Require(!result.accepted, "cyclic relational DAG was accepted");
  passed &= Require(HasIssue(result, "SBLR.PLAN_TREE.INVALID_HANDLE", "cycle"),
                    "cycle diagnostic differs");
  return passed;
}

bool ValidateSharingRefusal() {
  auto dag = SharedDag();
  dag.nodes[0].shareable = false;
  const auto result = api::ValidateTypedRelationalDag(dag);
  bool passed = true;
  passed &= Require(!result.accepted,
                    "undeclared shared relational node was accepted");
  passed &= Require(HasIssue(result, "SBLR.PLAN_TREE.INVALID_HANDLE", "shareable"),
                    "sharing diagnostic differs");
  return passed;
}

bool ValidateOrphanRefusal() {
  auto dag = SharedDag();
  dag.descriptors.push_back(
      {4, "019f0000-0000-7200-8000-000000000204",
       "019f0000-0000-7300-8000-000000000214",
       api::RelationalNullability::kNonNull});
  dag.nodes.push_back(
      {5, api::RelationalDagNodeKind::kScan, {}, {4}, false});
  const auto result = api::ValidateTypedRelationalDag(dag);
  bool passed = true;
  passed &= Require(!result.accepted, "orphan relational node was accepted");
  passed &= Require(HasIssue(result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                             "orphan_node"),
                    "orphan diagnostic differs");
  return passed;
}

bool ValidateDepthLimitRefusal() {
  api::TypedRelationalDag dag;
  dag.descriptors = {
      {1, "019f0000-0000-7200-8000-000000000241",
       "019f0000-0000-7300-8000-000000000251",
       api::RelationalNullability::kNonNull}};
  api::RelationalExpressionRecord literal;
  literal.expression_id = 1;
  literal.expression_kind = api::RelationalExpressionKind::kLiteral;
  literal.result_descriptor_id = 1;
  literal.literal_kind = api::RelationalLiteralKind::kNumeric;
  literal.literal_or_parameter_ref = "1";
  dag.expressions.push_back(std::move(literal));
  dag.outputs = {{1, 1, 1, "column_1", 1, true, 0}};
  dag.values_rows = {{1, {1}}};
  for (std::uint32_t node_id = 1; node_id <= 257; ++node_id) {
    api::RelationalDagNode node;
    node.node_id = node_id;
    node.node_kind = node_id == 1 ? api::RelationalDagNodeKind::kValues
                                  : api::RelationalDagNodeKind::kProject;
    if (node_id != 1) node.input_node_ids.push_back(node_id - 1);
    node.output_descriptor_ids = {1};
    if (node_id == 1) node.values_row_ids = {1};
    dag.nodes.push_back(std::move(node));
  }
  dag.root_node_id = 257;
  const auto result = api::ValidateTypedRelationalDag(dag);
  bool passed = true;
  passed &= Require(!result.accepted, "depth-257 relational DAG was accepted");
  passed &= Require(HasIssue(result, "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                             "maximum_depth"),
                    "depth-limit diagnostic differs");
  return passed;
}

bool ValidateFanoutLimitRefusal() {
  api::TypedRelationalDag dag;
  dag.descriptors = {
      {1, "019f0000-0000-7200-8000-000000000261",
       "019f0000-0000-7300-8000-000000000271",
       api::RelationalNullability::kNonNull}};
  api::RelationalDagNode root;
  root.node_id = 1026;
  root.node_kind = api::RelationalDagNodeKind::kSetOperation;
  root.output_descriptor_ids = {1};
  for (std::uint32_t node_id = 1; node_id <= 1025; ++node_id) {
    dag.nodes.push_back(
        {node_id, api::RelationalDagNodeKind::kScan, {}, {1}, false});
    root.input_node_ids.push_back(node_id);
  }
  dag.nodes.push_back(std::move(root));
  dag.root_node_id = 1026;
  const auto result = api::ValidateTypedRelationalDag(dag);
  bool passed = true;
  passed &= Require(!result.accepted, "fanout-1025 relational node was accepted");
  passed &= Require(HasIssue(result, "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                             "input_node_ids"),
                    "fanout-limit diagnostic differs");
  return passed;
}

bool ValidateTypedValuesRefusal() {
  auto missing_rows = SharedDag();
  missing_rows.nodes[0].values_row_ids.clear();
  const auto missing_rows_result =
      api::ValidateTypedRelationalDag(missing_rows);

  auto wrong_descriptor = SharedDag();
  wrong_descriptor.expressions[0].result_descriptor_id = 2;
  const auto wrong_descriptor_result =
      api::ValidateTypedRelationalDag(wrong_descriptor);

  auto missing_name_handle = SharedDag();
  missing_name_handle.expressions[0].expression_kind =
      api::RelationalExpressionKind::kIdentifier;
  missing_name_handle.expressions[0].literal_kind.reset();
  missing_name_handle.expressions[0].literal_or_parameter_ref.reset();
  const auto missing_name_result =
      api::ValidateTypedRelationalDag(missing_name_handle);

  bool passed = true;
  passed &= Require(
      !missing_rows_result.accepted &&
          HasIssue(missing_rows_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "literal_table_descriptor"),
      "VALUES without row records was accepted");
  passed &= Require(
      !wrong_descriptor_result.accepted &&
          HasIssue(wrong_descriptor_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "output_record"),
      "VALUES expression descriptor mismatch was accepted");
  passed &= Require(
      !missing_name_result.accepted &&
          HasIssue(missing_name_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "expression_typed_fields"),
      "identifier without immutable name handle was accepted");
  return passed;
}

bool ValidateTypedGroupingSetContract() {
  const auto accepted = api::ValidateTypedRelationalDag(GroupingSetDag());

  auto nonaggregate = GroupingSetDag();
  nonaggregate.grouping_sets[0].relation_node_id = 3;
  const auto nonaggregate_result =
      api::ValidateTypedRelationalDag(nonaggregate);

  auto duplicate_ordinal = GroupingSetDag();
  duplicate_ordinal.grouping_sets[1].ordinal = 0;
  const auto duplicate_ordinal_result =
      api::ValidateTypedRelationalDag(duplicate_ordinal);

  auto sparse_ordinal = GroupingSetDag();
  sparse_ordinal.grouping_sets[1].ordinal = 3;
  const auto sparse_ordinal_result =
      api::ValidateTypedRelationalDag(sparse_ordinal);

  auto unbound_expression = GroupingSetDag();
  unbound_expression.grouping_sets[0].expression_ids = {99};
  const auto unbound_expression_result =
      api::ValidateTypedRelationalDag(unbound_expression);

  auto duplicate_expression = GroupingSetDag();
  duplicate_expression.grouping_sets[0].expression_ids = {1, 1};
  const auto duplicate_expression_result =
      api::ValidateTypedRelationalDag(duplicate_expression);

  bool passed = true;
  passed &= Require(
      accepted.accepted && accepted.issues.empty(),
      "ordered grouping-set records, including a repeated set, were refused");
  passed &= Require(
      !nonaggregate_result.accepted &&
          HasIssue(nonaggregate_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "grouping_set_record"),
      "grouping-set record bound to a non-aggregate node was accepted");
  passed &= Require(
      !duplicate_ordinal_result.accepted &&
          HasIssue(duplicate_ordinal_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "grouping_set_record"),
      "duplicate grouping-set ordinal was accepted");
  passed &= Require(
      !sparse_ordinal_result.accepted &&
          HasIssue(sparse_ordinal_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "grouping_set_ordinals"),
      "sparse grouping-set ordinals were accepted");
  passed &= Require(
      !unbound_expression_result.accepted &&
          HasIssue(unbound_expression_result,
                   "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "grouping_set_expression_ids"),
      "unbound grouping-set expression was accepted");
  passed &= Require(
      !duplicate_expression_result.accepted &&
          HasIssue(duplicate_expression_result,
                   "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "grouping_set_expression_ids"),
      "duplicate expression inside a grouping set was accepted");
  return passed;
}

} // namespace

// QOW-TEST-QRY-002-V1
int main() {
  bool passed = true;
  passed &= ValidateAcceptedSharedDag();
  passed &= ValidateRootRefusal();
  passed &= ValidateVersionAndKindRefusal();
  passed &= ValidateDescriptorHandleRefusal();
  passed &= ValidateDuplicateAndDanglingRefusal();
  passed &= ValidateCycleRefusal();
  passed &= ValidateSharingRefusal();
  passed &= ValidateOrphanRefusal();
  passed &= ValidateDepthLimitRefusal();
  passed &= ValidateFanoutLimitRefusal();
  passed &= ValidateTypedValuesRefusal();
  passed &= ValidateTypedGroupingSetContract();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
