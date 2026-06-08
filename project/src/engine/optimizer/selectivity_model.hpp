// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_statistics_full.hpp"
#include "statistics_catalog.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_SELECTIVITY_MODEL
struct SelectivityEstimate {
  double selectivity = 1.0;
  CostConfidence confidence = CostConfidence::kUnknown;
  std::string diagnostic_code;
  bool conservative = true;
  bool exact_rows_known = false;
  std::uint64_t exact_rows = 0;
};

struct ExtendedStatsSelectivityRequest {
  std::string relation_uuid;
  std::vector<std::string> column_uuids;
  std::vector<std::string> document_path_digests;
  std::vector<std::string> value_encodings;
  std::vector<SelectivityEstimate> children;
  CostConfidence minimum_confidence = CostConfidence::kMedium;
  bool require_fresh = true;
  bool join_cardinality_request = false;
};

struct ExtendedStatsSelectivityResult {
  SelectivityEstimate estimate;
  bool used_extended_stats = false;
  ExtendedOptimizerStatisticKind used_kind = ExtendedOptimizerStatisticKind::kMultiColumnNdv;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
  bool mga_visibility_recheck_required = true;
  bool security_recheck_required = true;
  bool finality_authority = false;
};

struct PredicateSelectivityInput {
  std::string predicate_kind;
  std::uint64_t input_rows = 0;
  bool has_histogram = false;
  bool has_mcv = false;
  bool has_mcv_frequency = false;
  std::uint64_t distinct_values = 0;
  std::uint64_t in_list_count = 0;
  double null_fraction = 0.0;
  double range_fraction = 0.25;
  double mcv_frequency = 0.0;
  bool like_has_fixed_prefix = false;
  double like_prefix_fraction = 0.0;
  std::uint64_t left_rows = 0;
  std::uint64_t right_rows = 0;
  std::uint64_t left_distinct_values = 0;
  std::uint64_t right_distinct_values = 0;
  bool left_unique = false;
  bool right_unique = false;
  CostConfidence input_confidence = CostConfidence::kUnknown;
  std::vector<SelectivityEstimate> children;
};

SelectivityEstimate EstimatePredicateSelectivity(const PredicateSelectivityInput& input);
ExtendedStatsSelectivityResult EstimateCorrelatedConjunctionSelectivity(
    const ExtendedStatsSelectivityRequest& request,
    const std::vector<ExtendedOptimizerStatistic>& extended_stats);
std::uint64_t EstimateRowsAfterSelectivity(std::uint64_t input_rows, const SelectivityEstimate& estimate);
std::uint64_t EstimateJoinRowsAfterSelectivity(std::uint64_t left_rows,
                                               std::uint64_t right_rows,
                                               const SelectivityEstimate& estimate);

}  // namespace scratchbird::engine::optimizer
