// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_index_costing.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OEIC_TEXT_WAND_RANKING_COSTING
enum class EnterpriseTextSearchProfile {
  kFullText,
  kInverted,
  kGin,
  kNgram,
  kSparseWand,
};

struct EnterpriseTextSearchMetric {
  std::string metric_snapshot_id;
  std::string route_label;
  std::string analyzer_id;
  std::string analyzer_epoch;
  std::string index_generation;
  std::string result_contract_hash;
  std::string evidence_digest;
  std::uint64_t generation = 0;
  std::uint64_t route_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t corpus_docs = 0;
  std::uint64_t query_terms = 0;
  std::uint64_t posting_length = 0;
  std::uint64_t phrase_position_hits = 0;
  std::uint64_t blockmax_skips = 0;
  std::uint64_t impact_ordered_postings = 0;
  std::uint64_t candidate_rows = 0;
  std::uint64_t exact_rerank_rows = 0;
  std::uint64_t top_k = 0;
  double bm25_selectivity = 1.0;
  double false_positive_ratio = 0.0;
  bool fresh = false;
  bool trusted = false;
  bool phrase_position_proof_present = false;
  bool exact_recheck_available = false;
  bool exact_rerank_available = false;
  bool exact_fallback_available = false;
  bool wand_topk_exact_equivalence = false;
  bool parser_or_reference_authority = false;
  bool client_authority = false;
  bool metric_finality_or_visibility_authority = false;
  bool provider_finality_or_visibility_authority = false;
  bool recovery_or_wal_authority = false;
  bool cluster_route_or_metric_projection = false;
};

struct EnterpriseTextWandCostingRequest {
  EnterpriseTextSearchProfile profile = EnterpriseTextSearchProfile::kFullText;
  IndexStats index;
  TableCardinalityStats table;
  OptimizerCostEnvironment environment;
  EnterpriseIndexCostAuthority authority;
  EnterpriseTextSearchMetric metric;
  double max_false_positive_ratio = 0.35;
};

struct EnterpriseTextWandCostingResult {
  bool accepted = false;
  bool selectable = false;
  bool exact_fallback_selected = false;
  std::string diagnostic_code;
  EnterpriseTextSearchProfile profile = EnterpriseTextSearchProfile::kFullText;
  std::uint64_t candidate_rows = 0;
  std::uint64_t exact_rerank_rows = 0;
  CostVector cost;
  std::vector<std::string> evidence;
};

const char* EnterpriseTextSearchProfileName(EnterpriseTextSearchProfile profile);

EnterpriseTextWandCostingResult EstimateEnterpriseTextWandCost(
    const EnterpriseTextWandCostingRequest& request);

}  // namespace scratchbird::engine::optimizer
