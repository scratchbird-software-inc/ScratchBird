// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-OVERFLOW-PERSISTENCE-ANCHOR
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::disk {
class FileDevice;
}  // namespace scratchbird::storage::disk

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class OverflowValueState : u32 {
  absent,
  durable_uncommitted,
  committed_visible,
  rolled_back,
  cleanup_reclaimed,
  quarantine
};

enum class OverflowRecoveryAction : u32 {
  no_action,
  retain,
  roll_back,
  reclaim_after_horizon,
  fail_closed
};

struct OverflowPersistRequest {
  TypedUuid row_uuid;
  TypedUuid object_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  u64 generation = 1;
  std::string value_descriptor;
  std::vector<byte> payload_bytes;
  TypedUuid chunk_policy_uuid;
  u32 chunk_size = 4096;
};

struct OverflowChunkRecord {
  TypedUuid chunk_uuid;
  TypedUuid page_uuid;
  u64 page_number = 0;
  u64 page_generation = 0;
  u32 ordinal = 0;
  u32 byte_count = 0;
  std::vector<byte> payload_fragment;
  u64 checksum = 0;
};

struct OverflowValueRecord {
  TypedUuid overflow_value_uuid;
  TypedUuid row_uuid;
  TypedUuid object_uuid;
  TypedUuid transaction_uuid;
  TypedUuid chunk_policy_uuid;
  u64 local_transaction_id = 0;
  u64 generation = 1;
  std::string value_descriptor;
  std::string content_hash;
  OverflowValueState state = OverflowValueState::absent;
  std::vector<OverflowChunkRecord> chunks;
};

struct OverflowChunkPageBody {
  TypedUuid overflow_value_uuid;
  TypedUuid chunk_uuid;
  TypedUuid page_uuid;
  TypedUuid row_uuid;
  TypedUuid object_uuid;
  TypedUuid transaction_uuid;
  TypedUuid chunk_policy_uuid;
  OverflowValueState state = OverflowValueState::absent;
  u64 generation = 0;
  u64 local_transaction_id = 0;
  u64 byte_offset = 0;
  u32 ordinal = 0;
  u32 chunk_count = 0;
  u32 byte_count = 0;
  u64 checksum = 0;
  std::string content_hash;
  std::vector<byte> payload_fragment;
};

struct OverflowChunkPageBodyResult {
  Status status;
  OverflowChunkPageBody body;
  std::vector<byte> serialized;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct OverflowEvidenceRecord {
  u64 sequence = 0;
  std::string action;
  TypedUuid evidence_id;
  TypedUuid overflow_value_uuid;
  TypedUuid row_uuid;
  TypedUuid object_uuid;
  TypedUuid transaction_uuid;
  TypedUuid chunk_policy_uuid;
  u64 local_transaction_id = 0;
  u32 chunk_count = 0;
  u64 payload_bytes = 0;
  std::string content_hash;
  OverflowValueState previous_state = OverflowValueState::absent;
  OverflowValueState new_state = OverflowValueState::absent;
  u64 authoritative_cleanup_horizon_local_transaction_id = 0;
  std::string reason;
  std::string diagnostic_code;
  bool durable_state_changed = false;
};

struct OverflowLedger {
  std::vector<OverflowValueRecord> values;
  std::vector<OverflowEvidenceRecord> evidence;
  u64 next_evidence_sequence = 1;
};

struct OverflowPersistResult {
  Status status;
  bool persisted = false;
  TypedUuid overflow_value_uuid;
  u32 chunk_count = 0;
  TypedUuid first_page_uuid;
  std::string content_hash;
  OverflowValueRecord record;
  OverflowEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && persisted; }
};

struct OverflowReadRequest {
  TypedUuid overflow_value_uuid;
  bool include_uncommitted_own_transaction = false;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
};

struct OverflowReadResult {
  Status status;
  bool found = false;
  bool visible = false;
  std::vector<byte> payload_bytes;
  std::string content_hash;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && found && visible; }
};

struct OverflowRollbackRequest {
  TypedUuid overflow_value_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  std::string reason;
};

struct OverflowMutationResult {
  Status status;
  bool changed = false;
  OverflowValueRecord record;
  OverflowEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && changed; }
};

struct OverflowCommitRequest {
  TypedUuid overflow_value_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  std::string reason;
};

struct OverflowCleanupRequest {
  u64 authoritative_cleanup_horizon_local_transaction_id = 0;
  bool cleanup_horizon_authoritative = false;
  std::string reason;
};

struct OverflowCleanupResult {
  Status status;
  bool cleaned = false;
  u64 cleaned_count = 0;
  u64 retained_count = 0;
  OverflowEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && cleaned; }
};

struct OverflowRecoveryClassification {
  TypedUuid overflow_value_uuid;
  OverflowValueState observed_state = OverflowValueState::absent;
  OverflowRecoveryAction action = OverflowRecoveryAction::fail_closed;
  bool fail_closed = false;
  std::string stable_reason;
};

struct OverflowRecoveryResult {
  Status status;
  std::vector<OverflowRecoveryClassification> classifications;
  DiagnosticRecord diagnostic;
  bool write_admission_must_remain_fenced = false;
  bool durable_state_changed = false;

  bool ok() const { return status.ok(); }
};

struct OverflowBlobPageLocation {
  TypedUuid chunk_uuid;
  TypedUuid page_uuid;
  u64 page_number = 0;
  u64 page_generation = 0;
  u32 ordinal = 0;
  u32 byte_count = 0;
};

struct OverflowBlobPageWriteRequest {
  scratchbird::storage::disk::FileDevice* device = nullptr;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  OverflowValueRecord record;
  u32 page_size = 0;
  u64 first_page_number = 0;
  u64 page_generation = 1;
  bool sync_after_write = true;
};

struct OverflowBlobPageReadRequest {
  scratchbird::storage::disk::FileDevice* device = nullptr;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  OverflowValueRecord record;
  u32 page_size = 0;
};

struct OverflowBlobPageReclaimRequest {
  scratchbird::storage::disk::FileDevice* device = nullptr;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  OverflowValueRecord record;
  u32 page_size = 0;
  u64 authoritative_cleanup_horizon_local_transaction_id = 0;
  bool cleanup_horizon_authoritative = false;
  bool sync_after_write = true;
};

struct OverflowBlobPageResult {
  Status status;
  bool completed = false;
  OverflowValueRecord record;
  std::vector<byte> payload_bytes;
  std::vector<OverflowBlobPageLocation> locations;
  u64 page_count = 0;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && completed; }
};

const char* OverflowValueStateName(OverflowValueState state);
const char* OverflowRecoveryActionName(OverflowRecoveryAction action);

OverflowPersistResult PersistOverflowValue(OverflowLedger* ledger, const OverflowPersistRequest& request);
OverflowReadResult ReadOverflowValue(const OverflowLedger& ledger, const OverflowReadRequest& request);
OverflowMutationResult CommitOverflowValue(OverflowLedger* ledger, const OverflowCommitRequest& request);
OverflowMutationResult RollbackOverflowValue(OverflowLedger* ledger, const OverflowRollbackRequest& request);
OverflowCleanupResult CleanupOverflowValues(OverflowLedger* ledger, const OverflowCleanupRequest& request);
OverflowRecoveryResult ClassifyOverflowLedgerForRecovery(const OverflowLedger& ledger);
OverflowRecoveryResult ApplyOverflowLedgerRecovery(OverflowLedger* ledger);
const OverflowValueRecord* FindOverflowValue(const OverflowLedger& ledger, const TypedUuid& overflow_value_uuid);
OverflowChunkPageBodyResult BuildOverflowChunkPageBody(const OverflowValueRecord& record,
                                                       const OverflowChunkRecord& chunk,
                                                       u32 page_size);
OverflowChunkPageBodyResult ParseOverflowChunkPageBody(const std::vector<byte>& serialized);
OverflowChunkPageBodyResult ValidateOverflowChunkPageBody(const OverflowValueRecord& record,
                                                          const OverflowChunkRecord& chunk,
                                                          const OverflowChunkPageBody& body);
OverflowBlobPageResult WriteOverflowValueBlobPages(const OverflowBlobPageWriteRequest& request);
OverflowBlobPageResult ReadOverflowValueBlobPages(const OverflowBlobPageReadRequest& request);
OverflowBlobPageResult ReclaimOverflowValueBlobPages(const OverflowBlobPageReclaimRequest& request);
DiagnosticRecord MakeOverflowPersistenceDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {});

}  // namespace scratchbird::storage::page
