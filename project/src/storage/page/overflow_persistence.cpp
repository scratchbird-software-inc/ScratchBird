// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-OVERFLOW-PERSISTENCE-ANCHOR
#include "overflow_persistence.hpp"

#include "disk_device.hpp"
#include "page_body_integrity.hpp"
#include "page_header.hpp"
#include "page_manager.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::HostToLittle32;
using scratchbird::core::platform::HostToLittle64;
using scratchbird::core::platform::LittleToHost32;
using scratchbird::core::platform::LittleToHost64;
using scratchbird::core::uuid::IsEngineIdentityUuid;
using scratchbird::storage::disk::FileDevice;
using scratchbird::storage::disk::kPageHeaderSerializedBytes;

Status OverflowOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status OverflowErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

TypedUuid GeneratedId(UuidKind kind, u64 seed) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

std::string HashPayload(const std::vector<byte>& payload) {
  u64 hash = 1469598103934665603ULL;
  for (const byte value : payload) {
    hash ^= static_cast<u64>(value);
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << "fnv1a64:" << hash;
  return out.str();
}

u64 ChecksumFragment(const std::vector<byte>& payload) {
  u64 checksum = 0;
  for (const byte value : payload) {
    checksum = (checksum * 131) + static_cast<u64>(value);
  }
  return checksum;
}

constexpr std::array<byte, 8> kOverflowChunkPageBodyMagic = {
    'S', 'B', 'O', 'V', 'C', 'H', '0', '1'};
constexpr u32 kOverflowChunkPageBodyVersion = 1;
constexpr u32 kOffsetMagic = 0;
constexpr u32 kOffsetVersion = 8;
constexpr u32 kOffsetState = 12;
constexpr u32 kOffsetOrdinal = 16;
constexpr u32 kOffsetChunkCount = 20;
constexpr u32 kOffsetByteCount = 24;
constexpr u32 kOffsetContentHashBytes = 28;
constexpr u32 kOffsetGeneration = 32;
constexpr u32 kOffsetLocalTransactionId = 40;
constexpr u32 kOffsetByteOffset = 48;
constexpr u32 kOffsetChecksum = 56;
constexpr u32 kOffsetOverflowValueUuid = 64;
constexpr u32 kOffsetChunkUuid = 80;
constexpr u32 kOffsetPageUuid = 96;
constexpr u32 kOffsetRowUuid = 112;
constexpr u32 kOffsetObjectUuid = 128;
constexpr u32 kOffsetTransactionUuid = 144;
constexpr u32 kOffsetChunkPolicyUuid = 160;
constexpr u32 kOffsetVariablePayload = 176;

void Store32(std::vector<byte>* buffer, u32 offset, u32 value) {
  const u32 stored = HostToLittle32(value);
  std::memcpy(buffer->data() + offset, &stored, sizeof(stored));
}

void Store64(std::vector<byte>* buffer, u32 offset, u64 value) {
  const u64 stored = HostToLittle64(value);
  std::memcpy(buffer->data() + offset, &stored, sizeof(stored));
}

u32 Load32(const std::vector<byte>& buffer, u32 offset) {
  u32 value = 0;
  std::memcpy(&value, buffer.data() + offset, sizeof(value));
  return LittleToHost32(value);
}

u64 Load64(const std::vector<byte>& buffer, u32 offset) {
  u64 value = 0;
  std::memcpy(&value, buffer.data() + offset, sizeof(value));
  return LittleToHost64(value);
}

void StoreUuid(std::vector<byte>* buffer, u32 offset, const TypedUuid& uuid) {
  std::copy(uuid.value.bytes.begin(), uuid.value.bytes.end(), buffer->begin() + offset);
}

TypedUuid LoadUuid(const std::vector<byte>& buffer, u32 offset, UuidKind kind) {
  TypedUuid uuid;
  uuid.kind = kind;
  std::copy(buffer.begin() + offset,
            buffer.begin() + offset + uuid.value.bytes.size(),
            uuid.value.bytes.begin());
  return uuid;
}

u64 ChunkByteOffset(const OverflowValueRecord& record, u32 ordinal) {
  u64 offset = 0;
  for (const auto& chunk : record.chunks) {
    if (chunk.ordinal == ordinal) {
      return offset;
    }
    offset += chunk.byte_count;
  }
  return offset;
}

OverflowChunkPageBodyResult ChunkPageBodyError(std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {}) {
  OverflowChunkPageBodyResult result;
  result.status = OverflowErrorStatus();
  result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                        std::move(diagnostic_code),
                                                        std::move(message_key),
                                                        std::move(detail));
  return result;
}

OverflowBlobPageResult BlobPageError(std::string diagnostic_code,
                                     std::string message_key,
                                     std::string detail = {}) {
  OverflowBlobPageResult result;
  result.status = OverflowErrorStatus();
  result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                        std::move(diagnostic_code),
                                                        std::move(message_key),
                                                        std::move(detail));
  return result;
}

bool AddWouldOverflow(u64 left, u64 right) {
  return right > std::numeric_limits<u64>::max() - left;
}

bool ValidBlobPageContext(FileDevice* device,
                          const TypedUuid& database_uuid,
                          const TypedUuid& filespace_uuid,
                          u32 page_size) {
  return device != nullptr && device->is_open() &&
         IsTypedEngineIdentity(database_uuid, UuidKind::database) &&
         IsTypedEngineIdentity(filespace_uuid, UuidKind::filespace) &&
         scratchbird::storage::disk::IsSupportedDatabasePageSize(page_size);
}

PageManagerContext BlobPageManagerContext(const TypedUuid& database_uuid,
                                          const TypedUuid& filespace_uuid,
                                          u32 page_size) {
  PageManagerContext context;
  context.page_size = page_size;
  context.database_uuid = database_uuid;
  context.filespace_uuid = filespace_uuid;
  return context;
}

OverflowBlobPageLocation LocationFor(const OverflowChunkRecord& chunk) {
  OverflowBlobPageLocation location;
  location.chunk_uuid = chunk.chunk_uuid;
  location.page_uuid = chunk.page_uuid;
  location.page_number = chunk.page_number;
  location.page_generation = chunk.page_generation;
  location.ordinal = chunk.ordinal;
  location.byte_count = chunk.byte_count;
  return location;
}

OverflowEvidenceRecord BuildEvidence(OverflowLedger* ledger,
                                     const OverflowValueRecord& record,
                                     std::string action,
                                     OverflowValueState previous_state,
                                     OverflowValueState new_state,
                                     std::string diagnostic_code,
                                     std::string reason,
                                     bool durable_state_changed,
                                     u64 cleanup_horizon = 0) {
  OverflowEvidenceRecord evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.action = std::move(action);
  evidence.evidence_id = GeneratedId(UuidKind::object, evidence.sequence);
  evidence.overflow_value_uuid = record.overflow_value_uuid;
  evidence.row_uuid = record.row_uuid;
  evidence.object_uuid = record.object_uuid;
  evidence.transaction_uuid = record.transaction_uuid;
  evidence.chunk_policy_uuid = record.chunk_policy_uuid;
  evidence.local_transaction_id = record.local_transaction_id;
  evidence.chunk_count = static_cast<u32>(record.chunks.size());
  for (const auto& chunk : record.chunks) {
    evidence.payload_bytes += chunk.byte_count;
  }
  evidence.content_hash = record.content_hash;
  evidence.previous_state = previous_state;
  evidence.new_state = new_state;
  evidence.authoritative_cleanup_horizon_local_transaction_id = cleanup_horizon;
  evidence.reason = std::move(reason);
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.durable_state_changed = durable_state_changed;
  return evidence;
}

OverflowPersistResult RefusePersist(OverflowLedger* ledger,
                                    const OverflowPersistRequest& request,
                                    std::string diagnostic_code,
                                    std::string message_key,
                                    std::string detail) {
  OverflowPersistResult result;
  result.status = OverflowErrorStatus();
  result.persisted = false;
  OverflowValueRecord record;
  record.row_uuid = request.row_uuid;
  record.object_uuid = request.object_uuid;
  record.transaction_uuid = request.transaction_uuid;
  record.chunk_policy_uuid = request.chunk_policy_uuid;
  record.local_transaction_id = request.local_transaction_id;
  record.generation = request.generation;
  record.value_descriptor = request.value_descriptor;
  record.state = OverflowValueState::absent;
  result.evidence = BuildEvidence(ledger,
                                  record,
                                  "refuse_overflow_persist",
                                  OverflowValueState::absent,
                                  OverflowValueState::absent,
                                  diagnostic_code,
                                  detail,
                                  false);
  result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                       std::move(diagnostic_code),
                                                       std::move(message_key),
                                                       std::move(detail));
  if (ledger != nullptr) {
    ledger->evidence.push_back(result.evidence);
  }
  return result;
}

OverflowValueRecord* FindMutableOverflowValue(OverflowLedger* ledger, const TypedUuid& overflow_value_uuid) {
  if (ledger == nullptr) {
    return nullptr;
  }
  const auto found = std::find_if(ledger->values.begin(),
                                  ledger->values.end(),
                                  [&](const OverflowValueRecord& record) {
                                    return SameUuid(record.overflow_value_uuid, overflow_value_uuid);
                                  });
  return found == ledger->values.end() ? nullptr : &(*found);
}

bool OwnTransaction(const OverflowValueRecord& record, const TypedUuid& transaction_uuid, u64 local_transaction_id) {
  return SameUuid(record.transaction_uuid, transaction_uuid) &&
         record.local_transaction_id == local_transaction_id;
}

bool ValidateOverflowChunkChain(const OverflowValueRecord& record, std::string* detail) {
  // SEARCH_KEY: SB_MGA_OVERFLOW_CHUNK_CHAIN_FAIL_CLOSED
  if (record.state == OverflowValueState::absent || record.state == OverflowValueState::cleanup_reclaimed) {
    if (!record.chunks.empty()) {
      if (detail != nullptr) { *detail = "reclaimed_or_absent_overflow_has_chunks"; }
      return false;
    }
    return true;
  }
  if (record.chunks.empty()) {
    if (detail != nullptr) { *detail = "overflow_chunk_chain_empty"; }
    return false;
  }

  std::vector<byte> reconstructed;
  for (u32 expected_ordinal = 0; expected_ordinal < record.chunks.size(); ++expected_ordinal) {
    const auto& chunk = record.chunks[expected_ordinal];
    if (chunk.ordinal != expected_ordinal) {
      if (detail != nullptr) { *detail = "overflow_chunk_ordinal_gap"; }
      return false;
    }
    if (chunk.byte_count == 0 || chunk.byte_count != chunk.payload_fragment.size()) {
      if (detail != nullptr) { *detail = "overflow_chunk_byte_count_invalid"; }
      return false;
    }
    if (ChecksumFragment(chunk.payload_fragment) != chunk.checksum) {
      if (detail != nullptr) { *detail = "overflow_chunk_checksum_mismatch"; }
      return false;
    }
    reconstructed.insert(reconstructed.end(), chunk.payload_fragment.begin(), chunk.payload_fragment.end());
  }
  if (HashPayload(reconstructed) != record.content_hash) {
    if (detail != nullptr) { *detail = "overflow_content_hash_mismatch"; }
    return false;
  }
  return true;
}

}  // namespace

const char* OverflowValueStateName(OverflowValueState state) {
  switch (state) {
    case OverflowValueState::absent:
      return "absent";
    case OverflowValueState::durable_uncommitted:
      return "durable_uncommitted";
    case OverflowValueState::committed_visible:
      return "committed_visible";
    case OverflowValueState::rolled_back:
      return "rolled_back";
    case OverflowValueState::cleanup_reclaimed:
      return "cleanup_reclaimed";
    case OverflowValueState::quarantine:
      return "quarantine";
  }
  return "unknown";
}

const char* OverflowRecoveryActionName(OverflowRecoveryAction action) {
  switch (action) {
    case OverflowRecoveryAction::no_action:
      return "no_action";
    case OverflowRecoveryAction::retain:
      return "retain";
    case OverflowRecoveryAction::roll_back:
      return "roll_back";
    case OverflowRecoveryAction::reclaim_after_horizon:
      return "reclaim_after_horizon";
    case OverflowRecoveryAction::fail_closed:
      return "fail_closed";
  }
  return "unknown";
}

OverflowPersistResult PersistOverflowValue(OverflowLedger* ledger, const OverflowPersistRequest& request) {
  if (ledger == nullptr) {
    return RefusePersist(nullptr,
                         request,
                         "overflow_persist_missing_ledger",
                         "storage.page.overflow.missing_ledger",
                         "overflow ledger is required");
  }
  if (!request.row_uuid.valid() || !request.object_uuid.valid() || !request.transaction_uuid.valid()) {
    return RefusePersist(ledger,
                         request,
                         "overflow_persist_invalid_identity",
                         "storage.page.overflow.invalid_identity",
                         "row_uuid, object_uuid, and transaction_uuid must be valid engine UUIDs");
  }
  if (request.local_transaction_id == 0) {
    return RefusePersist(ledger,
                         request,
                         "overflow_persist_invalid_transaction",
                         "storage.page.overflow.invalid_transaction",
                         "local_transaction_id must be non-zero");
  }
  if (request.value_descriptor.empty()) {
    return RefusePersist(ledger,
                         request,
                         "overflow_persist_missing_descriptor",
                         "storage.page.overflow.missing_descriptor",
                         "value_descriptor is required");
  }
  if (request.payload_bytes.empty()) {
    return RefusePersist(ledger,
                         request,
                         "overflow_persist_empty_payload",
                         "storage.page.overflow.empty_payload",
                         "payload_bytes must be non-empty");
  }
  if (request.chunk_size == 0) {
    return RefusePersist(ledger,
                         request,
                         "overflow_persist_invalid_chunk_size",
                         "storage.page.overflow.invalid_chunk_size",
                         "chunk_size must be non-zero");
  }

  OverflowValueRecord record;
  record.overflow_value_uuid = GeneratedId(UuidKind::object, 100000 + ledger->next_evidence_sequence);
  record.row_uuid = request.row_uuid;
  record.object_uuid = request.object_uuid;
  record.transaction_uuid = request.transaction_uuid;
  record.chunk_policy_uuid = request.chunk_policy_uuid;
  record.local_transaction_id = request.local_transaction_id;
  record.generation = request.generation == 0 ? 1 : request.generation;
  record.value_descriptor = request.value_descriptor;
  record.content_hash = HashPayload(request.payload_bytes);
  record.state = OverflowValueState::durable_uncommitted;

  u32 ordinal = 0;
  for (std::size_t offset = 0; offset < request.payload_bytes.size(); offset += request.chunk_size) {
    const std::size_t end = std::min<std::size_t>(request.payload_bytes.size(), offset + request.chunk_size);
    OverflowChunkRecord chunk;
    chunk.chunk_uuid = GeneratedId(UuidKind::object, 110000 + ledger->next_evidence_sequence + ordinal);
    chunk.page_uuid = GeneratedId(UuidKind::page, 120000 + ledger->next_evidence_sequence + ordinal);
    chunk.ordinal = ordinal++;
    chunk.byte_count = static_cast<u32>(end - offset);
    chunk.payload_fragment.assign(request.payload_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                  request.payload_bytes.begin() + static_cast<std::ptrdiff_t>(end));
    chunk.checksum = ChecksumFragment(chunk.payload_fragment);
    record.chunks.push_back(std::move(chunk));
  }

  OverflowPersistResult result;
  result.status = OverflowOkStatus();
  result.persisted = true;
  result.overflow_value_uuid = record.overflow_value_uuid;
  result.chunk_count = static_cast<u32>(record.chunks.size());
  result.first_page_uuid = record.chunks.empty() ? TypedUuid{} : record.chunks.front().page_uuid;
  result.content_hash = record.content_hash;
  result.record = record;
  result.evidence = BuildEvidence(ledger,
                                  record,
                                  "persist_overflow_value",
                                  OverflowValueState::absent,
                                  OverflowValueState::durable_uncommitted,
                                  "ok",
                                  "overflow value persisted",
                                  true);
  result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                       "ok",
                                                       "storage.page.overflow.persisted",
                                                       "overflow value persisted");
  ledger->values.push_back(record);
  ledger->evidence.push_back(result.evidence);
  return result;
}

OverflowReadResult ReadOverflowValue(const OverflowLedger& ledger, const OverflowReadRequest& request) {
  OverflowReadResult result;
  const auto* record = FindOverflowValue(ledger, request.overflow_value_uuid);
  if (record == nullptr) {
    result.status = OverflowErrorStatus();
    result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                         "overflow_read_not_found",
                                                         "storage.page.overflow.read_not_found",
                                                         "overflow value UUID was not found");
    return result;
  }
  result.found = true;
  std::string chunk_detail;
  if (!ValidateOverflowChunkChain(*record, &chunk_detail)) {
    result.status = OverflowErrorStatus();
    result.visible = false;
    result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                         "overflow_read_chunk_chain_invalid",
                                                         "storage.page.overflow.chunk_chain_invalid",
                                                         chunk_detail);
    return result;
  }
  const bool own_uncommitted = request.include_uncommitted_own_transaction &&
                               record->state == OverflowValueState::durable_uncommitted &&
                               OwnTransaction(*record, request.transaction_uuid, request.local_transaction_id);
  if (record->state != OverflowValueState::committed_visible && !own_uncommitted) {
    result.status = OverflowErrorStatus();
    result.visible = false;
    result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                         "overflow_read_not_visible",
                                                         "storage.page.overflow.read_not_visible",
                                                         OverflowValueStateName(record->state));
    return result;
  }

  result.status = OverflowOkStatus();
  result.visible = true;
  for (const auto& chunk : record->chunks) {
    if (ChecksumFragment(chunk.payload_fragment) != chunk.checksum) {
      result.status = OverflowErrorStatus();
      result.visible = false;
      result.payload_bytes.clear();
      result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                           "overflow_read_checksum_mismatch",
                                                           "storage.page.overflow.checksum_mismatch",
                                                           "overflow chunk checksum mismatch");
      return result;
    }
    result.payload_bytes.insert(result.payload_bytes.end(), chunk.payload_fragment.begin(), chunk.payload_fragment.end());
  }
  result.content_hash = HashPayload(result.payload_bytes);
  if (result.content_hash != record->content_hash) {
    result.status = OverflowErrorStatus();
    result.visible = false;
    result.payload_bytes.clear();
    result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                         "overflow_read_hash_mismatch",
                                                         "storage.page.overflow.hash_mismatch",
                                                         "overflow content hash mismatch");
    return result;
  }
  result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                       "ok",
                                                       "storage.page.overflow.read",
                                                       "overflow value reconstructed");
  return result;
}

OverflowMutationResult CommitOverflowValue(OverflowLedger* ledger, const OverflowCommitRequest& request) {
  OverflowMutationResult result;
  auto* record = FindMutableOverflowValue(ledger, request.overflow_value_uuid);
  if (record == nullptr || !OwnTransaction(*record, request.transaction_uuid, request.local_transaction_id) ||
      record->state != OverflowValueState::durable_uncommitted) {
    result.status = OverflowErrorStatus();
    result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                         "overflow_commit_refused",
                                                         "storage.page.overflow.commit_refused",
                                                         "overflow value is not commit-eligible");
    return result;
  }
  std::string chunk_detail;
  if (!ValidateOverflowChunkChain(*record, &chunk_detail)) {
    result.status = OverflowErrorStatus();
    result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                         "overflow_commit_chunk_chain_invalid",
                                                         "storage.page.overflow.commit_chunk_chain_invalid",
                                                         chunk_detail);
    return result;
  }
  const auto previous = record->state;
  record->state = OverflowValueState::committed_visible;
  result.status = OverflowOkStatus();
  result.changed = true;
  result.record = *record;
  result.evidence = BuildEvidence(ledger,
                                  *record,
                                  "commit_overflow_value",
                                  previous,
                                  record->state,
                                  "ok",
                                  request.reason,
                                  true);
  result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                       "ok",
                                                       "storage.page.overflow.committed",
                                                       "overflow value committed visible");
  ledger->evidence.push_back(result.evidence);
  return result;
}

OverflowMutationResult RollbackOverflowValue(OverflowLedger* ledger, const OverflowRollbackRequest& request) {
  OverflowMutationResult result;
  auto* record = FindMutableOverflowValue(ledger, request.overflow_value_uuid);
  if (record == nullptr || !OwnTransaction(*record, request.transaction_uuid, request.local_transaction_id) ||
      record->state == OverflowValueState::cleanup_reclaimed) {
    result.status = OverflowErrorStatus();
    result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                         "overflow_rollback_refused",
                                                         "storage.page.overflow.rollback_refused",
                                                         "overflow value is not rollback-eligible");
    return result;
  }
  const auto previous = record->state;
  record->state = OverflowValueState::rolled_back;
  result.status = OverflowOkStatus();
  result.changed = true;
  result.record = *record;
  result.evidence = BuildEvidence(ledger,
                                  *record,
                                  "rollback_overflow_value",
                                  previous,
                                  record->state,
                                  "ok",
                                  request.reason,
                                  true);
  result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                       "ok",
                                                       "storage.page.overflow.rolled_back",
                                                       "overflow value rolled back");
  ledger->evidence.push_back(result.evidence);
  return result;
}

OverflowCleanupResult CleanupOverflowValues(OverflowLedger* ledger, const OverflowCleanupRequest& request) {
  OverflowCleanupResult result;
  if (ledger == nullptr || !request.cleanup_horizon_authoritative) {
    result.status = OverflowErrorStatus();
    result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                         "overflow_cleanup_horizon_not_authoritative",
                                                         "storage.page.overflow.cleanup_horizon_not_authoritative",
                                                         "cleanup requires authoritative transaction horizon");
    return result;
  }

  OverflowValueRecord summary;
  summary.state = OverflowValueState::cleanup_reclaimed;
  u64 retained = 0;
  u64 cleaned = 0;
  for (auto& record : ledger->values) {
    if ((record.state == OverflowValueState::rolled_back || record.state == OverflowValueState::committed_visible) &&
        record.local_transaction_id <= request.authoritative_cleanup_horizon_local_transaction_id) {
      if (cleaned == 0) {
        summary = record;
      }
      ++cleaned;
      record.state = OverflowValueState::cleanup_reclaimed;
      record.chunks.clear();
    } else if (record.state != OverflowValueState::cleanup_reclaimed) {
      ++retained;
    }
  }

  result.status = OverflowOkStatus();
  result.cleaned = cleaned > 0;
  result.cleaned_count = cleaned;
  result.retained_count = retained;
  result.evidence = BuildEvidence(ledger,
                                  summary,
                                  "cleanup_overflow_values",
                                  OverflowValueState::rolled_back,
                                  OverflowValueState::cleanup_reclaimed,
                                  cleaned > 0 ? "ok" : "overflow_cleanup_noop",
                                  request.reason,
                                  cleaned > 0,
                                  request.authoritative_cleanup_horizon_local_transaction_id);
  result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                       cleaned > 0 ? "ok" : "overflow_cleanup_noop",
                                                       "storage.page.overflow.cleaned",
                                                       "overflow cleanup evaluated");
  ledger->evidence.push_back(result.evidence);
  return result;
}

OverflowRecoveryResult ClassifyOverflowLedgerForRecovery(const OverflowLedger& ledger) {
  OverflowRecoveryResult result;
  result.status = OverflowOkStatus();
  result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                       "ok",
                                                       "storage.page.overflow.recovery_classified",
                                                       "overflow ledger classified");
  result.classifications.reserve(ledger.values.size());
  for (const auto& record : ledger.values) {
    OverflowRecoveryClassification classification;
    classification.overflow_value_uuid = record.overflow_value_uuid;
    classification.observed_state = record.state;
    std::string chunk_detail;
    if (!ValidateOverflowChunkChain(record, &chunk_detail)) {
      classification.action = OverflowRecoveryAction::fail_closed;
      classification.fail_closed = true;
      classification.stable_reason = "overflow chunk chain invalid:" + chunk_detail;
      result.write_admission_must_remain_fenced = true;
      result.classifications.push_back(classification);
      continue;
    }
    switch (record.state) {
      case OverflowValueState::durable_uncommitted:
        classification.action = OverflowRecoveryAction::roll_back;
        classification.fail_closed = false;
        classification.stable_reason = "uncommitted overflow value rolls back during recovery";
        break;
      case OverflowValueState::committed_visible:
        classification.action = OverflowRecoveryAction::retain;
        classification.fail_closed = false;
        classification.stable_reason = "committed overflow value retained until cleanup horizon";
        break;
      case OverflowValueState::rolled_back:
        classification.action = OverflowRecoveryAction::reclaim_after_horizon;
        classification.fail_closed = false;
        classification.stable_reason = "rolled-back overflow value waits for authoritative cleanup horizon";
        break;
      case OverflowValueState::cleanup_reclaimed:
      case OverflowValueState::absent:
        classification.action = OverflowRecoveryAction::no_action;
        classification.fail_closed = false;
        classification.stable_reason = "state has no restart mutation";
        break;
      case OverflowValueState::quarantine:
        classification.action = OverflowRecoveryAction::fail_closed;
        classification.fail_closed = true;
        classification.stable_reason = "quarantined overflow value blocks automatic restart";
        result.write_admission_must_remain_fenced = true;
        break;
    }
    result.classifications.push_back(classification);
  }
  return result;
}

OverflowRecoveryResult ApplyOverflowLedgerRecovery(OverflowLedger* ledger) {
  // SEARCH_KEY: SB_MGA_OVERFLOW_RECOVERY_APPLY
  OverflowRecoveryResult result;
  if (ledger == nullptr) {
    result.status = OverflowErrorStatus();
    result.write_admission_must_remain_fenced = true;
    result.diagnostic = MakeOverflowPersistenceDiagnostic(result.status,
                                                         "overflow_recovery_missing_ledger",
                                                         "storage.page.overflow.recovery_missing_ledger",
                                                         "overflow ledger is required for recovery");
    return result;
  }

  result = ClassifyOverflowLedgerForRecovery(*ledger);
  if (result.write_admission_must_remain_fenced) {
    return result;
  }

  for (auto& record : ledger->values) {
    if (record.state != OverflowValueState::durable_uncommitted) { continue; }
    const auto previous = record.state;
    record.state = OverflowValueState::rolled_back;
    result.durable_state_changed = true;
    auto evidence = BuildEvidence(ledger,
                                  record,
                                  "recover_rollback_overflow_value",
                                  previous,
                                  record.state,
                                  "ok",
                                  "uncommitted overflow value rolled back during recovery",
                                  true);
    ledger->evidence.push_back(std::move(evidence));
  }

  result = ClassifyOverflowLedgerForRecovery(*ledger);
  result.durable_state_changed = true;
  return result;
}

OverflowChunkPageBodyResult BuildOverflowChunkPageBody(const OverflowValueRecord& record,
                                                       const OverflowChunkRecord& chunk,
                                                       u32 page_size) {
  if (page_size <= scratchbird::storage::disk::kPageHeaderSerializedBytes ||
      page_size - scratchbird::storage::disk::kPageHeaderSerializedBytes < kOffsetVariablePayload) {
    return ChunkPageBodyError("overflow_chunk_page_body_page_size_invalid",
                              "storage.page.overflow.chunk_page_body_page_size_invalid",
                              std::to_string(page_size));
  }
  if (!ValidateOverflowChunkChain(record, nullptr)) {
    return ChunkPageBodyError("overflow_chunk_page_body_record_invalid",
                              "storage.page.overflow.chunk_page_body_record_invalid",
                              "record chunk chain is invalid");
  }
  if (chunk.byte_count == 0 || chunk.byte_count != chunk.payload_fragment.size()) {
    return ChunkPageBodyError("overflow_chunk_page_body_chunk_invalid",
                              "storage.page.overflow.chunk_page_body_chunk_invalid",
                              "chunk byte count does not match payload");
  }
  const u32 content_hash_bytes = static_cast<u32>(record.content_hash.size());
  const u64 required_bytes = static_cast<u64>(kOffsetVariablePayload) +
                             content_hash_bytes +
                             chunk.payload_fragment.size();
  const u64 body_capacity = page_size - scratchbird::storage::disk::kPageHeaderSerializedBytes;
  if (required_bytes > body_capacity) {
    return ChunkPageBodyError("overflow_chunk_page_body_capacity_exceeded",
                              "storage.page.overflow.chunk_page_body_capacity_exceeded",
                              std::to_string(required_bytes) + ":" + std::to_string(body_capacity));
  }

  OverflowChunkPageBody body;
  body.overflow_value_uuid = record.overflow_value_uuid;
  body.chunk_uuid = chunk.chunk_uuid;
  body.page_uuid = chunk.page_uuid;
  body.row_uuid = record.row_uuid;
  body.object_uuid = record.object_uuid;
  body.transaction_uuid = record.transaction_uuid;
  body.chunk_policy_uuid = record.chunk_policy_uuid;
  body.state = record.state;
  body.generation = record.generation;
  body.local_transaction_id = record.local_transaction_id;
  body.byte_offset = ChunkByteOffset(record, chunk.ordinal);
  body.ordinal = chunk.ordinal;
  body.chunk_count = static_cast<u32>(record.chunks.size());
  body.byte_count = chunk.byte_count;
  body.checksum = chunk.checksum;
  body.content_hash = record.content_hash;
  body.payload_fragment = chunk.payload_fragment;

  std::vector<byte> serialized(static_cast<std::size_t>(body_capacity), 0);
  std::copy(kOverflowChunkPageBodyMagic.begin(),
            kOverflowChunkPageBodyMagic.end(),
            serialized.begin() + kOffsetMagic);
  Store32(&serialized, kOffsetVersion, kOverflowChunkPageBodyVersion);
  Store32(&serialized, kOffsetState, static_cast<u32>(body.state));
  Store32(&serialized, kOffsetOrdinal, body.ordinal);
  Store32(&serialized, kOffsetChunkCount, body.chunk_count);
  Store32(&serialized, kOffsetByteCount, body.byte_count);
  Store32(&serialized, kOffsetContentHashBytes, content_hash_bytes);
  Store64(&serialized, kOffsetGeneration, body.generation);
  Store64(&serialized, kOffsetLocalTransactionId, body.local_transaction_id);
  Store64(&serialized, kOffsetByteOffset, body.byte_offset);
  Store64(&serialized, kOffsetChecksum, body.checksum);
  StoreUuid(&serialized, kOffsetOverflowValueUuid, body.overflow_value_uuid);
  StoreUuid(&serialized, kOffsetChunkUuid, body.chunk_uuid);
  StoreUuid(&serialized, kOffsetPageUuid, body.page_uuid);
  StoreUuid(&serialized, kOffsetRowUuid, body.row_uuid);
  StoreUuid(&serialized, kOffsetObjectUuid, body.object_uuid);
  StoreUuid(&serialized, kOffsetTransactionUuid, body.transaction_uuid);
  StoreUuid(&serialized, kOffsetChunkPolicyUuid, body.chunk_policy_uuid);
  std::copy(body.content_hash.begin(),
            body.content_hash.end(),
            serialized.begin() + kOffsetVariablePayload);
  std::copy(body.payload_fragment.begin(),
            body.payload_fragment.end(),
            serialized.begin() + kOffsetVariablePayload + content_hash_bytes);

  OverflowChunkPageBodyResult result;
  result.status = OverflowOkStatus();
  result.body = std::move(body);
  result.serialized = std::move(serialized);
  return result;
}

OverflowChunkPageBodyResult ParseOverflowChunkPageBody(const std::vector<byte>& serialized) {
  if (serialized.size() < kOffsetVariablePayload) {
    return ChunkPageBodyError("overflow_chunk_page_body_too_short",
                              "storage.page.overflow.chunk_page_body_too_short",
                              std::to_string(serialized.size()));
  }
  if (!std::equal(kOverflowChunkPageBodyMagic.begin(),
                  kOverflowChunkPageBodyMagic.end(),
                  serialized.begin() + kOffsetMagic)) {
    return ChunkPageBodyError("overflow_chunk_page_body_magic_invalid",
                              "storage.page.overflow.chunk_page_body_magic_invalid");
  }
  if (Load32(serialized, kOffsetVersion) != kOverflowChunkPageBodyVersion) {
    return ChunkPageBodyError("overflow_chunk_page_body_version_invalid",
                              "storage.page.overflow.chunk_page_body_version_invalid");
  }

  OverflowChunkPageBody body;
  body.state = static_cast<OverflowValueState>(Load32(serialized, kOffsetState));
  body.ordinal = Load32(serialized, kOffsetOrdinal);
  body.chunk_count = Load32(serialized, kOffsetChunkCount);
  body.byte_count = Load32(serialized, kOffsetByteCount);
  const u32 content_hash_bytes = Load32(serialized, kOffsetContentHashBytes);
  body.generation = Load64(serialized, kOffsetGeneration);
  body.local_transaction_id = Load64(serialized, kOffsetLocalTransactionId);
  body.byte_offset = Load64(serialized, kOffsetByteOffset);
  body.checksum = Load64(serialized, kOffsetChecksum);
  body.overflow_value_uuid = LoadUuid(serialized, kOffsetOverflowValueUuid, UuidKind::object);
  body.chunk_uuid = LoadUuid(serialized, kOffsetChunkUuid, UuidKind::object);
  body.page_uuid = LoadUuid(serialized, kOffsetPageUuid, UuidKind::page);
  body.row_uuid = LoadUuid(serialized, kOffsetRowUuid, UuidKind::row);
  body.object_uuid = LoadUuid(serialized, kOffsetObjectUuid, UuidKind::object);
  body.transaction_uuid = LoadUuid(serialized, kOffsetTransactionUuid, UuidKind::transaction);
  body.chunk_policy_uuid = LoadUuid(serialized, kOffsetChunkPolicyUuid, UuidKind::object);

  const u64 required_bytes = static_cast<u64>(kOffsetVariablePayload) +
                             content_hash_bytes +
                             body.byte_count;
  if (required_bytes > serialized.size()) {
    return ChunkPageBodyError("overflow_chunk_page_body_bounds_invalid",
                              "storage.page.overflow.chunk_page_body_bounds_invalid",
                              std::to_string(required_bytes) + ":" + std::to_string(serialized.size()));
  }
  body.content_hash.assign(reinterpret_cast<const char*>(serialized.data() + kOffsetVariablePayload),
                           content_hash_bytes);
  body.payload_fragment.assign(serialized.begin() + kOffsetVariablePayload + content_hash_bytes,
                               serialized.begin() + kOffsetVariablePayload + content_hash_bytes + body.byte_count);
  if (ChecksumFragment(body.payload_fragment) != body.checksum) {
    return ChunkPageBodyError("overflow_chunk_page_body_checksum_mismatch",
                              "storage.page.overflow.chunk_page_body_checksum_mismatch");
  }

  OverflowChunkPageBodyResult result;
  result.status = OverflowOkStatus();
  result.body = std::move(body);
  result.serialized = serialized;
  return result;
}

OverflowChunkPageBodyResult ValidateOverflowChunkPageBody(const OverflowValueRecord& record,
                                                          const OverflowChunkRecord& chunk,
                                                          const OverflowChunkPageBody& body) {
  if (!SameUuid(body.overflow_value_uuid, record.overflow_value_uuid) ||
      !SameUuid(body.chunk_uuid, chunk.chunk_uuid) ||
      !SameUuid(body.page_uuid, chunk.page_uuid) ||
      !SameUuid(body.row_uuid, record.row_uuid) ||
      !SameUuid(body.object_uuid, record.object_uuid) ||
      !SameUuid(body.transaction_uuid, record.transaction_uuid) ||
      !SameUuid(body.chunk_policy_uuid, record.chunk_policy_uuid)) {
    return ChunkPageBodyError("overflow_chunk_page_body_identity_mismatch",
                              "storage.page.overflow.chunk_page_body_identity_mismatch");
  }
  if (body.state != record.state ||
      body.generation != record.generation ||
      body.local_transaction_id != record.local_transaction_id ||
      body.ordinal != chunk.ordinal ||
      body.chunk_count != record.chunks.size() ||
      body.byte_count != chunk.byte_count ||
      body.byte_offset != ChunkByteOffset(record, chunk.ordinal) ||
      body.content_hash != record.content_hash) {
    return ChunkPageBodyError("overflow_chunk_page_body_metadata_mismatch",
                              "storage.page.overflow.chunk_page_body_metadata_mismatch");
  }
  if (body.payload_fragment != chunk.payload_fragment ||
      body.checksum != chunk.checksum ||
      ChecksumFragment(body.payload_fragment) != body.checksum) {
    return ChunkPageBodyError("overflow_chunk_page_body_payload_mismatch",
                              "storage.page.overflow.chunk_page_body_payload_mismatch");
  }

  OverflowChunkPageBodyResult result;
  result.status = OverflowOkStatus();
  result.body = body;
  return result;
}

OverflowBlobPageResult WriteOverflowValueBlobPages(
    const OverflowBlobPageWriteRequest& request) {
  if (!ValidBlobPageContext(request.device,
                            request.database_uuid,
                            request.filespace_uuid,
                            request.page_size)) {
    return BlobPageError("overflow_blob_page_write_context_invalid",
                         "storage.page.overflow.blob_page_write_context_invalid");
  }
  if (request.device->read_only()) {
    return BlobPageError("overflow_blob_page_write_read_only",
                         "storage.page.overflow.blob_page_write_read_only");
  }
  if (request.first_page_number == 0 || request.page_generation == 0 ||
      request.record.chunks.empty()) {
    return BlobPageError("overflow_blob_page_write_location_invalid",
                         "storage.page.overflow.blob_page_write_location_invalid");
  }
  if (request.record.state != OverflowValueState::durable_uncommitted &&
      request.record.state != OverflowValueState::committed_visible) {
    return BlobPageError("overflow_blob_page_write_state_invalid",
                         "storage.page.overflow.blob_page_write_state_invalid",
                         OverflowValueStateName(request.record.state));
  }
  if (!ValidateOverflowChunkChain(request.record, nullptr)) {
    return BlobPageError("overflow_blob_page_write_chain_invalid",
                         "storage.page.overflow.blob_page_write_chain_invalid");
  }

  OverflowBlobPageResult result;
  result.status = OverflowOkStatus();
  result.record = request.record;
  for (auto& chunk : result.record.chunks) {
    if (AddWouldOverflow(request.first_page_number, chunk.ordinal)) {
      return BlobPageError("overflow_blob_page_write_page_number_overflow",
                           "storage.page.overflow.blob_page_write_page_number_overflow");
    }
    chunk.page_number = request.first_page_number + chunk.ordinal;
    chunk.page_generation = request.page_generation;
  }

  const PageManagerContext context =
      BlobPageManagerContext(request.database_uuid,
                             request.filespace_uuid,
                             request.page_size);
  for (const auto& chunk : result.record.chunks) {
    const auto body =
        BuildOverflowChunkPageBody(result.record, chunk, request.page_size);
    if (!body.ok()) {
      return BlobPageError("overflow_blob_page_write_body_invalid",
                           "storage.page.overflow.blob_page_write_body_invalid",
                           body.diagnostic.diagnostic_code);
    }

    ManagedPageHeaderRequest header_request;
    header_request.context = context;
    header_request.page_type = scratchbird::storage::disk::PageType::blob;
    header_request.page_uuid = chunk.page_uuid;
    header_request.page_number = chunk.page_number;
    header_request.page_generation = chunk.page_generation;
    const auto header = BuildManagedPageHeader(header_request);
    if (!header.ok()) {
      return BlobPageError("overflow_blob_page_write_header_invalid",
                           "storage.page.overflow.blob_page_write_header_invalid",
                           header.diagnostic.diagnostic_code);
    }

    PageBodyAgreementRequest agreement;
    agreement.header = header.serialized;
    agreement.body = body.serialized;
    agreement.checksum_profile = PageBodyChecksumProfile::strong;
    const auto agreed = ValidatePageBodyAgreement(agreement);
    if (!agreed.ok() || agreed.body_kind != PageBodyKind::blob_overflow ||
        !agreed.production_admitted) {
      return BlobPageError("overflow_blob_page_write_agreement_refused",
                           "storage.page.overflow.blob_page_write_agreement_refused",
                           agreed.diagnostic.diagnostic_code);
    }

    auto page_buffer = AllocateManagedPageBuffer(context,
                                                 scratchbird::storage::disk::PageType::blob,
                                                 "overflow_blob_page_write_buffer");
    if (!page_buffer.ok()) {
      return BlobPageError("overflow_blob_page_write_buffer_failed",
                           "storage.page.overflow.blob_page_write_buffer_failed",
                           page_buffer.diagnostic.diagnostic_code);
    }
    auto* page_bytes = static_cast<byte*>(page_buffer.buffer.data());
    const auto page_bytes_size = page_buffer.buffer.size();
    if (page_bytes_size != request.page_size) {
      return BlobPageError("overflow_blob_page_write_buffer_size_mismatch",
                           "storage.page.overflow.blob_page_write_buffer_size_mismatch");
    }
    std::copy(header.serialized.begin(),
              header.serialized.end(),
              page_bytes);
    std::copy(body.serialized.begin(),
              body.serialized.end(),
              page_bytes + kPageHeaderSerializedBytes);

    const auto page_offset =
        CheckedPageOffset(request.page_size, chunk.page_number);
    if (!page_offset.ok()) {
      return BlobPageError("overflow_blob_page_write_offset_invalid",
                           "storage.page.overflow.blob_page_write_offset_invalid",
                           page_offset.diagnostic.diagnostic_code);
    }
    const auto write = request.device->WriteAt(page_offset.offset,
                                               page_bytes,
                                               page_bytes_size);
    if (!write.ok() || write.bytes_transferred != page_bytes_size) {
      return BlobPageError("overflow_blob_page_write_failed",
                           "storage.page.overflow.blob_page_write_failed",
                           write.diagnostic.diagnostic_code);
    }
    result.locations.push_back(LocationFor(chunk));
  }

  if (request.sync_after_write) {
    const auto sync = request.device->Sync();
    if (!sync.ok()) {
      return BlobPageError("overflow_blob_page_write_sync_failed",
                           "storage.page.overflow.blob_page_write_sync_failed",
                           sync.diagnostic.diagnostic_code);
    }
  }

  result.completed = true;
  result.page_count = result.locations.size();
  return result;
}

OverflowBlobPageResult ReadOverflowValueBlobPages(
    const OverflowBlobPageReadRequest& request) {
  if (!ValidBlobPageContext(request.device,
                            request.database_uuid,
                            request.filespace_uuid,
                            request.page_size)) {
    return BlobPageError("overflow_blob_page_read_context_invalid",
                         "storage.page.overflow.blob_page_read_context_invalid");
  }
  if (request.record.chunks.empty()) {
    return BlobPageError("overflow_blob_page_read_chain_empty",
                         "storage.page.overflow.blob_page_read_chain_empty");
  }
  if (!ValidateOverflowChunkChain(request.record, nullptr)) {
    return BlobPageError("overflow_blob_page_read_chain_invalid",
                         "storage.page.overflow.blob_page_read_chain_invalid");
  }

  OverflowBlobPageResult result;
  result.status = OverflowOkStatus();
  result.record = request.record;
  const PageManagerContext context =
      BlobPageManagerContext(request.database_uuid,
                             request.filespace_uuid,
                             request.page_size);

  for (const auto& chunk : request.record.chunks) {
    if (chunk.page_number == 0 || chunk.page_generation == 0) {
      return BlobPageError("overflow_blob_page_read_location_missing",
                           "storage.page.overflow.blob_page_read_location_missing");
    }
    const auto page_offset =
        CheckedPageOffset(request.page_size, chunk.page_number);
    if (!page_offset.ok()) {
      return BlobPageError("overflow_blob_page_read_offset_invalid",
                           "storage.page.overflow.blob_page_read_offset_invalid",
                           page_offset.diagnostic.diagnostic_code);
    }

    auto page_buffer = AllocateManagedPageBuffer(context,
                                                 scratchbird::storage::disk::PageType::blob,
                                                 "overflow_blob_page_read_buffer");
    if (!page_buffer.ok()) {
      return BlobPageError("overflow_blob_page_read_buffer_failed",
                           "storage.page.overflow.blob_page_read_buffer_failed",
                           page_buffer.diagnostic.diagnostic_code);
    }
    auto* page_bytes = static_cast<byte*>(page_buffer.buffer.data());
    const auto page_bytes_size = page_buffer.buffer.size();
    if (page_bytes_size != request.page_size) {
      return BlobPageError("overflow_blob_page_read_buffer_size_mismatch",
                           "storage.page.overflow.blob_page_read_buffer_size_mismatch");
    }
    const auto read = request.device->ReadAt(page_offset.offset,
                                             page_bytes,
                                             page_bytes_size);
    if (!read.ok() || read.bytes_transferred != page_bytes_size) {
      return BlobPageError("overflow_blob_page_read_failed",
                           "storage.page.overflow.blob_page_read_failed",
                           read.diagnostic.diagnostic_code);
    }

    scratchbird::storage::disk::SerializedPageHeader serialized_header{};
    std::copy(page_bytes,
              page_bytes + kPageHeaderSerializedBytes,
              serialized_header.begin());
    const auto header = ValidateManagedPageHeader(context, serialized_header);
    if (!header.ok()) {
      return BlobPageError("overflow_blob_page_read_header_invalid",
                           "storage.page.overflow.blob_page_read_header_invalid",
                           header.diagnostic.diagnostic_code);
    }
    if (header.header.page_type != scratchbird::storage::disk::PageType::blob ||
        header.header.page_uuid != chunk.page_uuid.value ||
        header.header.page_number != chunk.page_number ||
        header.header.page_generation != chunk.page_generation) {
      return BlobPageError("overflow_blob_page_read_header_mismatch",
                           "storage.page.overflow.blob_page_read_header_mismatch");
    }

    std::vector<byte> body_bytes(
        page_bytes + kPageHeaderSerializedBytes,
        page_bytes + page_bytes_size);
    PageBodyAgreementRequest agreement;
    agreement.header = serialized_header;
    agreement.body = body_bytes;
    agreement.checksum_profile = PageBodyChecksumProfile::strong;
    const auto agreed = ValidatePageBodyAgreement(agreement);
    if (!agreed.ok() || agreed.body_kind != PageBodyKind::blob_overflow) {
      return BlobPageError("overflow_blob_page_read_agreement_refused",
                           "storage.page.overflow.blob_page_read_agreement_refused",
                           agreed.diagnostic.diagnostic_code);
    }

    const auto parsed = ParseOverflowChunkPageBody(body_bytes);
    if (!parsed.ok()) {
      return BlobPageError("overflow_blob_page_read_body_invalid",
                           "storage.page.overflow.blob_page_read_body_invalid",
                           parsed.diagnostic.diagnostic_code);
    }
    const auto validated =
        ValidateOverflowChunkPageBody(request.record, chunk, parsed.body);
    if (!validated.ok()) {
      return BlobPageError("overflow_blob_page_read_body_mismatch",
                           "storage.page.overflow.blob_page_read_body_mismatch",
                           validated.diagnostic.diagnostic_code);
    }
    result.payload_bytes.insert(result.payload_bytes.end(),
                                parsed.body.payload_fragment.begin(),
                                parsed.body.payload_fragment.end());
    result.locations.push_back(LocationFor(chunk));
  }

  if (HashPayload(result.payload_bytes) != request.record.content_hash) {
    return BlobPageError("overflow_blob_page_read_hash_mismatch",
                         "storage.page.overflow.blob_page_read_hash_mismatch");
  }
  result.completed = true;
  result.page_count = result.locations.size();
  return result;
}

OverflowBlobPageResult ReclaimOverflowValueBlobPages(
    const OverflowBlobPageReclaimRequest& request) {
  if (!ValidBlobPageContext(request.device,
                            request.database_uuid,
                            request.filespace_uuid,
                            request.page_size)) {
    return BlobPageError("overflow_blob_page_reclaim_context_invalid",
                         "storage.page.overflow.blob_page_reclaim_context_invalid");
  }
  if (request.device->read_only()) {
    return BlobPageError("overflow_blob_page_reclaim_read_only",
                         "storage.page.overflow.blob_page_reclaim_read_only");
  }
  if (!request.cleanup_horizon_authoritative ||
      request.authoritative_cleanup_horizon_local_transaction_id == 0 ||
      request.authoritative_cleanup_horizon_local_transaction_id <=
          request.record.local_transaction_id) {
    return BlobPageError("overflow_blob_page_reclaim_horizon_required",
                         "storage.page.overflow.blob_page_reclaim_horizon_required");
  }
  if (request.record.state != OverflowValueState::rolled_back &&
      request.record.state != OverflowValueState::committed_visible) {
    return BlobPageError("overflow_blob_page_reclaim_state_invalid",
                         "storage.page.overflow.blob_page_reclaim_state_invalid",
                         OverflowValueStateName(request.record.state));
  }
  if (request.record.chunks.empty() ||
      !ValidateOverflowChunkChain(request.record, nullptr)) {
    return BlobPageError("overflow_blob_page_reclaim_chain_invalid",
                         "storage.page.overflow.blob_page_reclaim_chain_invalid");
  }

  OverflowBlobPageResult result;
  result.status = OverflowOkStatus();
  result.record = request.record;
  std::vector<byte> zero_page(request.page_size, 0);
  for (const auto& chunk : request.record.chunks) {
    if (chunk.page_number == 0 || chunk.page_generation == 0) {
      return BlobPageError("overflow_blob_page_reclaim_location_missing",
                           "storage.page.overflow.blob_page_reclaim_location_missing");
    }
    const auto page_offset =
        CheckedPageOffset(request.page_size, chunk.page_number);
    if (!page_offset.ok()) {
      return BlobPageError("overflow_blob_page_reclaim_offset_invalid",
                           "storage.page.overflow.blob_page_reclaim_offset_invalid",
                           page_offset.diagnostic.diagnostic_code);
    }
    const auto write = request.device->WriteAt(page_offset.offset,
                                               zero_page.data(),
                                               zero_page.size());
    if (!write.ok() || write.bytes_transferred != zero_page.size()) {
      return BlobPageError("overflow_blob_page_reclaim_write_failed",
                           "storage.page.overflow.blob_page_reclaim_write_failed",
                           write.diagnostic.diagnostic_code);
    }
    result.locations.push_back(LocationFor(chunk));
  }
  if (request.sync_after_write) {
    const auto sync = request.device->Sync();
    if (!sync.ok()) {
      return BlobPageError("overflow_blob_page_reclaim_sync_failed",
                           "storage.page.overflow.blob_page_reclaim_sync_failed",
                           sync.diagnostic.diagnostic_code);
    }
  }
  result.record.state = OverflowValueState::cleanup_reclaimed;
  result.record.chunks.clear();
  result.completed = true;
  result.page_count = result.locations.size();
  return result;
}

const OverflowValueRecord* FindOverflowValue(const OverflowLedger& ledger, const TypedUuid& overflow_value_uuid) {
  const auto found = std::find_if(ledger.values.begin(),
                                  ledger.values.end(),
                                  [&](const OverflowValueRecord& record) {
                                    return SameUuid(record.overflow_value_uuid, overflow_value_uuid);
                                  });
  return found == ledger.values.end() ? nullptr : &(*found);
}

DiagnosticRecord MakeOverflowPersistenceDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail) {
  std::vector<scratchbird::core::platform::DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return scratchbird::core::platform::MakeDiagnostic(status.code,
                                                     status.severity,
                                                     status.subsystem,
                                                     std::move(diagnostic_code),
                                                     std::move(message_key),
                                                     std::move(arguments),
                                                     {},
                                                     "storage.page.overflow_persistence",
                                                     status.ok() ? "" : "keep oversized row on safe refusal path or retry after overflow authority is available");
}

}  // namespace scratchbird::storage::page
