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
#include "hash_digest.hpp"
#include "sblr_literal_runtime.hpp"

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

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_GLOBAL_AGGREGATE_PREPARATION_AUTHORITY
// Prepares typed aggregate profiles, descriptors, and bounded workspaces.
// Owns no plan selection, executor dispatch, storage access, MGA snapshot
// construction, transaction finality, or public route selection.

namespace {

LiveUnaryAggregateExpressionProfile MatchLiveUnaryAggregateExpressionProfile(
    const std::string_view semantic_variant_id) {
  LiveUnaryAggregateExpressionProfile result;
  if (semantic_variant_id == "aggregate.global-count-star.v1") {
    result.matched = true;
    result.count_star = true;
    result.function = exec::CanonicalAggregateFunction::count;
    result.transformation_id =
        "canonical.aggregate.global-count-star.v1";
    return result;
  }

  struct FunctionProfile {
    std::string_view stem;
    exec::CanonicalAggregateFunction function;
  };
  static constexpr std::array<FunctionProfile, 43> kFunctionProfiles = {{
      {"count", exec::CanonicalAggregateFunction::count},
      {"sum", exec::CanonicalAggregateFunction::sum},
      {"avg", exec::CanonicalAggregateFunction::avg},
      {"min", exec::CanonicalAggregateFunction::min},
      {"max", exec::CanonicalAggregateFunction::max},
      {"bool-and", exec::CanonicalAggregateFunction::bool_and},
      {"bool-or", exec::CanonicalAggregateFunction::bool_or},
      {"every", exec::CanonicalAggregateFunction::every},
      {"stddev-pop", exec::CanonicalAggregateFunction::stddev_pop},
      {"variance-pop", exec::CanonicalAggregateFunction::variance_pop},
      {"stddev", exec::CanonicalAggregateFunction::stddev},
      {"variance", exec::CanonicalAggregateFunction::variance},
      {"stddev-samp", exec::CanonicalAggregateFunction::stddev_samp},
      {"variance-samp", exec::CanonicalAggregateFunction::variance_samp},
      {"corr", exec::CanonicalAggregateFunction::corr},
      {"covar-pop", exec::CanonicalAggregateFunction::covar_pop},
      {"covar-samp", exec::CanonicalAggregateFunction::covar_samp},
      {"regr-count", exec::CanonicalAggregateFunction::regr_count},
      {"regr-avgx", exec::CanonicalAggregateFunction::regr_avgx},
      {"regr-avgy", exec::CanonicalAggregateFunction::regr_avgy},
      {"regr-intercept", exec::CanonicalAggregateFunction::regr_intercept},
      {"regr-r2", exec::CanonicalAggregateFunction::regr_r2},
      {"regr-slope", exec::CanonicalAggregateFunction::regr_slope},
      {"regr-sxx", exec::CanonicalAggregateFunction::regr_sxx},
      {"regr-sxy", exec::CanonicalAggregateFunction::regr_sxy},
      {"regr-syy", exec::CanonicalAggregateFunction::regr_syy},
      {"approx-count-distinct",
       exec::CanonicalAggregateFunction::approx_count_distinct},
      {"approx-median", exec::CanonicalAggregateFunction::approx_median},
      {"string-agg", exec::CanonicalAggregateFunction::string_agg},
      {"listagg-ordered", exec::CanonicalAggregateFunction::listagg},
      {"mode-ordered", exec::CanonicalAggregateFunction::mode},
      {"percentile-cont-ordered",
       exec::CanonicalAggregateFunction::percentile_cont},
      {"percentile-disc-ordered",
       exec::CanonicalAggregateFunction::percentile_disc},
      {"rank-hypothetical", exec::CanonicalAggregateFunction::rank},
      {"dense-rank-hypothetical",
       exec::CanonicalAggregateFunction::dense_rank},
      {"percent-rank-hypothetical",
       exec::CanonicalAggregateFunction::percent_rank},
      {"cume-dist-hypothetical",
       exec::CanonicalAggregateFunction::cume_dist},
      {"approx-percentile-cont-ordered",
       exec::CanonicalAggregateFunction::approx_percentile_cont},
      {"approx-percentile-disc-ordered",
       exec::CanonicalAggregateFunction::approx_percentile_disc},
      {"array-agg-ordered", exec::CanonicalAggregateFunction::array_agg},
      {"json-agg-ordered", exec::CanonicalAggregateFunction::json_agg},
      {"json-object-agg-ordered",
       exec::CanonicalAggregateFunction::json_object_agg},
      {"approx-top-k", exec::CanonicalAggregateFunction::approx_top_k},
  }};

  for (const auto& profile : kFunctionProfiles) {
    const std::string prefix =
        "aggregate.global-" + std::string(profile.stem);
    if (semantic_variant_id == prefix + "-expression.v1") {
      result.matched = true;
    } else if (semantic_variant_id == prefix + "-filter-expression.v1") {
      result.matched = true;
      result.has_filter = true;
    } else if (semantic_variant_id ==
               prefix + "-distinct-expression.v1") {
      result.matched = true;
      result.distinct = true;
    } else if (semantic_variant_id ==
               prefix + "-distinct-filter-expression.v1") {
      result.matched = true;
      result.distinct = true;
      result.has_filter = true;
    }
    if (!result.matched) continue;
    result.function = profile.function;
    result.transformation_id =
        "canonical." + std::string(semantic_variant_id);
    return result;
  }
  return result;
}

LivePairStatisticalExpressionProfile MatchLivePairStatisticalExpressionProfile(
    const std::string_view semantic_variant_id) {
  LivePairStatisticalExpressionProfile result;
  struct FunctionProfile {
    std::string_view stem;
    exec::CanonicalAggregateFunction function;
  };
  static constexpr std::array<FunctionProfile, 12> kFunctionProfiles = {{
      {"corr", exec::CanonicalAggregateFunction::corr},
      {"covar-pop", exec::CanonicalAggregateFunction::covar_pop},
      {"covar-samp", exec::CanonicalAggregateFunction::covar_samp},
      {"regr-count", exec::CanonicalAggregateFunction::regr_count},
      {"regr-avgx", exec::CanonicalAggregateFunction::regr_avgx},
      {"regr-avgy", exec::CanonicalAggregateFunction::regr_avgy},
      {"regr-intercept", exec::CanonicalAggregateFunction::regr_intercept},
      {"regr-r2", exec::CanonicalAggregateFunction::regr_r2},
      {"regr-slope", exec::CanonicalAggregateFunction::regr_slope},
      {"regr-sxx", exec::CanonicalAggregateFunction::regr_sxx},
      {"regr-sxy", exec::CanonicalAggregateFunction::regr_sxy},
      {"regr-syy", exec::CanonicalAggregateFunction::regr_syy},
  }};

  for (const auto& profile : kFunctionProfiles) {
    const std::string prefix =
        "aggregate.global-" + std::string(profile.stem);
    if (semantic_variant_id == prefix + "-expression.v1") {
      result.matched = true;
    } else if (semantic_variant_id == prefix + "-filter-expression.v1") {
      result.matched = true;
      result.has_filter = true;
    } else if (semantic_variant_id ==
               prefix + "-distinct-expression.v1") {
      result.matched = true;
      result.distinct = true;
    } else if (semantic_variant_id ==
               prefix + "-distinct-filter-expression.v1") {
      result.matched = true;
      result.distinct = true;
      result.has_filter = true;
    }
    if (!result.matched) continue;
    result.function = profile.function;
    result.transformation_id =
        "canonical." + std::string(semantic_variant_id);
    return result;
  }
  return result;
}

LiveStringAggregateExpressionProfile MatchLiveStringAggregateExpressionProfile(
    const std::string_view semantic_variant_id) {
  LiveStringAggregateExpressionProfile result;
  struct OrderProfile {
    std::string_view prefix;
    bool ordered;
  };
  static constexpr std::array<OrderProfile, 2> kOrderProfiles = {{
      {"aggregate.global-string-agg", false},
      {"aggregate.global-string-agg-ordered", true},
  }};

  for (const auto& profile : kOrderProfiles) {
    if (semantic_variant_id ==
        std::string(profile.prefix) + "-expression.v1") {
      result.matched = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-filter-expression.v1") {
      result.matched = true;
      result.has_filter = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-distinct-expression.v1") {
      result.matched = true;
      result.distinct = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) +
                   "-distinct-filter-expression.v1") {
      result.matched = true;
      result.distinct = true;
      result.has_filter = true;
    }
    if (!result.matched) continue;
    result.ordered = profile.ordered;
    result.transformation_id =
        "canonical." + std::string(semantic_variant_id);
    return result;
  }
  return result;
}

LiveOrderedSingleCollectionExpressionProfile
MatchLiveOrderedSingleCollectionExpressionProfile(
    const std::string_view semantic_variant_id) {
  LiveOrderedSingleCollectionExpressionProfile result;
  struct FunctionProfile {
    std::string_view prefix;
    exec::CanonicalAggregateFunction function;
  };
  static constexpr std::array<FunctionProfile, 2> kFunctionProfiles = {{
      {"aggregate.global-array-agg-ordered",
       exec::CanonicalAggregateFunction::array_agg},
      {"aggregate.global-json-agg-ordered",
       exec::CanonicalAggregateFunction::json_agg},
  }};

  for (const auto& profile : kFunctionProfiles) {
    if (semantic_variant_id ==
        std::string(profile.prefix) + "-expression.v1") {
      result.matched = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-filter-expression.v1") {
      result.matched = true;
      result.has_filter = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-distinct-expression.v1") {
      result.matched = true;
      result.distinct = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) +
                   "-distinct-filter-expression.v1") {
      result.matched = true;
      result.distinct = true;
      result.has_filter = true;
    }
    if (!result.matched) continue;
    result.function = profile.function;
    result.transformation_id =
        "canonical." + std::string(semantic_variant_id);
    return result;
  }
  return result;
}

LiveJsonObjectAggregateExpressionProfile
MatchLiveJsonObjectAggregateExpressionProfile(
    const std::string_view semantic_variant_id) {
  LiveJsonObjectAggregateExpressionProfile result;
  constexpr std::string_view kPrefix =
      "aggregate.global-json-object-agg-ordered";
  if (semantic_variant_id == std::string(kPrefix) + "-expression.v1") {
    result.matched = true;
  } else if (semantic_variant_id ==
             std::string(kPrefix) + "-filter-expression.v1") {
    result.matched = true;
    result.has_filter = true;
  } else if (semantic_variant_id ==
             std::string(kPrefix) + "-distinct-expression.v1") {
    result.matched = true;
    result.distinct = true;
  } else if (semantic_variant_id ==
             std::string(kPrefix) +
                 "-distinct-filter-expression.v1") {
    result.matched = true;
    result.distinct = true;
    result.has_filter = true;
  }
  if (result.matched) {
    result.transformation_id =
        "canonical." + std::string(semantic_variant_id);
  }
  return result;
}

LiveListaggExpressionProfile MatchLiveListaggExpressionProfile(
    const std::string_view semantic_variant_id) {
  LiveListaggExpressionProfile result;
  struct FormProfile {
    std::string_view prefix;
    std::size_t base_argument_count;
    exec::CanonicalListaggOverflowMode overflow_mode;
  };
  static constexpr std::array<FormProfile, 3> kFormProfiles = {{
      {"aggregate.global-listagg-ordered", 3,
       exec::CanonicalListaggOverflowMode::none},
      {"aggregate.global-listagg-ordered-overflow-error", 4,
       exec::CanonicalListaggOverflowMode::error},
      {"aggregate.global-listagg-ordered-overflow-truncate", 6,
       exec::CanonicalListaggOverflowMode::truncate},
  }};

  for (const auto& profile : kFormProfiles) {
    if (semantic_variant_id ==
        std::string(profile.prefix) + "-expression.v1") {
      result.matched = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-filter-expression.v1") {
      result.matched = true;
      result.has_filter = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-distinct-expression.v1") {
      result.matched = true;
      result.distinct = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) +
                   "-distinct-filter-expression.v1") {
      result.matched = true;
      result.distinct = true;
      result.has_filter = true;
    }
    if (!result.matched) continue;
    result.base_argument_count = profile.base_argument_count;
    result.overflow_mode = profile.overflow_mode;
    result.transformation_id =
        "canonical." + std::string(semantic_variant_id);
    return result;
  }
  return result;
}

LiveOrderedSetExpressionProfile MatchLiveOrderedSetExpressionProfile(
    const std::string_view semantic_variant_id) {
  LiveOrderedSetExpressionProfile result;
  struct FunctionProfile {
    std::string_view prefix;
    exec::CanonicalAggregateFunction function;
  };
  static constexpr std::array<FunctionProfile, 7> kFunctionProfiles = {{
      {"aggregate.global-rank-hypothetical",
       exec::CanonicalAggregateFunction::rank},
      {"aggregate.global-dense-rank-hypothetical",
       exec::CanonicalAggregateFunction::dense_rank},
      {"aggregate.global-percent-rank-hypothetical",
       exec::CanonicalAggregateFunction::percent_rank},
      {"aggregate.global-cume-dist-hypothetical",
       exec::CanonicalAggregateFunction::cume_dist},
      {"aggregate.global-mode-ordered",
       exec::CanonicalAggregateFunction::mode},
      {"aggregate.global-percentile-cont-ordered",
       exec::CanonicalAggregateFunction::percentile_cont},
      {"aggregate.global-percentile-disc-ordered",
       exec::CanonicalAggregateFunction::percentile_disc},
  }};

  for (const auto& profile : kFunctionProfiles) {
    if (semantic_variant_id ==
        std::string(profile.prefix) + "-expression.v1") {
      result.matched = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-filter-expression.v1") {
      result.matched = true;
      result.has_filter = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-distinct-expression.v1") {
      result.matched = true;
      result.distinct = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) +
                   "-distinct-filter-expression.v1") {
      result.matched = true;
      result.distinct = true;
      result.has_filter = true;
    }
    if (!result.matched) continue;
    result.function = profile.function;
    result.transformation_id =
        "canonical." + std::string(semantic_variant_id);
    return result;
  }
  return result;
}

LiveApproximateExpressionProfile MatchLiveApproximateExpressionProfile(
    const std::string_view semantic_variant_id) {
  LiveApproximateExpressionProfile result;
  struct FunctionProfile {
    std::string_view prefix;
    exec::CanonicalAggregateFunction function;
  };
  static constexpr std::array<FunctionProfile, 5> kFunctionProfiles = {{
      {"aggregate.global-approx-count-distinct",
       exec::CanonicalAggregateFunction::approx_count_distinct},
      {"aggregate.global-approx-median",
       exec::CanonicalAggregateFunction::approx_median},
      {"aggregate.global-approx-percentile-cont-ordered",
       exec::CanonicalAggregateFunction::approx_percentile_cont},
      {"aggregate.global-approx-percentile-disc-ordered",
       exec::CanonicalAggregateFunction::approx_percentile_disc},
      {"aggregate.global-approx-top-k",
       exec::CanonicalAggregateFunction::approx_top_k},
  }};

  for (const auto& profile : kFunctionProfiles) {
    if (semantic_variant_id ==
        std::string(profile.prefix) + "-expression.v1") {
      result.matched = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-filter-expression.v1") {
      result.matched = true;
      result.has_filter = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-distinct-expression.v1") {
      result.matched = true;
      result.distinct = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) +
                   "-distinct-filter-expression.v1") {
      result.matched = true;
      result.distinct = true;
      result.has_filter = true;
    }
    if (!result.matched) continue;
    result.function = profile.function;
    result.transformation_id =
        "canonical." + std::string(semantic_variant_id);
    return result;
  }
  return result;
}


PreparedGlobalAggregateRoot PrepareGlobalAggregateRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    const exec::CanonicalAggregateFunction function,
    const bool count_star,
    const bool distinct,
    const bool has_filter,
    const std::uint32_t expected_output_ordinal = 0,
    const bool allow_sibling_outputs = false) {
  PreparedGlobalAggregateRoot result;
  result.count_star = count_star;
  result.distinct = distinct;
  const bool is_count = function == exec::CanonicalAggregateFunction::count;
  const bool is_sum = function == exec::CanonicalAggregateFunction::sum;
  const bool is_avg = function == exec::CanonicalAggregateFunction::avg;
  const bool is_min = function == exec::CanonicalAggregateFunction::min;
  const bool is_max = function == exec::CanonicalAggregateFunction::max;
  const bool is_bounded_signed_integer_aggregate =
      is_sum || is_min || is_max;
  const bool is_bool_and =
      function == exec::CanonicalAggregateFunction::bool_and;
  const bool is_bool_or =
      function == exec::CanonicalAggregateFunction::bool_or;
  const bool is_every = function == exec::CanonicalAggregateFunction::every;
  const bool is_boolean = is_bool_and || is_bool_or || is_every;
  const bool is_stddev_pop =
      function == exec::CanonicalAggregateFunction::stddev_pop;
  const bool is_variance_pop =
      function == exec::CanonicalAggregateFunction::variance_pop;
  const bool is_stddev =
      function == exec::CanonicalAggregateFunction::stddev;
  const bool is_variance =
      function == exec::CanonicalAggregateFunction::variance;
  const bool is_stddev_samp =
      function == exec::CanonicalAggregateFunction::stddev_samp;
  const bool is_variance_samp =
      function == exec::CanonicalAggregateFunction::variance_samp;
  const bool is_statistical =
      is_stddev_pop || is_variance_pop || is_stddev || is_variance ||
      is_stddev_samp || is_variance_samp;
  const bool is_regr_count =
      function == exec::CanonicalAggregateFunction::regr_count;
  const bool is_pair_statistical =
      function == exec::CanonicalAggregateFunction::corr ||
      function == exec::CanonicalAggregateFunction::covar_pop ||
      function == exec::CanonicalAggregateFunction::covar_samp ||
      is_regr_count ||
      function == exec::CanonicalAggregateFunction::regr_avgx ||
      function == exec::CanonicalAggregateFunction::regr_avgy ||
      function == exec::CanonicalAggregateFunction::regr_intercept ||
      function == exec::CanonicalAggregateFunction::regr_r2 ||
      function == exec::CanonicalAggregateFunction::regr_slope ||
      function == exec::CanonicalAggregateFunction::regr_sxx ||
      function == exec::CanonicalAggregateFunction::regr_sxy ||
      function == exec::CanonicalAggregateFunction::regr_syy;
  const bool requires_bounded_signed_input =
      is_avg || is_statistical || is_pair_statistical ||
      function == exec::CanonicalAggregateFunction::approx_median;
  const bool is_string_agg =
      function == exec::CanonicalAggregateFunction::string_agg;
  const auto string_aggregate_profile =
      MatchLiveStringAggregateExpressionProfile(root.semantic_variant_id);
  const bool is_ordered_string_agg =
      is_string_agg && string_aggregate_profile.matched &&
      string_aggregate_profile.ordered;
  const bool is_listagg =
      function == exec::CanonicalAggregateFunction::listagg;
  const auto listagg_profile =
      MatchLiveListaggExpressionProfile(root.semantic_variant_id);
  const bool is_listagg_profile = is_listagg && listagg_profile.matched;
  const auto ordered_single_collection_profile =
      MatchLiveOrderedSingleCollectionExpressionProfile(
          root.semantic_variant_id);
  const bool is_array_agg =
      function == exec::CanonicalAggregateFunction::array_agg &&
      ordered_single_collection_profile.matched &&
      ordered_single_collection_profile.function == function;
  const bool is_json_agg =
      function == exec::CanonicalAggregateFunction::json_agg &&
      ordered_single_collection_profile.matched &&
      ordered_single_collection_profile.function == function;
  const auto json_object_aggregate_profile =
      MatchLiveJsonObjectAggregateExpressionProfile(
          root.semantic_variant_id);
  const bool is_json_object_agg =
      function == exec::CanonicalAggregateFunction::json_object_agg &&
      json_object_aggregate_profile.matched;
  const bool is_ordered_single_collection = is_array_agg || is_json_agg;
  const bool is_ordered_collection =
      is_ordered_single_collection || is_json_object_agg;
  const bool has_widened_independent_order_argument =
      is_ordered_string_agg || is_listagg_profile || is_ordered_collection;
  const bool is_hypothetical_rank =
      function == exec::CanonicalAggregateFunction::rank;
  const bool is_hypothetical_dense_rank =
      function == exec::CanonicalAggregateFunction::dense_rank;
  const bool is_hypothetical_percent_rank =
      function == exec::CanonicalAggregateFunction::percent_rank;
  const bool is_hypothetical_cume_dist =
      function == exec::CanonicalAggregateFunction::cume_dist;
  const bool is_hypothetical =
      is_hypothetical_rank || is_hypothetical_dense_rank ||
      is_hypothetical_percent_rank || is_hypothetical_cume_dist;
  const bool is_mode = function == exec::CanonicalAggregateFunction::mode;
  const bool uses_bounded_signed_value =
      is_bounded_signed_integer_aggregate || is_mode;
  const bool is_percentile_cont =
      function == exec::CanonicalAggregateFunction::percentile_cont;
  const bool is_percentile_disc =
      function == exec::CanonicalAggregateFunction::percentile_disc;
  const bool is_exact_percentile = is_percentile_cont || is_percentile_disc;
  const bool is_approx_count_distinct =
      function == exec::CanonicalAggregateFunction::approx_count_distinct;
  const bool is_approx_median =
      function == exec::CanonicalAggregateFunction::approx_median;
  const bool is_approx_percentile_cont =
      function == exec::CanonicalAggregateFunction::approx_percentile_cont;
  const bool is_approx_percentile_disc =
      function == exec::CanonicalAggregateFunction::approx_percentile_disc;
  const bool is_approx_percentile =
      is_approx_percentile_cont || is_approx_percentile_disc;
  const bool is_approx_top_k =
      function == exec::CanonicalAggregateFunction::approx_top_k;
  const bool is_approximate =
      is_approx_count_distinct || is_approx_median ||
      is_approx_percentile || is_approx_top_k;
  const bool is_ordered_set =
      is_hypothetical || is_mode || is_exact_percentile ||
      is_approx_percentile;
  const bool uses_exact_core_real64_result =
      is_avg || is_statistical ||
      (is_pair_statistical && !is_regr_count) || is_exact_percentile ||
      is_approx_median || is_approx_percentile ||
      is_hypothetical_percent_rank || is_hypothetical_cume_dist;
  const bool uses_exact_core_int64_result =
      uses_bounded_signed_value || is_count || is_regr_count ||
      is_approx_count_distinct || is_hypothetical_rank ||
      is_hypothetical_dense_rank;
  if ((!is_count && !is_sum && !is_avg && !is_min && !is_max &&
       !is_boolean && !is_statistical && !is_pair_statistical &&
       !is_string_agg && !is_listagg_profile && !is_ordered_collection &&
       !is_ordered_set && !is_approximate) ||
      (count_star && !is_count)) {
    result.detail = "global aggregate function profile is not admitted";
    return result;
  }
  constexpr std::array<std::string_view, 4> kBoundedSignedTypeNames = {
      "int8", "int16", "int32", "int64"};
  std::array<std::string, kBoundedSignedTypeNames.size()>
      bounded_signed_source_type_uuids;
  std::string core_int64_type_uuid;
  std::string core_real64_result_type_uuid;
  std::string core_boolean_type_uuid;
  if (uses_exact_core_int64_result || requires_bounded_signed_input ||
      is_ordered_set ||
      has_widened_independent_order_argument) {
    const auto core_manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
    if (!core_manifest.ok()) {
      result.detail =
          "global bounded-signed aggregate/order core datatype catalog is "
          "unavailable";
      return result;
    }
    for (std::size_t index = 0; index < kBoundedSignedTypeNames.size();
         ++index) {
      const auto stable_name = kBoundedSignedTypeNames[index];
      const auto count = std::ranges::count_if(
          core_manifest.manifest.descriptor_rows, [&](const auto& row) {
            return row.stable_name == stable_name;
          });
      const auto row = std::ranges::find_if(
          core_manifest.manifest.descriptor_rows, [&](const auto& candidate) {
            return candidate.stable_name == stable_name;
          });
      if (count != 1 ||
          row == core_manifest.manifest.descriptor_rows.end() ||
          !row->descriptor_uuid.valid()) {
        result.detail =
            "global bounded-signed aggregate/order core datatype cohort is "
            "incomplete";
        return result;
      }
      bounded_signed_source_type_uuids[index] =
          ExactCanonicalCoreDatatypeTypeUuidV1(stable_name);
      if (bounded_signed_source_type_uuids[index].empty()) {
        result.detail =
            "global bounded-signed aggregate/order core datatype identity is "
            "unavailable";
        return result;
      }
    }
  }
  if (uses_exact_core_int64_result || requires_bounded_signed_input) {
    core_int64_type_uuid = ExactCanonicalInt64TypeUuidV1();
    if (core_int64_type_uuid.empty()) {
      result.detail =
          "global aggregate core int64 datatype identity is unavailable";
      return result;
    }
  }
  if (uses_exact_core_real64_result) {
    const auto core_manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
    const auto real64_count =
        core_manifest.ok()
            ? std::ranges::count_if(
                  core_manifest.manifest.descriptor_rows,
                  [](const auto& row) { return row.stable_name == "real64"; })
            : 0;
    const auto real64_type =
        core_manifest.ok()
            ? std::ranges::find_if(
                  core_manifest.manifest.descriptor_rows,
                  [](const auto& row) { return row.stable_name == "real64"; })
            : core_manifest.manifest.descriptor_rows.end();
    if (!core_manifest.ok() || real64_count != 1 ||
        real64_type == core_manifest.manifest.descriptor_rows.end() ||
        !real64_type->descriptor_uuid.valid()) {
      result.detail =
          "global real64 aggregate result core datatype cohort is incomplete";
      return result;
    }
    core_real64_result_type_uuid = scratchbird::core::uuid::UuidToString(
        real64_type->descriptor_uuid.value);
    if (core_real64_result_type_uuid.empty()) {
      result.detail =
          "global real64 aggregate result core datatype identity is unavailable";
      return result;
    }
  }
  if (is_boolean || has_filter) {
    const auto core_manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
    const auto boolean_count =
        core_manifest.ok()
            ? std::ranges::count_if(
                  core_manifest.manifest.descriptor_rows,
                  [](const auto& row) { return row.stable_name == "boolean"; })
            : 0;
    const auto boolean_type =
        core_manifest.ok()
            ? std::ranges::find_if(
                  core_manifest.manifest.descriptor_rows,
                  [](const auto& row) { return row.stable_name == "boolean"; })
            : core_manifest.manifest.descriptor_rows.end();
    if (!core_manifest.ok() || boolean_count != 1 ||
        boolean_type == core_manifest.manifest.descriptor_rows.end() ||
        !boolean_type->descriptor_uuid.valid()) {
      result.detail =
          "global aggregate boolean-input core datatype cohort is incomplete";
      return result;
    }
    core_boolean_type_uuid = scratchbird::core::uuid::UuidToString(
        boolean_type->descriptor_uuid.value);
    if (core_boolean_type_uuid.empty()) {
      result.detail =
          "global aggregate boolean-input core datatype identity is unavailable";
      return result;
    }
  }
  if (root.output_descriptor_ids.size() != 1 ||
      root.bound_expression_ids.size() != 1 ||
      input.result_bindings.size() != input.batch.columns.size() ||
      input_node.output_descriptor_ids.size() != input.batch.columns.size() ||
      std::ranges::find(input_node.output_descriptor_ids,
                        root.output_descriptor_ids.front()) !=
          input_node.output_descriptor_ids.end()) {
    result.detail =
        "global aggregate input or output descriptor coverage is unresolved";
    return result;
  }

  const api::RelationalOutputRecord* output = nullptr;
  for (const auto& candidate : dag.outputs) {
    if (candidate.relation_node_id != root.logical_node_id) continue;
    if (allow_sibling_outputs &&
        (candidate.ordinal != expected_output_ordinal ||
         candidate.descriptor_id != root.output_descriptor_ids.front() ||
         candidate.expression_id != root.bound_expression_ids.front())) {
      continue;
    }
    if (output != nullptr) {
      result.detail = "global aggregate requires exactly one bound output";
      return result;
    }
    output = &candidate;
  }
  if (output == nullptr || output->ordinal != expected_output_ordinal ||
      !output->visible ||
      output->output_name_utf8.empty() ||
      output->descriptor_id != root.output_descriptor_ids.front() ||
      output->expression_id != root.bound_expression_ids.front()) {
    result.detail = "global aggregate output lineage is not exact";
    return result;
  }

  const auto expression = std::ranges::find_if(
      dag.expressions, [&](const auto& candidate) {
        return candidate.expression_id == root.bound_expression_ids.front();
      });
  const auto descriptor = std::ranges::find_if(
      dag.descriptors, [&](const auto& candidate) {
        return candidate.descriptor_id == root.output_descriptor_ids.front();
      });
  const auto* aggregate =
      exec::LookupCanonicalAggregateByFunctionV1(function);
  std::size_t expected_argument_count = 1;
  if (count_star) {
    expected_argument_count = 0;
  } else if (is_listagg_profile) {
    expected_argument_count = listagg_profile.base_argument_count +
                              (has_filter ? 1U : 0U);
  } else if (is_ordered_set) {
    expected_argument_count = (is_mode ? 1U : 2U) +
                              (has_filter ? 1U : 0U);
  } else if (is_approx_top_k) {
    expected_argument_count = 2U + (has_filter ? 1U : 0U);
  } else if (has_filter) {
    expected_argument_count = 2;
    if (is_pair_statistical) {
      expected_argument_count = 3;
    } else if (is_string_agg) {
      expected_argument_count = is_ordered_string_agg ? 4 : 3;
    } else if (is_ordered_single_collection) {
      expected_argument_count = 3;
    } else if (is_json_object_agg) {
      expected_argument_count = 4;
    }
  } else if (is_hypothetical || is_exact_percentile ||
             is_approx_percentile || is_approx_top_k) {
    expected_argument_count = 2;
  } else if (is_json_object_agg || is_ordered_string_agg) {
    expected_argument_count = 3;
  } else if (is_pair_statistical || is_string_agg ||
             is_ordered_single_collection) {
    expected_argument_count = 2;
  }
  if (expression == dag.expressions.end() ||
      descriptor == dag.descriptors.end() || aggregate == nullptr ||
      !aggregate->executable ||
      expression->expression_kind !=
          api::RelationalExpressionKind::kFunctionCall ||
      expression->child_expression_ids.size() != expected_argument_count ||
      expression->result_descriptor_id != descriptor->descriptor_id ||
      !expression->function_uuid.has_value() ||
      *expression->function_uuid != aggregate->function_uuid ||
      expression->bound_name_uuid.has_value() ||
      expression->literal_kind.has_value() ||
      expression->operator_name.has_value() ||
      expression->literal_or_parameter_ref.has_value()) {
    result.detail =
        "global aggregate function identity or argument binding is invalid";
    return result;
  }
  if (uses_exact_core_int64_result &&
      (descriptor->type_uuid != core_int64_type_uuid ||
       descriptor->descriptor_uuid == descriptor->type_uuid ||
       descriptor->descriptor_uuid == aggregate->function_uuid)) {
    result.detail =
        "global int64 aggregate result does not bind the exact core int64 "
        "type with a distinct result descriptor identity";
    return result;
  }
  if (uses_exact_core_real64_result &&
      (descriptor->type_uuid != core_real64_result_type_uuid ||
       descriptor->descriptor_uuid == descriptor->type_uuid ||
       descriptor->descriptor_uuid == aggregate->function_uuid)) {
    result.detail =
        "global real64 aggregate result does not bind the exact core real64 "
        "type with a distinct result descriptor identity";
    return result;
  }
  if (is_boolean &&
      (descriptor->type_uuid != core_boolean_type_uuid ||
       descriptor->descriptor_uuid == descriptor->type_uuid ||
       descriptor->descriptor_uuid == aggregate->function_uuid)) {
    result.detail =
        "global boolean aggregate result does not bind the exact core "
        "boolean type with a distinct result descriptor identity";
    return result;
  }

  if (!count_star) {
    CanonicalRelationalExpressionRuntime expression_runtime(dag);
    const auto exact_bounded_signed_input =
        [&](const std::uint32_t expression_descriptor_id,
            const std::size_t value_column,
            const std::string_view input_type) {
          const auto source_descriptor = std::ranges::find_if(
              dag.descriptors, [&](const auto& candidate) {
                return candidate.descriptor_id == expression_descriptor_id;
              });
          const auto& source_column = input.batch.columns[value_column];
          const auto source_type_id = dt::CanonicalTypeIdFromStableName(
              std::string(input_type));
          const auto source_index = [&]() -> std::optional<std::size_t> {
            switch (source_type_id) {
              case dt::CanonicalTypeId::int8:
                return 0;
              case dt::CanonicalTypeId::int16:
                return 1;
              case dt::CanonicalTypeId::int32:
                return 2;
              case dt::CanonicalTypeId::int64:
                return 3;
              default:
                return std::nullopt;
            }
          }();
          return source_index.has_value() &&
                 source_descriptor != dag.descriptors.end() &&
                 *source_index < bounded_signed_source_type_uuids.size() &&
                 source_descriptor->descriptor_uuid ==
                     source_column.descriptor.descriptor_uuid.canonical &&
                 source_descriptor->descriptor_uuid !=
                     source_descriptor->type_uuid &&
                 source_descriptor->descriptor_uuid !=
                     aggregate->function_uuid &&
                 source_descriptor->descriptor_uuid !=
                     descriptor->descriptor_uuid &&
                 source_descriptor->type_uuid ==
                     bounded_signed_source_type_uuids[*source_index] &&
                 source_descriptor->nullability ==
                     (source_column.nullable
                          ? api::RelationalNullability::kNullable
                          : api::RelationalNullability::kNonNull) &&
                 !source_descriptor->collation_uuid.has_value() &&
                 !source_descriptor->timezone_profile_id.has_value() &&
                 !source_descriptor->width.has_value() &&
                 !source_descriptor->precision.has_value() &&
                 !source_descriptor->scale.has_value() &&
                 source_column.descriptor.descriptor_kind == "scalar" &&
                 exec::IsCanonicalBoundedSignedIntegerDescriptor(
                     source_column.descriptor) &&
                 api::QowCanonicalDescriptorIdentityV1(
                     source_column.descriptor);
        };
    const auto exact_boolean_input =
        [&](const std::uint32_t expression_descriptor_id,
            const std::size_t value_column,
            const std::string_view input_type) {
          const auto source_descriptor = std::ranges::find_if(
              dag.descriptors, [&](const auto& candidate) {
                return candidate.descriptor_id == expression_descriptor_id;
              });
          const auto& source_column = input.batch.columns[value_column];
          return input_type == "boolean" &&
                 !core_boolean_type_uuid.empty() &&
                 source_descriptor != dag.descriptors.end() &&
                 source_descriptor->descriptor_uuid ==
                     source_column.descriptor.descriptor_uuid.canonical &&
                 source_descriptor->descriptor_uuid !=
                     source_descriptor->type_uuid &&
                 source_descriptor->descriptor_uuid !=
                     aggregate->function_uuid &&
                 source_descriptor->descriptor_uuid !=
                     descriptor->descriptor_uuid &&
                 source_descriptor->type_uuid == core_boolean_type_uuid &&
                 source_descriptor->nullability ==
                     (source_column.nullable
                          ? api::RelationalNullability::kNullable
                          : api::RelationalNullability::kNonNull) &&
                 !source_descriptor->collation_uuid.has_value() &&
                 !source_descriptor->timezone_profile_id.has_value() &&
                 !source_descriptor->width.has_value() &&
                 !source_descriptor->precision.has_value() &&
                 !source_descriptor->scale.has_value() &&
                 source_column.descriptor.descriptor_kind == "scalar" &&
                 source_column.descriptor.canonical_type_name == "boolean" &&
                 api::QowCanonicalDescriptorIdentityV1(
                     source_column.descriptor);
        };
    for (std::size_t argument_ordinal = 0;
         argument_ordinal < expression->child_expression_ids.size();
         ++argument_ordinal) {
      const auto child_expression_id =
          expression->child_expression_ids[argument_ordinal];
      const auto argument = std::ranges::find_if(
          dag.expressions, [&](const auto& candidate) {
            return candidate.expression_id == child_expression_id;
          });
      if ((is_hypothetical || is_exact_percentile ||
           is_approx_percentile || is_approx_top_k) &&
          argument_ordinal == 0) {
        const auto direct_descriptor = std::ranges::find_if(
            dag.descriptors, [&](const auto& candidate) {
              return argument != dag.expressions.end() &&
                     candidate.descriptor_id == argument->result_descriptor_id;
            });
        bool direct_is_int64 = false;
        SblrLiteralExactDecimalCodecResultV1 direct_decimal;
        const bool exact_typed_literal = [&]() {
          if (argument == dag.expressions.end() ||
              !argument->literal_typed_value_v1.has_value() ||
              argument->parameter_typed_value_v1.has_value() ||
              direct_descriptor == dag.descriptors.end()) {
            return false;
          }
          const auto& typed = *argument->literal_typed_value_v1;
          const auto digest =
              scratchbird::core::hash::ComputeSha256Digest(
                  typed.canonical_value_bytes);
          const auto direct_int64 = DecodeSblrLiteralInt64LeV1(
              typed.canonical_value_bytes.data(),
              typed.canonical_value_bytes.size());
          direct_is_int64 = direct_int64.has_value() && *direct_int64 >= 0;
          direct_decimal = DecodeSblrLiteralExactDecimalV1(
              typed.canonical_value_bytes.data(),
              typed.canonical_value_bytes.size());
          return typed.descriptor_uuid ==
                     direct_descriptor->descriptor_uuid &&
                 typed.descriptor_generation != 0 &&
                 typed.value_state == "value" &&
                 (direct_is_int64 || direct_decimal.ok) &&
                 digest.ok() &&
                 digest.digest == typed.canonical_value_sha256;
        }();
        const bool exact_literal =
            argument != dag.expressions.end() &&
            argument->expression_kind ==
                api::RelationalExpressionKind::kLiteral &&
            argument->child_expression_ids.empty() &&
            !argument->bound_name_uuid.has_value() &&
            !argument->function_uuid.has_value() &&
            argument->literal_kind == api::RelationalLiteralKind::kNumeric &&
            !argument->operator_name.has_value() &&
            !argument->literal_or_parameter_ref.has_value() &&
            exact_typed_literal &&
            direct_descriptor != dag.descriptors.end() &&
            direct_descriptor->nullability ==
                api::RelationalNullability::kNonNull &&
            !direct_descriptor->collation_uuid.has_value() &&
            !direct_descriptor->timezone_profile_id.has_value() &&
            !direct_descriptor->width.has_value() &&
            ((direct_is_int64 &&
              !direct_descriptor->precision.has_value() &&
              !direct_descriptor->scale.has_value()) ||
             (direct_decimal.ok &&
              direct_descriptor->precision == direct_decimal.precision &&
              direct_descriptor->scale == direct_decimal.scale)) &&
            argument->result_descriptor_id !=
                root.output_descriptor_ids.front() &&
            std::ranges::find(input_node.output_descriptor_ids,
                              argument->result_descriptor_id) ==
                input_node.output_descriptor_ids.end();
        if (!exact_literal) {
          result.detail =
              "global ordered-set direct argument must be one standalone, "
              "unqualified, non-NULL canonical numeric literal";
          return result;
        }
        api::EngineTypedValue direct_argument;
        std::string direct_detail;
        const std::string_view direct_type =
            (is_exact_percentile || is_approx_percentile)
                ? std::string_view("real64")
                : std::string_view("int64");
        if (!expression_runtime.EvaluateForConsumer(
                child_expression_id, direct_type,
                api::EngineCanonicalExpressionConsumer::aggregate,
                &direct_argument, &direct_detail) ||
            direct_argument.state != api::EngineValueState::value ||
            direct_argument.is_null ||
            direct_argument.descriptor.canonical_type_name != direct_type) {
          if (is_exact_percentile || is_approx_percentile) {
            result.detail =
                "global percentile fraction must be a canonical real64 "
                "literal";
          } else if (is_approx_top_k) {
            result.detail =
                "global approximate top-k bound must be a canonical int64 "
                "literal";
          } else {
            result.detail =
                "global hypothetical-set direct argument must be a canonical "
                "int64 literal";
          }
          if (!direct_detail.empty()) result.detail += ": " + direct_detail;
          return result;
        }
        result.direct_arguments.push_back(std::move(direct_argument));
        continue;
      }
      if ((is_string_agg || is_listagg_profile) && argument_ordinal == 1) {
        const auto separator_descriptor = std::ranges::find_if(
            dag.descriptors, [&](const auto& candidate) {
              return argument != dag.expressions.end() &&
                     candidate.descriptor_id ==
                         argument->result_descriptor_id;
            });
        api::EngineTypedValue separator;
        std::string separator_detail;
        if (argument == dag.expressions.end() ||
            argument->expression_kind !=
                api::RelationalExpressionKind::kLiteral ||
            !argument->child_expression_ids.empty() ||
            argument->bound_name_uuid.has_value() ||
            argument->function_uuid.has_value() ||
            !argument->literal_kind.has_value() ||
            argument->operator_name.has_value() ||
            !argument->literal_or_parameter_ref.has_value() ||
            separator_descriptor == dag.descriptors.end() ||
            separator_descriptor->nullability !=
                api::RelationalNullability::kNonNull ||
            separator_descriptor->collation_uuid.has_value() ||
            separator_descriptor->timezone_profile_id.has_value() ||
            separator_descriptor->width.has_value() ||
            separator_descriptor->precision.has_value() ||
            separator_descriptor->scale.has_value() ||
            argument->result_descriptor_id ==
                root.output_descriptor_ids.front() ||
            std::ranges::find(input_node.output_descriptor_ids,
                              argument->result_descriptor_id) !=
                input_node.output_descriptor_ids.end() ||
            !expression_runtime.EvaluateForConsumer(
                child_expression_id, "text",
                api::EngineCanonicalExpressionConsumer::aggregate,
                &separator, &separator_detail) ||
            separator.state != api::EngineValueState::value ||
            separator.is_null ||
            separator.descriptor.canonical_type_name != "text") {
          result.detail =
              "global STRING_AGG/LISTAGG separator must be one standalone, "
              "unqualified, non-NULL canonical text literal";
          if (!separator_detail.empty()) {
            result.detail += ": " + separator_detail;
          }
          return result;
        }
        result.aggregate_separator = separator.encoded_value;
        continue;
      }
      if (is_listagg_profile && argument_ordinal >= 3 &&
          argument_ordinal < listagg_profile.base_argument_count) {
        const auto option_descriptor = std::ranges::find_if(
            dag.descriptors, [&](const auto& candidate) {
              return argument != dag.expressions.end() &&
                     candidate.descriptor_id == argument->result_descriptor_id;
            });
        const bool exact_literal =
            argument != dag.expressions.end() &&
            argument->expression_kind ==
                api::RelationalExpressionKind::kLiteral &&
            argument->child_expression_ids.empty() &&
            !argument->bound_name_uuid.has_value() &&
            !argument->function_uuid.has_value() &&
            argument->literal_kind.has_value() &&
            !argument->operator_name.has_value() &&
            (argument->literal_or_parameter_ref.has_value() ||
             argument->literal_typed_value_v1.has_value()) &&
            !argument->parameter_typed_value_v1.has_value() &&
            option_descriptor != dag.descriptors.end() &&
            option_descriptor->nullability ==
                api::RelationalNullability::kNonNull &&
            !option_descriptor->collation_uuid.has_value() &&
            !option_descriptor->timezone_profile_id.has_value() &&
            !option_descriptor->width.has_value() &&
            !option_descriptor->precision.has_value() &&
            !option_descriptor->scale.has_value() &&
            argument->result_descriptor_id !=
                root.output_descriptor_ids.front() &&
            std::ranges::find(input_node.output_descriptor_ids,
                              argument->result_descriptor_id) ==
                input_node.output_descriptor_ids.end();
        if (!exact_literal) {
          result.detail =
              "global LISTAGG overflow options must be standalone, "
              "unqualified, non-NULL canonical literals";
          return result;
        }
        api::EngineTypedValue option;
        std::string option_detail;
        if (argument_ordinal == 3) {
          if (!expression_runtime.EvaluateForConsumer(
                  child_expression_id, "int64",
                  api::EngineCanonicalExpressionConsumer::aggregate,
                  &option, &option_detail) ||
              option.state != api::EngineValueState::value || option.is_null ||
              option.descriptor.canonical_type_name != "int64") {
            result.detail =
                "global LISTAGG overflow bound must be a positive canonical "
                "int64 literal";
            if (!option_detail.empty()) result.detail += ": " + option_detail;
            return result;
          }
          const auto decoded = exec::DecodeInt64Value(option);
          if (!decoded.ok() || decoded.value <= 0 ||
              static_cast<std::uint64_t>(decoded.value) >
                  std::numeric_limits<std::size_t>::max()) {
            result.detail =
                "global LISTAGG overflow bound must be a positive canonical "
                "int64 literal";
            return result;
          }
          result.listagg_max_output_bytes =
              static_cast<std::size_t>(decoded.value);
          continue;
        }
        if (argument_ordinal == 4) {
          if (!expression_runtime.EvaluateForConsumer(
                  child_expression_id, "text",
                  api::EngineCanonicalExpressionConsumer::aggregate,
                  &option, &option_detail) ||
              option.state != api::EngineValueState::value || option.is_null ||
              option.descriptor.canonical_type_name != "text") {
            result.detail =
                "global LISTAGG truncation indicator must be a canonical "
                "text literal";
            if (!option_detail.empty()) result.detail += ": " + option_detail;
            return result;
          }
          result.listagg_truncation_indicator = option.encoded_value;
          continue;
        }
        if (argument_ordinal == 5) {
          if (!expression_runtime.EvaluateForConsumer(
                  child_expression_id, "boolean",
                  api::EngineCanonicalExpressionConsumer::aggregate,
                  &option, &option_detail) ||
              option.state != api::EngineValueState::value || option.is_null ||
              option.descriptor.canonical_type_name != "boolean" ||
              (option.encoded_value != "true" &&
               option.encoded_value != "false")) {
            result.detail =
                "global LISTAGG WITH/WITHOUT COUNT option must be a canonical "
                "boolean literal";
            if (!option_detail.empty()) result.detail += ": " + option_detail;
            return result;
          }
          result.listagg_with_count = option.encoded_value == "true";
          continue;
        }
      }
      if (argument == dag.expressions.end() ||
          argument->expression_kind !=
              api::RelationalExpressionKind::kIdentifier ||
          !argument->child_expression_ids.empty() ||
          !argument->bound_name_uuid.has_value() ||
          argument->function_uuid.has_value() ||
          argument->literal_kind.has_value() ||
          argument->operator_name.has_value() ||
          argument->literal_or_parameter_ref.has_value()) {
        result.detail =
            "global aggregate expression arguments are not exact bound input "
            "identifiers";
        return result;
      }
      const auto input_descriptor = std::ranges::find(
          input_node.output_descriptor_ids, argument->result_descriptor_id);
      if (input_descriptor == input_node.output_descriptor_ids.end() ||
          std::ranges::count(input_node.output_descriptor_ids,
                             argument->result_descriptor_id) != 1) {
        result.detail =
            "global aggregate expression descriptor is not uniquely supplied "
            "by its input";
        return result;
      }
      const auto value_column = static_cast<std::size_t>(
          std::distance(input_node.output_descriptor_ids.begin(),
                        input_descriptor));
      if (value_column >= input.batch.columns.size() ||
          input.batch.columns[value_column].descriptor_id !=
              argument->result_descriptor_id) {
        result.detail =
            "global aggregate expression input ordinal is not "
            "descriptor-exact";
        return result;
      }
      const auto input_type =
          input.batch.columns[value_column].descriptor.canonical_type_name;
      std::size_t filter_argument_ordinal = 1;
      if (is_pair_statistical) {
        filter_argument_ordinal = 2;
      } else if (is_string_agg) {
        filter_argument_ordinal = is_ordered_string_agg ? 3 : 2;
      } else if (is_ordered_single_collection) {
        filter_argument_ordinal = 2;
      } else if (is_json_object_agg) {
        filter_argument_ordinal = 3;
      } else if (is_listagg_profile) {
        filter_argument_ordinal = listagg_profile.base_argument_count;
      } else if (is_ordered_set) {
        filter_argument_ordinal = is_mode ? 1U : 2U;
      } else if (is_approx_top_k) {
        filter_argument_ordinal = 2U;
      }
      const bool is_filter_argument =
          has_filter && argument_ordinal == filter_argument_ordinal;
      if (is_filter_argument) {
        if (!exact_boolean_input(argument->result_descriptor_id, value_column,
                                 input_type)) {
          result.detail =
              "global aggregate FILTER input is not one exact core boolean "
              "column";
          return result;
        }
        if (!ValidateAggregateFilterTruthValues(
                input.batch, value_column, argument->result_descriptor_id,
                &result.detail)) {
          return result;
        }
        result.filter_column = value_column;
        result.filter_descriptor_id = argument->result_descriptor_id;
        continue;
      }
      const bool exact_bounded_signed_argument =
          exact_bounded_signed_input(argument->result_descriptor_id,
                                     value_column, input_type);
      if (uses_bounded_signed_value &&
          !exact_bounded_signed_argument) {
        result.detail =
            "global bounded-signed aggregate input is not one exact core "
            "bounded-signed column";
        return result;
      }
      const bool is_order_argument =
          (is_ordered_string_agg && argument_ordinal == 2) ||
          (is_listagg_profile && argument_ordinal == 2) ||
          (is_ordered_single_collection && argument_ordinal == 1) ||
          (is_json_object_agg && argument_ordinal == 2) ||
          (is_mode && argument_ordinal == 0) ||
          ((is_hypothetical || is_exact_percentile ||
            is_approx_percentile) &&
           argument_ordinal == 1);
      if (is_order_argument) {
        if (!exact_bounded_signed_argument) {
          result.detail =
              is_mode
                  ? "global mode value/order input must be one exact core "
                    "bounded-signed column"
                  : is_ordered_set
                  ? "global ordered-set value/order input must be one exact "
                    "core bounded-signed column"
                  : (is_ordered_string_agg
                         ? "global aggregate order input must be one exact "
                           "core bounded-signed column"
                         : "global aggregate order input must be one exact "
                           "core bounded-signed column");
          return result;
        }
        exec::CanonicalDescriptorOrderTerm order_term;
        order_term.column = value_column;
        order_term.expression_descriptor_id = argument->result_descriptor_id;
        order_term.direction =
            exec::CanonicalDescriptorOrderDirection::ascending;
        order_term.null_placement =
            exec::CanonicalDescriptorNullPlacement::last;
        const auto validation = exec::ValidateCanonicalDescriptorOrderTerm(
            order_term, input.batch.columns[value_column]);
        if (!validation.ok) {
          result.detail = validation.detail;
          return result;
        }
        result.aggregate_order_terms.push_back(std::move(order_term));
        if (is_ordered_set) {
          result.value_columns.push_back(value_column);
          result.value_descriptor_ids.push_back(
              argument->result_descriptor_id);
        }
        continue;
      }
      if (requires_bounded_signed_input) {
        if (!exact_bounded_signed_argument) {
          if (is_avg) {
            result.detail =
                "global AVG input must be one exact core bounded-signed column";
          } else if (is_statistical) {
            result.detail =
                "global unary statistical input must be one exact core "
                "bounded-signed column";
          } else if (is_pair_statistical) {
            result.detail =
                "global pair statistical inputs must each be exact core "
                "bounded-signed columns";
          }
          return result;
        }
        const auto source_descriptor = std::ranges::find_if(
            dag.descriptors, [&](const auto& candidate) {
              return candidate.descriptor_id ==
                     argument->result_descriptor_id;
            });
        const auto& source_column = input.batch.columns[value_column];
        result.exact_value_binding_receipts.push_back(
            {value_column, argument->result_descriptor_id,
             source_descriptor->descriptor_uuid,
             source_descriptor->type_uuid, source_column.nullable,
             source_column.descriptor.canonical_type_name,
             source_column.descriptor.encoded_descriptor});
      }
      result.value_columns.push_back(value_column);
      result.value_descriptor_ids.push_back(argument->result_descriptor_id);
      if (is_boolean &&
          !exact_boolean_input(argument->result_descriptor_id, value_column,
                               input_type)) {
        result.detail =
            "global BOOL_AND/BOOL_OR/EVERY input is not one exact core "
            "boolean column";
        return result;
      }
      if (is_string_agg && input_type != "text") {
        result.detail =
            "global STRING_AGG input must be a canonical text column";
        return result;
      }
      if (is_listagg_profile && input_type != "text") {
        result.detail =
            "global LISTAGG input must be a canonical text column";
        return result;
      }
      if (is_array_agg && input_type != "text") {
        result.detail =
            "global ARRAY_AGG input must be a canonical text column";
        return result;
      }
      if (is_json_object_agg && argument_ordinal == 0 &&
          input_type != "text") {
        result.detail =
            "global JSON_OBJECT_AGG key must be a canonical text column";
        return result;
      }
      if (is_approx_top_k && input_type != "text") {
        result.detail =
            "global APPROX_TOP_K input must be a canonical text column";
        return result;
      }
    }
  }
  const bool result_nullable =
      is_sum || is_avg || is_min || is_max || is_boolean || is_statistical ||
      (is_pair_statistical && !is_regr_count) || is_string_agg ||
      is_listagg_profile || is_ordered_collection || is_mode ||
      is_exact_percentile || is_approx_median || is_approx_percentile ||
      is_approx_top_k;
  const auto expected_nullability =
      result_nullable ? api::RelationalNullability::kNullable
                      : api::RelationalNullability::kNonNull;
  if (descriptor->nullability != expected_nullability ||
      descriptor->collation_uuid.has_value() ||
      descriptor->timezone_profile_id.has_value() ||
      descriptor->width.has_value() || descriptor->precision.has_value() ||
      descriptor->scale.has_value()) {
    if (is_sum) {
      result.detail =
          "global SUM result must be an unqualified nullable int64";
    } else if (is_avg) {
      result.detail =
          "global AVG result must be an unqualified nullable real64";
    } else if (is_min || is_max) {
      result.detail =
          "global MIN/MAX result must be an unqualified nullable int64";
    } else if (is_boolean) {
      result.detail =
          "global BOOL_AND/BOOL_OR/EVERY result must be an unqualified "
          "nullable boolean";
    } else if (is_statistical) {
      result.detail =
          "global unary statistical result must be an unqualified nullable "
          "real64";
    } else if (is_pair_statistical) {
      result.detail =
          is_regr_count
              ? "global REGR_COUNT result must be an unqualified non-null "
                "int64"
              : "global pair statistical result must be an unqualified "
                "nullable real64";
    } else if (is_string_agg) {
      result.detail =
          "global STRING_AGG result must be an unqualified nullable text";
    } else if (is_listagg_profile) {
      result.detail =
          "global LISTAGG result must be an unqualified nullable text";
    } else if (is_array_agg) {
      result.detail =
          "global ARRAY_AGG result must be an unqualified nullable "
          "list<text nullable>";
    } else if (is_json_agg) {
      result.detail =
          "global JSON_AGG result must be an unqualified nullable json";
    } else if (is_json_object_agg) {
      result.detail =
          "global JSON_OBJECT_AGG result must be an unqualified nullable "
          "json";
    } else if (is_mode) {
      result.detail =
          "global MODE result must be an unqualified nullable int64";
    } else if (is_exact_percentile) {
      result.detail =
          "global exact percentile result must be an unqualified nullable "
          "real64";
    } else if (is_approx_median || is_approx_percentile) {
      result.detail =
          "global approximate quantile result must be an unqualified "
          "nullable real64";
    } else if (is_approx_top_k) {
      result.detail =
          "global APPROX_TOP_K result must be an unqualified nullable json";
    } else if (is_approx_count_distinct) {
      result.detail =
          "global APPROX_COUNT_DISTINCT result must be an unqualified "
          "non-null int64";
    } else if (is_hypothetical_rank || is_hypothetical_dense_rank) {
      result.detail =
          "global hypothetical RANK/DENSE_RANK result must be an unqualified "
          "non-null int64";
    } else if (is_hypothetical_percent_rank ||
               is_hypothetical_cume_dist) {
      result.detail =
          "global hypothetical PERCENT_RANK/CUME_DIST result must be an "
          "unqualified non-null real64";
    } else {
      result.detail =
          "global COUNT result must be an unqualified non-null int64";
    }
    return result;
  }

  result.aggregate_descriptor =
      {aggregate->abi_version, aggregate->function, aggregate->builtin_id,
       aggregate->function_uuid, count_star};
  api::EngineDescriptor engine_descriptor;
  engine_descriptor.descriptor_uuid.canonical = descriptor->descriptor_uuid;
  engine_descriptor.descriptor_kind = "scalar";
  if (is_array_agg) {
    engine_descriptor.canonical_type_name = "list<text nullable>";
  } else if (is_json_agg || is_json_object_agg || is_approx_top_k) {
    engine_descriptor.canonical_type_name = "json";
  } else if (is_string_agg || is_listagg_profile) {
    engine_descriptor.canonical_type_name = "text";
  } else if (is_avg || is_statistical ||
             (is_pair_statistical && !is_regr_count) ||
             is_exact_percentile || is_approx_median ||
             is_approx_percentile || is_hypothetical_percent_rank ||
             is_hypothetical_cume_dist) {
    engine_descriptor.canonical_type_name = "real64";
  } else if (is_boolean) {
    engine_descriptor.canonical_type_name = "boolean";
  } else {
    engine_descriptor.canonical_type_name = "int64";
  }
  engine_descriptor.encoded_descriptor =
      "type_uuid=" + descriptor->type_uuid + ";nullability=" +
      (result_nullable ? "nullable" : "non_null");
  result.result_column = {output->output_name_utf8, engine_descriptor,
                          result_nullable, descriptor->descriptor_id};
  exec::CanonicalResultColumnBinding binding;
  binding.physical_column_ordinal = expected_output_ordinal;
  binding.visible = true;
  binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
      expected_output_ordinal,
      output->output_name_utf8,
      descriptor->descriptor_uuid,
      descriptor->type_uuid,
      result_nullable ? exec::CanonicalResultNullability::kNullable
                      : exec::CanonicalResultNullability::kNonNull,
      std::nullopt,
      std::nullopt};
  result.result_bindings.push_back(std::move(binding));
  if (is_listagg_profile) {
    result.listagg_overflow_mode = listagg_profile.overflow_mode;
  }
  result.ok = true;
  return result;
}

bool InitializePlanningAggregateDistinctState(
    const api::EngineRequestContext& context,
    const exec::DescriptorBatch& input_batch,
    const PreparedGlobalAggregateRoot& prepared,
    const std::uint64_t maximum_auxiliary_memory_bytes,
    PlanningAggregateDistinctState* state,
    std::string* detail,
    const std::optional<std::size_t> admitted_row_count = std::nullopt) {
  if (state == nullptr || detail == nullptr) return false;
  *state = {};
  detail->clear();
  state->active = prepared.distinct;
  if (!state->active) return true;
  if (prepared.value_columns.empty() ||
      prepared.value_columns.size() != prepared.value_descriptor_ids.size()) {
    *detail = "aggregate DISTINCT planning arity is not exact";
    return false;
  }
  const auto planned_row_count =
      admitted_row_count.value_or(input_batch.rows.size());
  if (planned_row_count > input_batch.rows.size()) {
    *detail = "aggregate DISTINCT admitted-row count exceeds its input";
    return false;
  }
  std::uint64_t planned_container_bytes = 0;
  if (!CheckedMultiply(planned_row_count, sizeof(std::string),
                       &planned_container_bytes) ||
      planned_container_bytes > maximum_auxiliary_memory_bytes) {
    *detail = "aggregate DISTINCT planning container exceeds its memory bound";
    return false;
  }
  try {
    state->equality_terms.reserve(prepared.value_columns.size());
    for (std::size_t index = 0; index < prepared.value_columns.size(); ++index) {
      const auto column = prepared.value_columns[index];
      if (column >= input_batch.columns.size() ||
          input_batch.columns[column].descriptor_id !=
              prepared.value_descriptor_ids[index]) {
        *detail = "aggregate DISTINCT planning descriptor is unresolved";
        return false;
      }
      exec::CanonicalDescriptorOrderTerm term;
      if (!BindCanonicalDescriptorEqualityTerm(
              context, input_batch.columns[column], column, &term, detail)) {
        return false;
      }
      state->equality_terms.push_back(std::move(term));
    }
    state->keys.reserve(planned_row_count);
  } catch (const std::bad_alloc&) {
    *detail = "aggregate DISTINCT planning allocation was refused";
    return false;
  } catch (const std::length_error&) {
    *detail = "aggregate DISTINCT planning allocation exceeded its size domain";
    return false;
  }
  if (!CheckedMultiply(state->keys.capacity(), sizeof(std::string),
                       &state->retained_memory_bytes) ||
      state->retained_memory_bytes > maximum_auxiliary_memory_bytes) {
    *detail = "aggregate DISTINCT planning container exceeds its memory bound";
    return false;
  }
  state->peak_memory_bytes = state->retained_memory_bytes;
  return true;
}

bool InitializePlanningAggregateModifierState(
    const exec::DescriptorBatch& input_batch,
    const PreparedGlobalAggregateRoot& prepared,
    const std::uint64_t input_memory_bytes,
    const std::uint64_t memory_budget_bytes,
    PlanningAggregateModifierState* state,
    std::string* detail) {
  if (state == nullptr || detail == nullptr) return false;
  *state = {};
  detail->clear();
  if (input_memory_bytes > memory_budget_bytes) {
    *detail = "aggregate modifier input exceeds its admitted memory bound";
    return false;
  }
  if (prepared.filter_column.has_value()) {
    std::vector<api::EngineSqlTruthValue> filter_truth_values;
    if (!MaterializeAggregateFilterTruthValues(
            input_batch, *prepared.filter_column,
            prepared.filter_descriptor_id,
            memory_budget_bytes - input_memory_bytes,
            &filter_truth_values, &state->filter_truth_memory_bytes,
            detail)) {
      return false;
    }
    state->filter_truth_values = std::move(filter_truth_values);
  }
  state->admitted_row_count =
      state->filter_truth_values.has_value()
          ? static_cast<std::size_t>(std::ranges::count(
                *state->filter_truth_values,
                api::EngineSqlTruthValue::true_value))
          : input_batch.rows.size();
  try {
    state->transition_ordinals.reserve(state->admitted_row_count);
  } catch (const std::bad_alloc&) {
    *detail = "aggregate transition workspace allocation was refused";
    return false;
  } catch (const std::length_error&) {
    *detail = "aggregate transition workspace capacity overflowed";
    return false;
  }
  if (!CheckedMultiply(state->transition_ordinals.capacity(),
                       sizeof(std::size_t),
                       &state->transition_memory_bytes) ||
      state->filter_truth_memory_bytes >
          memory_budget_bytes - input_memory_bytes ||
      state->transition_memory_bytes >
          memory_budget_bytes - input_memory_bytes -
              state->filter_truth_memory_bytes) {
    *detail = "aggregate modifier workspace exceeds its admitted memory bound";
    return false;
  }
  state->distinct_memory_bound =
      memory_budget_bytes - input_memory_bytes -
      state->filter_truth_memory_bytes - state->transition_memory_bytes;
  if (prepared.distinct &&
      (!CheckedMultiply(state->admitted_row_count,
                        prepared.value_columns.size(),
                        &state->distinct_generation_bound) ||
       (state->admitted_row_count > 1 &&
        !CheckedMultiply(state->admitted_row_count,
                         state->admitted_row_count - 1,
                         &state->distinct_comparison_bound)))) {
    *detail = "aggregate DISTINCT work bound overflowed";
    return false;
  }
  state->distinct_comparison_bound /= 2;
  return true;
}

bool AdmitPlanningAggregateDistinctTuple(
    const std::array<const api::EngineTypedValue*, 2>& values,
    const std::size_t value_count,
    const std::uint64_t maximum_generation_count,
    const std::uint64_t maximum_comparison_count,
    const std::uint64_t maximum_auxiliary_memory_bytes,
    PlanningAggregateDistinctState* state,
    bool* admitted,
    std::string* detail) {
  if (state == nullptr || admitted == nullptr || detail == nullptr ||
      !state->active || value_count == 0 || value_count > values.size() ||
      value_count != state->equality_terms.size()) {
    return false;
  }
  *admitted = false;
  detail->clear();

  std::size_t retained_key_bound = 0;
  std::size_t maximum_scalar_generation_peak = 0;
  for (std::size_t index = 0; index < value_count; ++index) {
    if (values[index] == nullptr) {
      *detail = "aggregate DISTINCT planning value is absent";
      return false;
    }
    const auto scalar_plan = exec::PlanCanonicalDescriptorEqualityKey(
        *values[index], state->equality_terms[index]);
    std::uint64_t framed_retained = scalar_plan.retained_key_bytes;
    if (!scalar_plan.diagnostic.ok ||
        !CheckedAdd(framed_retained, sizeof(std::uint64_t),
                    &framed_retained) ||
        framed_retained >
            std::numeric_limits<std::size_t>::max() - retained_key_bound) {
      *detail = scalar_plan.diagnostic.ok
                    ? "aggregate DISTINCT tuple-key size overflowed"
                    : (scalar_plan.diagnostic.detail.empty()
                           ? scalar_plan.diagnostic.diagnostic_code
                           : scalar_plan.diagnostic.detail);
      return false;
    }
    retained_key_bound += static_cast<std::size_t>(framed_retained);
    maximum_scalar_generation_peak = std::max(
        maximum_scalar_generation_peak,
        scalar_plan.peak_workspace_bytes);
  }
  std::uint64_t peak_workspace_bound = retained_key_bound;
  if (!CheckedAdd(peak_workspace_bound, maximum_scalar_generation_peak,
                  &peak_workspace_bound)) {
    *detail = "aggregate DISTINCT tuple-key workspace overflowed";
    return false;
  }
  std::uint64_t live_peak = state->retained_memory_bytes;
  if (!CheckedAdd(live_peak, peak_workspace_bound, &live_peak) ||
      live_peak > maximum_auxiliary_memory_bytes) {
    *detail = "aggregate DISTINCT tuple-key workspace exceeds its memory bound";
    return false;
  }
  state->peak_memory_bytes = std::max(state->peak_memory_bytes, live_peak);
  if (value_count > maximum_generation_count ||
      state->equality_key_generation_count >
          maximum_generation_count - value_count) {
    *detail = "aggregate DISTINCT equality-key generation bound is exhausted";
    return false;
  }
  state->equality_key_generation_count += value_count;

  std::string tuple_key;
  try {
    tuple_key.reserve(retained_key_bound);
    for (std::size_t index = 0; index < value_count; ++index) {
      const auto identity = exec::MakeCanonicalDescriptorEqualityKey(
          *values[index], state->equality_terms[index]);
      if (!identity.diagnostic.ok) {
        *detail = identity.diagnostic.detail.empty()
                      ? identity.diagnostic.diagnostic_code
                      : identity.diagnostic.detail;
        return false;
      }
      const auto size = static_cast<std::uint64_t>(
          identity.equality_key.size());
      for (unsigned shift = 0; shift < 64; shift += 8) {
        tuple_key.push_back(static_cast<char>(size >> shift));
      }
      tuple_key.append(identity.equality_key);
    }
  } catch (const std::bad_alloc&) {
    *detail = "aggregate DISTINCT tuple-key allocation was refused";
    return false;
  } catch (const std::length_error&) {
    *detail = "aggregate DISTINCT tuple-key exceeded its size domain";
    return false;
  }
  if (tuple_key.size() > retained_key_bound ||
      tuple_key.capacity() > retained_key_bound) {
    *detail = "aggregate DISTINCT tuple-key exceeded its allocation plan";
    return false;
  }
  for (const auto& candidate : state->keys) {
    if (state->equality_comparison_count >= maximum_comparison_count) {
      *detail = "aggregate DISTINCT equality comparison bound is exhausted";
      return false;
    }
    ++state->equality_comparison_count;
    if (candidate == tuple_key) return true;
  }
  if (tuple_key.capacity() >
          maximum_auxiliary_memory_bytes - state->retained_memory_bytes ||
      !CheckedAdd(state->retained_memory_bytes, tuple_key.capacity(),
                  &state->retained_memory_bytes)) {
    *detail = "aggregate DISTINCT retained keys exceed their memory bound";
    return false;
  }
  try {
    state->keys.push_back(std::move(tuple_key));
  } catch (const std::bad_alloc&) {
    *detail = "aggregate DISTINCT retained-key allocation was refused";
    return false;
  } catch (const std::length_error&) {
    *detail = "aggregate DISTINCT retained-key count exceeded its size domain";
    return false;
  }
  state->peak_memory_bytes =
      std::max(state->peak_memory_bytes, state->retained_memory_bytes);
  *admitted = true;
  return true;
}

}  // namespace

LiveUnaryAggregateExpressionProfile
MatchLiveUnaryAggregateExpressionProfileForComposition(
    const std::string_view semantic_variant_id) {
  return MatchLiveUnaryAggregateExpressionProfile(semantic_variant_id);
}

LivePairStatisticalExpressionProfile
MatchLivePairStatisticalExpressionProfileForComposition(
    const std::string_view semantic_variant_id) {
  return MatchLivePairStatisticalExpressionProfile(semantic_variant_id);
}

LiveStringAggregateExpressionProfile
MatchLiveStringAggregateExpressionProfileForComposition(
    const std::string_view semantic_variant_id) {
  return MatchLiveStringAggregateExpressionProfile(semantic_variant_id);
}

LiveOrderedSingleCollectionExpressionProfile
MatchLiveOrderedSingleCollectionExpressionProfileForComposition(
    const std::string_view semantic_variant_id) {
  return MatchLiveOrderedSingleCollectionExpressionProfile(
      semantic_variant_id);
}

LiveJsonObjectAggregateExpressionProfile
MatchLiveJsonObjectAggregateExpressionProfileForComposition(
    const std::string_view semantic_variant_id) {
  return MatchLiveJsonObjectAggregateExpressionProfile(semantic_variant_id);
}

LiveListaggExpressionProfile MatchLiveListaggExpressionProfileForComposition(
    const std::string_view semantic_variant_id) {
  return MatchLiveListaggExpressionProfile(semantic_variant_id);
}

LiveOrderedSetExpressionProfile
MatchLiveOrderedSetExpressionProfileForComposition(
    const std::string_view semantic_variant_id) {
  return MatchLiveOrderedSetExpressionProfile(semantic_variant_id);
}

LiveApproximateExpressionProfile
MatchLiveApproximateExpressionProfileForComposition(
    const std::string_view semantic_variant_id) {
  return MatchLiveApproximateExpressionProfile(semantic_variant_id);
}

bool MeasureAggregateDistinctPeakMemoryForComposition(
    const api::EngineRequestContext& context,
    const exec::DescriptorBatch& input_batch,
    const PreparedGlobalAggregateRoot& prepared,
    const std::optional<std::vector<api::EngineSqlTruthValue>>&
        filter_truth_values,
    const std::size_t admitted_input_row_count,
    const std::uint64_t distinct_generation_bound,
    const std::uint64_t distinct_comparison_bound,
    const std::uint64_t distinct_memory_bound,
    std::uint64_t* peak_memory_bytes,
    std::string* detail) {
  if (peak_memory_bytes == nullptr || detail == nullptr) return false;
  *peak_memory_bytes = 0;
  PlanningAggregateDistinctState state;
  if (!InitializePlanningAggregateDistinctState(
          context, input_batch, prepared, distinct_memory_bound, &state,
          detail, admitted_input_row_count)) {
    return false;
  }
  for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
    if (filter_truth_values.has_value() &&
        (*filter_truth_values)[row] != api::EngineSqlTruthValue::true_value) {
      continue;
    }
    std::array<const api::EngineTypedValue*, 2> values{};
    for (std::size_t value = 0; value < prepared.value_columns.size();
         ++value) {
      values[value] =
          &input_batch.rows[row].values[prepared.value_columns[value]];
    }
    bool admitted = false;
    if (!AdmitPlanningAggregateDistinctTuple(
            values, prepared.value_columns.size(), distinct_generation_bound,
            distinct_comparison_bound, distinct_memory_bound, &state,
            &admitted, detail)) {
      return false;
    }
  }
  *peak_memory_bytes = state.peak_memory_bytes;
  return true;
}

PreparedGlobalAggregateRoot PrepareGlobalAggregateRootForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    const exec::CanonicalAggregateFunction function,
    const bool count_star,
    const bool distinct,
    const bool has_filter,
    const std::uint32_t expected_output_ordinal,
    const bool allow_sibling_outputs) {
  return PrepareGlobalAggregateRoot(
      dag, root, input_node, input, function, count_star, distinct, has_filter,
      expected_output_ordinal, allow_sibling_outputs);
}

bool InitializePlanningAggregateDistinctStateForComposition(
    const api::EngineRequestContext& context,
    const exec::DescriptorBatch& input_batch,
    const PreparedGlobalAggregateRoot& prepared,
    const std::uint64_t maximum_auxiliary_memory_bytes,
    PlanningAggregateDistinctState* state,
    std::string* detail,
    const std::optional<std::size_t> admitted_row_count) {
  return InitializePlanningAggregateDistinctState(
      context, input_batch, prepared, maximum_auxiliary_memory_bytes, state,
      detail, admitted_row_count);
}

bool InitializePlanningAggregateModifierStateForComposition(
    const exec::DescriptorBatch& input_batch,
    const PreparedGlobalAggregateRoot& prepared,
    const std::uint64_t input_memory_bytes,
    const std::uint64_t memory_budget_bytes,
    PlanningAggregateModifierState* state,
    std::string* detail) {
  return InitializePlanningAggregateModifierState(
      input_batch, prepared, input_memory_bytes, memory_budget_bytes, state,
      detail);
}

bool AdmitPlanningAggregateDistinctTupleForComposition(
    const std::array<const api::EngineTypedValue*, 2>& values,
    const std::size_t value_count,
    const std::uint64_t maximum_generation_count,
    const std::uint64_t maximum_comparison_count,
    const std::uint64_t maximum_auxiliary_memory_bytes,
    PlanningAggregateDistinctState* state,
    bool* admitted,
    std::string* detail) {
  return AdmitPlanningAggregateDistinctTuple(
      values, value_count, maximum_generation_count,
      maximum_comparison_count, maximum_auxiliary_memory_bytes, state,
      admitted, detail);
}

}  // namespace scratchbird::engine::sblr
