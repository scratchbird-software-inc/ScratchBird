// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_dirty_manifest.hpp"
#include "native_checkpoint_selection.hpp"
#include "native_management_control_authority.hpp"
#include "hash_digest_parts.hpp"
#include "disk_device.hpp"
#include "transaction_inventory_validation.hpp"

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
constexpr std::array<byte,8> operation_magic{{'S','B','C','P','N','T','0','2'}};
constexpr std::array<byte,8> operation_domain{{'S','B','C','P','S','E','T','2'}};
constexpr std::array<u32,17> types{{0,0x301,0x302,9,3,5,10,11,8,5,0x303,0x305,0x307,0x308,0x309,0x30b,0x500}};
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
  const bool transaction_owner=r.creator_operation_uuid.is_nil();
  const bool valid_creator=transaction_owner?
    V7(r.creator_transaction_uuid)&&r.creator_local_transaction_id&&r.creator_local_transaction_id<=r.selected_local_transaction_id:
    V7(r.creator_operation_uuid)&&r.creator_transaction_uuid.is_nil()&&r.creator_local_transaction_id==0;
  if(!V7(r.object_uuid)||!V7(r.timeline_uuid)||!valid_creator||!r.checkpoint_generation
    ||!r.root_set_generation||!r.selected_local_transaction_id
    ||r.stable_local_transaction_id>r.local_durable_transaction_id
    ||r.local_durable_transaction_id>r.selected_local_transaction_id
    ||r.cluster_quorum_transaction_id>r.local_durable_transaction_id||(r.flags&~15ull))return Error::invalid_family;
  const auto self=Self(r);if(!RefValid(self))return Error::invalid_reference;
  const bool empty_digest=Zero(r.predecessor_sha256.data(),32);
  if((r.checkpoint_generation==1&&(r.predecessor||!empty_digest))
    ||(r.checkpoint_generation>1&&(!r.predecessor||empty_digest)))return Error::invalid_reference;
  if(r.predecessor&&(!RefValid(*r.predecessor)||SameSlot(self,*r.predecessor)||!ProfilesAgree(self,*r.predecessor)))return Error::invalid_reference;
  if(r.roots.size()<10||r.roots.size()>16)return Error::invalid_roots;
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
  if(LoadLittle16(b.data()+family+8)==2){
    const hash::HashDigestSegment parts[]={{operation_domain.data(),operation_domain.size()},
      {b.data()+family+32,96},{b.data()+family+280,16},{b.data()+entries,used-entries}};
    return hash::ComputeSha256DigestParts(parts,4);
  }
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
    const bool operation_owner=!r.creator_operation_uuid.is_nil();
    const auto& image_magic=operation_owner?operation_magic:magic;
    std::copy(image_magic.begin(),image_magic.end(),f);StoreLittle16(f+8,operation_owner?2:1);StoreLittle16(f+10,384);
    const auto used=entries+112*r.roots.size();StoreLittle32(f+12,static_cast<u32>(used));Put(f+16,r.object_uuid);
    StoreLittle64(f+32,r.checkpoint_generation);StoreLittle64(f+40,r.root_set_generation);
    StoreLittle64(f+48,r.selected_local_transaction_id);StoreLittle64(f+56,r.stable_local_transaction_id);
    StoreLittle64(f+64,r.local_durable_transaction_id);StoreLittle64(f+72,r.cluster_quorum_transaction_id);
    Put(f+80,r.timeline_uuid);Put(f+96,r.creator_transaction_uuid);StoreLittle64(f+112,r.creator_local_transaction_id);StoreLittle64(f+120,r.flags);
    if(r.predecessor)PutRef(f+128,*r.predecessor);
    std::copy(r.predecessor_sha256.begin(),r.predecessor_sha256.end(),f+176);
    StoreLittle64(f+272,r.completed?1:0);
    Put(f+280,r.creator_operation_uuid);
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
    const auto version=LoadLittle16(f+8);
    if(!((version==1&&std::equal(magic.begin(),magic.end(),f))||
         (version==2&&std::equal(operation_magic.begin(),operation_magic.end(),f)))||LoadLittle16(f+10)!=384
      ||used<entries+112*10||used>entries+112*16||(used-entries)%112||LoadLittle64(f+272)>1
      ||!Zero(f+(version==1?280:296),version==1?104:88)||!Zero(b.data()+used,b.size()-used))return Fail(Error::invalid_family);
    const auto digest=RootDigest(b,used);if(!digest.ok())return Fail(Error::hash_failure);
    if(!std::equal(digest.digest.begin(),digest.digest.end(),b.begin()+root_digest_at))return Fail(Error::invalid_integrity);
    NativeCheckpointRoot r;r.header=*common.header;r.object_uuid=Get(f+16);
    r.checkpoint_generation=LoadLittle64(f+32);r.root_set_generation=LoadLittle64(f+40);
    r.selected_local_transaction_id=LoadLittle64(f+48);r.stable_local_transaction_id=LoadLittle64(f+56);
    r.local_durable_transaction_id=LoadLittle64(f+64);r.cluster_quorum_transaction_id=LoadLittle64(f+72);
    r.timeline_uuid=Get(f+80);r.creator_transaction_uuid=Get(f+96);r.creator_local_transaction_id=LoadLittle64(f+112);r.flags=LoadLittle64(f+120);
    if(version==2){r.creator_operation_uuid=Get(f+280);if(r.creator_operation_uuid.is_nil())return Fail(Error::invalid_family);}
    if(!Zero(f+128,48))r.predecessor=GetRef(f+128);
    std::copy_n(f+176,32,r.predecessor_sha256.begin());r.completed=LoadLittle64(f+272)==1;
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
    if(!chain.ok()){
      using I=scratchbird::storage::page::NativeInventoryError;
      const auto error=chain.error==I::resource_exhausted?Error::resource_exhausted:
        chain.error==I::hash_failure?Error::hash_failure:
        chain.error==I::io_failure?Error::io_failure:
        chain.error==I::encrypted_requires_crypto_authority?Error::encrypted_requires_crypto_authority:
        Error::inventory_failure;
      auto result=fail(error);result.inventory_error=chain.error;return result;
    }
    const auto digest=hash::ComputeSha256Digest(chain.pages.front().bytes);
    if(!digest.ok())return fail(Error::hash_failure);
    if(digest.digest!=target.sha256)return fail(Error::invalid_integrity);
    const auto& root=*loaded.root;
    if(chain.inventory.next_local_transaction_id<=root.selected_local_transaction_id
      ||chain.inventory.next_local_transaction_id-1!=root.selected_local_transaction_id)return fail(Error::inventory_mismatch);
    const auto checkpoint_digest=hash::ComputeSha256Digest(loaded.bytes);
    if(!checkpoint_digest.ok())return fail(Error::hash_failure);
    u64 operation_work=0;
    if(root.creator_operation_uuid.is_nil()){
      const auto creator=scratchbird::transaction::mga::LookupLocalTransaction(chain.inventory,
        scratchbird::transaction::mga::MakeLocalTransactionId(root.creator_local_transaction_id));
      if(!creator.ok()||creator.entry.identity.transaction_uuid.value!=root.creator_transaction_uuid
        ||(!(root.flags&4)&&creator.entry.identity.scope!=scratchbird::transaction::mga::TransactionScope::local_node))return fail(Error::inventory_mismatch);
      if(!scratchbird::transaction::mga::HasCommittedInventoryOutcome(creator.entry))return fail(Error::creator_not_committed);
    }else{
      const auto proof=ReadNativeManagementControlAuthorityFromOpenDevices(database_uuid,ordered,
        root.header.filespace_uuid,maximum_retained_image_bytes-loaded.bytes.size()-chain.retained_image_bytes);
      if(!proof.ok()){using P=NativeManagementControlAuthorityError;return fail(proof.error==P::hash_failure?Error::hash_failure:proof.error==P::resource_exhausted?Error::resource_exhausted:proof.error==P::io_failure?Error::io_failure:proof.error==P::encrypted_requires_authority?Error::encrypted_requires_crypto_authority:proof.error==P::cluster_requires_authority?Error::cluster_requires_authority:Error::creator_not_committed);}
      if(!MatchesNativeManagementPublishedCheckpoint(proof,root,checkpoint_digest.digest))return fail(Error::creator_not_committed);
      operation_work=proof.verified_image_bytes;
    }
    NativeCheckpointInventoryResult result;result.error=Error::none;
    result.checkpoint_sha256=checkpoint_digest.digest;
    result.inventory_generation=chain.pages.front().page->inventory_generation;
    result.retained_image_bytes=loaded.bytes.size()+chain.retained_image_bytes+operation_work;
    result.inventory_pages.reserve(chain.pages.size());
    for(const auto& image:chain.pages)
      result.inventory_pages.push_back({image.page->header,image.page->object_uuid});
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

namespace {
struct CurrentCheckpointInventory {
  NativeCheckpointInventoryResult pair;
  bool selector_bound=false;
};
CurrentCheckpointInventory ResolveCurrentCheckpointInventory(
    const core::platform::Uuid& database_uuid,const std::vector<disk::NativeFilespaceDevice>& devices,
    const disk::FilespaceRootReference& requested,u64 budget) {
  CurrentCheckpointInventory result;const disk::NativeFilespaceDevice* selector_owner=nullptr;
  for(const auto& file:devices){
    const disk::FilespaceBootstrapBinding binding{database_uuid,file.filespace_uuid,file.page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*file.device,&binding);
    if(!zero.ok()){result.pair.error=zero.error==disk::FilespacePageZeroError::resource_exhausted?NativeCheckpointError::resource_exhausted:
      zero.error==disk::FilespacePageZeroError::hash_provider_failure?NativeCheckpointError::hash_failure:
      zero.error==disk::FilespacePageZeroError::io_failure?NativeCheckpointError::io_failure:NativeCheckpointError::invalid_filespace;return result;}
    if(std::any_of(zero.record->roots.begin(),zero.record->roots.end(),[](const auto& r){return r.kind==18;})){
      if(selector_owner){result.pair.error=NativeCheckpointError::binding_mismatch;return result;}selector_owner=&file;
    }
  }
  if(!selector_owner){result.pair=VerifyNativeCheckpointInventoryFromOpenDevices(database_uuid,devices,requested,budget);return result;}
  auto selected=ReadNativeBoundCheckpointSelectionFromOpenDevices(database_uuid,devices,selector_owner->filespace_uuid,budget);
  if(!selected.ok()){result.pair.error=selected.error==NativeCheckpointSelectionError::resource_exhausted?NativeCheckpointError::resource_exhausted:
    selected.error==NativeCheckpointSelectionError::hash_failure?NativeCheckpointError::hash_failure:
    selected.error==NativeCheckpointSelectionError::io_failure?NativeCheckpointError::io_failure:NativeCheckpointError::binding_mismatch;return result;}
  const auto& s=*selected.selection;
  if(requested.kind!=9||requested.page_type!=0x300||requested.object_uuid!=s.checkpoint_object_uuid||
    disk::NativePageReference{requested.filespace_uuid,requested.page_number,requested.page_generation,requested.page_size_profile_uuid}!=s.checkpoint){
    result.pair.error=NativeCheckpointError::binding_mismatch;return result;
  }
  result.pair=std::move(selected.checkpoint_inventory);
  // Selection admission also read predecessors, allocation maps and exact
  // operation lineage. Its downstream consumers must charge that complete
  // work, not just the retained checkpoint/inventory subset moved above.
  result.pair.retained_image_bytes=selected.retained_image_bytes;
  result.selector_bound=true;return result;
}
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
    auto current_inventory=ResolveCurrentCheckpointInventory(database_uuid,locked.ordered,checkpoint,maximum_retained_image_bytes);
    auto pair=std::move(current_inventory.pair);
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
    if(!current_inventory.selector_bound&&(current==z.roots.end()||!same(*current,checkpoint)||pair.checkpoint->root_set_generation!=z.root_set_generation))
      return fail(Error::binding_mismatch);
    const auto target=std::find_if(pair.checkpoint->roots.begin(),pair.checkpoint->roots.end(),
                                  [](const auto& ref){return ref.role==4;});
    const auto actual=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& ref){return ref.kind==3;});
    if(target==pair.checkpoint->roots.end()||(!current_inventory.selector_bound&&actual==z.roots.end()))return fail(Error::invalid_roots);
    const disk::FilespaceRootReference expected{3,target->page_type,target->page.filespace_uuid,
        target->page.page_number,target->page.page_generation,target->page.page_size_profile_uuid,target->object_uuid};
    if(!current_inventory.selector_bound&&!same(*actual,expected))return fail(Error::binding_mismatch);
    const auto map_file=std::lower_bound(locked.ordered.begin(),locked.ordered.end(),expected.filespace_uuid,[](const auto& f,const auto& id){return f.filespace_uuid<id;});
    if(map_file==locked.ordered.end()||map_file->filespace_uuid!=expected.filespace_uuid||map_file->page_size_profile_uuid!=expected.page_size_profile_uuid)return fail(Error::invalid_filespace);
    const disk::FilespaceBootstrapBinding map_binding{database_uuid,map_file->filespace_uuid,map_file->page_size_profile_uuid};
    auto allocation=current_inventory.selector_bound?
      page::ReadNativeAllocationChainAtRootFromOpenDevice(*map_file->device,map_binding,expected,maximum_retained_image_bytes-pair.retained_image_bytes):
      page::ReadNativeAllocationChainFromOpenDevice(*map_file->device,map_binding,maximum_retained_image_bytes-pair.retained_image_bytes);
    if(!allocation.ok()) {
      auto result=fail(Error::allocation_failure);result.allocation_error=allocation.error;return result;
    }
    const auto digest=hash::ComputeSha256Digest(allocation.pages.front().bytes);
    if(!digest.ok())return fail(Error::hash_failure);
    if(digest.digest!=target->sha256)return fail(Error::invalid_integrity);
    if(pair.retained_image_bytes>maximum_retained_image_bytes||allocation.retained_image_bytes>maximum_retained_image_bytes-pair.retained_image_bytes)
      return fail(Error::resource_exhausted);
    u64 retained=pair.retained_image_bytes+allocation.retained_image_bytes;
    std::optional<NativeManagementControlAuthority> operation_proof;
    const bool operation_map=std::any_of(allocation.pages.begin(),allocation.pages.end(),[](const auto& image){return !image.map->creator_operation_uuid.is_nil();});
    const bool operation_owned=operation_map||std::any_of(allocation.pages.begin(),allocation.pages.end(),[](const auto& image){
      return std::any_of(image.map->records.begin(),image.map->records.end(),
        [](const auto& record){return !record.creator_operation_uuid.is_nil();});
    });
    if(operation_owned){
      if(retained==maximum_retained_image_bytes)return fail(Error::resource_exhausted);
      auto proof=ReadNativeManagementControlAuthorityFromOpenDevices(database_uuid,locked.ordered,
        pair.checkpoint->header.filespace_uuid,maximum_retained_image_bytes-retained);
      if(!proof.ok()){using P=NativeManagementControlAuthorityError;
        return fail(proof.error==P::resource_exhausted?Error::resource_exhausted:
          proof.error==P::hash_failure?Error::hash_failure:proof.error==P::io_failure?Error::io_failure:
          proof.error==P::encrypted_requires_authority?Error::encrypted_requires_crypto_authority:
          proof.error==P::cluster_requires_authority?Error::cluster_requires_authority:
          operation_map?Error::allocation_creator_mismatch:Error::allocation_record_creator_mismatch);
      }
      const auto& h=pair.checkpoint->header;
      if(proof.selection->checkpoint!=disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid}||
        proof.selection->checkpoint_sha256!=pair.checkpoint_sha256)return fail(Error::binding_mismatch);
      if(proof.verified_image_bytes>maximum_retained_image_bytes-retained)return fail(Error::resource_exhausted);
      retained+=proof.verified_image_bytes;operation_proof=std::move(proof);
    }
    for(const auto& image:allocation.pages) {
      const auto& map=*image.map;
      if(map.creator_operation_uuid.is_nil()){
        const auto creator=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(map.creator_local_transaction_id));
        if(!creator.ok()||creator.entry.identity.transaction_uuid.value!=map.creator_transaction_uuid||
            (!(pair.checkpoint->flags&4)&&creator.entry.identity.scope!=mga::TransactionScope::local_node))
          return fail(Error::allocation_creator_mismatch);
        if(!mga::HasCommittedInventoryOutcome(creator.entry))return fail(Error::allocation_creator_not_committed);
      }else if(!operation_proof||!MatchesNativeManagementControlMap(*operation_proof,map))return fail(Error::allocation_creator_mismatch);
      for(const auto& record:map.records) {
        if(record.creator_operation_uuid.is_nil()){
          const auto original=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(record.creator_local_transaction_id));
          if(!original.ok()||original.entry.identity.transaction_uuid.value!=record.creator_transaction_uuid||
              (!(pair.checkpoint->flags&4)&&original.entry.identity.scope!=mga::TransactionScope::local_node))
            return fail(Error::allocation_record_creator_mismatch);
        }else if(!operation_proof||!MatchesNativeManagementControlAllocation(*operation_proof,map.header.filespace_uuid,record,map.states[record.page_number-map.first_page]))return fail(Error::allocation_record_creator_mismatch);
      }
    }
    NativeCheckpointAllocationResult result;result.error=Error::none;
    result.retained_image_bytes=retained;
    result.checkpoint_inventory=std::move(pair);result.allocation=std::move(allocation);return result;
  }catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}
   catch(const std::length_error&){return fail(Error::resource_exhausted);}
   catch(...){return fail(Error::io_failure);}
}

NativeCheckpointDirectoryResult VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u64 maximum_retained_image_bytes) noexcept {
  using namespace native_checkpoint;
  namespace mga=scratchbird::transaction::mga;
  namespace page=scratchbird::storage::page;
  const auto fail=[](Error error){NativeCheckpointDirectoryResult r;r.error=error;return r;};
  try {
    if(!V7(database_uuid)||devices.empty()||!maximum_retained_image_bytes)return fail(Error::invalid_reference);
    auto locked=LockFilespaces(devices);if(locked.error!=Error::none)return fail(locked.error);
    auto current_inventory=ResolveCurrentCheckpointInventory(database_uuid,locked.ordered,checkpoint,maximum_retained_image_bytes);
    auto pair=std::move(current_inventory.pair);
    if(!pair.ok()){auto failure=fail(pair.error);failure.checkpoint_inventory.error=pair.error;
      failure.checkpoint_inventory.inventory_error=pair.inventory_error;return failure;}
    const auto fs=std::lower_bound(locked.ordered.begin(),locked.ordered.end(),checkpoint.filespace_uuid,
      [](const auto& file,const auto& id){return file.filespace_uuid<id;});
    if(fs==locked.ordered.end()||fs->filespace_uuid!=checkpoint.filespace_uuid||
        fs->page_size_profile_uuid!=checkpoint.page_size_profile_uuid)return fail(Error::invalid_filespace);
    const disk::FilespaceBootstrapBinding binding{database_uuid,fs->filespace_uuid,fs->page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*fs->device,&binding);
    if(!zero.ok()){
      if(zero.error==disk::FilespacePageZeroError::resource_exhausted)return fail(Error::resource_exhausted);
      if(zero.error==disk::FilespacePageZeroError::hash_provider_failure)return fail(Error::hash_failure);
      if(zero.error==disk::FilespacePageZeroError::io_failure)return fail(Error::io_failure);
      return fail(Error::invalid_filespace);
    }
    const auto& z=*zero.record;
    const auto same=[](const auto& a,const auto& b){return a.kind==b.kind&&a.page_type==b.page_type&&
      a.filespace_uuid==b.filespace_uuid&&a.page_number==b.page_number&&a.page_generation==b.page_generation&&
      a.page_size_profile_uuid==b.page_size_profile_uuid&&a.object_uuid==b.object_uuid;};
    const auto current=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==9;});
    if(!current_inventory.selector_bound&&(current==z.roots.end()||!same(*current,checkpoint)||pair.checkpoint->root_set_generation!=z.root_set_generation))
      return fail(Error::binding_mismatch);
    const auto target=std::find_if(pair.checkpoint->roots.begin(),pair.checkpoint->roots.end(),[](const auto& r){return r.role==3;});
    const auto actual=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==5;});
    if(target==pair.checkpoint->roots.end()||(!current_inventory.selector_bound&&actual==z.roots.end()))return fail(Error::invalid_roots);
    const disk::FilespaceRootReference expected{5,target->page_type,target->page.filespace_uuid,target->page.page_number,
      target->page.page_generation,target->page.page_size_profile_uuid,target->object_uuid};
    if(!current_inventory.selector_bound&&!same(*actual,expected))return fail(Error::binding_mismatch);
    auto directory=page::ReadNativeFilespaceDirectoryFromOpenDevices(database_uuid,locked.ordered,expected,
      maximum_retained_image_bytes-pair.retained_image_bytes);
    if(!directory.ok()){auto failure=fail(Error::directory_failure);failure.directory_error=directory.error;return failure;}
    const auto digest=hash::ComputeSha256Digest(directory.pages.front().bytes);
    if(!digest.ok())return fail(Error::hash_failure);
    if(digest.digest!=target->sha256)return fail(Error::invalid_integrity);
    if(pair.retained_image_bytes>maximum_retained_image_bytes||directory.retained_image_bytes>maximum_retained_image_bytes-pair.retained_image_bytes)
      return fail(Error::resource_exhausted);
    u64 retained=pair.retained_image_bytes+directory.retained_image_bytes;
    const auto& root=*directory.pages.front().directory;
    if(root.creator_operation_uuid.is_nil()){
      const auto creator=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(root.creator_local_transaction_id));
      if(!creator.ok()||creator.entry.identity.transaction_uuid.value!=root.creator_transaction_uuid||
          (!(pair.checkpoint->flags&4)&&creator.entry.identity.scope!=mga::TransactionScope::local_node))
        return fail(Error::directory_creator_mismatch);
      if(!mga::HasCommittedInventoryOutcome(creator.entry))return fail(Error::directory_creator_not_committed);
    }else{
      if(retained==maximum_retained_image_bytes)return fail(Error::resource_exhausted);
      const auto proof=ReadNativeManagementControlAuthorityFromOpenDevices(database_uuid,locked.ordered,
        pair.checkpoint->header.filespace_uuid,maximum_retained_image_bytes-retained);
      if(!proof.ok()){using P=NativeManagementControlAuthorityError;
        return fail(proof.error==P::resource_exhausted?Error::resource_exhausted:
          proof.error==P::hash_failure?Error::hash_failure:proof.error==P::io_failure?Error::io_failure:
          proof.error==P::encrypted_requires_authority?Error::encrypted_requires_crypto_authority:
          proof.error==P::cluster_requires_authority?Error::cluster_requires_authority:Error::directory_creator_mismatch);
      }
      const auto& h=pair.checkpoint->header;
      if(proof.selection->checkpoint!=disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid}||
         proof.selection->checkpoint_sha256!=pair.checkpoint_sha256)return fail(Error::binding_mismatch);
      if(proof.verified_image_bytes>maximum_retained_image_bytes-retained)return fail(Error::resource_exhausted);
      retained+=proof.verified_image_bytes;
      if(!MatchesNativeManagementPublishedDirectory(proof,directory,digest.digest))return fail(Error::directory_creator_mismatch);
    }
    NativeCheckpointDirectoryResult result;result.error=Error::none;
    result.retained_image_bytes=retained;
    result.checkpoint_inventory=std::move(pair);result.directory=std::move(directory);return result;
  }catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}
   catch(const std::length_error&){return fail(Error::resource_exhausted);}
   catch(...){return fail(Error::io_failure);}
}

static NativeCheckpointHistoryResult ReadNativeCheckpointHistoryFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& head,
    const scratchbird::storage::disk::FilespaceRootReference& terminal,
    u64 maximum_retained_image_bytes,
    std::optional<NativeCheckpointInventoryResult> admitted_head) noexcept {
  using namespace native_checkpoint;
  const auto fail=[](Error error){NativeCheckpointHistoryResult r;r.error=error;return r;};
  const auto page_ref=[](const disk::FilespaceRootReference& r){return disk::NativePageReference{r.filespace_uuid,r.page_number,r.page_generation,r.page_size_profile_uuid};};
  try {
    if(!V7(database_uuid)||!V7(head.object_uuid)||head.object_uuid!=terminal.object_uuid
      ||head.kind!=9||terminal.kind!=9||head.page_type!=0x300||terminal.page_type!=0x300
      ||!RefValid(page_ref(head))||!RefValid(page_ref(terminal))||!maximum_retained_image_bytes)return fail(Error::invalid_reference);
    if(admitted_head){
      if(!admitted_head->ok()||!admitted_head->checkpoint)return fail(Error::binding_mismatch);
      const auto& cp=*admitted_head->checkpoint;
      const disk::NativePageReference actual{cp.header.filespace_uuid,cp.header.page_number,cp.header.page_generation,cp.header.page_size_profile_uuid};
      if(cp.header.database_uuid!=database_uuid||actual!=page_ref(head)||cp.object_uuid!=head.object_uuid)
        return fail(Error::binding_mismatch);
    }
    auto locked=LockFilespaces(devices);if(locked.error!=Error::none)return fail(locked.error);
    NativeCheckpointHistoryResult result;auto next=head;
    std::set<std::pair<Uuid,u64>> slots;std::set<Uuid> page_ids;
    for(;;) {
      if(result.retained_image_bytes>=maximum_retained_image_bytes)return fail(Error::resource_exhausted);
      if(!slots.emplace(next.filespace_uuid,next.page_number).second)return fail(Error::history_mismatch);
      // Only current-root consumers can supply this already admitted head.
      // Its charge includes selector and control proof work, not just the
      // checkpoint/inventory images retained by the raw-root history reader.
      auto pair=admitted_head?std::move(*admitted_head):VerifyNativeCheckpointInventoryFromOpenDevices(database_uuid,locked.ordered,next,
        maximum_retained_image_bytes-result.retained_image_bytes);
      admitted_head.reset();
      if(!pair.ok()){auto error=fail(pair.error);error.inventory_error=pair.inventory_error;return error;}
      if(pair.retained_image_bytes>maximum_retained_image_bytes-result.retained_image_bytes)return fail(Error::resource_exhausted);
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

NativeCheckpointHistoryResult VerifyNativeCheckpointHistoryFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& head,
    const scratchbird::storage::disk::FilespaceRootReference& terminal,
    u64 maximum_retained_image_bytes) noexcept {
  return ReadNativeCheckpointHistoryFromOpenDevices(database_uuid,devices,head,terminal,maximum_retained_image_bytes,{});
}

NativeCheckpointSystemStateResult VerifyCurrentNativeCheckpointSystemStateFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,u64 budget) noexcept {
  using namespace native_checkpoint;namespace mga=scratchbird::transaction::mga;
  const auto fail=[](Error e){NativeCheckpointSystemStateResult r;r.error=e;return r;};
  const auto same=[](const auto& a,const auto& b){return a.kind==b.kind&&a.page_type==b.page_type&&a.object_uuid==b.object_uuid&&
    a.filespace_uuid==b.filespace_uuid&&a.page_size_profile_uuid==b.page_size_profile_uuid&&a.page_number==b.page_number&&a.page_generation==b.page_generation;};
  try{
    if(!V7(database_uuid)||devices.empty()||!budget)return fail(Error::invalid_reference);
    auto locked=LockFilespaces(devices);if(locked.error!=Error::none)return fail(locked.error);
    auto current_inventory=ResolveCurrentCheckpointInventory(database_uuid,locked.ordered,checkpoint,budget);
    auto pair=std::move(current_inventory.pair);
    if(!pair.ok()){auto r=fail(pair.error);r.checkpoints.error=pair.error;r.checkpoints.inventory_error=pair.inventory_error;return r;}
    const auto primary=std::lower_bound(locked.ordered.begin(),locked.ordered.end(),checkpoint.filespace_uuid,[](const auto& f,const auto& id){return f.filespace_uuid<id;});
    if(primary==locked.ordered.end()||primary->filespace_uuid!=checkpoint.filespace_uuid||primary->page_size_profile_uuid!=checkpoint.page_size_profile_uuid)return fail(Error::invalid_filespace);
    const disk::FilespaceBootstrapBinding binding{database_uuid,primary->filespace_uuid,primary->page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*primary->device,&binding);
    if(!zero.ok())return fail(zero.error==disk::FilespacePageZeroError::resource_exhausted?Error::resource_exhausted:
      zero.error==disk::FilespacePageZeroError::hash_provider_failure?Error::hash_failure:zero.error==disk::FilespacePageZeroError::io_failure?Error::io_failure:Error::invalid_filespace);
    const auto& z=*zero.record;const auto current=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==9;});
    if(!current_inventory.selector_bound&&(current==z.roots.end()||!same(*current,checkpoint)||pair.checkpoint->root_set_generation!=z.root_set_generation))return fail(Error::binding_mismatch);
    const auto actual=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==1;});
    const auto target=std::find_if(pair.checkpoint->roots.begin(),pair.checkpoint->roots.end(),[](const auto& r){return r.role==8;});
    if((!current_inventory.selector_bound&&actual==z.roots.end())||target==pair.checkpoint->roots.end())return fail(Error::invalid_roots);
    const disk::FilespaceRootReference expected{1,target->page_type,target->page.filespace_uuid,target->page.page_number,target->page.page_generation,target->page.page_size_profile_uuid,target->object_uuid};
    if(!current_inventory.selector_bound&&!same(*actual,expected))return fail(Error::binding_mismatch);
    const auto file=std::lower_bound(locked.ordered.begin(),locked.ordered.end(),expected.filespace_uuid,[](const auto& f,const auto& id){return f.filespace_uuid<id;});
    if(file==locked.ordered.end()||file->filespace_uuid!=expected.filespace_uuid||file->page_size_profile_uuid!=expected.page_size_profile_uuid)return fail(Error::invalid_filespace);
    const auto* profile=disk::FindCanonicalFilespacePageProfile(expected.page_size_profile_uuid);
    if(!profile||profile->page_size_bytes>budget-pair.retained_image_bytes)return fail(Error::resource_exhausted);
    auto state=ReadNativeSystemStateFromOpenDevice(*file->device,database_uuid,expected);
    if(!state.ok()){auto r=fail(Error::system_state_failure);r.system_error=state.error;return r;}
    const auto digest=hash::ComputeSha256Digest(state.bytes);if(!digest.ok())return fail(Error::hash_failure);
    if(digest.digest!=target->sha256)return fail(Error::invalid_integrity);
    const auto& s=*state.state;const bool cluster=pair.checkpoint->flags&4;
    if(bool(s.flags&NativeSystemFlag::cluster)!=cluster||bool(z.bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required)!=cluster||
      bool(s.flags&NativeSystemFlag::clean)!=bool(pair.checkpoint->flags&1))return fail(Error::binding_mismatch);
    const auto creator_check=[&](const Uuid& id,u64 local,Error mismatch,Error uncommitted){
      const auto entry=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(local));
      if(!entry.ok()||entry.entry.identity.transaction_uuid.value!=id||(!cluster&&entry.entry.identity.scope!=mga::TransactionScope::local_node))return mismatch;
      return mga::HasCommittedInventoryOutcome(entry.entry)?Error::none:uncommitted;};
    const auto creator=creator_check(s.creator_transaction_uuid,s.creator_local_transaction_id,Error::system_state_creator_mismatch,Error::system_state_creator_not_committed);
    if(creator!=Error::none)return fail(creator);
    if(s.clean_local_transaction_id){const auto clean=creator_check(s.clean_transaction_uuid,s.clean_local_transaction_id,Error::system_state_clean_mismatch,Error::system_state_clean_not_committed);if(clean!=Error::none)return fail(clean);}
    NativeCheckpointHistoryResult history;
    if(s.checkpoint_generation){const auto& ref=*s.checkpoint;
      const disk::FilespaceRootReference observed{9,0x300,ref.filespace_uuid,ref.page_number,ref.page_generation,ref.page_size_profile_uuid,s.checkpoint_object_uuid};
      if(same(observed,checkpoint)){if(s.checkpoint_generation!=pair.checkpoint->checkpoint_generation)return fail(Error::system_state_observation_mismatch);}
      else {
        if(s.checkpoint_object_uuid!=pair.checkpoint->object_uuid||s.checkpoint_generation>=pair.checkpoint->checkpoint_generation)return fail(Error::system_state_observation_mismatch);
        history=ReadNativeCheckpointHistoryFromOpenDevices(database_uuid,locked.ordered,checkpoint,observed,budget-state.bytes.size(),std::move(pair));
        if(!history.ok()){auto r=fail(history.error);r.checkpoints.error=history.error;r.checkpoints.inventory_error=history.inventory_error;return r;}
        if(history.checkpoints.back().checkpoint->checkpoint_generation!=s.checkpoint_generation)return fail(Error::system_state_observation_mismatch);
      }
    }
    if(history.checkpoints.empty()){history.error=Error::none;history.retained_image_bytes=pair.retained_image_bytes;history.checkpoints.push_back(std::move(pair));}
    NativeCheckpointSystemStateResult result;result.error=Error::none;result.retained_image_bytes=history.retained_image_bytes+state.bytes.size();
    result.checkpoints=std::move(history);result.system_state=std::move(state);return result;
  }catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}catch(const std::length_error&){return fail(Error::resource_exhausted);}catch(...){return fail(Error::io_failure);}
}

NativeCheckpointHorizonResult VerifyCurrentNativeCheckpointHorizonFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,u64 budget) noexcept {
  using namespace native_checkpoint;namespace mga=scratchbird::transaction::mga;namespace page=scratchbird::storage::page;
  const auto fail=[](Error e){NativeCheckpointHorizonResult r;r.error=e;return r;};
  const auto ref=[](const auto& r){return disk::NativePageReference{r.filespace_uuid,r.page_number,r.page_generation,r.page_size_profile_uuid};};
  try{
    if(!V7(database_uuid)||devices.empty()||!budget)return fail(Error::invalid_reference);
    auto locked=LockFilespaces(devices);if(locked.error!=Error::none)return fail(locked.error);
    auto current_inventory=ResolveCurrentCheckpointInventory(database_uuid,locked.ordered,checkpoint,budget);
    auto pair=std::move(current_inventory.pair);
    if(!pair.ok()){auto r=fail(pair.error);r.checkpoints.error=pair.error;r.checkpoints.inventory_error=pair.inventory_error;return r;}
    const auto primary=std::lower_bound(locked.ordered.begin(),locked.ordered.end(),checkpoint.filespace_uuid,[](const auto& f,const auto& id){return f.filespace_uuid<id;});
    if(primary==locked.ordered.end()||primary->filespace_uuid!=checkpoint.filespace_uuid||primary->page_size_profile_uuid!=checkpoint.page_size_profile_uuid)return fail(Error::invalid_filespace);
    const disk::FilespaceBootstrapBinding binding{database_uuid,primary->filespace_uuid,primary->page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*primary->device,&binding);
    if(!zero.ok())return fail(zero.error==disk::FilespacePageZeroError::resource_exhausted?Error::resource_exhausted:
      zero.error==disk::FilespacePageZeroError::hash_provider_failure?Error::hash_failure:zero.error==disk::FilespacePageZeroError::io_failure?Error::io_failure:Error::invalid_filespace);
    const auto current=std::find_if(zero.record->roots.begin(),zero.record->roots.end(),[](const auto& r){return r.kind==9;});
    if(!current_inventory.selector_bound&&(current==zero.record->roots.end()||current->page_type!=checkpoint.page_type||current->object_uuid!=checkpoint.object_uuid||
        ref(*current)!=ref(checkpoint)||zero.record->root_set_generation!=pair.checkpoint->root_set_generation))return fail(Error::binding_mismatch);
    const auto target=std::find_if(pair.checkpoint->roots.begin(),pair.checkpoint->roots.end(),[](const auto& r){return r.role==2;});
    const auto retention=std::find_if(pair.checkpoint->roots.begin(),pair.checkpoint->roots.end(),[](const auto& r){return r.role==10;});
    if(target==pair.checkpoint->roots.end()||retention==pair.checkpoint->roots.end())return fail(Error::invalid_roots);
    auto horizons=page::ReadNativeHorizonRootFromOpenDevices(database_uuid,locked.ordered,target->object_uuid,target->page,budget-pair.retained_image_bytes);
    if(!horizons.ok()){auto r=fail(Error::horizon_failure);r.horizon_error=horizons.error;return r;}
    const auto digest=hash::ComputeSha256Digest(horizons.pages.front().bytes);if(!digest.ok())return fail(Error::hash_failure);
    if(digest.digest!=target->sha256)return fail(Error::invalid_integrity);
    const auto& h=*horizons.pages.front().root;const bool cluster=pair.checkpoint->flags&4;
    if(bool(h.flags&1)!=cluster||bool(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required)!=cluster)return fail(Error::binding_mismatch);
    const auto creator=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(h.creator_local_transaction_id));
    if(!creator.ok()||creator.entry.identity.transaction_uuid.value!=h.creator_transaction_uuid||(!cluster&&creator.entry.identity.scope!=mga::TransactionScope::local_node))return fail(Error::horizon_creator_mismatch);
    if(!mga::HasCommittedInventoryOutcome(creator.entry))return fail(Error::horizon_creator_not_committed);
    if(h.retention!=retention->page||h.retention_object_uuid!=retention->object_uuid||h.retention_sha256!=retention->sha256)return fail(Error::horizon_retention_mismatch);
    std::optional<disk::FilespaceRootReference> oldest;u64 oldest_generation=pair.checkpoint->checkpoint_generation;
    for(const auto& image:horizons.pages)for(const auto& record:image.root->records){
      if(record.local_boundary>pair.inventory.next_local_transaction_id)return fail(Error::horizon_boundary_mismatch);
      if(!record.checkpoint)continue;
      if(record.checkpoint_object_uuid!=pair.checkpoint->object_uuid||record.checkpoint_generation>pair.checkpoint->checkpoint_generation)return fail(Error::horizon_observation_mismatch);
      if(record.checkpoint_generation<oldest_generation){const auto& r=*record.checkpoint;oldest_generation=record.checkpoint_generation;oldest=disk::FilespaceRootReference{9,0x300,r.filespace_uuid,r.page_number,r.page_generation,r.page_size_profile_uuid,record.checkpoint_object_uuid};}
    }
    NativeCheckpointHistoryResult history;
    if(oldest){history=ReadNativeCheckpointHistoryFromOpenDevices(database_uuid,locked.ordered,checkpoint,*oldest,budget-horizons.retained_image_bytes,std::move(pair));
      if(!history.ok()){auto r=fail(history.error);r.checkpoints.error=history.error;r.checkpoints.inventory_error=history.inventory_error;return r;}}
    else{history.error=Error::none;history.retained_image_bytes=pair.retained_image_bytes;history.checkpoints.push_back(std::move(pair));}
    std::map<u64,const NativeCheckpointRoot*> observed;std::set<Uuid> checkpoint_ids;
    for(const auto& value:history.checkpoints){observed.emplace(value.checkpoint->checkpoint_generation,&*value.checkpoint);checkpoint_ids.insert(value.checkpoint->header.page_uuid);}
    for(const auto& image:horizons.pages){if(checkpoint_ids.contains(image.root->header.page_uuid))return fail(Error::binding_mismatch);
      for(const auto& record:image.root->records)if(record.checkpoint){const auto at=observed.find(record.checkpoint_generation);
        if(at==observed.end()||at->second->object_uuid!=record.checkpoint_object_uuid||ref(at->second->header)!=*record.checkpoint)return fail(Error::horizon_observation_mismatch);}}
    const u64 used=history.retained_image_bytes+horizons.retained_image_bytes;
    auto pins=page::ReadNativeRetentionRootFromOpenDevices(database_uuid,locked.ordered,h.retention_object_uuid,h.retention,budget-used);
    if(!pins.ok()){auto r=fail(Error::retention_failure);r.retention_error=pins.error;return r;}
    const auto pin_digest=hash::ComputeSha256Digest(pins.images.front().bytes);if(!pin_digest.ok())return fail(Error::hash_failure);
    if(pin_digest.digest!=h.retention_sha256)return fail(Error::invalid_integrity);
    const auto& root=*pins.images.front().page;const auto& inventory=history.checkpoints.front().inventory;
    if(bool(root.flags&1)!=cluster)return fail(Error::binding_mismatch);
    const auto pin_creator=mga::LookupLocalTransaction(inventory,mga::MakeLocalTransactionId(root.creator_local_transaction_id));
    if(!pin_creator.ok()||pin_creator.entry.identity.transaction_uuid.value!=root.creator_transaction_uuid||
        (!cluster&&pin_creator.entry.identity.scope!=mga::TransactionScope::local_node))return fail(Error::retention_creator_mismatch);
    if(!mga::HasCommittedInventoryOutcome(pin_creator.entry))return fail(Error::retention_creator_not_committed);
    std::set<Uuid> page_ids=checkpoint_ids;for(const auto& image:horizons.pages)page_ids.insert(image.root->header.page_uuid);
    std::map<Uuid,const page::NativeRetentionPin*> pin_index;
    for(const auto& image:pins.images){if(!page_ids.insert(image.page->header.page_uuid).second)return fail(Error::binding_mismatch);
      for(const auto& pin:image.page->records)pin_index.emplace(pin.pin_uuid,&pin);}
    for(const auto& image:horizons.pages)for(const auto& record:image.root->records){
      if(record.pin_uuid.is_nil())continue;
      const auto pin=pin_index.find(record.pin_uuid);if(pin==pin_index.end())return fail(Error::horizon_pin_missing);
      if(pin->second->timeline_uuid!=record.timeline_uuid)return fail(Error::horizon_pin_lineage_mismatch);
    }
    NativeCheckpointHorizonResult result;result.error=Error::none;result.retained_image_bytes=used+pins.retained_image_bytes;
    result.checkpoints=std::move(history);result.horizons=std::move(horizons);result.retention=std::move(pins);return result;
  }catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}catch(const std::length_error&){return fail(Error::resource_exhausted);}catch(...){return fail(Error::io_failure);}
}

}  // namespace scratchbird::storage::database
