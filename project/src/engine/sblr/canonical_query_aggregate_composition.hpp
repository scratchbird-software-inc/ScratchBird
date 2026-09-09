// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_execute.hpp"
#include "canonical_relational_expression.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;

struct MaterializedValues;

struct PreparedGroupedHavingRoot {
  bool ok{false};
  std::uint32_t predicate_expression_id{0};
  CanonicalRelationalExpressionRowBinding row_binding;
  std::size_t output_column_count{0};
  std::string detail;
};

struct LiveGroupedCountSumProfile {
  bool matched{false};
  std::size_t key_count{0};
  exec::CanonicalAggregateGroupingExpansionKind expansion_kind =
      exec::CanonicalAggregateGroupingExpansionKind::explicit_sets;
  std::vector<exec::CanonicalAggregateGroupingSet> grouping_sets;
  bool grouping_sets_from_sblr{false};
  bool projects_grouping_metadata{false};
  std::string transformation_id;
};

struct LiveUnaryAggregateExpressionProfile {
  bool matched{false};
  bool count_star{false};
  bool distinct{false};
  bool has_filter{false};
  exec::CanonicalAggregateFunction function =
      exec::CanonicalAggregateFunction::unknown;
  std::string transformation_id;
};

struct LivePairStatisticalExpressionProfile {
  bool matched{false};
  bool distinct{false};
  bool has_filter{false};
  exec::CanonicalAggregateFunction function =
      exec::CanonicalAggregateFunction::unknown;
  std::string transformation_id;
};

struct LiveStringAggregateExpressionProfile {
  bool matched{false};
  bool ordered{false};
  bool distinct{false};
  bool has_filter{false};
  std::string transformation_id;
};

struct LiveOrderedSingleCollectionExpressionProfile {
  bool matched{false};
  bool distinct{false};
  bool has_filter{false};
  exec::CanonicalAggregateFunction function =
      exec::CanonicalAggregateFunction::unknown;
  std::string transformation_id;
};

struct LiveJsonObjectAggregateExpressionProfile {
  bool matched{false};
  bool distinct{false};
  bool has_filter{false};
  std::string transformation_id;
};

struct LiveListaggExpressionProfile {
  bool matched{false};
  bool distinct{false};
  bool has_filter{false};
  std::size_t base_argument_count{0};
  exec::CanonicalListaggOverflowMode overflow_mode =
      exec::CanonicalListaggOverflowMode::none;
  std::string transformation_id;
};

struct LiveOrderedSetExpressionProfile {
  bool matched{false};
  bool distinct{false};
  bool has_filter{false};
  exec::CanonicalAggregateFunction function =
      exec::CanonicalAggregateFunction::unknown;
  std::string transformation_id;
};

struct LiveApproximateExpressionProfile {
  bool matched{false};
  bool distinct{false};
  bool has_filter{false};
  exec::CanonicalAggregateFunction function =
      exec::CanonicalAggregateFunction::unknown;
  std::string transformation_id;
};

struct PlanningAggregateDistinctState {
  bool active{false};
  std::vector<exec::CanonicalDescriptorOrderTerm> equality_terms;
  std::vector<std::string> keys;
  std::uint64_t retained_memory_bytes{0};
  std::uint64_t peak_memory_bytes{0};
  std::uint64_t equality_key_generation_count{0};
  std::uint64_t equality_comparison_count{0};
};

struct PlanningAggregateModifierState {
  std::optional<std::vector<api::EngineSqlTruthValue>> filter_truth_values;
  std::vector<std::size_t> transition_ordinals;
  std::size_t admitted_row_count{0};
  std::uint64_t filter_truth_memory_bytes{0};
  std::uint64_t transition_memory_bytes{0};
  std::uint64_t distinct_generation_bound{0};
  std::uint64_t distinct_comparison_bound{0};
  std::uint64_t distinct_memory_bound{0};
};

LiveGroupedCountSumProfile MatchLiveGroupedCountSumProfileForComposition(
    std::string_view semantic_variant_id);
bool IsLiveGroupedHavingProfileForComposition(
    std::string_view semantic_variant_id);
LiveUnaryAggregateExpressionProfile
MatchLiveUnaryAggregateExpressionProfileForComposition(
    std::string_view semantic_variant_id);
LivePairStatisticalExpressionProfile
MatchLivePairStatisticalExpressionProfileForComposition(
    std::string_view semantic_variant_id);
LiveStringAggregateExpressionProfile
MatchLiveStringAggregateExpressionProfileForComposition(
    std::string_view semantic_variant_id);
LiveOrderedSingleCollectionExpressionProfile
MatchLiveOrderedSingleCollectionExpressionProfileForComposition(
    std::string_view semantic_variant_id);
LiveJsonObjectAggregateExpressionProfile
MatchLiveJsonObjectAggregateExpressionProfileForComposition(
    std::string_view semantic_variant_id);
LiveListaggExpressionProfile MatchLiveListaggExpressionProfileForComposition(
    std::string_view semantic_variant_id);
LiveOrderedSetExpressionProfile
MatchLiveOrderedSetExpressionProfileForComposition(
    std::string_view semantic_variant_id);
LiveApproximateExpressionProfile
MatchLiveApproximateExpressionProfileForComposition(
    std::string_view semantic_variant_id);

PreparedGroupedCountSumRoot PrepareGroupedCountSumRootForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    const LiveGroupedCountSumProfile& profile);

PreparedGroupedHavingRoot PrepareGroupedHavingRootForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& aggregate_root,
    const plan::CanonicalLogicalRelationalNode& values_node,
    const PreparedGroupedCountSumRoot& aggregate);

bool MeasureAggregateDistinctPeakMemoryForComposition(
    const api::EngineRequestContext& context,
    const exec::DescriptorBatch& input_batch,
    const PreparedGlobalAggregateRoot& prepared,
    const std::optional<std::vector<api::EngineSqlTruthValue>>&
        filter_truth_values,
    std::size_t admitted_input_row_count,
    std::uint64_t distinct_generation_bound,
    std::uint64_t distinct_comparison_bound,
    std::uint64_t distinct_memory_bound,
    std::uint64_t* peak_memory_bytes,
    std::string* detail);

bool InitializePlanningAggregateDistinctStateForComposition(
    const api::EngineRequestContext& context,
    const exec::DescriptorBatch& input_batch,
    const PreparedGlobalAggregateRoot& prepared,
    std::uint64_t maximum_auxiliary_memory_bytes,
    PlanningAggregateDistinctState* state,
    std::string* detail,
    std::optional<std::size_t> admitted_row_count = std::nullopt);

bool InitializePlanningAggregateModifierStateForComposition(
    const exec::DescriptorBatch& input_batch,
    const PreparedGlobalAggregateRoot& prepared,
    std::uint64_t input_memory_bytes,
    std::uint64_t memory_budget_bytes,
    PlanningAggregateModifierState* state,
    std::string* detail);

bool AdmitPlanningAggregateDistinctTupleForComposition(
    const std::array<const api::EngineTypedValue*, 2>& values,
    std::size_t value_count,
    std::uint64_t maximum_generation_count,
    std::uint64_t maximum_comparison_count,
    std::uint64_t maximum_auxiliary_memory_bytes,
    PlanningAggregateDistinctState* state,
    bool* admitted,
    std::string* detail);

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeGroupedCountSumQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request);

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeGlobalAggregateQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request);

}  // namespace scratchbird::engine::sblr
