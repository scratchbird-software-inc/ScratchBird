// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "page_extent_summary.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace db = scratchbird::storage::database;
namespace idx = scratchbird::core::index;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kGateSearchKey =
    "DPC_PAGE_EXTENT_SUMMARY_METADATA_GATE";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

struct UuidFactory {
  std::uint64_t base_millis = NowMillis();

  TypedUuid Typed(UuidKind kind, std::uint64_t salt) const {
    const auto generated =
        uuid::GenerateEngineIdentityV7(kind, base_millis + salt);
    Require(generated.ok(), "DPC-011 UUID generation failed");
    return generated.value;
  }

  std::string Text(UuidKind kind, std::uint64_t salt) const {
    return uuid::UuidToString(Typed(kind, salt).value);
  }
};

db::DatabaseArtifactVersionCompatibilityRequest VersionRequest(
    idx::PageExtentSummaryFormatVersion observed) {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  db::DatabaseArtifactVersionCompatibilityRequest request;
  request.artifact_kind = contract.artifact_kind;
  request.format_major = observed.major;
  request.format_minor = observed.minor;
  request.min_supported_major = contract.min_supported.major;
  request.min_supported_minor = contract.min_supported.minor;
  request.current_major = contract.current.major;
  request.current_minor = contract.current.minor;
  request.max_supported_major = contract.max_supported.major;
  request.max_supported_minor = contract.max_supported.minor;
  return request;
}

idx::PageExtentSummaryFormatCompatibility LinkedFormat(
    idx::PageExtentSummaryFormatVersion observed,
    const db::DatabaseArtifactCompatibilityResult& result) {
  return idx::PageExtentSummaryFormatCompatibilityFromArtifactResult(
      observed,
      result.ok(),
      result.migration_required,
      db::DatabaseOpenCompatibilityClassName(result.compatibility_class),
      result.diagnostic.diagnostic_code);
}

idx::PageExtentSummaryFormatCompatibility CurrentFormat() {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  const auto classified =
      db::ClassifyDatabaseArtifactVersionCompatibility(
          VersionRequest(contract.current));
  Require(classified.ok(), "DPC-011 current format was not accepted");
  return LinkedFormat(contract.current, classified);
}

idx::PageExtentSummaryMetadata CurrentSummary(const UuidFactory& uuids) {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::PageExtentSummaryMetadata metadata;
  metadata.relation_uuid = uuids.Text(UuidKind::object, 10);
  metadata.summary_uuid = uuids.Text(UuidKind::object, 11);
  metadata.range.kind = idx::PageExtentSummaryRangeKind::page_range;
  metadata.range.first_page_id = 128;
  metadata.range.page_count = 16;
  metadata.boundary.scalar_type_key = "int64";
  metadata.boundary.encoded_min = "100";
  metadata.boundary.encoded_max = "900";
  metadata.boundary.min_present = true;
  metadata.boundary.max_present = true;
  metadata.row_count = 512;
  metadata.null_count = 7;
  metadata.nulls_present = true;
  metadata.status = idx::PageExtentSummaryStatus::current;
  metadata.format_version = contract.current;
  metadata.generation = 42;
  metadata.persisted_record_present = true;
  metadata.checksum_valid = true;
  return metadata;
}

void RequireUse(const idx::PageExtentSummaryDecision& decision,
                std::string_view message) {
  if (!decision.summary_usable || decision.full_scan_required ||
      decision.use_class != idx::PageExtentSummaryUseClass::use_summary) {
    std::cerr << decision.diagnostic.diagnostic_code << ' '
              << idx::PageExtentSummaryUseClassName(decision.use_class) << '\n';
  }
  Require(decision.summary_usable, message);
  Require(!decision.full_scan_required, message);
  Require(decision.use_class == idx::PageExtentSummaryUseClass::use_summary,
          message);
}

void RequireFallback(const idx::PageExtentSummaryDecision& decision,
                     idx::PageExtentSummaryFallbackReason reason,
                     std::string_view message) {
  if (!decision.full_scan_required || decision.fallback_reason != reason) {
    std::cerr << decision.diagnostic.diagnostic_code << ' '
              << idx::PageExtentSummaryFallbackReasonName(
                     decision.fallback_reason)
              << '\n';
  }
  Require(decision.full_scan_required, message);
  Require(decision.fallback_reason == reason, message);
}

void ValidateGeneratedUuidSafeIdentity() {
  const UuidFactory uuids;
  const auto relation_uuid = uuids.Text(UuidKind::object, 1);
  const auto summary_uuid = uuids.Text(UuidKind::object, 2);

  Require(idx::PageExtentSummaryUuidTextValid(relation_uuid),
          "DPC-011 generated relation UUID was rejected");
  Require(idx::PageExtentSummaryUuidTextValid(summary_uuid),
          "DPC-011 generated summary UUID was rejected");
  Require(!idx::PageExtentSummaryUuidTextValid(
              "00000000-0000-0000-0000-000000000000"),
          "DPC-011 nil UUID was accepted");
  Require(!idx::PageExtentSummaryUuidTextValid("catalog.page_extent_summary"),
          "DPC-011 non-UUID catalog key was accepted");

  auto metadata = CurrentSummary(uuids);
  metadata.relation_uuid = "catalog.page_extent_summary";
  const auto decision =
      idx::ClassifyPageExtentSummaryForUse(metadata, CurrentFormat());
  RequireFallback(decision,
                  idx::PageExtentSummaryFallbackReason::invalid_identity_full_scan,
                  "DPC-011 invalid relation UUID did not force safe fallback");
}

void ValidatePersistedFormatLinkage() {
  const UuidFactory uuids;
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  Require(contract.artifact_kind == idx::kPageExtentSummaryArtifactKind,
          "DPC-011 artifact kind is not page extent summary");
  Require(contract.feature_map_key == idx::kPageExtentSummaryFeatureMapKey,
          "DPC-011 feature map key mismatch");

  auto metadata = CurrentSummary(uuids);
  const auto current_classified =
      db::ClassifyDatabaseArtifactVersionCompatibility(
          VersionRequest(contract.current));
  const auto current_format = LinkedFormat(contract.current, current_classified);
  Require(current_classified.ok(), "DPC-011 current format helper refused");
  Require(current_format.compatible &&
              current_format.open_class ==
                  idx::PageExtentSummaryFormatOpenClass::current,
          "DPC-011 current format was not linked as current");
  RequireUse(idx::ClassifyPageExtentSummaryForUse(metadata, current_format),
             "DPC-011 current format summary was not usable");

  const idx::PageExtentSummaryFormatVersion old_format{1, 0};
  auto old_without_plan = VersionRequest(old_format);
  const auto old_without_plan_result =
      db::ClassifyDatabaseArtifactVersionCompatibility(old_without_plan);
  Require(!old_without_plan_result.ok() &&
              old_without_plan_result.compatibility_class ==
                  db::DatabaseOpenCompatibilityClass::
                      migration_required_without_plan_refused,
          "DPC-011 old format without plan was not refused by helper");
  metadata.format_version = old_format;
  RequireFallback(
      idx::ClassifyPageExtentSummaryForUse(
          metadata, LinkedFormat(old_format, old_without_plan_result)),
      idx::PageExtentSummaryFallbackReason::incompatible_summary_full_scan,
      "DPC-011 helper-refused old format did not force fallback");

  auto old_with_plan = VersionRequest(old_format);
  old_with_plan.migration_plan_id =
      idx::PageExtentSummaryMigrationPlanId(old_format);
  const auto old_with_plan_result =
      db::ClassifyDatabaseArtifactVersionCompatibility(old_with_plan);
  const auto old_accepted_format =
      LinkedFormat(old_format, old_with_plan_result);
  if (!old_with_plan_result.ok() ||
      old_with_plan_result.compatibility_class !=
          db::DatabaseOpenCompatibilityClass::supported_migration ||
      !old_accepted_format.compatible ||
      !old_accepted_format.migration_required) {
    std::cerr << "old_with_plan_class="
              << db::DatabaseOpenCompatibilityClassName(
                     old_with_plan_result.compatibility_class)
              << " diagnostic="
              << old_with_plan_result.diagnostic.diagnostic_code
              << " plan=" << old_with_plan.migration_plan_id << '\n';
  }
  Require(old_with_plan_result.ok() &&
              old_with_plan_result.compatibility_class ==
                  db::DatabaseOpenCompatibilityClass::supported_migration &&
              old_accepted_format.compatible &&
              old_accepted_format.migration_required,
          "DPC-011 old format migration plan was not accepted by helper");
  const auto old_decision =
      idx::ClassifyPageExtentSummaryForUse(metadata, old_accepted_format);
  RequireFallback(
      old_decision,
      idx::PageExtentSummaryFallbackReason::incompatible_summary_full_scan,
      "DPC-011 migration-required summary did not force safe fallback");
  Require(old_decision.rebuild_classification ==
              idx::PageExtentSummaryRebuildClassification::
                  persisted_repair_required,
          "DPC-011 accepted migration did not require persisted repair");

  const idx::PageExtentSummaryFormatVersion future_format{
      contract.current.major + 1, 0};
  const auto future_result =
      db::ClassifyDatabaseArtifactVersionCompatibility(
          VersionRequest(future_format));
  Require(!future_result.ok() &&
              future_result.compatibility_class ==
                  db::DatabaseOpenCompatibilityClass::unsupported_new,
          "DPC-011 future format was not refused by helper");
  metadata.format_version = future_format;
  RequireFallback(
      idx::ClassifyPageExtentSummaryForUse(
          metadata, LinkedFormat(future_format, future_result)),
      idx::PageExtentSummaryFallbackReason::incompatible_summary_full_scan,
      "DPC-011 future format refusal did not force fallback");
}

void ValidateFallbackAndTransientRebuildClassification() {
  const UuidFactory uuids;
  const auto format = CurrentFormat();
  auto metadata = CurrentSummary(uuids);
  RequireUse(idx::ClassifyPageExtentSummaryForUse(metadata, format),
             "DPC-011 current summary was not usable");

  metadata.status = idx::PageExtentSummaryStatus::stale;
  auto stale = idx::ClassifyPageExtentSummaryForUse(metadata, format);
  RequireFallback(stale,
                  idx::PageExtentSummaryFallbackReason::stale_summary_full_scan,
                  "DPC-011 stale summary did not fall back to full scan");
  Require(stale.rebuild_classification ==
              idx::PageExtentSummaryRebuildClassification::
                  transient_rebuild_allowed,
          "DPC-011 stale summary did not allow transient rebuild");

  const auto stale_rebuild =
      idx::ClassifyPageExtentSummaryTransientRebuild(metadata, format, true);
  Require(stale_rebuild.transient_rebuild_allowed,
          "DPC-011 transient rebuild was not allowed for stale summary");

  metadata = CurrentSummary(uuids);
  metadata.status = idx::PageExtentSummaryStatus::missing;
  metadata.persisted_record_present = false;
  metadata.boundary = {};
  metadata.row_count = 0;
  metadata.null_count = 0;
  metadata.nulls_present = false;
  auto missing = idx::ClassifyPageExtentSummaryForUse(metadata, format);
  RequireFallback(missing,
                  idx::PageExtentSummaryFallbackReason::missing_summary_full_scan,
                  "DPC-011 missing summary did not fall back to full scan");
  Require(missing.transient_rebuild_allowed,
          "DPC-011 missing summary did not classify transient rebuild allowed");

  metadata = CurrentSummary(uuids);
  metadata.status = idx::PageExtentSummaryStatus::corrupt;
  metadata.checksum_valid = false;
  const auto corrupt = idx::ClassifyPageExtentSummaryForUse(metadata, format);
  RequireFallback(corrupt,
                  idx::PageExtentSummaryFallbackReason::corrupt_summary_full_scan,
                  "DPC-011 corrupt summary did not fall back to full scan");
  Require(corrupt.restricted_repair_required,
          "DPC-011 corrupt summary did not require restricted repair");

  metadata = CurrentSummary(uuids);
  metadata.status = idx::PageExtentSummaryStatus::incompatible_format;
  const auto incompatible =
      idx::ClassifyPageExtentSummaryForUse(metadata, format);
  RequireFallback(incompatible,
                  idx::PageExtentSummaryFallbackReason::
                      incompatible_summary_full_scan,
                  "DPC-011 incompatible summary did not fall back to full scan");
  Require(incompatible.restricted_repair_required,
          "DPC-011 incompatible summary did not require restricted repair");
}

void ValidateAuthorityBoundary() {
  const UuidFactory uuids;
  const auto format = CurrentFormat();
  auto metadata = CurrentSummary(uuids);
  Require(idx::PageExtentSummaryAuthorityFlagsClean(metadata),
          "DPC-011 clean authority flags rejected");
  RequireUse(idx::ClassifyPageExtentSummaryForUse(metadata, format),
             "DPC-011 clean authority summary was not usable");

  metadata.parser_finality_authority_claimed = true;
  metadata.donor_finality_authority_claimed = true;
  metadata.write_ahead_log_finality_authority_claimed = true;
  Require(!idx::PageExtentSummaryAuthorityFlagsClean(metadata),
          "DPC-011 external finality authority flags were accepted");
  const auto decision = idx::ClassifyPageExtentSummaryForUse(metadata, format);
  RequireFallback(
      decision,
      idx::PageExtentSummaryFallbackReason::external_finality_authority_full_scan,
      "DPC-011 external finality authority did not force full scan fallback");
  Require(decision.restricted_repair_required,
          "DPC-011 external finality authority did not require restricted repair");
}

}  // namespace

int main() {
  ValidateGeneratedUuidSafeIdentity();
  ValidatePersistedFormatLinkage();
  ValidateFallbackAndTransientRebuildClassification();
  ValidateAuthorityBoundary();
  std::cout << kGateSearchKey << "=passed "
            << idx::kPageExtentSummaryMetadataSearchKey
            << "=metadata_storage_contract_only\n";
  return EXIT_SUCCESS;
}
