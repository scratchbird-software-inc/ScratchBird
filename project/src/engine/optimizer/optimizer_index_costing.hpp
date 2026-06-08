// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_cost_full.hpp"
#include "optimizer_statistics_full.hpp"
#include "selectivity_model.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OEIC_INDEX_COSTING_ENTERPRISE_CLOSURE
// Enterprise index costing consumes maintained stats and route capabilities. It
// never treats an index, donor, metric, or benchmark as row truth, transaction
// finality, visibility, security, authorization, or recovery authority.
enum class EnterpriseIndexAccessIntent {
  kEqualityLookup,
  kOrderedRange,
  kOrderedScan,
  kNegativePrune,
  kCandidateSet,
  kRankedSearch,
  kVectorSearch,
  kSpatialProbe,
  kDocumentProbe,
  kGraphSeed,
};

struct EnterpriseIndexCostAuthority {
  bool optimizer_scope = false;
  bool catalog_descriptor_authority = false;
  bool index_stats_authority = false;
  bool route_capability_authority = false;
  bool runtime_metric_authority = false;
  bool generated_index_readiness_manifest = false;
  bool readiness_manifest_current = false;
  bool route_runtime_proof = false;
  bool operation_metric_producer_proof = false;
  bool support_bundle_producer_proof = false;
  bool crash_cleanup_corruption_proof = false;
  bool storage_integration_proof = false;
  bool exact_recheck_preserved = false;
  bool mga_recheck_preserved = false;
  bool security_recheck_preserved = false;
  bool exact_rerank_proven = false;
  bool static_registry_only = false;
  bool smoke_only = false;
  bool stale_manifest = false;
  bool synthetic_or_fixture_evidence = false;

  bool parser_or_donor_authority = false;
  bool client_finality_authority = false;
  bool client_visibility_authority = false;
  bool metric_finality_authority = false;
  bool metric_visibility_authority = false;
  bool external_recovery_authority = false;
  bool cluster_authority = false;
  bool external_cluster_overclaim = false;
  bool benchmark_authority = false;
};

struct EnterpriseIndexCostRequest {
  IndexStats index;
  TableCardinalityStats table;
  OptimizerCostEnvironment environment;
  EnterpriseIndexAccessIntent intent = EnterpriseIndexAccessIntent::kEqualityLookup;
  EnterpriseIndexCostAuthority authority;
  std::uint64_t requested_limit = 0;
  double requested_range_fraction = 0.0;
  bool require_benchmark_clean = true;
};

struct EnterpriseIndexCostResult {
  bool accepted = false;
  bool selectable = false;
  std::string diagnostic_code;
  std::string family_id;
  EnterpriseIndexAccessIntent intent = EnterpriseIndexAccessIntent::kEqualityLookup;
  SelectivityEstimate selectivity;
  std::uint64_t estimated_rows = 0;
  std::uint64_t recheck_rows = 0;
  std::uint64_t false_positive_rows = 0;
  CostVector cost;
  std::vector<std::string> evidence;
};

const char* EnterpriseIndexAccessIntentName(EnterpriseIndexAccessIntent intent);

EnterpriseIndexCostResult EstimateEnterpriseIndexAccessCost(
    const EnterpriseIndexCostRequest& request);

std::vector<StatisticsContractStatus> ValidateEnterpriseIndexCostingFamilyMatrix();

}  // namespace scratchbird::engine::optimizer
