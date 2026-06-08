// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// DPC_SECONDARY_INDEX_DELTA_LEDGER
#include "secondary_index_delta_ledger.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

constexpr std::array<byte, 8> kLedgerMagic = {
    'S', 'B', 'D', 'L', 'D', 'G', '1', '\0'};
constexpr u64 kFnvOffset = 1469598103934665603ull;
constexpr u64 kFnvPrime = 1099511628211ull;

Status LedgerOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status LedgerErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

Status LedgerOverflowStatus() {
  return {StatusCode::memory_limit_exceeded, Severity::error, Subsystem::engine};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool GeneratedDurableUuid(const TypedUuid& value, UuidKind expected_kind) {
  return value.kind == expected_kind && value.valid() &&
         scratchbird::core::uuid::IsDurableEngineIdentityKind(value.kind) &&
         scratchbird::core::uuid::IsEngineIdentityUuid(value.value);
}

bool DeltaKindValid(SecondaryIndexDeltaKind kind) {
  switch (kind) {
    case SecondaryIndexDeltaKind::insert:
    case SecondaryIndexDeltaKind::delete_row:
    case SecondaryIndexDeltaKind::update_before:
    case SecondaryIndexDeltaKind::update_after:
      return true;
  }
  return false;
}

bool CommitStateValid(SecondaryIndexDeltaLedgerCommitState state) {
  switch (state) {
    case SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted:
    case SecondaryIndexDeltaLedgerCommitState::committed_premerge:
    case SecondaryIndexDeltaLedgerCommitState::merged_cleaned:
    case SecondaryIndexDeltaLedgerCommitState::repair_rebuild_required:
    case SecondaryIndexDeltaLedgerCommitState::refused:
      return true;
  }
  return false;
}

void AppendByte(std::vector<byte>* out, byte value) {
  out->push_back(value);
}

void AppendBytes(std::vector<byte>* out, const byte* data, std::size_t size) {
  out->insert(out->end(), data, data + size);
}

void AppendU32(std::vector<byte>* out, u32 value) {
  const u32 little = scratchbird::core::platform::HostToLittle32(value);
  const auto* data = reinterpret_cast<const byte*>(&little);
  AppendBytes(out, data, sizeof(little));
}

void AppendU64(std::vector<byte>* out, u64 value) {
  const u64 little = scratchbird::core::platform::HostToLittle64(value);
  const auto* data = reinterpret_cast<const byte*>(&little);
  AppendBytes(out, data, sizeof(little));
}

void AppendString(std::vector<byte>* out, const std::string& value) {
  AppendU32(out, static_cast<u32>(value.size()));
  if (!value.empty()) {
    AppendBytes(out, reinterpret_cast<const byte*>(value.data()), value.size());
  }
}

void AppendTypedUuid(std::vector<byte>* out, const TypedUuid& value) {
  AppendByte(out, static_cast<byte>(value.kind));
  AppendBytes(out, value.value.bytes.data(), value.value.bytes.size());
}

u64 StableBytesFingerprint(const std::vector<byte>& bytes) {
  u64 hash = kFnvOffset;
  for (byte value : bytes) {
    hash ^= static_cast<u64>(value);
    hash *= kFnvPrime;
  }
  return hash;
}

struct ByteReader {
  const std::vector<byte>& bytes;
  std::size_t offset = 0;

  bool ReadByte(byte* value) {
    if (offset + 1 > bytes.size()) {
      return false;
    }
    *value = bytes[offset++];
    return true;
  }

  bool ReadBytes(byte* target, std::size_t size) {
    if (offset + size > bytes.size()) {
      return false;
    }
    std::copy_n(bytes.data() + offset, size, target);
    offset += size;
    return true;
  }

  bool ReadU32(u32* value) {
    if (offset + sizeof(u32) > bytes.size()) {
      return false;
    }
    *value = scratchbird::core::platform::LoadLittle32(bytes.data() + offset);
    offset += sizeof(u32);
    return true;
  }

  bool ReadU64(u64* value) {
    if (offset + sizeof(u64) > bytes.size()) {
      return false;
    }
    *value = scratchbird::core::platform::LoadLittle64(bytes.data() + offset);
    offset += sizeof(u64);
    return true;
  }

  bool ReadString(std::string* value) {
    u32 size = 0;
    if (!ReadU32(&size)) {
      return false;
    }
    if (offset + size > bytes.size()) {
      return false;
    }
    value->assign(reinterpret_cast<const char*>(bytes.data() + offset), size);
    offset += size;
    return true;
  }

  bool ReadTypedUuid(TypedUuid* value) {
    byte kind = 0;
    if (!ReadByte(&kind)) {
      return false;
    }
    value->kind = static_cast<UuidKind>(kind);
    return ReadBytes(value->value.bytes.data(), value->value.bytes.size());
  }
};

SecondaryIndexDeltaLedgerEncodeResult RefuseEncode(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail) {
  SecondaryIndexDeltaLedgerEncodeResult result;
  result.status = status;
  result.diagnostic = MakeSecondaryIndexDeltaLedgerDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key), std::move(detail));
  return result;
}

SecondaryIndexDeltaLedgerDecodeResult RefuseDecode(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail) {
  SecondaryIndexDeltaLedgerDecodeResult result;
  result.status = status;
  result.diagnostic = MakeSecondaryIndexDeltaLedgerDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key), std::move(detail));
  return result;
}

SecondaryIndexDeltaLedgerAppendResult RefuseAppend(Status status,
                                                   const PersistentSecondaryIndexDeltaLedger* ledger,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail) {
  SecondaryIndexDeltaLedgerAppendResult result;
  result.status = status;
  result.appended = false;
  result.record_count = ledger == nullptr ? 0 : ledger->records.size();
  result.encoded_bytes = ledger == nullptr ? 0 : ledger->encoded_bytes;
  result.diagnostic = MakeSecondaryIndexDeltaLedgerDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key), std::move(detail));
  return result;
}

DiagnosticRecord ValidateRecord(const SecondaryIndexDeltaLedgerRecord& record) {
  const Status status = LedgerErrorStatus();
  if (!GeneratedDurableUuid(record.delta.delta_id, UuidKind::object) ||
      !GeneratedDurableUuid(record.delta.index_uuid, UuidKind::object) ||
      !GeneratedDurableUuid(record.delta.table_uuid, UuidKind::object) ||
      !GeneratedDurableUuid(record.delta.row_uuid, UuidKind::row) ||
      !GeneratedDurableUuid(record.delta.version_uuid, UuidKind::row) ||
      !GeneratedDurableUuid(record.delta.transaction_uuid, UuidKind::transaction)) {
    return MakeSecondaryIndexDeltaLedgerDiagnostic(
        status,
        "secondary_index_delta_ledger_invalid_identity",
        "core.index.secondary_delta_ledger.invalid_identity",
        "ledger records require generated durable typed UUID identities");
  }
  if (record.delta.local_transaction_id == 0) {
    return MakeSecondaryIndexDeltaLedgerDiagnostic(
        status,
        "secondary_index_delta_ledger_invalid_transaction",
        "core.index.secondary_delta_ledger.invalid_transaction",
        "local transaction id must be nonzero");
  }
  if (!DeltaKindValid(record.delta.delta_kind)) {
    return MakeSecondaryIndexDeltaLedgerDiagnostic(
        status,
        "secondary_index_delta_ledger_malformed_operation",
        "core.index.secondary_delta_ledger.malformed_operation",
        "operation kind is outside the persisted ledger contract");
  }
  if (record.delta.key_payload.empty() || record.delta.cleanup_horizon_token.empty() ||
      record.source_evidence_reference.empty()) {
    return MakeSecondaryIndexDeltaLedgerDiagnostic(
        status,
        "secondary_index_delta_ledger_missing_required_payload",
        "core.index.secondary_delta_ledger.missing_required_payload",
        "key payload cleanup horizon and source evidence reference are required");
  }
  if (!CommitStateValid(record.commit_state)) {
    return MakeSecondaryIndexDeltaLedgerDiagnostic(
        status,
        "secondary_index_delta_ledger_malformed_commit_state",
        "core.index.secondary_delta_ledger.malformed_commit_state",
        "commit state is outside the persisted ledger contract");
  }
  if (record.commit_state == SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted &&
      record.delta.committed) {
    return MakeSecondaryIndexDeltaLedgerDiagnostic(
        status,
        "secondary_index_delta_ledger_commit_state_mismatch",
        "core.index.secondary_delta_ledger.commit_state_mismatch",
        "precommit records cannot carry committed transaction metadata");
  }
  if (record.commit_state != SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted &&
      !record.delta.committed) {
    return MakeSecondaryIndexDeltaLedgerDiagnostic(
        status,
        "secondary_index_delta_ledger_commit_state_mismatch",
        "core.index.secondary_delta_ledger.commit_state_mismatch",
        "postcommit records require committed transaction metadata");
  }
  return MakeSecondaryIndexDeltaLedgerDiagnostic(
      LedgerOkStatus(), "ok", "core.index.secondary_delta_ledger.record_validated");
}

void EncodeRecord(std::vector<byte>* out,
                  const SecondaryIndexDeltaLedgerRecord& record) {
  AppendTypedUuid(out, record.delta.delta_id);
  AppendTypedUuid(out, record.delta.index_uuid);
  AppendTypedUuid(out, record.delta.table_uuid);
  AppendTypedUuid(out, record.delta.row_uuid);
  AppendTypedUuid(out, record.delta.version_uuid);
  AppendTypedUuid(out, record.delta.transaction_uuid);
  AppendU64(out, record.delta.local_transaction_id);
  AppendU32(out, static_cast<u32>(record.delta.delta_kind));
  AppendString(out, record.delta.key_payload);
  AppendString(out, record.delta.cleanup_horizon_token);
  AppendU32(out, static_cast<u32>(record.commit_state));
  AppendByte(out, record.delta.committed ? 1 : 0);
  AppendString(out, record.source_evidence_reference);
}

bool DecodeRecord(ByteReader* reader, SecondaryIndexDeltaLedgerRecord* record) {
  u32 delta_kind = 0;
  u32 commit_state = 0;
  byte committed = 0;
  if (!reader->ReadTypedUuid(&record->delta.delta_id) ||
      !reader->ReadTypedUuid(&record->delta.index_uuid) ||
      !reader->ReadTypedUuid(&record->delta.table_uuid) ||
      !reader->ReadTypedUuid(&record->delta.row_uuid) ||
      !reader->ReadTypedUuid(&record->delta.version_uuid) ||
      !reader->ReadTypedUuid(&record->delta.transaction_uuid) ||
      !reader->ReadU64(&record->delta.local_transaction_id) ||
      !reader->ReadU32(&delta_kind) ||
      !reader->ReadString(&record->delta.key_payload) ||
      !reader->ReadString(&record->delta.cleanup_horizon_token) ||
      !reader->ReadU32(&commit_state) ||
      !reader->ReadByte(&committed) ||
      !reader->ReadString(&record->source_evidence_reference)) {
    return false;
  }
  record->delta.delta_kind = static_cast<SecondaryIndexDeltaKind>(delta_kind);
  record->commit_state = static_cast<SecondaryIndexDeltaLedgerCommitState>(commit_state);
  record->delta.committed = committed != 0;
  return committed == 0 || committed == 1;
}

bool LedgerShapeCompatible(const PersistentSecondaryIndexDeltaLedger& ledger) {
  return ledger.format_major == kSecondaryIndexDeltaLedgerFormatMajor &&
         ledger.format_minor == kSecondaryIndexDeltaLedgerFormatMinor &&
         ledger.artifact_kind == kSecondaryIndexDeltaLedgerArtifactKind;
}

}  // namespace

const char* SecondaryIndexDeltaLedgerCommitStateName(
    SecondaryIndexDeltaLedgerCommitState state) {
  switch (state) {
    case SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted:
      return "precommit_uncommitted";
    case SecondaryIndexDeltaLedgerCommitState::committed_premerge:
      return "committed_premerge";
    case SecondaryIndexDeltaLedgerCommitState::merged_cleaned:
      return "merged_cleaned";
    case SecondaryIndexDeltaLedgerCommitState::repair_rebuild_required:
      return "repair_rebuild_required";
    case SecondaryIndexDeltaLedgerCommitState::refused:
      return "refused";
  }
  return "unknown";
}

const char* SecondaryIndexDeltaLedgerRecoveryClassName(
    SecondaryIndexDeltaLedgerRecoveryClass recovery_class) {
  switch (recovery_class) {
    case SecondaryIndexDeltaLedgerRecoveryClass::empty_clean:
      return "empty_clean";
    case SecondaryIndexDeltaLedgerRecoveryClass::has_uncommitted_precommit_delta:
      return "has_uncommitted_precommit_delta";
    case SecondaryIndexDeltaLedgerRecoveryClass::committed_premerge_requires_overlay_merge:
      return "committed_premerge_requires_overlay_merge";
    case SecondaryIndexDeltaLedgerRecoveryClass::clean_after_merge:
      return "clean_after_merge";
    case SecondaryIndexDeltaLedgerRecoveryClass::repair_rebuild_required:
      return "repair_rebuild_required";
    case SecondaryIndexDeltaLedgerRecoveryClass::refused:
      return "refused";
    case SecondaryIndexDeltaLedgerRecoveryClass::corrupt_incompatible_fail_closed:
      return "corrupt_incompatible_fail_closed";
  }
  return "unknown";
}

const char* SecondaryIndexDeltaLedgerRecoveryActionName(
    SecondaryIndexDeltaLedgerRecoveryAction action) {
  switch (action) {
    case SecondaryIndexDeltaLedgerRecoveryAction::no_action:
      return "no_action";
    case SecondaryIndexDeltaLedgerRecoveryAction::retain_for_mga_transaction_finality:
      return "retain_for_mga_transaction_finality";
    case SecondaryIndexDeltaLedgerRecoveryAction::apply_overlay_then_merge:
      return "apply_overlay_then_merge";
    case SecondaryIndexDeltaLedgerRecoveryAction::rebuild_from_authoritative_base:
      return "rebuild_from_authoritative_base";
    case SecondaryIndexDeltaLedgerRecoveryAction::refuse_open:
      return "refuse_open";
    case SecondaryIndexDeltaLedgerRecoveryAction::fail_closed:
      return "fail_closed";
  }
  return "unknown";
}

bool SecondaryIndexDeltaLedgerRecordEquals(
    const SecondaryIndexDeltaLedgerRecord& left,
    const SecondaryIndexDeltaLedgerRecord& right) {
  return SameUuid(left.delta.delta_id, right.delta.delta_id) &&
         SameUuid(left.delta.index_uuid, right.delta.index_uuid) &&
         SameUuid(left.delta.table_uuid, right.delta.table_uuid) &&
         SameUuid(left.delta.row_uuid, right.delta.row_uuid) &&
         SameUuid(left.delta.version_uuid, right.delta.version_uuid) &&
         SameUuid(left.delta.transaction_uuid, right.delta.transaction_uuid) &&
         left.delta.local_transaction_id == right.delta.local_transaction_id &&
         left.delta.delta_kind == right.delta.delta_kind &&
         left.delta.key_payload == right.delta.key_payload &&
         left.delta.cleanup_horizon_token == right.delta.cleanup_horizon_token &&
         left.delta.committed == right.delta.committed &&
         left.commit_state == right.commit_state &&
         left.source_evidence_reference == right.source_evidence_reference;
}

bool PersistentSecondaryIndexDeltaLedgerEquals(
    const PersistentSecondaryIndexDeltaLedger& left,
    const PersistentSecondaryIndexDeltaLedger& right) {
  if (left.format_major != right.format_major ||
      left.format_minor != right.format_minor ||
      left.artifact_kind != right.artifact_kind ||
      left.records.size() != right.records.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.records.size(); ++i) {
    if (!SecondaryIndexDeltaLedgerRecordEquals(left.records[i], right.records[i])) {
      return false;
    }
  }
  return true;
}

SecondaryIndexDeltaLedgerAppendResult AppendPersistentSecondaryIndexDelta(
    PersistentSecondaryIndexDeltaLedger* ledger,
    const SecondaryIndexDeltaLedgerRecord& record,
    const SecondaryIndexDeltaLedgerLimits& limits) {
  if (ledger == nullptr) {
    return RefuseAppend(LedgerErrorStatus(),
                        nullptr,
                        "secondary_index_delta_ledger_missing_ledger",
                        "core.index.secondary_delta_ledger.missing_ledger",
                        "persistent ledger is required");
  }
  if (!LedgerShapeCompatible(*ledger)) {
    return RefuseAppend(LedgerErrorStatus(),
                        ledger,
                        "secondary_index_delta_ledger_incompatible_object",
                        "core.index.secondary_delta_ledger.incompatible_object",
                        "ledger object kind or format version is incompatible");
  }
  if (ledger->records.size() >= limits.max_entries) {
    return RefuseAppend(LedgerOverflowStatus(),
                        ledger,
                        "secondary_index_delta_ledger_overflow",
                        "core.index.secondary_delta_ledger.overflow",
                        "ledger entry limit reached");
  }
  const auto validation = ValidateRecord(record);
  if (!validation.status.ok()) {
    return RefuseAppend(validation.status,
                        ledger,
                        validation.diagnostic_code,
                        validation.message_key,
                        validation.arguments.empty() ? "" : validation.arguments.front().value);
  }

  PersistentSecondaryIndexDeltaLedger candidate = *ledger;
  candidate.records.push_back(record);
  const auto encoded = EncodePersistentSecondaryIndexDeltaLedger(candidate, limits);
  if (!encoded.ok()) {
    return RefuseAppend(encoded.status,
                        ledger,
                        encoded.diagnostic.diagnostic_code,
                        encoded.diagnostic.message_key,
                        encoded.diagnostic.arguments.empty() ? "" : encoded.diagnostic.arguments.front().value);
  }

  ledger->records.push_back(record);
  ledger->encoded_bytes = encoded.bytes.size();

  SecondaryIndexDeltaLedgerAppendResult result;
  result.status = LedgerOkStatus();
  result.appended = true;
  result.record_count = ledger->records.size();
  result.encoded_bytes = ledger->encoded_bytes;
  result.diagnostic = MakeSecondaryIndexDeltaLedgerDiagnostic(
      result.status, "ok", "core.index.secondary_delta_ledger.appended",
      "secondary-index delta appended to persistent ledger");
  return result;
}

SecondaryIndexDeltaLedgerEncodeResult EncodePersistentSecondaryIndexDeltaLedger(
    const PersistentSecondaryIndexDeltaLedger& ledger,
    const SecondaryIndexDeltaLedgerLimits& limits) {
  if (!LedgerShapeCompatible(ledger)) {
    return RefuseEncode(LedgerErrorStatus(),
                        "secondary_index_delta_ledger_incompatible_object",
                        "core.index.secondary_delta_ledger.incompatible_object",
                        "ledger object kind or format version is incompatible");
  }
  if (ledger.records.size() > limits.max_entries ||
      ledger.records.size() > std::numeric_limits<u32>::max()) {
    return RefuseEncode(LedgerOverflowStatus(),
                        "secondary_index_delta_ledger_overflow",
                        "core.index.secondary_delta_ledger.overflow",
                        "ledger entry limit reached");
  }
  std::vector<byte> out;
  out.reserve(128 + ledger.records.size() * 128);
  AppendBytes(&out, kLedgerMagic.data(), kLedgerMagic.size());
  AppendU32(&out, ledger.format_major);
  AppendU32(&out, ledger.format_minor);
  AppendString(&out, ledger.artifact_kind);
  AppendString(&out, kSecondaryIndexDeltaLedgerFeatureMapKey);
  AppendU32(&out, static_cast<u32>(ledger.records.size()));
  for (const auto& record : ledger.records) {
    const auto validation = ValidateRecord(record);
    if (!validation.status.ok()) {
      return RefuseEncode(validation.status,
                          validation.diagnostic_code,
                          validation.message_key,
                          validation.arguments.empty() ? "" : validation.arguments.front().value);
    }
    EncodeRecord(&out, record);
    if (out.size() + sizeof(u64) > limits.max_encoded_bytes) {
      return RefuseEncode(LedgerOverflowStatus(),
                          "secondary_index_delta_ledger_overflow",
                          "core.index.secondary_delta_ledger.overflow",
                          "ledger byte limit reached");
    }
  }
  AppendU64(&out, StableBytesFingerprint(out));
  if (out.size() > limits.max_encoded_bytes) {
    return RefuseEncode(LedgerOverflowStatus(),
                        "secondary_index_delta_ledger_overflow",
                        "core.index.secondary_delta_ledger.overflow",
                        "ledger byte limit reached");
  }

  SecondaryIndexDeltaLedgerEncodeResult result;
  result.status = LedgerOkStatus();
  result.bytes = std::move(out);
  result.diagnostic = MakeSecondaryIndexDeltaLedgerDiagnostic(
      result.status, "ok", "core.index.secondary_delta_ledger.encoded",
      "secondary-index delta ledger encoded");
  return result;
}

SecondaryIndexDeltaLedgerDecodeResult DecodePersistentSecondaryIndexDeltaLedger(
    const std::vector<byte>& bytes,
    const SecondaryIndexDeltaLedgerLimits& limits) {
  if (bytes.empty()) {
    return RefuseDecode(LedgerErrorStatus(),
                        "secondary_index_delta_ledger_missing",
                        "core.index.secondary_delta_ledger.missing",
                        "ledger image is missing");
  }
  if (bytes.size() > limits.max_encoded_bytes) {
    return RefuseDecode(LedgerOverflowStatus(),
                        "secondary_index_delta_ledger_overflow",
                        "core.index.secondary_delta_ledger.overflow",
                        "ledger byte limit reached");
  }
  if (bytes.size() < kLedgerMagic.size() + sizeof(u64)) {
    return RefuseDecode(LedgerErrorStatus(),
                        "secondary_index_delta_ledger_corrupt_truncated",
                        "core.index.secondary_delta_ledger.corrupt_truncated",
                        "ledger image is truncated");
  }
  const std::vector<byte> checksum_material(bytes.begin(), bytes.end() - sizeof(u64));
  const u64 expected_checksum = StableBytesFingerprint(checksum_material);
  const u64 observed_checksum =
      scratchbird::core::platform::LoadLittle64(bytes.data() + bytes.size() - sizeof(u64));
  if (expected_checksum != observed_checksum) {
    return RefuseDecode(LedgerErrorStatus(),
                        "secondary_index_delta_ledger_corrupt_checksum",
                        "core.index.secondary_delta_ledger.corrupt_checksum",
                        "ledger image checksum mismatch");
  }

  ByteReader reader{bytes};
  std::array<byte, 8> magic{};
  if (!reader.ReadBytes(magic.data(), magic.size()) || magic != kLedgerMagic) {
    return RefuseDecode(LedgerErrorStatus(),
                        "secondary_index_delta_ledger_incompatible_object",
                        "core.index.secondary_delta_ledger.incompatible_object",
                        "ledger magic does not identify a secondary-index delta ledger");
  }

  PersistentSecondaryIndexDeltaLedger ledger;
  std::string feature_map_key;
  u32 record_count = 0;
  if (!reader.ReadU32(&ledger.format_major) ||
      !reader.ReadU32(&ledger.format_minor) ||
      !reader.ReadString(&ledger.artifact_kind) ||
      !reader.ReadString(&feature_map_key) ||
      !reader.ReadU32(&record_count)) {
    return RefuseDecode(LedgerErrorStatus(),
                        "secondary_index_delta_ledger_corrupt_truncated",
                        "core.index.secondary_delta_ledger.corrupt_truncated",
                        "ledger header is truncated");
  }
  if (ledger.format_major != kSecondaryIndexDeltaLedgerFormatMajor ||
      ledger.format_minor != kSecondaryIndexDeltaLedgerFormatMinor) {
    return RefuseDecode(LedgerErrorStatus(),
                        "secondary_index_delta_ledger_wrong_version",
                        "core.index.secondary_delta_ledger.wrong_version",
                        "ledger format version is not supported");
  }
  if (ledger.artifact_kind != kSecondaryIndexDeltaLedgerArtifactKind ||
      feature_map_key != kSecondaryIndexDeltaLedgerFeatureMapKey) {
    return RefuseDecode(LedgerErrorStatus(),
                        "secondary_index_delta_ledger_incompatible_object",
                        "core.index.secondary_delta_ledger.incompatible_object",
                        "ledger artifact kind is incompatible");
  }
  if (record_count > limits.max_entries) {
    return RefuseDecode(LedgerOverflowStatus(),
                        "secondary_index_delta_ledger_overflow",
                        "core.index.secondary_delta_ledger.overflow",
                        "ledger entry limit reached");
  }
  ledger.records.reserve(record_count);
  for (u32 i = 0; i < record_count; ++i) {
    SecondaryIndexDeltaLedgerRecord record;
    if (!DecodeRecord(&reader, &record)) {
      return RefuseDecode(LedgerErrorStatus(),
                          "secondary_index_delta_ledger_malformed_operation",
                          "core.index.secondary_delta_ledger.malformed_operation",
                          "ledger record is malformed");
    }
    const auto validation = ValidateRecord(record);
    if (!validation.status.ok()) {
      return RefuseDecode(validation.status,
                          validation.diagnostic_code,
                          validation.message_key,
                          validation.arguments.empty() ? "" : validation.arguments.front().value);
    }
    ledger.records.push_back(std::move(record));
  }
  if (reader.offset != bytes.size() - sizeof(u64)) {
    return RefuseDecode(LedgerErrorStatus(),
                        "secondary_index_delta_ledger_malformed_operation",
                        "core.index.secondary_delta_ledger.malformed_operation",
                        "ledger image has trailing malformed operation data");
  }
  ledger.encoded_bytes = bytes.size();

  SecondaryIndexDeltaLedgerDecodeResult result;
  result.status = LedgerOkStatus();
  result.ledger = std::move(ledger);
  result.diagnostic = MakeSecondaryIndexDeltaLedgerDiagnostic(
      result.status, "ok", "core.index.secondary_delta_ledger.decoded",
      "secondary-index delta ledger decoded");
  return result;
}

u64 StableSecondaryIndexDeltaLedgerFingerprint(
    const PersistentSecondaryIndexDeltaLedger& ledger,
    const SecondaryIndexDeltaLedgerLimits& limits) {
  const auto encoded = EncodePersistentSecondaryIndexDeltaLedger(ledger, limits);
  if (!encoded.ok()) {
    return 0;
  }
  return StableBytesFingerprint(encoded.bytes);
}

SecondaryIndexDeltaLedgerRecoveryResult ClassifySecondaryIndexDeltaLedgerForRecovery(
    const PersistentSecondaryIndexDeltaLedger& ledger) {
  SecondaryIndexDeltaLedgerRecoveryResult result;
  if (!LedgerShapeCompatible(ledger)) {
    result.status = LedgerErrorStatus();
    result.recovery_class =
        SecondaryIndexDeltaLedgerRecoveryClass::corrupt_incompatible_fail_closed;
    result.action = SecondaryIndexDeltaLedgerRecoveryAction::fail_closed;
    result.fail_closed = true;
    result.stable_reason = "ledger object is incompatible with the current persisted contract";
    result.diagnostic = MakeSecondaryIndexDeltaLedgerDiagnostic(
        result.status,
        "secondary_index_delta_ledger_incompatible_object",
        "core.index.secondary_delta_ledger.incompatible_object",
        result.stable_reason);
    return result;
  }
  if (ledger.records.empty()) {
    result.status = LedgerOkStatus();
    result.recovery_class = SecondaryIndexDeltaLedgerRecoveryClass::empty_clean;
    result.action = SecondaryIndexDeltaLedgerRecoveryAction::no_action;
    result.fail_closed = false;
    result.stable_reason = "ledger contains no pending secondary-index deltas";
    result.diagnostic = MakeSecondaryIndexDeltaLedgerDiagnostic(
        result.status, "ok", "core.index.secondary_delta_ledger.recovery_clean",
        result.stable_reason);
    return result;
  }

  bool saw_repair = false;
  bool saw_refused = false;
  bool saw_merged = false;
  for (const auto& record : ledger.records) {
    const auto validation = ValidateRecord(record);
    if (!validation.status.ok()) {
      result.status = validation.status;
      result.recovery_class =
          SecondaryIndexDeltaLedgerRecoveryClass::corrupt_incompatible_fail_closed;
      result.action = SecondaryIndexDeltaLedgerRecoveryAction::fail_closed;
      result.fail_closed = true;
      result.stable_reason = validation.arguments.empty()
                                 ? "ledger record failed validation"
                                 : validation.arguments.front().value;
      result.diagnostic = validation;
      return result;
    }
    switch (record.commit_state) {
      case SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted:
        ++result.uncommitted_delta_count;
        break;
      case SecondaryIndexDeltaLedgerCommitState::committed_premerge:
        ++result.committed_premerge_delta_count;
        break;
      case SecondaryIndexDeltaLedgerCommitState::merged_cleaned:
        saw_merged = true;
        break;
      case SecondaryIndexDeltaLedgerCommitState::repair_rebuild_required:
        saw_repair = true;
        break;
      case SecondaryIndexDeltaLedgerCommitState::refused:
        saw_refused = true;
        break;
    }
  }

  result.status = LedgerOkStatus();
  result.fail_closed = false;
  if (saw_refused) {
    result.recovery_class = SecondaryIndexDeltaLedgerRecoveryClass::refused;
    result.action = SecondaryIndexDeltaLedgerRecoveryAction::refuse_open;
    result.fail_closed = true;
    result.stable_reason = "ledger contains a durable refusal state";
  } else if (saw_repair) {
    result.recovery_class = SecondaryIndexDeltaLedgerRecoveryClass::repair_rebuild_required;
    result.action = SecondaryIndexDeltaLedgerRecoveryAction::rebuild_from_authoritative_base;
    result.stable_reason = "ledger requires repair or rebuild from authoritative base index state";
  } else if (result.uncommitted_delta_count > 0) {
    result.recovery_class =
        SecondaryIndexDeltaLedgerRecoveryClass::has_uncommitted_precommit_delta;
    result.action =
        SecondaryIndexDeltaLedgerRecoveryAction::retain_for_mga_transaction_finality;
    result.stable_reason = "ledger contains precommit delta records requiring transaction metadata classification";
  } else if (result.committed_premerge_delta_count > 0) {
    result.recovery_class =
        SecondaryIndexDeltaLedgerRecoveryClass::committed_premerge_requires_overlay_merge;
    result.action = SecondaryIndexDeltaLedgerRecoveryAction::apply_overlay_then_merge;
    result.stable_reason = "ledger contains committed premerge delta records requiring overlay visibility and idempotent merge";
  } else if (saw_merged) {
    result.recovery_class = SecondaryIndexDeltaLedgerRecoveryClass::clean_after_merge;
    result.action = SecondaryIndexDeltaLedgerRecoveryAction::no_action;
    result.stable_reason = "ledger contains only clean merged records";
  } else {
    result.recovery_class =
        SecondaryIndexDeltaLedgerRecoveryClass::corrupt_incompatible_fail_closed;
    result.action = SecondaryIndexDeltaLedgerRecoveryAction::fail_closed;
    result.fail_closed = true;
    result.stable_reason = "ledger state could not be classified";
  }
  result.diagnostic = MakeSecondaryIndexDeltaLedgerDiagnostic(
      result.status,
      result.fail_closed ? "secondary_index_delta_ledger_recovery_refused" : "ok",
      result.fail_closed ? "core.index.secondary_delta_ledger.recovery_refused"
                         : "core.index.secondary_delta_ledger.recovery_classified",
      result.stable_reason);
  return result;
}

SecondaryIndexDeltaLedgerRecoveryResult ClassifySecondaryIndexDeltaLedgerImageForRecovery(
    const std::vector<byte>& bytes,
    const SecondaryIndexDeltaLedgerLimits& limits) {
  if (bytes.empty()) {
    PersistentSecondaryIndexDeltaLedger empty;
    return ClassifySecondaryIndexDeltaLedgerForRecovery(empty);
  }
  const auto decoded = DecodePersistentSecondaryIndexDeltaLedger(bytes, limits);
  if (!decoded.ok()) {
    SecondaryIndexDeltaLedgerRecoveryResult result;
    result.status = decoded.status;
    result.recovery_class =
        SecondaryIndexDeltaLedgerRecoveryClass::corrupt_incompatible_fail_closed;
    result.action = SecondaryIndexDeltaLedgerRecoveryAction::fail_closed;
    result.fail_closed = true;
    result.stable_reason = decoded.diagnostic.arguments.empty()
                               ? "ledger image failed persisted-format validation"
                               : decoded.diagnostic.arguments.front().value;
    result.diagnostic = decoded.diagnostic;
    return result;
  }
  return ClassifySecondaryIndexDeltaLedgerForRecovery(decoded.ledger);
}

DiagnosticRecord MakeSecondaryIndexDeltaLedgerDiagnostic(
    Status status,
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
                        "core.index.secondary_delta_ledger",
                        status.ok() ? "" : "retain bounded ledger image and recover only from engine transaction metadata");
}

}  // namespace scratchbird::core::index
