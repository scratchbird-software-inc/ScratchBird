// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_dirty_manifest.hpp"
#include "uuid.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

scratchbird::core::platform::TypedUuid Id(scratchbird::core::platform::UuidKind kind,
                                          scratchbird::core::platform::u64 seed) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, seed);
  return generated.ok() ? generated.value : scratchbird::core::platform::TypedUuid{};
}

}  // namespace

int main() {
  using namespace scratchbird::storage::database;

  bool ok = true;
  DirtyObjectManifest manifest;
  manifest.checkpoint_generation = 7;
  manifest.completed = true;
  manifest.classification_only = true;
  manifest.entries.push_back(
      DirtyObjectManifestEntry{DirtyObjectKind::transaction_inventory,
                               Id(scratchbird::core::platform::UuidKind::page, 7100),
                               2,
                               1,
                               1234,
                               true,
                               true});
  manifest.entries.push_back(
      DirtyObjectManifestEntry{DirtyObjectKind::row_data_page,
                               Id(scratchbird::core::platform::UuidKind::page, 7101),
                               12,
                               3,
                               5678,
                               false,
                               true});

  const auto built = BuildDirtyObjectManifest(manifest);
  ok &= Require(built.ok(), "dirty manifest builds");
  const auto parsed = ParseDirtyObjectManifest(built.serialized);
  ok &= Require(parsed.ok(), "dirty manifest parses");
  ok &= Require(parsed.manifest.entries.size() == 2, "dirty manifest entry count preserved");

  const auto recovery = ClassifyDirtyObjectManifestForRecovery(parsed.manifest);
  ok &= Require(recovery.ok(), "dirty manifest recovery classifies");
  ok &= Require(recovery.rebuild_by_scan_required, "dirty entry requires rebuild/classification scan");
  ok &= Require(!recovery.quarantine_required, "authoritative manifest avoids quarantine");
  ok &= Require(recovery.classifications.size() == 2, "dirty manifest recovery rows produced");
  ok &= Require(recovery.classifications.front().action == DirtyManifestRecoveryAction::use_manifest,
                "dirty transaction inventory uses manifest classification");
  ok &= Require(recovery.classifications.back().action == DirtyManifestRecoveryAction::no_action,
                "clean row data page has no recovery action");

  DirtyObjectManifest invalid_mode = manifest;
  invalid_mode.classification_only = false;
  const auto redo_refused = BuildDirtyObjectManifest(invalid_mode);
  ok &= Require(!redo_refused.ok(), "non-classification manifest refused");
  ok &= Require(redo_refused.diagnostic.diagnostic_code == "RECOVERY.MANIFEST_WAL_CONFUSION_FORBIDDEN",
                "redo authority confusion diagnostic");

  const auto forbidden_parse = ParseDirtyObjectManifest("SBDIRTY1\tWAL\t7\t1\n");
  ok &= Require(!forbidden_parse.ok(), "WAL-like manifest parse refused");
  ok &= Require(forbidden_parse.diagnostic.diagnostic_code == "RECOVERY.MANIFEST_WAL_CONFUSION_FORBIDDEN",
                "WAL parse diagnostic");

  const auto incomplete = ParseDirtyObjectManifest("SBDIRTY1\tclassification_only\t8\t0\n");
  ok &= Require(!incomplete.ok(), "incomplete manifest refused");
  ok &= Require(incomplete.diagnostic.diagnostic_code == "SB-DIRTY-MANIFEST-HEADER-INVALID",
                "incomplete manifest diagnostic");

  const auto selected_root = SelectCheckpointRootSet({
      CheckpointRootCandidate{6, 5, Id(scratchbird::core::platform::UuidKind::page, 7206), 6006, true, true},
      CheckpointRootCandidate{5, 0, Id(scratchbird::core::platform::UuidKind::page, 7205), 6005, true, true},
      CheckpointRootCandidate{9, 8, Id(scratchbird::core::platform::UuidKind::page, 7209), 6009, true, true},
      CheckpointRootCandidate{8, 7, Id(scratchbird::core::platform::UuidKind::page, 7208), 6008, false, true}});
  ok &= Require(selected_root.ok(), "checkpoint root-set selection succeeds");
  ok &= Require(selected_root.selected, "checkpoint root-set selected");
  ok &= Require(selected_root.root.checkpoint_generation == 6, "newest valid predecessor chain selected");
  ok &= Require(selected_root.predecessor_chain.size() == 2, "checkpoint predecessor chain retained");

  const auto invalid_root = SelectCheckpointRootSet({
      CheckpointRootCandidate{4, 3, Id(scratchbird::core::platform::UuidKind::page, 7304), 7004, true, true}});
  ok &= Require(!invalid_root.ok(), "checkpoint root-set without complete chain fails closed");
  ok &= Require(invalid_root.diagnostic.diagnostic_code == "SB-CHECKPOINT-ROOTSET-NO-VALID-CHAIN",
                "checkpoint root-set fail-closed diagnostic");

  const std::string evidence_path = "/tmp/sb_dirty_manifest_recovery_probe.recovery_evidence";
  std::filesystem::remove(evidence_path);
  const auto first_evidence = PersistDirtyManifestRecoveryRunEvidence(evidence_path,
                                                                      parsed.manifest,
                                                                      recovery,
                                                                      "recovery-run-1");
  ok &= Require(first_evidence.ok(), "recovery-run evidence persists");
  ok &= Require(!first_evidence.already_recorded, "first recovery-run evidence is new");
  ok &= Require(first_evidence.evidence.checkpoint_generation == parsed.manifest.checkpoint_generation,
                "recovery-run checkpoint generation retained");
  const auto second_evidence = PersistDirtyManifestRecoveryRunEvidence(evidence_path,
                                                                       parsed.manifest,
                                                                       recovery,
                                                                       "recovery-run-2");
  ok &= Require(second_evidence.ok(), "recovery-run evidence reloads");
  ok &= Require(second_evidence.already_recorded, "second dirty-open evidence becomes no-op");
  ok &= Require(second_evidence.evidence.recovery_run_uuid == "recovery-run-1",
                "second dirty-open reuses original completed evidence");
  std::filesystem::remove(evidence_path);

  return ok ? 0 : 1;
}
