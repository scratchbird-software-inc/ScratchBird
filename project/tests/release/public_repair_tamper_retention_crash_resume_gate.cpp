// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_header.hpp"
#include "repair_event_ledger.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

using platform::TypedUuid;
using platform::UuidKind;
using platform::u64;

inline constexpr u64 kBaseMillis = 1771200000000ull;

bool Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool ContainsEvidence(const std::vector<std::string>& evidence,
                      std::string_view expected) {
  return std::find(evidence.begin(), evidence.end(), expected) !=
         evidence.end();
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
  u64 page_number = 410;
};

struct PhaseStep {
  db::RepairEventPhase phase = db::RepairEventPhase::unknown;
  std::string reason_code;
};

db::RepairEventRecord EventFor(const RepairFixture& fixture,
                               db::RepairEventPhase phase,
                               u64 sequence,
                               u64 previous_digest,
                               std::string reason_code) {
  db::RepairEventRecord event;
  event.sequence = sequence;
  event.ledger_epoch = 77;
  event.phase = phase;
  event.database_uuid = fixture.database_uuid;
  event.operation_uuid = fixture.operation_uuid;
  event.finding_uuid = fixture.finding_uuid;
  event.page_uuid = fixture.page_uuid;
  event.object_uuid = fixture.object_uuid;
  event.row_uuid = fixture.row_uuid;
  event.version_uuid = fixture.version_uuid;
  event.transaction_uuid = fixture.transaction_uuid;
  event.local_transaction_id = 9001;
  event.page_number = fixture.page_number;
  event.page_generation = 11;
  event.page_type = disk::PageType::row_data;
  event.observed_header_checksum = 0x7100ull + sequence;
  event.observed_body_checksum_low64 = 0x7200ull + sequence;
  event.observed_body_checksum_high64 = 0x7300ull + sequence;
  event.previous_event_digest = previous_digest;
  event.reason_code = std::move(reason_code);
  event.stable_detail = "repair_retention_crash_resume_public_gate";
  return event;
}

std::vector<PhaseStep> BaseRepairPhases() {
  return {{db::RepairEventPhase::finding_recorded, "repair_finding_recorded"},
          {db::RepairEventPhase::scan_admission, "repair_scan_admitted"},
          {db::RepairEventPhase::mutation_admission,
           "repair_mutation_admitted"},
          {db::RepairEventPhase::retention_hold_recorded,
           "retention_hold_recorded"},
          {db::RepairEventPhase::retention_purge_blocked,
           "retention_purge_blocked"}};
}

bool BuildLedger(const std::filesystem::path& ledger_path,
                 const RepairFixture& fixture,
                 const std::vector<PhaseStep>& phases,
                 db::RepairEventLedger* ledger) {
  std::filesystem::remove(ledger_path);
  u64 previous_digest = 0;
  for (std::size_t index = 0; index < phases.size(); ++index) {
    const auto appended = db::AppendRepairEventToLedger(
        ledger_path.string(),
        EventFor(fixture,
                 phases[index].phase,
                 static_cast<u64>(index + 1),
                 previous_digest,
                 phases[index].reason_code));
    if (!Expect(appended.ok(), "repair event should append durably")) {
      std::cerr << appended.diagnostic.diagnostic_code << '\n';
      return false;
    }
    previous_digest = appended.event.event_digest;
  }

  const auto loaded = db::LoadRepairEventLedger(ledger_path.string());
  if (!Expect(loaded.ok(), "repair ledger should load after phase appends")) {
    std::cerr << loaded.diagnostic.diagnostic_code << '\n';
    return false;
  }
  *ledger = loaded.ledger;
  return Expect(ledger->verified_append_only,
                "repair ledger chain should verify append-only");
}

std::vector<PhaseStep> StartedCrashResumePhases() {
  auto phases = BaseRepairPhases();
  phases.push_back({db::RepairEventPhase::crash_resume_started,
                    "crash_resume_started"});
  return phases;
}

std::vector<PhaseStep> ReplayCrashResumePhases() {
  auto phases = StartedCrashResumePhases();
  phases.push_back({db::RepairEventPhase::crash_resume_replay_admitted,
                    "crash_resume_replay_admitted"});
  return phases;
}

std::vector<PhaseStep> CompletedCrashResumePhases() {
  auto phases = ReplayCrashResumePhases();
  phases.push_back({db::RepairEventPhase::crash_resume_completed,
                    "crash_resume_completed"});
  return phases;
}

bool LedgerTamperProof(const std::filesystem::path& ledger_path,
                       const std::filesystem::path& tampered_path) {
  bool ok = true;
  std::ifstream in(ledger_path, std::ios::binary);
  ok = Expect(in.is_open(), "repair ledger should be readable for tamper proof") &&
       ok;
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  ok = Expect(!content.empty(), "repair ledger content should be present") && ok;
  const std::size_t pos = content.find("crash_resume_completed");
  ok = Expect(pos != std::string::npos,
              "repair ledger should contain completed crash-resume event") &&
       ok;
  if (!ok) {
    return false;
  }

  content[pos] = 'x';
  std::ofstream out(tampered_path, std::ios::binary | std::ios::trunc);
  out << content;
  out.close();

  const auto tampered = db::LoadRepairEventLedger(tampered_path.string());
  ok = Expect(!tampered.ok(),
              "tampered repair ledger should fail digest or phase validation") &&
       ok;
  return ok;
}

bool RetentionProof(const db::RepairEventLedger& ledger) {
  bool ok = true;
  db::RepairEventRetentionRequest base;
  base.ledger = ledger;
  base.now_epoch_millis = 1000;
  base.durable_retention_policy_loaded = true;
  base.purge_requested = true;

  auto legal_hold = base;
  legal_hold.legal_hold_active = true;
  const auto legal = db::EvaluateRepairEventRetention(legal_hold);
  ok = Expect(legal.ok() && legal.purge_blocked && !legal.purge_allowed,
              "legal hold should block repair ledger purge") &&
       ok;
  ok = Expect(legal.legal_hold_blocker,
              "retention decision should expose legal hold blocker") &&
       ok;
  ok = Expect(legal.tamper_chain_verified,
              "retention decision should prove verified tamper chain") &&
       ok;
  ok = Expect(!legal.repair_evidence_is_transaction_authority,
              "retention evidence must not become transaction authority") &&
       ok;
  ok = Expect(legal.diagnostic.diagnostic_code ==
                  "SB-REPAIR-RETENTION-PURGE-BLOCKED",
              "legal hold blocker diagnostic should be stable") &&
       ok;

  auto maintenance_hold = base;
  maintenance_hold.maintenance_hold_active = true;
  const auto maintenance = db::EvaluateRepairEventRetention(maintenance_hold);
  ok = Expect(maintenance.ok() && maintenance.maintenance_hold_blocker &&
                  maintenance.purge_blocked,
              "maintenance hold should block repair ledger purge") &&
       ok;

  auto future_deadline = base;
  future_deadline.retention_deadline_epoch_millis = 2000;
  const auto deadline = db::EvaluateRepairEventRetention(future_deadline);
  ok = Expect(deadline.ok() && deadline.retention_deadline_blocker &&
                  deadline.purge_blocked,
              "unexpired retention deadline should block repair ledger purge") &&
       ok;

  auto expired_deadline = base;
  expired_deadline.retention_deadline_epoch_millis = 999;
  const auto allowed = db::EvaluateRepairEventRetention(expired_deadline);
  ok = Expect(allowed.ok() && allowed.purge_allowed && !allowed.purge_blocked,
              "expired retention deadline with no holds should allow purge") &&
       ok;
  ok = Expect(ContainsEvidence(allowed.evidence, "purge_allowed=1"),
              "retention evidence should record purge allowance") &&
       ok;

  auto missing_policy = base;
  missing_policy.durable_retention_policy_loaded = false;
  const auto no_policy = db::EvaluateRepairEventRetention(missing_policy);
  ok = Expect(!no_policy.ok() &&
                  no_policy.diagnostic.diagnostic_code ==
                      "SB-REPAIR-RETENTION-POLICY-REQUIRED",
              "retention without durable policy should fail closed") &&
       ok;

  auto authority_drift = base;
  authority_drift.repair_evidence_is_transaction_authority = true;
  const auto refused = db::EvaluateRepairEventRetention(authority_drift);
  ok = Expect(!refused.ok() &&
                  refused.diagnostic.diagnostic_code ==
                      "SB-REPAIR-RETENTION-AUTHORITY-REFUSED",
              "retention should reject repair evidence authority drift") &&
       ok;

  auto forged = base;
  forged.ledger.events.back().event_digest ^= 0x1ull;
  const auto forged_decision = db::EvaluateRepairEventRetention(forged);
  ok = Expect(!forged_decision.ok() &&
                  forged_decision.diagnostic.diagnostic_code ==
                      "SB-REPAIR-RETENTION-LEDGER-UNVERIFIED",
              "retention should reject forged in-memory ledger state") &&
       ok;
  return ok;
}

bool CrashResumeProof(const db::RepairEventLedger& started,
                      const db::RepairEventLedger& replay_admitted,
                      const db::RepairEventLedger& completed) {
  bool ok = true;
  db::RepairCrashResumeRequest request;
  request.ledger = started;
  request.crash_recovery_open = true;
  request.durable_mga_inventory_authority = true;

  const auto started_decision =
      db::EvaluateRepairCrashResumeFromLedger(request);
  ok = Expect(started_decision.ok() && started_decision.resume_required &&
                  started_decision.replay_required &&
                  !started_decision.completed,
              "started repair crash-resume phase should require replay") &&
       ok;
  ok = Expect(started_decision.last_phase ==
                  db::RepairEventPhase::crash_resume_started,
              "started crash-resume last phase should be preserved") &&
       ok;
  ok = Expect(started_decision.tamper_chain_verified,
              "crash-resume decision should prove verified tamper chain") &&
       ok;
  ok = Expect(!started_decision.repair_evidence_is_recovery_authority,
              "repair ledger evidence must not become recovery authority") &&
       ok;

  request.ledger = replay_admitted;
  const auto replay_decision =
      db::EvaluateRepairCrashResumeFromLedger(request);
  ok = Expect(replay_decision.ok() && replay_decision.resume_required &&
                  replay_decision.replay_required &&
                  !replay_decision.completed,
              "replay-admitted repair phase should still require completion") &&
       ok;
  ok = Expect(replay_decision.last_phase ==
                  db::RepairEventPhase::crash_resume_replay_admitted,
              "replay-admitted last phase should be preserved") &&
       ok;

  request.ledger = completed;
  const auto completed_decision =
      db::EvaluateRepairCrashResumeFromLedger(request);
  ok = Expect(completed_decision.ok() &&
                  !completed_decision.resume_required &&
                  !completed_decision.replay_required &&
                  completed_decision.completed,
              "completed crash-resume phase should require no replay") &&
       ok;
  ok = Expect(completed_decision.last_phase ==
                  db::RepairEventPhase::crash_resume_completed,
              "completed last phase should be preserved") &&
       ok;
  ok = Expect(ContainsEvidence(completed_decision.evidence,
                               "repair_evidence_recovery_authority=false"),
              "crash-resume evidence should record non-authority") &&
       ok;

  auto closed_recovery = request;
  closed_recovery.ledger = started;
  closed_recovery.crash_recovery_open = false;
  const auto closed = db::EvaluateRepairCrashResumeFromLedger(closed_recovery);
  ok = Expect(closed.ok() && !closed.resume_required && closed.completed,
              "closed crash recovery should not request repair replay") &&
       ok;

  auto missing_mga = request;
  missing_mga.ledger = started;
  missing_mga.durable_mga_inventory_authority = false;
  const auto no_mga = db::EvaluateRepairCrashResumeFromLedger(missing_mga);
  ok = Expect(!no_mga.ok() &&
                  no_mga.diagnostic.diagnostic_code ==
                      "SB-REPAIR-CRASH-RESUME-AUTHORITY-REFUSED",
              "crash resume should require durable MGA inventory authority") &&
       ok;

  auto recovery_authority_drift = request;
  recovery_authority_drift.ledger = started;
  recovery_authority_drift.repair_evidence_is_recovery_authority = true;
  const auto authority_refused =
      db::EvaluateRepairCrashResumeFromLedger(recovery_authority_drift);
  ok = Expect(!authority_refused.ok() &&
                  authority_refused.diagnostic.diagnostic_code ==
                      "SB-REPAIR-CRASH-RESUME-AUTHORITY-REFUSED",
              "crash resume should reject repair evidence recovery authority") &&
       ok;

  auto forged = request;
  forged.ledger = started;
  forged.ledger.last_event_digest ^= 0x1ull;
  const auto forged_decision =
      db::EvaluateRepairCrashResumeFromLedger(forged);
  ok = Expect(!forged_decision.ok() &&
                  forged_decision.diagnostic.diagnostic_code ==
                      "SB-REPAIR-CRASH-RESUME-LEDGER-UNVERIFIED",
              "crash resume should reject forged in-memory ledger state") &&
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
  db::RepairEventLedger started_ledger;
  db::RepairEventLedger replay_ledger;
  db::RepairEventLedger completed_ledger;

  bool ok = BuildLedger(work_dir / "repair-started.sbrel",
                        fixture,
                        StartedCrashResumePhases(),
                        &started_ledger);
  ok = BuildLedger(work_dir / "repair-replay.sbrel",
                   fixture,
                   ReplayCrashResumePhases(),
                   &replay_ledger) &&
       ok;
  const auto completed_path = work_dir / "repair-completed.sbrel";
  ok = BuildLedger(completed_path,
                   fixture,
                   CompletedCrashResumePhases(),
                   &completed_ledger) &&
       ok;

  ok = LedgerTamperProof(completed_path,
                         work_dir / "repair-completed-tampered.sbrel") &&
       ok;
  ok = RetentionProof(completed_ledger) && ok;
  ok = CrashResumeProof(started_ledger, replay_ledger, completed_ledger) && ok;

  std::filesystem::remove_all(work_dir);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
