// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_node_composition.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_query_sort_registration.hpp"
#include "canonical_query_window_registration.hpp"
#include "canonical_relational_expression.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_WINDOW_PREPARATION_AUTHORITY
// Validates typed window descriptors, operand identities, ordering, and result
// bindings, including bounded literal preparation and existing descriptor helpers.
// Owns no plan selection, physical executor dispatch, storage access, snapshot
// construction, transaction finality, or public query route selection.

namespace {

bool DirectValueWindowUsesExactTypeV1(
    const api::TypedRelationalDag& dag,
    const std::uint32_t relation_node_id,
    const std::string_view expected_builtin_id,
    const std::string_view type_uuid) {
  if (type_uuid.empty() ||
      (expected_builtin_id != "sb.window.lag" &&
       expected_builtin_id != "sb.window.lead" &&
       expected_builtin_id != "sb.window.first_value" &&
       expected_builtin_id != "sb.window.last_value" &&
       expected_builtin_id != "sb.window.nth_value")) {
    return false;
  }
  const api::RelationalWindowInvocationRecord* exact_invocation = nullptr;
  for (const auto& invocation : dag.window_invocations) {
    if (invocation.relation_node_id != relation_node_id) continue;
    if (exact_invocation != nullptr) return false;
    exact_invocation = &invocation;
  }
  const std::size_t expected_argument_count =
      expected_builtin_id == "sb.window.nth_value" ? 2U : 1U;
  if (exact_invocation == nullptr ||
      exact_invocation->builtin_id != expected_builtin_id ||
      exact_invocation->argument_expression_ids.size() !=
          expected_argument_count) {
    return false;
  }
  const auto argument = std::ranges::find_if(
      dag.expressions, [&](const auto& expression) {
        return expression.expression_id ==
               exact_invocation->argument_expression_ids.front();
      });
  const auto descriptor = std::ranges::find_if(
      dag.descriptors, [&](const auto& candidate) {
        return argument != dag.expressions.end() &&
               candidate.descriptor_id == argument->result_descriptor_id;
      });
  return argument != dag.expressions.end() &&
         argument->expression_kind ==
             api::RelationalExpressionKind::kIdentifier &&
         descriptor != dag.descriptors.end() &&
         descriptor->type_uuid == type_uuid;
}

bool ExactCanonicalBooleanWindowSourceV1(
    const api::RelationalTypeDescriptor& relational_descriptor,
    const api::EngineDescriptor& runtime_descriptor,
    const bool runtime_nullable,
    const std::string_view boolean_type_uuid,
    const std::string_view function_uuid,
    const std::string_view result_descriptor_uuid,
    const std::string_view ordering_property_uuid,
    const std::string_view window_property_uuid,
    const std::string_view window_frame_descriptor_uuid) {
  return !boolean_type_uuid.empty() &&
         relational_descriptor.descriptor_uuid ==
             runtime_descriptor.descriptor_uuid.canonical &&
         relational_descriptor.descriptor_uuid !=
             relational_descriptor.type_uuid &&
         relational_descriptor.descriptor_uuid != function_uuid &&
         relational_descriptor.descriptor_uuid != result_descriptor_uuid &&
         relational_descriptor.descriptor_uuid != ordering_property_uuid &&
         relational_descriptor.descriptor_uuid != window_property_uuid &&
         relational_descriptor.descriptor_uuid !=
             window_frame_descriptor_uuid &&
         relational_descriptor.type_uuid == boolean_type_uuid &&
         relational_descriptor.nullability ==
             (runtime_nullable ? api::RelationalNullability::kNullable
                               : api::RelationalNullability::kNonNull) &&
         !relational_descriptor.collation_uuid.has_value() &&
         !relational_descriptor.timezone_profile_id.has_value() &&
         !relational_descriptor.width.has_value() &&
         !relational_descriptor.precision.has_value() &&
         !relational_descriptor.scale.has_value() &&
         runtime_descriptor.descriptor_kind == "scalar" &&
         runtime_descriptor.canonical_type_name == "boolean" &&
         api::QowCanonicalDescriptorIdentityV1(runtime_descriptor) &&
         runtime_descriptor.encoded_descriptor ==
             "type_uuid=" + std::string(boolean_type_uuid) +
                 ";nullability=" +
                 (runtime_nullable ? "nullable" : "non_null");
}

bool CanonicalDescriptorFieldEqualsV1(
    const api::EngineDescriptor& descriptor,
    const std::string_view key,
    const std::optional<std::string_view> expected) {
  const auto prefix = std::string(key) + "=";
  std::optional<std::string_view> value;
  std::size_t begin = 0;
  while (begin <= descriptor.encoded_descriptor.size()) {
    const auto end = descriptor.encoded_descriptor.find(';', begin);
    const auto field = std::string_view(descriptor.encoded_descriptor).substr(
        begin, end == std::string::npos ? std::string::npos : end - begin);
    if (field.starts_with(prefix)) {
      if (value.has_value()) return false;
      value = field.substr(prefix.size());
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return expected.has_value() ? value == expected : !value.has_value();
}


unsigned ExactBoundedSignedIntegerTypeRankV1(
    const std::string_view type_uuid) {
  constexpr std::array<std::string_view, 4> kTypes{
      "int8", "int16", "int32", "int64"};
  for (std::size_t ordinal = 0; ordinal < kTypes.size(); ++ordinal) {
    if (type_uuid == ExactCanonicalCoreDatatypeTypeUuidV1(kTypes[ordinal])) {
      return static_cast<unsigned>(ordinal + 1);
    }
  }
  return 0;
}


bool ExactCanonicalScalarWindowOperandV1(
    const api::RelationalTypeDescriptor& relational_descriptor,
    const api::EngineDescriptor& runtime_descriptor,
    const bool runtime_nullable,
    const std::string_view function_uuid,
    const std::string_view result_descriptor_uuid,
    const std::string_view result_type_uuid,
    const std::string_view counterpart_descriptor_uuid,
    const std::string_view counterpart_type_uuid,
    const bool same_operand_ordinal,
    const std::string_view ordering_property_uuid,
    const std::string_view window_property_uuid,
    const std::string_view window_frame_descriptor_uuid) {
  const auto expected_nullability =
      runtime_nullable ? std::string_view("nullable")
                       : std::string_view("non_null");
  const auto expected_legacy_nullability =
      runtime_nullable ? std::string_view("true") : std::string_view("false");
  const bool exact_nullability =
      (CanonicalDescriptorFieldEqualsV1(
           runtime_descriptor, "nullability", expected_nullability) &&
       CanonicalDescriptorFieldEqualsV1(runtime_descriptor, "nullable",
                                        std::nullopt)) ||
      (CanonicalDescriptorFieldEqualsV1(runtime_descriptor, "nullability",
                                        std::nullopt) &&
       CanonicalDescriptorFieldEqualsV1(
           runtime_descriptor, "nullable", expected_legacy_nullability));
  const auto width = relational_descriptor.width.has_value()
                         ? std::optional<std::string>(
                               std::to_string(*relational_descriptor.width))
                         : std::nullopt;
  const auto precision =
      relational_descriptor.precision.has_value()
          ? std::optional<std::string>(
                std::to_string(*relational_descriptor.precision))
          : std::nullopt;
  const auto scale = relational_descriptor.scale.has_value()
                         ? std::optional<std::string>(
                               std::to_string(*relational_descriptor.scale))
                         : std::nullopt;
  const auto field_matches = [&](const std::string_view key,
                                 const std::optional<std::string>& value) {
    return CanonicalDescriptorFieldEqualsV1(
        runtime_descriptor, key,
        value.has_value()
            ? std::optional<std::string_view>(*value)
            : std::optional<std::string_view>{});
  };
  return CanonicalUuidText(relational_descriptor.type_uuid) &&
         !result_type_uuid.empty() &&
         relational_descriptor.descriptor_uuid ==
             runtime_descriptor.descriptor_uuid.canonical &&
         relational_descriptor.descriptor_uuid !=
             relational_descriptor.type_uuid &&
         relational_descriptor.descriptor_uuid != result_type_uuid &&
         (same_operand_ordinal || relational_descriptor.descriptor_uuid !=
                                      counterpart_descriptor_uuid) &&
         relational_descriptor.descriptor_uuid != counterpart_type_uuid &&
         relational_descriptor.descriptor_uuid != function_uuid &&
         relational_descriptor.descriptor_uuid != result_descriptor_uuid &&
         relational_descriptor.descriptor_uuid != ordering_property_uuid &&
         relational_descriptor.descriptor_uuid != window_property_uuid &&
         relational_descriptor.descriptor_uuid !=
             window_frame_descriptor_uuid &&
         relational_descriptor.type_uuid != function_uuid &&
         relational_descriptor.type_uuid != result_descriptor_uuid &&
         relational_descriptor.type_uuid != counterpart_descriptor_uuid &&
         relational_descriptor.type_uuid != ordering_property_uuid &&
         relational_descriptor.type_uuid != window_property_uuid &&
         relational_descriptor.type_uuid != window_frame_descriptor_uuid &&
         relational_descriptor.nullability ==
             (runtime_nullable ? api::RelationalNullability::kNullable
                               : api::RelationalNullability::kNonNull) &&
         runtime_descriptor.descriptor_kind == "scalar" &&
         !runtime_descriptor.canonical_type_name.empty() &&
         api::QowCanonicalDescriptorIdentityV1(runtime_descriptor) &&
         exec::CanonicalDerivedDescriptorTypeMatches(
             runtime_descriptor, runtime_nullable, runtime_descriptor,
             runtime_nullable) &&
         CanonicalDescriptorFieldEqualsV1(runtime_descriptor, "type_uuid",
                                          relational_descriptor.type_uuid) &&
         exact_nullability &&
         CanonicalDescriptorFieldEqualsV1(
             runtime_descriptor, "collation_uuid",
             relational_descriptor.collation_uuid.has_value()
                 ? std::optional<std::string_view>(
                       *relational_descriptor.collation_uuid)
                 : std::optional<std::string_view>{}) &&
         (!relational_descriptor.collation_uuid.has_value() ||
          CanonicalUuidText(*relational_descriptor.collation_uuid)) &&
         CanonicalDescriptorFieldEqualsV1(
             runtime_descriptor, "timezone_profile_id",
             relational_descriptor.timezone_profile_id.has_value()
                 ? std::optional<std::string_view>(
                       *relational_descriptor.timezone_profile_id)
                 : std::optional<std::string_view>{}) &&
         field_matches("width", width) &&
         field_matches("precision", precision) &&
         field_matches("scale", scale);
}

bool ExactCanonicalBoundedSignedWindowSourceV1(
    const api::RelationalTypeDescriptor& relational_descriptor,
    const api::EngineDescriptor& runtime_descriptor,
    const bool runtime_nullable,
    const std::array<std::string, 4>& bounded_signed_type_uuids,
    const std::string_view function_uuid,
    const std::string_view result_descriptor_uuid,
    const std::string_view ordering_property_uuid,
    const std::string_view window_property_uuid,
    const std::string_view window_frame_descriptor_uuid) {
  constexpr std::array<std::string_view, 4> kBoundedSignedTypeNames = {
      "int8", "int16", "int32", "int64"};
  const auto type_uuid = std::ranges::find(
      bounded_signed_type_uuids, relational_descriptor.type_uuid);
  const auto type_index = static_cast<std::size_t>(
      std::distance(bounded_signed_type_uuids.begin(), type_uuid));
  const auto& result_type_uuid = bounded_signed_type_uuids.back();
  return type_uuid != bounded_signed_type_uuids.end() &&
         !type_uuid->empty() && type_index < kBoundedSignedTypeNames.size() &&
         !result_type_uuid.empty() &&
         relational_descriptor.descriptor_uuid ==
             runtime_descriptor.descriptor_uuid.canonical &&
         relational_descriptor.descriptor_uuid !=
             relational_descriptor.type_uuid &&
         relational_descriptor.descriptor_uuid != result_type_uuid &&
         relational_descriptor.descriptor_uuid != function_uuid &&
         relational_descriptor.descriptor_uuid != result_descriptor_uuid &&
         relational_descriptor.descriptor_uuid != ordering_property_uuid &&
         relational_descriptor.descriptor_uuid != window_property_uuid &&
         relational_descriptor.descriptor_uuid !=
             window_frame_descriptor_uuid &&
         *type_uuid != function_uuid &&
         *type_uuid != result_descriptor_uuid &&
         *type_uuid != ordering_property_uuid &&
         *type_uuid != window_property_uuid &&
         *type_uuid != window_frame_descriptor_uuid &&
         (type_index == kBoundedSignedTypeNames.size() - 1 ||
          *type_uuid != result_type_uuid) &&
         relational_descriptor.nullability ==
             (runtime_nullable ? api::RelationalNullability::kNullable
                               : api::RelationalNullability::kNonNull) &&
         !relational_descriptor.collation_uuid.has_value() &&
         !relational_descriptor.timezone_profile_id.has_value() &&
         !relational_descriptor.width.has_value() &&
         !relational_descriptor.precision.has_value() &&
         !relational_descriptor.scale.has_value() &&
         runtime_descriptor.descriptor_kind == "scalar" &&
         runtime_descriptor.canonical_type_name ==
             kBoundedSignedTypeNames[type_index] &&
         exec::IsCanonicalBoundedSignedIntegerDescriptor(
             runtime_descriptor) &&
         api::QowCanonicalDescriptorIdentityV1(runtime_descriptor) &&
         runtime_descriptor.encoded_descriptor ==
             "type_uuid=" + *type_uuid + ";nullability=" +
                 (runtime_nullable ? "nullable" : "non_null");
}

bool ExactCanonicalBoundedSignedWindowOrderV1(
    const api::RelationalTypeDescriptor& relational_descriptor,
    const api::EngineDescriptor& runtime_descriptor,
    const bool runtime_nullable,
    const std::array<std::string, 4>& bounded_signed_type_uuids,
    const std::string_view function_uuid,
    const std::string_view result_descriptor_uuid,
    const std::string_view result_type_uuid,
    const std::string_view ordering_property_uuid,
    const std::string_view window_property_uuid,
    const std::string_view window_frame_descriptor_uuid) {
  constexpr std::array<std::string_view, 4> kBoundedSignedTypeNames = {
      "int8", "int16", "int32", "int64"};
  const auto type_uuid = std::ranges::find(
      bounded_signed_type_uuids, relational_descriptor.type_uuid);
  const auto type_index = static_cast<std::size_t>(
      std::distance(bounded_signed_type_uuids.begin(), type_uuid));
  return type_uuid != bounded_signed_type_uuids.end() &&
         !type_uuid->empty() && type_index < kBoundedSignedTypeNames.size() &&
         !result_type_uuid.empty() &&
         relational_descriptor.descriptor_uuid ==
             runtime_descriptor.descriptor_uuid.canonical &&
         relational_descriptor.descriptor_uuid !=
             relational_descriptor.type_uuid &&
         relational_descriptor.descriptor_uuid != result_type_uuid &&
         relational_descriptor.descriptor_uuid != function_uuid &&
         relational_descriptor.descriptor_uuid != result_descriptor_uuid &&
         relational_descriptor.descriptor_uuid != ordering_property_uuid &&
         relational_descriptor.descriptor_uuid != window_property_uuid &&
         relational_descriptor.descriptor_uuid !=
             window_frame_descriptor_uuid &&
         *type_uuid != function_uuid &&
         *type_uuid != result_descriptor_uuid &&
         *type_uuid != ordering_property_uuid &&
         *type_uuid != window_property_uuid &&
         *type_uuid != window_frame_descriptor_uuid &&
         relational_descriptor.nullability ==
             (runtime_nullable ? api::RelationalNullability::kNullable
                               : api::RelationalNullability::kNonNull) &&
         !relational_descriptor.collation_uuid.has_value() &&
         !relational_descriptor.timezone_profile_id.has_value() &&
         !relational_descriptor.width.has_value() &&
         !relational_descriptor.precision.has_value() &&
         !relational_descriptor.scale.has_value() &&
         runtime_descriptor.descriptor_kind == "scalar" &&
         runtime_descriptor.canonical_type_name ==
             kBoundedSignedTypeNames[type_index] &&
         exec::IsCanonicalBoundedSignedIntegerDescriptor(
             runtime_descriptor) &&
         api::QowCanonicalDescriptorIdentityV1(runtime_descriptor) &&
         runtime_descriptor.encoded_descriptor ==
             "type_uuid=" + *type_uuid + ";nullability=" +
                 (runtime_nullable ? "nullable" : "non_null");
}

GlobalRankingWindowProfile GlobalAggregateWindowProfileV1(
    const api::TypedRelationalDag& dag,
    const std::uint32_t relation_node_id) {
  const api::RelationalWindowInvocationRecord* exact_invocation = nullptr;
  for (const auto& invocation : dag.window_invocations) {
    if (invocation.relation_node_id != relation_node_id) continue;
    if (exact_invocation != nullptr) return {};
    exact_invocation = &invocation;
  }
  if (exact_invocation == nullptr) return {};
  const auto* row = exec::LookupCanonicalAggregateByUuidV1(
      exact_invocation->function_uuid);
  if (row == nullptr || !row->executable || !row->aggregate_as_window ||
      row->abi_version != exact_invocation->function_abi_version ||
      row->builtin_id != exact_invocation->builtin_id) {
    return {};
  }
  std::string_view display_name;
  std::string_view result_type_name = "int64";
  switch (row->function) {
    case exec::CanonicalAggregateFunction::sum:
      display_name = "SUM";
      break;
    case exec::CanonicalAggregateFunction::min:
      display_name = "MIN";
      break;
    case exec::CanonicalAggregateFunction::max:
      display_name = "MAX";
      break;
    case exec::CanonicalAggregateFunction::count:
      display_name = "COUNT";
      break;
    case exec::CanonicalAggregateFunction::bool_and:
      display_name = "BOOL_AND";
      result_type_name = "boolean";
      break;
    case exec::CanonicalAggregateFunction::bool_or:
      display_name = "BOOL_OR";
      result_type_name = "boolean";
      break;
    case exec::CanonicalAggregateFunction::every:
      display_name = "EVERY";
      result_type_name = "boolean";
      break;
    default:
      return {};
  }
  return {"window.aggregate-bridge.v1", row->builtin_id,
          row->function_uuid, display_name, result_type_name};
}
// The optional Project-root form preserves a narrower public SELECT list and
// additionally binds every Window passthrough to its ordered input expression.
PreparedGlobalRowNumberWindowBinding PrepareGlobalRankingWindowBinding(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalPropertyCatalog& logical_properties,
    const api::RelationalDagNode& consumer,
    const plan::CanonicalLogicalRelationalNode& logical_consumer,
    const plan::CanonicalLogicalRelationalNode& previous_logical,
    const PreparedSortRoot& prepared_sort,
    const std::size_t materialized_column_count,
    const std::size_t result_binding_count,
    const std::string& result_type_uuid,
    const std::string& order_type_uuid,
    const std::string& boolean_type_uuid,
    const std::array<std::string, 4>& bounded_signed_type_uuids,
    const std::string_view family_label,
    const GlobalRankingWindowProfile& profile,
    const bool allow_project_root = false) {
  PreparedGlobalRowNumberWindowBinding result;
  result.detail = std::string(family_label) + " " +
                  std::string(profile.display_name) +
                  " binding is not exact";

  std::vector<const api::RelationalWindowDefinitionRecord*> definitions;
  std::vector<const api::RelationalWindowInvocationRecord*> invocations;
  for (const auto& definition : dag.window_definitions) {
    if (definition.relation_node_id == consumer.node_id) {
      definitions.push_back(&definition);
    }
  }
  for (const auto& invocation : dag.window_invocations) {
    if (invocation.relation_node_id == consumer.node_id) {
      invocations.push_back(&invocation);
    }
  }
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == consumer.node_id) {
      result.outputs.push_back(&output);
    }
  }
  std::ranges::sort(result.outputs, {},
                    &api::RelationalOutputRecord::ordinal);
  const auto typed_root = std::ranges::find_if(
      dag.nodes, [&](const auto& node) {
        return node.node_id == dag.root_node_id;
      });
  const bool exact_window_position =
      consumer.node_id == dag.root_node_id ||
      (allow_project_root && typed_root != dag.nodes.end() &&
       typed_root->node_kind == api::RelationalDagNodeKind::kProject &&
       typed_root->input_node_ids ==
           std::vector<std::uint32_t>{consumer.node_id});

  const auto typed_sort = std::ranges::find_if(
      dag.nodes, [&](const auto& node) {
        return node.node_id == previous_logical.logical_node_id;
      });
  const auto typed_order_expression =
      typed_sort != dag.nodes.end() &&
              typed_sort->bound_expression_ids.size() == 1
          ? std::ranges::find_if(dag.expressions, [&](const auto& expression) {
              return expression.expression_id ==
                     typed_sort->bound_expression_ids.front();
            })
          : dag.expressions.end();
  const auto typed_order_descriptor =
      typed_order_expression == dag.expressions.end()
          ? dag.descriptors.end()
          : std::ranges::find_if(dag.descriptors, [&](const auto& descriptor) {
              return descriptor.descriptor_id ==
                     typed_order_expression->result_descriptor_id;
            });
  const auto function =
      invocations.size() == 1
          ? std::ranges::find_if(dag.expressions, [&](const auto& expression) {
              return expression.expression_id ==
                     invocations.front()->function_expression_id;
            })
          : dag.expressions.end();
  const auto result_descriptor =
      invocations.size() == 1
          ? std::ranges::find_if(dag.descriptors, [&](const auto& descriptor) {
              return descriptor.descriptor_id ==
                     invocations.front()->result_descriptor_id;
            })
          : dag.descriptors.end();
  const bool ntile_window = profile.builtin_id == "sb.window.ntile";
  const bool lag_window = profile.builtin_id == "sb.window.lag";
  const bool lead_window = profile.builtin_id == "sb.window.lead";
  const bool navigation_window = lag_window || lead_window;
  const bool first_value_window =
      profile.builtin_id == "sb.window.first_value";
  const bool last_value_window =
      profile.builtin_id == "sb.window.last_value";
  const bool nth_value_window =
      profile.builtin_id == "sb.window.nth_value";
  const bool navigation_value_window =
      navigation_window || first_value_window || last_value_window ||
      nth_value_window;
  const bool aggregate_window =
      profile.semantic_variant_id == "window.aggregate-bridge.v1";
  const bool aggregate_count_window =
      aggregate_window && profile.builtin_id == "sb.aggregate.count";
  const bool aggregate_boolean_window =
      aggregate_window &&
      (profile.builtin_id == "sb.aggregate.bool_and" ||
       profile.builtin_id == "sb.aggregate.bool_or" ||
       profile.builtin_id == "sb.aggregate.every");
  const bool aggregate_bounded_signed_window =
      aggregate_window && !aggregate_count_window &&
      !aggregate_boolean_window;
  const bool fixed_unqualified_ranking_result_window =
      profile.builtin_id == "sb.window.row_number" ||
      profile.builtin_id == "sb.window.rank" ||
      profile.builtin_id == "sb.window.dense_rank" ||
      profile.builtin_id == "sb.window.percent_rank" ||
      profile.builtin_id == "sb.window.cume_dist" ||
      profile.builtin_id == "sb.window.ntile";
  const bool aggregate_count_star_window =
      aggregate_count_window && invocations.size() == 1 &&
      invocations.front()->argument_expression_ids.empty();
  const bool value_window =
      navigation_window || first_value_window || last_value_window ||
      nth_value_window || aggregate_window;
  const bool value_operand_window =
      value_window && !aggregate_count_star_window;
  const bool exact_argument_arity =
      invocations.size() == 1 &&
      (nth_value_window
           ? invocations.front()->argument_expression_ids.size() == 2
           : aggregate_count_star_window
           ? invocations.front()->argument_expression_ids.empty()
           : ((ntile_window || value_operand_window)
                  ? invocations.front()->argument_expression_ids.size() == 1
                  : invocations.front()->argument_expression_ids.empty()));
  std::vector<std::uint32_t> expected_bound_expression_ids;
  if (typed_sort != dag.nodes.end() &&
      typed_sort->bound_expression_ids.size() == 1 &&
      invocations.size() == 1 && exact_argument_arity) {
    expected_bound_expression_ids.push_back(
        typed_sort->bound_expression_ids.front());
    expected_bound_expression_ids.insert(
        expected_bound_expression_ids.end(),
        invocations.front()->argument_expression_ids.begin(),
        invocations.front()->argument_expression_ids.end());
    expected_bound_expression_ids.push_back(
        invocations.front()->function_expression_id);
  }
  std::uint32_t input_lineage_node_id = 0;
  bool input_lineage_exact = !allow_project_root;
  if (allow_project_root) {
    input_lineage_exact =
        typed_sort != dag.nodes.end() &&
        typed_sort->input_node_ids.size() == 1;
  }
  if (allow_project_root && input_lineage_exact) {
    input_lineage_node_id = typed_sort->input_node_ids.front();
    std::unordered_set<std::uint32_t> lineage_visited;
    while (input_lineage_exact &&
           std::ranges::none_of(dag.outputs, [&](const auto& output) {
             return output.relation_node_id == input_lineage_node_id;
           })) {
      if (!lineage_visited.insert(input_lineage_node_id).second) {
        input_lineage_exact = false;
        break;
      }
      const auto lineage_node = std::ranges::find_if(
          dag.nodes, [&](const auto& node) {
            return node.node_id == input_lineage_node_id;
          });
      if (lineage_node == dag.nodes.end() ||
          (lineage_node->node_kind != api::RelationalDagNodeKind::kFilter &&
           lineage_node->node_kind != api::RelationalDagNodeKind::kSort) ||
          lineage_node->input_node_ids.size() != 1) {
        input_lineage_exact = false;
        break;
      }
      const auto lineage_input = std::ranges::find_if(
          dag.nodes, [&](const auto& node) {
            return node.node_id == lineage_node->input_node_ids.front();
          });
      if (lineage_input == dag.nodes.end() ||
          lineage_node->output_descriptor_ids !=
              lineage_input->output_descriptor_ids) {
        input_lineage_exact = false;
        break;
      }
      input_lineage_node_id = lineage_input->node_id;
    }
  }
  std::vector<const api::RelationalOutputRecord*> input_outputs;
  for (const auto& output : dag.outputs) {
    if (input_lineage_exact &&
        output.relation_node_id == input_lineage_node_id) {
      input_outputs.push_back(&output);
    }
  }
  std::ranges::sort(input_outputs, {},
                    &api::RelationalOutputRecord::ordinal);
  const bool ordered_input =
      previous_logical.node_kind ==
          plan::CanonicalLogicalRelationalNodeKind::kSort &&
      typed_sort != dag.nodes.end() &&
      typed_sort->bound_expression_ids.size() == 1 &&
      consumer.required_property_uuids ==
          std::vector<std::string>{prepared_sort.ordering_property_uuid} &&
      consumer.delivered_property_uuids.size() == 2 &&
      std::ranges::find(consumer.delivered_property_uuids,
                        prepared_sort.ordering_property_uuid) !=
          consumer.delivered_property_uuids.end();
  bool passthrough_outputs =
      result.outputs.size() ==
          previous_logical.output_descriptor_ids.size() + 1 &&
      (!allow_project_root ||
       (input_lineage_exact &&
        input_outputs.size() ==
            previous_logical.output_descriptor_ids.size())) &&
      materialized_column_count ==
          previous_logical.output_descriptor_ids.size() &&
      result_binding_count == previous_logical.output_descriptor_ids.size();
  for (std::size_t ordinal = 0;
       passthrough_outputs &&
       ordinal < previous_logical.output_descriptor_ids.size();
       ++ordinal) {
    passthrough_outputs =
        (!allow_project_root ||
         (input_outputs[ordinal]->ordinal == ordinal &&
          input_outputs[ordinal]->descriptor_id ==
              previous_logical.output_descriptor_ids[ordinal] &&
          result.outputs[ordinal]->expression_id ==
              input_outputs[ordinal]->expression_id)) &&
        result.outputs[ordinal]->ordinal == ordinal &&
        result.outputs[ordinal]->visible &&
        result.outputs[ordinal]->descriptor_id ==
            previous_logical.output_descriptor_ids[ordinal];
  }

  if (consumer.semantic_variant_id != profile.semantic_variant_id ||
      !exact_window_position ||
      consumer.input_node_ids !=
          std::vector<std::uint32_t>{previous_logical.logical_node_id} ||
      logical_consumer.input_logical_node_ids !=
          std::vector<std::uint32_t>{previous_logical.logical_node_id} ||
      !ordered_input ||
      definitions.size() != 1 || invocations.size() != 1 ||
      !exact_argument_arity || !passthrough_outputs ||
      !consumer.required_object_uuids.empty() ||
      consumer.bound_expression_ids != expected_bound_expression_ids ||
      definitions.front()->canonical_name_key.has_value() ||
      definitions.front()->inherited_window_id.has_value() ||
      !definitions.front()->partition_expression_ids.empty() ||
      definitions.front()->ordering_terms.size() != 1 ||
      definitions.front()->ordering_terms.front().expression_id !=
          typed_sort->bound_expression_ids.front() ||
      definitions.front()->frame_unit.has_value() ||
      definitions.front()->frame_start.has_value() ||
      definitions.front()->frame_end.has_value() ||
      definitions.front()->exclusion !=
          api::RelationalWindowFrameExclusion::kNoOthers ||
      invocations.front()->window_definition_id !=
          definitions.front()->window_id ||
      invocations.front()->function_abi_version != 1 ||
      invocations.front()->builtin_id != profile.builtin_id ||
      invocations.front()->function_uuid != profile.function_uuid ||
      function == dag.expressions.end() ||
      function->expression_kind !=
          api::RelationalExpressionKind::kFunctionCall ||
      function->function_uuid !=
          std::optional<std::string>(profile.function_uuid) ||
      function->bound_name_uuid.has_value() ||
      function->operator_name.has_value() ||
      function->literal_kind.has_value() ||
      function->literal_or_parameter_ref.has_value() ||
      function->child_expression_ids !=
          invocations.front()->argument_expression_ids ||
      function->result_descriptor_id !=
          invocations.front()->result_descriptor_id ||
      result_descriptor == dag.descriptors.end() || result_type_uuid.empty() ||
      result_descriptor->type_uuid != result_type_uuid ||
      result_descriptor->nullability !=
          (value_window && !aggregate_count_window
               ? api::RelationalNullability::kNullable
               : api::RelationalNullability::kNonNull) ||
      ((aggregate_window || fixed_unqualified_ranking_result_window) &&
       (result_descriptor->collation_uuid.has_value() ||
        result_descriptor->timezone_profile_id.has_value() ||
        result_descriptor->width.has_value() ||
        result_descriptor->precision.has_value() ||
        result_descriptor->scale.has_value())) ||
      consumer.output_descriptor_ids.size() !=
          previous_logical.output_descriptor_ids.size() + 1 ||
      !std::equal(previous_logical.output_descriptor_ids.begin(),
                  previous_logical.output_descriptor_ids.end(),
                  consumer.output_descriptor_ids.begin()) ||
      consumer.output_descriptor_ids.back() !=
          result_descriptor->descriptor_id ||
      logical_consumer.output_descriptor_ids !=
          consumer.output_descriptor_ids ||
      result.outputs.back()->ordinal !=
          previous_logical.output_descriptor_ids.size() ||
      !result.outputs.back()->visible ||
      result.outputs.back()->expression_id != function->expression_id ||
      result.outputs.back()->descriptor_id !=
          result_descriptor->descriptor_id ||
      result.outputs.back()->output_name_utf8 !=
          invocations.front()->output_name_utf8) {
    return result;
  }

  const auto window_property_uuid = std::ranges::find_if(
      consumer.delivered_property_uuids, [&](const auto& property_uuid) {
        return property_uuid != prepared_sort.ordering_property_uuid;
      });
  const auto window_property = std::ranges::find_if(
      logical_properties.properties, [&](const auto& property) {
        return window_property_uuid != consumer.delivered_property_uuids.end() &&
               property.property_uuid == *window_property_uuid;
      });
  if (window_property_uuid == consumer.delivered_property_uuids.end() ||
      window_property == logical_properties.properties.end() ||
      window_property->property_kind !=
          plan::CanonicalLogicalPropertyKind::kWindow ||
      window_property->origin_logical_node_id != consumer.node_id ||
      window_property->dependency_property_uuids !=
          std::vector<std::string>{prepared_sort.ordering_property_uuid} ||
      window_property->window_frame_descriptor_uuid.empty()) {
    result.diagnostic_id = "QOW-DIAG-WINDOW-PROPERTY-CARRIAGE-V1";
    result.detail = std::string(family_label) + " " +
                    std::string(profile.display_name) +
                    " property binding is not exact";
    return result;
  }
  if (fixed_unqualified_ranking_result_window) {
    const std::array<std::string_view, 6> ranking_identity_domain{
        result_descriptor->descriptor_uuid,
        result_type_uuid,
        profile.function_uuid,
        prepared_sort.ordering_property_uuid,
        *window_property_uuid,
        window_property->window_frame_descriptor_uuid};
    const std::unordered_set<std::string_view> distinct_ranking_identities(
        ranking_identity_domain.begin(), ranking_identity_domain.end());
    if (distinct_ranking_identities.size() != ranking_identity_domain.size() ||
        std::ranges::any_of(ranking_identity_domain, [](const auto identity) {
          return !CanonicalUuidText(identity);
        })) {
      result.detail = std::string(family_label) + " " +
                      std::string(profile.display_name) +
                      " result identity domain is not independent";
      return result;
    }
  }
  if (aggregate_window &&
      (result_descriptor->descriptor_uuid == result_type_uuid ||
       result_descriptor->descriptor_uuid == profile.function_uuid ||
       result_descriptor->descriptor_uuid ==
           prepared_sort.ordering_property_uuid ||
       result_descriptor->descriptor_uuid == *window_property_uuid ||
       result_descriptor->descriptor_uuid ==
           window_property->window_frame_descriptor_uuid)) {
    result.detail = std::string(family_label) + " " +
                    std::string(profile.display_name) +
                    " result descriptor identity is not independent";
    return result;
  }

  if (ntile_window) {
    const auto argument_expression_id =
        invocations.front()->argument_expression_ids.front();
    const auto argument = std::ranges::find_if(
        dag.expressions, [&](const auto& expression) {
          return expression.expression_id == argument_expression_id;
        });
    const auto argument_descriptor = std::ranges::find_if(
        dag.descriptors, [&](const auto& descriptor) {
          return argument != dag.expressions.end() &&
                 descriptor.descriptor_id == argument->result_descriptor_id;
        });
    if (argument == dag.expressions.end() ||
        argument->expression_kind != api::RelationalExpressionKind::kLiteral ||
        !argument->child_expression_ids.empty() ||
        argument->bound_name_uuid.has_value() ||
        argument->function_uuid.has_value() ||
        argument->literal_kind != api::RelationalLiteralKind::kNumeric ||
        argument->operator_name.has_value() ||
        !argument->literal_or_parameter_ref.has_value() ||
        argument_descriptor == dag.descriptors.end() ||
        argument_descriptor->descriptor_id ==
            result_descriptor->descriptor_id ||
        argument_descriptor->descriptor_uuid ==
            result_descriptor->descriptor_uuid ||
        argument_descriptor->descriptor_uuid == profile.function_uuid ||
        argument_descriptor->descriptor_uuid ==
            prepared_sort.ordering_property_uuid ||
        argument_descriptor->descriptor_uuid == *window_property_uuid ||
        argument_descriptor->type_uuid != result_type_uuid ||
        argument_descriptor->nullability !=
            api::RelationalNullability::kNonNull ||
        argument_descriptor->collation_uuid.has_value() ||
        argument_descriptor->timezone_profile_id.has_value() ||
        argument_descriptor->width.has_value() ||
        argument_descriptor->precision.has_value() ||
        argument_descriptor->scale.has_value()) {
      result.detail = std::string(family_label) +
                      " NTILE bucket count is not one independent non-NULL "
                      "canonical int64 literal";
      return result;
    }
    CanonicalRelationalExpressionRuntime runtime(dag);
    api::EngineTypedValue operand;
    std::string operand_detail;
    if (!runtime.EvaluateForConsumer(
            argument_expression_id, "int64",
            api::EngineCanonicalExpressionConsumer::window, &operand,
            &operand_detail) ||
        operand.state != api::EngineValueState::value || operand.is_null ||
        !operand.binary_value.empty() ||
        operand.descriptor.canonical_type_name != "int64" ||
        !api::QowCanonicalDescriptorIdentityV1(operand.descriptor) ||
        operand.descriptor.descriptor_uuid.canonical !=
            argument_descriptor->descriptor_uuid) {
      result.detail = std::string(family_label) +
                      " NTILE bucket count materialization failed";
      if (!operand_detail.empty()) result.detail += ": " + operand_detail;
      return result;
    }
    const auto decoded = exec::DecodeInt64Value(operand);
    if (!decoded.ok() || decoded.value <= 0) {
      result.detail = std::string(family_label) +
                      " NTILE bucket count must be positive int64";
      return result;
    }
    result.ntile_bucket_count_operand = std::move(operand);
  } else if (value_operand_window) {
    const auto argument_expression_id =
        invocations.front()->argument_expression_ids.front();
    const auto argument = std::ranges::find_if(
        dag.expressions, [&](const auto& expression) {
          return expression.expression_id == argument_expression_id;
        });
    const auto argument_descriptor = std::ranges::find_if(
        dag.descriptors, [&](const auto& descriptor) {
          return argument != dag.expressions.end() &&
                 descriptor.descriptor_id == argument->result_descriptor_id;
        });
    const auto value_column =
        argument_descriptor == dag.descriptors.end()
            ? previous_logical.output_descriptor_ids.end()
            : std::ranges::find(previous_logical.output_descriptor_ids,
                                argument_descriptor->descriptor_id);
    const bool exact_aggregate_argument_type =
        argument_descriptor != dag.descriptors.end() &&
        (!aggregate_window || aggregate_count_window ||
         (aggregate_bounded_signed_window &&
          std::ranges::find(bounded_signed_type_uuids,
                            argument_descriptor->type_uuid) !=
              bounded_signed_type_uuids.end()) ||
         (aggregate_boolean_window && !boolean_type_uuid.empty() &&
          argument_descriptor->type_uuid == boolean_type_uuid));
    if (argument == dag.expressions.end() ||
        argument->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        !argument->child_expression_ids.empty() ||
        !argument->bound_name_uuid.has_value() ||
        argument->function_uuid.has_value() ||
        argument->literal_kind.has_value() ||
        argument->literal_or_parameter_ref.has_value() ||
        argument->operator_name.has_value() ||
        argument_descriptor == dag.descriptors.end() ||
        value_column == previous_logical.output_descriptor_ids.end() ||
        argument_descriptor->descriptor_id ==
            result_descriptor->descriptor_id ||
        argument_descriptor->descriptor_uuid ==
            result_descriptor->descriptor_uuid ||
        argument_descriptor->descriptor_uuid == profile.function_uuid ||
        (aggregate_window &&
         (!exact_aggregate_argument_type ||
          (!aggregate_count_window &&
           (argument_descriptor->collation_uuid.has_value() ||
            argument_descriptor->timezone_profile_id.has_value() ||
            argument_descriptor->width.has_value() ||
            argument_descriptor->precision.has_value() ||
            argument_descriptor->scale.has_value())) ||
          result_descriptor->collation_uuid.has_value() ||
          result_descriptor->timezone_profile_id.has_value() ||
          result_descriptor->width.has_value() ||
          result_descriptor->precision.has_value() ||
          result_descriptor->scale.has_value())) ||
        (!aggregate_window &&
         (argument_descriptor->type_uuid != result_type_uuid ||
          (argument_descriptor->type_uuid != order_type_uuid &&
           (!navigation_value_window || boolean_type_uuid.empty() ||
            argument_descriptor->type_uuid != boolean_type_uuid)) ||
          result_descriptor->type_uuid != argument_descriptor->type_uuid ||
          argument_descriptor->collation_uuid.has_value() ||
          argument_descriptor->timezone_profile_id.has_value() ||
          argument_descriptor->width.has_value() ||
          argument_descriptor->precision.has_value() ||
          argument_descriptor->scale.has_value() ||
          result_descriptor->collation_uuid.has_value() ||
          result_descriptor->timezone_profile_id.has_value() ||
          result_descriptor->width.has_value() ||
          result_descriptor->precision.has_value() ||
          result_descriptor->scale.has_value() ||
          result_descriptor->collation_uuid !=
              argument_descriptor->collation_uuid ||
          result_descriptor->timezone_profile_id !=
              argument_descriptor->timezone_profile_id ||
          result_descriptor->width != argument_descriptor->width ||
          result_descriptor->precision != argument_descriptor->precision ||
          result_descriptor->scale != argument_descriptor->scale))) {
      result.detail = std::string(family_label) +
                      " " + std::string(profile.display_name) +
                      " value is not one direct canonical " +
                      std::string(aggregate_count_window
                                      ? "engine-bound"
                                      : profile.result_type_name) +
                      " column "
                      "with an exact distinct result descriptor";
      return result;
    }
    result.navigation_value_column = static_cast<std::size_t>(
        std::distance(previous_logical.output_descriptor_ids.begin(),
                      value_column));
    if (nth_value_window) {
      const auto position_expression_id =
          invocations.front()->argument_expression_ids[1];
      const auto position = std::ranges::find_if(
          dag.expressions, [&](const auto& expression) {
            return expression.expression_id == position_expression_id;
          });
      const auto position_descriptor = std::ranges::find_if(
          dag.descriptors, [&](const auto& descriptor) {
            return position != dag.expressions.end() &&
                   descriptor.descriptor_id == position->result_descriptor_id;
          });
      if (position == dag.expressions.end() ||
          position->expression_kind !=
              api::RelationalExpressionKind::kLiteral ||
          !position->child_expression_ids.empty() ||
          position->bound_name_uuid.has_value() ||
          position->function_uuid.has_value() ||
          position->literal_kind != api::RelationalLiteralKind::kNumeric ||
          position->operator_name.has_value() ||
          !position->literal_or_parameter_ref.has_value() ||
          position_descriptor == dag.descriptors.end() ||
          typed_order_expression == dag.expressions.end() ||
          typed_order_expression->expression_kind !=
              api::RelationalExpressionKind::kIdentifier ||
          typed_order_descriptor == dag.descriptors.end() ||
          typed_order_descriptor->type_uuid != order_type_uuid ||
          position_descriptor->descriptor_id ==
              argument_descriptor->descriptor_id ||
          position_descriptor->descriptor_id ==
              result_descriptor->descriptor_id ||
          position_descriptor->descriptor_id ==
              typed_order_descriptor->descriptor_id ||
          position_descriptor->descriptor_uuid ==
              argument_descriptor->descriptor_uuid ||
          position_descriptor->descriptor_uuid ==
              result_descriptor->descriptor_uuid ||
          position_descriptor->descriptor_uuid ==
              typed_order_descriptor->descriptor_uuid ||
          position_descriptor->descriptor_uuid == profile.function_uuid ||
          position_descriptor->descriptor_uuid ==
              prepared_sort.ordering_property_uuid ||
          position_descriptor->descriptor_uuid == *window_property_uuid ||
          position_descriptor->type_uuid != order_type_uuid ||
          position_descriptor->nullability !=
              api::RelationalNullability::kNonNull ||
          position_descriptor->collation_uuid.has_value() ||
          position_descriptor->timezone_profile_id.has_value() ||
          position_descriptor->width.has_value() ||
          position_descriptor->precision.has_value() ||
          position_descriptor->scale.has_value()) {
        result.detail = std::string(family_label) +
                        " NTH_VALUE position is not one independent "
                        "non-NULL canonical int64 literal";
        return result;
      }
      CanonicalRelationalExpressionRuntime runtime(dag);
      api::EngineTypedValue operand;
      std::string operand_detail;
      if (!runtime.EvaluateForConsumer(
              position_expression_id, "int64",
              api::EngineCanonicalExpressionConsumer::window, &operand,
              &operand_detail) ||
          operand.state != api::EngineValueState::value || operand.is_null ||
          !operand.binary_value.empty() ||
          operand.descriptor.canonical_type_name != "int64" ||
          !api::QowCanonicalDescriptorIdentityV1(operand.descriptor) ||
          operand.descriptor.descriptor_uuid.canonical !=
              position_descriptor->descriptor_uuid) {
        result.detail = std::string(family_label) +
                        " NTH_VALUE position materialization failed";
        if (!operand_detail.empty()) result.detail += ": " + operand_detail;
        return result;
      }
      const auto decoded = exec::DecodeInt64Value(operand);
      if (!decoded.ok() || decoded.value <= 0) {
        result.detail = std::string(family_label) +
                        " NTH_VALUE position must be positive int64";
        return result;
      }
      result.nth_value_position_operand = std::move(operand);
    }
  }

  if (aggregate_window) {
    const auto order_expression_id =
        definitions.front()->ordering_terms.front().expression_id;
    const auto order_expression = std::ranges::find_if(
        dag.expressions, [&](const auto& expression) {
          return expression.expression_id == order_expression_id;
        });
    const auto order_descriptor = std::ranges::find_if(
        dag.descriptors, [&](const auto& descriptor) {
          return order_expression != dag.expressions.end() &&
                 descriptor.descriptor_id ==
                     order_expression->result_descriptor_id;
        });
    const bool exact_bounded_signed_order =
        order_descriptor != dag.descriptors.end() &&
        std::ranges::find(bounded_signed_type_uuids,
                          order_descriptor->type_uuid) !=
            bounded_signed_type_uuids.end();
    if (prepared_sort.order_terms.size() != 1 ||
        order_expression == dag.expressions.end() ||
        order_expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        !order_expression->child_expression_ids.empty() ||
        !order_expression->bound_name_uuid.has_value() ||
        order_expression->function_uuid.has_value() ||
        order_expression->literal_kind.has_value() ||
        order_expression->literal_or_parameter_ref.has_value() ||
        order_expression->operator_name.has_value() ||
        order_descriptor == dag.descriptors.end() ||
        !exact_bounded_signed_order ||
        order_descriptor->collation_uuid.has_value() ||
        order_descriptor->timezone_profile_id.has_value() ||
        order_descriptor->width.has_value() ||
        order_descriptor->precision.has_value() ||
        order_descriptor->scale.has_value() ||
        std::ranges::find(previous_logical.output_descriptor_ids,
                          order_descriptor->descriptor_id) ==
            previous_logical.output_descriptor_ids.end()) {
      result.detail = std::string(family_label) +
                      " " + std::string(profile.display_name) +
                      " order key is not one direct canonical bounded-signed "
                      "column";
      return result;
    }
  }

  result.ok = true;
  result.invocation = invocations.front();
  result.function = &*function;
  result.result_descriptor = &*result_descriptor;
  result.aggregate_count_star = aggregate_count_star_window;
  result.window_property_uuid = *window_property_uuid;
  result.window_frame_descriptor_uuid =
      window_property->window_frame_descriptor_uuid;
  return result;
}

PreparedGlobalRowNumberWindowBinding PrepareGlobalRowNumberWindowBinding(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalPropertyCatalog& logical_properties,
    const api::RelationalDagNode& consumer,
    const plan::CanonicalLogicalRelationalNode& logical_consumer,
    const plan::CanonicalLogicalRelationalNode& previous_logical,
    const PreparedSortRoot& prepared_sort,
    const std::size_t materialized_column_count,
    const std::size_t result_binding_count,
    const std::string& int64_type_uuid,
    const std::string_view family_label,
    const bool allow_project_root = false) {
  return PrepareGlobalRankingWindowBinding(
      dag, logical_properties, consumer, logical_consumer, previous_logical,
      prepared_sort, materialized_column_count, result_binding_count,
      int64_type_uuid, int64_type_uuid, int64_type_uuid,
      std::array<std::string, 4>{}, family_label, kGlobalRowNumberProfile,
      allow_project_root);
}

}  // namespace

bool CanonicalDescriptorFieldEqualsForComposition(
    const api::EngineDescriptor& descriptor,
    const std::string_view key,
    const std::optional<std::string_view> expected) {
  return CanonicalDescriptorFieldEqualsV1(descriptor, key, expected);
}

bool DirectValueWindowUsesExactTypeForComposition(
    const api::TypedRelationalDag& dag,
    const std::uint32_t relation_node_id,
    const std::string_view expected_builtin_id,
    const std::string_view type_uuid) {
  return DirectValueWindowUsesExactTypeV1(
      dag, relation_node_id, expected_builtin_id, type_uuid);
}

bool ExactCanonicalBooleanWindowSourceForComposition(
    const api::RelationalTypeDescriptor& relational_descriptor,
    const api::EngineDescriptor& runtime_descriptor,
    const bool runtime_nullable,
    const std::string_view boolean_type_uuid,
    const std::string_view function_uuid,
    const std::string_view result_descriptor_uuid,
    const std::string_view ordering_property_uuid,
    const std::string_view window_property_uuid,
    const std::string_view window_frame_descriptor_uuid) {
  return ExactCanonicalBooleanWindowSourceV1(
      relational_descriptor, runtime_descriptor, runtime_nullable,
      boolean_type_uuid, function_uuid, result_descriptor_uuid,
      ordering_property_uuid, window_property_uuid,
      window_frame_descriptor_uuid);
}

bool ExactCanonicalScalarWindowOperandForComposition(
    const api::RelationalTypeDescriptor& relational_descriptor,
    const api::EngineDescriptor& runtime_descriptor,
    const bool runtime_nullable,
    const std::string_view function_uuid,
    const std::string_view result_descriptor_uuid,
    const std::string_view result_type_uuid,
    const std::string_view counterpart_descriptor_uuid,
    const std::string_view counterpart_type_uuid,
    const bool same_operand_ordinal,
    const std::string_view ordering_property_uuid,
    const std::string_view window_property_uuid,
    const std::string_view window_frame_descriptor_uuid) {
  return ExactCanonicalScalarWindowOperandV1(
      relational_descriptor, runtime_descriptor, runtime_nullable,
      function_uuid, result_descriptor_uuid, result_type_uuid,
      counterpart_descriptor_uuid, counterpart_type_uuid,
      same_operand_ordinal, ordering_property_uuid, window_property_uuid,
      window_frame_descriptor_uuid);
}

bool ExactCanonicalBoundedSignedWindowSourceForComposition(
    const api::RelationalTypeDescriptor& relational_descriptor,
    const api::EngineDescriptor& runtime_descriptor,
    const bool runtime_nullable,
    const std::array<std::string, 4>& bounded_signed_type_uuids,
    const std::string_view function_uuid,
    const std::string_view result_descriptor_uuid,
    const std::string_view ordering_property_uuid,
    const std::string_view window_property_uuid,
    const std::string_view window_frame_descriptor_uuid) {
  return ExactCanonicalBoundedSignedWindowSourceV1(
      relational_descriptor, runtime_descriptor, runtime_nullable,
      bounded_signed_type_uuids, function_uuid, result_descriptor_uuid,
      ordering_property_uuid, window_property_uuid,
      window_frame_descriptor_uuid);
}

bool ExactCanonicalBoundedSignedWindowOrderForComposition(
    const api::RelationalTypeDescriptor& relational_descriptor,
    const api::EngineDescriptor& runtime_descriptor,
    const bool runtime_nullable,
    const std::array<std::string, 4>& bounded_signed_type_uuids,
    const std::string_view function_uuid,
    const std::string_view result_descriptor_uuid,
    const std::string_view result_type_uuid,
    const std::string_view ordering_property_uuid,
    const std::string_view window_property_uuid,
    const std::string_view window_frame_descriptor_uuid) {
  return ExactCanonicalBoundedSignedWindowOrderV1(
      relational_descriptor, runtime_descriptor, runtime_nullable,
      bounded_signed_type_uuids, function_uuid, result_descriptor_uuid,
      result_type_uuid, ordering_property_uuid, window_property_uuid,
      window_frame_descriptor_uuid);
}

GlobalRankingWindowProfile GlobalAggregateWindowProfileForComposition(
    const api::TypedRelationalDag& dag,
    const std::uint32_t relation_node_id) {
  return GlobalAggregateWindowProfileV1(dag, relation_node_id);
}

PreparedGlobalRowNumberWindowBinding
PrepareGlobalRankingWindowBindingForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalPropertyCatalog& logical_properties,
    const api::RelationalDagNode& consumer,
    const plan::CanonicalLogicalRelationalNode& logical_consumer,
    const plan::CanonicalLogicalRelationalNode& previous_logical,
    const PreparedSortRoot& prepared_sort,
    const std::size_t materialized_column_count,
    const std::size_t result_binding_count,
    const std::string& result_type_uuid,
    const std::string& order_type_uuid,
    const std::string& boolean_type_uuid,
    const std::array<std::string, 4>& bounded_signed_type_uuids,
    const std::string_view family_label,
    const GlobalRankingWindowProfile& profile,
    const bool allow_project_root) {
  return PrepareGlobalRankingWindowBinding(
      dag, logical_properties, consumer, logical_consumer, previous_logical,
      prepared_sort, materialized_column_count, result_binding_count,
      result_type_uuid, order_type_uuid, boolean_type_uuid,
      bounded_signed_type_uuids, family_label, profile, allow_project_root);
}

PreparedGlobalRowNumberWindowBinding
PrepareGlobalRowNumberWindowBindingForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalPropertyCatalog& logical_properties,
    const api::RelationalDagNode& consumer,
    const plan::CanonicalLogicalRelationalNode& logical_consumer,
    const plan::CanonicalLogicalRelationalNode& previous_logical,
    const PreparedSortRoot& prepared_sort,
    const std::size_t materialized_column_count,
    const std::size_t result_binding_count,
    const std::string& int64_type_uuid,
    const std::string_view family_label,
    const bool allow_project_root) {
  return PrepareGlobalRowNumberWindowBinding(
      dag, logical_properties, consumer, logical_consumer, previous_logical,
      prepared_sort, materialized_column_count, result_binding_count,
      int64_type_uuid, family_label, allow_project_root);
}

unsigned ExactBoundedSignedIntegerTypeRankForComposition(
    const std::string_view type_uuid) {
  return ExactBoundedSignedIntegerTypeRankV1(type_uuid);
}

}  // namespace scratchbird::engine::sblr
