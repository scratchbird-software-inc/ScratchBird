// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "selectivity_model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

double Clamp(double value, double minimum, double maximum) {
  return std::max(minimum, std::min(maximum, value));
}

std::uint64_t SaturatingMultiply(std::uint64_t left, std::uint64_t right) {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left * right;
}

double OneRowSelectivity(std::uint64_t rows) {
  return rows == 0 ? 0.0 : 1.0 / static_cast<double>(rows);
}

int ConfidenceRank(CostConfidence confidence) {
  switch (confidence) {
    case CostConfidence::kExact: return 5;
    case CostConfidence::kHigh: return 4;
    case CostConfidence::kMedium: return 3;
    case CostConfidence::kLow: return 2;
    case CostConfidence::kUnknown: return 1;
    case CostConfidence::kRejected: return 0;
  }
  return 1;
}

CostConfidence LowerConfidence(CostConfidence left, CostConfidence right) {
  return ConfidenceRank(left) < ConfidenceRank(right) ? left : right;
}

bool ConfidenceAtLeast(CostConfidence actual, CostConfidence minimum) {
  return ConfidenceRank(actual) >= ConfidenceRank(minimum);
}

CostConfidence ConfidenceFromStats(bool exact, bool has_stats, CostConfidence input_confidence) {
  if (exact) return CostConfidence::kExact;
  if (input_confidence == CostConfidence::kRejected) return CostConfidence::kRejected;
  if (input_confidence == CostConfidence::kUnknown) return has_stats ? CostConfidence::kMedium : CostConfidence::kLow;
  return input_confidence;
}

SelectivityEstimate Make(double selectivity,
                         CostConfidence confidence,
                         std::string diagnostic_code,
                         bool conservative) {
  SelectivityEstimate estimate;
  estimate.selectivity = Clamp(selectivity, 0.0, 1.0);
  estimate.confidence = confidence;
  estimate.diagnostic_code = std::move(diagnostic_code);
  estimate.conservative = conservative;
  return estimate;
}

SelectivityEstimate OneRowEstimate(std::uint64_t input_rows, std::string diagnostic_code) {
  auto estimate = Make(OneRowSelectivity(input_rows),
                       CostConfidence::kExact,
                       std::move(diagnostic_code),
                       false);
  estimate.exact_rows_known = true;
  estimate.exact_rows = 1;
  return estimate;
}

SelectivityEstimate ScalarEqualityEstimate(const PredicateSelectivityInput& input) {
  const double non_null_fraction = Clamp(1.0 - input.null_fraction, 0.0, 1.0);
  if (input.has_mcv_frequency || input.has_mcv) {
    const double frequency = input.has_mcv_frequency ? input.mcv_frequency : 0.05;
    return Make(Clamp(frequency, OneRowSelectivity(input.input_rows), non_null_fraction),
                ConfidenceFromStats(false, true, input.input_confidence),
                "SB_OPTIMIZER_SELECTIVITY.MCV_EQ",
                false);
  }
  if (input.distinct_values != 0) {
    return Make(Clamp(non_null_fraction / static_cast<double>(input.distinct_values),
                      OneRowSelectivity(input.input_rows),
                      non_null_fraction),
                ConfidenceFromStats(false, true, input.input_confidence),
                "SB_OPTIMIZER_SELECTIVITY.NDV_EQ",
                false);
  }
  return Make(0.10,
              ConfidenceFromStats(false, false, input.input_confidence),
              "SB_OPTIMIZER_SELECTIVITY.DEFAULT_EQ",
              true);
}

SelectivityEstimate CombinedAnd(const PredicateSelectivityInput& input) {
  if (input.children.empty()) {
    return Make(0.5, CostConfidence::kUnknown, "SB_OPTIMIZER_SELECTIVITY.AND_EMPTY", true);
  }
  double value = 1.0;
  CostConfidence confidence = CostConfidence::kExact;
  bool conservative = false;
  for (const auto& child : input.children) {
    value *= Clamp(child.selectivity, 0.0, 1.0);
    confidence = LowerConfidence(confidence, child.confidence);
    conservative = conservative || child.conservative;
  }
  return Make(value, confidence, "SB_OPTIMIZER_SELECTIVITY.AND_PRODUCT", conservative);
}

double IndependentChildSelectivity(const std::vector<SelectivityEstimate>& children) {
  double value = 1.0;
  for (const auto& child : children) {
    value *= Clamp(child.selectivity, 0.0, 1.0);
  }
  return value;
}

CostConfidence ChildConfidence(const std::vector<SelectivityEstimate>& children) {
  CostConfidence confidence = CostConfidence::kExact;
  if (children.empty()) return CostConfidence::kUnknown;
  for (const auto& child : children) {
    confidence = LowerConfidence(confidence, child.confidence);
  }
  return confidence;
}

bool ChildConservative(const std::vector<SelectivityEstimate>& children) {
  for (const auto& child : children) {
    if (child.conservative) return true;
  }
  return false;
}

std::vector<std::string> Sorted(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  return values;
}

bool SameShape(const std::vector<std::string>& left, const std::vector<std::string>& right) {
  return Sorted(left) == Sorted(right);
}

struct ShapeDimension {
  std::string key;
  std::string value_encoding;
  SelectivityEstimate child;
};

std::vector<ShapeDimension> RequestDimensions(const ExtendedStatsSelectivityRequest& request) {
  std::vector<ShapeDimension> dimensions;
  dimensions.reserve(request.column_uuids.size() + request.document_path_digests.size());
  for (std::size_t i = 0; i < request.column_uuids.size(); ++i) {
    ShapeDimension dimension;
    dimension.key = "column:" + request.column_uuids[i];
    if (i < request.value_encodings.size()) dimension.value_encoding = request.value_encodings[i];
    if (i < request.children.size()) dimension.child = request.children[i];
    dimensions.push_back(std::move(dimension));
  }
  for (std::size_t i = 0; i < request.document_path_digests.size(); ++i) {
    ShapeDimension dimension;
    dimension.key = "path:" + request.document_path_digests[i];
    const auto request_index = request.column_uuids.size() + i;
    if (request_index < request.value_encodings.size()) {
      dimension.value_encoding = request.value_encodings[request_index];
    }
    if (request_index < request.children.size()) dimension.child = request.children[request_index];
    dimensions.push_back(std::move(dimension));
  }
  return dimensions;
}

std::vector<std::string> StatisticDimensionKeys(const ExtendedOptimizerStatistic& stats) {
  std::vector<std::string> keys;
  keys.reserve(stats.column_uuids.size() + stats.document_path_digests.size());
  for (const auto& column_uuid : stats.column_uuids) keys.push_back("column:" + column_uuid);
  for (const auto& path_digest : stats.document_path_digests) keys.push_back("path:" + path_digest);
  return keys;
}

bool FindStatisticShapeInRequest(const std::vector<ShapeDimension>& request_dimensions,
                                 const ExtendedOptimizerStatistic& stats,
                                 std::vector<std::size_t>* request_indices) {
  request_indices->clear();
  for (const auto& key : StatisticDimensionKeys(stats)) {
    auto it = std::find_if(request_dimensions.begin(),
                           request_dimensions.end(),
                           [&](const ShapeDimension& dimension) {
                             return dimension.key == key;
                           });
    if (it == request_dimensions.end()) return false;
    request_indices->push_back(static_cast<std::size_t>(it - request_dimensions.begin()));
  }
  return !request_indices->empty();
}

bool ExactShapeMatch(const std::vector<ShapeDimension>& request_dimensions,
                     const ExtendedOptimizerStatistic& stats) {
  std::vector<std::string> request_keys;
  request_keys.reserve(request_dimensions.size());
  for (const auto& dimension : request_dimensions) request_keys.push_back(dimension.key);
  return SameShape(request_keys, StatisticDimensionKeys(stats));
}

bool JointMcvValuesMatch(const ExtendedStatsSelectivityRequest& request,
                         const ExtendedOptimizerJointMcvEntry& entry) {
  return !request.value_encodings.empty() &&
         request.value_encodings.size() == entry.value_encodings.size() &&
         request.value_encodings == entry.value_encodings;
}

bool JointMcvExactValueMatch(const ExtendedStatsSelectivityRequest& request,
                             const ExtendedOptimizerStatistic& stats) {
  if (stats.kind != ExtendedOptimizerStatisticKind::kJointMcv) return false;
  for (const auto& entry : stats.joint_mcv) {
    if (JointMcvValuesMatch(request, entry)) return true;
  }
  return false;
}

ExtendedStatsSelectivityRequest SubsetRequest(
    const ExtendedStatsSelectivityRequest& request,
    const std::vector<ShapeDimension>& request_dimensions,
    const ExtendedOptimizerStatistic& stats,
    const std::vector<std::size_t>& request_indices,
    bool exact_shape) {
  ExtendedStatsSelectivityRequest subset;
  subset.relation_uuid = request.relation_uuid;
  subset.column_uuids = stats.column_uuids;
  subset.document_path_digests = stats.document_path_digests;
  subset.minimum_confidence = request.minimum_confidence;
  subset.require_fresh = request.require_fresh;
  subset.join_cardinality_request = request.join_cardinality_request;
  subset.children.reserve(request_indices.size());
  subset.value_encodings.reserve(request_indices.size());
  for (const auto index : request_indices) {
    subset.children.push_back(request_dimensions[index].child);
    subset.value_encodings.push_back(request_dimensions[index].value_encoding);
  }
  if (exact_shape && request.value_encodings.size() == request_indices.size()) {
    subset.value_encodings = request.value_encodings;
  }
  return subset;
}

SelectivityEstimate FallbackEstimate(const ExtendedStatsSelectivityRequest& request,
                                      std::string diagnostic_code) {
  return Make(IndependentChildSelectivity(request.children),
              ChildConfidence(request.children),
              std::move(diagnostic_code),
              ChildConservative(request.children));
}

double ExtendedSelectivity(const ExtendedStatsSelectivityRequest& request,
                           const ExtendedOptimizerStatistic& stats) {
  switch (stats.kind) {
    case ExtendedOptimizerStatisticKind::kJointMcv:
      for (const auto& entry : stats.joint_mcv) {
        if (JointMcvValuesMatch(request, entry)) {
          return entry.frequency;
        }
      }
      return stats.multi_column_distinct_count == 0
                 ? IndependentChildSelectivity(request.children)
                 : 1.0 / static_cast<double>(stats.multi_column_distinct_count);
    case ExtendedOptimizerStatisticKind::kMultiColumnNdv:
      return stats.multi_column_distinct_count == 0
                 ? IndependentChildSelectivity(request.children)
                 : 1.0 / static_cast<double>(stats.multi_column_distinct_count);
    case ExtendedOptimizerStatisticKind::kFunctionalDependency: {
      const double strongest = request.children.empty()
                                   ? 1.0
                                   : std::max_element(request.children.begin(),
                                                      request.children.end(),
                                                      [](const auto& left, const auto& right) {
                                                        return left.selectivity < right.selectivity;
                                                      })
                                         ->selectivity;
      const double independent = IndependentChildSelectivity(request.children);
      return (Clamp(stats.functional_dependency_strength, 0.0, 1.0) * strongest) +
             ((1.0 - Clamp(stats.functional_dependency_strength, 0.0, 1.0)) *
              independent);
    }
    case ExtendedOptimizerStatisticKind::kCrossColumnCorrelation: {
      const double strongest = request.children.empty()
                                   ? 1.0
                                   : std::max_element(request.children.begin(),
                                                      request.children.end(),
                                                      [](const auto& left, const auto& right) {
                                                        return left.selectivity < right.selectivity;
                                                      })
                                         ->selectivity;
      const double independent = IndependentChildSelectivity(request.children);
      const double correlation = std::abs(Clamp(stats.correlation_coefficient, -1.0, 1.0));
      return (correlation * strongest) + ((1.0 - correlation) * independent);
    }
    case ExtendedOptimizerStatisticKind::kMultiColumnHistogram:
      return stats.histogram_selectivity;
    case ExtendedOptimizerStatisticKind::kSampledDependency:
      return stats.sampled_dependency_selectivity;
    case ExtendedOptimizerStatisticKind::kFkPkJoinCardinality:
      return request.join_cardinality_request && stats.fk_pk_shortcut &&
                     stats.fk_pk_estimated_rows != 0
                 ? 1.0
                 : IndependentChildSelectivity(request.children);
    case ExtendedOptimizerStatisticKind::kDocumentPathBridge:
      if (stats.sampled_dependency_selectivity > 0.0) return stats.sampled_dependency_selectivity;
      if (stats.histogram_selectivity > 0.0) return stats.histogram_selectivity;
      return stats.multi_column_distinct_count == 0
                 ? IndependentChildSelectivity(request.children)
                 : 1.0 / static_cast<double>(stats.multi_column_distinct_count);
  }
  return IndependentChildSelectivity(request.children);
}

struct ExtendedStatsCandidate {
  const ExtendedOptimizerStatistic* stats = nullptr;
  ExtendedStatsSelectivityRequest subset_request;
  std::vector<bool> covered_dimensions;
  std::size_t coverage_count = 0;
  int family_rank = 0;
  int confidence_rank = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t catalog_epoch = 0;
  double observed_selectivity_error = -1.0;
  bool exact_shape = false;
  bool exact_joint_mcv = false;
};

// SEARCH_KEY: ORH_EXTENDED_STATS_RANKED_SELECTION
int FamilyRank(const ExtendedStatsSelectivityRequest& subset_request,
               const ExtendedOptimizerStatistic& stats,
               bool exact_joint_mcv) {
  if (stats.kind == ExtendedOptimizerStatisticKind::kFkPkJoinCardinality &&
      subset_request.join_cardinality_request && stats.fk_pk_shortcut &&
      stats.fk_pk_estimated_rows != 0) {
    return 800;
  }
  switch (stats.kind) {
    case ExtendedOptimizerStatisticKind::kJointMcv:
      return exact_joint_mcv ? 700 : 200;
    case ExtendedOptimizerStatisticKind::kMultiColumnHistogram:
      return 600;
    case ExtendedOptimizerStatisticKind::kSampledDependency:
      return 500;
    case ExtendedOptimizerStatisticKind::kFunctionalDependency:
      return 400;
    case ExtendedOptimizerStatisticKind::kCrossColumnCorrelation:
      return 300;
    case ExtendedOptimizerStatisticKind::kMultiColumnNdv:
      return 200;
    case ExtendedOptimizerStatisticKind::kDocumentPathBridge:
      return 100;
    case ExtendedOptimizerStatisticKind::kFkPkJoinCardinality:
      return 50;
  }
  return 0;
}

bool ObservedErrorIsBetter(double left, double right) {
  if (left >= 0.0 && right >= 0.0) return left < right;
  return left >= 0.0 && right < 0.0;
}

bool CandidateBetter(const ExtendedStatsCandidate& left,
                     const ExtendedStatsCandidate& right) {
  if (left.family_rank != right.family_rank) return left.family_rank > right.family_rank;
  if (left.confidence_rank != right.confidence_rank) {
    return left.confidence_rank > right.confidence_rank;
  }
  if (left.stats_epoch != right.stats_epoch) return left.stats_epoch > right.stats_epoch;
  if (left.catalog_epoch != right.catalog_epoch) return left.catalog_epoch > right.catalog_epoch;
  if (left.coverage_count != right.coverage_count) {
    return left.coverage_count > right.coverage_count;
  }
  if (left.observed_selectivity_error != right.observed_selectivity_error) {
    return ObservedErrorIsBetter(left.observed_selectivity_error,
                                 right.observed_selectivity_error);
  }
  const auto left_uuid = left.stats == nullptr ? std::string{} : left.stats->identity.statistic_uuid;
  const auto right_uuid = right.stats == nullptr ? std::string{} : right.stats->identity.statistic_uuid;
  return left_uuid < right_uuid;
}

// SEARCH_KEY: ORH_EXTENDED_STATS_PARTIAL_SHAPE_COMPOSITION
bool CandidateOverlaps(const ExtendedStatsCandidate& candidate,
                       const std::vector<bool>& already_covered) {
  for (std::size_t i = 0; i < candidate.covered_dimensions.size() &&
                          i < already_covered.size(); ++i) {
    if (candidate.covered_dimensions[i] && already_covered[i]) return true;
  }
  return false;
}

void MarkCandidateCovered(const ExtendedStatsCandidate& candidate,
                          std::vector<bool>* already_covered) {
  for (std::size_t i = 0; i < candidate.covered_dimensions.size() &&
                          i < already_covered->size(); ++i) {
    if (candidate.covered_dimensions[i]) (*already_covered)[i] = true;
  }
}

std::size_t CoveredCount(const std::vector<bool>& covered) {
  return static_cast<std::size_t>(
      std::count(covered.begin(), covered.end(), true));
}

SelectivityEstimate CombinedOr(const PredicateSelectivityInput& input) {
  if (input.children.empty()) {
    return Make(0.5, CostConfidence::kUnknown, "SB_OPTIMIZER_SELECTIVITY.OR_EMPTY", true);
  }
  double complement = 1.0;
  CostConfidence confidence = CostConfidence::kExact;
  bool conservative = false;
  for (const auto& child : input.children) {
    complement *= (1.0 - Clamp(child.selectivity, 0.0, 1.0));
    confidence = LowerConfidence(confidence, child.confidence);
    conservative = conservative || child.conservative;
  }
  return Make(1.0 - complement, confidence, "SB_OPTIMIZER_SELECTIVITY.OR_INCLUSION_EXCLUSION", conservative);
}

SelectivityEstimate CombinedNot(const PredicateSelectivityInput& input) {
  if (input.children.empty()) {
    return Make(0.5, CostConfidence::kUnknown, "SB_OPTIMIZER_SELECTIVITY.NOT_EMPTY", true);
  }
  const auto& child = input.children.front();
  return Make(1.0 - Clamp(child.selectivity, 0.0, 1.0),
              child.confidence,
              "SB_OPTIMIZER_SELECTIVITY.NOT_COMPLEMENT",
              child.conservative);
}

}  // namespace

SelectivityEstimate EstimatePredicateSelectivity(const PredicateSelectivityInput& input) {
  if (input.predicate_kind == "row_uuid_eq" || input.predicate_kind == "unique_eq") {
    return OneRowEstimate(input.input_rows, "SB_OPTIMIZER_SELECTIVITY.UNIQUE_ONE_ROW");
  }
  if (input.predicate_kind == "scalar_eq") {
    return ScalarEqualityEstimate(input);
  }
  if (input.predicate_kind == "scalar_range") {
    return Make(input.has_histogram ? input.range_fraction : 0.33,
                ConfidenceFromStats(false, input.has_histogram, input.input_confidence),
                input.has_histogram ? "SB_OPTIMIZER_SELECTIVITY.HISTOGRAM_RANGE"
                                    : "SB_OPTIMIZER_SELECTIVITY.DEFAULT_RANGE",
                !input.has_histogram);
  }
  if (input.predicate_kind == "in_list") {
    if (input.in_list_count == 0) {
      return Make(0.0, CostConfidence::kExact, "SB_OPTIMIZER_SELECTIVITY.EMPTY_IN", false);
    }
    auto eq_input = input;
    eq_input.predicate_kind = "scalar_eq";
    eq_input.has_mcv = false;
    eq_input.has_mcv_frequency = false;
    const auto eq = ScalarEqualityEstimate(eq_input);
    return Make(eq.selectivity * static_cast<double>(input.in_list_count),
                eq.confidence,
                eq.conservative ? "SB_OPTIMIZER_SELECTIVITY.DEFAULT_IN"
                                : "SB_OPTIMIZER_SELECTIVITY.STATS_IN",
                eq.conservative);
  }
  if (input.predicate_kind == "like") {
    if (input.has_histogram) {
      return Make(input.range_fraction,
                  ConfidenceFromStats(false, true, input.input_confidence),
                  "SB_OPTIMIZER_SELECTIVITY.HISTOGRAM_LIKE_PREFIX",
                  false);
    }
    if (input.like_has_fixed_prefix) {
      return Make(input.like_prefix_fraction > 0.0 ? input.like_prefix_fraction : 0.05,
                  ConfidenceFromStats(false, true, input.input_confidence),
                  "SB_OPTIMIZER_SELECTIVITY.PREFIX_LIKE",
                  false);
    }
    return Make(0.25,
                ConfidenceFromStats(false, false, input.input_confidence),
                "SB_OPTIMIZER_SELECTIVITY.DEFAULT_LIKE",
                true);
  }
  if (input.predicate_kind == "and") {
    return CombinedAnd(input);
  }
  if (input.predicate_kind == "or") {
    return CombinedOr(input);
  }
  if (input.predicate_kind == "not") {
    return CombinedNot(input);
  }
  if (input.predicate_kind == "join_eq") {
    const auto max_rows = std::max(input.left_rows, input.right_rows);
    if (input.left_unique && input.right_unique && max_rows != 0) {
      return Make(1.0 / static_cast<double>(max_rows),
                  ConfidenceFromStats(false, true, input.input_confidence),
                  "SB_OPTIMIZER_SELECTIVITY.JOIN_UNIQUE_EQ",
                  false);
    }
    const auto max_ndv = std::max(input.left_distinct_values, input.right_distinct_values);
    if (max_ndv != 0) {
      return Make(1.0 / static_cast<double>(max_ndv),
                  ConfidenceFromStats(false, true, input.input_confidence),
                  "SB_OPTIMIZER_SELECTIVITY.JOIN_NDV_EQ",
                  false);
    }
    return Make(0.10,
                ConfidenceFromStats(false, false, input.input_confidence),
                "SB_OPTIMIZER_SELECTIVITY.DEFAULT_JOIN_EQ",
                true);
  }
  if (input.predicate_kind == "is_null") {
    return Make(input.null_fraction,
                ConfidenceFromStats(false, input.input_confidence != CostConfidence::kUnknown, input.input_confidence),
                "SB_OPTIMIZER_SELECTIVITY.NULL_FRACTION",
                input.input_confidence == CostConfidence::kUnknown);
  }
  return Make(0.5, CostConfidence::kUnknown, "SB_OPTIMIZER_SELECTIVITY.UNKNOWN_CONSERVATIVE", true);
}

std::uint64_t EstimateRowsAfterSelectivity(std::uint64_t input_rows, const SelectivityEstimate& estimate) {
  if (estimate.exact_rows_known) return std::min(input_rows, estimate.exact_rows);
  if (input_rows == 0 || estimate.selectivity <= 0.0) return 0;
  const double rows = std::ceil((static_cast<double>(input_rows) * Clamp(estimate.selectivity, 0.0, 1.0)) - 1e-9);
  return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(rows));
}

ExtendedStatsSelectivityResult EstimateCorrelatedConjunctionSelectivity(
    const ExtendedStatsSelectivityRequest& request,
    const std::vector<ExtendedOptimizerStatistic>& extended_stats) {
  ExtendedStatsSelectivityResult result;
  result.estimate = FallbackEstimate(request, "SB_OPTIMIZER_EXTENDED_STATS.FALLBACK_MISSING");
  result.diagnostic_code = result.estimate.diagnostic_code;
  result.evidence.push_back("extended_stats_independent_scalar_fallback=true");

  if (request.relation_uuid.empty() ||
      (request.column_uuids.empty() && request.document_path_digests.empty()) ||
      request.children.empty()) {
    result.estimate = FallbackEstimate(request,
                                       "SB_OPTIMIZER_EXTENDED_STATS.FALLBACK_SHAPE_MISMATCH");
    result.diagnostic_code = result.estimate.diagnostic_code;
    result.evidence.push_back("extended_stats_shape_match=false");
    return result;
  }

  const auto request_dimensions = RequestDimensions(request);
  if (request_dimensions.empty() || request.children.size() < request_dimensions.size()) {
    result.estimate = FallbackEstimate(request,
                                       "SB_OPTIMIZER_EXTENDED_STATS.FALLBACK_SHAPE_MISMATCH");
    result.diagnostic_code = result.estimate.diagnostic_code;
    result.evidence.push_back("extended_stats_shape_match=false");
    return result;
  }

  bool relation_seen = false;
  bool shape_mismatch = false;
  bool stale = false;
  bool unusable = false;
  bool low_confidence = false;
  std::vector<ExtendedStatsCandidate> candidates;
  for (const auto& stats : extended_stats) {
    if (stats.relation_uuid != request.relation_uuid &&
        stats.identity.object_uuid != request.relation_uuid) {
      continue;
    }
    relation_seen = true;
    std::vector<std::size_t> request_indices;
    if (!FindStatisticShapeInRequest(request_dimensions, stats, &request_indices)) {
      shape_mismatch = true;
      continue;
    }
    if (request.require_fresh &&
        stats.identity.freshness != OptimizerStatsFreshnessState::kFresh) {
      stale = true;
      continue;
    }
    if (!OptimizerStatsIdentityIsUsable(stats.identity) ||
        stats.identity.source == StatisticSource::kUnavailable ||
        stats.finality_authority ||
        !stats.mga_visibility_recheck_required ||
        !stats.security_recheck_required) {
      unusable = true;
      continue;
    }
    if (!ConfidenceAtLeast(stats.identity.confidence, request.minimum_confidence)) {
      low_confidence = true;
      continue;
    }
    ExtendedStatsCandidate candidate;
    candidate.stats = &stats;
    candidate.coverage_count = request_indices.size();
    candidate.covered_dimensions.assign(request_dimensions.size(), false);
    for (const auto index : request_indices) candidate.covered_dimensions[index] = true;
    candidate.exact_shape = ExactShapeMatch(request_dimensions, stats);
    candidate.subset_request = SubsetRequest(request,
                                             request_dimensions,
                                             stats,
                                             request_indices,
                                             candidate.exact_shape);
    candidate.exact_joint_mcv = JointMcvExactValueMatch(candidate.subset_request, stats);
    candidate.family_rank = FamilyRank(candidate.subset_request, stats, candidate.exact_joint_mcv);
    candidate.confidence_rank = ConfidenceRank(stats.identity.confidence);
    candidate.stats_epoch = stats.identity.stats_epoch;
    candidate.catalog_epoch = stats.identity.catalog_epoch;
    candidate.observed_selectivity_error = stats.observed_selectivity_error;
    candidates.push_back(std::move(candidate));
  }

  if (candidates.empty()) {
    std::string code = "SB_OPTIMIZER_EXTENDED_STATS.FALLBACK_MISSING";
    if (stale) code = "SB_OPTIMIZER_EXTENDED_STATS.FALLBACK_STALE";
    else if (low_confidence) code = "SB_OPTIMIZER_EXTENDED_STATS.FALLBACK_LOW_CONFIDENCE";
    else if (unusable) code = "SB_OPTIMIZER_EXTENDED_STATS.FALLBACK_UNUSABLE";
    else if (relation_seen && shape_mismatch) code = "SB_OPTIMIZER_EXTENDED_STATS.FALLBACK_SHAPE_MISMATCH";
    result.estimate = FallbackEstimate(request, code);
    result.diagnostic_code = code;
    result.evidence.push_back("extended_stats_used=false");
    return result;
  }

  std::sort(candidates.begin(), candidates.end(), CandidateBetter);

  std::vector<ExtendedStatsCandidate> selected_candidates;
  std::vector<bool> covered(request_dimensions.size(), false);
  for (const auto& candidate : candidates) {
    if (CandidateOverlaps(candidate, covered)) continue;
    selected_candidates.push_back(candidate);
    MarkCandidateCovered(candidate, &covered);
    if (CoveredCount(covered) == covered.size()) break;
  }

  if (selected_candidates.empty()) {
    result.estimate = FallbackEstimate(request,
                                       "SB_OPTIMIZER_EXTENDED_STATS.FALLBACK_SHAPE_MISMATCH");
    result.diagnostic_code = result.estimate.diagnostic_code;
    result.evidence.push_back("extended_stats_used=false");
    return result;
  }

  double selectivity = 1.0;
  CostConfidence confidence = CostConfidence::kExact;
  bool conservative = false;
  for (const auto& candidate : selected_candidates) {
    selectivity *= Clamp(ExtendedSelectivity(candidate.subset_request, *candidate.stats), 0.0, 1.0);
    confidence = LowerConfidence(confidence, candidate.stats->identity.confidence);
  }
  for (std::size_t i = 0; i < request_dimensions.size(); ++i) {
    if (covered[i]) continue;
    selectivity *= Clamp(request_dimensions[i].child.selectivity, 0.0, 1.0);
    confidence = LowerConfidence(confidence, request_dimensions[i].child.confidence);
    conservative = conservative || request_dimensions[i].child.conservative;
  }

  const auto& selected = *selected_candidates.front().stats;
  result.estimate = Make(selectivity,
                         confidence,
                         "SB_OPTIMIZER_EXTENDED_STATS.USED",
                         conservative);
  if (selected.kind == ExtendedOptimizerStatisticKind::kFkPkJoinCardinality &&
      request.join_cardinality_request && selected.fk_pk_estimated_rows != 0 &&
      CoveredCount(covered) == covered.size()) {
    result.estimate.exact_rows_known = true;
    result.estimate.exact_rows = selected.fk_pk_estimated_rows;
  }
  result.used_extended_stats = true;
  result.used_kind = selected.kind;
  result.diagnostic_code = "SB_OPTIMIZER_EXTENDED_STATS.USED";
  result.mga_visibility_recheck_required = selected.mga_visibility_recheck_required;
  result.security_recheck_required = selected.security_recheck_required;
  result.finality_authority = false;
  const auto covered_count = CoveredCount(covered);
  const bool partial_shape_composition =
      selected_candidates.size() > 1 ||
      !std::all_of(selected_candidates.begin(),
                   selected_candidates.end(),
                   [](const ExtendedStatsCandidate& candidate) {
                     return candidate.exact_shape;
                   }) ||
      covered_count < request_dimensions.size();
  result.evidence = {"extended_stats_used=true",
                     std::string("extended_stats_kind=") +
                         ExtendedOptimizerStatisticKindName(selected.kind),
                     "mga_visibility_recheck_required=true",
                     "security_recheck_required=true",
                     "finality_authority=false",
                     std::string("extended_stats_selected_count=") +
                         std::to_string(selected_candidates.size()),
                     std::string("extended_stats_covered_predicates=") +
                         std::to_string(covered_count),
                     std::string("extended_stats_remaining_scalar_predicates=") +
                         std::to_string(request_dimensions.size() - covered_count),
                     std::string("extended_stats_partial_shape_composition=") +
                         (partial_shape_composition ? "true" : "false")};
  for (const auto& candidate : selected_candidates) {
    result.evidence.push_back(std::string("extended_stats_selected_kind=") +
                              ExtendedOptimizerStatisticKindName(candidate.stats->kind));
    result.evidence.push_back(std::string("extended_stats_selected_uuid=") +
                              candidate.stats->identity.statistic_uuid);
  }
  return result;
}

std::uint64_t EstimateJoinRowsAfterSelectivity(std::uint64_t left_rows,
                                               std::uint64_t right_rows,
                                               const SelectivityEstimate& estimate) {
  if (left_rows == 0 || right_rows == 0 || estimate.selectivity <= 0.0) return 0;
  if (estimate.exact_rows_known) return estimate.exact_rows;
  const auto pair_count = SaturatingMultiply(left_rows, right_rows);
  const double rows = std::ceil((static_cast<double>(pair_count) * Clamp(estimate.selectivity, 0.0, 1.0)) - 1e-9);
  return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(rows));
}

}  // namespace scratchbird::engine::optimizer
