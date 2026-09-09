// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_correlated_registration.hpp"
#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_node_composition.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_predicate_support.hpp"
#include "canonical_query_projection_registration.hpp"
#include "canonical_query_recursive_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_query_set_registration.hpp"
#include "canonical_query_sort_registration.hpp"
#include "canonical_relational_expression.hpp"
#include "catalog/name_resolution_api.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
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

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_SUBQUERY_PREPARATION_AUTHORITY
// Predicate-subquery, LATERAL/APPLY, and recursive profile matching plus
// bounded recursive cardinality preparation over supplied typed profiles.
// Owns no plan selection, physical DAG dispatch, relation-store access,
// snapshot construction, transaction finality, or public route selection.

namespace {

LivePredicateSubqueryProfile MatchLivePredicateSubqueryProfile(
    const std::string_view semantic_variant_id) {
  LivePredicateSubqueryProfile profile;
  if (semantic_variant_id == "subquery.exists.v1") {
    profile.matched = true;
    profile.kind = LivePredicateSubqueryKind::kExists;
    profile.implementation_id = "subquery.exists.typed.v1";
    profile.transformation_id = "canonical.subquery.composed-exists.v1";
    return profile;
  }

  struct QuantifiedName {
    std::string_view suffix;
    api::EngineComparisonPredicateOperator operation;
  };
  constexpr std::array<QuantifiedName, 6> kNames{{
      {"eq", api::EngineComparisonPredicateOperator::equal},
      {"ne", api::EngineComparisonPredicateOperator::not_equal},
      {"lt", api::EngineComparisonPredicateOperator::less_than},
      {"le", api::EngineComparisonPredicateOperator::less_than_or_equal},
      {"gt", api::EngineComparisonPredicateOperator::greater_than},
      {"ge", api::EngineComparisonPredicateOperator::greater_than_or_equal},
  }};
  for (const auto& name : kNames) {
    for (const auto& type_profile :
         std::array<std::pair<std::string_view, std::string_view>, 2>{{
             {"int64", "int64"},
             {"typed", ""},
         }}) {
      const auto any = "subquery.quantified-any-" +
                       std::string(name.suffix) + "-" +
                       std::string(type_profile.first) + ".v1";
      const auto all = "subquery.quantified-all-" +
                       std::string(name.suffix) + "-" +
                       std::string(type_profile.first) + ".v1";
      if (semantic_variant_id != any && semantic_variant_id != all) continue;
      profile.matched = true;
      profile.kind = LivePredicateSubqueryKind::kQuantified;
      profile.comparison_operator = name.operation;
      profile.quantifier =
          semantic_variant_id == any
              ? exec::CanonicalQuantifiedSubqueryQuantifier::kAny
              : exec::CanonicalQuantifiedSubqueryQuantifier::kAll;
      profile.required_operand_type = type_profile.second;
      profile.implementation_id =
          type_profile.second.empty()
              ? "subquery.quantified.typed.v1"
              : "subquery.quantified.int64.typed.v1";
      profile.transformation_id =
          "canonical." + std::string(semantic_variant_id);
      return profile;
    }
  }
  if (semantic_variant_id == "subquery.in-int64.v1" ||
      semantic_variant_id == "subquery.in-typed.v1") {
    profile.matched = true;
    profile.kind = LivePredicateSubqueryKind::kQuantified;
    profile.comparison_operator =
        api::EngineComparisonPredicateOperator::equal;
    profile.quantifier =
        exec::CanonicalQuantifiedSubqueryQuantifier::kAny;
    profile.required_operand_type =
        semantic_variant_id == "subquery.in-int64.v1" ? "int64" : "";
    profile.implementation_id =
        profile.required_operand_type.empty()
            ? "subquery.quantified.typed.v1"
            : "subquery.quantified.int64.typed.v1";
    profile.transformation_id =
        semantic_variant_id == "subquery.in-int64.v1"
            ? "canonical.subquery.composed-in-int64.v1"
            : "canonical.subquery.composed-in-typed.v1";
  }
  return profile;
}

using PreparedCorrelatedSubqueryRoot =
    LiveCorrelatedSubqueryRegistrationProfile;

LiveLateralSubqueryProfile MatchLiveLateralSubqueryProfile(
    const std::string_view semantic_variant_id) {
  LiveLateralSubqueryProfile profile;
  struct Name {
    std::string_view semantic_variant_id;
    exec::CanonicalLateralJoinForm form;
    std::string_view required_operand_type;
    std::string_view implementation_id;
  };
  constexpr std::array<Name, 8> kNames{{
      {"join.lateral-inner-int64-equality.v1",
       exec::CanonicalLateralJoinForm::kInnerLateral, "int64",
       "join.lateral-inner.correlated.typed.v1"},
      {"join.lateral-inner-typed-equality.v1",
       exec::CanonicalLateralJoinForm::kInnerLateral, "",
       "join.lateral-inner.correlated.typed.v1"},
      {"join.lateral-left-int64-equality.v1",
       exec::CanonicalLateralJoinForm::kLeftLateral, "int64",
       "join.lateral-left.correlated.typed.v1"},
      {"join.lateral-left-typed-equality.v1",
       exec::CanonicalLateralJoinForm::kLeftLateral, "",
       "join.lateral-left.correlated.typed.v1"},
      {"join.cross-apply-int64-equality.v1",
       exec::CanonicalLateralJoinForm::kCrossApply, "int64",
       "join.cross-apply.correlated.typed.v1"},
      {"join.cross-apply-typed-equality.v1",
       exec::CanonicalLateralJoinForm::kCrossApply, "",
       "join.cross-apply.correlated.typed.v1"},
      {"join.outer-apply-int64-equality.v1",
       exec::CanonicalLateralJoinForm::kOuterApply, "int64",
       "join.outer-apply.correlated.typed.v1"},
      {"join.outer-apply-typed-equality.v1",
       exec::CanonicalLateralJoinForm::kOuterApply, "",
       "join.outer-apply.correlated.typed.v1"},
  }};
  for (const auto& name : kNames) {
    if (semantic_variant_id != name.semantic_variant_id) continue;
    profile.matched = true;
    profile.form = name.form;
    profile.required_operand_type = name.required_operand_type;
    profile.implementation_id = name.implementation_id;
    profile.transformation_id =
        "canonical." + std::string(semantic_variant_id);
    break;
  }
  return profile;
}

LiveRecursiveCteProfile MatchLiveRecursiveCteProfile(
    const std::string_view semantic_variant_id) {
  LiveRecursiveCteProfile profile;
  if (semantic_variant_id ==
      "cte.recursive-union-all-int64-increment.v1") {
    profile.matched = true;
    profile.union_mode = exec::CanonicalRecursiveCteUnionMode::kAll;
    profile.implementation_id = "cte.recursive.union-all.typed.v1";
  } else if (semantic_variant_id ==
             "cte.recursive-union-distinct-int64-increment.v1") {
    profile.matched = true;
    profile.union_mode = exec::CanonicalRecursiveCteUnionMode::kDistinct;
    profile.emit_current_duplicate = true;
    profile.implementation_id =
        "cte.recursive.union-distinct-int64.typed.v1";
  } else if (semantic_variant_id ==
             "cte.recursive-search-breadth-cycle-int64-increment.v1") {
    profile.matched = true;
    profile.search_cycle = true;
    profile.implementation_id =
        "cte.recursive.search-breadth-cycle-int64.typed.v1";
  }
  if (profile.matched) {
    profile.transformation_id =
        "canonical." + std::string(semantic_variant_id);
  }
  return profile;
}

// Runtime executor-registration and producer-memory evidence.  Several legacy
// fields remain populated by specialized executors, but the optimizer consumes
bool BindPreparedRecursiveCteCardinalityImpl(
    PreparedRecursiveCteRoot* prepared,
    const std::vector<LivePhysicalNodeProfile>& profiles,
    const std::uint32_t anchor_logical_node_id,
    const std::uint64_t recursive_upper_bound,
    const std::size_t execution_row_ceiling) {
  if (prepared == nullptr || execution_row_ceiling == 0 ||
      recursive_upper_bound >= execution_row_ceiling ||
      recursive_upper_bound >=
          std::numeric_limits<std::size_t>::max() ||
      prepared->profile.search_cycle ||
      prepared->profile.emit_current_duplicate ||
      prepared->profile.union_mode !=
          exec::CanonicalRecursiveCteUnionMode::kAll) {
    return false;
  }
  const LivePhysicalNodeProfile* anchor_profile = nullptr;
  for (const auto& profile : profiles) {
    if (profile.logical_node_id != anchor_logical_node_id) continue;
    if (anchor_profile != nullptr) return false;
    anchor_profile = &profile;
  }
  if (anchor_profile == nullptr ||
      anchor_profile->physical_node_kind !=
          exec::PhysicalNodeKind::kAggregate ||
      anchor_profile->implementation_id != "aggregate.count-star.v1") {
    return false;
  }
  // estimated_rows is a cost input, not an execution limit. The five model
  // compositions currently admit recursion only over the canonical global
  // COUNT(*) producer, whose hard result cardinality is exactly one row.
  // Future multi-row anchors require a separate hard cardinality carrier.
  constexpr std::size_t anchor_row_bound = 1;
  const auto result_row_bound =
      static_cast<std::size_t>(recursive_upper_bound + 1);
  constexpr std::size_t term_row_multiplier = 1;
  std::uint64_t maximum_term_output_rows = 0;
  std::uint64_t rows_examined = 0;
  if (!CheckedMultiply(anchor_row_bound, term_row_multiplier,
                       &maximum_term_output_rows) ||
      !CheckedMultiply(result_row_bound, std::uint64_t{2},
                       &rows_examined) ||
      maximum_term_output_rows >
          std::numeric_limits<std::size_t>::max() ||
      rows_examined > std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  // COUNT(*) is nonnegative. The accepted UNION ALL increment therefore
  // emits at most upper_bound + 1 total rows, including its anchor; a count
  // already above the bound emits only that anchor.
  prepared->maximum_anchor_row_count = anchor_row_bound;
  prepared->maximum_iteration_count = result_row_bound;
  prepared->maximum_working_row_count = anchor_row_bound;
  prepared->maximum_term_output_row_count =
      static_cast<std::size_t>(maximum_term_output_rows);
  prepared->maximum_result_row_count = result_row_bound;
  prepared->maximum_value_comparison_count = 1;
  prepared->rows_examined = static_cast<std::size_t>(rows_examined);
  return true;
}

}  // namespace

LivePredicateSubqueryProfile
MatchLivePredicateSubqueryProfileForComposition(
    const std::string_view semantic_variant_id) {
  return MatchLivePredicateSubqueryProfile(semantic_variant_id);
}

LiveLateralSubqueryProfile MatchLiveLateralSubqueryProfileForComposition(
    const std::string_view semantic_variant_id) {
  return MatchLiveLateralSubqueryProfile(semantic_variant_id);
}

LiveRecursiveCteProfile MatchLiveRecursiveCteProfileForComposition(
    const std::string_view semantic_variant_id) {
  return MatchLiveRecursiveCteProfile(semantic_variant_id);
}

bool BindPreparedRecursiveCteCardinality(
    PreparedRecursiveCteRoot* prepared,
    const std::vector<LivePhysicalNodeProfile>& profiles,
    const std::uint32_t anchor_logical_node_id,
    const std::uint64_t recursive_upper_bound,
    const std::size_t execution_row_ceiling) {
  return BindPreparedRecursiveCteCardinalityImpl(
      prepared, profiles, anchor_logical_node_id, recursive_upper_bound,
      execution_row_ceiling);
}

}  // namespace scratchbird::engine::sblr
