// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_evidence.hpp"
#include "transaction_inventory_validation.hpp"

#include "uuid.hpp"

#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using E = TransactionEvidenceError;
bool ValidContext(const TransactionEvidenceContext& context) noexcept {
  const auto& db = context.database_uuid;
  const auto& snapshot = context.snapshot_uuid;
  const auto& policy = context.policy_snapshot_uuid;
  return core::uuid::IsEngineIdentityUuid(db) && core::uuid::IsEngineIdentityUuid(snapshot) &&
      core::uuid::IsEngineIdentityUuid(policy) && db != snapshot && db != policy && snapshot != policy &&
      context.snapshot_generation && context.catalog_generation && context.security_generation && context.policy_generation;
}

std::string EventClassForState(TransactionState state) {
  switch (state) {
    case TransactionState::created:
    case TransactionState::active:
    case TransactionState::preparing:
      return "begin";
    case TransactionState::read_only_active:
      return "read_only_begin";
    case TransactionState::prepared:
      return "prepare";
    case TransactionState::committing:
    case TransactionState::committed:
      return "commit";
    case TransactionState::rolling_back:
    case TransactionState::rolled_back:
      return "rollback";
    case TransactionState::limbo:
    case TransactionState::recovering:
      return "recovery_hold";
    case TransactionState::failed_terminal:
      return "failed_terminal";
    case TransactionState::archived:
      return "archived";
    case TransactionState::none:
      return "unknown";
  }
  return "unknown";
}

bool RestoreSafeTerminal(TransactionState state) {
  return state == TransactionState::committed || state == TransactionState::rolled_back;
}

std::string RestoreClassificationFor(const TransactionRecoveryClassification& classification,
                                     TransactionState outcome) {
  if (classification.fail_closed) {
    return "refuse_fail_closed";
  }
  if (RestoreSafeTerminal(outcome)) {
    return "restore_terminal_evidence";
  }
  if (classification.action == TransactionRecoveryAction::complete_rollback) {
    return "restore_after_local_rollback_recovery";
  }
  if (classification.action == TransactionRecoveryAction::complete_commit) {
    return "restore_after_local_commit_recovery";
  }
  return "restore_requires_classification";
}

TransactionLineageEvidenceRecord BuildRecord(const TransactionInventoryEntry& entry,
                                             const TransactionEvidenceContext& context) {
  const auto classification = ClassifyLocalTransactionForRecovery(entry);
  const auto outcome = InventoryVisibilityState(entry);
  TransactionLineageEvidenceRecord record;
  record.local_id = entry.identity.local_id;
  record.transaction_uuid = entry.identity.transaction_uuid.value;
  record.context = context;
  record.event_class = EventClassForState(entry.state);
  record.observed_state = TransactionStateName(entry.state);
  record.terminal_state = IsTerminalTransactionState(outcome) ? TransactionStateName(outcome) : "";
  record.restore_classification = RestoreClassificationFor(classification, outcome);
  record.refusal_condition = classification.fail_closed ? classification.stable_reason : "";
  record.terminal = IsTerminalTransactionState(outcome);
  record.evidence_written = entry.evidence_record_written;
  record.wal_required = false;
  return record;
}

}  // namespace

const char* TransactionEvidenceErrorCode(TransactionEvidenceError error) noexcept {
  switch(error) {
    case E::none: return nullptr;
    case E::invalid_context: return "MGA.EVIDENCE.INVALID_CONTEXT";
    case E::wal_not_authority: return "MGA.EVIDENCE.WAL_NOT_AUTHORITY";
    case E::invalid_inventory: return "MGA.EVIDENCE.INVENTORY_INVALID";
    case E::restore_refused: return "MGA.EVIDENCE.RESTORE_REFUSED";
    case E::resource_exhausted: return "MGA.EVIDENCE.RESOURCE_EXHAUSTED";
    case E::internal_failure: return "MGA.EVIDENCE.INTERNAL_FAILURE";
  }
  return "MGA.EVIDENCE.INTERNAL_FAILURE";
}

TransactionLineageEvidenceResult BuildTransactionLineageEvidence(
    const LocalTransactionInventory& inventory,
    const TransactionEvidenceContext& context) noexcept {
  if (!ValidContext(context)) return {};
  try {
    TransactionLineageEvidenceResult result;
    const char* invalid = ValidateLocalTransactionInventoryStructure(inventory);
    result.records.reserve(inventory.entries.size());
    for (const auto& entry : inventory.entries) {
      auto record = BuildRecord(entry, context);
      if (*invalid) {
        record.restore_classification = "refuse_fail_closed";
        record.refusal_condition = invalid;
      }
      result.records.push_back(std::move(record));
    }
    result.error = E::none;
    return result;
  } catch (const std::bad_alloc&) {
    return {E::resource_exhausted, {}};
  } catch (const std::length_error&) {
    return {E::resource_exhausted, {}};
  } catch (...) {
    return {E::internal_failure, {}};
  }
}

TransactionRestoreClassificationResult ClassifyTransactionInventoryForRestore(
    const LocalTransactionInventory& inventory,
    const TransactionEvidenceContext& context,
    bool caller_requires_wal) noexcept {
  if (!ValidContext(context)) return {};
  if (caller_requires_wal) return {E::wal_not_authority, {}, false, false};
  try {
    if (const auto reason = ValidateLocalTransactionInventoryStructure(inventory); *reason)
      return {E::invalid_inventory, {}, false, false};
    auto projection = BuildTransactionLineageEvidence(inventory, context);
    if (!projection.ok()) return {projection.error, {}, false, false};
    TransactionRestoreClassificationResult result;
    result.error = E::none;
    result.restore_allowed = true;
    result.records = std::move(projection.records);
    for (const auto& record : result.records) if (record.restore_classification == "refuse_fail_closed") {
      result.error = E::restore_refused;
      result.restore_allowed = false;
    }
    return result;
  } catch (const std::bad_alloc&) {
    return {E::resource_exhausted, {}, false, false};
  } catch (const std::length_error&) {
    return {E::resource_exhausted, {}, false, false};
  } catch (...) {
    return {E::internal_failure, {}, false, false};
  }
}

}  // namespace scratchbird::transaction::mga
