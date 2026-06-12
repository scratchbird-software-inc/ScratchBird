// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_extent_summary.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace idx = scratchbird::core::index;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kGateSearchKey =
    "DPC_PAGE_EXTENT_SUMMARY_DML_MAINTENANCE_GATE";

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
    Require(generated.ok(), "DPC-012 UUID generation failed");
    return generated.value;
  }

  std::string Text(UuidKind kind, std::uint64_t salt) const {
    return uuid::UuidToString(Typed(kind, salt).value);
  }
};

idx::PageExtentSummaryFormatCompatibility CurrentFormat() {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::PageExtentSummaryFormatCompatibility format;
  format.observed = contract.current;
  format.open_class = idx::PageExtentSummaryFormatOpenClass::current;
  format.compatible = true;
  format.migration_required = false;
  format.diagnostic_code = "DPC-012.current_format";
  return format;
}

idx::PageExtentSummaryMetadata Summary(const UuidFactory& uuids) {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::PageExtentSummaryMetadata metadata;
  metadata.relation_uuid = uuids.Text(UuidKind::object, 20);
  metadata.summary_uuid = uuids.Text(UuidKind::object, 21);
  metadata.range.kind = idx::PageExtentSummaryRangeKind::page_range;
  metadata.range.first_page_id = 100;
  metadata.range.page_count = 4;
  metadata.boundary.scalar_type_key = "int64_lex";
  metadata.boundary.encoded_min = "020";
  metadata.boundary.encoded_max = "050";
  metadata.boundary.min_present = true;
  metadata.boundary.max_present = true;
  metadata.row_count = 3;
  metadata.null_count = 1;
  metadata.nulls_present = true;
  metadata.status = idx::PageExtentSummaryStatus::current;
  metadata.format_version = contract.current;
  metadata.generation = 7;
  metadata.persisted_record_present = true;
  metadata.checksum_valid = true;
  return metadata;
}

idx::PageExtentSummaryRowEvidence Row(std::uint64_t page_id,
                                      std::string encoded,
                                      bool is_null = false) {
  idx::PageExtentSummaryRowEvidence row;
  row.page_id = page_id;
  row.extent_id = page_id / 16;
  row.scalar_type_key = "int64_lex";
  row.encoded_scalar = std::move(encoded);
  row.value_is_null = is_null;
  return row;
}

idx::PageExtentSummaryMaintenanceEvent Event(
    idx::PageExtentSummaryMaintenanceEventKind kind,
    const idx::PageExtentSummaryMetadata& metadata) {
  idx::PageExtentSummaryMaintenanceEvent event;
  event.kind = kind;
  event.relation_uuid = metadata.relation_uuid;
  event.summary_uuid = metadata.summary_uuid;
  event.caller_allows_transient_rebuild = true;
  return event;
}

void RequireUse(const idx::PageExtentSummaryDecision& decision,
                std::string_view message) {
  if (!decision.summary_usable || decision.full_scan_required) {
    std::cerr << decision.diagnostic.diagnostic_code << ' '
              << idx::PageExtentSummaryUseClassName(decision.use_class) << '\n';
  }
  Require(decision.summary_usable, message);
  Require(!decision.full_scan_required, message);
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

void ValidateInsertUpdateDeleteMaintenance() {
  const UuidFactory uuids;
  const auto format = CurrentFormat();
  auto metadata = Summary(uuids);

  auto outside_insert =
      Event(idx::PageExtentSummaryMaintenanceEventKind::insert_row, metadata);
  outside_insert.after_row_present = true;
  outside_insert.after_row = Row(500, "001");
  auto outside_inserted = idx::ApplyPageExtentSummaryMaintenanceEvent(
      metadata, format, outside_insert);
  RequireUse(outside_inserted.decision,
             "DPC-012 outside insert did not keep summary usable");
  Require(!outside_inserted.applied && !outside_inserted.summary_invalidated &&
              outside_inserted.metadata.row_count == metadata.row_count &&
              outside_inserted.metadata.generation == metadata.generation,
          "DPC-012 outside insert was not a no-op");

  auto outside_delete =
      Event(idx::PageExtentSummaryMaintenanceEventKind::delete_row, metadata);
  outside_delete.before_row_present = true;
  outside_delete.before_row = Row(500, "001");
  auto outside_deleted = idx::ApplyPageExtentSummaryMaintenanceEvent(
      metadata, format, outside_delete);
  RequireUse(outside_deleted.decision,
             "DPC-012 outside delete did not keep summary usable");
  Require(!outside_deleted.applied && !outside_deleted.summary_invalidated &&
              outside_deleted.metadata.row_count == metadata.row_count &&
              outside_deleted.metadata.generation == metadata.generation,
          "DPC-012 outside delete was not a no-op");

  auto outside_update =
      Event(idx::PageExtentSummaryMaintenanceEventKind::update_row, metadata);
  outside_update.before_row_present = true;
  outside_update.before_row = Row(500, "001");
  outside_update.after_row_present = true;
  outside_update.after_row = Row(501, "002");
  auto outside_updated = idx::ApplyPageExtentSummaryMaintenanceEvent(
      metadata, format, outside_update);
  RequireUse(outside_updated.decision,
             "DPC-012 outside update did not keep summary usable");
  Require(!outside_updated.applied && !outside_updated.summary_invalidated &&
              outside_updated.metadata.row_count == metadata.row_count &&
              outside_updated.metadata.generation == metadata.generation,
          "DPC-012 outside update was not a no-op");

  auto insert = Event(idx::PageExtentSummaryMaintenanceEventKind::insert_row,
                      metadata);
  insert.after_row_present = true;
  insert.after_row = Row(101, "010");
  auto inserted =
      idx::ApplyPageExtentSummaryMaintenanceEvent(metadata, format, insert);
  RequireUse(inserted.decision, "DPC-012 insert did not leave summary usable");
  Require(inserted.applied && !inserted.summary_invalidated,
          "DPC-012 insert did not apply exactly");
  Require(inserted.metadata.row_count == 4 &&
              inserted.metadata.null_count == 1 &&
              inserted.metadata.boundary.encoded_min == "010" &&
              inserted.metadata.boundary.encoded_max == "050" &&
              inserted.metadata.generation == 8,
          "DPC-012 insert did not expand row/min metadata");

  metadata = inserted.metadata;
  auto update = Event(idx::PageExtentSummaryMaintenanceEventKind::update_row,
                      metadata);
  update.before_row_present = true;
  update.before_row = Row(102, "030");
  update.after_row_present = true;
  update.after_row = Row(102, "090");
  auto updated =
      idx::ApplyPageExtentSummaryMaintenanceEvent(metadata, format, update);
  RequireUse(updated.decision, "DPC-012 update did not leave summary usable");
  Require(updated.metadata.row_count == 4 &&
              updated.metadata.null_count == 1 &&
              updated.metadata.boundary.encoded_min == "010" &&
              updated.metadata.boundary.encoded_max == "090",
          "DPC-012 update did not expand max metadata");

  metadata = updated.metadata;
  auto delete_null = Event(
      idx::PageExtentSummaryMaintenanceEventKind::delete_row, metadata);
  delete_null.before_row_present = true;
  delete_null.before_row = Row(103, "", true);
  auto deleted_null =
      idx::ApplyPageExtentSummaryMaintenanceEvent(metadata, format, delete_null);
  RequireUse(deleted_null.decision,
             "DPC-012 null delete did not leave summary usable");
  Require(deleted_null.metadata.row_count == 3 &&
              deleted_null.metadata.null_count == 0 &&
              !deleted_null.metadata.nulls_present,
          "DPC-012 delete did not maintain exact row/null counts");

  metadata = deleted_null.metadata;
  auto delete_boundary = Event(
      idx::PageExtentSummaryMaintenanceEventKind::delete_row, metadata);
  delete_boundary.before_row_present = true;
  delete_boundary.before_row = Row(101, "010");
  auto invalidated = idx::ApplyPageExtentSummaryMaintenanceEvent(
      metadata, format, delete_boundary);
  RequireFallback(invalidated.decision,
                  idx::PageExtentSummaryFallbackReason::stale_summary_full_scan,
                  "DPC-012 boundary delete did not fail safe to full scan");
  Require(invalidated.summary_invalidated &&
              invalidated.decision.transient_rebuild_allowed,
          "DPC-012 boundary delete did not request transient rebuild");

  auto update_boundary = Event(
      idx::PageExtentSummaryMaintenanceEventKind::update_row, metadata);
  update_boundary.before_row_present = true;
  update_boundary.before_row = Row(101, "010");
  update_boundary.after_row_present = true;
  update_boundary.after_row = Row(101, "040");
  auto invalid_update = idx::ApplyPageExtentSummaryMaintenanceEvent(
      metadata, format, update_boundary);
  RequireFallback(invalid_update.decision,
                  idx::PageExtentSummaryFallbackReason::stale_summary_full_scan,
                  "DPC-012 boundary update did not fail safe to full scan");
}

void ValidateBulkIngestAndRebuild() {
  const UuidFactory uuids;
  const auto format = CurrentFormat();
  auto metadata = Summary(uuids);
  auto bulk = Event(idx::PageExtentSummaryMaintenanceEventKind::bulk_ingest,
                    metadata);
  bulk.base_page_rows = {
      Row(103, "090"),
      Row(100, "005"),
      Row(101, "", true),
      Row(102, "030"),
      Row(999, "001"),
  };
  bulk.base_page_rows.back().engine_mga_visible = true;

  auto rebuilt =
      idx::ApplyPageExtentSummaryMaintenanceEvent(metadata, format, bulk);
  RequireUse(rebuilt.decision,
             "DPC-012 bulk ingest rebuild did not leave summary usable");
  Require(rebuilt.rebuild_performed && rebuilt.metadata.row_count == 4 &&
              rebuilt.metadata.null_count == 1 &&
              rebuilt.metadata.boundary.encoded_min == "005" &&
              rebuilt.metadata.boundary.encoded_max == "090" &&
              rebuilt.metadata.generation == 8,
          "DPC-012 bulk ingest did not produce deterministic summary");

  auto rebuild = Event(idx::PageExtentSummaryMaintenanceEventKind::rebuild,
                       rebuilt.metadata);
  rebuild.base_page_rows = {Row(100, "012"), Row(101, "015"),
                            Row(102, "099")};
  auto rebuilt_again = idx::RebuildPageExtentSummaryFromBasePageEvidence(
      rebuilt.metadata, format, rebuild);
  RequireUse(rebuilt_again.decision,
             "DPC-012 rebuild from base pages did not classify usable");
  Require(rebuilt_again.metadata.boundary.encoded_min == "012" &&
              rebuilt_again.metadata.boundary.encoded_max == "099" &&
              rebuilt_again.metadata.row_count == 3,
          "DPC-012 rebuild did not derive current base-page summary");

  auto tainted = Event(idx::PageExtentSummaryMaintenanceEventKind::rebuild,
                       rebuilt_again.metadata);
  tainted.write_ahead_log_finality_authority_claimed = true;
  tainted.base_page_rows = {Row(100, "010")};
  const auto refused = idx::RebuildPageExtentSummaryFromBasePageEvidence(
      rebuilt_again.metadata, format, tainted);
  RequireFallback(
      refused.decision,
      idx::PageExtentSummaryFallbackReason::external_finality_authority_full_scan,
      "DPC-012 external finality rebuild claim did not fail closed");
  Require(refused.decision.restricted_repair_required,
          "DPC-012 external finality rebuild claim did not require repair");
}

void ValidateRepairAndCrashReopenClassification() {
  const UuidFactory uuids;
  const auto format = CurrentFormat();
  auto metadata = Summary(uuids);
  RequireUse(idx::ClassifyPageExtentSummaryRepairOrCrashReopen(
                 metadata, format, true),
             "DPC-012 current crash reopen summary was not usable");

  auto stale = metadata;
  stale.status = idx::PageExtentSummaryStatus::stale;
  const auto stale_decision =
      idx::ClassifyPageExtentSummaryRepairOrCrashReopen(stale, format, true);
  RequireFallback(stale_decision,
                  idx::PageExtentSummaryFallbackReason::stale_summary_full_scan,
                  "DPC-012 stale crash reopen did not fall back");
  Require(stale_decision.transient_rebuild_allowed,
          "DPC-012 stale crash reopen did not allow rebuild");

  auto missing = metadata;
  missing.status = idx::PageExtentSummaryStatus::missing;
  missing.persisted_record_present = false;
  missing.row_count = 0;
  missing.null_count = 0;
  missing.nulls_present = false;
  missing.boundary = {};
  const auto missing_decision =
      idx::ClassifyPageExtentSummaryRepairOrCrashReopen(missing, format, true);
  RequireFallback(
      missing_decision,
      idx::PageExtentSummaryFallbackReason::missing_summary_full_scan,
      "DPC-012 missing crash reopen did not fall back");
  Require(missing_decision.transient_rebuild_allowed,
          "DPC-012 missing crash reopen did not allow rebuild");

  auto corrupt = metadata;
  corrupt.status = idx::PageExtentSummaryStatus::corrupt;
  corrupt.checksum_valid = false;
  const auto corrupt_decision =
      idx::ClassifyPageExtentSummaryRepairOrCrashReopen(corrupt, format, true);
  RequireFallback(
      corrupt_decision,
      idx::PageExtentSummaryFallbackReason::corrupt_summary_full_scan,
      "DPC-012 corrupt crash reopen did not fall back");
  Require(corrupt_decision.restricted_repair_required,
          "DPC-012 corrupt crash reopen did not require restricted repair");

  auto incompatible = metadata;
  incompatible.status = idx::PageExtentSummaryStatus::incompatible_format;
  const auto incompatible_decision =
      idx::ClassifyPageExtentSummaryRepairOrCrashReopen(
          incompatible, format, true);
  RequireFallback(
      incompatible_decision,
      idx::PageExtentSummaryFallbackReason::incompatible_summary_full_scan,
      "DPC-012 incompatible crash reopen did not fall back");
  Require(incompatible_decision.restricted_repair_required,
          "DPC-012 incompatible crash reopen did not require repair");

  const auto mismatched_generation =
      idx::ClassifyPageExtentSummaryRepairOrCrashReopen(
          metadata, format, true, true, metadata.generation + 1);
  RequireFallback(
      mismatched_generation,
      idx::PageExtentSummaryFallbackReason::stale_summary_full_scan,
      "DPC-012 mismatched generation did not fall back as stale");
  Require(mismatched_generation.transient_rebuild_allowed,
          "DPC-012 mismatched generation did not allow rebuild");
}

void ValidateIdentityAndAuthorityBoundaries() {
  const UuidFactory uuids;
  const auto format = CurrentFormat();
  auto metadata = Summary(uuids);
  auto invalid = metadata;
  invalid.summary_uuid = "00000000-0000-0000-0000-000000000000";
  auto event = Event(idx::PageExtentSummaryMaintenanceEventKind::insert_row,
                     invalid);
  event.after_row_present = true;
  event.after_row = Row(100, "010");
  const auto invalid_identity =
      idx::ApplyPageExtentSummaryMaintenanceEvent(invalid, format, event);
  RequireFallback(
      invalid_identity.decision,
      idx::PageExtentSummaryFallbackReason::invalid_identity_full_scan,
      "DPC-012 nil summary UUID was not rejected");

  auto tainted = Event(idx::PageExtentSummaryMaintenanceEventKind::insert_row,
                       metadata);
  tainted.after_row_present = true;
  tainted.after_row = Row(100, "010");
  tainted.after_row.parser_finality_authority_claimed = true;
  const auto row_tainted =
      idx::ApplyPageExtentSummaryMaintenanceEvent(metadata, format, tainted);
  RequireFallback(
      row_tainted.decision,
      idx::PageExtentSummaryFallbackReason::external_finality_authority_full_scan,
      "DPC-012 row-authority taint did not fail closed");
  Require(row_tainted.decision.restricted_repair_required,
          "DPC-012 row-authority taint did not require restricted repair");

  auto external = Event(idx::PageExtentSummaryMaintenanceEventKind::insert_row,
                        metadata);
  external.after_row_present = true;
  external.after_row = Row(100, "010");
  external.reference_finality_authority_claimed = true;
  const auto event_tainted =
      idx::ApplyPageExtentSummaryMaintenanceEvent(metadata, format, external);
  RequireFallback(
      event_tainted.decision,
      idx::PageExtentSummaryFallbackReason::external_finality_authority_full_scan,
      "DPC-012 event external finality claim did not fail closed");
}

}  // namespace

int main() {
  ValidateInsertUpdateDeleteMaintenance();
  ValidateBulkIngestAndRebuild();
  ValidateRepairAndCrashReopenClassification();
  ValidateIdentityAndAuthorityBoundaries();
  std::cout << kGateSearchKey << "=passed "
            << idx::kPageExtentSummaryMaintenanceSearchKey
            << "=dml_maintenance_rebuild_repair\n";
  return EXIT_SUCCESS;
}
