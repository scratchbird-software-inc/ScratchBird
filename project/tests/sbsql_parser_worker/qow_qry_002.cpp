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

api::TypedRelationalDag WindowDag() {
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid = "019f0000-0000-7000-8000-000000000291";
  dag.bound_catalog_epoch_uuid = "019f0000-0000-7000-8000-000000000292";
  dag.bound_security_context_uuid = "019f0000-0000-7000-8000-000000000293";
  dag.statement_uuid = "019f0000-0000-7000-8000-000000000294";
  dag.owning_transaction_uuid = "019f0000-0000-7000-8000-000000000295";
  dag.statement_snapshot_uuid = "019f0000-0000-7000-8000-000000000296";
  dag.statement_metadata_snapshot_uuid =
      "019f0000-0000-7000-8000-000000000297";
  dag.local_transaction_id = 29;
  dag.root_node_id = 2;
  dag.descriptors = {
      {1, "019f0000-0000-7200-8000-000000000291",
       "019f0000-0000-7300-8000-000000000291",
       api::RelationalNullability::kNonNull},
      {2, "019f0000-0000-7200-8000-000000000292",
       "019f0000-0000-7300-8000-000000000292",
       api::RelationalNullability::kNonNull},
  };
  api::RelationalExpressionRecord identifier;
  identifier.expression_id = 1;
  identifier.expression_kind = api::RelationalExpressionKind::kIdentifier;
  identifier.result_descriptor_id = 1;
  identifier.bound_name_uuid =
      "019f0000-0000-7600-8000-000000000291";
  api::RelationalExpressionRecord function;
  function.expression_id = 2;
  function.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  function.result_descriptor_id = 2;
  function.function_uuid = "019de5fc-2400-7539-bcce-00eef3ae7220";
  dag.expressions = {std::move(identifier), std::move(function)};
  dag.outputs = {
      {1, 1, 1, "account_id", 1, false, 0},
      {2, 2, 2, "sequence_no", 2, true, 0},
  };
  api::RelationalDagNode scan;
  scan.node_id = 1;
  scan.node_kind = api::RelationalDagNodeKind::kScan;
  scan.output_descriptor_ids = {1};
  scan.bound_expression_ids = {1};
  scan.semantic_variant_id = "catalog.relation-source.v1";
  api::RelationalDagNode window;
  window.node_id = 2;
  window.node_kind = api::RelationalDagNodeKind::kWindow;
  window.input_node_ids = {1};
  window.output_descriptor_ids = {2};
  window.bound_expression_ids = {1, 2};
  window.semantic_variant_id = "window.row-number.v1";
  window.required_property_uuids = {
      "019f0000-0000-7200-8000-000000000298",
      "019f0000-0000-7200-8000-000000000299",
  };
  window.delivered_property_uuids = {
      "019f0000-0000-7200-8000-000000000298",
      "019f0000-0000-7200-8000-000000000299",
      "019f0000-0000-7200-8000-00000000029a",
  };
  dag.nodes = {std::move(scan), std::move(window)};
  api::RelationalPropertyRecord partitioning;
  partitioning.property_uuid =
      "019f0000-0000-7200-8000-000000000298";
  partitioning.property_kind = api::RelationalPropertyKind::kPartitioning;
  partitioning.origin_node_id = 2;
  partitioning.expression_ids = {1};
  api::RelationalPropertyRecord ordering;
  ordering.property_uuid = "019f0000-0000-7200-8000-000000000299";
  ordering.property_kind = api::RelationalPropertyKind::kOrdering;
  ordering.origin_node_id = 2;
  ordering.ordering_terms = {
      {1, api::RelationalPropertySortDirection::kAscending,
       api::RelationalPropertyNullPlacement::kNullsLast, ""},
  };
  api::RelationalPropertyRecord window_property;
  window_property.property_uuid =
      "019f0000-0000-7200-8000-00000000029a";
  window_property.property_kind = api::RelationalPropertyKind::kWindow;
  window_property.origin_node_id = 2;
  window_property.dependency_property_uuids = {
      "019f0000-0000-7200-8000-000000000298",
      "019f0000-0000-7200-8000-000000000299",
  };
  window_property.window_frame_descriptor_uuid =
      "019f0000-0000-7200-8000-00000000029b";
  dag.properties = {std::move(partitioning), std::move(ordering),
                    std::move(window_property)};
  api::RelationalWindowDefinitionRecord definition;
  definition.window_id = 1;
  definition.relation_node_id = 2;
  definition.partition_expression_ids = {1};
  definition.ordering_terms = {
      {1, api::RelationalPropertySortDirection::kAscending,
       api::RelationalPropertyNullPlacement::kNullsLast, ""},
  };
  dag.window_definitions.push_back(std::move(definition));
  api::RelationalWindowInvocationRecord invocation;
  invocation.invocation_id = 1;
  invocation.relation_node_id = 2;
  invocation.function_expression_id = 2;
  invocation.window_definition_id = 1;
  invocation.function_abi_version = 1;
  invocation.builtin_id = "sb.window.row_number";
  invocation.function_uuid = "019de5fc-2400-7539-bcce-00eef3ae7220";
  invocation.result_descriptor_id = 2;
  invocation.output_name_utf8 = "sequence_no";
  dag.window_invocations.push_back(std::move(invocation));
  return dag;
}

api::TypedRelationalDag NamedWindowDag() {
  auto dag = WindowDag();
  dag.window_definitions.clear();
  api::RelationalWindowDefinitionRecord partitioned;
  partitioned.window_id = 1;
  partitioned.relation_node_id = 2;
  partitioned.canonical_name_key = "partitioned";
  partitioned.partition_expression_ids = {1};
  api::RelationalWindowDefinitionRecord ordered;
  ordered.window_id = 2;
  ordered.relation_node_id = 2;
  ordered.canonical_name_key = "ordered";
  ordered.inherited_window_id = 1;
  ordered.ordering_terms = {
      {1, api::RelationalPropertySortDirection::kAscending,
       api::RelationalPropertyNullPlacement::kNullsLast, ""},
  };
  api::RelationalWindowDefinitionRecord framed;
  framed.window_id = 3;
  framed.relation_node_id = 2;
  framed.canonical_name_key = "framed";
  framed.inherited_window_id = 2;
  dag.window_definitions = {std::move(partitioned), std::move(ordered),
                            std::move(framed)};
  dag.window_invocations.front().window_definition_id = 3;
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

bool ValidateDescriptorUuidOccurrenceAuthority() {
  auto repeated_v1 = SharedDag();
  repeated_v1.descriptors[1].descriptor_uuid =
      repeated_v1.descriptors[0].descriptor_uuid;
  repeated_v1.descriptors[1].type_uuid = repeated_v1.descriptors[0].type_uuid;
  repeated_v1.descriptors[1].nullability =
      repeated_v1.descriptors[0].nullability;
  repeated_v1.descriptors[1].collation_uuid =
      repeated_v1.descriptors[0].collation_uuid;
  repeated_v1.descriptors[1].timezone_profile_id =
      repeated_v1.descriptors[0].timezone_profile_id;
  repeated_v1.descriptors[1].width = repeated_v1.descriptors[0].width;
  repeated_v1.descriptors[1].precision = repeated_v1.descriptors[0].precision;
  repeated_v1.descriptors[1].scale = repeated_v1.descriptors[0].scale;
  const auto repeated_v1_result =
      api::ValidateTypedRelationalDag(repeated_v1);

  auto conflicting_v1 = repeated_v1;
  conflicting_v1.descriptors[1].nullability =
      api::RelationalNullability::kNullable;
  const auto conflicting_v1_result =
      api::ValidateTypedRelationalDag(conflicting_v1);

  const auto make_authoritative = [] {
    auto dag = SharedDag();
    auto& first = dag.descriptors[0];
    auto& second = dag.descriptors[1];
    first.descriptor_uuid = "019d0000-0000-7000-8000-00000000d718";
    first.type_uuid = "019d0000-0000-7000-8000-00000000d719";
    first.nullability = api::RelationalNullability::kNullable;
    first.width = 256;
    first.datatype_identity_authoritative = true;
    first.descriptor_generation = 1;
    first.type_generation = 1;
    first.codec_id = "datatype.text.utf8.v1";
    first.codec_version = 1;
    first.codec_generation = 1;
    first.statement_receipt_uuid =
        "019f0000-0000-7000-8000-0000000002a1";
    first.datatype_catalog_snapshot_uuid =
        "019d0000-0000-7000-8000-00000000d701";
    first.datatype_catalog_generation = 1;
    first.datatype_registry_generation = 1;

    second.descriptor_uuid = first.descriptor_uuid;
    second.type_uuid = first.type_uuid;
    second.nullability = api::RelationalNullability::kNonNull;
    second.width = 128;
    second.datatype_identity_authoritative = true;
    second.descriptor_generation = first.descriptor_generation;
    second.type_generation = first.type_generation;
    second.codec_id = first.codec_id;
    second.codec_version = first.codec_version;
    second.codec_generation = first.codec_generation;
    second.statement_receipt_uuid = first.statement_receipt_uuid;
    second.datatype_catalog_snapshot_uuid =
        first.datatype_catalog_snapshot_uuid;
    second.datatype_catalog_generation = first.datatype_catalog_generation;
    second.datatype_registry_generation = first.datatype_registry_generation;
    return dag;
  };

  const auto repeated_v2 = make_authoritative();
  const auto repeated_v2_result =
      api::ValidateTypedRelationalDag(repeated_v2);

  auto mixed_distinct_uuid = make_authoritative();
  auto& mixed_distinct = mixed_distinct_uuid.descriptors[1];
  mixed_distinct.descriptor_uuid =
      "019f0000-0000-7200-8000-0000000002a3";
  mixed_distinct.type_uuid =
      "019f0000-0000-7300-8000-0000000002a4";
  mixed_distinct.datatype_identity_authoritative = false;
  mixed_distinct.descriptor_generation = 0;
  mixed_distinct.type_generation = 0;
  mixed_distinct.codec_id.clear();
  mixed_distinct.codec_version = 0;
  mixed_distinct.codec_generation = 0;
  mixed_distinct.statement_receipt_uuid.clear();
  mixed_distinct.datatype_catalog_snapshot_uuid.clear();
  mixed_distinct.datatype_catalog_generation = 0;
  mixed_distinct.datatype_registry_generation = 0;
  const auto mixed_distinct_result =
      api::ValidateTypedRelationalDag(mixed_distinct_uuid);

  bool passed = true;
  passed &= Require(
      repeated_v1_result.accepted && repeated_v1_result.issues.empty(),
      "alias-distinct v1 descriptor occurrence with identical UUID authority was refused");
  passed &= Require(
      !conflicting_v1_result.accepted &&
          HasIssue(conflicting_v1_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "descriptor_record"),
      "repeated v1 descriptor UUID with conflicting slot authority was accepted");
  passed &= Require(
      repeated_v2_result.accepted && repeated_v2_result.issues.empty(),
      "complete v2 descriptor occurrences with distinct slot facets were refused");
  passed &= Require(
      mixed_distinct_result.accepted && mixed_distinct_result.issues.empty(),
      "mixed v2/v1 descriptors with distinct UUID handles were refused");

  constexpr std::size_t kImmutableFieldCount = 10;
  for (std::size_t field = 0; field < kImmutableFieldCount; ++field) {
    auto conflicting_v2 = make_authoritative();
    auto& second = conflicting_v2.descriptors[1];
    switch (field) {
      case 0:
        second.type_uuid =
            "019f0000-0000-7300-8000-0000000002b0";
        break;
      case 1: second.descriptor_generation = 2; break;
      case 2: second.type_generation = 2; break;
      case 3: second.codec_id = "datatype.text.other.v1"; break;
      case 4: second.codec_version = 2; break;
      case 5: second.codec_generation = 2; break;
      case 6:
        second.statement_receipt_uuid =
            "019f0000-0000-7000-8000-0000000002b6";
        break;
      case 7:
        second.datatype_catalog_snapshot_uuid =
            "019f0000-0000-7000-8000-0000000002b7";
        break;
      case 8: second.datatype_catalog_generation = 2; break;
      case 9: second.datatype_registry_generation = 2; break;
      default: break;
    }
    const auto result = api::ValidateTypedRelationalDag(conflicting_v2);
    passed &= Require(
        !result.accepted &&
            HasIssue(result, "DATATYPE.DESCRIPTOR.INVALID",
                     "descriptor_record"),
        "complete v2 repeated UUID admitted an immutable authority mutation");
  }

  auto mixed_same_uuid = make_authoritative();
  auto& mixed_same = mixed_same_uuid.descriptors[1];
  mixed_same.datatype_identity_authoritative = false;
  mixed_same.descriptor_generation = 0;
  mixed_same.type_generation = 0;
  mixed_same.codec_id.clear();
  mixed_same.codec_version = 0;
  mixed_same.codec_generation = 0;
  mixed_same.statement_receipt_uuid.clear();
  mixed_same.datatype_catalog_snapshot_uuid.clear();
  mixed_same.datatype_catalog_generation = 0;
  mixed_same.datatype_registry_generation = 0;
  const auto mixed_same_result =
      api::ValidateTypedRelationalDag(mixed_same_uuid);
  passed &= Require(
      !mixed_same_result.accepted &&
          HasIssue(mixed_same_result, "DATATYPE.DESCRIPTOR.INVALID",
                   "descriptor_record"),
      "same descriptor UUID mixed complete v2 and v1 authority");

  auto partial = make_authoritative();
  partial.descriptors[1].codec_generation = 0;
  const auto partial_result = api::ValidateTypedRelationalDag(partial);
  passed &= Require(
      !partial_result.accepted &&
          HasIssue(partial_result, "DATATYPE.DESCRIPTOR.INVALID",
                   "descriptor_record"),
      "partial v2 datatype authority was accepted");

  auto hidden_partial = SharedDag();
  hidden_partial.descriptors[0].descriptor_generation = 1;
  const auto hidden_partial_result =
      api::ValidateTypedRelationalDag(hidden_partial);
  passed &= Require(
      !hidden_partial_result.accepted &&
          HasIssue(hidden_partial_result, "DATATYPE.DESCRIPTOR.INVALID",
                   "descriptor_record"),
      "non-authoritative descriptor carried a hidden partial v2 tuple");
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

bool ValidateTypedWindowContract() {
  const auto accepted = api::ValidateTypedRelationalDag(WindowDag());
  const auto named_accepted =
      api::ValidateTypedRelationalDag(NamedWindowDag());

  auto missing_definition = WindowDag();
  missing_definition.window_definitions.clear();
  const auto missing_definition_result =
      api::ValidateTypedRelationalDag(missing_definition);

  auto unframed_exclusion = WindowDag();
  unframed_exclusion.window_definitions.front().exclusion =
      api::RelationalWindowFrameExclusion::kTies;
  const auto unframed_exclusion_result =
      api::ValidateTypedRelationalDag(unframed_exclusion);

  auto wrong_abi = WindowDag();
  wrong_abi.window_invocations.front().function_abi_version = 2;
  const auto wrong_abi_result = api::ValidateTypedRelationalDag(wrong_abi);

  auto forward_inheritance = NamedWindowDag();
  forward_inheritance.window_definitions[0].inherited_window_id = 3;
  const auto forward_inheritance_result =
      api::ValidateTypedRelationalDag(forward_inheritance);

  auto inherited_override = NamedWindowDag();
  inherited_override.window_definitions[1].partition_expression_ids = {1};
  const auto inherited_override_result =
      api::ValidateTypedRelationalDag(inherited_override);

  auto duplicate_name = NamedWindowDag();
  duplicate_name.window_definitions[1].canonical_name_key = "partitioned";
  const auto duplicate_name_result =
      api::ValidateTypedRelationalDag(duplicate_name);

  bool passed = true;
  passed &= Require(accepted.accepted && accepted.issues.empty(),
                    "canonical typed window records were refused");
  passed &= Require(named_accepted.accepted && named_accepted.issues.empty(),
                    "canonical named-window inheritance was refused");
  passed &= Require(
      !missing_definition_result.accepted &&
          HasIssue(missing_definition_result,
                   "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "window_invocation_record"),
      "window invocation without a definition was accepted");
  passed &= Require(
      !unframed_exclusion_result.accepted &&
          HasIssue(unframed_exclusion_result,
                   "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "window_definition_record"),
      "window exclusion without a frame was accepted");
  passed &= Require(
      !wrong_abi_result.accepted &&
          HasIssue(wrong_abi_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "window_invocation_record"),
      "unknown window function ABI was accepted");
  passed &= Require(
      !forward_inheritance_result.accepted &&
          HasIssue(forward_inheritance_result,
                   "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "inherited_window_id"),
      "forward named-window inheritance was accepted");
  passed &= Require(
      !inherited_override_result.accepted &&
          HasIssue(inherited_override_result,
                   "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "inherited_window_id"),
      "named-window inherited PARTITION override was accepted");
  passed &= Require(
      !duplicate_name_result.accepted &&
          HasIssue(duplicate_name_result,
                   "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "window_definition_name_scope"),
      "duplicate canonical named-window key was accepted");
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
  passed &= ValidateDescriptorUuidOccurrenceAuthority();
  passed &= ValidateDuplicateAndDanglingRefusal();
  passed &= ValidateCycleRefusal();
  passed &= ValidateSharingRefusal();
  passed &= ValidateOrphanRefusal();
  passed &= ValidateDepthLimitRefusal();
  passed &= ValidateFanoutLimitRefusal();
  passed &= ValidateTypedValuesRefusal();
  passed &= ValidateTypedGroupingSetContract();
  passed &= ValidateTypedWindowContract();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
