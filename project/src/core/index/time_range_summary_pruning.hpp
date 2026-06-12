// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_TIME_RANGE_SUMMARY_PRUNING
#include "index_optimizer_integration.hpp"
#include "page_extent_summary.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kTimeRangeSummaryPruningSearchKey =
    "DPC_TIME_RANGE_SUMMARY_PRUNING";
inline constexpr const char* kTimeRangeSummaryArtifactKind =
    "dpc_time_range_summary";
inline constexpr const char* kTimeRangeSummaryAuthoritySource =
    "engine_mga_base_pages";

enum class TimeRangeSummaryFallbackReason : u32 {
  none = 1,
  disabled_summary_exact_fallback = 2,
  missing_summary_exact_fallback = 3,
  stale_summary_exact_fallback = 4,
  corrupt_summary_exact_fallback = 5,
  incompatible_summary_exact_fallback = 6,
  non_authoritative_summary_exact_fallback = 7,
  invalid_identity_exact_fallback = 8
};

struct TimeRangeSummaryDescriptor {
  std::string table_uuid;
  std::string index_uuid;
  std::string range_family_uuid;
  std::string summary_uuid;
  PageExtentSummaryRange range;
  std::string time_scalar_type_key;
  std::string encoded_min_time;
  std::string encoded_max_time;
  bool min_time_present = false;
  bool max_time_present = false;
  bool min_inclusive = true;
  bool max_inclusive = true;
  u64 row_count = 0;
  u64 null_count = 0;
  bool nulls_present = false;
  PageExtentSummaryStatus status = PageExtentSummaryStatus::missing;
  PageExtentSummaryFormatVersion format_version;
  u64 generation = 0;
  bool persisted_record_present = false;
  bool checksum_valid = true;
  std::string authority_source = kTimeRangeSummaryAuthoritySource;
  bool parser_finality_authority_claimed = false;
  bool client_finality_authority_claimed = false;
  bool timestamp_finality_authority_claimed = false;
  bool uuid_ordering_finality_authority_claimed = false;
  bool event_stream_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
};

struct TimeRangeSummaryPredicate {
  std::string time_scalar_type_key;
  std::string encoded_lower_time;
  std::string encoded_upper_time;
  bool lower_present = false;
  bool upper_present = false;
  bool lower_inclusive = true;
  bool upper_inclusive = true;
};

struct TimeRangeSummaryPruneCounters {
  u64 prune_candidates = 0;
  u64 ranges_pruned = 0;
  u64 ranges_scanned = 0;
  u64 pages_considered = 0;
  u64 pages_pruned = 0;
  u64 pages_scanned = 0;
};

struct TimeRangeSummaryRangeEvidence {
  PageExtentSummaryRange range;
  u64 generation = 0;
  std::string summary_status = "missing";
  std::string decision = "scan";
};

struct TimeRangeSummaryPruneRequest {
  std::vector<TimeRangeSummaryDescriptor> summaries;
  PageExtentSummaryFormatCompatibility format;
  TimeRangeSummaryPredicate predicate;
  bool time_range_prune_enabled = true;
  bool base_row_mga_recheck_required = true;
  bool base_row_security_recheck_required = true;
};

struct TimeRangeSummaryPrunePlan {
  Status status;
  DiagnosticRecord diagnostic;
  IndexPlanCategory selected_category = IndexPlanCategory::fallback_full_scan;
  std::string selected_access = "full_scan";
  std::string prune_reason = "none";
  std::string fallback_reason = "none";
  std::string summary_status = "missing";
  u64 summary_generation = 0;
  std::string authority_source = kTimeRangeSummaryAuthoritySource;
  TimeRangeSummaryPruneCounters counters;
  bool summary_prune_selected = false;
  bool exact_fallback_required = true;
  bool base_row_mga_recheck_required = true;
  bool base_row_security_recheck_required = true;
  bool summary_metadata_visibility_authority = false;
  bool summary_metadata_finality_authority = false;
  std::vector<TimeRangeSummaryRangeEvidence> range_evidence;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && summary_prune_selected; }
};

struct TimeRangeSummaryResultEqualityEvidence {
  u64 baseline_row_count = 0;
  u64 planned_row_count = 0;
  std::string baseline_result_hash;
  std::string planned_result_hash;
  bool exact_match = false;
  std::vector<std::string> deterministic_row_ids;
};

bool TimeRangeSummaryDescriptorIdentityValid(
    const TimeRangeSummaryDescriptor& descriptor);
bool TimeRangeSummaryDescriptorAuthorityClean(
    const TimeRangeSummaryDescriptor& descriptor);
bool TimeRangeSummaryDescriptorBoundsValid(
    const TimeRangeSummaryDescriptor& descriptor);

TimeRangeSummaryPrunePlan PlanTimeRangeSummaryPrune(
    const TimeRangeSummaryPruneRequest& request);
TimeRangeSummaryResultEqualityEvidence BuildTimeRangeSummaryResultEqualityEvidence(
    const std::vector<std::string>& baseline_row_ids,
    const std::vector<std::string>& planned_row_ids);

const char* TimeRangeSummaryFallbackReasonName(
    TimeRangeSummaryFallbackReason reason);

}  // namespace scratchbird::core::index
