// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "specialized_planner.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OEIC_SPECIALIZED_FUSION_ENTERPRISE
enum class EnterpriseFusionFamily {
  kDocument,
  kSearch,
  kVector,
  kGraph,
  kCandidateSet,
};

struct EnterpriseFusionFamilyMetric {
  EnterpriseFusionFamily family = EnterpriseFusionFamily::kDocument;
  std::string metric_snapshot_id;
  std::string route_label;
  std::string provider_id;
  std::string plan_node_id;
  std::string result_contract_hash;
  std::string evidence_digest;
  std::uint64_t generation = 0;
  std::uint64_t route_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t estimated_rows = 0;
  std::uint64_t candidate_rows = 0;
  std::uint64_t exact_recheck_rows = 0;
  std::uint64_t cost_units = 0;
  double selectivity = 1.0;
  double false_positive_ratio = 0.0;
  bool route_consumed = false;
  bool fresh = false;
  bool trusted = false;
  bool exact_recheck_required = true;
  bool exact_recheck_available = true;
  bool exact_rerank_required = false;
  bool exact_rerank_available = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool descriptor_scan_fallback = false;
  bool behavior_store_scan_fallback = false;
  bool parser_or_reference_authority = false;
  bool client_authority = false;
  bool provider_finality_or_visibility_authority = false;
  bool metric_finality_or_visibility_authority = false;
  bool recovery_or_wal_authority = false;
  bool cluster_route_or_metric_projection = false;
};

struct EnterpriseSpecializedFusionRequest {
  std::string route_label;
  std::string sql_result_hash;
  std::string fusion_result_hash;
  bool sql_route_consumed = false;
  std::vector<EnterpriseFusionFamilyMetric> family_metrics;
  std::vector<PlanCandidate> candidates;
};

struct EnterpriseSpecializedFusionDecision {
  bool ok = false;
  bool fail_closed = true;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
  std::vector<PlanCandidate> selected_candidates;
  std::uint64_t fused_estimated_rows = 0;
  std::uint64_t fused_candidate_rows = 0;
  std::uint64_t fused_exact_recheck_rows = 0;
  std::uint64_t fused_cost_units = 0;
};

const char* EnterpriseFusionFamilyName(EnterpriseFusionFamily family);

EnterpriseSpecializedFusionDecision PlanEnterpriseSpecializedFusion(
    const EnterpriseSpecializedFusionRequest& request);

}  // namespace scratchbird::engine::optimizer
