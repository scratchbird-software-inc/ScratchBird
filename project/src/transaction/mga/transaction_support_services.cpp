// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_support_services.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status SupportOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status SupportErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga};
}

void AddEvidence(std::vector<TransactionSupportEvidenceField>* evidence,
                 std::string key,
                 std::string value) {
  evidence->push_back({std::move(key), std::move(value)});
}

std::vector<TransactionSupportEvidenceField> BaseEvidence(const char* service_name) {
  std::vector<TransactionSupportEvidenceField> evidence;
  AddEvidence(&evidence, "service", service_name);
  AddEvidence(&evidence, "durable_inventory_remains_authority", "true");
  AddEvidence(&evidence, "support_structure_is_finality_authority", "false");
  return evidence;
}

const TransactionInventoryEntry* FindInventoryEntry(const LocalTransactionInventory& inventory,
                                                    LocalTransactionId local_transaction_id) {
  for (const TransactionInventoryEntry& entry : inventory.entries) {
    if (entry.identity.local_id.value == local_transaction_id.value) {
      return &entry;
    }
  }
  return nullptr;
}

bool IsCommittedOrArchived(const TransactionInventoryEntry& entry) {
  return entry.state == TransactionState::committed || entry.state == TransactionState::archived;
}

bool IsStatementReservableState(TransactionState state) {
  return state == TransactionState::active || state == TransactionState::read_only_active;
}

bool SameTransactionUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool HasDuplicateLocalIds(const std::vector<LocalTransactionId>& ids) {
  for (std::size_t i = 0; i < ids.size(); ++i) {
    for (std::size_t j = i + 1; j < ids.size(); ++j) {
      if (ids[i].value == ids[j].value) {
        return true;
      }
    }
  }
  return false;
}

u64 CountDeltas(const std::vector<SavepointStateDelta>& deltas,
                LocalTransactionId local_transaction_id,
                SavepointStateDeltaState state) {
  u64 count = 0;
  for (const SavepointStateDelta& delta : deltas) {
    if (delta.local_transaction_id.value == local_transaction_id.value && delta.state == state) {
      ++count;
    }
  }
  return count;
}

bool SameStaticInputs(const TransactionSnapshotTemplateStaticInputs& left,
                      const TransactionSnapshotTemplateStaticInputs& right) {
  return left.isolation_profile == right.isolation_profile &&
         left.catalog_epoch == right.catalog_epoch &&
         left.security_epoch == right.security_epoch &&
         left.policy_epoch == right.policy_epoch &&
         left.metadata_epoch == right.metadata_epoch &&
         left.allow_reader_own_uncommitted == right.allow_reader_own_uncommitted;
}

bool ValidStaticInputs(const TransactionSnapshotTemplateStaticInputs& inputs) {
  return !inputs.isolation_profile.empty() &&
         inputs.catalog_epoch != 0 &&
         inputs.security_epoch != 0 &&
         inputs.policy_epoch != 0 &&
         inputs.metadata_epoch != 0;
}

bool SameAccelerationEntry(const MgaVisibilityAccelerationEntry& accelerated,
                           const TransactionInventoryEntry& inventory_entry) {
  return accelerated.local_transaction_id.value == inventory_entry.identity.local_id.value &&
         accelerated.state == inventory_entry.state &&
         accelerated.evidence_record_written == inventory_entry.evidence_record_written &&
         accelerated.final_unix_epoch_millis == inventory_entry.final_unix_epoch_millis;
}

const MgaVisibilityAccelerationEntry* FindAccelerationEntry(
    const MgaVisibilityAccelerationTable& table,
    LocalTransactionId local_transaction_id) {
  for (const MgaVisibilityAccelerationEntry& entry : table.entries) {
    if (entry.local_transaction_id.value == local_transaction_id.value) {
      return &entry;
    }
  }
  return nullptr;
}

TransactionCommitFenceCoalescingResult FenceRefusal(
    const TransactionCommitFenceCoalescingRequest& request,
    bool error,
    std::string reason,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  TransactionCommitFenceCoalescingResult result;
  result.status = error ? SupportErrorStatus() : SupportOkStatus();
  result.policy_required = request.policy_required;
  result.input_transaction_count = static_cast<u64>(request.local_transaction_ids.size());
  result.fallback_to_individual_fences = !request.policy_required;
  result.refusal_reason = std::move(reason);
  result.evidence = BaseEvidence("transaction.commit_fence_coalescing");
  AddEvidence(&result.evidence, "accepted", "false");
  AddEvidence(&result.evidence, "fallback_to_individual_fences",
              result.fallback_to_individual_fences ? "true" : "false");
  result.diagnostic = MakeTransactionSupportDiagnostic(result.status,
                                                       std::move(diagnostic_code),
                                                       std::move(message_key),
                                                       std::move(detail));
  return result;
}

SavepointStateDeltaResult SavepointDeltaError(std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail = {}) {
  SavepointStateDeltaResult result;
  result.status = SupportErrorStatus();
  result.evidence = BaseEvidence("transaction.savepoint_state_delta");
  result.diagnostic = MakeTransactionSupportDiagnostic(result.status,
                                                       std::move(diagnostic_code),
                                                       std::move(message_key),
                                                       std::move(detail));
  return result;
}

SavepointStateDeltaRollbackResult SavepointRollbackError(std::string diagnostic_code,
                                                         std::string message_key,
                                                         std::string detail = {}) {
  SavepointStateDeltaRollbackResult result;
  result.status = SupportErrorStatus();
  result.evidence = BaseEvidence("transaction.savepoint_state_delta.rollback");
  result.diagnostic = MakeTransactionSupportDiagnostic(result.status,
                                                       std::move(diagnostic_code),
                                                       std::move(message_key),
                                                       std::move(detail));
  return result;
}

TransactionSnapshotTemplateCacheResult SnapshotTemplateRefusal(
    bool error,
    std::string reason,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  TransactionSnapshotTemplateCacheResult result;
  result.status = error ? SupportErrorStatus() : SupportOkStatus();
  result.refusal_reason = std::move(reason);
  result.evidence = BaseEvidence("transaction.snapshot_template_cache");
  AddEvidence(&result.evidence, "accepted", "false");
  AddEvidence(&result.evidence, "template_static_inputs_only", "true");
  result.diagnostic = MakeTransactionSupportDiagnostic(result.status,
                                                       std::move(diagnostic_code),
                                                       std::move(message_key),
                                                       std::move(detail));
  return result;
}

ReadAfterWriteOverlayResult OverlayRefusal(bool error,
                                           std::string reason,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {}) {
  ReadAfterWriteOverlayResult result;
  result.status = error ? SupportErrorStatus() : SupportOkStatus();
  result.refusal_reason = std::move(reason);
  result.evidence = BaseEvidence("transaction.read_after_write_overlay");
  AddEvidence(&result.evidence, "accepted", "false");
  AddEvidence(&result.evidence, "visible_to_other_transactions", "false");
  result.diagnostic = MakeTransactionSupportDiagnostic(result.status,
                                                       std::move(diagnostic_code),
                                                       std::move(message_key),
                                                       std::move(detail));
  return result;
}

TransactionStatementIdReservationResult StatementReservationRefusal(
    std::string reason,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  TransactionStatementIdReservationResult result;
  result.status = SupportErrorStatus();
  result.refusal_reason = std::move(reason);
  result.evidence = BaseEvidence("transaction.statement_id_reservation");
  AddEvidence(&result.evidence, "accepted", "false");
  result.diagnostic = MakeTransactionSupportDiagnostic(result.status,
                                                       std::move(diagnostic_code),
                                                       std::move(message_key),
                                                       std::move(detail));
  return result;
}

}  // namespace

const char* SavepointStateDeltaStateName(SavepointStateDeltaState state) {
  switch (state) {
    case SavepointStateDeltaState::active: return "active";
    case SavepointStateDeltaState::rolled_back: return "rolled_back";
  }
  return "unknown";
}

TransactionCommitFenceCoalescingResult PlanTransactionCommitFenceCoalescing(
    const TransactionCommitFenceCoalescingRequest& request) {
  if (!request.policy_enabled || !request.allow_commit_fence_coalescing) {
    return FenceRefusal(request,
                        request.policy_required,
                        "policy_disabled",
                        "SB-MGA-COMMIT-FENCE-COALESCING-DISABLED",
                        "transaction.support.commit_fence_coalescing_disabled");
  }
  if (request.local_transaction_ids.empty()) {
    return FenceRefusal(request,
                        request.policy_required,
                        "empty_transaction_set",
                        "SB-MGA-COMMIT-FENCE-COALESCING-EMPTY",
                        "transaction.support.commit_fence_coalescing_empty");
  }
  if (!request.inventory_authoritative) {
    return FenceRefusal(request,
                        true,
                        "inventory_not_authoritative",
                        "SB-MGA-COMMIT-FENCE-INVENTORY-NOT-AUTHORITATIVE",
                        "transaction.support.commit_fence_inventory_not_authoritative");
  }
  if (request.fence_generation == 0) {
    return FenceRefusal(request,
                        true,
                        "fence_generation_invalid",
                        "SB-MGA-COMMIT-FENCE-GENERATION-INVALID",
                        "transaction.support.commit_fence_generation_invalid");
  }
  if (request.max_transactions_per_fence != 0 &&
      request.local_transaction_ids.size() > request.max_transactions_per_fence) {
    return FenceRefusal(request,
                        request.policy_required,
                        "policy_limit_exceeded",
                        "SB-MGA-COMMIT-FENCE-COALESCING-LIMIT",
                        "transaction.support.commit_fence_coalescing_limit",
                        std::to_string(request.max_transactions_per_fence));
  }
  if (HasDuplicateLocalIds(request.local_transaction_ids)) {
    return FenceRefusal(request,
                        true,
                        "duplicate_transaction_id",
                        "SB-MGA-COMMIT-FENCE-DUPLICATE-TX",
                        "transaction.support.commit_fence_duplicate_transaction");
  }

  for (const LocalTransactionId& local_transaction_id : request.local_transaction_ids) {
    if (!local_transaction_id.valid()) {
      return FenceRefusal(request,
                          true,
                          "invalid_transaction_id",
                          "SB-MGA-COMMIT-FENCE-INVALID-TX",
                          "transaction.support.commit_fence_invalid_transaction");
    }
    const TransactionInventoryEntry* entry = FindInventoryEntry(request.inventory, local_transaction_id);
    if (entry == nullptr) {
      return FenceRefusal(request,
                          true,
                          "transaction_not_found",
                          "SB-MGA-COMMIT-FENCE-TX-NOT-FOUND",
                          "transaction.support.commit_fence_transaction_not_found",
                          std::to_string(local_transaction_id.value));
    }
    if (!IsCommittedOrArchived(*entry) || !entry->evidence_record_written) {
      return FenceRefusal(request,
                          true,
                          "transaction_not_durably_committed",
                          "SB-MGA-COMMIT-FENCE-TX-NOT-DURABLY-COMMITTED",
                          "transaction.support.commit_fence_transaction_not_durably_committed",
                          std::to_string(local_transaction_id.value));
    }
  }

  TransactionCommitFenceCoalescingResult result;
  result.status = SupportOkStatus();
  result.accepted = true;
  result.policy_required = request.policy_required;
  result.coalesced_fence_count = 1;
  result.input_transaction_count = static_cast<u64>(request.local_transaction_ids.size());
  result.evidence = BaseEvidence("transaction.commit_fence_coalescing");
  AddEvidence(&result.evidence, "accepted", "true");
  AddEvidence(&result.evidence, "coalesced_fence_count", "1");
  AddEvidence(&result.evidence, "fence_generation", std::to_string(request.fence_generation));
  AddEvidence(&result.evidence, "inventory_authoritative", "true");
  return result;
}

SavepointStateDeltaResult SavepointStateDeltaManager::RecordDelta(SavepointStateDelta delta) {
  if (!delta.local_transaction_id.valid()) {
    return SavepointDeltaError("SB-MGA-SAVEPOINT-DELTA-INVALID-TX",
                               "transaction.support.savepoint_delta_invalid_transaction");
  }
  if (delta.mutation_sequence == 0 || delta.object_uuid.empty() || delta.record_uuid.empty()) {
    return SavepointDeltaError("SB-MGA-SAVEPOINT-DELTA-INVALID-MUTATION",
                               "transaction.support.savepoint_delta_invalid_mutation");
  }
  if (!delta.durable_evidence_written) {
    return SavepointDeltaError("SB-MGA-SAVEPOINT-DELTA-EVIDENCE-REQUIRED",
                               "transaction.support.savepoint_delta_evidence_required",
                               delta.record_uuid);
  }
  for (const SavepointStateDelta& existing : deltas_) {
    if (existing.local_transaction_id.value == delta.local_transaction_id.value &&
        existing.mutation_sequence == delta.mutation_sequence) {
      return SavepointDeltaError("SB-MGA-SAVEPOINT-DELTA-DUPLICATE-MUTATION",
                                 "transaction.support.savepoint_delta_duplicate_mutation",
                                 std::to_string(delta.mutation_sequence));
    }
  }

  delta.state = SavepointStateDeltaState::active;
  deltas_.push_back(delta);

  SavepointStateDeltaResult result;
  result.status = SupportOkStatus();
  result.delta = delta;
  result.active_delta_count = CountDeltas(deltas_, delta.local_transaction_id, SavepointStateDeltaState::active);
  result.rolled_back_delta_count = CountDeltas(deltas_, delta.local_transaction_id, SavepointStateDeltaState::rolled_back);
  result.evidence = BaseEvidence("transaction.savepoint_state_delta");
  AddEvidence(&result.evidence, "delta_state", SavepointStateDeltaStateName(delta.state));
  AddEvidence(&result.evidence, "savepoint_delta_is_transaction_authority", "false");
  return result;
}

SavepointStateDeltaRollbackResult SavepointStateDeltaManager::RollbackToSavepoint(
    LocalTransactionId local_transaction_id,
    u64 savepoint_ordinal) {
  if (!local_transaction_id.valid()) {
    return SavepointRollbackError("SB-MGA-SAVEPOINT-DELTA-ROLLBACK-INVALID-TX",
                                  "transaction.support.savepoint_delta_rollback_invalid_transaction");
  }

  u64 affected = 0;
  for (SavepointStateDelta& delta : deltas_) {
    if (delta.local_transaction_id.value != local_transaction_id.value ||
        delta.state == SavepointStateDeltaState::rolled_back) {
      continue;
    }
    if (savepoint_ordinal == 0 || delta.savepoint_ordinal >= savepoint_ordinal) {
      delta.state = SavepointStateDeltaState::rolled_back;
      ++affected;
    }
  }

  SavepointStateDeltaRollbackResult result;
  result.status = SupportOkStatus();
  result.affected_delta_count = affected;
  result.active_delta_count = CountDeltas(deltas_, local_transaction_id, SavepointStateDeltaState::active);
  result.rolled_back_delta_count = CountDeltas(deltas_, local_transaction_id, SavepointStateDeltaState::rolled_back);
  result.evidence = BaseEvidence("transaction.savepoint_state_delta.rollback");
  AddEvidence(&result.evidence, "affected_delta_count", std::to_string(affected));
  AddEvidence(&result.evidence, "transaction_identity_unchanged", "true");
  return result;
}

std::optional<SavepointStateDelta> SavepointStateDeltaManager::LatestActiveDelta(
    LocalTransactionId local_transaction_id,
    const std::string& record_uuid) const {
  const SavepointStateDelta* best = nullptr;
  for (const SavepointStateDelta& delta : deltas_) {
    if (delta.local_transaction_id.value != local_transaction_id.value ||
        delta.record_uuid != record_uuid ||
        delta.state != SavepointStateDeltaState::active) {
      continue;
    }
    if (best == nullptr || delta.mutation_sequence > best->mutation_sequence) {
      best = &delta;
    }
  }
  if (best == nullptr) {
    return std::nullopt;
  }
  return *best;
}

u64 SavepointStateDeltaManager::ActiveDeltaCount(LocalTransactionId local_transaction_id) const {
  return CountDeltas(deltas_, local_transaction_id, SavepointStateDeltaState::active);
}

u64 SavepointStateDeltaManager::RolledBackDeltaCount(LocalTransactionId local_transaction_id) const {
  return CountDeltas(deltas_, local_transaction_id, SavepointStateDeltaState::rolled_back);
}

TransactionSnapshotTemplateCache::TransactionSnapshotTemplateCache(u64 cache_generation,
                                                                   u64 invalidation_generation)
    : cache_generation_(cache_generation == 0 ? 1 : cache_generation),
      invalidation_generation_(invalidation_generation == 0 ? 1 : invalidation_generation) {}

TransactionSnapshotTemplateCacheResult TransactionSnapshotTemplateCache::Store(
    const TransactionSnapshotTemplateCacheRequest& request) {
  if (!request.inventory_authoritative) {
    return SnapshotTemplateRefusal(true,
                                   "inventory_not_authoritative",
                                   "SB-MGA-SNAPSHOT-TEMPLATE-INVENTORY-NOT-AUTHORITATIVE",
                                   "transaction.support.snapshot_template_inventory_not_authoritative");
  }
  if (request.inventory_generation == 0 || !request.next_local_transaction_id.valid() ||
      !ValidStaticInputs(request.static_inputs)) {
    return SnapshotTemplateRefusal(true,
                                   "invalid_template_inputs",
                                   "SB-MGA-SNAPSHOT-TEMPLATE-INPUTS-INVALID",
                                   "transaction.support.snapshot_template_inputs_invalid");
  }

  TransactionSnapshotTemplate snapshot_template;
  snapshot_template.static_inputs = request.static_inputs;
  snapshot_template.cache_generation = cache_generation_;
  snapshot_template.invalidation_generation = invalidation_generation_;
  snapshot_template.inventory_generation = request.inventory_generation;
  snapshot_template.next_local_transaction_id = request.next_local_transaction_id;
  snapshot_template.visible_through_local_transaction_id = request.visible_through_local_transaction_id;

  auto existing = std::find_if(templates_.begin(), templates_.end(), [&](const TransactionSnapshotTemplate& item) {
    return SameStaticInputs(item.static_inputs, request.static_inputs);
  });
  if (existing == templates_.end()) {
    templates_.push_back(snapshot_template);
  } else {
    *existing = snapshot_template;
  }

  TransactionSnapshotTemplateCacheResult result;
  result.status = SupportOkStatus();
  result.accepted = true;
  result.snapshot_template = snapshot_template;
  result.evidence = BaseEvidence("transaction.snapshot_template_cache");
  AddEvidence(&result.evidence, "accepted", "true");
  AddEvidence(&result.evidence, "template_static_inputs_only", "true");
  AddEvidence(&result.evidence, "active_transaction_set_cached", "false");
  return result;
}

TransactionSnapshotTemplateCacheResult TransactionSnapshotTemplateCache::Lookup(
    const TransactionSnapshotTemplateCacheRequest& request) const {
  if (!request.inventory_authoritative) {
    return SnapshotTemplateRefusal(true,
                                   "inventory_not_authoritative",
                                   "SB-MGA-SNAPSHOT-TEMPLATE-LOOKUP-INVENTORY-NOT-AUTHORITATIVE",
                                   "transaction.support.snapshot_template_lookup_inventory_not_authoritative");
  }
  if (request.inventory_generation == 0 || !request.next_local_transaction_id.valid() ||
      !ValidStaticInputs(request.static_inputs)) {
    return SnapshotTemplateRefusal(true,
                                   "invalid_template_inputs",
                                   "SB-MGA-SNAPSHOT-TEMPLATE-LOOKUP-INPUTS-INVALID",
                                   "transaction.support.snapshot_template_lookup_inputs_invalid");
  }

  for (const TransactionSnapshotTemplate& item : templates_) {
    if (!SameStaticInputs(item.static_inputs, request.static_inputs)) {
      continue;
    }
    if (item.cache_generation != cache_generation_ ||
        item.invalidation_generation != invalidation_generation_ ||
        item.inventory_generation != request.inventory_generation ||
        item.next_local_transaction_id.value != request.next_local_transaction_id.value ||
        item.visible_through_local_transaction_id.value !=
            request.visible_through_local_transaction_id.value) {
      return SnapshotTemplateRefusal(true,
                                     "template_stale",
                                     "SB-MGA-SNAPSHOT-TEMPLATE-STALE",
                                     "transaction.support.snapshot_template_stale");
    }

    TransactionSnapshotTemplateCacheResult result;
    result.status = SupportOkStatus();
    result.accepted = true;
    result.cache_hit = true;
    result.snapshot_template = item;
    result.evidence = BaseEvidence("transaction.snapshot_template_cache");
    AddEvidence(&result.evidence, "accepted", "true");
    AddEvidence(&result.evidence, "cache_hit", "true");
    AddEvidence(&result.evidence, "template_static_inputs_only", "true");
    AddEvidence(&result.evidence, "active_transaction_set_cached", "false");
    return result;
  }

  return SnapshotTemplateRefusal(false,
                                 "cache_miss",
                                 "SB-MGA-SNAPSHOT-TEMPLATE-MISS",
                                 "transaction.support.snapshot_template_miss");
}

void TransactionSnapshotTemplateCache::Invalidate(u64 invalidation_generation) {
  invalidation_generation_ = invalidation_generation == 0 ? invalidation_generation_ + 1 : invalidation_generation;
}

MgaVisibilityAccelerationBuildResult BuildMgaVisibilityAccelerationTable(
    const MgaVisibilityAccelerationBuildRequest& request) {
  MgaVisibilityAccelerationBuildResult result;
  result.evidence = BaseEvidence("transaction.visibility_acceleration_table");

  if (!request.inventory_authoritative) {
    result.status = SupportErrorStatus();
    result.refusal_reason = "inventory_not_authoritative";
    result.diagnostic = MakeTransactionSupportDiagnostic(
        result.status,
        "SB-MGA-VISIBILITY-ACCELERATION-INVENTORY-NOT-AUTHORITATIVE",
        "transaction.support.visibility_acceleration_inventory_not_authoritative");
    return result;
  }
  if (request.inventory_generation == 0 || request.table_generation == 0) {
    result.status = SupportErrorStatus();
    result.refusal_reason = "generation_invalid";
    result.diagnostic = MakeTransactionSupportDiagnostic(result.status,
                                                         "SB-MGA-VISIBILITY-ACCELERATION-GENERATION-INVALID",
                                                         "transaction.support.visibility_acceleration_generation_invalid");
    return result;
  }

  result.status = SupportOkStatus();
  result.accepted = true;
  result.table.inventory_generation = request.inventory_generation;
  result.table.table_generation = request.table_generation;
  result.table.built_from_authoritative_inventory = true;
  for (const TransactionInventoryEntry& inventory_entry : request.inventory.entries) {
    if (!inventory_entry.identity.local_id.valid()) {
      result.status = SupportErrorStatus();
      result.accepted = false;
      result.refusal_reason = "inventory_entry_invalid";
      result.table.entries.clear();
      result.diagnostic = MakeTransactionSupportDiagnostic(result.status,
                                                           "SB-MGA-VISIBILITY-ACCELERATION-ENTRY-INVALID",
                                                           "transaction.support.visibility_acceleration_entry_invalid");
      return result;
    }
    MgaVisibilityAccelerationEntry entry;
    entry.local_transaction_id = inventory_entry.identity.local_id;
    entry.state = inventory_entry.state;
    entry.evidence_record_written = inventory_entry.evidence_record_written;
    entry.final_unix_epoch_millis = inventory_entry.final_unix_epoch_millis;
    result.table.entries.push_back(entry);
  }
  AddEvidence(&result.evidence, "accepted", "true");
  AddEvidence(&result.evidence, "inventory_generation", std::to_string(request.inventory_generation));
  AddEvidence(&result.evidence, "entry_count", std::to_string(result.table.entries.size()));
  return result;
}

MgaVisibilityAccelerationProbeResult ProbeMgaVisibilityAccelerationTable(
    const MgaVisibilityAccelerationProbeRequest& request) {
  MgaVisibilityAccelerationProbeResult result;
  result.evidence = BaseEvidence("transaction.visibility_acceleration_table.probe");

  auto refuse = [&](std::string reason,
                    std::string code,
                    std::string message,
                    std::string detail = {}) {
    result.status = SupportErrorStatus();
    result.accepted = false;
    result.refusal_reason = std::move(reason);
    result.diagnostic = MakeTransactionSupportDiagnostic(result.status,
                                                         std::move(code),
                                                         std::move(message),
                                                         std::move(detail));
    AddEvidence(&result.evidence, "accepted", "false");
    return result;
  };

  if (!request.current_inventory_authoritative || !request.table.built_from_authoritative_inventory) {
    return refuse("inventory_not_authoritative",
                  "SB-MGA-VISIBILITY-ACCELERATION-PROBE-INVENTORY-NOT-AUTHORITATIVE",
                  "transaction.support.visibility_acceleration_probe_inventory_not_authoritative");
  }
  if (request.inventory_generation == 0 ||
      request.inventory_generation != request.table.inventory_generation) {
    return refuse("inventory_generation_mismatch",
                  "SB-MGA-VISIBILITY-ACCELERATION-PROBE-GENERATION-MISMATCH",
                  "transaction.support.visibility_acceleration_probe_generation_mismatch");
  }
  if (!request.first_local_transaction_id.valid() ||
      !request.last_local_transaction_id.valid() ||
      request.first_local_transaction_id.value > request.last_local_transaction_id.value) {
    return refuse("invalid_range",
                  "SB-MGA-VISIBILITY-ACCELERATION-PROBE-RANGE-INVALID",
                  "transaction.support.visibility_acceleration_probe_range_invalid");
  }

  bool all_committed = true;
  for (u64 value = request.first_local_transaction_id.value;
       value <= request.last_local_transaction_id.value;
       ++value) {
    const LocalTransactionId local_transaction_id = MakeLocalTransactionId(value);
    const MgaVisibilityAccelerationEntry* accelerated =
        FindAccelerationEntry(request.table, local_transaction_id);
    const TransactionInventoryEntry* inventory_entry =
        FindInventoryEntry(request.current_inventory, local_transaction_id);
    if (accelerated == nullptr || inventory_entry == nullptr) {
      return refuse("entry_missing",
                    "SB-MGA-VISIBILITY-ACCELERATION-PROBE-ENTRY-MISSING",
                    "transaction.support.visibility_acceleration_probe_entry_missing",
                    std::to_string(value));
    }
    if (!SameAccelerationEntry(*accelerated, *inventory_entry)) {
      return refuse("entry_mismatch",
                    "SB-MGA-VISIBILITY-ACCELERATION-PROBE-ENTRY-MISMATCH",
                    "transaction.support.visibility_acceleration_probe_entry_mismatch",
                    std::to_string(value));
    }
    all_committed = all_committed && IsCommittedOrArchived(*inventory_entry) &&
                    inventory_entry->evidence_record_written;
  }

  result.status = SupportOkStatus();
  result.accepted = true;
  result.all_committed = all_committed;
  AddEvidence(&result.evidence, "accepted", "true");
  AddEvidence(&result.evidence, "all_committed", all_committed ? "true" : "false");
  AddEvidence(&result.evidence, "validated_against_current_inventory", "true");
  return result;
}

ReadAfterWriteOverlayResult ReadAfterWriteLocalOverlayCache::Put(ReadAfterWriteOverlayEntry entry) {
  if (!entry.local_transaction_id.valid() || entry.object_uuid.empty() ||
      entry.record_uuid.empty() || entry.mutation_sequence == 0 || entry.value_hash.empty()) {
    return OverlayRefusal(true,
                          "invalid_overlay_entry",
                          "SB-MGA-RAW-OVERLAY-ENTRY-INVALID",
                          "transaction.support.raw_overlay_entry_invalid");
  }
  if (!entry.visible_to_own_transaction || entry.visible_to_other_transactions ||
      entry.overlay_is_transaction_finality_authority) {
    return OverlayRefusal(true,
                          "overlay_scope_invalid",
                          "SB-MGA-RAW-OVERLAY-SCOPE-INVALID",
                          "transaction.support.raw_overlay_scope_invalid");
  }

  entries_.push_back(entry);
  ReadAfterWriteOverlayResult result;
  result.status = SupportOkStatus();
  result.accepted = true;
  result.entry = entry;
  result.evidence = BaseEvidence("transaction.read_after_write_overlay");
  AddEvidence(&result.evidence, "accepted", "true");
  AddEvidence(&result.evidence, "visible_to_own_transaction", "true");
  AddEvidence(&result.evidence, "visible_to_other_transactions", "false");
  return result;
}

ReadAfterWriteOverlayResult ReadAfterWriteLocalOverlayCache::Lookup(
    LocalTransactionId local_transaction_id,
    const std::string& record_uuid) const {
  if (!local_transaction_id.valid() || record_uuid.empty()) {
    return OverlayRefusal(true,
                          "invalid_lookup",
                          "SB-MGA-RAW-OVERLAY-LOOKUP-INVALID",
                          "transaction.support.raw_overlay_lookup_invalid");
  }

  const ReadAfterWriteOverlayEntry* best = nullptr;
  for (const ReadAfterWriteOverlayEntry& entry : entries_) {
    if (entry.local_transaction_id.value != local_transaction_id.value ||
        entry.record_uuid != record_uuid) {
      continue;
    }
    if (best == nullptr || entry.mutation_sequence > best->mutation_sequence) {
      best = &entry;
    }
  }
  if (best == nullptr) {
    return OverlayRefusal(false,
                          "cache_miss",
                          "SB-MGA-RAW-OVERLAY-MISS",
                          "transaction.support.raw_overlay_miss");
  }

  ReadAfterWriteOverlayResult result;
  result.status = SupportOkStatus();
  result.accepted = true;
  result.cache_hit = true;
  result.entry = *best;
  result.evidence = BaseEvidence("transaction.read_after_write_overlay");
  AddEvidence(&result.evidence, "accepted", "true");
  AddEvidence(&result.evidence, "cache_hit", "true");
  AddEvidence(&result.evidence, "visible_to_own_transaction", "true");
  AddEvidence(&result.evidence, "visible_to_other_transactions", "false");
  return result;
}

ReadAfterWriteOverlayRollbackResult ReadAfterWriteLocalOverlayCache::RollbackToSavepoint(
    LocalTransactionId local_transaction_id,
    u64 savepoint_ordinal) {
  ReadAfterWriteOverlayRollbackResult result;
  result.evidence = BaseEvidence("transaction.read_after_write_overlay.rollback");
  if (!local_transaction_id.valid()) {
    result.status = SupportErrorStatus();
    result.diagnostic = MakeTransactionSupportDiagnostic(result.status,
                                                         "SB-MGA-RAW-OVERLAY-ROLLBACK-INVALID-TX",
                                                         "transaction.support.raw_overlay_rollback_invalid_transaction");
    return result;
  }

  const auto before = entries_.size();
  entries_.erase(std::remove_if(entries_.begin(),
                                entries_.end(),
                                [&](const ReadAfterWriteOverlayEntry& entry) {
                                  return entry.local_transaction_id.value == local_transaction_id.value &&
                                         (savepoint_ordinal == 0 ||
                                          entry.savepoint_ordinal >= savepoint_ordinal);
                                }),
                 entries_.end());
  result.status = SupportOkStatus();
  result.removed_entry_count = static_cast<u64>(before - entries_.size());
  AddEvidence(&result.evidence, "removed_entry_count", std::to_string(result.removed_entry_count));
  AddEvidence(&result.evidence, "overlay_is_transaction_finality_authority", "false");
  return result;
}

u64 ReadAfterWriteLocalOverlayCache::size() const {
  return static_cast<u64>(entries_.size());
}

TransactionStatementIdReservationResult TransactionStatementIdReservationService::Reserve(
    const TransactionStatementIdReservationRequest& request) {
  if (!request.inventory_authoritative) {
    return StatementReservationRefusal("inventory_not_authoritative",
                                       "SB-MGA-STATEMENT-ID-INVENTORY-NOT-AUTHORITATIVE",
                                       "transaction.support.statement_id_inventory_not_authoritative");
  }
  if (!request.local_transaction_id.valid() || request.statement_count == 0) {
    return StatementReservationRefusal("invalid_request",
                                       "SB-MGA-STATEMENT-ID-REQUEST-INVALID",
                                       "transaction.support.statement_id_request_invalid");
  }

  const TransactionInventoryEntry* entry =
      FindInventoryEntry(request.inventory, request.local_transaction_id);
  if (entry == nullptr) {
    return StatementReservationRefusal("transaction_not_found",
                                       "SB-MGA-STATEMENT-ID-TX-NOT-FOUND",
                                       "transaction.support.statement_id_transaction_not_found",
                                       std::to_string(request.local_transaction_id.value));
  }
  if (!SameTransactionUuid(entry->identity.transaction_uuid, request.transaction_uuid)) {
    return StatementReservationRefusal("transaction_uuid_mismatch",
                                       "SB-MGA-STATEMENT-ID-TX-UUID-MISMATCH",
                                       "transaction.support.statement_id_transaction_uuid_mismatch",
                                       std::to_string(request.local_transaction_id.value));
  }
  if (!IsStatementReservableState(entry->state) || entry->rollback_only) {
    return StatementReservationRefusal("transaction_not_active",
                                       "SB-MGA-STATEMENT-ID-TX-NOT-ACTIVE",
                                       "transaction.support.statement_id_transaction_not_active",
                                       TransactionStateName(entry->state));
  }

  Counter* counter = nullptr;
  for (Counter& existing : counters_) {
    if (existing.local_transaction_id.value == request.local_transaction_id.value) {
      counter = &existing;
      break;
    }
  }
  if (counter == nullptr) {
    counters_.push_back({request.local_transaction_id, 1});
    counter = &counters_.back();
  }

  TransactionStatementIdReservationResult result;
  result.status = SupportOkStatus();
  result.accepted = true;
  result.statement_ids.reserve(static_cast<std::size_t>(request.statement_count));
  for (u64 i = 0; i < request.statement_count; ++i) {
    TransactionStatementId id;
    id.local_transaction_id = request.local_transaction_id;
    id.statement_sequence = counter->next_statement_sequence++;
    result.statement_ids.push_back(id);
  }
  result.next_statement_sequence = counter->next_statement_sequence;
  result.evidence = BaseEvidence("transaction.statement_id_reservation");
  AddEvidence(&result.evidence, "accepted", "true");
  AddEvidence(&result.evidence, "reserved_against_inventory_state", TransactionStateName(entry->state));
  AddEvidence(&result.evidence, "statement_id_is_transaction_finality_authority", "false");
  return result;
}

void TransactionStatementIdReservationService::Clear(LocalTransactionId local_transaction_id) {
  counters_.erase(std::remove_if(counters_.begin(),
                                 counters_.end(),
                                 [&](const Counter& counter) {
                                   return counter.local_transaction_id.value == local_transaction_id.value;
                                 }),
                  counters_.end());
}

DiagnosticRecord MakeTransactionSupportDiagnostic(Status status,
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
                        "transaction.mga.support_services");
}

}  // namespace scratchbird::transaction::mga
