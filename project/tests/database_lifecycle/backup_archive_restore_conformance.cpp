// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "backup_archive/backup_archive_api.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace engine_api = scratchbird::engine::internal_api;

constexpr std::string_view kDatabaseUuid = "019e0f62-0000-7000-8000-000000000013";
constexpr std::string_view kFilespaceUuid = "019e0f62-0000-7000-8000-000000000014";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc013k_backup_archive.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-013K test");
  return std::filesystem::path(made);
}

bool HasDiagnostic(const engine_api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
    if (diagnostic.detail == code) return true;
    if (diagnostic.detail.find(code) != std::string::npos) return true;
    if (diagnostic.detail.size() > code.size() &&
        diagnostic.detail.compare(diagnostic.detail.size() - code.size(), code.size(), code) == 0) {
      return true;
    }
  }
  return false;
}

bool HasAdmissionDiagnostic(const engine_api::BackupArchiveLifecycleAdmission& admission,
                            std::string_view code) {
  if (admission.diagnostic.detail == code) return true;
  return admission.diagnostic.detail.size() > code.size() &&
         admission.diagnostic.detail.compare(admission.diagnostic.detail.size() - code.size(),
                                             code.size(),
                                             code) == 0;
}

bool HasEvidence(const engine_api::EngineApiResult& result, std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) return true;
  }
  return false;
}

void DumpDiagnostics(const engine_api::EngineApiResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
  }
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string HexEncode(std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(text.size() * 2);
  for (unsigned char c : text) {
    out.push_back(kHex[(c >> 4) & 0x0f]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

std::uint64_t Fnv1a64(std::string_view value) {
  std::uint64_t hash = 1469598103934665603ull;
  for (unsigned char c : value) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string EncodePairs(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string encoded;
  for (const auto& [key, value] : fields) {
    if (!encoded.empty()) { encoded.push_back('|'); }
    encoded += HexEncode(key);
    encoded.push_back('=');
    encoded += HexEncode(value);
  }
  return encoded;
}

std::string RecordLine(const std::string& kind,
                       const std::vector<std::pair<std::string, std::string>>& fields) {
  return kind + "\t" + EncodePairs(fields) + "\n";
}

bool HasEncodedManifestField(std::string_view manifest,
                             std::string_view key,
                             std::string_view value) {
  const std::string encoded = HexEncode(key) + "=" + HexEncode(value);
  return manifest.find(encoded) != std::string_view::npos;
}

void WriteFile(const std::filesystem::path& path, std::string_view content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
  out.close();
  Require(static_cast<bool>(out), "file write failed");
}

void WriteManifestWithChecksum(const std::filesystem::path& path, const std::string& body) {
  WriteFile(path, body + "CHECKSUM\t" + std::to_string(Fnv1a64(body)) + "\n");
}

engine_api::EngineRequestContext Context(const std::filesystem::path& database_path,
                                         std::uint64_t transaction_id = 0) {
  engine_api::EngineRequestContext context;
  context.trust_mode = engine_api::EngineTrustMode::server_isolated;
  context.database_path = database_path.string();
  context.database_uuid.canonical = std::string(kDatabaseUuid);
  context.security_context_present = true;
  context.trace_tags.push_back("security.bootstrap");
  context.local_transaction_id = transaction_id;
  return context;
}

std::vector<std::string> LifecycleOptions() {
  return {std::string("filespace_uuid:") + std::string(kFilespaceUuid)};
}

std::vector<std::string> RestoreLifecycleOptions() {
  auto options = LifecycleOptions();
  options.push_back("restore_inspection_open:true");
  options.push_back("recovery_classification:restore_inspection");
  options.push_back("target_database_open:false");
  return options;
}

void TestLifecycleAdmissionPolicy(const std::filesystem::path& database_path) {
  engine_api::EngineApiRequest request;
  request.context = Context(database_path);
  auto acquired = engine_api::EvaluateBackupArchiveLifecycleAdmission(
      request, engine_api::BackupArchiveLifecycleOperation::physical_backup);
  Require(acquired.admitted, "engine did not acquire backup lifecycle holds itself");
  Require(acquired.snapshot_hold_acquired && acquired.filespace_hold_acquired,
          "backup lifecycle admission did not acquire snapshot and filespace holds");

  request.option_envelopes = LifecycleOptions();
  request.option_envelopes.push_back("live_file_shortcut:true");
  auto live = engine_api::EvaluateBackupArchiveLifecycleAdmission(
      request, engine_api::BackupArchiveLifecycleOperation::physical_backup);
  Require(!live.admitted && HasAdmissionDiagnostic(live, "BACKUP_LIVE_FILE_SHORTCUT_FORBIDDEN"),
          "backup admission accepted live-file shortcut");

  request.option_envelopes = LifecycleOptions();
  request.option_envelopes.push_back("legal_hold_active:true");
  auto legal = engine_api::EvaluateBackupArchiveLifecycleAdmission(
      request, engine_api::BackupArchiveLifecycleOperation::physical_backup);
  Require(!legal.admitted && HasAdmissionDiagnostic(legal, "BACKUP_LEGAL_RETENTION_REQUIRED"),
          "backup admission ignored active legal hold");

  request.option_envelopes = LifecycleOptions();
  request.option_envelopes.push_back("shadow_target_registered:true");
  auto shadow = engine_api::EvaluateBackupArchiveLifecycleAdmission(
      request, engine_api::BackupArchiveLifecycleOperation::shadow_snapshot);
  Require(shadow.admitted, "shadow snapshot lifecycle admission was refused");
  Require(shadow.live_file_shortcut_refused, "shadow snapshot did not publish shortcut refusal");

  request.option_envelopes = LifecycleOptions();
  auto restore = engine_api::EvaluateBackupArchiveLifecycleAdmission(
      request, engine_api::BackupArchiveLifecycleOperation::physical_restore);
  Require(!restore.admitted && HasAdmissionDiagnostic(restore, "RESTORE_INSPECTION_OPEN_REQUIRED"),
          "restore admission accepted non-inspection open path");

  request.option_envelopes = RestoreLifecycleOptions();
  request.option_envelopes.push_back("authoritative_wal:true");
  auto wal = engine_api::EvaluateBackupArchiveLifecycleAdmission(
      request, engine_api::BackupArchiveLifecycleOperation::delta_package);
  Require(!wal.admitted && HasAdmissionDiagnostic(wal, "BACKUP_AUTHORITATIVE_WAL_FORBIDDEN"),
          "archive admission accepted authoritative WAL");
}

void TestPhysicalBackupRestoreLifecycle(const std::filesystem::path& temp_dir) {
  const auto source_database = temp_dir / "source.sbdb";
  const auto restored_database = temp_dir / "restored.sbdb";
  const auto manifest = temp_dir / "backup" / "source.sbpb";
  const std::string source_image = "SBDB_IMAGE_UNDER_ENGINE_OWNED_BACKUP_PATH";
  WriteFile(source_database, source_image);

  engine_api::EngineStartPhysicalBackupRequest backup;
  backup.context = Context(source_database);
  backup.option_envelopes = LifecycleOptions();
  backup.option_envelopes.push_back("target_uri:" + manifest.string());
  auto backup_result = engine_api::EngineStartPhysicalBackup(backup);
  Require(backup_result.ok, "physical backup lifecycle path failed");
  Require(backup_result.image_bytes == source_image.size(), "physical backup image size mismatch");
  Require(HasEvidence(backup_result, "snapshot_hold"), "backup did not publish snapshot hold");
  Require(HasEvidence(backup_result, "filespace_hold"), "backup did not publish filespace hold");
  Require(HasEvidence(backup_result, "shutdown_blocker"), "backup did not publish shutdown blocker");
  Require(HasEvidence(backup_result, "drop_blocker"), "backup did not publish drop blocker");
  Require(HasEvidence(backup_result, "live_file_shortcut"), "backup did not refuse live shortcut");
  const auto ledger = ReadFile(source_database.string() + ".sb.backup_archive_lifecycle");
  Require(ledger.find("snapshot_hold") != std::string::npos,
          "backup lifecycle ledger omitted snapshot hold");
  Require(ledger.find("filespace_hold") != std::string::npos,
          "backup lifecycle ledger omitted filespace hold");
  Require(ledger.find("shutdown_blocker") != std::string::npos,
          "backup lifecycle ledger omitted shutdown blocker");
  Require(ledger.find("drop_blocker") != std::string::npos,
          "backup lifecycle ledger omitted drop blocker");
  Require(ledger.find("engine_checkpoint_filespace_evidence") != std::string::npos,
          "backup lifecycle ledger omitted checkpoint/filespace evidence");
  Require(HasEncodedManifestField(ReadFile(manifest), "authoritative_wal", "false"),
          "physical backup manifest omitted anti-WAL evidence");
  Require(HasEncodedManifestField(ReadFile(manifest), "filespace_uuid", std::string(kFilespaceUuid)),
          "physical backup manifest omitted filespace coverage proof");
  Require(HasEncodedManifestField(ReadFile(manifest), "coverage_contiguous", "true"),
          "physical backup manifest omitted contiguous coverage proof");
  Require(HasEncodedManifestField(ReadFile(manifest), "signature_profile", "unsigned-local-manifest-proof-v1"),
          "physical backup manifest omitted signature profile");

  engine_api::EngineRestorePhysicalBackupRequest restore_missing_inspection;
  restore_missing_inspection.context = Context(restored_database, 2);
  restore_missing_inspection.option_envelopes = LifecycleOptions();
  restore_missing_inspection.option_envelopes.push_back("source_manifest_uri:" + manifest.string());
  auto missing_result = engine_api::EngineRestorePhysicalBackup(restore_missing_inspection);
  Require(!missing_result.ok && HasDiagnostic(missing_result, "RESTORE_INSPECTION_OPEN_REQUIRED"),
          "physical restore accepted missing inspection-open state");

  engine_api::EngineRestorePhysicalBackupRequest restore;
  restore.context = Context(restored_database, 2);
  restore.option_envelopes = RestoreLifecycleOptions();
  restore.option_envelopes.push_back("source_manifest_uri:" + manifest.string());
  auto restore_result = engine_api::EngineRestorePhysicalBackup(restore);
  Require(restore_result.ok, "physical restore lifecycle path failed");
  Require(restore_result.restored_database_path == restored_database.string(),
          "physical restore target path mismatch");
  Require(ReadFile(restored_database) == source_image,
          "physical restore did not install the validated engine-owned image");
  Require(HasEvidence(restore_result, "restore_inspection_open"),
          "restore did not publish inspection-open evidence");
  Require(HasEvidence(restore_result, "restore_recovery_classification"),
          "restore did not publish recovery-classification evidence");
}

void TestBackupRestoreManifestCoverageProof(const std::filesystem::path& temp_dir) {
  const auto restored_database = temp_dir / "missing_filespace_restore.sbdb";
  const auto image = temp_dir / "missing_filespace.image";
  const auto manifest = temp_dir / "missing_filespace.sbpb";
  WriteFile(image, "IMAGE");
  const std::string body =
      "SBPHYSICALBACKUP1\n" +
      RecordLine("META", {{"backup_uuid", "physical-backup-missing-filespace"},
                            {"manifest_version", "1"},
                            {"database_uuid", std::string(kDatabaseUuid)},
                            {"timeline_uuid", "timeline-local"},
                            {"fork_uuid", "fork-primary"},
                            {"key_lineage_id", "key-lineage-local"},
                            {"coverage_start_transaction_id", "0"},
                            {"coverage_end_transaction_id", "0"},
                            {"coverage_contiguous", "true"},
                            {"coverage_proof", "177"},
                            {"checksum_profile", "fnv1a64-manifest-body"},
                            {"signature_profile", "unsigned-local-manifest-proof-v1"},
                            {"finality_source", "local_mga_transaction_inventory"},
                            {"image_uri", image.string()},
                            {"image_bytes", "5"},
                            {"image_checksum", std::to_string(Fnv1a64("IMAGE"))},
                            {"authoritative_wal", "false"},
                            {"format", "physical_snapshot_v1"}});
  WriteManifestWithChecksum(manifest, body);

  engine_api::EngineRestorePhysicalBackupRequest restore;
  restore.context = Context(restored_database, 2);
  restore.option_envelopes = RestoreLifecycleOptions();
  restore.option_envelopes.push_back("source_manifest_uri:" + manifest.string());
  auto result = engine_api::EngineRestorePhysicalBackup(restore);
  Require(!result.ok && HasDiagnostic(result, "RESTORE_MANIFEST_COVERAGE_FIELD_MISSING:filespace_uuid"),
          "physical restore accepted manifest with missing filespace UUID coverage proof");
}

std::string DeltaManifestBody(std::uint64_t start_tx,
                              std::uint64_t end_tx,
                              std::string restore_point_name = "rp-good") {
  return "SBDELTAPACKAGE1\n" +
         RecordLine("META", {{"delta_uuid", "delta-coverage-proof"},
                               {"manifest_version", "1"},
                               {"source_backup_uuid", "physical-backup-source"},
                               {"database_uuid", std::string(kDatabaseUuid)},
                               {"filespace_uuid", std::string(kFilespaceUuid)},
                               {"timeline_uuid", "timeline-local"},
                               {"fork_uuid", "fork-primary"},
                               {"key_lineage_id", "key-lineage-local"},
                               {"start_transaction_id", std::to_string(start_tx)},
                               {"end_transaction_id", std::to_string(end_tx)},
                               {"coverage_start_transaction_id", std::to_string(start_tx)},
                               {"coverage_end_transaction_id", std::to_string(end_tx)},
                               {"coverage_contiguous", "true"},
                               {"coverage_gap_classification", "none"},
                               {"coverage_proof", "377"},
                               {"restore_point_name", std::move(restore_point_name)},
                               {"coverage_start_unix_micros", "1000"},
                               {"coverage_end_unix_micros", "2000"},
                               {"checksum_profile", "fnv1a64-manifest-body"},
                               {"signature_profile", "unsigned-local-manifest-proof-v1"},
                               {"replay_profile", "deterministic_mga_operation_envelopes"},
                               {"finality_source", "local_mga_transaction_inventory"},
                               {"delta_source", "mga_row_version_lineage"},
                               {"contains_committed_final_states_only", "true"},
                               {"authoritative_wal", "false"},
                               {"format", "write_after_delta_v1"}});
}

void TestDeltaCoverageAndPitrRollforward(const std::filesystem::path& temp_dir) {
  const auto target_database = temp_dir / "delta_target.sbdb";
  const auto delta_manifest = temp_dir / "delta.sbdelta";
  WriteManifestWithChecksum(delta_manifest, DeltaManifestBody(5, 10));

  engine_api::EngineApplyDeltaStreamRequest gap_apply;
  gap_apply.context = Context(target_database, 44);
  gap_apply.option_envelopes = RestoreLifecycleOptions();
  gap_apply.option_envelopes.push_back("source_manifest_uri:" + delta_manifest.string());
  gap_apply.option_envelopes.push_back("expected_previous_end_transaction_id:3");
  auto gap_result = engine_api::EngineApplyDeltaStream(gap_apply);
  Require(!gap_result.ok && HasDiagnostic(gap_result, "BACKUP_DELTA_COVERAGE_GAP"),
          "delta apply accepted missing segment coverage gap");

  engine_api::EngineApplyDeltaStreamRequest pitr_outside;
  pitr_outside.context = Context(target_database, 45);
  pitr_outside.option_envelopes = RestoreLifecycleOptions();
  pitr_outside.option_envelopes.push_back("source_manifest_uri:" + delta_manifest.string());
  pitr_outside.option_envelopes.push_back("pitr_target_transaction_id:11");
  auto pitr_outside_result = engine_api::EngineApplyDeltaStream(pitr_outside);
  Require(!pitr_outside_result.ok && HasDiagnostic(pitr_outside_result, "PITR_TARGET_OUTSIDE_COVERAGE"),
          "PITR rollforward accepted target outside coverage");

  engine_api::EngineApplyDeltaStreamRequest pitr_latest;
  pitr_latest.context = Context(target_database, 46);
  pitr_latest.option_envelopes = RestoreLifecycleOptions();
  pitr_latest.option_envelopes.push_back("source_manifest_uri:" + delta_manifest.string());
  pitr_latest.option_envelopes.push_back("expected_previous_end_transaction_id:4");
  pitr_latest.option_envelopes.push_back("pitr_target_selector:latest-valid");
  auto latest_result = engine_api::EngineApplyDeltaStream(pitr_latest);
  if (!latest_result.ok) { DumpDiagnostics(latest_result); }
  Require(latest_result.ok, "PITR latest-valid rollforward was refused inside coverage");
  Require(latest_result.pitr_target_transaction_id == 10,
          "PITR latest-valid target did not select coverage end transaction");
  Require(HasEvidence(latest_result, "pitr_rollforward_profile"),
          "PITR rollforward did not publish deterministic replay profile");
}

}  // namespace

int main() {
  const auto temp_dir = MakeTempDir();
  TestLifecycleAdmissionPolicy(temp_dir / "policy.sbdb");
  TestPhysicalBackupRestoreLifecycle(temp_dir);
  TestBackupRestoreManifestCoverageProof(temp_dir);
  TestDeltaCoverageAndPitrRollforward(temp_dir);
  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
