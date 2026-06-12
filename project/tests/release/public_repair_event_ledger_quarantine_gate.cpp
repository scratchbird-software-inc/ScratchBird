// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "damaged_page_quarantine.hpp"
#include "page_header.hpp"
#include "repair_event_ledger.hpp"
#include "row_data_page.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

using platform::TypedUuid;
using platform::UuidKind;
using platform::byte;
using platform::u64;

inline constexpr u64 kBaseMillis = 1770800000000ull;
inline constexpr platform::u32 kPageSize = 8192;

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

struct RepairFixture {
  TypedUuid database_uuid = MakeUuid(UuidKind::database, 1);
  TypedUuid operation_uuid = MakeUuid(UuidKind::object, 2);
  TypedUuid finding_uuid = MakeUuid(UuidKind::object, 3);
  TypedUuid page_uuid = MakeUuid(UuidKind::page, 4);
  TypedUuid object_uuid = MakeUuid(UuidKind::object, 5);
  TypedUuid row_uuid = MakeUuid(UuidKind::row, 6);
  TypedUuid version_uuid = MakeUuid(UuidKind::row, 7);
  TypedUuid transaction_uuid = MakeUuid(UuidKind::transaction, 8);
  u64 page_number = 42;
};

db::RepairEventRecord EventFor(const RepairFixture& fixture,
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
  event.object_uuid = fixture.object_uuid;
  event.row_uuid = fixture.row_uuid;
  event.version_uuid = fixture.version_uuid;
  event.transaction_uuid = fixture.transaction_uuid;
  event.local_transaction_id = 77;
  event.page_number = fixture.page_number;
  event.page_generation = 9;
  event.page_type = disk::PageType::row_data;
  event.observed_header_checksum = 0x1000ull + sequence;
  event.observed_body_checksum_low64 = 0x2000ull + sequence;
  event.observed_body_checksum_high64 = 0x3000ull + sequence;
  event.previous_event_digest = previous_digest;
  event.reason_code = std::move(reason_code);
  event.stable_detail = "page_body_integrity_refusal";
  return event;
}

db::RepairAccessRequest AccessFor(const RepairFixture& fixture,
                                  db::RepairAccessIntent intent) {
  db::RepairAccessRequest request;
  request.intent = intent;
  request.operation_uuid = fixture.operation_uuid;
  request.finding_uuid = fixture.finding_uuid;
  request.page_uuid = fixture.page_uuid;
  request.page_number = fixture.page_number;
  request.durable_mga_inventory_authority = true;
  return request;
}

bool LedgerAppendAndAccessProof(const std::filesystem::path& ledger_path,
                                const RepairFixture& fixture,
                                u64* scan_digest,
                                u64* mutation_digest) {
  bool ok = true;
  const auto finding = db::AppendRepairEventToLedger(
      ledger_path.string(),
      EventFor(fixture,
               db::RepairEventPhase::finding_recorded,
               1,
               0,
               "damaged_page_finding"));
  ok = Expect(finding.ok(), "finding event should append durably") && ok;

  const auto scan = db::AppendRepairEventToLedger(
      ledger_path.string(),
      EventFor(fixture,
               db::RepairEventPhase::scan_admission,
               2,
               finding.event.event_digest,
               "repair_scan_admitted"));
  ok = Expect(scan.ok(), "scan admission event should append durably") && ok;
  if (!ok) {
    return false;
  }
  *scan_digest = scan.event.event_digest;

  const auto loaded = db::LoadRepairEventLedger(ledger_path.string());
  ok = Expect(loaded.ok(), "repair ledger should load after append") && ok;
  ok = Expect(loaded.ledger.verified_append_only,
              "repair ledger should verify append-only chain") && ok;
  ok = Expect(loaded.ledger.events.size() == 2,
              "repair ledger should contain finding and scan events") && ok;
  ok = Expect(loaded.ledger.last_event_digest == scan.event.event_digest,
              "repair ledger last digest should match scan event") && ok;

  const auto scan_access =
      db::AdmitRepairAccessFromLedger(loaded.ledger,
                                      AccessFor(fixture,
                                                db::RepairAccessIntent::
                                                    repair_scan));
  ok = Expect(scan_access.ok() && scan_access.scan_allowed,
              "repair scan should require and find prior scan event") && ok;
  ok = Expect(scan_access.prior_event_digest == scan.event.event_digest,
              "scan access should cite persisted scan event digest") && ok;

  const auto mutation_before_event =
      db::AdmitRepairAccessFromLedger(loaded.ledger,
                                      AccessFor(fixture,
                                                db::RepairAccessIntent::
                                                    repair_mutation));
  ok = Expect(!mutation_before_event.ok(),
              "repair mutation should fail before mutation event is durable") &&
       ok;
  ok = Expect(mutation_before_event.diagnostic.diagnostic_code ==
                  "SB-REPAIR-ACCESS-PRIOR-EVENT-REQUIRED",
              "missing mutation event diagnostic should be stable") && ok;

  const auto mutation = db::AppendRepairEventToLedger(
      ledger_path.string(),
      EventFor(fixture,
               db::RepairEventPhase::mutation_admission,
               3,
               scan.event.event_digest,
               "repair_mutation_admitted"));
  ok = Expect(mutation.ok(),
              "mutation admission event should append durably") && ok;
  if (!ok) {
    return false;
  }
  *mutation_digest = mutation.event.event_digest;

  const auto quarantined = db::AppendRepairEventToLedger(
      ledger_path.string(),
      EventFor(fixture,
               db::RepairEventPhase::page_quarantined,
               4,
               mutation.event.event_digest,
               "page_quarantined"));
  ok = Expect(quarantined.ok(),
              "page quarantine event should append durably") && ok;

  const auto loaded_after_quarantine =
      db::LoadRepairEventLedger(ledger_path.string());
  ok = Expect(loaded_after_quarantine.ok(),
              "repair ledger should reload after quarantine") && ok;
  ok = Expect(loaded_after_quarantine.ledger.events.size() == 4,
              "repair ledger should contain four ordered events") && ok;

  const auto mutation_access =
      db::AdmitRepairAccessFromLedger(loaded_after_quarantine.ledger,
                                      AccessFor(fixture,
                                                db::RepairAccessIntent::
                                                    repair_mutation));
  ok = Expect(mutation_access.ok() && mutation_access.mutation_allowed,
              "repair mutation should require and find mutation event") && ok;

  const auto normal_access =
      db::AdmitRepairAccessFromLedger(loaded_after_quarantine.ledger,
                                      AccessFor(fixture,
                                                db::RepairAccessIntent::
                                                    normal_access));
  ok = Expect(!normal_access.ok(),
              "normal page access should fail while page is quarantined") && ok;
  ok = Expect(normal_access.diagnostic.diagnostic_code ==
                  "SB-REPAIR-ACCESS-PAGE-QUARANTINED",
              "quarantined normal access diagnostic should be stable") && ok;
  return ok;
}

bool LedgerRefusalProof(const std::filesystem::path& ledger_path,
                        const RepairFixture& fixture) {
  bool ok = true;
  const auto loaded = db::LoadRepairEventLedger(ledger_path.string());
  ok = Expect(loaded.ok(), "repair ledger should load for refusal proof") && ok;
  if (!ok) {
    return false;
  }

  auto sequence_gap = EventFor(fixture,
                               db::RepairEventPhase::scan_admission,
                               loaded.ledger.last_sequence + 2,
                               loaded.ledger.last_event_digest,
                               "sequence_gap");
  const auto sequence_gap_result =
      db::AppendRepairEventToLedger(ledger_path.string(), sequence_gap);
  ok = Expect(!sequence_gap_result.ok(),
              "repair ledger should reject sequence gaps") && ok;
  ok = Expect(sequence_gap_result.diagnostic.diagnostic_code ==
                  "SB-REPAIR-EVENT-LEDGER-CHAIN-INVALID",
              "sequence gap diagnostic should be stable") && ok;

  auto finality_authority = EventFor(fixture,
                                     db::RepairEventPhase::scan_admission,
                                     loaded.ledger.last_sequence + 1,
                                     loaded.ledger.last_event_digest,
                                     "finality_authority_refused");
  finality_authority.authority
      .repair_evidence_is_transaction_finality_authority = true;
  const auto finality_result =
      db::AppendRepairEventToLedger(ledger_path.string(), finality_authority);
  ok = Expect(!finality_result.ok(),
              "repair event should not claim transaction finality authority") &&
       ok;
  ok = Expect(finality_result.diagnostic.diagnostic_code ==
                  "SB-REPAIR-EVENT-AUTHORITY-REFUSED",
              "finality authority refusal diagnostic should be stable") && ok;

  auto reference_authority = EventFor(fixture,
                                  db::RepairEventPhase::scan_admission,
                                  loaded.ledger.last_sequence + 1,
                                  loaded.ledger.last_event_digest,
                                  "reference_flag_refused");
  reference_authority.authority.parser_or_reference_authority = true;
  const auto reference_result =
      db::AppendRepairEventToLedger(ledger_path.string(), reference_authority);
  ok = Expect(!reference_result.ok(),
              "repair event should reject parser or reference authority") && ok;

  const auto still_loaded = db::LoadRepairEventLedger(ledger_path.string());
  ok = Expect(still_loaded.ok(),
              "repair ledger should remain loadable after refused appends") && ok;
  ok = Expect(still_loaded.ledger.events.size() == loaded.ledger.events.size(),
              "refused events should not mutate append-only ledger") && ok;
  return ok;
}

bool LedgerTamperProof(const std::filesystem::path& ledger_path,
                       const std::filesystem::path& tampered_path) {
  bool ok = true;
  std::ifstream in(ledger_path, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  ok = Expect(!content.empty(), "repair ledger content should be present") && ok;
  const std::size_t pos = content.find("page_quarantined");
  ok = Expect(pos != std::string::npos,
              "repair ledger should contain quarantine event") && ok;
  if (!ok) {
    return false;
  }
  content[pos] = 'x';
  std::ofstream out(tampered_path, std::ios::binary | std::ios::trunc);
  out << content;
  out.close();

  const auto tampered = db::LoadRepairEventLedger(tampered_path.string());
  ok = Expect(!tampered.ok(),
              "tampered repair ledger should fail chain or digest validation") &&
       ok;
  return ok;
}

disk::SerializedPageHeader HeaderFor(disk::PageType page_type,
                                     const RepairFixture& fixture) {
  disk::PageHeader header;
  header.page_size = kPageSize;
  header.page_type = page_type;
  header.database_uuid = fixture.database_uuid.value;
  header.filespace_uuid = MakeUuid(UuidKind::filespace, 20).value;
  header.page_uuid = fixture.page_uuid.value;
  header.page_number = fixture.page_number;
  header.page_generation = 9;
  const auto serialized = disk::SerializePageHeader(header);
  return serialized.ok() ? serialized.serialized : disk::SerializedPageHeader{};
}

page::RowDataPageBody RowBody(const RepairFixture& fixture) {
  page::RowDataPageBody body;
  body.relation_uuid = fixture.object_uuid;
  body.segment_id = 1;
  body.segment_generation = 1;
  body.page_number = fixture.page_number;
  body.page_generation = 9;
  page::RowDataRecord row;
  row.row_uuid = fixture.row_uuid;
  row.transaction_uuid = fixture.transaction_uuid;
  row.local_transaction_id = 77;
  row.stable_slot_id = 10;
  row.row_version = 1;
  body.rows.push_back(row);
  return body;
}

page::PageBodyAgreementResult Agreement(disk::PageType page_type,
                                        const RepairFixture& fixture,
                                        const std::vector<byte>& body) {
  page::PageBodyAgreementRequest request;
  request.header = HeaderFor(page_type, fixture);
  request.body = body;
  request.checksum_profile = page::PageBodyChecksumProfile::strong;
  return page::ValidatePageBodyAgreement(request);
}

page::DamagedPageEvidence EvidenceFor(
    const page::PageBodyAgreementResult& agreement,
    u64 repair_event_digest,
    bool event_persisted) {
  page::DamagedPageEvidence evidence =
      page::MakeDamagedPageEvidenceFromAgreement(agreement);
  evidence.durable_mga_inventory_authority_available = true;
  evidence.normal_mga_visibility_recheck_available = false;
  evidence.repair_event_persisted_before_access = event_persisted;
  evidence.repair_event_digest = repair_event_digest;
  evidence.safe_quarantine_boundary_available = true;
  return evidence;
}

bool PageQuarantineProof(const RepairFixture& fixture,
                         u64 scan_digest,
                         u64 mutation_digest) {
  bool ok = true;
  const auto built = page::BuildRowDataPageBody(RowBody(fixture), kPageSize);
  ok = Expect(built.ok(), "row body should build for quarantine proof") && ok;
  if (!ok) {
    return false;
  }

  const auto clean_agreement =
      Agreement(disk::PageType::row_data, fixture, built.serialized);
  ok = Expect(clean_agreement.ok(),
              "clean row page should pass body/header agreement") && ok;
  page::DamagedPageQuarantineRequest clean_request;
  clean_request.intent = page::DamagedPageAccessIntent::normal_access;
  clean_request.page_number = fixture.page_number;
  clean_request.evidence = EvidenceFor(clean_agreement, 0, false);
  const auto clean =
      page::ClassifyDamagedPageAccess(clean_request);
  ok = Expect(clean.ok() && clean.normal_access_allowed,
              "clean page should allow normal access under MGA authority") && ok;

  const auto damaged_agreement =
      Agreement(disk::PageType::index_hash, fixture, built.serialized);
  ok = Expect(!damaged_agreement.ok(),
              "row body should not agree with hash page header") && ok;

  page::DamagedPageQuarantineRequest missing_event;
  missing_event.intent = page::DamagedPageAccessIntent::normal_access;
  missing_event.page_number = fixture.page_number;
  missing_event.evidence = EvidenceFor(damaged_agreement, 0, false);
  const auto missing_event_decision =
      page::ClassifyDamagedPageAccess(missing_event);
  ok = Expect(!missing_event_decision.ok(),
              "damaged page should block without prior repair event") && ok;
  ok = Expect(missing_event_decision.diagnostic.diagnostic_code ==
                  "SB-DAMAGED-PAGE-REPAIR-EVENT-REQUIRED",
              "missing repair event diagnostic should be stable") && ok;

  page::DamagedPageQuarantineRequest normal_damaged;
  normal_damaged.intent = page::DamagedPageAccessIntent::normal_access;
  normal_damaged.page_number = fixture.page_number;
  normal_damaged.evidence = EvidenceFor(damaged_agreement, scan_digest, true);
  const auto quarantined =
      page::ClassifyDamagedPageAccess(normal_damaged);
  ok = Expect(quarantined.ok() && quarantined.quarantine_required &&
                  !quarantined.normal_access_allowed,
              "damaged normal access should classify quarantine before use") &&
       ok;

  page::DamagedPageQuarantineRequest scan;
  scan.intent = page::DamagedPageAccessIntent::repair_scan;
  scan.page_number = fixture.page_number;
  scan.evidence = EvidenceFor(damaged_agreement, scan_digest, true);
  const auto scan_decision = page::ClassifyDamagedPageAccess(scan);
  ok = Expect(scan_decision.ok() && scan_decision.repair_scan_allowed &&
                  scan_decision.quarantine_required,
              "damaged repair scan should be allowed only inside quarantine") &&
       ok;

  page::DamagedPageQuarantineRequest mutation_without_recheck;
  mutation_without_recheck.intent = page::DamagedPageAccessIntent::repair_mutation;
  mutation_without_recheck.page_number = fixture.page_number;
  mutation_without_recheck.evidence =
      EvidenceFor(damaged_agreement, mutation_digest, true);
  const auto missing_recheck =
      page::ClassifyDamagedPageAccess(mutation_without_recheck);
  ok = Expect(!missing_recheck.ok(),
              "damaged repair mutation should require normal MGA recheck") && ok;
  ok = Expect(missing_recheck.diagnostic.diagnostic_code ==
                  "SB-DAMAGED-PAGE-MGA-RECHECK-REQUIRED",
              "missing MGA recheck diagnostic should be stable") && ok;

  auto mutation_evidence = EvidenceFor(damaged_agreement, mutation_digest, true);
  mutation_evidence.normal_mga_visibility_recheck_available = true;
  page::DamagedPageQuarantineRequest mutation;
  mutation.intent = page::DamagedPageAccessIntent::repair_mutation;
  mutation.page_number = fixture.page_number;
  mutation.evidence = mutation_evidence;
  const auto mutation_decision = page::ClassifyDamagedPageAccess(mutation);
  ok = Expect(mutation_decision.ok() &&
                  mutation_decision.repair_mutation_allowed &&
                  mutation_decision.quarantine_required,
              "damaged repair mutation should require event plus MGA recheck") &&
       ok;

  mutation_evidence.repair_evidence_is_transaction_authority = true;
  mutation.evidence = mutation_evidence;
  const auto authority_drift =
      page::ClassifyDamagedPageAccess(mutation);
  ok = Expect(!authority_drift.ok(),
              "damaged page classifier should reject repair evidence authority drift") &&
       ok;
  ok = Expect(authority_drift.diagnostic.diagnostic_code ==
                  "SB-DAMAGED-PAGE-AUTHORITY-REFUSED",
              "repair evidence authority drift diagnostic should be stable") &&
       ok;
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

  const RepairFixture fixture;
  const auto ledger_path = work_dir / "repair-ledger.sbrel";
  const auto tampered_path = work_dir / "repair-ledger-tampered.sbrel";
  u64 scan_digest = 0;
  u64 mutation_digest = 0;

  bool ok = LedgerAppendAndAccessProof(ledger_path,
                                       fixture,
                                       &scan_digest,
                                       &mutation_digest);
  ok = LedgerRefusalProof(ledger_path, fixture) && ok;
  ok = LedgerTamperProof(ledger_path, tampered_path) && ok;
  ok = PageQuarantineProof(fixture, scan_digest, mutation_digest) && ok;

  std::filesystem::remove_all(work_dir);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
