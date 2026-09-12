// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_statistics_full.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OEIC_STATISTICS_INVALIDATION_ENTERPRISE
// Enterprise stats invalidation is cache/pinned-descriptor hygiene only. It
// refuses unsafe authority and never changes row visibility, transaction
// finality, parser behavior, reference behavior, or recovery outcome.
struct OptimizerStatisticsInvalidationAuthority {
  bool engine_runtime_scope = false;
  bool optimizer_cache_owner = false;
  bool catalog_generation_authority = false;
  bool security_generation_authority = false;
  bool redaction_generation_authority = false;
  bool metric_generation_authority = false;
  bool analyze_generation_authority = false;
  bool index_generation_authority = false;

  bool parser_or_reference_authority = false;
  bool client_finality_authority = false;
  bool client_visibility_authority = false;
  bool metric_finality_authority = false;
  bool metric_visibility_authority = false;
  bool external_recovery_authority = false;
  bool cluster_authority = false;
  bool fixture_or_synthetic_source = false;
};

struct OptimizerStatisticsInvalidationRequest {
  OptimizerStatisticsInvalidationKind kind =
      OptimizerStatisticsInvalidationKind::kStatsRefresh;
  OptimizerStatisticsInvalidationAuthority authority;
  planner::CanonicalPlannerUuid object_uuid;
  planner::CanonicalPlannerUuid index_uuid;
  planner::CanonicalPlannerUuid filespace_uuid;
  planner::CanonicalPlannerUuid security_policy_identity;
  planner::CanonicalPlannerUuid redaction_policy_identity;
  std::string evidence_digest;
  std::string reason;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::uint64_t name_resolution_epoch = 0;
  std::uint64_t metric_generation = 0;
  std::uint64_t analyze_generation = 0;
  std::uint64_t index_generation = 0;
};

enum class OptimizerStatisticsInvalidationFailure {
  kNone, kInvalidRequest, kResourceExhausted, kInternalFailure,
};

struct OptimizerStatisticsInvalidationResult {
  OptimizerStatisticsInvalidationFailure failure{
      OptimizerStatisticsInvalidationFailure::kNone};
  bool accepted = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
  std::vector<OptimizerPinnedStatsInvalidatedEntry> invalidated_entries;
};

struct OptimizerPinnedStatsBenchmarkCleanRequest {
  OptimizerPinnedStatsDescriptorSnapshot snapshot;
  std::uint64_t required_catalog_epoch = 0;
  std::uint64_t required_security_epoch = 0;
  std::uint64_t required_resource_policy_epoch = 0;
  std::uint64_t required_name_resolution_epoch = 0;
  std::uint64_t required_stats_epoch = 0;
  std::string required_descriptor_set_digest;
  planner::CanonicalPlannerUuid required_security_policy_identity;
  planner::CanonicalPlannerUuid required_redaction_policy_identity;
  std::vector<planner::CanonicalPlannerUuid> required_object_uuids;
  std::vector<planner::CanonicalPlannerUuid> required_index_uuids;
};

const char* OptimizerStatisticsInvalidationKindName(
    OptimizerStatisticsInvalidationKind kind);

OptimizerStatisticsInvalidationResult DispatchOptimizerStatisticsInvalidation(
    const OptimizerStatisticsInvalidationRequest& request,
    OptimizerPinnedStatsDescriptorCache* cache);

std::vector<StatisticsContractStatus> ValidatePinnedStatsForBenchmarkCleanAdmission(
    const OptimizerPinnedStatsBenchmarkCleanRequest& request);

}  // namespace scratchbird::engine::optimizer
