// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// IRC-090 physical BRIN/zone-map summary storage.
#include "page_extent_summary.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr const char* kPhysicalZoneSummaryArtifactKind =
    "physical_zone_summary_page";
inline constexpr const char* kPhysicalZoneSummaryFormatSearchKey =
    "IRC_090_PHYSICAL_ZONE_SUMMARY_FORMAT";
inline constexpr u32 kPhysicalZoneSummaryCurrentMajor = 1;
inline constexpr u32 kPhysicalZoneSummaryCurrentMinor = 0;
inline constexpr u32 kPhysicalZoneSummaryDefaultSmallSetLimit = 8;

enum class PhysicalZoneSummaryOpenClass : u32 {
  current = 1,
  stale_format = 2,
  stale_generation = 3,
  bad_checksum = 4,
  corrupt_payload = 5,
  refused = 6
};

enum class PhysicalZoneSummaryRangeDecision : u32 {
  scan_with_recheck = 1,
  pruned_by_min_max = 2,
  pruned_by_small_set = 3,
  full_scan_fallback = 4
};

enum class PhysicalZoneSummaryMutationKind : u32 {
  append_row = 1,
  update_row = 2,
  delete_row = 3
};

struct PhysicalZoneRangeSizingMetadata {
  u32 min_pages_per_range = 1;
  u32 target_pages_per_range = 1;
  u32 max_pages_per_range = 1;
  u64 base_generation = 0;
  u64 summary_generation = 0;
  bool adaptive = false;
};

struct PhysicalZoneColumnValueEvidence {
  u32 column_ordinal = 0;
  std::string scalar_type_key;
  std::string encoded_scalar;
  bool value_is_null = false;
};

struct PhysicalZoneRowEvidence {
  u64 page_id = 0;
  u64 extent_id = 0;
  u64 base_generation = 0;
  bool engine_mga_visible = true;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  std::vector<PhysicalZoneColumnValueEvidence> columns;
};

struct PhysicalZoneColumnSummary {
  u32 column_ordinal = 0;
  std::string scalar_type_key;
  u64 row_count = 0;
  u64 null_count = 0;
  bool min_present = false;
  bool max_present = false;
  std::string encoded_min;
  std::string encoded_max;
  bool small_set_exact = true;
  bool small_set_overflow = false;
  std::vector<std::string> small_set_values;
};

struct PhysicalZoneMultiColumnSummary {
  std::vector<u32> column_ordinals;
  u64 row_count = 0;
  u64 null_tuple_count = 0;
  bool min_present = false;
  bool max_present = false;
  std::string encoded_min_tuple;
  std::string encoded_max_tuple;
  bool small_set_exact = true;
  bool small_set_overflow = false;
  std::vector<std::string> small_set_values;
};

struct PhysicalZoneRangeSummaryRecord {
  PageExtentSummaryRange range;
  PhysicalZoneRangeSizingMetadata range_sizing;
  u64 row_count = 0;
  u64 base_generation = 0;
  u64 summary_generation = 0;
  PageExtentSummaryStatus status = PageExtentSummaryStatus::missing;
  bool persisted_record_present = false;
  bool checksum_valid = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  std::vector<PhysicalZoneColumnSummary> columns;
  std::vector<PhysicalZoneMultiColumnSummary> multi_columns;
};

struct PhysicalZoneSummaryPage {
  std::string relation_uuid;
  std::string summary_uuid;
  PageExtentSummaryFormatVersion format_version{
      kPhysicalZoneSummaryCurrentMajor, kPhysicalZoneSummaryCurrentMinor};
  u64 base_generation = 0;
  u64 summary_generation = 0;
  PhysicalZoneRangeSizingMetadata range_sizing;
  std::vector<PhysicalZoneRangeSummaryRecord> ranges;
  std::vector<std::string> evidence;
};

struct PhysicalZoneSummaryBuildRequest {
  std::string relation_uuid;
  std::string summary_uuid;
  PhysicalZoneRangeSizingMetadata range_sizing;
  u32 small_set_limit = kPhysicalZoneSummaryDefaultSmallSetLimit;
  std::vector<PhysicalZoneRowEvidence> base_page_rows;
};

struct PhysicalZoneSummaryBuildResult {
  Status status;
  DiagnosticRecord diagnostic;
  PhysicalZoneSummaryPage page;
  bool built = false;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && built; }
};

struct PhysicalZoneSummarySerializeResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<byte> bytes;
  u64 checksum = 0;

  bool ok() const { return status.ok() && !bytes.empty(); }
};

struct PhysicalZoneSummaryOpenRequest {
  std::vector<byte> bytes;
  bool expected_base_generation_present = false;
  u64 expected_base_generation = 0;
};

struct PhysicalZoneSummaryOpenResult {
  Status status;
  DiagnosticRecord diagnostic;
  PhysicalZoneSummaryOpenClass open_class =
      PhysicalZoneSummaryOpenClass::refused;
  PhysicalZoneSummaryPage page;
  bool full_scan_required = true;
  bool restricted_repair_required = false;
  std::vector<std::string> actions;

  bool ok() const {
    return status.ok() && open_class == PhysicalZoneSummaryOpenClass::current;
  }
};

struct PhysicalZonePredicate {
  u32 column_ordinal = 0;
  std::string scalar_type_key;
  bool lower_present = false;
  bool upper_present = false;
  std::string encoded_lower;
  std::string encoded_upper;
};

struct PhysicalZoneRangePruneEvidence {
  PageExtentSummaryRange range;
  PhysicalZoneSummaryRangeDecision decision =
      PhysicalZoneSummaryRangeDecision::full_scan_fallback;
  bool scan_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  std::string reason;
};

struct PhysicalZonePruneRequest {
  PhysicalZoneSummaryPage page;
  std::vector<PhysicalZonePredicate> predicates;
};

struct PhysicalZonePruneResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool any_pruned = false;
  bool full_scan_required = false;
  std::vector<PhysicalZoneRangePruneEvidence> ranges;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !full_scan_required; }
};

struct PhysicalZoneSummaryMutation {
  PhysicalZoneSummaryMutationKind kind = PhysicalZoneSummaryMutationKind::append_row;
  bool before_row_present = false;
  PhysicalZoneRowEvidence before_row;
  bool after_row_present = false;
  PhysicalZoneRowEvidence after_row;
  bool rebuild_admitted = false;
  std::vector<PhysicalZoneRowEvidence> authoritative_base_page_rows;
};

struct PhysicalZoneSummaryMutationResult {
  Status status;
  DiagnosticRecord diagnostic;
  PhysicalZoneSummaryPage page;
  bool applied = false;
  bool summary_invalidated = false;
  bool rebuild_performed = false;
  bool full_scan_required = false;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && applied; }
};

PhysicalZoneSummaryBuildResult BuildPhysicalZoneSummaryFromBasePageEvidence(
    const PhysicalZoneSummaryBuildRequest& request);

PhysicalZoneSummarySerializeResult SerializePhysicalZoneSummaryPage(
    const PhysicalZoneSummaryPage& page);

PhysicalZoneSummaryOpenResult OpenPhysicalZoneSummaryPage(
    const PhysicalZoneSummaryOpenRequest& request);

PhysicalZonePruneResult PrunePhysicalZoneSummaryRanges(
    const PhysicalZonePruneRequest& request);

PhysicalZoneSummaryMutationResult ApplyPhysicalZoneSummaryMutation(
    const PhysicalZoneSummaryPage& page,
    const PhysicalZoneSummaryMutation& mutation,
    u32 small_set_limit = kPhysicalZoneSummaryDefaultSmallSetLimit);

PhysicalZoneSummaryMutationResult RepairPhysicalZoneSummaryFromBasePageEvidence(
    const PhysicalZoneSummaryPage& stale_or_corrupt_page,
    const std::vector<PhysicalZoneRowEvidence>& authoritative_base_page_rows,
    bool repair_admitted,
    u32 small_set_limit = kPhysicalZoneSummaryDefaultSmallSetLimit);

const char* PhysicalZoneSummaryOpenClassName(PhysicalZoneSummaryOpenClass open_class);
const char* PhysicalZoneSummaryRangeDecisionName(
    PhysicalZoneSummaryRangeDecision decision);

DiagnosticRecord MakePhysicalZoneSummaryDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {});

}  // namespace scratchbird::core::index
