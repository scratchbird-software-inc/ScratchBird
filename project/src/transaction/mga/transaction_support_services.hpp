// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "transaction_inventory.hpp"

#include <optional>
#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

struct TransactionSupportEvidenceField {
  std::string key;
  std::string value;
};

struct TransactionCommitFenceCoalescingRequest {
  LocalTransactionInventory inventory;
  bool inventory_authoritative = false;
  bool policy_enabled = false;
  bool policy_required = false;
  bool allow_commit_fence_coalescing = false;
  u64 fence_generation = 0;
  u64 max_transactions_per_fence = 0;
  std::vector<LocalTransactionId> local_transaction_ids;
};

struct TransactionCommitFenceCoalescingResult {
  Status status;
  bool accepted = false;
  bool fallback_to_individual_fences = false;
  bool policy_required = false;
  u64 coalesced_fence_count = 0;
  u64 input_transaction_count = 0;
  bool durable_inventory_remains_authority = true;
  bool coalesced_fence_is_finality_authority = false;
  std::string refusal_reason;
  std::vector<TransactionSupportEvidenceField> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && accepted;
  }
};

enum class SavepointStateDeltaState : u16 {
  active,
  rolled_back
};

struct SavepointStateDelta {
  LocalTransactionId local_transaction_id;
  u64 savepoint_ordinal = 0;
  u64 mutation_sequence = 0;
  std::string object_uuid;
  std::string record_uuid;
  std::string before_image_hash;
  std::string after_image_hash;
  bool durable_evidence_written = false;
  SavepointStateDeltaState state = SavepointStateDeltaState::active;
};

struct SavepointStateDeltaResult {
  Status status;
  SavepointStateDelta delta;
  u64 active_delta_count = 0;
  u64 rolled_back_delta_count = 0;
  bool durable_inventory_remains_authority = true;
  bool savepoint_delta_is_transaction_authority = false;
  std::vector<TransactionSupportEvidenceField> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct SavepointStateDeltaRollbackResult {
  Status status;
  u64 affected_delta_count = 0;
  u64 active_delta_count = 0;
  u64 rolled_back_delta_count = 0;
  bool transaction_identity_unchanged = true;
  bool durable_inventory_remains_authority = true;
  bool savepoint_delta_is_transaction_authority = false;
  std::vector<TransactionSupportEvidenceField> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

class SavepointStateDeltaManager {
 public:
  SavepointStateDeltaResult RecordDelta(SavepointStateDelta delta);
  SavepointStateDeltaRollbackResult RollbackToSavepoint(LocalTransactionId local_transaction_id,
                                                        u64 savepoint_ordinal);
  std::optional<SavepointStateDelta> LatestActiveDelta(LocalTransactionId local_transaction_id,
                                                       const std::string& record_uuid) const;
  u64 ActiveDeltaCount(LocalTransactionId local_transaction_id) const;
  u64 RolledBackDeltaCount(LocalTransactionId local_transaction_id) const;

 private:
  std::vector<SavepointStateDelta> deltas_;
};

struct TransactionSnapshotTemplateStaticInputs {
  std::string isolation_profile;
  u64 catalog_epoch = 0;
  u64 security_epoch = 0;
  u64 policy_epoch = 0;
  u64 metadata_epoch = 0;
  bool allow_reader_own_uncommitted = true;
};

struct TransactionSnapshotTemplate {
  TransactionSnapshotTemplateStaticInputs static_inputs;
  u64 cache_generation = 0;
  u64 invalidation_generation = 0;
  u64 inventory_generation = 0;
  LocalTransactionId next_local_transaction_id;
  LocalTransactionId visible_through_local_transaction_id;
  bool static_inputs_only = true;
  bool active_transaction_set_cached = false;
  bool durable_inventory_remains_authority = true;
};

struct TransactionSnapshotTemplateCacheRequest {
  bool inventory_authoritative = false;
  u64 inventory_generation = 0;
  TransactionSnapshotTemplateStaticInputs static_inputs;
  LocalTransactionId next_local_transaction_id;
  LocalTransactionId visible_through_local_transaction_id;
};

struct TransactionSnapshotTemplateCacheResult {
  Status status;
  bool accepted = false;
  bool cache_hit = false;
  std::string refusal_reason;
  TransactionSnapshotTemplate snapshot_template;
  bool durable_inventory_remains_authority = true;
  bool template_cache_is_visibility_authority = false;
  std::vector<TransactionSupportEvidenceField> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && accepted;
  }
};

class TransactionSnapshotTemplateCache {
 public:
  TransactionSnapshotTemplateCache(u64 cache_generation, u64 invalidation_generation);

  TransactionSnapshotTemplateCacheResult Store(const TransactionSnapshotTemplateCacheRequest& request);
  TransactionSnapshotTemplateCacheResult Lookup(const TransactionSnapshotTemplateCacheRequest& request) const;
  void Invalidate(u64 invalidation_generation);

  u64 cache_generation() const {
    return cache_generation_;
  }

  u64 invalidation_generation() const {
    return invalidation_generation_;
  }

 private:
  u64 cache_generation_ = 1;
  u64 invalidation_generation_ = 1;
  std::vector<TransactionSnapshotTemplate> templates_;
};

struct MgaVisibilityAccelerationEntry {
  LocalTransactionId local_transaction_id;
  TransactionState state = TransactionState::none;
  bool evidence_record_written = false;
  u64 final_unix_epoch_millis = 0;
};

struct MgaVisibilityAccelerationTable {
  u64 table_generation = 0;
  u64 inventory_generation = 0;
  bool built_from_authoritative_inventory = false;
  bool durable_inventory_remains_authority = true;
  bool acceleration_table_is_finality_authority = false;
  std::vector<MgaVisibilityAccelerationEntry> entries;
};

struct MgaVisibilityAccelerationBuildRequest {
  LocalTransactionInventory inventory;
  bool inventory_authoritative = false;
  u64 inventory_generation = 0;
  u64 table_generation = 0;
};

struct MgaVisibilityAccelerationBuildResult {
  Status status;
  bool accepted = false;
  std::string refusal_reason;
  MgaVisibilityAccelerationTable table;
  std::vector<TransactionSupportEvidenceField> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && accepted;
  }
};

struct MgaVisibilityAccelerationProbeRequest {
  MgaVisibilityAccelerationTable table;
  LocalTransactionInventory current_inventory;
  bool current_inventory_authoritative = false;
  u64 inventory_generation = 0;
  LocalTransactionId first_local_transaction_id;
  LocalTransactionId last_local_transaction_id;
};

struct MgaVisibilityAccelerationProbeResult {
  Status status;
  bool accepted = false;
  bool all_committed = false;
  std::string refusal_reason;
  bool durable_inventory_remains_authority = true;
  bool acceleration_table_is_finality_authority = false;
  std::vector<TransactionSupportEvidenceField> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && accepted;
  }
};

struct ReadAfterWriteOverlayEntry {
  LocalTransactionId local_transaction_id;
  u64 savepoint_ordinal = 0;
  u64 mutation_sequence = 0;
  std::string object_uuid;
  std::string record_uuid;
  std::string value_hash;
  bool delete_marker = false;
  bool durable_evidence_written = false;
  bool visible_to_own_transaction = true;
  bool visible_to_other_transactions = false;
  bool overlay_is_transaction_finality_authority = false;
};

struct ReadAfterWriteOverlayResult {
  Status status;
  bool accepted = false;
  bool cache_hit = false;
  std::string refusal_reason;
  ReadAfterWriteOverlayEntry entry;
  bool durable_inventory_remains_authority = true;
  bool overlay_is_transaction_finality_authority = false;
  std::vector<TransactionSupportEvidenceField> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && accepted;
  }
};

struct ReadAfterWriteOverlayRollbackResult {
  Status status;
  u64 removed_entry_count = 0;
  bool durable_inventory_remains_authority = true;
  bool overlay_is_transaction_finality_authority = false;
  std::vector<TransactionSupportEvidenceField> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

class ReadAfterWriteLocalOverlayCache {
 public:
  ReadAfterWriteOverlayResult Put(ReadAfterWriteOverlayEntry entry);
  ReadAfterWriteOverlayResult Lookup(LocalTransactionId local_transaction_id,
                                     const std::string& record_uuid) const;
  ReadAfterWriteOverlayRollbackResult RollbackToSavepoint(LocalTransactionId local_transaction_id,
                                                          u64 savepoint_ordinal);
  u64 size() const;

 private:
  std::vector<ReadAfterWriteOverlayEntry> entries_;
};

struct TransactionStatementId {
  LocalTransactionId local_transaction_id;
  u64 statement_sequence = 0;
};

struct TransactionStatementIdReservationRequest {
  LocalTransactionInventory inventory;
  bool inventory_authoritative = false;
  LocalTransactionId local_transaction_id;
  TypedUuid transaction_uuid;
  u64 statement_count = 1;
};

struct TransactionStatementIdReservationResult {
  Status status;
  bool accepted = false;
  std::string refusal_reason;
  std::vector<TransactionStatementId> statement_ids;
  u64 next_statement_sequence = 0;
  bool durable_inventory_remains_authority = true;
  bool statement_id_is_transaction_finality_authority = false;
  std::vector<TransactionSupportEvidenceField> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && accepted;
  }
};

class TransactionStatementIdReservationService {
 public:
  TransactionStatementIdReservationResult Reserve(
      const TransactionStatementIdReservationRequest& request);
  void Clear(LocalTransactionId local_transaction_id);

 private:
  struct Counter {
    LocalTransactionId local_transaction_id;
    u64 next_statement_sequence = 1;
  };

  std::vector<Counter> counters_;
};

const char* SavepointStateDeltaStateName(SavepointStateDeltaState state);
TransactionCommitFenceCoalescingResult PlanTransactionCommitFenceCoalescing(
    const TransactionCommitFenceCoalescingRequest& request);
MgaVisibilityAccelerationBuildResult BuildMgaVisibilityAccelerationTable(
    const MgaVisibilityAccelerationBuildRequest& request);
MgaVisibilityAccelerationProbeResult ProbeMgaVisibilityAccelerationTable(
    const MgaVisibilityAccelerationProbeRequest& request);
DiagnosticRecord MakeTransactionSupportDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail = {});

}  // namespace scratchbird::transaction::mga
