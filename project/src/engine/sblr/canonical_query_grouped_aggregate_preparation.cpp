// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_aggregate_composition.hpp"
#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_predicate_support.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_relational_expression.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_GROUPED_AGGREGATE_PREPARATION_AUTHORITY
// Prepares typed aggregate profiles, descriptors, and bounded workspaces.
// Owns no plan selection, executor dispatch, storage access, MGA snapshot
// construction, transaction finality, or public route selection.

namespace {

LiveGroupedCountSumProfile MatchLiveGroupedCountSumProfile(
    const std::string_view semantic_variant_id) {
  LiveGroupedCountSumProfile result;
  if (semantic_variant_id ==
      "aggregate.grouped-int64-key-count-sum.v1") {
    result.matched = true;
    result.key_count = 1;
    result.grouping_sets = {{{0}}};
    result.transformation_id =
        "canonical.aggregate.grouped-int64-key-count-sum.v1";
  } else if (semantic_variant_id ==
             "aggregate.grouped-int64-keys-count-sum.v1") {
    // QOW-SOURCE-QRY-005-LIVE-TWO-KEY-GROUP-BY-V1
    result.matched = true;
    result.key_count = 2;
    result.grouping_sets = {{{0, 1}}};
    result.transformation_id =
        "canonical.aggregate.grouped-int64-keys-count-sum.v1";
  } else if (semantic_variant_id ==
             "aggregate.rollup-int64-keys-count-sum.v1") {
    result.matched = true;
    result.key_count = 2;
    result.expansion_kind =
        exec::CanonicalAggregateGroupingExpansionKind::rollup;
    result.transformation_id =
        "canonical.aggregate.rollup-int64-keys-count-sum.v1";
  } else if (semantic_variant_id ==
             "aggregate.rollup-int64-keys-count-sum-grouping.v1") {
    result.matched = true;
    result.key_count = 2;
    result.expansion_kind =
        exec::CanonicalAggregateGroupingExpansionKind::rollup;
    result.projects_grouping_metadata = true;
    result.transformation_id =
        "canonical.aggregate.rollup-int64-keys-count-sum-grouping.v1";
  } else if (semantic_variant_id ==
             "aggregate.cube-int64-keys-count-sum.v1") {
    result.matched = true;
    result.key_count = 2;
    result.expansion_kind =
        exec::CanonicalAggregateGroupingExpansionKind::cube;
    result.transformation_id =
        "canonical.aggregate.cube-int64-keys-count-sum.v1";
  } else if (semantic_variant_id ==
             "aggregate.cube-int64-keys-count-sum-grouping.v1") {
    result.matched = true;
    result.key_count = 2;
    result.expansion_kind =
        exec::CanonicalAggregateGroupingExpansionKind::cube;
    result.projects_grouping_metadata = true;
    result.transformation_id =
        "canonical.aggregate.cube-int64-keys-count-sum-grouping.v1";
  } else if (semantic_variant_id ==
             "aggregate.grouping-sets-int64-keys-count-sum.v1") {
    result.matched = true;
    result.key_count = 2;
    result.grouping_sets_from_sblr = true;
    result.transformation_id =
        "canonical.aggregate.grouping-sets-int64-keys-count-sum.v1";
  } else if (semantic_variant_id ==
             "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1") {
    result.matched = true;
    result.key_count = 2;
    result.grouping_sets_from_sblr = true;
    result.projects_grouping_metadata = true;
    result.transformation_id =
        "canonical.aggregate.grouping-sets-int64-keys-count-sum-grouping.v1";
  }
  return result;
}

bool IsLiveGroupedHavingProfile(const std::string_view semantic_variant_id) {
  return semantic_variant_id ==
             "filter.having-sum-gt-int64-literal.v1" ||
         semantic_variant_id ==
             "filter.having-not-not-sum-gt-int64-literal.v1" ||
         semantic_variant_id ==
             "filter.having-not-not-count-gt-int64-literal.v1" ||
         semantic_variant_id ==
             "filter.having-not-not-count-sum-and-gt-int64-literals.v1" ||
         semantic_variant_id ==
             "filter.having-not-not-count-sum-or-gt-int64-literals.v1" ||
         semantic_variant_id ==
             "filter.having-not-not-sum-count-or-gt-int64-literals.v1" ||
         semantic_variant_id ==
             "filter.having-not-sum-gt-int64-literal.v1" ||
         semantic_variant_id ==
             "filter.having-not-count-gt-int64-literal.v1" ||
         semantic_variant_id ==
             "filter.having-not-count-sum-and-gt-int64-literals.v1" ||
         semantic_variant_id ==
             "filter.having-not-count-sum-or-gt-int64-literals.v1" ||
         semantic_variant_id ==
             "filter.having-count-sum-and-gt-int64-literals.v1" ||
         semantic_variant_id ==
             "filter.having-count-sum-or-gt-int64-literals.v1";
}

PreparedGroupedCountSumRoot PrepareGroupedCountSumRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    const LiveGroupedCountSumProfile& profile) {
  PreparedGroupedCountSumRoot result;
  const auto grouping_projection_count =
      profile.projects_grouping_metadata ? profile.key_count + 1 : 0;
  const auto expected_output_count =
      profile.key_count + 2 + grouping_projection_count;
  if (!profile.matched || profile.key_count == 0 ||
      (!profile.grouping_sets_from_sblr &&
       profile.expansion_kind ==
           exec::CanonicalAggregateGroupingExpansionKind::explicit_sets &&
       profile.grouping_sets.empty()) ||
      (profile.grouping_sets_from_sblr && !profile.grouping_sets.empty()) ||
      root.output_descriptor_ids.size() != expected_output_count ||
      root.bound_expression_ids.size() != expected_output_count ||
      input.result_bindings.size() != input.batch.columns.size() ||
      input_node.output_descriptor_ids.size() != input.batch.columns.size() ||
      std::ranges::any_of(root.output_descriptor_ids,
                          [&](const auto descriptor_id) {
                            return std::ranges::count(
                                       root.output_descriptor_ids,
                                       descriptor_id) != 1;
                          }) ||
      root.output_descriptor_ids[profile.key_count] ==
          root.output_descriptor_ids[profile.key_count + 1]) {
    result.detail =
        "grouped COUNT/SUM shape does not match its exact key profile";
    return result;
  }

  std::vector<exec::CanonicalAggregateGroupingSet> explicit_grouping_sets;
  if (profile.grouping_sets_from_sblr) {
    std::vector<const api::RelationalGroupingSetRecord*> grouping_sets;
    for (const auto& grouping_set : dag.grouping_sets) {
      if (grouping_set.relation_node_id == root.logical_node_id) {
        grouping_sets.push_back(&grouping_set);
      }
    }
    std::ranges::sort(grouping_sets, {},
                      &api::RelationalGroupingSetRecord::ordinal);
    for (std::size_t ordinal = 0; ordinal < grouping_sets.size(); ++ordinal) {
      if (grouping_sets[ordinal]->ordinal != ordinal) {
        result.detail =
            "grouped COUNT/SUM grouping-set ordinals are not dense";
        return result;
      }
      exec::CanonicalAggregateGroupingSet prepared;
      for (const auto expression_id : grouping_sets[ordinal]->expression_ids) {
        const auto key = std::ranges::find(
            root.bound_expression_ids.begin(),
            root.bound_expression_ids.begin() + profile.key_count,
            expression_id);
        if (key == root.bound_expression_ids.begin() + profile.key_count) {
          result.detail =
              "grouped COUNT/SUM grouping-set member is not a bound key";
          return result;
        }
        const auto key_ordinal = static_cast<std::size_t>(std::distance(
            root.bound_expression_ids.begin(), key));
        if (!prepared.key_term_ordinals.empty() &&
            key_ordinal <= prepared.key_term_ordinals.back()) {
          result.detail =
              "grouped COUNT/SUM grouping-set members are not in key order";
          return result;
        }
        prepared.key_term_ordinals.push_back(key_ordinal);
      }
      explicit_grouping_sets.push_back(std::move(prepared));
    }
  } else {
    if (std::ranges::any_of(
            dag.grouping_sets, [&](const auto& grouping_set) {
              return grouping_set.relation_node_id == root.logical_node_id;
            })) {
      result.detail =
          "grouped COUNT/SUM fixed grouping profile has an unexpected payload";
      return result;
    }
    explicit_grouping_sets = profile.grouping_sets;
  }
  exec::CanonicalAggregateGroupingExpansionRequest expansion_request;
  expansion_request.kind = profile.expansion_kind;
  expansion_request.group_key_count = profile.key_count;
  expansion_request.explicit_grouping_sets =
      std::move(explicit_grouping_sets);
  const auto expansion =
      exec::ExpandCanonicalAggregateGroupingSets(expansion_request);
  if (!expansion.diagnostic.ok) {
    result.detail = "grouped COUNT/SUM expansion: " +
                    expansion.diagnostic.detail;
    return result;
  }
  result.grouping_sets = expansion.grouping_sets;
  std::vector<bool> key_used(profile.key_count, false);
  for (const auto& grouping_set : result.grouping_sets) {
    for (const auto key_ordinal : grouping_set.key_term_ordinals) {
      key_used[key_ordinal] = true;
    }
  }
  if (std::ranges::find(key_used, false) != key_used.end()) {
    result.detail =
        "grouped COUNT/SUM grouping-set expansion has an unused key";
    return result;
  }
  const auto core_int64_key_type_uuid = ExactCanonicalInt64TypeUuidV1();
  if (!CanonicalUuidText(core_int64_key_type_uuid)) {
    result.detail =
        "grouped COUNT/SUM core int64 key identity is unavailable";
    return result;
  }
  for (std::size_t key_ordinal = 0; key_ordinal < profile.key_count;
       ++key_ordinal) {
    const auto key_expression = std::ranges::find_if(
        dag.expressions, [&](const auto& candidate) {
          return candidate.expression_id ==
                 root.bound_expression_ids[key_ordinal];
        });
    const auto key_descriptor = std::ranges::find_if(
        dag.descriptors, [&](const auto& candidate) {
          return key_expression != dag.expressions.end() &&
                 candidate.descriptor_id ==
                     key_expression->result_descriptor_id;
        });
    const auto key_output = std::ranges::find_if(
        dag.outputs, [&](const auto& candidate) {
          return key_expression != dag.expressions.end() &&
                 candidate.relation_node_id == root.logical_node_id &&
                 candidate.expression_id == key_expression->expression_id &&
                 candidate.descriptor_id ==
                     key_expression->result_descriptor_id;
        });
    if (key_expression == dag.expressions.end() ||
        key_descriptor == dag.descriptors.end() ||
        key_output == dag.outputs.end() ||
        key_expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        !key_expression->child_expression_ids.empty() ||
        !key_expression->bound_name_uuid.has_value() ||
        key_expression->function_uuid.has_value() ||
        key_expression->literal_kind.has_value() ||
        key_expression->operator_name.has_value() ||
        key_expression->literal_or_parameter_ref.has_value() ||
        root.output_descriptor_ids[key_ordinal] !=
            key_expression->result_descriptor_id ||
        key_output->ordinal != key_ordinal || !key_output->visible ||
        key_output->output_name_utf8.empty()) {
      result.detail =
          "grouped COUNT/SUM key lineage is not an exact bound identifier";
      return result;
    }
    const auto supplied_key = std::ranges::find(
        input_node.output_descriptor_ids,
        key_expression->result_descriptor_id);
    if (supplied_key == input_node.output_descriptor_ids.end() ||
        std::ranges::count(input_node.output_descriptor_ids,
                           key_expression->result_descriptor_id) != 1 ||
        std::ranges::count(root.output_descriptor_ids,
                           key_expression->result_descriptor_id) != 1) {
      result.detail =
          "grouped COUNT/SUM key descriptor is not uniquely bound";
      return result;
    }
    const auto key_column = static_cast<std::size_t>(std::distance(
        input_node.output_descriptor_ids.begin(), supplied_key));
    if (key_column >= input.batch.columns.size() ||
        input.batch.columns[key_column].descriptor_id !=
            key_expression->result_descriptor_id) {
      result.detail =
          "grouped COUNT/SUM key must be a descriptor-exact int64 column";
      return result;
    }
    const auto& runtime_key = input.batch.columns[key_column];
    const auto expected_nullability =
        runtime_key.nullable ? std::string_view("nullable")
                             : std::string_view("non_null");
    if (key_descriptor->type_uuid != core_int64_key_type_uuid ||
        key_descriptor->descriptor_uuid !=
            runtime_key.descriptor.descriptor_uuid.canonical ||
        key_descriptor->descriptor_uuid == key_descriptor->type_uuid ||
        key_descriptor->nullability !=
            (runtime_key.nullable
                 ? api::RelationalNullability::kNullable
                 : api::RelationalNullability::kNonNull) ||
        key_descriptor->collation_uuid.has_value() ||
        key_descriptor->timezone_profile_id.has_value() ||
        key_descriptor->width.has_value() ||
        key_descriptor->precision.has_value() ||
        key_descriptor->scale.has_value() ||
        runtime_key.descriptor.descriptor_kind != "scalar" ||
        runtime_key.descriptor.canonical_type_name != "int64" ||
        !api::QowCanonicalDescriptorIdentityV1(runtime_key.descriptor) ||
        runtime_key.descriptor.encoded_descriptor !=
            "type_uuid=" + core_int64_key_type_uuid +
                ";nullability=" + std::string(expected_nullability)) {
      result.detail =
          "grouped COUNT/SUM key must bind the exact core int64 type";
      return result;
    }

    exec::CanonicalDescriptorOrderTerm key_term;
    key_term.column = key_column;
    key_term.expression_descriptor_id = key_expression->result_descriptor_id;
    key_term.direction = exec::CanonicalDescriptorOrderDirection::ascending;
    key_term.null_placement = exec::CanonicalDescriptorNullPlacement::last;
    const auto key_validation = exec::ValidateCanonicalDescriptorOrderTerm(
        key_term, input.batch.columns[key_column]);
    if (!key_validation.ok) {
      result.detail = key_validation.detail;
      return result;
    }
    result.key_terms.push_back(std::move(key_term));
    result.key_binding_receipts.push_back(
        PreparedAggregateValueBindingReceipt{
            key_column,
            key_expression->result_descriptor_id,
            key_descriptor->descriptor_uuid,
            key_descriptor->type_uuid,
            runtime_key.nullable,
            runtime_key.descriptor.canonical_type_name,
            runtime_key.descriptor.encoded_descriptor});

    auto key_result_column = input.batch.columns[key_column];
    key_result_column.stable_name = key_output->output_name_utf8;
    const bool key_can_be_omitted = std::ranges::any_of(
        result.grouping_sets, [&](const auto& grouping_set) {
          return std::ranges::find(grouping_set.key_term_ordinals,
                                   key_ordinal) ==
                 grouping_set.key_term_ordinals.end();
        });
    if (key_can_be_omitted && !key_result_column.nullable) {
      result.detail =
          "grouped COUNT/SUM grouping-set key result must admit grouping NULL";
      return result;
    }
    result.key_result_columns.push_back(std::move(key_result_column));

    exec::CanonicalResultColumnBinding key_binding;
    key_binding.physical_column_ordinal = key_ordinal;
    key_binding.visible = true;
    key_binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
        static_cast<std::uint32_t>(key_ordinal),
        key_output->output_name_utf8,
        key_descriptor->descriptor_uuid,
        key_descriptor->type_uuid,
        ResultNullability(key_descriptor->nullability),
        key_descriptor->collation_uuid,
        key_descriptor->timezone_profile_id};
    result.result_bindings.push_back(std::move(key_binding));
  }

  auto count_root = root;
  count_root.output_descriptor_ids = {
      root.output_descriptor_ids[profile.key_count]};
  count_root.bound_expression_ids = {
      root.bound_expression_ids[profile.key_count]};
  result.count = PrepareGlobalAggregateRootForComposition(
      dag, count_root, input_node, input,
      exec::CanonicalAggregateFunction::count, true, false, false,
      static_cast<std::uint32_t>(profile.key_count), true);
  if (!result.count.ok) {
    result.detail = "grouped COUNT: " + result.count.detail;
    return result;
  }

  auto sum_root = root;
  sum_root.output_descriptor_ids = {
      root.output_descriptor_ids[profile.key_count + 1]};
  sum_root.bound_expression_ids = {
      root.bound_expression_ids[profile.key_count + 1]};
  result.sum = PrepareGlobalAggregateRootForComposition(
      dag, sum_root, input_node, input,
      exec::CanonicalAggregateFunction::sum, false, false, false,
      static_cast<std::uint32_t>(profile.key_count + 1), true);
  if (!result.sum.ok) {
    result.detail = "grouped SUM: " + result.sum.detail;
    return result;
  }

  result.result_bindings.push_back(result.count.result_bindings.front());
  result.result_bindings.push_back(result.sum.result_bindings.front());

  if (profile.projects_grouping_metadata) {
    const auto grouping_int64_type_uuid = ExactCanonicalInt64TypeUuidV1();
    if (!CanonicalUuidText(grouping_int64_type_uuid)) {
      result.detail = "GROUPING metadata core int64 identity is not exact";
      return result;
    }
    std::unordered_set<std::string_view> grouping_descriptor_uuids;
    const auto prepare_projection =
        [&](const std::size_t projection_ordinal,
            const api::RelationalExpressionKind expected_kind,
            const std::string_view expected_operator,
            const std::vector<std::uint32_t>& expected_children) {
          const auto expression_id =
              root.bound_expression_ids[projection_ordinal];
          const auto descriptor_id =
              root.output_descriptor_ids[projection_ordinal];
          const auto expression = std::ranges::find_if(
              dag.expressions, [&](const auto& candidate) {
                return candidate.expression_id == expression_id;
              });
          const auto descriptor = std::ranges::find_if(
              dag.descriptors, [&](const auto& candidate) {
                return candidate.descriptor_id == descriptor_id;
              });
          const auto output = std::ranges::find_if(
              dag.outputs, [&](const auto& candidate) {
                return candidate.relation_node_id == root.logical_node_id &&
                       candidate.expression_id == expression_id &&
                       candidate.descriptor_id == descriptor_id;
              });
          if (expression == dag.expressions.end() ||
              descriptor == dag.descriptors.end() ||
              output == dag.outputs.end() ||
              expression->expression_kind != expected_kind ||
              expression->child_expression_ids != expected_children ||
              expression->result_descriptor_id != descriptor_id ||
              expression->function_uuid.has_value() ||
              expression->bound_name_uuid.has_value() ||
              expression->literal_kind.has_value() ||
              expression->operator_name != expected_operator ||
              expression->literal_or_parameter_ref.has_value() ||
              descriptor->type_uuid != grouping_int64_type_uuid ||
              !CanonicalUuidText(descriptor->descriptor_uuid) ||
              descriptor->descriptor_uuid == grouping_int64_type_uuid ||
              !grouping_descriptor_uuids
                   .insert(descriptor->descriptor_uuid)
                   .second ||
              descriptor->nullability !=
                  api::RelationalNullability::kNonNull ||
              descriptor->collation_uuid.has_value() ||
              descriptor->timezone_profile_id.has_value() ||
              descriptor->width.has_value() ||
              descriptor->precision.has_value() ||
              descriptor->scale.has_value() ||
              output->ordinal != projection_ordinal || !output->visible ||
              output->output_name_utf8.empty() ||
              std::ranges::count_if(
                  dag.outputs, [&](const auto& candidate) {
                    return candidate.relation_node_id ==
                               root.logical_node_id &&
                           candidate.expression_id == expression_id &&
                           candidate.descriptor_id == descriptor_id;
                  }) != 1) {
            return false;
          }

          api::EngineDescriptor engine_descriptor;
          engine_descriptor.descriptor_uuid.canonical =
              descriptor->descriptor_uuid;
          engine_descriptor.descriptor_kind = "scalar";
          engine_descriptor.canonical_type_name = "int64";
          engine_descriptor.encoded_descriptor =
              "type_uuid=" + descriptor->type_uuid +
              ";nullability=non_null";
          result.grouping_projection_columns.push_back(
              {output->output_name_utf8, engine_descriptor, false,
               descriptor_id});

          exec::CanonicalResultColumnBinding binding;
          binding.physical_column_ordinal = projection_ordinal;
          binding.visible = true;
          binding.published_descriptor =
              exec::CanonicalResultColumnDescriptor{
                  static_cast<std::uint32_t>(projection_ordinal),
                  output->output_name_utf8,
                  descriptor->descriptor_uuid,
                  descriptor->type_uuid,
                  exec::CanonicalResultNullability::kNonNull,
                  std::nullopt,
                  std::nullopt};
          result.result_bindings.push_back(std::move(binding));
          return true;
        };

    const auto projection_begin = profile.key_count + 2;
    for (std::size_t key_ordinal = 0; key_ordinal < profile.key_count;
         ++key_ordinal) {
      if (!prepare_projection(
              projection_begin + key_ordinal,
              api::RelationalExpressionKind::kUnary, "grouping",
              {root.bound_expression_ids[key_ordinal]})) {
        result.detail =
            "GROUPING projection is not an exact bound-key special form";
        return result;
      }
    }
    std::vector<std::uint32_t> grouping_id_children;
    grouping_id_children.insert(
        grouping_id_children.end(), root.bound_expression_ids.begin(),
        root.bound_expression_ids.begin() +
            static_cast<std::ptrdiff_t>(profile.key_count));
    if (!prepare_projection(
            projection_begin + profile.key_count,
            api::RelationalExpressionKind::kBinary, "grouping_id",
            grouping_id_children)) {
      result.detail =
          "GROUPING_ID projection is not an exact ordered-key special form";
      return result;
    }
  }
  result.ok = true;
  return result;
}

// QOW-SOURCE-QRY-001-HAVING-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-HAVING-COUNT-SUM-AND-GT-LIVE-V1
// QOW-SOURCE-QRY-001-HAVING-COUNT-SUM-OR-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-COUNT-SUM-OR-GT-LIVE-V1
// QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-COUNT-SUM-OR-GT-LIVE-V1
// QOW-SOURCE-QRY-001-ROLLUP-HAVING-COUNT-SUM-OR-GT-LIVE-V1
// QOW-SOURCE-QRY-001-CUBE-HAVING-COUNT-SUM-OR-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-COUNT-SUM-AND-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-COUNT-SUM-AND-GT-LIVE-V1
// QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-GROUPING-SETS-GROUPING-METADATA-HAVING-LIVE-V1
// QOW-SOURCE-QRY-001-GROUPING-SETS-GROUPING-METADATA-HAVING-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-ROLLUP-HAVING-COUNT-SUM-AND-GT-LIVE-V1
// QOW-SOURCE-QRY-001-ROLLUP-HAVING-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-ROLLUP-GROUPING-METADATA-HAVING-LIVE-V1
// QOW-SOURCE-QRY-001-ROLLUP-GROUPING-METADATA-HAVING-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-CUBE-HAVING-COUNT-SUM-AND-GT-LIVE-V1
// QOW-SOURCE-QRY-001-CUBE-HAVING-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-CUBE-GROUPING-METADATA-HAVING-LIVE-V1
// QOW-SOURCE-QRY-001-CUBE-GROUPING-METADATA-HAVING-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-NOT-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-GROUPING-SETS-GROUPING-METADATA-HAVING-NOT-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-ROLLUP-HAVING-NOT-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-ROLLUP-GROUPING-METADATA-HAVING-NOT-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-CUBE-HAVING-NOT-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-CUBE-GROUPING-METADATA-HAVING-NOT-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-COUNT-SUM-AND-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-NOT-COUNT-SUM-AND-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-NOT-COUNT-SUM-OR-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-COUNT-SUM-OR-GT-LIVE-V1
// QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-NOT-COUNT-SUM-AND-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-COUNT-GT-LIVE-V1
PreparedGroupedHavingRoot PrepareGroupedHavingRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& filter_root,
    const plan::CanonicalLogicalRelationalNode& aggregate_root,
    const plan::CanonicalLogicalRelationalNode& values_node,
    const PreparedGroupedCountSumRoot& prepared_aggregate) {
  PreparedGroupedHavingRoot result;
  const auto* sum_registry_entry =
      exec::LookupCanonicalAggregateByFunctionV1(
          exec::CanonicalAggregateFunction::sum);
  const auto* count_registry_entry =
      exec::LookupCanonicalAggregateByFunctionV1(
          exec::CanonicalAggregateFunction::count);
  if (sum_registry_entry == nullptr || count_registry_entry == nullptr ||
      !sum_registry_entry->executable || !count_registry_entry->executable) {
    result.detail = "canonical COUNT/SUM aggregate registry is unavailable";
    return result;
  }
  const bool simple_sum_profile =
      filter_root.semantic_variant_id ==
      "filter.having-sum-gt-int64-literal.v1";
  // QOW-SOURCE-QRY-001-ENGINE-TWO-KEY-HAVING-NOT-NOT-SUM-GT-V1
  const bool not_not_sum_profile =
      filter_root.semantic_variant_id ==
      "filter.having-not-not-sum-gt-int64-literal.v1";
  // QOW-SOURCE-QRY-001-ENGINE-TWO-KEY-HAVING-NOT-NOT-COUNT-GT-V1
  const bool not_not_count_profile =
      filter_root.semantic_variant_id ==
      "filter.having-not-not-count-gt-int64-literal.v1";
  const bool not_not_count_sum_and_profile =
      filter_root.semantic_variant_id ==
      "filter.having-not-not-count-sum-and-gt-int64-literals.v1";
  const bool not_not_count_sum_or_profile =
      filter_root.semantic_variant_id ==
      "filter.having-not-not-count-sum-or-gt-int64-literals.v1";
  // QOW-SOURCE-QRY-001-ENGINE-TWO-KEY-HAVING-NOT-NOT-SUM-COUNT-OR-GT-LIVE-V1
  const bool not_not_sum_count_or_profile =
      filter_root.semantic_variant_id ==
      "filter.having-not-not-sum-count-or-gt-int64-literals.v1";
  const bool not_not_count_sum_boolean_profile =
      not_not_count_sum_and_profile || not_not_count_sum_or_profile ||
      not_not_sum_count_or_profile;
  const bool not_sum_profile =
      filter_root.semantic_variant_id ==
      "filter.having-not-sum-gt-int64-literal.v1";
  const bool not_count_profile =
      filter_root.semantic_variant_id ==
          "filter.having-not-count-gt-int64-literal.v1";
  const bool not_count_sum_and_profile =
      filter_root.semantic_variant_id ==
      "filter.having-not-count-sum-and-gt-int64-literals.v1";
  const bool not_count_sum_or_profile =
      filter_root.semantic_variant_id ==
      "filter.having-not-count-sum-or-gt-int64-literals.v1";
  const bool not_count_sum_boolean_profile =
      not_count_sum_and_profile || not_count_sum_or_profile;
  const bool count_sum_and_profile =
      filter_root.semantic_variant_id ==
      "filter.having-count-sum-and-gt-int64-literals.v1";
  const bool count_sum_or_profile =
      filter_root.semantic_variant_id ==
      "filter.having-count-sum-or-gt-int64-literals.v1";
  const bool count_sum_boolean_profile =
      count_sum_and_profile || count_sum_or_profile ||
      not_count_sum_boolean_profile || not_not_count_sum_boolean_profile;
  std::size_t key_count = 0;
  std::size_t grouping_projection_count = 0;
  if (aggregate_root.semantic_variant_id ==
      "aggregate.grouped-int64-key-count-sum.v1") {
    key_count = 1;
  } else if (aggregate_root.semantic_variant_id ==
             "aggregate.grouped-int64-keys-count-sum.v1") {
    key_count = 2;
  } else if (aggregate_root.semantic_variant_id ==
             "aggregate.grouping-sets-int64-keys-count-sum.v1") {
    key_count = 2;
  } else if (aggregate_root.semantic_variant_id ==
             "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1") {
    key_count = 2;
    grouping_projection_count = key_count + 1;
  } else if (aggregate_root.semantic_variant_id ==
             "aggregate.rollup-int64-keys-count-sum.v1") {
    key_count = 2;
  } else if (aggregate_root.semantic_variant_id ==
             "aggregate.rollup-int64-keys-count-sum-grouping.v1") {
    key_count = 2;
    grouping_projection_count = key_count + 1;
  } else if (aggregate_root.semantic_variant_id ==
             "aggregate.cube-int64-keys-count-sum.v1") {
    key_count = 2;
  } else if (aggregate_root.semantic_variant_id ==
             "aggregate.cube-int64-keys-count-sum-grouping.v1") {
    key_count = 2;
    grouping_projection_count = key_count + 1;
  } else {
    result.detail =
        "HAVING aggregate is not an admitted grouped COUNT/SUM profile";
    return result;
  }
  const auto expected_output_count =
      key_count + 2 + grouping_projection_count;
  const bool admitted_two_key_sum_profile =
      simple_sum_profile &&
      (aggregate_root.semantic_variant_id ==
           "aggregate.grouped-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.grouping-sets-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.rollup-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.rollup-int64-keys-count-sum-grouping.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.cube-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.cube-int64-keys-count-sum-grouping.v1");
  const bool exact_ordinary_two_key_not_sum_profile =
      not_sum_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_ordinary_two_key_not_not_sum_profile =
      not_not_sum_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_ordinary_two_key_not_not_count_profile =
      not_not_count_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_ordinary_two_key_not_not_count_sum_and_profile =
      not_not_count_sum_and_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_ordinary_two_key_not_not_count_sum_or_profile =
      not_not_count_sum_or_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_ordinary_two_key_not_not_sum_count_or_profile =
      not_not_sum_count_or_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_ordinary_two_key_not_count_profile =
      not_count_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_ordinary_two_key_not_count_sum_and_profile =
      not_count_sum_and_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_ordinary_two_key_not_count_sum_or_profile =
      not_count_sum_or_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_grouping_sets_not_count_sum_and_profile =
      not_count_sum_and_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouping-sets-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals.empty() &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals ==
          prepared_aggregate.grouping_sets[0].key_term_ordinals;
  // QOW-SOURCE-QRY-001-GROUPING-SETS-GROUPING-METADATA-HAVING-NOT-COUNT-SUM-AND-GT-LIVE-V1
  const bool exact_grouping_sets_metadata_not_count_sum_and_profile =
      not_count_sum_and_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1" &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals.empty() &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals ==
          prepared_aggregate.grouping_sets[0].key_term_ordinals;
  // QOW-SOURCE-QRY-001-ROLLUP-HAVING-NOT-COUNT-SUM-AND-GT-LIVE-V1
  const bool exact_rollup_not_count_sum_and_profile =
      not_count_sum_and_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.rollup-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 3 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals.empty();
  // QOW-SOURCE-QRY-001-ROLLUP-GROUPING-METADATA-HAVING-NOT-COUNT-SUM-AND-GT-LIVE-V1
  const bool exact_rollup_metadata_not_count_sum_and_profile =
      not_count_sum_and_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.rollup-int64-keys-count-sum-grouping.v1" &&
      prepared_aggregate.grouping_sets.size() == 3 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals.empty();
  // QOW-SOURCE-QRY-001-CUBE-HAVING-NOT-COUNT-SUM-AND-GT-LIVE-V1
  const bool exact_cube_not_count_sum_and_profile =
      not_count_sum_and_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.cube-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals.empty();
  // QOW-SOURCE-QRY-001-CUBE-GROUPING-METADATA-HAVING-NOT-COUNT-SUM-AND-GT-LIVE-V1
  const bool exact_cube_metadata_not_count_sum_and_profile =
      not_count_sum_and_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.cube-int64-keys-count-sum-grouping.v1" &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals.empty();
  const bool admitted_not_count_sum_and_profile =
      exact_ordinary_two_key_not_count_sum_and_profile ||
      exact_grouping_sets_not_count_sum_and_profile ||
      exact_grouping_sets_metadata_not_count_sum_and_profile ||
      exact_rollup_not_count_sum_and_profile ||
      exact_rollup_metadata_not_count_sum_and_profile ||
      exact_cube_not_count_sum_and_profile ||
      exact_cube_metadata_not_count_sum_and_profile;
  const bool exact_grouping_sets_not_sum_profile =
      not_sum_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouping-sets-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals.empty() &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals ==
          prepared_aggregate.grouping_sets[0].key_term_ordinals;
  const bool exact_grouping_sets_metadata_not_sum_profile =
      not_sum_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1" &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals.empty() &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals ==
          prepared_aggregate.grouping_sets[0].key_term_ordinals;
  const bool exact_rollup_not_sum_profile =
      not_sum_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.rollup-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 3 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals.empty();
  const bool exact_rollup_metadata_not_sum_profile =
      not_sum_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.rollup-int64-keys-count-sum-grouping.v1" &&
      prepared_aggregate.grouping_sets.size() == 3 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals.empty();
  const bool exact_cube_not_sum_profile =
      not_sum_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.cube-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals.empty();
  const bool exact_cube_metadata_not_sum_profile =
      not_sum_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.cube-int64-keys-count-sum-grouping.v1" &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals.empty();
  const bool admitted_not_sum_profile =
      exact_ordinary_two_key_not_sum_profile ||
      exact_grouping_sets_not_sum_profile ||
      exact_grouping_sets_metadata_not_sum_profile ||
      exact_rollup_not_sum_profile || exact_rollup_metadata_not_sum_profile ||
      exact_cube_not_sum_profile || exact_cube_metadata_not_sum_profile;
  const bool admitted_or_profile =
      count_sum_or_profile &&
      (aggregate_root.semantic_variant_id ==
           "aggregate.grouped-int64-key-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.grouped-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.grouping-sets-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.rollup-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.rollup-int64-keys-count-sum-grouping.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.cube-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.cube-int64-keys-count-sum-grouping.v1");
  const bool exact_grouping_sets_or_profile =
      count_sum_or_profile &&
      (aggregate_root.semantic_variant_id ==
           "aggregate.grouping-sets-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1") &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals.empty() &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals ==
          prepared_aggregate.grouping_sets[0].key_term_ordinals;
  const bool exact_rollup_or_profile =
      count_sum_or_profile &&
      (aggregate_root.semantic_variant_id ==
           "aggregate.rollup-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.rollup-int64-keys-count-sum-grouping.v1") &&
      prepared_aggregate.grouping_sets.size() == 3 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals.empty();
  const bool exact_cube_or_profile =
      count_sum_or_profile &&
      (aggregate_root.semantic_variant_id ==
           "aggregate.cube-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.cube-int64-keys-count-sum-grouping.v1") &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals.empty();

  if (!prepared_aggregate.ok ||
      aggregate_root.output_descriptor_ids.size() != expected_output_count ||
      aggregate_root.bound_expression_ids.size() != expected_output_count ||
      (!simple_sum_profile && !not_not_sum_profile &&
       !not_not_count_profile && !not_not_count_sum_boolean_profile &&
       !not_sum_profile && !not_count_profile &&
       !count_sum_boolean_profile) ||
      (not_not_sum_profile &&
       !exact_ordinary_two_key_not_not_sum_profile) ||
      (not_not_count_profile &&
       !exact_ordinary_two_key_not_not_count_profile) ||
      (not_not_count_sum_and_profile &&
       !exact_ordinary_two_key_not_not_count_sum_and_profile) ||
      (not_not_count_sum_or_profile &&
       !exact_ordinary_two_key_not_not_count_sum_or_profile) ||
      (not_not_sum_count_or_profile &&
       !exact_ordinary_two_key_not_not_sum_count_or_profile) ||
      (not_count_profile && !exact_ordinary_two_key_not_count_profile) ||
      (not_count_sum_and_profile && !admitted_not_count_sum_and_profile) ||
      (not_count_sum_or_profile &&
       !exact_ordinary_two_key_not_count_sum_or_profile) ||
      (not_sum_profile && !admitted_not_sum_profile) ||
      (count_sum_or_profile && !admitted_or_profile) ||
      (count_sum_or_profile &&
       (aggregate_root.semantic_variant_id ==
            "aggregate.grouping-sets-int64-keys-count-sum.v1" ||
        aggregate_root.semantic_variant_id ==
            "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1") &&
       !exact_grouping_sets_or_profile) ||
      (count_sum_or_profile &&
       (aggregate_root.semantic_variant_id ==
            "aggregate.rollup-int64-keys-count-sum.v1" ||
        aggregate_root.semantic_variant_id ==
            "aggregate.rollup-int64-keys-count-sum-grouping.v1") &&
       !exact_rollup_or_profile) ||
      (count_sum_or_profile &&
       (aggregate_root.semantic_variant_id ==
            "aggregate.cube-int64-keys-count-sum.v1" ||
        aggregate_root.semantic_variant_id ==
            "aggregate.cube-int64-keys-count-sum-grouping.v1") &&
       !exact_cube_or_profile) ||
      (key_count == 2 && !count_sum_and_profile &&
       !exact_ordinary_two_key_not_not_sum_profile &&
       !exact_ordinary_two_key_not_not_count_profile &&
       !exact_ordinary_two_key_not_not_count_sum_and_profile &&
       !exact_ordinary_two_key_not_not_count_sum_or_profile &&
       !exact_ordinary_two_key_not_not_sum_count_or_profile &&
       !admitted_not_count_sum_and_profile &&
       !exact_ordinary_two_key_not_count_sum_or_profile &&
       !admitted_or_profile &&
       !admitted_two_key_sum_profile &&
       !admitted_not_sum_profile &&
       !exact_ordinary_two_key_not_count_profile) ||
      filter_root.input_logical_node_ids !=
          std::vector<std::uint32_t>{aggregate_root.logical_node_id} ||
      filter_root.output_descriptor_ids !=
          aggregate_root.output_descriptor_ids ||
      filter_root.bound_expression_ids.size() != 1 ||
      prepared_aggregate.key_terms.size() != key_count ||
      prepared_aggregate.result_bindings.size() != expected_output_count) {
    result.detail = "HAVING root does not preserve the grouped aggregate schema";
    return result;
  }

  const auto expression_by_id = [&](const std::uint32_t expression_id)
      -> const api::RelationalExpressionRecord* {
    const auto expression =
        std::ranges::find_if(dag.expressions, [&](const auto& candidate) {
          return candidate.expression_id == expression_id;
        });
    return expression == dag.expressions.end() ? nullptr : &*expression;
  };
  if (exact_ordinary_two_key_not_not_sum_profile ||
      exact_ordinary_two_key_not_not_count_profile ||
      exact_ordinary_two_key_not_not_count_sum_and_profile ||
      exact_ordinary_two_key_not_not_count_sum_or_profile ||
      exact_ordinary_two_key_not_not_sum_count_or_profile ||
      exact_ordinary_two_key_not_count_profile ||
      exact_ordinary_two_key_not_count_sum_or_profile) {
    std::unordered_map<std::uint32_t,
                       const api::RelationalExpressionRecord*>
        expressions_by_id;
    expressions_by_id.reserve(dag.expressions.size());
    for (const auto& expression : dag.expressions) {
      expressions_by_id.emplace(expression.expression_id, &expression);
    }

    std::vector<std::uint32_t> pending_expression_ids;
    for (const auto& row : dag.values_rows) {
      pending_expression_ids.insert(pending_expression_ids.end(),
                                    row.expression_ids.begin(),
                                    row.expression_ids.end());
    }
    for (const auto& node : dag.nodes) {
      pending_expression_ids.insert(pending_expression_ids.end(),
                                    node.bound_expression_ids.begin(),
                                    node.bound_expression_ids.end());
    }
    for (const auto& output : dag.outputs) {
      pending_expression_ids.push_back(output.expression_id);
    }
    for (const auto& grouping_set : dag.grouping_sets) {
      pending_expression_ids.insert(pending_expression_ids.end(),
                                    grouping_set.expression_ids.begin(),
                                    grouping_set.expression_ids.end());
    }
    for (const auto& property : dag.properties) {
      pending_expression_ids.insert(pending_expression_ids.end(),
                                    property.expression_ids.begin(),
                                    property.expression_ids.end());
      for (const auto& term : property.ordering_terms) {
        pending_expression_ids.push_back(term.expression_id);
      }
    }

    std::unordered_set<std::uint32_t> reachable_expression_ids;
    reachable_expression_ids.reserve(dag.expressions.size());
    while (!pending_expression_ids.empty()) {
      const auto expression_id = pending_expression_ids.back();
      pending_expression_ids.pop_back();
      if (!reachable_expression_ids.insert(expression_id).second) continue;
      const auto expression = expressions_by_id.find(expression_id);
      if (expression == expressions_by_id.end()) {
        result.detail =
            "bounded NOT profile contains an invalid expression owner";
        return result;
      }
      pending_expression_ids.insert(
          pending_expression_ids.end(),
          expression->second->child_expression_ids.begin(),
          expression->second->child_expression_ids.end());
    }
    if (expressions_by_id.size() != dag.expressions.size() ||
        reachable_expression_ids.size() != expressions_by_id.size()) {
      result.detail =
          "bounded NOT profile contains an unowned relational expression";
      return result;
    }
  }
  const auto descriptor_by_id = [&](const std::uint32_t descriptor_id)
      -> const api::RelationalTypeDescriptor* {
    const auto descriptor =
        std::ranges::find_if(dag.descriptors, [&](const auto& candidate) {
          return candidate.descriptor_id == descriptor_id;
        });
    return descriptor == dag.descriptors.end() ? nullptr : &*descriptor;
  };
  const auto isolated_predicate_descriptor =
      [&](const api::RelationalExpressionRecord* expression) {
        if (expression == nullptr) return false;
        const auto* descriptor =
            descriptor_by_id(expression->result_descriptor_id);
        return descriptor != nullptr &&
               descriptor->nullability ==
                   api::RelationalNullability::kNullable &&
               !descriptor->collation_uuid.has_value() &&
               !descriptor->timezone_profile_id.has_value() &&
               !descriptor->width.has_value() &&
               !descriptor->precision.has_value() &&
               !descriptor->scale.has_value() &&
               std::ranges::find(filter_root.output_descriptor_ids,
                                 expression->result_descriptor_id) ==
                   filter_root.output_descriptor_ids.end();
      };
  const auto exact_binary_operator =
      [&](const api::RelationalExpressionRecord* expression,
          const std::string_view operator_name) {
        return expression != nullptr &&
               expression->expression_kind ==
                   api::RelationalExpressionKind::kBinary &&
               expression->child_expression_ids.size() == 2 &&
               expression->operator_name == operator_name &&
               !expression->function_uuid.has_value() &&
               !expression->bound_name_uuid.has_value() &&
               !expression->literal_kind.has_value() &&
               !expression->literal_or_parameter_ref.has_value() &&
               isolated_predicate_descriptor(expression);
      };
  const auto exact_unary_operator =
      [&](const api::RelationalExpressionRecord* expression,
          const std::string_view operator_name) {
        return expression != nullptr &&
               expression->expression_kind ==
                   api::RelationalExpressionKind::kUnary &&
               expression->child_expression_ids.size() == 1 &&
               expression->operator_name == operator_name &&
               !expression->function_uuid.has_value() &&
               !expression->bound_name_uuid.has_value() &&
               !expression->literal_kind.has_value() &&
               !expression->literal_or_parameter_ref.has_value() &&
               isolated_predicate_descriptor(expression);
      };

  std::vector<const api::RelationalOutputRecord*> aggregate_outputs;
  std::vector<const api::RelationalOutputRecord*> filter_outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == aggregate_root.logical_node_id) {
      aggregate_outputs.push_back(&output);
    } else if (output.relation_node_id == filter_root.logical_node_id) {
      filter_outputs.push_back(&output);
    }
  }
  std::ranges::sort(aggregate_outputs, {},
                    &api::RelationalOutputRecord::ordinal);
  std::ranges::sort(filter_outputs, {},
                    &api::RelationalOutputRecord::ordinal);
  if (aggregate_outputs.size() != expected_output_count ||
      filter_outputs.size() != expected_output_count) {
    result.detail = "HAVING output lineage coverage is incomplete";
    return result;
  }
  if (exact_grouping_sets_metadata_not_count_sum_and_profile ||
      exact_rollup_metadata_not_count_sum_and_profile ||
      exact_cube_metadata_not_count_sum_and_profile ||
      exact_grouping_sets_metadata_not_sum_profile ||
      exact_rollup_metadata_not_sum_profile ||
      exact_cube_metadata_not_sum_profile) {
    constexpr std::array<std::string_view, 7> kOutputNames = {
        "key_a",      "key_b",     "row_count", "total_amount",
        "grouping_a", "grouping_b", "grouping_id"};
    for (std::size_t ordinal = 0; ordinal < kOutputNames.size(); ++ordinal) {
      if (aggregate_outputs[ordinal]->output_id != 4U + ordinal ||
          filter_outputs[ordinal]->output_id != 11U + ordinal ||
          aggregate_outputs[ordinal]->output_name_utf8 !=
              kOutputNames[ordinal] ||
          filter_outputs[ordinal]->output_name_utf8 !=
              kOutputNames[ordinal]) {
        result.detail =
            "metadata NOT-SUM output identity is not exact";
        return result;
      }
    }
  }
  if ((exact_ordinary_two_key_not_not_sum_profile ||
       exact_ordinary_two_key_not_not_count_profile ||
       exact_ordinary_two_key_not_not_count_sum_and_profile ||
       exact_ordinary_two_key_not_not_count_sum_or_profile ||
       exact_ordinary_two_key_not_not_sum_count_or_profile ||
       admitted_not_count_sum_and_profile ||
       exact_ordinary_two_key_not_count_profile ||
       exact_ordinary_two_key_not_count_sum_or_profile) &&
      !exact_grouping_sets_metadata_not_count_sum_and_profile &&
      !exact_rollup_metadata_not_count_sum_and_profile &&
      !exact_cube_metadata_not_count_sum_and_profile) {
    constexpr std::array<std::string_view, 4> kOutputNames = {
        "key_a", "key_b", "row_count", "total_amount"};
    for (std::size_t ordinal = 0; ordinal < kOutputNames.size(); ++ordinal) {
      if (aggregate_outputs[ordinal]->output_id != 4U + ordinal ||
          filter_outputs[ordinal]->output_id != 8U + ordinal ||
          aggregate_outputs[ordinal]->output_name_utf8 !=
              kOutputNames[ordinal] ||
          filter_outputs[ordinal]->output_name_utf8 !=
              kOutputNames[ordinal] ||
          !aggregate_outputs[ordinal]->visible ||
          !filter_outputs[ordinal]->visible) {
        result.detail =
            "bounded NOT predicate output identity is not exact";
        return result;
      }
    }
  }
  for (std::size_t ordinal = 0; ordinal < filter_outputs.size(); ++ordinal) {
    const auto& aggregate_output = *aggregate_outputs[ordinal];
    const auto& filter_output = *filter_outputs[ordinal];
    if (aggregate_output.ordinal != ordinal || filter_output.ordinal != ordinal ||
        aggregate_output.expression_id !=
            aggregate_root.bound_expression_ids[ordinal] ||
        filter_output.expression_id != aggregate_output.expression_id ||
        filter_output.descriptor_id != aggregate_output.descriptor_id ||
        filter_output.descriptor_id !=
            filter_root.output_descriptor_ids[ordinal] ||
        filter_output.visible != aggregate_output.visible ||
        filter_output.output_name_utf8 != aggregate_output.output_name_utf8 ||
        filter_output.output_name_utf8.empty()) {
      result.detail = "HAVING output lineage does not exactly mirror its aggregate";
      return result;
    }
  }

  const auto* predicate =
      expression_by_id(filter_root.bound_expression_ids.front());
  const api::RelationalExpressionRecord* inner_not = nullptr;
  const api::RelationalExpressionRecord* boolean_root = nullptr;
  const api::RelationalExpressionRecord* count_comparison = nullptr;
  const api::RelationalExpressionRecord* sum_comparison = nullptr;
  if (simple_sum_profile && exact_binary_operator(predicate, ">")) {
    sum_comparison = predicate;
  } else if ((not_not_sum_profile || not_not_count_profile ||
              not_not_count_sum_boolean_profile) &&
             exact_unary_operator(predicate, "NOT")) {
    inner_not =
        expression_by_id(predicate->child_expression_ids.front());
    if (exact_unary_operator(inner_not, "NOT")) {
      if (not_not_count_sum_boolean_profile) {
        boolean_root =
            expression_by_id(inner_not->child_expression_ids.front());
        if (exact_binary_operator(
                boolean_root,
                (not_not_count_sum_or_profile ||
                 not_not_sum_count_or_profile)
                    ? "OR"
                    : "AND")) {
          if (not_not_sum_count_or_profile) {
            sum_comparison =
                expression_by_id(boolean_root->child_expression_ids[0]);
            count_comparison =
                expression_by_id(boolean_root->child_expression_ids[1]);
          } else {
            count_comparison =
                expression_by_id(boolean_root->child_expression_ids[0]);
            sum_comparison =
                expression_by_id(boolean_root->child_expression_ids[1]);
          }
        }
      } else if (not_not_count_profile) {
        count_comparison =
            expression_by_id(inner_not->child_expression_ids.front());
      } else {
        sum_comparison =
            expression_by_id(inner_not->child_expression_ids.front());
      }
    }
  } else if ((not_sum_profile || not_count_profile ||
              not_count_sum_boolean_profile) &&
             exact_unary_operator(predicate, "NOT")) {
    const auto* operand =
        expression_by_id(predicate->child_expression_ids.front());
    if (not_count_sum_boolean_profile &&
        exact_binary_operator(
            operand, not_count_sum_or_profile ? "OR" : "AND")) {
      boolean_root = operand;
      count_comparison =
          expression_by_id(operand->child_expression_ids[0]);
      sum_comparison =
          expression_by_id(operand->child_expression_ids[1]);
    } else if (not_count_profile) {
      count_comparison = operand;
    } else {
      sum_comparison = operand;
    }
  } else if (count_sum_boolean_profile &&
             exact_binary_operator(
                 predicate,
                 (count_sum_or_profile || not_count_sum_or_profile)
                     ? "OR"
                     : "AND")) {
    boolean_root = predicate;
    count_comparison = expression_by_id(predicate->child_expression_ids[0]);
    sum_comparison = expression_by_id(predicate->child_expression_ids[1]);
  }
  if ((!not_count_profile && !not_not_count_profile &&
       !exact_binary_operator(sum_comparison, ">")) ||
      (not_count_profile &&
       (!exact_binary_operator(count_comparison, ">") ||
        predicate->child_expression_ids !=
            std::vector<std::uint32_t>{count_comparison->expression_id} ||
        predicate->result_descriptor_id !=
            count_comparison->result_descriptor_id)) ||
      (not_sum_profile &&
       predicate->result_descriptor_id !=
           sum_comparison->result_descriptor_id) ||
      (not_not_sum_profile &&
       (inner_not == nullptr ||
        predicate->child_expression_ids !=
            std::vector<std::uint32_t>{inner_not->expression_id} ||
        inner_not->child_expression_ids !=
            std::vector<std::uint32_t>{sum_comparison->expression_id} ||
        predicate->result_descriptor_id !=
            inner_not->result_descriptor_id ||
        inner_not->result_descriptor_id !=
            sum_comparison->result_descriptor_id)) ||
      (not_not_count_profile &&
       (inner_not == nullptr ||
        !exact_binary_operator(count_comparison, ">") ||
        predicate->child_expression_ids !=
            std::vector<std::uint32_t>{inner_not->expression_id} ||
        inner_not->child_expression_ids !=
            std::vector<std::uint32_t>{count_comparison->expression_id} ||
        predicate->result_descriptor_id !=
            inner_not->result_descriptor_id ||
        inner_not->result_descriptor_id !=
            count_comparison->result_descriptor_id)) ||
      (not_not_count_sum_boolean_profile &&
       (inner_not == nullptr || boolean_root == nullptr ||
        predicate->child_expression_ids !=
            std::vector<std::uint32_t>{inner_not->expression_id} ||
        inner_not->child_expression_ids !=
            std::vector<std::uint32_t>{boolean_root->expression_id} ||
        predicate->result_descriptor_id !=
            inner_not->result_descriptor_id ||
        inner_not->result_descriptor_id !=
            boolean_root->result_descriptor_id)) ||
      (count_sum_boolean_profile &&
       (boolean_root == nullptr ||
        !exact_binary_operator(count_comparison, ">") ||
        boolean_root->result_descriptor_id !=
            count_comparison->result_descriptor_id ||
        boolean_root->result_descriptor_id !=
            sum_comparison->result_descriptor_id ||
        (!not_count_sum_boolean_profile &&
         !not_not_count_sum_boolean_profile && boolean_root != predicate) ||
        (not_count_sum_boolean_profile &&
         (predicate->child_expression_ids !=
              std::vector<std::uint32_t>{boolean_root->expression_id} ||
          predicate->result_descriptor_id !=
              boolean_root->result_descriptor_id)) ||
        (not_not_count_sum_boolean_profile &&
         inner_not->result_descriptor_id !=
             boolean_root->result_descriptor_id)))) {
    result.detail =
        "HAVING predicate is not the exact admitted comparison or ordered Boolean profile";
    return result;
  }

  const api::RelationalExpressionRecord* having_sum = nullptr;
  const api::RelationalExpressionRecord* sum_threshold = nullptr;
  if (!not_count_profile && !not_not_count_profile) {
    having_sum = expression_by_id(sum_comparison->child_expression_ids[0]);
    sum_threshold = expression_by_id(sum_comparison->child_expression_ids[1]);
    const auto* projected_sum =
        expression_by_id(aggregate_root.bound_expression_ids[key_count + 1]);
    if (having_sum == nullptr || sum_threshold == nullptr ||
        projected_sum == nullptr ||
        having_sum->expression_kind !=
            api::RelationalExpressionKind::kFunctionCall ||
        having_sum->function_uuid != sum_registry_entry->function_uuid ||
        having_sum->child_expression_ids.size() != 1 ||
        having_sum->result_descriptor_id !=
            aggregate_root.output_descriptor_ids[key_count + 1] ||
        having_sum->bound_name_uuid.has_value() ||
        having_sum->literal_kind.has_value() ||
        having_sum->operator_name.has_value() ||
        having_sum->literal_or_parameter_ref.has_value() ||
        projected_sum->expression_kind !=
            api::RelationalExpressionKind::kFunctionCall ||
        projected_sum->function_uuid != having_sum->function_uuid ||
        projected_sum->child_expression_ids.size() != 1 ||
        projected_sum->result_descriptor_id !=
            having_sum->result_descriptor_id) {
      result.detail =
          "HAVING SUM identity does not match the projected aggregate state";
      return result;
    }

    const auto* having_argument =
        expression_by_id(having_sum->child_expression_ids.front());
    const auto* projected_argument =
        expression_by_id(projected_sum->child_expression_ids.front());
    if (having_argument == nullptr || projected_argument == nullptr ||
        having_argument->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        projected_argument->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        having_argument->result_descriptor_id !=
            projected_argument->result_descriptor_id ||
        having_argument->bound_name_uuid !=
            projected_argument->bound_name_uuid ||
        !having_argument->bound_name_uuid.has_value() ||
        std::ranges::find(values_node.output_descriptor_ids,
                          having_argument->result_descriptor_id) ==
            values_node.output_descriptor_ids.end()) {
      result.detail =
          "HAVING SUM argument does not bind the aggregate input column";
      return result;
    }
  }

  const auto prepare_threshold =
      [&](const api::RelationalExpressionRecord* threshold,
          const std::string_view label) {
        if (threshold == nullptr) return false;
        const auto* descriptor =
            descriptor_by_id(threshold->result_descriptor_id);
        if (threshold->expression_kind !=
                api::RelationalExpressionKind::kLiteral ||
            threshold->literal_kind !=
                api::RelationalLiteralKind::kNumeric ||
            !threshold->child_expression_ids.empty() ||
            threshold->function_uuid.has_value() ||
            threshold->bound_name_uuid.has_value() ||
            threshold->operator_name.has_value() ||
            !threshold->literal_or_parameter_ref.has_value() ||
            descriptor == nullptr ||
            descriptor->nullability !=
                api::RelationalNullability::kNonNull ||
            descriptor->collation_uuid.has_value() ||
            descriptor->timezone_profile_id.has_value() ||
            descriptor->width.has_value() || descriptor->precision.has_value() ||
            descriptor->scale.has_value() ||
            std::ranges::find(filter_root.output_descriptor_ids,
                              threshold->result_descriptor_id) !=
                filter_root.output_descriptor_ids.end()) {
          result.detail = std::string("HAVING ") + std::string(label) +
                          " threshold is not one standalone non-NULL numeric literal";
          return false;
        }
        CanonicalRelationalExpressionRuntime expression_runtime(dag);
        api::EngineTypedValue value;
        if (!expression_runtime.EvaluateForConsumer(
                threshold->expression_id, "int64",
                api::EngineCanonicalExpressionConsumer::aggregate, &value,
                &result.detail) ||
            value.state != api::EngineValueState::value || value.is_null ||
            value.descriptor.canonical_type_name != "int64") {
          if (result.detail.empty()) {
            result.detail = std::string("HAVING ") + std::string(label) +
                            " threshold is outside canonical int64";
          }
          return false;
        }
        std::int64_t exact = 0;
        const auto [end, error] = std::from_chars(
            value.encoded_value.data(),
            value.encoded_value.data() + value.encoded_value.size(), exact);
        if (error != std::errc{} ||
            end != value.encoded_value.data() + value.encoded_value.size()) {
          result.detail = std::string("HAVING ") + std::string(label) +
                          " threshold is outside exact int64 admission";
          return false;
        }
        return true;
      };
  if (!not_count_profile && !not_not_count_profile &&
      !prepare_threshold(sum_threshold, "SUM")) {
    return result;
  }

  const api::RelationalExpressionRecord* having_count = nullptr;
  if (count_sum_boolean_profile || not_count_profile ||
      not_not_count_profile) {
    having_count =
        expression_by_id(count_comparison->child_expression_ids[0]);
    const auto* count_threshold =
        expression_by_id(count_comparison->child_expression_ids[1]);
    const auto* projected_count =
        expression_by_id(aggregate_root.bound_expression_ids[key_count]);
    if (having_count == nullptr || projected_count == nullptr ||
        having_count->expression_kind !=
            api::RelationalExpressionKind::kFunctionCall ||
        having_count->function_uuid != count_registry_entry->function_uuid ||
        !having_count->child_expression_ids.empty() ||
        having_count->result_descriptor_id !=
            aggregate_root.output_descriptor_ids[key_count] ||
        having_count->bound_name_uuid.has_value() ||
        having_count->literal_kind.has_value() ||
        having_count->operator_name.has_value() ||
        having_count->literal_or_parameter_ref.has_value() ||
        projected_count->expression_kind !=
            api::RelationalExpressionKind::kFunctionCall ||
        projected_count->function_uuid != having_count->function_uuid ||
        !projected_count->child_expression_ids.empty() ||
        projected_count->result_descriptor_id !=
            having_count->result_descriptor_id) {
      result.detail =
          "HAVING COUNT identity does not match the projected aggregate state";
      return result;
    }
    if (!prepare_threshold(count_threshold, "COUNT")) {
      return result;
    }
  }
  result.predicate_expression_id = predicate->expression_id;
  result.row_binding.row_descriptor_ids =
      filter_root.output_descriptor_ids;
  if (having_sum != nullptr) {
    result.row_binding.slots.push_back(
        {having_sum->expression_id, having_sum->result_descriptor_id,
         key_count + 1,
         CanonicalRelationalExpressionRowSlotKind::materialized_function});
  }
  if (having_count != nullptr) {
    result.row_binding.slots.push_back(
        {having_count->expression_id, having_count->result_descriptor_id,
         key_count,
         CanonicalRelationalExpressionRowSlotKind::materialized_function});
  }
  result.output_column_count = expected_output_count;
  result.ok = true;
  return result;
}

}  // namespace

LiveGroupedCountSumProfile MatchLiveGroupedCountSumProfileForComposition(
    const std::string_view semantic_variant_id) {
  return MatchLiveGroupedCountSumProfile(semantic_variant_id);
}

bool IsLiveGroupedHavingProfileForComposition(
    const std::string_view semantic_variant_id) {
  return IsLiveGroupedHavingProfile(semantic_variant_id);
}

PreparedGroupedCountSumRoot PrepareGroupedCountSumRootForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    const LiveGroupedCountSumProfile& profile) {
  return PrepareGroupedCountSumRoot(dag, root, input_node, input, profile);
}

PreparedGroupedHavingRoot PrepareGroupedHavingRootForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& aggregate_root,
    const plan::CanonicalLogicalRelationalNode& values_node,
    const PreparedGroupedCountSumRoot& aggregate) {
  return PrepareGroupedHavingRoot(
      dag, root, aggregate_root, values_node, aggregate);
}

}  // namespace scratchbird::engine::sblr
