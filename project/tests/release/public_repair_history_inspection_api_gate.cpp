// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/repair_history_api.hpp"
#include "repair_event_ledger.hpp"
#include "repair_history_inspection.hpp"
#include "row_version.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

inline constexpr u64 kBaseMillis = 1770900000000ull;

bool Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

struct Fixture {
  TypedUuid database_uuid = MakeUuid(UuidKind::database, 1);
  TypedUuid operation_uuid = MakeUuid(UuidKind::object, 2);
  TypedUuid finding_uuid = MakeUuid(UuidKind::object, 3);
  TypedUuid page_uuid = MakeUuid(UuidKind::page, 4);
  TypedUuid table_uuid = MakeUuid(UuidKind::object, 5);
  TypedUuid row_uuid = MakeUuid(UuidKind::row, 6);
  TypedUuid version_one_uuid = MakeUuid(UuidKind::row, 7);
  TypedUuid version_two_uuid = MakeUuid(UuidKind::row, 8);
  TypedUuid transaction_one_uuid = MakeUuid(UuidKind::transaction, 9);
  TypedUuid transaction_two_uuid = MakeUuid(UuidKind::transaction, 10);
  u64 page_number = 88;
};

txn::TransactionIdentity TransactionIdentity(TypedUuid transaction_uuid,
                                             u64 local_id) {
  txn::TransactionIdentity identity;
  identity.local_id = txn::MakeLocalTransactionId(local_id);
  identity.transaction_uuid = transaction_uuid;
  identity.scope = txn::TransactionScope::local_node;
  return identity;
}

txn::RowVersionMetadata Metadata(const Fixture& fixture,
                                 TypedUuid transaction_uuid,
                                 u64 local_id,
                                 u64 sequence,
                                 txn::RowVersionState row_state,
                                 txn::TransactionState transaction_state) {
  txn::RowVersionMetadata metadata;
  metadata.identity.row.row_uuid = fixture.row_uuid;
  metadata.identity.creator_transaction =
      TransactionIdentity(transaction_uuid, local_id);
  metadata.identity.version_sequence = sequence;
  metadata.state = row_state;
  metadata.creator_transaction_state = transaction_state;
  metadata.payload_present =
      row_state != txn::RowVersionState::rolled_back &&
      row_state != txn::RowVersionState::delete_marker;
  return metadata;
}

db::RepairEventRecord EventFor(const Fixture& fixture,
                               db::RepairEventPhase phase,
                               u64 sequence,
                               u64 previous_digest,
                               std::string reason_code) {
  db::RepairEventRecord event;
  event.sequence = sequence;
  event.ledger_epoch = 1;
  event.phase = phase;
  event.database_uuid = fixture.database_uuid;
  event.operation_uuid = fixture.operation_uuid;
  event.finding_uuid = fixture.finding_uuid;
  event.page_uuid = fixture.page_uuid;
  event.object_uuid = fixture.table_uuid;
  event.row_uuid = fixture.row_uuid;
  event.version_uuid = fixture.version_two_uuid;
  event.transaction_uuid = fixture.transaction_two_uuid;
  event.local_transaction_id = 2;
  event.page_number = fixture.page_number;
  event.page_generation = 3;
  event.page_type = disk::PageType::row_data;
  event.observed_header_checksum = 0x4000ull + sequence;
  event.observed_body_checksum_low64 = 0x5000ull + sequence;
  event.observed_body_checksum_high64 = 0x6000ull + sequence;
  event.previous_event_digest = previous_digest;
  event.reason_code = std::move(reason_code);
  event.stable_detail = "repair_history_fixture";
  return event;
}

bool WriteLedger(const std::filesystem::path& ledger_path,
                 const Fixture& fixture) {
  bool ok = true;
  const auto finding = db::AppendRepairEventToLedger(
      ledger_path.string(),
      EventFor(fixture,
               db::RepairEventPhase::finding_recorded,
               1,
               0,
               "damaged_page_finding"));
  ok = Expect(finding.ok(), "PCR-075 finding event should append") && ok;
  const auto scan = db::AppendRepairEventToLedger(
      ledger_path.string(),
      EventFor(fixture,
               db::RepairEventPhase::scan_admission,
               2,
               finding.event.event_digest,
               "repair_scan_admitted"));
  ok = Expect(scan.ok(), "PCR-075 scan event should append") && ok;
  const auto quarantine = db::AppendRepairEventToLedger(
      ledger_path.string(),
      EventFor(fixture,
               db::RepairEventPhase::page_quarantined,
               3,
               scan.event.event_digest,
               "page_quarantined"));
  ok = Expect(quarantine.ok(), "PCR-075 quarantine event should append") && ok;
  return ok;
}

db::RepairHistoryInspectionRequest InspectionFixture(const Fixture& fixture) {
  db::RepairHistoryInspectionRequest request;
  request.durable_mga_inventory_authority = true;

  db::RepairOrdinaryVersionRecord first;
  first.metadata = Metadata(fixture,
                            fixture.transaction_one_uuid,
                            1,
                            1,
                            txn::RowVersionState::committed,
                            txn::TransactionState::committed);
  first.metadata.chain.next_version_sequence = 2;
  first.metadata.chain.next_version_uuid = fixture.version_two_uuid;
  first.version_uuid = fixture.version_one_uuid;
  first.page_uuid = fixture.page_uuid;
  first.page_number = fixture.page_number;
  request.ordinary_versions.push_back(first);

  db::RepairOrdinaryVersionRecord second;
  second.metadata = Metadata(fixture,
                             fixture.transaction_two_uuid,
                             2,
                             2,
                             txn::RowVersionState::committed,
                             txn::TransactionState::committed);
  second.metadata.chain.previous_version_sequence = 1;
  second.metadata.chain.previous_version_uuid = fixture.version_one_uuid;
  second.version_uuid = fixture.version_two_uuid;
  second.page_uuid = fixture.page_uuid;
  second.page_number = fixture.page_number;
  request.ordinary_versions.push_back(second);

  db::RepairArchiveEntry entry;
  entry.row_uuid = fixture.row_uuid;
  entry.version_uuid = fixture.version_one_uuid;
  entry.page_uuid = fixture.page_uuid;
  entry.object_uuid = fixture.table_uuid;
  entry.page_number = fixture.page_number;
  entry.version_sequence = 1;
  entry.local_transaction_id = 1;
  entry.archive_location_class = "local_archive_entry";
  entry.archive_manifest_digest = "archive_digest_pcr075";
  entry.payload_present = false;
  request.archive_entries.push_back(entry);

  db::RepairSalvageEvidence salvage;
  salvage.finding_uuid = fixture.finding_uuid;
  salvage.page_uuid = fixture.page_uuid;
  salvage.row_uuid = fixture.row_uuid;
  salvage.version_uuid = fixture.version_two_uuid;
  salvage.page_number = fixture.page_number;
  salvage.salvage_class = "uncertain_review_only";
  salvage.uncertain = true;
  request.salvage_evidence.push_back(salvage);

  db::RepairDiagnosticEvidence diagnostic;
  diagnostic.row_uuid = fixture.row_uuid;
  diagnostic.page_uuid = fixture.page_uuid;
  diagnostic.page_number = fixture.page_number;
  diagnostic.diagnostic_code = "SB-REPAIR-HISTORY-DATA-LOSS-POSSIBLE";
  diagnostic.message_key = "repair.history.data_loss_possible";
  diagnostic.detail = "archive_payload_absent";
  request.diagnostics.push_back(diagnostic);
  return request;
}

api::EngineRequestContext AuthorizedContext(const std::filesystem::path& work_dir,
                                            const Fixture& fixture) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "pcr075-repair-history";
  context.database_path = (work_dir / "pcr075.sbdb").string();
  context.database_uuid.canonical = uuid::UuidToString(fixture.database_uuid.value);
  context.principal_uuid.canonical =
      uuid::UuidToString(MakeUuid(UuidKind::principal, 20).value);
  context.security_context_present = true;
  context.catalog_generation_id = 5;
  context.security_epoch = 7;
  context.resource_epoch = 9;
  context.trace_tags.push_back("security.bootstrap");
  return context;
}

std::string FieldValue(const api::EngineRowValue& row, std::string_view name) {
  for (const auto& [field_name, value] : row.fields) {
    if (field_name == name) {
      return value.encoded_value;
    }
  }
  return {};
}

const api::EngineRowValue* RowByKind(const api::EngineApiResult& result,
                                     std::string_view kind) {
  for (const auto& row : result.result_shape.rows) {
    if (FieldValue(row, "record_kind") == kind) {
      return &row;
    }
  }
  return nullptr;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

bool StorageInspectionProof(const Fixture& fixture,
                            const std::filesystem::path& ledger_path) {
  bool ok = true;
  auto request = InspectionFixture(fixture);
  const auto loaded = db::LoadRepairEventLedger(ledger_path.string());
  ok = Expect(loaded.ok(), "PCR-075 ledger should load for storage proof") && ok;
  request.repair_events = loaded.ledger.events;

  const auto inspected = db::InspectRepairHistory(request);
  ok = Expect(inspected.ok(), "storage repair history inspection should pass") && ok;
  ok = Expect(inspected.ordinary_version_count == 2,
              "storage inspection should walk ordinary versions") && ok;
  ok = Expect(inspected.archive_entry_count == 1,
              "storage inspection should walk archive entries") && ok;
  ok = Expect(inspected.repair_event_count == 3,
              "storage inspection should walk repair events") && ok;
  ok = Expect(inspected.salvage_evidence_count == 1,
              "storage inspection should walk salvage evidence") && ok;
  ok = Expect(inspected.diagnostic_count == 1,
              "storage inspection should include diagnostics") && ok;
  ok = Expect(inspected.quarantine_present,
              "storage inspection should report quarantine") && ok;
  ok = Expect(inspected.data_loss_possible && inspected.restore_required,
              "storage inspection should assess data loss and restore-required") &&
       ok;
  ok = Expect(!inspected.repair_evidence_is_transaction_authority,
              "repair inspection evidence must not become transaction authority") &&
       ok;

  auto filtered = request;
  filtered.row_uuid_filter = fixture.row_uuid;
  filtered.page_number_filter = fixture.page_number;
  const auto filtered_result = db::InspectRepairHistory(filtered);
  ok = Expect(filtered_result.ok(),
              "row/page filtered repair history inspection should pass") && ok;
  ok = Expect(filtered_result.rows.size() == inspected.rows.size(),
              "matching row/page filter should retain fixture rows") && ok;

  auto drift = request;
  drift.repair_evidence_is_transaction_authority = true;
  const auto drift_result = db::InspectRepairHistory(drift);
  ok = Expect(!drift_result.ok(),
              "storage inspection should reject repair evidence authority drift") &&
       ok;
  ok = Expect(drift_result.diagnostic.diagnostic_code ==
                  "SB-REPAIR-HISTORY-AUTHORITY-REFUSED",
              "storage authority drift diagnostic should be stable") && ok;

  auto salvage_promotion = request;
  salvage_promotion.salvage_evidence.front().payload_promoted_to_committed = true;
  const auto salvage_result = db::InspectRepairHistory(salvage_promotion);
  ok = Expect(!salvage_result.ok(),
              "uncertain salvage must not be promoted to committed data") && ok;
  ok = Expect(salvage_result.diagnostic.diagnostic_code ==
                  "SB-REPAIR-HISTORY-SALVAGE-INVALID",
              "salvage promotion diagnostic should be stable") && ok;
  return ok;
}

bool EngineInspectionProof(const Fixture& fixture,
                           const std::filesystem::path& work_dir,
                           const std::filesystem::path& ledger_path) {
  bool ok = true;
  api::EngineInspectRepairHistoryRequest request;
  request.context = AuthorizedContext(work_dir, fixture);
  request.inspection = InspectionFixture(fixture);
  request.load_repair_ledger_from_path = true;
  request.repair_ledger_path = ledger_path.string();

  const auto result = api::EngineInspectRepairHistory(request);
  ok = Expect(result.ok && result.repair_history_ready,
              "engine repair history inspection should pass") && ok;
  ok = Expect(result.ordinary_version_count == 2,
              "engine inspection should count ordinary versions") && ok;
  ok = Expect(result.archive_entry_count == 1,
              "engine inspection should count archive entries") && ok;
  ok = Expect(result.repair_event_count == 3,
              "engine inspection should count ledger events") && ok;
  ok = Expect(result.salvage_evidence_count == 1,
              "engine inspection should count salvage evidence") && ok;
  ok = Expect(result.quarantine_present && result.restore_required,
              "engine inspection should report quarantine and restore-required") &&
       ok;
  ok = Expect(!result.repair_evidence_is_transaction_authority,
              "engine repair evidence should remain non-authoritative") && ok;
  ok = Expect(HasEvidence(result,
                          "durable_mga_inventory_authority",
                          "true"),
              "engine result should cite MGA inventory authority") && ok;
  ok = Expect(HasEvidence(result,
                          "repair_evidence_transaction_authority",
                          "false"),
              "engine result should cite repair non-authority") && ok;
  ok = Expect(RowByKind(result, "ordinary_version") != nullptr,
              "engine result should include ordinary version row") && ok;
  ok = Expect(RowByKind(result, "archive_entry") != nullptr,
              "engine result should include archive entry row") && ok;
  ok = Expect(RowByKind(result, "quarantine") != nullptr,
              "engine result should include quarantine row") && ok;
  ok = Expect(RowByKind(result, "salvage_evidence") != nullptr,
              "engine result should include salvage evidence row") && ok;
  const auto* assessment = RowByKind(result, "data_loss_assessment");
  ok = Expect(assessment != nullptr,
              "engine result should include data-loss assessment row") && ok;
  if (assessment != nullptr) {
    ok = Expect(FieldValue(*assessment, "data_loss_class") == "restore_required",
                "data-loss assessment should be restore_required") && ok;
  }

  auto unauthorized = request;
  unauthorized.context.trace_tags.clear();
  const auto denied = api::EngineInspectRepairHistory(unauthorized);
  ok = Expect(!denied.ok,
              "engine repair history inspection should require authorization") &&
       ok;
  ok = Expect(!denied.diagnostics.empty() &&
                  denied.diagnostics.front().code ==
                      "SECURITY.AUTHORIZATION.DENIED",
              "engine authorization diagnostic should be stable") && ok;

  auto missing_ledger = request;
  missing_ledger.repair_ledger_path = (work_dir / "missing.sbrel").string();
  const auto missing = api::EngineInspectRepairHistory(missing_ledger);
  ok = Expect(missing.ok,
              "missing optional repair ledger should produce empty ledger inspection") &&
       ok;
  ok = Expect(missing.repair_event_count == 0,
              "missing repair ledger path should not fabricate repair events") && ok;

  auto authority_drift = request;
  authority_drift.inspection.parser_or_reference_authority = true;
  const auto drift = api::EngineInspectRepairHistory(authority_drift);
  ok = Expect(!drift.ok,
              "engine inspection should reject parser or reference authority") && ok;
  ok = Expect(!drift.diagnostics.empty() &&
                  drift.diagnostics.front().code ==
                      "SB-REPAIR-HISTORY-AUTHORITY-REFUSED",
              "engine authority drift diagnostic should be stable") && ok;
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "work directory argument required\n";
    return EXIT_FAILURE;
  }

  const std::filesystem::path work_dir = argv[1];
  std::filesystem::remove_all(work_dir);
  std::filesystem::create_directories(work_dir);
  const auto ledger_path = work_dir / "repair-history.sbrel";
  const Fixture fixture;

  bool ok = WriteLedger(ledger_path, fixture);
  ok = StorageInspectionProof(fixture, ledger_path) && ok;
  ok = EngineInspectionProof(fixture, work_dir, ledger_path) && ok;

  std::filesystem::remove_all(work_dir);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
