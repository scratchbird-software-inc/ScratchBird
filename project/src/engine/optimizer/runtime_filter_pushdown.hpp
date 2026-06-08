// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "nosql/nosql_physical_provider_contract.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_RUNTIME_FILTER_PUSHDOWN_CONTRACT_ODF_095
// Runtime filters are candidate-pruning metadata only. They never own parser
// execution, row visibility, authorization, transaction finality, or recovery.

enum class RuntimeFilterFamily {
  kUnknown,
  kJoin,
  kGraph,
  kSearch,
  kVector,
  kTimeSeries,
  kCandidateSet,
};

enum class RuntimeFilterRoute {
  kUnknown,
  kScan,
  kProvider,
};

struct RuntimeFilterDescriptor {
  RuntimeFilterFamily family = RuntimeFilterFamily::kUnknown;
  RuntimeFilterRoute route = RuntimeFilterRoute::kUnknown;
  scratchbird::engine::internal_api::EngineNoSqlProviderFamily provider_family =
      scratchbird::engine::internal_api::EngineNoSqlProviderFamily::kUnknown;

  std::string filter_id;
  std::string plan_node_id;
  std::string provider_id;
  std::string predicate_digest;

  std::uint64_t descriptor_generation = 0;
  std::uint64_t required_descriptor_generation = 0;
  std::uint64_t input_rows = 0;
  std::uint64_t estimated_candidate_rows = 0;
  std::uint64_t estimated_pruned_rows = 0;
  std::uint64_t baseline_cost_units = 0;
  std::uint64_t filter_cost_units = 0;
  std::uint64_t exact_recheck_cost_units = 0;

  bool plan_shape_supported = false;
  bool provider_supports_runtime_filters = false;
  bool candidate_set_available = false;
  bool security_context_present = false;
  bool security_snapshot_bound = false;
  bool grants_proven = false;
  bool engine_mga_authoritative = false;
  bool exact_recheck_available = false;
  bool exact_fallback_available = false;
  bool mga_visibility_recheck_required = true;
  bool security_authorization_recheck_required = true;
  bool descriptor_scan_selected = false;
  bool behavior_store_scan_selected = false;
  bool stale = false;
  bool lossy_or_false_negative_possible = false;

  bool parser_or_donor_finality_or_visibility_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool provider_finality_or_visibility_authority = false;
  bool write_ahead_log_finality_or_visibility_authority = false;
};

struct RuntimeFilterPushdownRequest {
  std::string plan_id;
  std::vector<RuntimeFilterDescriptor> candidates;
  std::uint64_t min_net_benefit_units = 1;
};

struct RuntimeFilterCandidateDecision {
  RuntimeFilterDescriptor descriptor;
  std::uint64_t net_benefit_units = 0;
  bool selected = false;
  bool fallback_selected = false;
  bool refused = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

struct RuntimeFilterPushdownDecision {
  bool ok = false;
  bool fail_closed = true;
  std::string diagnostic_code;
  std::vector<RuntimeFilterCandidateDecision> candidate_decisions;
  std::vector<RuntimeFilterDescriptor> selected_filters;
  std::vector<std::string> evidence;
};

const char* RuntimeFilterFamilyName(RuntimeFilterFamily family);
const char* RuntimeFilterRouteName(RuntimeFilterRoute route);
bool RuntimeFilterFamilySupported(RuntimeFilterFamily family);
bool RuntimeFilterRouteSupported(RuntimeFilterRoute route);

RuntimeFilterPushdownDecision EvaluateRuntimeFilterPushdown(
    const RuntimeFilterPushdownRequest& request);

std::string SerializeRuntimeFilterPushdownEvidence(
    const RuntimeFilterPushdownDecision& decision);

}  // namespace scratchbird::engine::optimizer
