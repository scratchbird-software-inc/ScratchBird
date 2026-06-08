// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_extent_summary.hpp"

// DPC_PAGE_EXTENT_SUMMARY_MAINTENANCE

#include "index_key_encoding.hpp"

#include <cctype>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status WarnStatus() { return {StatusCode::ok, Severity::warning, Subsystem::engine}; }

bool IsHex(char value) {
  return std::isxdigit(static_cast<unsigned char>(value)) != 0;
}

bool SameFormatVersion(PageExtentSummaryFormatVersion left,
                       PageExtentSummaryFormatVersion right) {
  return left.major == right.major && left.minor == right.minor;
}

bool IsCompatibleCurrent(const PageExtentSummaryFormatCompatibility& format) {
  return format.compatible &&
         format.open_class == PageExtentSummaryFormatOpenClass::current &&
         !format.migration_required;
}

PageExtentSummaryDecision Decision(Status status,
                                   PageExtentSummaryUseClass use_class,
                                   PageExtentSummaryFallbackReason fallback_reason,
                                   PageExtentSummaryRebuildClassification rebuild_classification,
                                   std::string code,
                                   std::string key,
                                   std::string detail = {}) {
  PageExtentSummaryDecision decision;
  decision.status = status;
  decision.use_class = use_class;
  decision.fallback_reason = fallback_reason;
  decision.rebuild_classification = rebuild_classification;
  decision.summary_usable = use_class == PageExtentSummaryUseClass::use_summary;
  decision.full_scan_required = use_class != PageExtentSummaryUseClass::use_summary;
  decision.transient_rebuild_allowed =
      rebuild_classification ==
      PageExtentSummaryRebuildClassification::transient_rebuild_allowed;
  decision.restricted_repair_required =
      use_class == PageExtentSummaryUseClass::restricted_repair ||
      rebuild_classification ==
          PageExtentSummaryRebuildClassification::restricted_repair_required ||
      rebuild_classification ==
          PageExtentSummaryRebuildClassification::persisted_repair_required;
  decision.diagnostic = MakePageExtentSummaryDiagnostic(
      decision.status, std::move(code), std::move(key), std::move(detail));
  return decision;
}

PageExtentSummaryDecision Fallback(PageExtentSummaryFallbackReason reason,
                                   PageExtentSummaryRebuildClassification rebuild,
                                   std::string code,
                                   std::string key,
                                   std::string detail = {}) {
  auto decision = Decision(WarnStatus(),
                           rebuild == PageExtentSummaryRebuildClassification::restricted_repair_required ||
                                   rebuild == PageExtentSummaryRebuildClassification::persisted_repair_required
                               ? PageExtentSummaryUseClass::restricted_repair
                               : PageExtentSummaryUseClass::full_scan_fallback,
                           reason,
                           rebuild,
                           std::move(code),
                           std::move(key),
                           std::move(detail));
  decision.actions.push_back("select_full_scan_fallback");
  return decision;
}

bool CountsValid(const PageExtentSummaryMetadata& metadata) {
  if (metadata.null_count > metadata.row_count) {
    return false;
  }
  if (metadata.null_count > 0 && !metadata.nulls_present) {
    return false;
  }
  return true;
}

bool HasNonNullRows(u64 row_count, u64 null_count) {
  return row_count > 0 && null_count < row_count;
}

u64 NextGeneration(u64 generation) {
  return generation == 0 ? 1 : generation + 1;
}

bool ScalarEvidenceValid(const PageExtentSummaryRowEvidence& row) {
  return row.value_is_null ||
         (!row.scalar_type_key.empty() && !row.encoded_scalar.empty() &&
          !IsUnsafeLegacyIndexKeyEncoding(row.encoded_scalar));
}

bool EncodedLess(std::string_view left, std::string_view right) {
  if (IsOrderPreservingIndexKeyEncoding(left) &&
      IsOrderPreservingIndexKeyEncoding(right)) {
    const auto compare = CompareEncodedIndexKeyBytes(left, right);
    return compare.ok() && compare.comparison < 0;
  }
  return left < right;
}

bool EncodedGreater(std::string_view left, std::string_view right) {
  if (IsOrderPreservingIndexKeyEncoding(left) &&
      IsOrderPreservingIndexKeyEncoding(right)) {
    const auto compare = CompareEncodedIndexKeyBytes(left, right);
    return compare.ok() && compare.comparison > 0;
  }
  return left > right;
}

bool EventAuthorityClean(const PageExtentSummaryMaintenanceEvent& event) {
  return !event.parser_finality_authority_claimed &&
         !event.donor_finality_authority_claimed &&
         !event.write_ahead_log_finality_authority_claimed;
}

void TaintMetadataFromRowAuthority(PageExtentSummaryMetadata& metadata,
                                   const PageExtentSummaryRowEvidence& row) {
  metadata.parser_finality_authority_claimed =
      metadata.parser_finality_authority_claimed ||
      row.parser_finality_authority_claimed;
  metadata.donor_finality_authority_claimed =
      metadata.donor_finality_authority_claimed ||
      row.donor_finality_authority_claimed;
  metadata.write_ahead_log_finality_authority_claimed =
      metadata.write_ahead_log_finality_authority_claimed ||
      row.write_ahead_log_finality_authority_claimed;
}

bool SameIdentity(const PageExtentSummaryMetadata& metadata,
                  const PageExtentSummaryMaintenanceEvent& event) {
  return metadata.relation_uuid == event.relation_uuid &&
         metadata.summary_uuid == event.summary_uuid;
}

bool SameScalarTypeOrUnset(const PageExtentSummaryMetadata& metadata,
                           const PageExtentSummaryRowEvidence& row) {
  return row.value_is_null || metadata.boundary.scalar_type_key.empty() ||
         metadata.boundary.scalar_type_key == row.scalar_type_key;
}

bool RowEqualsMin(const PageExtentSummaryMetadata& metadata,
                  const PageExtentSummaryRowEvidence& row) {
  return !row.value_is_null && metadata.boundary.min_present &&
         row.encoded_scalar == metadata.boundary.encoded_min;
}

bool RowEqualsMax(const PageExtentSummaryMetadata& metadata,
                  const PageExtentSummaryRowEvidence& row) {
  return !row.value_is_null && metadata.boundary.max_present &&
         row.encoded_scalar == metadata.boundary.encoded_max;
}

void ClearBoundary(PageExtentSummaryMetadata& metadata) {
  metadata.boundary.encoded_min.clear();
  metadata.boundary.encoded_max.clear();
  metadata.boundary.min_present = false;
  metadata.boundary.max_present = false;
}

void ExpandBoundary(PageExtentSummaryMetadata& metadata,
                    const PageExtentSummaryRowEvidence& row) {
  if (row.value_is_null) {
    return;
  }
  if (metadata.boundary.scalar_type_key.empty()) {
    metadata.boundary.scalar_type_key = row.scalar_type_key;
  }
  if (!metadata.boundary.min_present ||
      EncodedLess(row.encoded_scalar, metadata.boundary.encoded_min)) {
    metadata.boundary.encoded_min = row.encoded_scalar;
    metadata.boundary.min_present = true;
  }
  if (!metadata.boundary.max_present ||
      EncodedGreater(row.encoded_scalar, metadata.boundary.encoded_max)) {
    metadata.boundary.encoded_max = row.encoded_scalar;
    metadata.boundary.max_present = true;
  }
}

PageExtentSummaryMaintenanceResult MaintenanceResult(
    PageExtentSummaryMetadata metadata,
    PageExtentSummaryDecision decision,
    bool applied,
    bool invalidated,
    bool rebuilt) {
  PageExtentSummaryMaintenanceResult result;
  result.metadata = std::move(metadata);
  result.decision = std::move(decision);
  result.applied = applied;
  result.summary_invalidated = invalidated;
  result.rebuild_performed = rebuilt;
  result.actions = result.decision.actions;
  return result;
}

PageExtentSummaryMaintenanceResult MaintenanceFallback(
    PageExtentSummaryMetadata metadata,
    PageExtentSummaryFallbackReason reason,
    PageExtentSummaryRebuildClassification rebuild,
    std::string code,
    std::string key,
    std::string action) {
  auto decision = Fallback(reason, rebuild, std::move(code), std::move(key));
  decision.actions.push_back(std::move(action));
  return MaintenanceResult(std::move(metadata), std::move(decision),
                           false, false, false);
}

PageExtentSummaryMaintenanceResult InvalidateForRebuild(
    PageExtentSummaryMetadata metadata,
    const PageExtentSummaryFormatCompatibility& format,
    bool caller_allows_transient_rebuild,
    std::string action) {
  metadata.status = PageExtentSummaryStatus::stale;
  metadata.safe_fallback_reason =
      PageExtentSummaryFallbackReason::stale_summary_full_scan;
  metadata.rebuild_classification =
      PageExtentSummaryRebuildClassification::transient_rebuild_allowed;
  metadata.generation = NextGeneration(metadata.generation);
  auto decision = ClassifyPageExtentSummaryTransientRebuild(
      metadata, format, caller_allows_transient_rebuild);
  decision.actions.push_back(std::move(action));
  return MaintenanceResult(std::move(metadata), std::move(decision),
                           true, true, false);
}

PageExtentSummaryMaintenanceResult NoOpForUnaffectedRange(
    PageExtentSummaryMetadata metadata,
    const PageExtentSummaryFormatCompatibility& format,
    std::string action) {
  auto decision = ClassifyPageExtentSummaryForUse(metadata, format);
  decision.actions.push_back(std::move(action));
  return MaintenanceResult(std::move(metadata), std::move(decision),
                           false, false, false);
}

bool ApplyInsertRow(PageExtentSummaryMetadata& metadata,
                    const PageExtentSummaryRowEvidence& row) {
  if (!SameScalarTypeOrUnset(metadata, row)) {
    return false;
  }
  ++metadata.row_count;
  if (row.value_is_null) {
    ++metadata.null_count;
    metadata.nulls_present = true;
  } else {
    ExpandBoundary(metadata, row);
  }
  metadata.status = PageExtentSummaryStatus::current;
  metadata.safe_fallback_reason = PageExtentSummaryFallbackReason::none;
  metadata.rebuild_classification = PageExtentSummaryRebuildClassification::none;
  metadata.generation = NextGeneration(metadata.generation);
  return true;
}

bool ApplyDeleteRow(PageExtentSummaryMetadata& metadata,
                    const PageExtentSummaryRowEvidence& row) {
  if (metadata.row_count == 0 || !SameScalarTypeOrUnset(metadata, row)) {
    return false;
  }
  const u64 old_non_null_count = metadata.row_count - metadata.null_count;
  if (row.value_is_null && metadata.null_count == 0) {
    return false;
  }
  if (!row.value_is_null && old_non_null_count == 0) {
    return false;
  }
  const bool boundary_removed = RowEqualsMin(metadata, row) ||
                                RowEqualsMax(metadata, row);
  if (!row.value_is_null && boundary_removed && old_non_null_count > 1) {
    return false;
  }

  --metadata.row_count;
  if (row.value_is_null) {
    --metadata.null_count;
    metadata.nulls_present = metadata.null_count > 0;
  } else if (old_non_null_count <= 1) {
    ClearBoundary(metadata);
  }
  if (metadata.row_count == 0) {
    metadata.null_count = 0;
    metadata.nulls_present = false;
    ClearBoundary(metadata);
  }
  metadata.status = PageExtentSummaryStatus::current;
  metadata.safe_fallback_reason = PageExtentSummaryFallbackReason::none;
  metadata.rebuild_classification = PageExtentSummaryRebuildClassification::none;
  metadata.generation = NextGeneration(metadata.generation);
  return true;
}

bool ApplyUpdateRows(PageExtentSummaryMetadata& metadata,
                     const PageExtentSummaryRowEvidence& before,
                     const PageExtentSummaryRowEvidence& after) {
  const bool before_in_range = before.engine_mga_visible &&
      PageExtentSummaryRangeContainsRow(metadata.range, before);
  const bool after_in_range = after.engine_mga_visible &&
      PageExtentSummaryRangeContainsRow(metadata.range, after);
  if (!before_in_range && !after_in_range) {
    return true;
  }
  if (before_in_range && !after_in_range) {
    return ApplyDeleteRow(metadata, before);
  }
  if (!before_in_range && after_in_range) {
    return ApplyInsertRow(metadata, after);
  }
  if (!SameScalarTypeOrUnset(metadata, before) ||
      !SameScalarTypeOrUnset(metadata, after)) {
    return false;
  }

  const u64 old_non_null_count = metadata.row_count - metadata.null_count;
  const bool removes_min = RowEqualsMin(metadata, before) &&
                           (after.value_is_null ||
                            EncodedGreater(after.encoded_scalar,
                                           metadata.boundary.encoded_min));
  const bool removes_max = RowEqualsMax(metadata, before) &&
                           (after.value_is_null ||
                            EncodedLess(after.encoded_scalar,
                                        metadata.boundary.encoded_max));
  if ((removes_min || removes_max) && old_non_null_count > 1) {
    return false;
  }

  if (before.value_is_null && !after.value_is_null) {
    if (metadata.null_count == 0) {
      return false;
    }
    --metadata.null_count;
    metadata.nulls_present = metadata.null_count > 0;
  } else if (!before.value_is_null && after.value_is_null) {
    ++metadata.null_count;
    metadata.nulls_present = true;
  }

  if (!after.value_is_null) {
    if (old_non_null_count <= 1 && !before.value_is_null) {
      metadata.boundary.scalar_type_key = after.scalar_type_key;
      metadata.boundary.encoded_min = after.encoded_scalar;
      metadata.boundary.encoded_max = after.encoded_scalar;
      metadata.boundary.min_present = true;
      metadata.boundary.max_present = true;
    } else {
      ExpandBoundary(metadata, after);
    }
  } else if (old_non_null_count <= 1 && !before.value_is_null) {
    ClearBoundary(metadata);
  }

  metadata.status = PageExtentSummaryStatus::current;
  metadata.safe_fallback_reason = PageExtentSummaryFallbackReason::none;
  metadata.rebuild_classification = PageExtentSummaryRebuildClassification::none;
  metadata.generation = NextGeneration(metadata.generation);
  return true;
}

}  // namespace

PageExtentSummaryFormatContract PageExtentSummaryPersistedFormatContract() {
  PageExtentSummaryFormatContract contract;
  contract.artifact_kind = kPageExtentSummaryArtifactKind;
  contract.feature_map_key = kPageExtentSummaryFeatureMapKey;
  contract.support_bundle_field = kPageExtentSummarySupportBundleField;
  contract.min_supported = {1, 0};
  contract.current = {2, 0};
  contract.max_supported = {3, 0};
  return contract;
}

std::string PageExtentSummaryMigrationPlanId(PageExtentSummaryFormatVersion from) {
  const auto contract = PageExtentSummaryPersistedFormatContract();
  return contract.artifact_kind + "_v" + std::to_string(from.major) + "_" +
         std::to_string(from.minor) + "_to_v" +
         std::to_string(contract.current.major) + "_" +
         std::to_string(contract.current.minor) + "_explicit_plan_v1";
}

bool PageExtentSummaryUuidTextValid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const bool dash_slot =
        index == 8 || index == 13 || index == 18 || index == 23;
    if (dash_slot) {
      if (value[index] != '-') {
        return false;
      }
    } else if (!IsHex(value[index])) {
      return false;
    }
  }
  return value != "00000000-0000-0000-0000-000000000000";
}

bool PageExtentSummaryRangeValid(const PageExtentSummaryRange& range) {
  switch (range.kind) {
    case PageExtentSummaryRangeKind::page_range:
      return range.page_count > 0;
    case PageExtentSummaryRangeKind::extent_range:
      return range.extent_count > 0;
  }
  return false;
}

bool PageExtentSummaryBoundaryValid(const PageExtentSummaryScalarBoundary& boundary,
                                    u64 row_count,
                                    u64 null_count) {
  if (!HasNonNullRows(row_count, null_count)) {
    return !boundary.min_present && !boundary.max_present &&
           boundary.encoded_min.empty() && boundary.encoded_max.empty();
  }
  if (boundary.scalar_type_key.empty()) {
    return false;
  }
  if (IsUnsafeLegacyIndexKeyEncoding(boundary.encoded_min) ||
      IsUnsafeLegacyIndexKeyEncoding(boundary.encoded_max)) {
    return false;
  }
  if (EncodedGreater(boundary.encoded_min, boundary.encoded_max)) {
    return false;
  }
  return boundary.min_present && boundary.max_present &&
         !boundary.encoded_min.empty() && !boundary.encoded_max.empty();
}

bool PageExtentSummaryMetadataIdentityValid(const PageExtentSummaryMetadata& metadata) {
  return PageExtentSummaryUuidTextValid(metadata.relation_uuid) &&
         PageExtentSummaryUuidTextValid(metadata.summary_uuid);
}

bool PageExtentSummaryAuthorityFlagsClean(const PageExtentSummaryMetadata& metadata) {
  return !metadata.parser_finality_authority_claimed &&
         !metadata.donor_finality_authority_claimed &&
         !metadata.write_ahead_log_finality_authority_claimed;
}

PageExtentSummaryFormatCompatibility PageExtentSummaryFormatCompatibilityFromArtifactResult(
    PageExtentSummaryFormatVersion observed,
    bool artifact_ok,
    bool migration_required,
    std::string_view compatibility_class_name,
    std::string diagnostic_code) {
  PageExtentSummaryFormatCompatibility format;
  format.observed = observed;
  const bool current = compatibility_class_name == "current";
  const bool supported_migration =
      compatibility_class_name == "supported_migration" ||
      compatibility_class_name == "supported-migration";
  format.compatible = artifact_ok &&
                      (current || supported_migration);
  format.migration_required = artifact_ok && migration_required;
  format.diagnostic_code = std::move(diagnostic_code);
  if (artifact_ok && current) {
    format.open_class = PageExtentSummaryFormatOpenClass::current;
  } else if (artifact_ok && supported_migration) {
    format.open_class = PageExtentSummaryFormatOpenClass::supported_migration;
  } else {
    format.open_class = PageExtentSummaryFormatOpenClass::refused;
  }
  return format;
}

PageExtentSummaryDecision ClassifyPageExtentSummaryForUse(
    const PageExtentSummaryMetadata& metadata,
    const PageExtentSummaryFormatCompatibility& format) {
  if (!PageExtentSummaryMetadataIdentityValid(metadata)) {
    auto decision = Fallback(PageExtentSummaryFallbackReason::invalid_identity_full_scan,
                             PageExtentSummaryRebuildClassification::transient_rebuild_refused,
                             "INDEX.PAGE_SUMMARY.INVALID_IDENTITY",
                             "index.page_summary.invalid_identity");
    decision.actions.push_back("ignore_summary_until_generated_uuid_identity_is_supplied");
    return decision;
  }
  if (!PageExtentSummaryAuthorityFlagsClean(metadata)) {
    auto decision = Fallback(
        PageExtentSummaryFallbackReason::external_finality_authority_full_scan,
        PageExtentSummaryRebuildClassification::restricted_repair_required,
        "INDEX.PAGE_SUMMARY.EXTERNAL_FINALITY_AUTHORITY_REFUSED",
        "index.page_summary.external_finality_authority_refused");
    decision.actions.push_back("discard_summary_authority_claim");
    return decision;
  }
  if (!format.compatible || format.open_class == PageExtentSummaryFormatOpenClass::refused ||
      metadata.status == PageExtentSummaryStatus::incompatible_format) {
    auto decision = Fallback(PageExtentSummaryFallbackReason::incompatible_summary_full_scan,
                             PageExtentSummaryRebuildClassification::restricted_repair_required,
                             "INDEX.PAGE_SUMMARY.FORMAT_REFUSED",
                             "index.page_summary.format_refused",
                             format.diagnostic_code);
    decision.actions.push_back("require_restricted_repair_or_upgrade_before_summary_use");
    return decision;
  }
  if (!SameFormatVersion(metadata.format_version, format.observed)) {
    auto decision = Fallback(PageExtentSummaryFallbackReason::incompatible_summary_full_scan,
                             PageExtentSummaryRebuildClassification::restricted_repair_required,
                             "INDEX.PAGE_SUMMARY.FORMAT_METADATA_MISMATCH",
                             "index.page_summary.format_metadata_mismatch");
    decision.actions.push_back("refuse_summary_until_persisted_format_identity_matches");
    return decision;
  }
  if (format.migration_required ||
      format.open_class == PageExtentSummaryFormatOpenClass::supported_migration) {
    auto decision = Fallback(PageExtentSummaryFallbackReason::incompatible_summary_full_scan,
                             PageExtentSummaryRebuildClassification::persisted_repair_required,
                             "INDEX.PAGE_SUMMARY.FORMAT_MIGRATION_REQUIRED",
                             "index.page_summary.format_migration_required");
    decision.actions.push_back("run_explicit_summary_format_migration_or_rebuild");
    return decision;
  }
  if (!metadata.persisted_record_present || metadata.status == PageExtentSummaryStatus::missing) {
    auto decision = Fallback(PageExtentSummaryFallbackReason::missing_summary_full_scan,
                             PageExtentSummaryRebuildClassification::transient_rebuild_allowed,
                             "INDEX.PAGE_SUMMARY.MISSING_FULL_SCAN",
                             "index.page_summary.missing_full_scan");
    decision.actions.push_back("schedule_transient_summary_rebuild");
    return decision;
  }
  if (!metadata.checksum_valid || metadata.status == PageExtentSummaryStatus::corrupt) {
    auto decision = Fallback(PageExtentSummaryFallbackReason::corrupt_summary_full_scan,
                             PageExtentSummaryRebuildClassification::restricted_repair_required,
                             "INDEX.PAGE_SUMMARY.CORRUPT_FULL_SCAN",
                             "index.page_summary.corrupt_full_scan");
    decision.actions.push_back("quarantine_summary_until_repair_validates_base_pages");
    return decision;
  }
  if (metadata.status == PageExtentSummaryStatus::stale) {
    auto decision = Fallback(PageExtentSummaryFallbackReason::stale_summary_full_scan,
                             PageExtentSummaryRebuildClassification::transient_rebuild_allowed,
                             "INDEX.PAGE_SUMMARY.STALE_FULL_SCAN",
                             "index.page_summary.stale_full_scan");
    decision.actions.push_back("refresh_summary_from_base_pages");
    return decision;
  }
  if (!PageExtentSummaryRangeValid(metadata.range) ||
      !CountsValid(metadata) ||
      !PageExtentSummaryBoundaryValid(metadata.boundary,
                                      metadata.row_count,
                                      metadata.null_count) ||
      metadata.generation == 0) {
    auto decision = Fallback(PageExtentSummaryFallbackReason::corrupt_summary_full_scan,
                             PageExtentSummaryRebuildClassification::restricted_repair_required,
                             "INDEX.PAGE_SUMMARY.INVALID_METADATA",
                             "index.page_summary.invalid_metadata");
    decision.actions.push_back("classify_summary_metadata_as_corrupt");
    return decision;
  }
  if (!IsCompatibleCurrent(format) || metadata.status != PageExtentSummaryStatus::current) {
    auto decision = Fallback(PageExtentSummaryFallbackReason::incompatible_summary_full_scan,
                             PageExtentSummaryRebuildClassification::restricted_repair_required,
                             "INDEX.PAGE_SUMMARY.UNSAFE_STATE_FULL_SCAN",
                             "index.page_summary.unsafe_state_full_scan");
    decision.actions.push_back("refuse_summary_until_state_is_current");
    return decision;
  }

  auto decision = Decision(OkStatus(),
                           PageExtentSummaryUseClass::use_summary,
                           PageExtentSummaryFallbackReason::none,
                           PageExtentSummaryRebuildClassification::none,
                           "INDEX.PAGE_SUMMARY.USABLE",
                           "index.page_summary.usable");
  decision.full_scan_required = false;
  decision.actions.push_back("use_page_extent_summary_bounds");
  decision.actions.push_back("retain_mga_visibility_recheck_for_rows");
  return decision;
}

PageExtentSummaryDecision ClassifyPageExtentSummaryTransientRebuild(
    const PageExtentSummaryMetadata& metadata,
    const PageExtentSummaryFormatCompatibility& format,
    bool caller_allows_transient_rebuild) {
  const auto base = ClassifyPageExtentSummaryForUse(metadata, format);
  if (base.summary_usable) {
    auto decision = Decision(OkStatus(),
                             PageExtentSummaryUseClass::use_summary,
                             PageExtentSummaryFallbackReason::none,
                             PageExtentSummaryRebuildClassification::none,
                             "INDEX.PAGE_SUMMARY.REBUILD_NOT_REQUIRED",
                             "index.page_summary.rebuild_not_required");
    decision.full_scan_required = false;
    decision.actions.push_back("summary_current_no_transient_rebuild");
    return decision;
  }
  if (!caller_allows_transient_rebuild || !format.compatible ||
      base.restricted_repair_required ||
      !PageExtentSummaryMetadataIdentityValid(metadata) ||
      !PageExtentSummaryAuthorityFlagsClean(metadata)) {
    auto decision = Fallback(base.fallback_reason,
                             PageExtentSummaryRebuildClassification::transient_rebuild_refused,
                             "INDEX.PAGE_SUMMARY.TRANSIENT_REBUILD_REFUSED",
                             "index.page_summary.transient_rebuild_refused");
    decision.actions.push_back("continue_full_scan_without_transient_summary");
    return decision;
  }

  auto decision = Fallback(base.fallback_reason,
                           PageExtentSummaryRebuildClassification::transient_rebuild_allowed,
                           "INDEX.PAGE_SUMMARY.TRANSIENT_REBUILD_ALLOWED",
                           "index.page_summary.transient_rebuild_allowed");
  decision.actions.push_back("rebuild_summary_transient_from_authoritative_base_pages");
  return decision;
}

PageExtentSummaryDecision ClassifyPageExtentSummaryRepairOrCrashReopen(
    const PageExtentSummaryMetadata& metadata,
    const PageExtentSummaryFormatCompatibility& format,
    bool caller_allows_transient_rebuild,
    bool expected_generation_present,
    u64 expected_generation) {
  if (expected_generation_present && metadata.generation != expected_generation) {
    auto generation_mismatch = metadata;
    generation_mismatch.status = PageExtentSummaryStatus::stale;
    generation_mismatch.safe_fallback_reason =
        PageExtentSummaryFallbackReason::stale_summary_full_scan;
    auto decision = ClassifyPageExtentSummaryTransientRebuild(
        generation_mismatch, format, caller_allows_transient_rebuild);
    decision.actions.push_back(
        "generation_mismatch_rebuild_from_authoritative_base_pages");
    return decision;
  }
  auto base = ClassifyPageExtentSummaryForUse(metadata, format);
  if (base.summary_usable || base.restricted_repair_required) {
    base.actions.push_back("repair_crash_reopen_summary_classified");
    return base;
  }
  return ClassifyPageExtentSummaryTransientRebuild(
      metadata, format, caller_allows_transient_rebuild);
}

bool PageExtentSummaryRangeContainsRow(const PageExtentSummaryRange& range,
                                       const PageExtentSummaryRowEvidence& row) {
  if (!PageExtentSummaryRangeValid(range)) {
    return false;
  }
  switch (range.kind) {
    case PageExtentSummaryRangeKind::page_range:
      return row.page_id >= range.first_page_id &&
             row.page_id < range.first_page_id + range.page_count;
    case PageExtentSummaryRangeKind::extent_range:
      return row.extent_id >= range.first_extent_id &&
             row.extent_id < range.first_extent_id + range.extent_count;
  }
  return false;
}

bool PageExtentSummaryRowEvidenceAuthorityClean(
    const PageExtentSummaryRowEvidence& row) {
  return !row.parser_finality_authority_claimed &&
         !row.donor_finality_authority_claimed &&
         !row.write_ahead_log_finality_authority_claimed;
}

PageExtentSummaryMaintenanceResult RebuildPageExtentSummaryFromBasePageEvidence(
    const PageExtentSummaryMetadata& seed,
    const PageExtentSummaryFormatCompatibility& format,
    const PageExtentSummaryMaintenanceEvent& event) {
  auto metadata = seed;
  if (!SameIdentity(seed, event) || !PageExtentSummaryMetadataIdentityValid(seed)) {
    return MaintenanceFallback(
        metadata,
        PageExtentSummaryFallbackReason::invalid_identity_full_scan,
        PageExtentSummaryRebuildClassification::transient_rebuild_refused,
        "INDEX.PAGE_SUMMARY.MAINTENANCE.IDENTITY_MISMATCH",
        "index.page_summary.maintenance.identity_mismatch",
        "refuse_maintenance_for_non_matching_generated_uuid_identity");
  }
  if (!PageExtentSummaryAuthorityFlagsClean(seed) || !EventAuthorityClean(event)) {
    metadata.parser_finality_authority_claimed =
        metadata.parser_finality_authority_claimed ||
        event.parser_finality_authority_claimed;
    metadata.donor_finality_authority_claimed =
        metadata.donor_finality_authority_claimed ||
        event.donor_finality_authority_claimed;
    metadata.write_ahead_log_finality_authority_claimed =
        metadata.write_ahead_log_finality_authority_claimed ||
        event.write_ahead_log_finality_authority_claimed;
    auto decision = ClassifyPageExtentSummaryForUse(metadata, format);
    decision.actions.push_back("external_finality_claim_rebuild_refused");
    return MaintenanceResult(std::move(metadata), std::move(decision),
                             false, false, false);
  }
  if (!PageExtentSummaryRangeValid(seed.range)) {
    metadata.status = PageExtentSummaryStatus::corrupt;
    auto decision = ClassifyPageExtentSummaryForUse(metadata, format);
    decision.actions.push_back("invalid_summary_range_rebuild_refused");
    return MaintenanceResult(std::move(metadata), std::move(decision),
                             false, false, false);
  }

  metadata.boundary = {};
  metadata.row_count = 0;
  metadata.null_count = 0;
  metadata.nulls_present = false;
  for (const auto& row : event.base_page_rows) {
    if (!row.engine_mga_visible ||
        !PageExtentSummaryRangeContainsRow(seed.range, row)) {
      continue;
    }
    if (!PageExtentSummaryRowEvidenceAuthorityClean(row)) {
      TaintMetadataFromRowAuthority(metadata, row);
      auto decision = ClassifyPageExtentSummaryForUse(metadata, format);
      decision.actions.push_back("base_page_external_finality_rebuild_refused");
      return MaintenanceResult(std::move(metadata), std::move(decision),
                               false, false, false);
    }
    if (!ScalarEvidenceValid(row) ||
        !SameScalarTypeOrUnset(metadata, row)) {
      metadata.status = PageExtentSummaryStatus::corrupt;
      metadata.safe_fallback_reason =
          PageExtentSummaryFallbackReason::corrupt_summary_full_scan;
      metadata.rebuild_classification =
          PageExtentSummaryRebuildClassification::restricted_repair_required;
      auto decision = ClassifyPageExtentSummaryForUse(metadata, format);
      decision.actions.push_back("base_page_evidence_rebuild_refused");
      return MaintenanceResult(std::move(metadata), std::move(decision),
                               false, false, false);
    }
    ++metadata.row_count;
    if (row.value_is_null) {
      ++metadata.null_count;
      metadata.nulls_present = true;
    } else {
      ExpandBoundary(metadata, row);
    }
  }

  metadata.status = PageExtentSummaryStatus::current;
  metadata.safe_fallback_reason = PageExtentSummaryFallbackReason::none;
  metadata.rebuild_classification = PageExtentSummaryRebuildClassification::none;
  metadata.persisted_record_present = true;
  metadata.checksum_valid = true;
  metadata.generation = NextGeneration(seed.generation);
  auto decision = ClassifyPageExtentSummaryForUse(metadata, format);
  decision.actions.push_back("rebuilt_summary_from_authoritative_base_pages");
  return MaintenanceResult(std::move(metadata), std::move(decision),
                           true, false, true);
}

PageExtentSummaryMaintenanceResult ApplyPageExtentSummaryMaintenanceEvent(
    const PageExtentSummaryMetadata& metadata,
    const PageExtentSummaryFormatCompatibility& format,
    const PageExtentSummaryMaintenanceEvent& event) {
  if (event.kind == PageExtentSummaryMaintenanceEventKind::bulk_ingest ||
      event.kind == PageExtentSummaryMaintenanceEventKind::rebuild ||
      event.kind == PageExtentSummaryMaintenanceEventKind::repair) {
    return RebuildPageExtentSummaryFromBasePageEvidence(metadata, format, event);
  }
  if (event.kind == PageExtentSummaryMaintenanceEventKind::crash_reopen) {
    auto decision = ClassifyPageExtentSummaryRepairOrCrashReopen(
        metadata, format, event.caller_allows_transient_rebuild);
    decision.actions.push_back("crash_reopen_summary_classified");
    return MaintenanceResult(metadata, std::move(decision), false, false, false);
  }

  auto working = metadata;
  if (!SameIdentity(metadata, event) ||
      !PageExtentSummaryMetadataIdentityValid(metadata)) {
    return MaintenanceFallback(
        working,
        PageExtentSummaryFallbackReason::invalid_identity_full_scan,
        PageExtentSummaryRebuildClassification::transient_rebuild_refused,
        "INDEX.PAGE_SUMMARY.MAINTENANCE.IDENTITY_MISMATCH",
        "index.page_summary.maintenance.identity_mismatch",
        "refuse_maintenance_for_non_matching_generated_uuid_identity");
  }
  if (!PageExtentSummaryAuthorityFlagsClean(metadata) ||
      !EventAuthorityClean(event)) {
    working.parser_finality_authority_claimed =
        working.parser_finality_authority_claimed ||
        event.parser_finality_authority_claimed;
    working.donor_finality_authority_claimed =
        working.donor_finality_authority_claimed ||
        event.donor_finality_authority_claimed;
    working.write_ahead_log_finality_authority_claimed =
        working.write_ahead_log_finality_authority_claimed ||
        event.write_ahead_log_finality_authority_claimed;
    auto decision = ClassifyPageExtentSummaryForUse(working, format);
    decision.actions.push_back("external_finality_claim_maintenance_refused");
    return MaintenanceResult(std::move(working), std::move(decision),
                             false, false, false);
  }

  const auto usable = ClassifyPageExtentSummaryForUse(metadata, format);
  if (!usable.summary_usable) {
    return MaintenanceResult(metadata, usable, false, false, false);
  }

  if ((event.before_row_present &&
       !PageExtentSummaryRowEvidenceAuthorityClean(event.before_row)) ||
      (event.after_row_present &&
       !PageExtentSummaryRowEvidenceAuthorityClean(event.after_row))) {
    if (event.before_row_present) {
      TaintMetadataFromRowAuthority(working, event.before_row);
    }
    if (event.after_row_present) {
      TaintMetadataFromRowAuthority(working, event.after_row);
    }
    auto decision = ClassifyPageExtentSummaryForUse(working, format);
    decision.actions.push_back("row_external_finality_maintenance_refused");
    return MaintenanceResult(std::move(working), std::move(decision),
                             false, false, false);
  }

  switch (event.kind) {
    case PageExtentSummaryMaintenanceEventKind::insert_row:
      if (event.after_row_present &&
          (!event.after_row.engine_mga_visible ||
           !PageExtentSummaryRangeContainsRow(metadata.range, event.after_row))) {
        return NoOpForUnaffectedRange(
            working, format, "insert_row_outside_summary_range_noop");
      }
      if (!event.after_row_present ||
          !ScalarEvidenceValid(event.after_row) ||
          !ApplyInsertRow(working, event.after_row)) {
        return InvalidateForRebuild(working, format,
                                    event.caller_allows_transient_rebuild,
                                    "insert_evidence_not_exact_summary_stale");
      }
      break;
    case PageExtentSummaryMaintenanceEventKind::update_row:
      if (event.before_row_present && event.after_row_present &&
          (!event.before_row.engine_mga_visible ||
           !PageExtentSummaryRangeContainsRow(metadata.range, event.before_row)) &&
          (!event.after_row.engine_mga_visible ||
           !PageExtentSummaryRangeContainsRow(metadata.range, event.after_row))) {
        return NoOpForUnaffectedRange(
            working, format, "update_row_outside_summary_range_noop");
      }
      if (!event.before_row_present || !event.after_row_present ||
          !ScalarEvidenceValid(event.before_row) ||
          !ScalarEvidenceValid(event.after_row) ||
          !ApplyUpdateRows(working, event.before_row, event.after_row)) {
        return InvalidateForRebuild(working, format,
                                    event.caller_allows_transient_rebuild,
                                    "update_evidence_not_exact_summary_stale");
      }
      break;
    case PageExtentSummaryMaintenanceEventKind::delete_row:
      if (event.before_row_present &&
          (!event.before_row.engine_mga_visible ||
           !PageExtentSummaryRangeContainsRow(metadata.range, event.before_row))) {
        return NoOpForUnaffectedRange(
            working, format, "delete_row_outside_summary_range_noop");
      }
      if (!event.before_row_present ||
          !ScalarEvidenceValid(event.before_row) ||
          !ApplyDeleteRow(working, event.before_row)) {
        return InvalidateForRebuild(working, format,
                                    event.caller_allows_transient_rebuild,
                                    "delete_evidence_not_exact_summary_stale");
      }
      break;
    case PageExtentSummaryMaintenanceEventKind::bulk_ingest:
    case PageExtentSummaryMaintenanceEventKind::rebuild:
    case PageExtentSummaryMaintenanceEventKind::repair:
    case PageExtentSummaryMaintenanceEventKind::crash_reopen:
      break;
  }

  auto decision = ClassifyPageExtentSummaryForUse(working, format);
  decision.actions.push_back("applied_page_summary_dml_maintenance");
  return MaintenanceResult(std::move(working), std::move(decision),
                           true, false, false);
}

const char* PageExtentSummaryStatusName(PageExtentSummaryStatus status) {
  switch (status) {
    case PageExtentSummaryStatus::missing: return "missing";
    case PageExtentSummaryStatus::current: return "current";
    case PageExtentSummaryStatus::stale: return "stale";
    case PageExtentSummaryStatus::corrupt: return "corrupt";
    case PageExtentSummaryStatus::incompatible_format: return "incompatible_format";
  }
  return "unknown";
}

const char* PageExtentSummaryMaintenanceEventKindName(
    PageExtentSummaryMaintenanceEventKind kind) {
  switch (kind) {
    case PageExtentSummaryMaintenanceEventKind::insert_row: return "insert_row";
    case PageExtentSummaryMaintenanceEventKind::update_row: return "update_row";
    case PageExtentSummaryMaintenanceEventKind::delete_row: return "delete_row";
    case PageExtentSummaryMaintenanceEventKind::bulk_ingest: return "bulk_ingest";
    case PageExtentSummaryMaintenanceEventKind::rebuild: return "rebuild";
    case PageExtentSummaryMaintenanceEventKind::repair: return "repair";
    case PageExtentSummaryMaintenanceEventKind::crash_reopen: return "crash_reopen";
  }
  return "unknown";
}

const char* PageExtentSummaryUseClassName(PageExtentSummaryUseClass use_class) {
  switch (use_class) {
    case PageExtentSummaryUseClass::use_summary: return "use_summary";
    case PageExtentSummaryUseClass::full_scan_fallback: return "full_scan_fallback";
    case PageExtentSummaryUseClass::restricted_repair: return "restricted_repair";
    case PageExtentSummaryUseClass::refused: return "refused";
  }
  return "unknown";
}

const char* PageExtentSummaryRebuildClassificationName(
    PageExtentSummaryRebuildClassification classification) {
  switch (classification) {
    case PageExtentSummaryRebuildClassification::none: return "none";
    case PageExtentSummaryRebuildClassification::transient_rebuild_allowed:
      return "transient_rebuild_allowed";
    case PageExtentSummaryRebuildClassification::transient_rebuild_refused:
      return "transient_rebuild_refused";
    case PageExtentSummaryRebuildClassification::persisted_repair_required:
      return "persisted_repair_required";
    case PageExtentSummaryRebuildClassification::restricted_repair_required:
      return "restricted_repair_required";
  }
  return "unknown";
}

const char* PageExtentSummaryFallbackReasonName(
    PageExtentSummaryFallbackReason reason) {
  switch (reason) {
    case PageExtentSummaryFallbackReason::none: return "none";
    case PageExtentSummaryFallbackReason::stale_summary_full_scan:
      return "stale_summary_full_scan";
    case PageExtentSummaryFallbackReason::missing_summary_full_scan:
      return "missing_summary_full_scan";
    case PageExtentSummaryFallbackReason::corrupt_summary_full_scan:
      return "corrupt_summary_full_scan";
    case PageExtentSummaryFallbackReason::incompatible_summary_full_scan:
      return "incompatible_summary_full_scan";
    case PageExtentSummaryFallbackReason::restricted_repair_required:
      return "restricted_repair_required";
    case PageExtentSummaryFallbackReason::invalid_identity_full_scan:
      return "invalid_identity_full_scan";
    case PageExtentSummaryFallbackReason::external_finality_authority_full_scan:
      return "external_finality_authority_full_scan";
  }
  return "unknown";
}

DiagnosticRecord MakePageExtentSummaryDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {}, "core.index.page_extent_summary");
}

}  // namespace scratchbird::core::index
