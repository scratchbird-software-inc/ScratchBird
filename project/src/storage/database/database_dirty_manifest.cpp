// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_dirty_manifest.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <utility>

namespace scratchbird::storage::database {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::ParseTypedUuid;
using scratchbird::core::uuid::UuidKindName;
using scratchbird::core::uuid::UuidToString;

constexpr const char* kDirtyManifestMagic = "SBDIRTY1";
constexpr const char* kClassificationOnlyMode = "classification_only";
constexpr const char* kRecoveryEvidenceMagic = "SBRECOVERY1";

Status DirtyManifestOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status DirtyManifestErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_disk};
}

DirtyObjectManifestResult ManifestError(std::string diagnostic_code,
                                        std::string message_key,
                                        std::string detail = {}) {
  DirtyObjectManifestResult result;
  result.status = DirtyManifestErrorStatus();
  result.diagnostic = MakeDirtyManifestDiagnostic(result.status,
                                                  std::move(diagnostic_code),
                                                  std::move(message_key),
                                                  std::move(detail));
  return result;
}

DirtyManifestRecoveryResult RecoveryError(std::string diagnostic_code,
                                          std::string message_key,
                                          std::string detail = {}) {
  DirtyManifestRecoveryResult result;
  result.status = DirtyManifestErrorStatus();
  result.diagnostic = MakeDirtyManifestDiagnostic(result.status,
                                                  std::move(diagnostic_code),
                                                  std::move(message_key),
                                                  std::move(detail));
  return result;
}

CheckpointRootSelectionResult CheckpointRootError(std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail = {}) {
  CheckpointRootSelectionResult result;
  result.status = DirtyManifestErrorStatus();
  result.diagnostic = MakeDirtyManifestDiagnostic(result.status,
                                                  std::move(diagnostic_code),
                                                  std::move(message_key),
                                                  std::move(detail));
  return result;
}

DirtyManifestRecoveryRunEvidenceResult RecoveryEvidenceError(std::string diagnostic_code,
                                                             std::string message_key,
                                                             std::string detail = {}) {
  DirtyManifestRecoveryRunEvidenceResult result;
  result.status = DirtyManifestErrorStatus();
  result.diagnostic = MakeDirtyManifestDiagnostic(result.status,
                                                  std::move(diagnostic_code),
                                                  std::move(message_key),
                                                  std::move(detail));
  return result;
}

std::vector<std::string> Split(const std::string& text, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(text);
  while (std::getline(in, current, delimiter)) {
    parts.push_back(current);
  }
  return parts;
}

u64 ParseU64(const std::string& text) {
  try {
    return static_cast<u64>(std::stoull(text));
  } catch (...) {
    return 0;
  }
}

bool ParseBool(const std::string& text) {
  return text == "1" || text == "true" || text == "TRUE";
}

DirtyObjectKind ParseDirtyObjectKind(const std::string& text) {
  if (text == "database_header") { return DirtyObjectKind::database_header; }
  if (text == "startup_state") { return DirtyObjectKind::startup_state; }
  if (text == "transaction_inventory") { return DirtyObjectKind::transaction_inventory; }
  if (text == "catalog_page") { return DirtyObjectKind::catalog_page; }
  if (text == "allocation_map") { return DirtyObjectKind::allocation_map; }
  if (text == "row_data_page") { return DirtyObjectKind::row_data_page; }
  if (text == "index_page") { return DirtyObjectKind::index_page; }
  if (text == "filespace_header") { return DirtyObjectKind::filespace_header; }
  if (text == "metric_history") { return DirtyObjectKind::metric_history; }
  return DirtyObjectKind::unknown;
}

UuidKind ParseUuidKindName(const std::string& text) {
  if (text == "database") { return UuidKind::database; }
  if (text == "filespace") { return UuidKind::filespace; }
  if (text == "page") { return UuidKind::page; }
  if (text == "object") { return UuidKind::object; }
  if (text == "row") { return UuidKind::row; }
  if (text == "transaction") { return UuidKind::transaction; }
  if (text == "schema") { return UuidKind::schema; }
  if (text == "cluster") { return UuidKind::cluster; }
  return UuidKind::unknown;
}

bool ContainsForbiddenRedoTerm(const std::string& serialized) {
  return serialized.find("WAL") != std::string::npos ||
         serialized.find("wal") != std::string::npos ||
         serialized.find("LSN") != std::string::npos ||
         serialized.find("lsn") != std::string::npos ||
         serialized.find("write-ahead") != std::string::npos;
}

u64 StableTextChecksum(const std::string& value) {
  u64 checksum = 1469598103934665603ull;
  for (unsigned char c : value) {
    checksum ^= static_cast<u64>(c);
    checksum *= 1099511628211ull;
  }
  return checksum;
}

std::string DirtyManifestChecksumMaterial(const DirtyObjectManifest& manifest) {
  std::ostringstream out;
  out << kDirtyManifestMagic << '\t'
      << manifest.format_version << '\t'
      << kClassificationOnlyMode << '\t'
      << manifest.checkpoint_generation << '\t'
      << (manifest.completed ? 1 : 0) << '\t'
      << manifest.entries.size() << '\n';
  for (const auto& entry : manifest.entries) {
    out << "ENTRY" << '\t'
        << DirtyObjectKindName(entry.kind) << '\t'
        << UuidKindName(entry.object_uuid.kind) << '\t'
        << UuidToString(entry.object_uuid.value) << '\t'
        << entry.page_number << '\t'
        << entry.page_generation << '\t'
        << entry.object_checksum << '\t'
        << entry.local_transaction_id << '\t'
        << entry.operation_envelope_checksum << '\t'
        << entry.transaction_evidence_checksum << '\t'
        << (entry.dirty ? 1 : 0) << '\t'
        << (entry.authoritative ? 1 : 0) << '\n';
  }
  return out.str();
}

std::string SerializeDirtyManifest(const DirtyObjectManifest& manifest) {
  std::ostringstream out;
  out << kDirtyManifestMagic << '\t'
      << manifest.format_version << '\t'
      << kClassificationOnlyMode << '\t'
      << manifest.checkpoint_generation << '\t'
      << (manifest.completed ? 1 : 0) << '\t'
      << manifest.entries.size() << '\t'
      << manifest.manifest_checksum << '\n';
  for (const auto& entry : manifest.entries) {
    out << "ENTRY" << '\t'
        << DirtyObjectKindName(entry.kind) << '\t'
        << UuidKindName(entry.object_uuid.kind) << '\t'
        << UuidToString(entry.object_uuid.value) << '\t'
        << entry.page_number << '\t'
        << entry.page_generation << '\t'
        << entry.object_checksum << '\t'
        << entry.local_transaction_id << '\t'
        << entry.operation_envelope_checksum << '\t'
        << entry.transaction_evidence_checksum << '\t'
        << (entry.dirty ? 1 : 0) << '\t'
        << (entry.authoritative ? 1 : 0) << '\n';
  }
  return out.str();
}

u64 DirtyManifestChecksum(const DirtyObjectManifest& manifest) {
  return StableTextChecksum(DirtyManifestChecksumMaterial(manifest));
}

u64 RecoveryClassificationChecksum(const DirtyObjectManifest& manifest,
                                   const DirtyManifestRecoveryResult& recovery) {
  std::ostringstream stable;
  stable << manifest.format_version << ':'
         << manifest.checkpoint_generation << ':'
         << manifest.manifest_checksum << ':'
         << recovery.classifications.size();
  for (const auto& classification : recovery.classifications) {
    stable << '|'
           << DirtyObjectKindName(classification.kind) << ':'
           << UuidKindName(classification.object_uuid.kind) << ':'
           << UuidToString(classification.object_uuid.value) << ':'
           << classification.page_number << ':'
           << DirtyManifestRecoveryActionName(classification.action) << ':'
           << (classification.fail_closed ? 1 : 0) << ':'
           << classification.stable_reason;
  }
  return StableTextChecksum(stable.str());
}

std::string RecoveryActionSummary(const DirtyManifestRecoveryResult& recovery) {
  if (recovery.quarantine_required) { return "quarantine"; }
  if (recovery.rebuild_by_scan_required) { return "classify_and_rebuild_by_manifest"; }
  return "no_action";
}

std::string SerializeRecoveryEvidence(const DirtyManifestRecoveryRunEvidence& evidence) {
  std::ostringstream out;
  out << kRecoveryEvidenceMagic << '\t'
      << evidence.recovery_run_uuid << '\t'
      << evidence.checkpoint_generation << '\t'
      << evidence.classification_count << '\t'
      << evidence.classification_checksum << '\t'
      << evidence.recovery_action << '\t'
      << (evidence.completed ? 1 : 0);
  return out.str();
}

bool ParseRecoveryEvidenceLine(const std::string& line, DirtyManifestRecoveryRunEvidence* evidence) {
  if (evidence == nullptr || ContainsForbiddenRedoTerm(line)) { return false; }
  const auto parts = Split(line, '\t');
  if (parts.size() != 7 || parts[0] != kRecoveryEvidenceMagic) { return false; }
  evidence->recovery_run_uuid = parts[1];
  evidence->checkpoint_generation = ParseU64(parts[2]);
  evidence->classification_count = ParseU64(parts[3]);
  evidence->classification_checksum = ParseU64(parts[4]);
  evidence->recovery_action = parts[5];
  evidence->completed = ParseBool(parts[6]);
  return evidence->checkpoint_generation != 0 && evidence->completed;
}

}  // namespace

const char* DirtyObjectKindName(DirtyObjectKind kind) {
  switch (kind) {
    case DirtyObjectKind::database_header: return "database_header";
    case DirtyObjectKind::startup_state: return "startup_state";
    case DirtyObjectKind::transaction_inventory: return "transaction_inventory";
    case DirtyObjectKind::catalog_page: return "catalog_page";
    case DirtyObjectKind::allocation_map: return "allocation_map";
    case DirtyObjectKind::row_data_page: return "row_data_page";
    case DirtyObjectKind::index_page: return "index_page";
    case DirtyObjectKind::filespace_header: return "filespace_header";
    case DirtyObjectKind::metric_history: return "metric_history";
    case DirtyObjectKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* DirtyManifestRecoveryActionName(DirtyManifestRecoveryAction action) {
  switch (action) {
    case DirtyManifestRecoveryAction::no_action: return "no_action";
    case DirtyManifestRecoveryAction::use_manifest: return "use_manifest";
    case DirtyManifestRecoveryAction::rebuild_by_scan: return "rebuild_by_scan";
    case DirtyManifestRecoveryAction::quarantine: return "quarantine";
    case DirtyManifestRecoveryAction::fail_closed: return "fail_closed";
  }
  return "fail_closed";
}

DirtyObjectManifestResult BuildDirtyObjectManifest(const DirtyObjectManifest& manifest) {
  if (manifest.format_version != kDirtyObjectManifestFormatVersion) {
    return ManifestError("SB-DIRTY-MANIFEST-FORMAT-UNSUPPORTED",
                         "recovery.dirty_manifest.format_unsupported",
                         std::to_string(manifest.format_version));
  }
  if (!manifest.classification_only) {
    return ManifestError("RECOVERY.MANIFEST_WAL_CONFUSION_FORBIDDEN",
                         "recovery.dirty_manifest.classification_only_required",
                         "dirty manifest is classification evidence only and is never redo authority");
  }
  if (manifest.checkpoint_generation == 0) {
    return ManifestError("SB-DIRTY-MANIFEST-CHECKPOINT-GENERATION-REQUIRED",
                         "recovery.dirty_manifest.checkpoint_generation_required");
  }
  if (!manifest.completed) {
    return ManifestError("SB-DIRTY-MANIFEST-INCOMPLETE",
                         "recovery.dirty_manifest.incomplete");
  }

  DirtyObjectManifestResult result;
  result.status = DirtyManifestOkStatus();
  result.manifest = manifest;
  for (const auto& entry : result.manifest.entries) {
    if (entry.kind == DirtyObjectKind::unknown || !entry.object_uuid.valid() || !entry.authoritative ||
        entry.page_generation == 0 || entry.object_checksum == 0 ||
        entry.local_transaction_id == 0 || entry.operation_envelope_checksum == 0 ||
        entry.transaction_evidence_checksum == 0) {
      return ManifestError("SB-DIRTY-MANIFEST-ENTRY-INVALID",
                           "recovery.dirty_manifest.entry_invalid",
                           DirtyObjectKindName(entry.kind));
    }
  }
  result.manifest.manifest_checksum = DirtyManifestChecksum(result.manifest);
  result.serialized = SerializeDirtyManifest(result.manifest);
  return result;
}

DirtyObjectManifestResult ParseDirtyObjectManifest(const std::string& serialized) {
  if (ContainsForbiddenRedoTerm(serialized)) {
    return ManifestError("RECOVERY.MANIFEST_WAL_CONFUSION_FORBIDDEN",
                         "recovery.dirty_manifest.redo_terms_forbidden",
                         "dirty manifest must not contain WAL, LSN, or write-ahead redo authority");
  }

  std::istringstream in(serialized);
  std::string line;
  if (!std::getline(in, line)) {
    return ManifestError("SB-DIRTY-MANIFEST-EMPTY", "recovery.dirty_manifest.empty");
  }
  const auto header = Split(line, '\t');
  if (header.size() != 7 || header[0] != kDirtyManifestMagic) {
    return ManifestError("SB-DIRTY-MANIFEST-MAGIC-INVALID", "recovery.dirty_manifest.magic_invalid");
  }
  DirtyObjectManifest manifest;
  manifest.format_version = static_cast<u32>(ParseU64(header[1]));
  if (manifest.format_version != kDirtyObjectManifestFormatVersion) {
    return ManifestError("SB-DIRTY-MANIFEST-FORMAT-UNSUPPORTED",
                         "recovery.dirty_manifest.format_unsupported",
                         header[1]);
  }
  if (header[2] != kClassificationOnlyMode) {
    return ManifestError("RECOVERY.MANIFEST_WAL_CONFUSION_FORBIDDEN",
                         "recovery.dirty_manifest.classification_only_required",
                         header[2]);
  }

  manifest.classification_only = true;
  manifest.checkpoint_generation = ParseU64(header[3]);
  manifest.completed = ParseBool(header[4]);
  const u64 expected_entry_count = ParseU64(header[5]);
  manifest.manifest_checksum = ParseU64(header[6]);
  if (manifest.checkpoint_generation == 0 || !manifest.completed) {
    return ManifestError("SB-DIRTY-MANIFEST-HEADER-INVALID", "recovery.dirty_manifest.header_invalid");
  }

  while (std::getline(in, line)) {
    if (line.empty()) { continue; }
    const auto parts = Split(line, '\t');
    if (parts.size() != 12 || parts[0] != "ENTRY") {
      return ManifestError("SB-DIRTY-MANIFEST-ENTRY-INVALID", "recovery.dirty_manifest.entry_invalid", line);
    }
    DirtyObjectManifestEntry entry;
    entry.kind = ParseDirtyObjectKind(parts[1]);
    const UuidKind uuid_kind = ParseUuidKindName(parts[2]);
    const auto parsed_uuid = ParseTypedUuid(uuid_kind, parts[3]);
    if (entry.kind == DirtyObjectKind::unknown || !parsed_uuid.ok()) {
      return ManifestError("SB-DIRTY-MANIFEST-ENTRY-INVALID", "recovery.dirty_manifest.entry_invalid", parts[1]);
    }
    entry.object_uuid = parsed_uuid.value;
    entry.page_number = ParseU64(parts[4]);
    entry.page_generation = ParseU64(parts[5]);
    entry.object_checksum = ParseU64(parts[6]);
    entry.local_transaction_id = ParseU64(parts[7]);
    entry.operation_envelope_checksum = ParseU64(parts[8]);
    entry.transaction_evidence_checksum = ParseU64(parts[9]);
    entry.dirty = ParseBool(parts[10]);
    entry.authoritative = ParseBool(parts[11]);
    manifest.entries.push_back(entry);
  }
  if (manifest.entries.size() != expected_entry_count) {
    return ManifestError("SB-DIRTY-MANIFEST-ENTRY-COUNT-MISMATCH",
                         "recovery.dirty_manifest.entry_count_mismatch");
  }
  if (manifest.manifest_checksum == 0 ||
      manifest.manifest_checksum != DirtyManifestChecksum(manifest)) {
    return ManifestError("SB-DIRTY-MANIFEST-CHECKSUM-MISMATCH",
                         "recovery.dirty_manifest.checksum_mismatch");
  }

  DirtyObjectManifestResult result;
  result.status = DirtyManifestOkStatus();
  result.manifest = std::move(manifest);
  result.serialized = serialized;
  return result;
}

DirtyManifestRecoveryResult ClassifyDirtyObjectManifestForRecovery(const DirtyObjectManifest& manifest) {
  if (manifest.format_version != kDirtyObjectManifestFormatVersion) {
    return RecoveryError("SB-DIRTY-MANIFEST-FORMAT-UNSUPPORTED",
                         "recovery.dirty_manifest.format_unsupported",
                         std::to_string(manifest.format_version));
  }
  if (!manifest.classification_only) {
    return RecoveryError("RECOVERY.MANIFEST_WAL_CONFUSION_FORBIDDEN",
                         "recovery.dirty_manifest.classification_only_required");
  }
  if (!manifest.completed || manifest.checkpoint_generation == 0) {
    return RecoveryError("SB-DIRTY-MANIFEST-HEADER-INVALID", "recovery.dirty_manifest.header_invalid");
  }
  if (manifest.manifest_checksum == 0 ||
      manifest.manifest_checksum != DirtyManifestChecksum(manifest)) {
    return RecoveryError("SB-DIRTY-MANIFEST-CHECKSUM-MISMATCH",
                         "recovery.dirty_manifest.checksum_mismatch");
  }

  DirtyManifestRecoveryResult result;
  result.status = DirtyManifestOkStatus();
  for (const auto& entry : manifest.entries) {
    DirtyManifestRecoveryClassification classification;
    classification.kind = entry.kind;
    classification.object_uuid = entry.object_uuid;
    classification.page_number = entry.page_number;
    if (!entry.authoritative || entry.kind == DirtyObjectKind::unknown || !entry.object_uuid.valid() ||
        entry.page_generation == 0 || entry.object_checksum == 0 ||
        entry.local_transaction_id == 0 || entry.operation_envelope_checksum == 0 ||
        entry.transaction_evidence_checksum == 0) {
      classification.action = DirtyManifestRecoveryAction::quarantine;
      classification.fail_closed = true;
      classification.stable_reason = "manifest_entry_not_authoritative";
      result.quarantine_required = true;
    } else if (!entry.dirty) {
      classification.action = DirtyManifestRecoveryAction::no_action;
      classification.stable_reason = "object_clean_at_checkpoint";
    } else {
      classification.action = DirtyManifestRecoveryAction::use_manifest;
      classification.stable_reason = "dirty_object_requires_classification_recovery";
      result.rebuild_by_scan_required = true;
    }
    result.classifications.push_back(std::move(classification));
  }
  return result;
}

CheckpointRootSelectionResult SelectCheckpointRootSet(const std::vector<CheckpointRootCandidate>& candidates) {
  if (candidates.empty()) {
    return CheckpointRootError("SB-CHECKPOINT-ROOTSET-MISSING",
                               "recovery.checkpoint_rootset.missing");
  }
  std::map<u64, CheckpointRootCandidate> by_generation;
  for (const auto& candidate : candidates) {
    if (candidate.checkpoint_generation == 0 || !candidate.completed || !candidate.authoritative ||
        !candidate.root_object_uuid.valid()) {
      continue;
    }
    by_generation[candidate.checkpoint_generation] = candidate;
  }
  std::vector<u64> descending_generations;
  for (const auto& [generation, ignored] : by_generation) {
    (void)ignored;
    descending_generations.push_back(generation);
  }
  std::sort(descending_generations.begin(), descending_generations.end(), std::greater<u64>());
  for (const u64 generation : descending_generations) {
    std::vector<u64> chain;
    std::map<u64, bool> visited;
    u64 current = generation;
    bool valid_chain = true;
    while (current != 0) {
      if (visited[current]) {
        valid_chain = false;
        break;
      }
      visited[current] = true;
      const auto found = by_generation.find(current);
      if (found == by_generation.end()) {
        valid_chain = false;
        break;
      }
      chain.push_back(current);
      current = found->second.predecessor_generation;
    }
    if (!valid_chain) { continue; }
    CheckpointRootSelectionResult result;
    result.status = DirtyManifestOkStatus();
    result.selected = true;
    result.root = by_generation[generation];
    result.predecessor_chain = std::move(chain);
    return result;
  }
  return CheckpointRootError("SB-CHECKPOINT-ROOTSET-NO-VALID-CHAIN",
                             "recovery.checkpoint_rootset.no_valid_predecessor_chain");
}

DirtyManifestRecoveryRunEvidenceResult PersistDirtyManifestRecoveryRunEvidence(
    const std::string& evidence_store_path,
    const DirtyObjectManifest& manifest,
    const DirtyManifestRecoveryResult& recovery,
    const std::string& recovery_run_uuid) {
  if (evidence_store_path.empty()) {
    return RecoveryEvidenceError("SB-RECOVERY-EVIDENCE-PATH-REQUIRED",
                                 "recovery.run_evidence.path_required");
  }
  if (!manifest.completed || manifest.checkpoint_generation == 0 || !manifest.classification_only || !recovery.ok()) {
    return RecoveryEvidenceError("SB-RECOVERY-EVIDENCE-INPUT-INVALID",
                                 "recovery.run_evidence.input_invalid");
  }
  if (recovery_run_uuid.empty()) {
    return RecoveryEvidenceError("SB-RECOVERY-EVIDENCE-RUN-UUID-REQUIRED",
                                 "recovery.run_evidence.run_uuid_required");
  }

  DirtyManifestRecoveryRunEvidence evidence;
  evidence.recovery_run_uuid = recovery_run_uuid;
  evidence.checkpoint_generation = manifest.checkpoint_generation;
  evidence.classification_count = static_cast<u64>(recovery.classifications.size());
  evidence.classification_checksum = RecoveryClassificationChecksum(manifest, recovery);
  evidence.recovery_action = RecoveryActionSummary(recovery);
  evidence.completed = true;

  std::ifstream existing_in(evidence_store_path, std::ios::binary);
  std::string existing_line;
  while (std::getline(existing_in, existing_line)) {
    DirtyManifestRecoveryRunEvidence existing;
    if (!ParseRecoveryEvidenceLine(existing_line, &existing)) { continue; }
    if (existing.checkpoint_generation == evidence.checkpoint_generation &&
        existing.classification_count == evidence.classification_count &&
        existing.classification_checksum == evidence.classification_checksum) {
      DirtyManifestRecoveryRunEvidenceResult result;
      result.status = DirtyManifestOkStatus();
      result.already_recorded = true;
      result.evidence = std::move(existing);
      result.serialized = existing_line;
      return result;
    }
  }

  const std::string serialized = SerializeRecoveryEvidence(evidence);
  std::ofstream out(evidence_store_path, std::ios::app | std::ios::binary);
  if (!out) {
    return RecoveryEvidenceError("SB-RECOVERY-EVIDENCE-APPEND-FAILED",
                                 "recovery.run_evidence.append_failed");
  }
  out << serialized << '\n';
  out.flush();
  if (!out) {
    return RecoveryEvidenceError("SB-RECOVERY-EVIDENCE-APPEND-FAILED",
                                 "recovery.run_evidence.append_failed");
  }

  DirtyManifestRecoveryRunEvidenceResult result;
  result.status = DirtyManifestOkStatus();
  result.already_recorded = false;
  result.evidence = std::move(evidence);
  result.serialized = serialized;
  return result;
}

DiagnosticRecord MakeDirtyManifestDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.database.dirty_manifest",
                        status.ok() ? "" : "treat dirty manifest as classification evidence only; rebuild by scan or quarantine when invalid");
}

}  // namespace scratchbird::storage::database
