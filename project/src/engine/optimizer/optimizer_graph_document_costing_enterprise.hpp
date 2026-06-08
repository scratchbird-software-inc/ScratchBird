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

// SEARCH_KEY: OEIC_GRAPH_DOCUMENT_ROUTE_COSTING
enum class EnterpriseGraphDocumentProfile {
  kDocumentPath,
  kGraphSeed,
};

struct EnterpriseGraphDocumentMetric {
  std::string metric_snapshot_id;
  std::string route_label;
  std::string provider_id;
  std::string index_generation;
  std::string result_contract_hash;
  std::string evidence_digest;
  std::uint64_t generation = 0;
  std::uint64_t route_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t candidate_rows = 0;
  std::uint64_t exact_recheck_rows = 0;
  std::uint64_t document_shape_count = 0;
  std::uint64_t document_array_expansion_rows = 0;
  std::uint64_t document_wildcard_fanout = 0;
  std::uint64_t graph_frontier_width = 0;
  std::uint64_t graph_adjacency_degree = 0;
  std::uint64_t graph_label_selectivity_ppm = 0;
  std::uint64_t graph_property_selectivity_ppm = 0;
  std::uint64_t graph_visited_bitmap_density_ppm = 0;
  double document_path_selectivity = 1.0;
  double document_shape_selectivity = 1.0;
  double false_positive_ratio = 0.0;
  bool fresh = false;
  bool trusted = false;
  bool exact_recheck_available = false;
  bool path_wildcard_proof_present = false;
  bool array_expansion_proof_present = false;
  bool graph_frontier_proof_present = false;
  bool graph_adjacency_proof_present = false;
  bool parser_or_donor_authority = false;
  bool client_authority = false;
  bool metric_finality_or_visibility_authority = false;
  bool provider_finality_or_visibility_authority = false;
  bool recovery_or_wal_authority = false;
  bool cluster_route_or_metric_projection = false;
};

struct EnterpriseGraphDocumentCostingRequest {
  EnterpriseGraphDocumentProfile profile = EnterpriseGraphDocumentProfile::kDocumentPath;
  IndexStats index;
  TableCardinalityStats table;
  OptimizerCostEnvironment environment;
  EnterpriseIndexCostAuthority authority;
  EnterpriseGraphDocumentMetric metric;
  double max_false_positive_ratio = 0.50;
};

struct EnterpriseGraphDocumentCostingResult {
  bool accepted = false;
  bool selectable = false;
  bool exact_fallback_required = false;
  std::string diagnostic_code;
  EnterpriseGraphDocumentProfile profile = EnterpriseGraphDocumentProfile::kDocumentPath;
  std::uint64_t candidate_rows = 0;
  std::uint64_t exact_recheck_rows = 0;
  CostVector cost;
  std::vector<std::string> evidence;
};

const char* EnterpriseGraphDocumentProfileName(EnterpriseGraphDocumentProfile profile);

EnterpriseGraphDocumentCostingResult EstimateEnterpriseGraphDocumentCost(
    const EnterpriseGraphDocumentCostingRequest& request);

}  // namespace scratchbird::engine::optimizer
