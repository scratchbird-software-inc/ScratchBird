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

// SEARCH_KEY: OEIC_VECTOR_ANN_RECALL_COSTING
enum class EnterpriseVectorIndexProfile {
  kExact,
  kHnsw,
  kIvfFlat,
  kIvfPq,
  kIvfSq8,
};

struct EnterpriseVectorRuntimeMetric {
  std::string metric_snapshot_id;
  std::string route_label;
  std::string provider_id;
  std::string index_generation;
  std::string result_contract_hash;
  std::string evidence_digest;
  std::uint64_t generation = 0;
  std::uint64_t route_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t index_generation_epoch = 0;
  std::uint64_t vector_count = 0;
  std::uint64_t dimensions = 0;
  std::uint64_t top_k = 0;
  std::uint64_t candidate_rows = 0;
  std::uint64_t exact_rerank_rows = 0;
  std::uint64_t tombstone_rows = 0;
  std::uint64_t ef_search = 0;
  std::uint64_t nprobe = 0;
  double recall_observed = 1.0;
  double metadata_prefilter_selectivity = 1.0;
  double list_imbalance_ratio = 0.0;
  double quantization_error_ratio = 0.0;
  bool fresh = false;
  bool trusted = false;
  bool exact_payload_available = false;
  bool exact_rerank_available = false;
  bool exact_fallback_available = false;
  bool metadata_prefilter_available = false;
  bool parser_or_reference_authority = false;
  bool client_authority = false;
  bool metric_finality_or_visibility_authority = false;
  bool provider_finality_or_visibility_authority = false;
  bool recovery_or_wal_authority = false;
  bool cluster_route_or_metric_projection = false;
};

struct EnterpriseVectorAnnCostingRequest {
  EnterpriseVectorIndexProfile profile = EnterpriseVectorIndexProfile::kExact;
  IndexStats index;
  TableCardinalityStats table;
  OptimizerCostEnvironment environment;
  EnterpriseIndexCostAuthority authority;
  EnterpriseVectorRuntimeMetric metric;
  double min_recall = 0.95;
  double max_quantization_error_ratio = 0.20;
  double max_list_imbalance_ratio = 0.75;
  double max_tombstone_ratio = 0.20;
};

struct EnterpriseVectorAnnCostingResult {
  bool accepted = false;
  bool selectable = false;
  bool exact_fallback_selected = false;
  bool rebuild_recommended = false;
  std::string diagnostic_code;
  EnterpriseVectorIndexProfile profile = EnterpriseVectorIndexProfile::kExact;
  std::uint64_t estimated_candidate_rows = 0;
  std::uint64_t exact_rerank_rows = 0;
  std::uint64_t tombstone_rows = 0;
  CostVector cost;
  std::vector<std::string> evidence;
};

const char* EnterpriseVectorIndexProfileName(EnterpriseVectorIndexProfile profile);

EnterpriseVectorAnnCostingResult EstimateEnterpriseVectorAnnCost(
    const EnterpriseVectorAnnCostingRequest& request);

}  // namespace scratchbird::engine::optimizer
