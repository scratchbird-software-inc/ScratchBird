// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_PAGE_EXTENT_SUMMARY_METADATA
// DPC_PAGE_EXTENT_SUMMARY_MAINTENANCE
#include "index_family_registry.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr const char* kPageExtentSummaryMetadataSearchKey =
    "DPC_PAGE_EXTENT_SUMMARY_METADATA";
inline constexpr const char* kPageExtentSummaryMaintenanceSearchKey =
    "DPC_PAGE_EXTENT_SUMMARY_MAINTENANCE";
inline constexpr const char* kPageExtentSummaryArtifactKind =
    "dpc_page_extent_summary";
inline constexpr const char* kPageExtentSummaryFeatureMapKey =
    "optimizer.persisted.page_extent_summaries";
inline constexpr const char* kPageExtentSummarySupportBundleField =
    "persisted_feature_map";

struct PageExtentSummaryFormatVersion {
  u32 major = 0;
  u32 minor = 0;
};

struct PageExtentSummaryFormatContract {
  std::string artifact_kind;
  std::string feature_map_key;
  std::string support_bundle_field;
  PageExtentSummaryFormatVersion min_supported;
  PageExtentSummaryFormatVersion current;
  PageExtentSummaryFormatVersion max_supported;
};

enum class PageExtentSummaryRangeKind : u32 {
  page_range = 1,
  extent_range = 2
};

struct PageExtentSummaryRange {
  PageExtentSummaryRangeKind kind = PageExtentSummaryRangeKind::page_range;
  u64 first_page_id = 0;
  u32 page_count = 0;
  u64 first_extent_id = 0;
  u32 extent_count = 0;
};

struct PageExtentSummaryScalarBoundary {
  std::string scalar_type_key;
  std::string encoded_min;
  std::string encoded_max;
  bool min_present = false;
  bool max_present = false;
  bool min_inclusive = true;
  bool max_inclusive = true;
};

enum class PageExtentSummaryStatus : u32 {
  missing = 1,
  current = 2,
  stale = 3,
  corrupt = 4,
  incompatible_format = 5
};

enum class PageExtentSummaryFormatOpenClass : u32 {
  current = 1,
  supported_migration = 2,
  refused = 3
};

enum class PageExtentSummaryUseClass : u32 {
  use_summary = 1,
  full_scan_fallback = 2,
  restricted_repair = 3,
  refused = 4
};

enum class PageExtentSummaryRebuildClassification : u32 {
  none = 1,
  transient_rebuild_allowed = 2,
  transient_rebuild_refused = 3,
  persisted_repair_required = 4,
  restricted_repair_required = 5
};

enum class PageExtentSummaryFallbackReason : u32 {
  none = 1,
  stale_summary_full_scan = 2,
  missing_summary_full_scan = 3,
  corrupt_summary_full_scan = 4,
  incompatible_summary_full_scan = 5,
  restricted_repair_required = 6,
  invalid_identity_full_scan = 7,
  external_finality_authority_full_scan = 8
};

struct PageExtentSummaryFormatCompatibility {
  PageExtentSummaryFormatVersion observed;
  PageExtentSummaryFormatOpenClass open_class =
      PageExtentSummaryFormatOpenClass::refused;
  bool compatible = false;
  bool migration_required = false;
  std::string diagnostic_code;
};

struct PageExtentSummaryMetadata {
  std::string relation_uuid;
  std::string summary_uuid;
  PageExtentSummaryRange range;
  PageExtentSummaryScalarBoundary boundary;
  u64 row_count = 0;
  u64 null_count = 0;
  bool nulls_present = false;
  PageExtentSummaryStatus status = PageExtentSummaryStatus::missing;
  PageExtentSummaryFormatVersion format_version;
  u64 generation = 0;
  PageExtentSummaryRebuildClassification rebuild_classification =
      PageExtentSummaryRebuildClassification::none;
  PageExtentSummaryFallbackReason safe_fallback_reason =
      PageExtentSummaryFallbackReason::none;
  bool persisted_record_present = false;
  bool checksum_valid = true;
  bool parser_finality_authority_claimed = false;
  bool donor_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
};

struct PageExtentSummaryDecision {
  Status status;
  DiagnosticRecord diagnostic;
  PageExtentSummaryUseClass use_class = PageExtentSummaryUseClass::refused;
  PageExtentSummaryFallbackReason fallback_reason =
      PageExtentSummaryFallbackReason::none;
  PageExtentSummaryRebuildClassification rebuild_classification =
      PageExtentSummaryRebuildClassification::none;
  bool summary_usable = false;
  bool full_scan_required = true;
  bool transient_rebuild_allowed = false;
  bool restricted_repair_required = false;
  std::vector<std::string> actions;

  bool ok() const {
    return status.ok() && use_class != PageExtentSummaryUseClass::refused;
  }
};

enum class PageExtentSummaryMaintenanceEventKind : u32 {
  insert_row = 1,
  update_row = 2,
  delete_row = 3,
  bulk_ingest = 4,
  rebuild = 5,
  repair = 6,
  crash_reopen = 7
};

struct PageExtentSummaryRowEvidence {
  u64 page_id = 0;
  u64 extent_id = 0;
  std::string scalar_type_key;
  std::string encoded_scalar;
  bool value_is_null = false;
  bool engine_mga_visible = true;
  bool parser_finality_authority_claimed = false;
  bool donor_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
};

struct PageExtentSummaryMaintenanceEvent {
  PageExtentSummaryMaintenanceEventKind kind =
      PageExtentSummaryMaintenanceEventKind::insert_row;
  std::string relation_uuid;
  std::string summary_uuid;
  bool before_row_present = false;
  PageExtentSummaryRowEvidence before_row;
  bool after_row_present = false;
  PageExtentSummaryRowEvidence after_row;
  std::vector<PageExtentSummaryRowEvidence> base_page_rows;
  bool caller_allows_transient_rebuild = true;
  bool parser_finality_authority_claimed = false;
  bool donor_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
};

struct PageExtentSummaryMaintenanceResult {
  PageExtentSummaryMetadata metadata;
  PageExtentSummaryDecision decision;
  bool applied = false;
  bool summary_invalidated = false;
  bool rebuild_performed = false;
  std::vector<std::string> actions;

  bool ok() const { return decision.ok(); }
};

PageExtentSummaryFormatContract PageExtentSummaryPersistedFormatContract();
std::string PageExtentSummaryMigrationPlanId(PageExtentSummaryFormatVersion from);

bool PageExtentSummaryUuidTextValid(std::string_view value);
bool PageExtentSummaryRangeValid(const PageExtentSummaryRange& range);
bool PageExtentSummaryBoundaryValid(const PageExtentSummaryScalarBoundary& boundary,
                                    u64 row_count,
                                    u64 null_count);
bool PageExtentSummaryMetadataIdentityValid(const PageExtentSummaryMetadata& metadata);
bool PageExtentSummaryAuthorityFlagsClean(const PageExtentSummaryMetadata& metadata);

PageExtentSummaryFormatCompatibility PageExtentSummaryFormatCompatibilityFromArtifactResult(
    PageExtentSummaryFormatVersion observed,
    bool artifact_ok,
    bool migration_required,
    std::string_view compatibility_class_name,
    std::string diagnostic_code = {});

PageExtentSummaryDecision ClassifyPageExtentSummaryForUse(
    const PageExtentSummaryMetadata& metadata,
    const PageExtentSummaryFormatCompatibility& format);
PageExtentSummaryDecision ClassifyPageExtentSummaryTransientRebuild(
    const PageExtentSummaryMetadata& metadata,
    const PageExtentSummaryFormatCompatibility& format,
    bool caller_allows_transient_rebuild);
PageExtentSummaryDecision ClassifyPageExtentSummaryRepairOrCrashReopen(
    const PageExtentSummaryMetadata& metadata,
    const PageExtentSummaryFormatCompatibility& format,
    bool caller_allows_transient_rebuild,
    bool expected_generation_present = false,
    u64 expected_generation = 0);

bool PageExtentSummaryRangeContainsRow(const PageExtentSummaryRange& range,
                                       const PageExtentSummaryRowEvidence& row);
bool PageExtentSummaryRowEvidenceAuthorityClean(
    const PageExtentSummaryRowEvidence& row);

PageExtentSummaryMaintenanceResult ApplyPageExtentSummaryMaintenanceEvent(
    const PageExtentSummaryMetadata& metadata,
    const PageExtentSummaryFormatCompatibility& format,
    const PageExtentSummaryMaintenanceEvent& event);
PageExtentSummaryMaintenanceResult RebuildPageExtentSummaryFromBasePageEvidence(
    const PageExtentSummaryMetadata& seed,
    const PageExtentSummaryFormatCompatibility& format,
    const PageExtentSummaryMaintenanceEvent& event);

const char* PageExtentSummaryStatusName(PageExtentSummaryStatus status);
const char* PageExtentSummaryMaintenanceEventKindName(
    PageExtentSummaryMaintenanceEventKind kind);
const char* PageExtentSummaryUseClassName(PageExtentSummaryUseClass use_class);
const char* PageExtentSummaryRebuildClassificationName(
    PageExtentSummaryRebuildClassification classification);
const char* PageExtentSummaryFallbackReasonName(
    PageExtentSummaryFallbackReason reason);

DiagnosticRecord MakePageExtentSummaryDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {});

}  // namespace scratchbird::core::index
