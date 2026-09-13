// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_dirty_manifest.hpp"
#include "hash_digest_parts.hpp"
#include "disk_device.hpp"
#include "transaction_inventory_validation.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <limits>
#include <mutex>
#include <set>
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

namespace native_checkpoint {
namespace disk=scratchbird::storage::disk;
namespace hash=scratchbird::core::hash;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using Error=NativeCheckpointError;
constexpr std::size_t family=128,entries=512,root_digest_at=336,digest_at=368;
constexpr std::array<byte,8> magic{{'S','B','C','P','N','T','0','1'}};
constexpr std::array<byte,8> domain{{'S','B','C','P','S','E','T','1'}};
constexpr std::array<u32,16> types{{0,0x301,0x302,9,3,5,10,11,8,5,0x303,0x305,0x307,0x308,0x309,0x30b}};
bool V7(const Uuid& id) { return scratchbird::core::uuid::IsEngineIdentityUuid(id); }
bool Zero(const byte* b,std::size_t size) { return std::all_of(b,b+size,[](byte v){return v==0;}); }
void Put(byte* b,const Uuid& id) {std::copy(id.bytes.begin(),id.bytes.end(),b);}
Uuid Get(const byte* b) {Uuid id;std::copy_n(b,16,id.bytes.begin());return id;}
void PutRef(byte* b,const disk::NativePageReference& r) {
  Put(b,r.filespace_uuid);StoreLittle64(b+16,r.page_number);StoreLittle64(b+24,r.page_generation);Put(b+32,r.page_size_profile_uuid);
}
disk::NativePageReference GetRef(const byte* b) {return {Get(b),LoadLittle64(b+16),LoadLittle64(b+24),Get(b+32)};}
disk::NativePageReference Self(const NativeCheckpointRoot& r) {
  return {r.header.filespace_uuid,r.header.page_number,r.header.page_generation,r.header.page_size_profile_uuid};
}
bool RefValid(const disk::NativePageReference& r) {
  const auto* profile=disk::FindCanonicalFilespacePageProfile(r.page_size_profile_uuid);
  return V7(r.filespace_uuid)&&r.page_number&&r.page_generation&&profile
    &&r.page_number<std::numeric_limits<u64>::max()/profile->page_size_bytes
    &&disk::CheckFileDeviceExtent(r.page_number*profile->page_size_bytes,profile->page_size_bytes).ok();
}
bool SameSlot(const disk::NativePageReference& a,const disk::NativePageReference& b) {return a.filespace_uuid==b.filespace_uuid&&a.page_number==b.page_number;}
bool ProfilesAgree(const disk::NativePageReference& a,const disk::NativePageReference& b) {return a.filespace_uuid!=b.filespace_uuid||a.page_size_profile_uuid==b.page_size_profile_uuid;}
NativeCheckpointRootResult Fail(Error error) {return {error,std::nullopt,{}};}
Error Validate(const NativeCheckpointRoot& r) {
  if(!disk::EncodeNativeCommonPageHeader(r.header).ok()||r.header.page_type!=0x300)return Error::invalid_header;
  if(!V7(r.object_uuid)||!V7(r.timeline_uuid)||!V7(r.creator_transaction_uuid)||!r.checkpoint_generation
    ||!r.root_set_generation||!r.selected_local_transaction_id||!r.creator_local_transaction_id
    ||r.creator_local_transaction_id>r.selected_local_transaction_id
    ||r.stable_local_transaction_id>r.local_durable_transaction_id
    ||r.local_durable_transaction_id>r.selected_local_transaction_id
    ||r.cluster_quorum_transaction_id>r.local_durable_transaction_id||(r.flags&~15ull))return Error::invalid_family;
  const auto self=Self(r);if(!RefValid(self))return Error::invalid_reference;
  const bool empty_digest=Zero(r.predecessor_sha256.data(),32);
  if((r.checkpoint_generation==1&&(r.predecessor||!empty_digest))
    ||(r.checkpoint_generation>1&&(!r.predecessor||empty_digest)))return Error::invalid_reference;
  if(r.predecessor&&(!RefValid(*r.predecessor)||SameSlot(self,*r.predecessor)||!ProfilesAgree(self,*r.predecessor)))return Error::invalid_reference;
  if(r.roots.size()<10||r.roots.size()>15)return Error::invalid_roots;
  u16 prior=0;u32 roles=0;
  for(std::size_t i=0;i<r.roots.size();++i) {
    const auto& target=r.roots[i];
    if(target.role<=prior||target.role>=types.size()||target.page_type!=types[target.role]
      ||!V7(target.object_uuid)||!RefValid(target.page)||Zero(target.sha256.data(),32)
      ||SameSlot(self,target.page)||!ProfilesAgree(self,target.page))return Error::invalid_roots;
    if(r.predecessor&&(SameSlot(*r.predecessor,target.page)||!ProfilesAgree(*r.predecessor,target.page)))return Error::invalid_roots;
    for(std::size_t j=0;j<i;++j) {
      const auto& other=r.roots[j];
      if(!ProfilesAgree(target.page,other.page))return Error::invalid_roots;
      if(SameSlot(target.page,other.page)&&(target.page!=other.page||target.object_uuid!=other.object_uuid
        ||target.page_type!=other.page_type||target.sha256!=other.sha256))return Error::invalid_roots;
    }
    prior=target.role;roles|=u32{1}<<target.role;
  }
  if((roles&0x7feu)!=0x7feu||static_cast<bool>(roles&(1u<<14))!=static_cast<bool>(r.flags&4)
    ||(!(r.flags&4)&&r.cluster_quorum_transaction_id))return Error::invalid_roots;
  return Error::none;
}
hash::HashDigestResult RootDigest(const std::vector<byte>& b,std::size_t used) {
  const hash::HashDigestSegment parts[]={{domain.data(),domain.size()},{b.data()+family+32,96},{b.data()+entries,used-entries}};
  return hash::ComputeSha256DigestParts(parts,3);
}
hash::HashDigestResult FullDigest(const std::vector<byte>& b) {
  const std::array<byte,32> zero{};
  const hash::HashDigestSegment parts[]={{b.data(),digest_at},{zero.data(),zero.size()},{b.data()+digest_at+32,b.size()-digest_at-32}};
  return hash::ComputeSha256DigestParts(parts,3);
}
struct LockedFilespaces {
  Error error=Error::invalid_filespace;
  std::vector<disk::NativeFilespaceDevice> ordered;
  std::vector<std::unique_lock<std::recursive_mutex>> guards;
};
LockedFilespaces LockFilespaces(const std::vector<disk::NativeFilespaceDevice>& devices) {
  LockedFilespaces result;result.ordered=devices;auto& ordered=result.ordered;
  if(ordered.empty())return result;
  std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.filespace_uuid.bytes<b.filespace_uuid.bytes;});
  for(std::size_t i=0;i<ordered.size();++i){const auto& fs=ordered[i];
    if(!V7(fs.filespace_uuid)||!disk::FindCanonicalFilespacePageProfile(fs.page_size_profile_uuid)
      ||!fs.device||(i&&fs.filespace_uuid==ordered[i-1].filespace_uuid))return result;
    for(std::size_t j=0;j<i;++j)if(fs.device==ordered[j].device)return result;}
  result.guards.reserve(ordered.size());
  for(const auto& fs:ordered)result.guards.push_back(fs.device->AcquireOperationGuard());
  result.error=Error::none;return result;
}
} // namespace native_checkpoint

NativeCheckpointRootResult EncodeNativeCheckpointRoot(const NativeCheckpointRoot& r) noexcept {
  using namespace native_checkpoint;
  try {
    const auto valid=Validate(r);if(valid!=Error::none)return Fail(valid);
    const auto common=disk::EncodeNativeCommonPageHeader(r.header);std::vector<byte> b(r.header.page_size_bytes,0);
    std::copy(common.bytes->begin(),common.bytes->end(),b.begin());auto* f=b.data()+family;
    std::copy(magic.begin(),magic.end(),f);StoreLittle16(f+8,1);StoreLittle16(f+10,384);
    const auto used=entries+112*r.roots.size();StoreLittle32(f+12,static_cast<u32>(used));Put(f+16,r.object_uuid);
    StoreLittle64(f+32,r.checkpoint_generation);StoreLittle64(f+40,r.root_set_generation);
    StoreLittle64(f+48,r.selected_local_transaction_id);StoreLittle64(f+56,r.stable_local_transaction_id);
    StoreLittle64(f+64,r.local_durable_transaction_id);StoreLittle64(f+72,r.cluster_quorum_transaction_id);
    Put(f+80,r.timeline_uuid);Put(f+96,r.creator_transaction_uuid);StoreLittle64(f+112,r.creator_local_transaction_id);StoreLittle64(f+120,r.flags);
    if(r.predecessor)PutRef(f+128,*r.predecessor);std::copy(r.predecessor_sha256.begin(),r.predecessor_sha256.end(),f+176);
    StoreLittle64(f+272,r.completed?1:0);
    for(std::size_t i=0;i<r.roots.size();++i){const auto& target=r.roots[i];auto* out=b.data()+entries+112*i;
      StoreLittle16(out,target.role);StoreLittle32(out+4,target.page_type);PutRef(out+8,target.page);Put(out+56,target.object_uuid);std::copy(target.sha256.begin(),target.sha256.end(),out+72);}
    const auto root_digest=RootDigest(b,used);if(!root_digest.ok())return Fail(Error::hash_failure);
    std::copy(root_digest.digest.begin(),root_digest.digest.end(),b.begin()+root_digest_at);
    const auto full=FullDigest(b);if(!full.ok())return Fail(Error::hash_failure);
    std::copy(full.digest.begin(),full.digest.end(),b.begin()+digest_at);return {Error::none,r,std::move(b)};
  }catch(const std::bad_alloc&){return Fail(Error::resource_exhausted);}
   catch(const std::length_error&){return Fail(Error::resource_exhausted);}
   catch(...){return Fail(Error::invalid_family);}
}

NativeCheckpointRootResult DecodeNativeCheckpointRoot(const std::vector<scratchbird::core::platform::byte>& b) noexcept {
  using namespace native_checkpoint;
  try {
    const auto common=disk::DecodeNativeCommonPageHeader(b.data(),std::min<std::size_t>(b.size(),128));
    if(!common.ok()||common.header->page_type!=0x300||b.size()!=common.header->page_size_bytes)return Fail(Error::invalid_header);
    const auto full=FullDigest(b);if(!full.ok())return Fail(Error::hash_failure);
    if(!std::equal(full.digest.begin(),full.digest.end(),b.begin()+digest_at))return Fail(Error::invalid_integrity);
    const auto* f=b.data()+family;const auto used=LoadLittle32(f+12);
    if(!std::equal(magic.begin(),magic.end(),f)||LoadLittle16(f+8)!=1||LoadLittle16(f+10)!=384
      ||used<entries+112*10||used>entries+112*15||(used-entries)%112||LoadLittle64(f+272)>1
      ||!Zero(f+280,104)||!Zero(b.data()+used,b.size()-used))return Fail(Error::invalid_family);
    const auto digest=RootDigest(b,used);if(!digest.ok())return Fail(Error::hash_failure);
    if(!std::equal(digest.digest.begin(),digest.digest.end(),b.begin()+root_digest_at))return Fail(Error::invalid_integrity);
    NativeCheckpointRoot r;r.header=*common.header;r.object_uuid=Get(f+16);
    r.checkpoint_generation=LoadLittle64(f+32);r.root_set_generation=LoadLittle64(f+40);
    r.selected_local_transaction_id=LoadLittle64(f+48);r.stable_local_transaction_id=LoadLittle64(f+56);
    r.local_durable_transaction_id=LoadLittle64(f+64);r.cluster_quorum_transaction_id=LoadLittle64(f+72);
    r.timeline_uuid=Get(f+80);r.creator_transaction_uuid=Get(f+96);r.creator_local_transaction_id=LoadLittle64(f+112);r.flags=LoadLittle64(f+120);
    if(!Zero(f+128,48))r.predecessor=GetRef(f+128);std::copy_n(f+176,32,r.predecessor_sha256.begin());r.completed=LoadLittle64(f+272)==1;
    for(std::size_t at=entries;at<used;at+=112){const auto* in=b.data()+at;NativeCheckpointRootReference target;
      if(!Zero(in+2,2)||!Zero(in+104,8))return Fail(Error::invalid_roots);
      target.role=LoadLittle16(in);target.page_type=LoadLittle32(in+4);target.page=GetRef(in+8);target.object_uuid=Get(in+56);
      std::copy_n(in+72,32,target.sha256.begin());r.roots.push_back(target);}
    const auto valid=Validate(r);if(valid!=Error::none)return Fail(valid);return {Error::none,std::move(r),b};
  }catch(const std::bad_alloc&){return Fail(Error::resource_exhausted);}
   catch(const std::length_error&){return Fail(Error::resource_exhausted);}
   catch(...){return Fail(Error::invalid_family);}
}

NativeCheckpointRootResult ReadNativeCheckpointRootFromOpenDevice(
    scratchbird::storage::disk::FileDevice& device,const scratchbird::core::platform::Uuid& database_uuid,
    const scratchbird::storage::disk::FilespaceRootReference& ref) noexcept {
  using namespace native_checkpoint;
  try {
    const disk::NativePageReference target{ref.filespace_uuid,ref.page_number,ref.page_generation,ref.page_size_profile_uuid};
    if(!V7(database_uuid)||!V7(ref.object_uuid)||ref.kind!=9||ref.page_type!=0x300||!RefValid(target))return Fail(Error::invalid_reference);
    const auto guard=device.AcquireOperationGuard();
    const disk::FilespaceBootstrapBinding binding{database_uuid,ref.filespace_uuid,ref.page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(device,&binding);
    if(!zero.ok()){
      if(zero.error==disk::FilespacePageZeroError::resource_exhausted)return Fail(Error::resource_exhausted);
      if(zero.error==disk::FilespacePageZeroError::hash_provider_failure)return Fail(Error::hash_failure);
      if(zero.error==disk::FilespacePageZeroError::io_failure)return Fail(Error::io_failure);
      return Fail(Error::invalid_filespace);}
    const auto& z=*zero.record;
    if(z.bootstrap.filespace_role>4||ref.page_number>=z.total_pages)return Fail(Error::invalid_filespace);
    if(z.bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted)return Fail(Error::encrypted_requires_crypto_authority);
    std::vector<byte> b(z.bootstrap.page_size_bytes);const auto io=device.ReadAt(ref.page_number*z.bootstrap.page_size_bytes,b.data(),b.size());
    if(!io.ok()||io.bytes_transferred!=b.size())return Fail(Error::io_failure);
    const auto common=disk::DecodeNativeCommonPageHeader(b.data(),128);if(!common.ok())return Fail(Error::invalid_header);
    if(common.header->flags&1u)return Fail(Error::encrypted_requires_crypto_authority);
    auto result=DecodeNativeCheckpointRoot(b);if(!result.ok())return result;const auto& r=*result.root;
    if(r.header.database_uuid!=database_uuid||Self(r)!=target||r.object_uuid!=ref.object_uuid)return Fail(Error::binding_mismatch);
    if(r.predecessor&&r.predecessor->filespace_uuid==ref.filespace_uuid&&r.predecessor->page_number>=z.total_pages)return Fail(Error::invalid_reference);
    for(const auto& root:r.roots)if(root.page.filespace_uuid==ref.filespace_uuid&&root.page.page_number>=z.total_pages)return Fail(Error::invalid_reference);
    return result;
  }catch(const std::bad_alloc&){return Fail(Error::resource_exhausted);}
   catch(const std::length_error&){return Fail(Error::resource_exhausted);}
   catch(...){return Fail(Error::io_failure);}
}

NativeCheckpointInventoryResult VerifyNativeCheckpointInventoryFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u64 maximum_retained_image_bytes) noexcept {
  using namespace native_checkpoint;
  const auto fail=[](Error error){NativeCheckpointInventoryResult r;r.error=error;return r;};
  try {
    if(!V7(database_uuid)||devices.empty()||!maximum_retained_image_bytes)return fail(Error::invalid_reference);
    auto locked=LockFilespaces(devices);if(locked.error!=Error::none)return fail(locked.error);
    const auto& ordered=locked.ordered;
    const auto fs=std::lower_bound(ordered.begin(),ordered.end(),checkpoint.filespace_uuid,
      [](const auto& a,const auto& id){return a.filespace_uuid.bytes<id.bytes;});
    if(fs==ordered.end()||fs->filespace_uuid!=checkpoint.filespace_uuid||fs->page_size_profile_uuid!=checkpoint.page_size_profile_uuid)return fail(Error::invalid_filespace);
    const auto* profile=disk::FindCanonicalFilespacePageProfile(checkpoint.page_size_profile_uuid);
    if(!profile||profile->page_size_bytes>=maximum_retained_image_bytes)return fail(Error::resource_exhausted);
    auto loaded=ReadNativeCheckpointRootFromOpenDevice(*fs->device,database_uuid,checkpoint);
    if(!loaded.ok())return fail(loaded.error);
    if(!loaded.root->completed)return fail(Error::incomplete);
    const auto& target=loaded.root->roots.front();
    const disk::FilespaceRootReference inventory_ref{4,target.page_type,target.page.filespace_uuid,
      target.page.page_number,target.page.page_generation,target.page.page_size_profile_uuid,target.object_uuid};
    auto chain=scratchbird::storage::page::ReadNativeTransactionInventoryChainFromOpenDevices(
      database_uuid,ordered,inventory_ref,maximum_retained_image_bytes-loaded.bytes.size());
    if(!chain.ok()){auto result=fail(Error::inventory_failure);result.inventory_error=chain.error;return result;}
    const auto digest=hash::ComputeSha256Digest(chain.pages.front().bytes);
    if(!digest.ok())return fail(Error::hash_failure);
    if(digest.digest!=target.sha256)return fail(Error::invalid_integrity);
    const auto& root=*loaded.root;
    if(chain.inventory.next_local_transaction_id<=root.selected_local_transaction_id
      ||chain.inventory.next_local_transaction_id-1!=root.selected_local_transaction_id)return fail(Error::inventory_mismatch);
    const auto creator=scratchbird::transaction::mga::LookupLocalTransaction(chain.inventory,
      scratchbird::transaction::mga::MakeLocalTransactionId(root.creator_local_transaction_id));
    if(!creator.ok()||creator.entry.identity.transaction_uuid.value!=root.creator_transaction_uuid
      ||(!(root.flags&4)&&creator.entry.identity.scope!=scratchbird::transaction::mga::TransactionScope::local_node))return fail(Error::inventory_mismatch);
    if(!scratchbird::transaction::mga::HasCommittedInventoryOutcome(creator.entry))return fail(Error::creator_not_committed);
    const auto checkpoint_digest=hash::ComputeSha256Digest(loaded.bytes);
    if(!checkpoint_digest.ok())return fail(Error::hash_failure);
    NativeCheckpointInventoryResult result;result.error=Error::none;
    result.checkpoint_sha256=checkpoint_digest.digest;
    result.inventory_generation=chain.pages.front().page->inventory_generation;
    result.retained_image_bytes=loaded.bytes.size()+chain.retained_image_bytes;
    result.checkpoint=std::move(loaded.root);result.inventory=std::move(chain.inventory);return result;
  }catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}
   catch(const std::length_error&){return fail(Error::resource_exhausted);}
   catch(...){return fail(Error::io_failure);}
}

NativeCheckpointCatalogResult VerifyNativeCheckpointCatalogRootsFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u64 maximum_retained_image_bytes) noexcept {
  using namespace native_checkpoint;
  const auto fail=[](Error error){NativeCheckpointCatalogResult r;r.error=error;return r;};
  try {
    if(!V7(database_uuid)||devices.empty()||!maximum_retained_image_bytes)return fail(Error::invalid_reference);
    auto locked=LockFilespaces(devices);if(locked.error!=Error::none)return fail(locked.error);
    auto pair=VerifyNativeCheckpointInventoryFromOpenDevices(database_uuid,locked.ordered,checkpoint,
                                                           maximum_retained_image_bytes);
    if(!pair.ok()) {
      auto failure=fail(pair.error);
      failure.checkpoint_inventory.error=pair.error;
      failure.checkpoint_inventory.inventory_error=pair.inventory_error;
      return failure;
    }
    NativeCheckpointCatalogResult result;
    result.retained_image_bytes=pair.retained_image_bytes;
    result.catalogs.reserve(2);
    const auto& roots=pair.checkpoint->roots;
    const auto catalog=std::find_if(roots.begin(),roots.end(),[](const auto& root){return root.role==5;});
    const auto feature=std::find_if(roots.begin(),roots.end(),[](const auto& root){return root.role==9;});
    if(catalog==roots.end()||feature==roots.end())return fail(Error::invalid_roots);
    for(const auto* target:{&*catalog,&*feature}) {
      if(target==&*feature&&target->page==catalog->page&&target->object_uuid==catalog->object_uuid
          &&target->page_type==catalog->page_type&&target->sha256==catalog->sha256)continue;
      const auto fs=std::lower_bound(locked.ordered.begin(),locked.ordered.end(),target->page.filespace_uuid,
        [](const auto& device,const auto& id){return device.filespace_uuid<id;});
      if(fs==locked.ordered.end()||fs->filespace_uuid!=target->page.filespace_uuid
          ||fs->page_size_profile_uuid!=target->page.page_size_profile_uuid)return fail(Error::invalid_filespace);
      const auto* profile=disk::FindCanonicalFilespacePageProfile(target->page.page_size_profile_uuid);
      if(!profile||profile->page_size_bytes>maximum_retained_image_bytes-result.retained_image_bytes)
        return fail(Error::resource_exhausted);
      const disk::FilespaceRootReference ref{static_cast<u16>(target->role==5?2:8),target->page_type,
        target->page.filespace_uuid,target->page.page_number,target->page.page_generation,
        target->page.page_size_profile_uuid,target->object_uuid};
      auto loaded=scratchbird::storage::page::ReadNativeCatalogRootFromOpenDevice(*fs->device,database_uuid,ref);
      if(!loaded.ok()) {auto failure=fail(Error::catalog_failure);failure.catalog_error=loaded.error;return failure;}
      const auto digest=hash::ComputeSha256Digest(loaded.bytes);
      if(!digest.ok())return fail(Error::hash_failure);
      if(digest.digest!=target->sha256)return fail(Error::invalid_integrity);
      const auto creator=scratchbird::transaction::mga::LookupLocalTransaction(pair.inventory,
        scratchbird::transaction::mga::MakeLocalTransactionId(loaded.root->creator_local_transaction_id));
      if(!creator.ok()||creator.entry.identity.transaction_uuid.value!=loaded.root->creator_transaction_uuid
          ||(!(pair.checkpoint->flags&4)&&creator.entry.identity.scope!=scratchbird::transaction::mga::TransactionScope::local_node))
        return fail(Error::catalog_creator_mismatch);
      if(!scratchbird::transaction::mga::HasCommittedInventoryOutcome(creator.entry))
        return fail(Error::catalog_creator_not_committed);
      result.retained_image_bytes+=loaded.bytes.size();
      if(target==&*feature)result.feature_root_index=1;
      result.catalogs.push_back(std::move(loaded));
    }
    result.checkpoint_inventory=std::move(pair);
    result.error=Error::none;
    return result;
  }catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}
   catch(const std::length_error&){return fail(Error::resource_exhausted);}
   catch(...){return fail(Error::io_failure);}
}

NativeCheckpointPolicyRootsResult VerifyNativeCheckpointPolicyRootsFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u64 maximum_retained_image_bytes) noexcept {
  using namespace native_checkpoint;
  namespace mga=scratchbird::transaction::mga;
  const auto fail=[](Error error){NativeCheckpointPolicyRootsResult r;r.error=error;return r;};
  try {
    if(!V7(database_uuid)||devices.empty()||!maximum_retained_image_bytes)return fail(Error::invalid_reference);
    auto locked=LockFilespaces(devices);if(locked.error!=Error::none)return fail(locked.error);
    auto catalog=VerifyNativeCheckpointCatalogRootsFromOpenDevices(database_uuid,locked.ordered,checkpoint,
                                                                  maximum_retained_image_bytes);
    if(!catalog.ok()) {
      auto failure=fail(catalog.error);failure.catalog_error=catalog.catalog_error;
      failure.catalog.error=catalog.error;
      failure.catalog.checkpoint_inventory.error=catalog.checkpoint_inventory.error;
      failure.catalog.checkpoint_inventory.inventory_error=catalog.checkpoint_inventory.inventory_error;
      return failure;
    }
    NativeCheckpointPolicyRootsResult result;result.retained_image_bytes=catalog.retained_image_bytes;
    const auto& pair=catalog.checkpoint_inventory;
    for(std::size_t index=0;index<2;++index) {
      const u16 kind=static_cast<u16>(index+6);
      const auto target=std::find_if(pair.checkpoint->roots.begin(),pair.checkpoint->roots.end(),
                                     [kind](const auto& root){return root.role==kind;});
      if(target==pair.checkpoint->roots.end())return fail(Error::invalid_roots);
      const auto fs=std::lower_bound(locked.ordered.begin(),locked.ordered.end(),target->page.filespace_uuid,
          [](const auto& file,const auto& id){return file.filespace_uuid<id;});
      if(fs==locked.ordered.end()||fs->filespace_uuid!=target->page.filespace_uuid||
          fs->page_size_profile_uuid!=target->page.page_size_profile_uuid)return fail(Error::invalid_filespace);
      const auto* profile=disk::FindCanonicalFilespacePageProfile(target->page.page_size_profile_uuid);
      if(!profile||profile->page_size_bytes>maximum_retained_image_bytes-result.retained_image_bytes)
        return fail(Error::resource_exhausted);
      const disk::FilespaceRootReference ref{kind,target->page_type,target->page.filespace_uuid,
          target->page.page_number,target->page.page_generation,target->page.page_size_profile_uuid,target->object_uuid};
      auto root=scratchbird::storage::page::ReadNativeCatalogRootFromOpenDevice(*fs->device,database_uuid,ref);
      if(!root.ok()){auto failure=fail(Error::catalog_failure);failure.catalog_error=root.error;return failure;}
      const auto digest=hash::ComputeSha256Digest(root.bytes);
      if(!digest.ok())return fail(Error::hash_failure);
      if(digest.digest!=target->sha256)return fail(Error::invalid_integrity);
      const auto creator=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(root.root->creator_local_transaction_id));
      if(!creator.ok()||creator.entry.identity.transaction_uuid.value!=root.root->creator_transaction_uuid||
          (!(pair.checkpoint->flags&4)&&creator.entry.identity.scope!=mga::TransactionScope::local_node))
        return fail(Error::catalog_creator_mismatch);
      if(!mga::HasCommittedInventoryOutcome(creator.entry))return fail(Error::catalog_creator_not_committed);
      const auto& actual=root.root->roots.front();
      const auto& expected=catalog.catalogs.front().root->roots[index==0?4:3];
      if(actual.role!=expected.role||actual.page_type!=expected.page_type||actual.page!=expected.page||
          actual.object_uuid!=expected.object_uuid)return fail(Error::policy_relation_mismatch);
      result.retained_image_bytes+=root.bytes.size();result.policies[index]=std::move(root);
    }
    result.catalog=std::move(catalog);result.error=Error::none;return result;
  }catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}
   catch(const std::length_error&){return fail(Error::resource_exhausted);}
   catch(...){return fail(Error::io_failure);}
}

NativeCheckpointAllocationResult VerifyCurrentNativeCheckpointAllocationFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u64 maximum_retained_image_bytes) noexcept {
  using namespace native_checkpoint;
  namespace mga = scratchbird::transaction::mga;
  namespace page = scratchbird::storage::page;
  const auto fail=[](Error error){NativeCheckpointAllocationResult result;result.error=error;return result;};
  try {
    if(!V7(database_uuid)||devices.empty()||!maximum_retained_image_bytes)return fail(Error::invalid_reference);
    auto locked=LockFilespaces(devices);if(locked.error!=Error::none)return fail(locked.error);
    auto pair=VerifyNativeCheckpointInventoryFromOpenDevices(database_uuid,locked.ordered,checkpoint,
                                                            maximum_retained_image_bytes);
    if(!pair.ok()) {
      auto result=fail(pair.error);result.checkpoint_inventory.error=pair.error;
      result.checkpoint_inventory.inventory_error=pair.inventory_error;return result;
    }
    const auto fs=std::lower_bound(locked.ordered.begin(),locked.ordered.end(),checkpoint.filespace_uuid,
        [](const auto& entry,const auto& id){return entry.filespace_uuid<id;});
    if(fs==locked.ordered.end()||fs->filespace_uuid!=checkpoint.filespace_uuid||
        fs->page_size_profile_uuid!=checkpoint.page_size_profile_uuid)return fail(Error::invalid_filespace);
    const disk::FilespaceBootstrapBinding binding{database_uuid,fs->filespace_uuid,fs->page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*fs->device,&binding);
    if(!zero.ok()) {
      if(zero.error==disk::FilespacePageZeroError::resource_exhausted)return fail(Error::resource_exhausted);
      if(zero.error==disk::FilespacePageZeroError::hash_provider_failure)return fail(Error::hash_failure);
      if(zero.error==disk::FilespacePageZeroError::io_failure)return fail(Error::io_failure);
      return fail(Error::invalid_filespace);
    }
    const auto& z=*zero.record;
    const auto same=[](const auto& a,const auto& b){return a.kind==b.kind&&a.page_type==b.page_type&&
        a.filespace_uuid==b.filespace_uuid&&a.page_number==b.page_number&&a.page_generation==b.page_generation&&
        a.page_size_profile_uuid==b.page_size_profile_uuid&&a.object_uuid==b.object_uuid;};
    const auto current=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& ref){return ref.kind==9;});
    if(current==z.roots.end()||!same(*current,checkpoint)||pair.checkpoint->root_set_generation!=z.root_set_generation)
      return fail(Error::binding_mismatch);
    const auto target=std::find_if(pair.checkpoint->roots.begin(),pair.checkpoint->roots.end(),
                                  [](const auto& ref){return ref.role==4;});
    const auto actual=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& ref){return ref.kind==3;});
    if(target==pair.checkpoint->roots.end()||actual==z.roots.end())return fail(Error::invalid_roots);
    const disk::FilespaceRootReference expected{3,target->page_type,target->page.filespace_uuid,
        target->page.page_number,target->page.page_generation,target->page.page_size_profile_uuid,target->object_uuid};
    if(!same(*actual,expected))return fail(Error::binding_mismatch);
    auto allocation=page::ReadNativeAllocationChainFromOpenDevice(*fs->device,binding,
        maximum_retained_image_bytes-pair.retained_image_bytes);
    if(!allocation.ok()) {
      auto result=fail(Error::allocation_failure);result.allocation_error=allocation.error;return result;
    }
    const auto digest=hash::ComputeSha256Digest(allocation.pages.front().bytes);
    if(!digest.ok())return fail(Error::hash_failure);
    if(digest.digest!=target->sha256)return fail(Error::invalid_integrity);
    for(const auto& image:allocation.pages) {
      const auto& map=*image.map;
      const auto creator=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(map.creator_local_transaction_id));
      if(!creator.ok()||creator.entry.identity.transaction_uuid.value!=map.creator_transaction_uuid||
          (!(pair.checkpoint->flags&4)&&creator.entry.identity.scope!=mga::TransactionScope::local_node))
        return fail(Error::allocation_creator_mismatch);
      if(!mga::HasCommittedInventoryOutcome(creator.entry))return fail(Error::allocation_creator_not_committed);
      for(const auto& record:map.records) {
        const auto original=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(record.creator_local_transaction_id));
        if(!original.ok()||original.entry.identity.transaction_uuid.value!=record.creator_transaction_uuid||
            (!(pair.checkpoint->flags&4)&&original.entry.identity.scope!=mga::TransactionScope::local_node))
          return fail(Error::allocation_record_creator_mismatch);
      }
    }
    NativeCheckpointAllocationResult result;result.error=Error::none;
    result.retained_image_bytes=pair.retained_image_bytes+allocation.retained_image_bytes;
    result.checkpoint_inventory=std::move(pair);result.allocation=std::move(allocation);return result;
  }catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}
   catch(const std::length_error&){return fail(Error::resource_exhausted);}
   catch(...){return fail(Error::io_failure);}
}

NativeCheckpointHistoryResult VerifyNativeCheckpointHistoryFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& head,
    const scratchbird::storage::disk::FilespaceRootReference& terminal,
    u64 maximum_retained_image_bytes) noexcept {
  using namespace native_checkpoint;
  const auto fail=[](Error error){NativeCheckpointHistoryResult r;r.error=error;return r;};
  const auto page_ref=[](const disk::FilespaceRootReference& r){return disk::NativePageReference{r.filespace_uuid,r.page_number,r.page_generation,r.page_size_profile_uuid};};
  try {
    if(!V7(database_uuid)||!V7(head.object_uuid)||head.object_uuid!=terminal.object_uuid
      ||head.kind!=9||terminal.kind!=9||head.page_type!=0x300||terminal.page_type!=0x300
      ||!RefValid(page_ref(head))||!RefValid(page_ref(terminal))||!maximum_retained_image_bytes)return fail(Error::invalid_reference);
    auto locked=LockFilespaces(devices);if(locked.error!=Error::none)return fail(locked.error);
    NativeCheckpointHistoryResult result;auto next=head;
    std::set<std::pair<Uuid,u64>> slots;std::set<Uuid> page_ids;
    for(;;) {
      if(result.retained_image_bytes>=maximum_retained_image_bytes)return fail(Error::resource_exhausted);
      if(!slots.emplace(next.filespace_uuid,next.page_number).second)return fail(Error::history_mismatch);
      auto pair=VerifyNativeCheckpointInventoryFromOpenDevices(database_uuid,locked.ordered,next,
        maximum_retained_image_bytes-result.retained_image_bytes);
      if(!pair.ok()){auto error=fail(pair.error);error.inventory_error=pair.inventory_error;return error;}
      const auto& older=*pair.checkpoint;
      if(!page_ids.insert(older.header.page_uuid).second)return fail(Error::history_mismatch);
      if(!result.checkpoints.empty()) {
        const auto& newer_pair=result.checkpoints.back();const auto& newer=*newer_pair.checkpoint;
        if(newer.predecessor_sha256!=pair.checkpoint_sha256)return fail(Error::invalid_integrity);
        if(older.object_uuid!=newer.object_uuid||older.timeline_uuid!=newer.timeline_uuid
          ||newer.checkpoint_generation<=1||older.checkpoint_generation!=newer.checkpoint_generation-1
          ||older.root_set_generation>newer.root_set_generation||older.selected_local_transaction_id>newer.selected_local_transaction_id
          ||older.stable_local_transaction_id>newer.stable_local_transaction_id||older.local_durable_transaction_id>newer.local_durable_transaction_id
          ||older.cluster_quorum_transaction_id>newer.cluster_quorum_transaction_id||pair.inventory_generation>newer_pair.inventory_generation
          ||pair.inventory.next_local_transaction_id>newer_pair.inventory.next_local_transaction_id
          ||pair.inventory.next_commit_sequence>newer_pair.inventory.next_commit_sequence)return fail(Error::history_mismatch);
        if((older.root_set_generation==newer.root_set_generation&&older.roots!=newer.roots)
          ||(pair.inventory_generation==newer_pair.inventory_generation&&older.roots.front()!=newer.roots.front()))return fail(Error::history_mismatch);
        if(*scratchbird::transaction::mga::ValidateLocalTransactionInventoryEvolution(pair.inventory,newer_pair.inventory))
          return fail(Error::inventory_mismatch);
      }
      result.retained_image_bytes+=pair.retained_image_bytes;result.checkpoints.push_back(std::move(pair));
      if(page_ref(next)==page_ref(terminal)){result.error=Error::none;return result;}
      const auto& current=*result.checkpoints.back().checkpoint;
      if(!current.predecessor)return fail(Error::history_mismatch);
      const auto& previous=*current.predecessor;
      next={9,0x300,previous.filespace_uuid,previous.page_number,previous.page_generation,previous.page_size_profile_uuid,current.object_uuid};
    }
  }catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}
   catch(const std::length_error&){return fail(Error::resource_exhausted);}
   catch(...){return fail(Error::io_failure);}
}

}  // namespace scratchbird::storage::database
