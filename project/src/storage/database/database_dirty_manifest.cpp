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
#include <filesystem>
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
using scratchbird::core::uuid::UuidKindName;

constexpr const char* kDirtyManifestMagic = "SBDIRTY2";
constexpr const char* kRecoveryEvidenceMagic = "SBRECV02";
constexpr std::size_t kRecoveryEvidenceBytes = 64;

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

constexpr std::size_t kDirtyHeaderBytes = 40;
constexpr std::size_t kDirtyEntryBytes = 72;
void PutDirtyInteger(std::string* bytes, u64 value, unsigned width) {
  for (unsigned n = 0; n < width; ++n) bytes->push_back(static_cast<char>(value >> (8 * n)));
}
u64 GetDirtyInteger(const std::string& bytes, std::size_t offset, unsigned width) {
  u64 value = 0;
  for (unsigned n = 0; n < width; ++n)
    value |= static_cast<u64>(static_cast<unsigned char>(bytes[offset + n])) << (8 * n);
  return value;
}
std::string EncodeDirtyManifest(const DirtyObjectManifest& manifest, bool include_checksum) {
  std::string bytes(kDirtyManifestMagic);
  PutDirtyInteger(&bytes, manifest.format_version, 4);
  PutDirtyInteger(&bytes, (manifest.classification_only ? 1u : 0u) | (manifest.completed ? 2u : 0u), 4);
  PutDirtyInteger(&bytes, manifest.checkpoint_generation, 8);
  PutDirtyInteger(&bytes, manifest.entries.size(), 8);
  PutDirtyInteger(&bytes, include_checksum ? manifest.manifest_checksum : 0, 8);
  for (const auto& entry : manifest.entries) {
    PutDirtyInteger(&bytes, static_cast<u16>(entry.kind), 2);
    PutDirtyInteger(&bytes, static_cast<u16>(entry.object_uuid.kind), 2);
    bytes.append(reinterpret_cast<const char*>(entry.object_uuid.value.bytes.data()), 16);
    PutDirtyInteger(&bytes, entry.page_number, 8);
    PutDirtyInteger(&bytes, entry.page_generation, 8);
    PutDirtyInteger(&bytes, entry.object_checksum, 8);
    PutDirtyInteger(&bytes, entry.local_transaction_id, 8);
    PutDirtyInteger(&bytes, entry.operation_envelope_checksum, 8);
    PutDirtyInteger(&bytes, entry.transaction_evidence_checksum, 8);
    PutDirtyInteger(&bytes, (entry.dirty ? 1u : 0u) | (entry.authoritative ? 2u : 0u), 4);
  }
  return bytes;
}
std::string DirtyManifestChecksumMaterial(const DirtyObjectManifest& manifest) {
  return EncodeDirtyManifest(manifest, false);
}
std::string SerializeDirtyManifest(const DirtyObjectManifest& manifest) {
  return EncodeDirtyManifest(manifest, true);
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
           << UuidKindName(classification.object_uuid.kind) << ':';
    stable.write(reinterpret_cast<const char*>(classification.object_uuid.value.bytes.data()), 16);
    stable << ':' << classification.page_number << ':'
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
  std::string bytes(kRecoveryEvidenceMagic);
  bytes.append(reinterpret_cast<const char*>(evidence.recovery_run_uuid.value.bytes.data()), 16);
  PutDirtyInteger(&bytes, evidence.checkpoint_generation, 8);
  PutDirtyInteger(&bytes, evidence.classification_count, 8);
  PutDirtyInteger(&bytes, evidence.classification_checksum, 8);
  const u32 action = evidence.recovery_action == "quarantine" ? 1u :
      evidence.recovery_action == "classify_and_rebuild_by_manifest" ? 2u : 3u;
  PutDirtyInteger(&bytes, action, 4);
  PutDirtyInteger(&bytes, evidence.completed ? 1u : 0u, 4);
  PutDirtyInteger(&bytes, StableTextChecksum(bytes), 8);
  return bytes;
}

bool ParseRecoveryEvidenceRecord(const std::string& bytes, DirtyManifestRecoveryRunEvidence* evidence) {
  if (!evidence || bytes.size() != kRecoveryEvidenceBytes || !bytes.starts_with(kRecoveryEvidenceMagic) ||
      GetDirtyInteger(bytes, 56, 8) != StableTextChecksum(bytes.substr(0, 56)) ||
      GetDirtyInteger(bytes, 52, 4) != 1) return false;
  scratchbird::core::platform::Uuid identity;
  std::copy_n(reinterpret_cast<const unsigned char*>(bytes.data() + 8), 16, identity.bytes.begin());
  const auto typed = scratchbird::core::uuid::MakeDurableEngineIdentityUuid(UuidKind::object, identity);
  if (!typed.ok() || !scratchbird::core::uuid::IsEngineIdentityUuid(identity)) return false;
  DirtyManifestRecoveryRunEvidence staged;
  staged.recovery_run_uuid = typed.value;
  staged.checkpoint_generation = GetDirtyInteger(bytes, 24, 8);
  staged.classification_count = GetDirtyInteger(bytes, 32, 8);
  staged.classification_checksum = GetDirtyInteger(bytes, 40, 8);
  const auto action = GetDirtyInteger(bytes, 48, 4);
  if (action < 1 || action > 3 || staged.checkpoint_generation == 0) return false;
  staged.recovery_action = action == 1 ? "quarantine" :
      action == 2 ? "classify_and_rebuild_by_manifest" : "no_action";
  staged.completed = true;
  *evidence = std::move(staged);
  return true;
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
    if (static_cast<u16>(entry.kind) >= static_cast<u16>(DirtyObjectKind::unknown) ||
        !scratchbird::core::uuid::MakeDurableEngineIdentityUuid(entry.object_uuid.kind, entry.object_uuid.value).ok() ||
        !scratchbird::core::uuid::IsEngineIdentityUuid(entry.object_uuid.value) ||
        !entry.authoritative ||
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
  // Text formats are not admitted. Scan only refused legacy input for its
  // historical diagnostic; arbitrary binary UUID bytes are never prose.
  if (!serialized.starts_with(kDirtyManifestMagic)) {
    if (ContainsForbiddenRedoTerm(serialized)) {
      return ManifestError("RECOVERY.MANIFEST_WAL_CONFUSION_FORBIDDEN",
                           "recovery.dirty_manifest.redo_terms_forbidden",
                           "dirty manifest must not contain WAL, LSN, or write-ahead redo authority");
    }
    return ManifestError("SB-DIRTY-MANIFEST-MAGIC-INVALID", "recovery.dirty_manifest.magic_invalid");
  }
  if (serialized.size() < kDirtyHeaderBytes) {
    return ManifestError("SB-DIRTY-MANIFEST-HEADER-INVALID", "recovery.dirty_manifest.header_invalid");
  }
  DirtyObjectManifest manifest;
  manifest.format_version = static_cast<u32>(GetDirtyInteger(serialized, 8, 4));
  if (manifest.format_version != kDirtyObjectManifestFormatVersion) {
    return ManifestError("SB-DIRTY-MANIFEST-FORMAT-UNSUPPORTED", "recovery.dirty_manifest.format_unsupported");
  }
  const auto flags = GetDirtyInteger(serialized, 12, 4);
  if ((flags & 1u) == 0) {
    return ManifestError("RECOVERY.MANIFEST_WAL_CONFUSION_FORBIDDEN",
                         "recovery.dirty_manifest.classification_only_required");
  }
  if (flags != 3) {
    return ManifestError("SB-DIRTY-MANIFEST-HEADER-INVALID", "recovery.dirty_manifest.header_invalid");
  }
  manifest.classification_only = true;
  manifest.completed = true;
  manifest.checkpoint_generation = GetDirtyInteger(serialized, 16, 8);
  const auto count = GetDirtyInteger(serialized, 24, 8);
  manifest.manifest_checksum = GetDirtyInteger(serialized, 32, 8);
  if ((serialized.size() - kDirtyHeaderBytes) % kDirtyEntryBytes != 0 ||
      count != (serialized.size() - kDirtyHeaderBytes) / kDirtyEntryBytes) {
    return ManifestError("SB-DIRTY-MANIFEST-ENTRY-COUNT-MISMATCH", "recovery.dirty_manifest.entry_count_mismatch");
  }
  for (std::size_t offset = kDirtyHeaderBytes; offset < serialized.size(); offset += kDirtyEntryBytes) {
    DirtyObjectManifestEntry entry;
    entry.kind = static_cast<DirtyObjectKind>(GetDirtyInteger(serialized, offset, 2));
    const auto kind = static_cast<UuidKind>(GetDirtyInteger(serialized, offset + 2, 2));
    scratchbird::core::platform::Uuid identity;
    std::copy_n(reinterpret_cast<const unsigned char*>(serialized.data() + offset + 4), 16,
                identity.bytes.begin());
    const auto parsed = scratchbird::core::uuid::MakeDurableEngineIdentityUuid(kind, identity);
    const auto entry_flags = GetDirtyInteger(serialized, offset + 68, 4);
    if (!parsed.ok() || entry_flags > 3 ||
        static_cast<u16>(entry.kind) >= static_cast<u16>(DirtyObjectKind::unknown)) {
      return ManifestError("SB-DIRTY-MANIFEST-ENTRY-INVALID", "recovery.dirty_manifest.entry_invalid");
    }
    entry.object_uuid = parsed.value;
    entry.page_number = GetDirtyInteger(serialized, offset + 20, 8);
    entry.page_generation = GetDirtyInteger(serialized, offset + 28, 8);
    entry.object_checksum = GetDirtyInteger(serialized, offset + 36, 8);
    entry.local_transaction_id = GetDirtyInteger(serialized, offset + 44, 8);
    entry.operation_envelope_checksum = GetDirtyInteger(serialized, offset + 52, 8);
    entry.transaction_evidence_checksum = GetDirtyInteger(serialized, offset + 60, 8);
    entry.dirty = (entry_flags & 1u) != 0;
    entry.authoritative = (entry_flags & 2u) != 0;
    manifest.entries.push_back(std::move(entry));
  }
  if (manifest.checkpoint_generation == 0) {
    return ManifestError("SB-DIRTY-MANIFEST-HEADER-INVALID", "recovery.dirty_manifest.header_invalid");
  }
  if (manifest.manifest_checksum == 0 || manifest.manifest_checksum != DirtyManifestChecksum(manifest)) {
    return ManifestError("SB-DIRTY-MANIFEST-CHECKSUM-MISMATCH", "recovery.dirty_manifest.checksum_mismatch");
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
    const TypedUuid& recovery_run_uuid) {
  if (evidence_store_path.empty()) {
    return RecoveryEvidenceError("SB-RECOVERY-EVIDENCE-PATH-REQUIRED",
                                 "recovery.run_evidence.path_required");
  }
  if (!manifest.completed || manifest.checkpoint_generation == 0 || !manifest.classification_only || !recovery.ok()) {
    return RecoveryEvidenceError("SB-RECOVERY-EVIDENCE-INPUT-INVALID",
                                 "recovery.run_evidence.input_invalid");
  }
  if (recovery_run_uuid.kind != UuidKind::object ||
      !scratchbird::core::uuid::IsEngineIdentityUuid(recovery_run_uuid.value)) {
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
  if (!existing_in.is_open() && std::filesystem::exists(evidence_store_path)) {
    return RecoveryEvidenceError("SB-RECOVERY-EVIDENCE-INPUT-INVALID",
                                 "recovery.run_evidence.input_invalid", "evidence_read_failed");
  }
  std::optional<DirtyManifestRecoveryRunEvidenceResult> recorded;
  while (existing_in.is_open()) {
    std::string existing_line(kRecoveryEvidenceBytes, '\0');
    existing_in.read(existing_line.data(), existing_line.size());
    if (existing_in.gcount() == 0 && existing_in.eof() && !existing_in.bad()) break;
    DirtyManifestRecoveryRunEvidence existing;
    if (existing_in.gcount() != static_cast<std::streamsize>(existing_line.size()) ||
        existing_in.bad() || !ParseRecoveryEvidenceRecord(existing_line, &existing)) {
      return RecoveryEvidenceError("SB-RECOVERY-EVIDENCE-INPUT-INVALID",
                                   "recovery.run_evidence.input_invalid", "invalid_or_truncated_evidence");
    }
    if (existing.checkpoint_generation == evidence.checkpoint_generation &&
        existing.classification_count == evidence.classification_count &&
        existing.classification_checksum == evidence.classification_checksum) {
      DirtyManifestRecoveryRunEvidenceResult result;
      result.status = DirtyManifestOkStatus();
      result.already_recorded = true;
      result.evidence = std::move(existing);
      result.serialized = existing_line;
      recorded = std::move(result);
    }
  }

  if (recorded) return std::move(*recorded);
  const std::string serialized = SerializeRecoveryEvidence(evidence);
  std::ofstream out(evidence_store_path, std::ios::app | std::ios::binary);
  if (!out) {
    return RecoveryEvidenceError("SB-RECOVERY-EVIDENCE-APPEND-FAILED",
                                 "recovery.run_evidence.append_failed");
  }
  out.write(serialized.data(), serialized.size());
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
