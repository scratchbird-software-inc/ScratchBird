// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: ODF030_DML_TARGET_ACCESS_PLAN_SURFACE
enum class DmlTargetAccessKind {
  refused,
  row_uuid_singleton,
  row_uuid_list,
  unique_index_lookup,
  nonunique_index_lookup,
  range_index_lookup,
  summary_pruned,
  table_scan,
};

struct DmlTargetSummaryPruneDescriptor {
  bool requested = false;
  bool summary_present = false;
  bool predicate_supported = false;
  std::uint64_t summary_generation = 0;
  std::uint64_t relation_generation = 0;
  std::uint64_t candidate_ranges = 0;
  std::uint64_t ranges_pruned = 0;
  std::uint64_t pages_considered = 0;
  std::uint64_t pages_pruned = 0;
};

struct DmlTargetAccessPlanRequest {
  std::string mutation_kind = "dml.target_rows";
  std::string database_uuid;
  std::string relation_uuid;
  std::string predicate_kind;
  std::string predicate_descriptor_digest;
  std::string row_uuid;
  std::vector<std::string> row_uuids;
  std::string index_uuid;
  std::string index_family = "btree";
  std::string security_policy_digest;
  std::string redaction_policy_digest;
  std::string access_policy_digest;
  std::string collation_profile_digest;
  bool index_unique = false;
  bool access_descriptor_present = false;
  bool explicit_table_scan_fallback = false;
  bool relation_present = true;
  bool mga_visibility_recheck_planned = true;
  bool security_recheck_planned = true;
  bool grants_proven = true;
  bool security_context_present = true;
  bool parser_or_donor_authority = false;
  std::uint64_t observed_catalog_epoch = 0;
  std::uint64_t current_catalog_epoch = 0;
  std::uint64_t observed_security_epoch = 0;
  std::uint64_t current_security_epoch = 0;
  std::uint64_t observed_policy_epoch = 0;
  std::uint64_t current_policy_epoch = 0;
  std::uint64_t observed_stats_epoch = 0;
  std::uint64_t current_stats_epoch = 0;
  std::uint64_t index_epoch = 0;
  std::uint64_t object_epoch = 0;
  std::uint64_t compatibility_epoch = 0;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t estimated_rows = 0;
  DmlTargetSummaryPruneDescriptor summary_prune;
};

struct DmlTargetAccessPlan {
  bool ok = false;
  DmlTargetAccessKind access_kind = DmlTargetAccessKind::refused;
  std::string physical_access_kind;
  std::string executor_capability;
  std::string relation_uuid;
  std::string predicate_kind;
  std::string predicate_descriptor_digest;
  std::string row_uuid;
  std::vector<std::string> row_uuids;
  std::string index_uuid;
  std::uint64_t estimated_rows = 0;
  std::vector<std::string> diagnostics;
  std::vector<std::string> evidence;
};

const char* DmlTargetAccessKindName(DmlTargetAccessKind kind);
DmlTargetAccessPlan BuildDmlTargetAccessPlan(const DmlTargetAccessPlanRequest& request);
void AdmitDmlHotPointLookupCacheSuccessfulRowLocator(
    const DmlTargetAccessPlanRequest& request,
    const std::string& actual_row_uuid,
    std::vector<std::string>* evidence);
std::string SerializeDmlTargetAccessPlanEvidence(const DmlTargetAccessPlan& plan);

}  // namespace scratchbird::engine::internal_api
