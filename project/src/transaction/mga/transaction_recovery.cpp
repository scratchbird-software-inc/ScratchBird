// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_recovery.hpp"

#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status RecoveryOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status RecoveryErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga};
}

bool TransitionForRecovery(TransactionInventoryEntry* entry,
                           TransactionState next,
                           u64 recovery_unix_epoch_millis,
                           DiagnosticRecord* diagnostic) {
  const auto transition = CheckTransactionStateTransition(entry->state, next, true);
  if (!transition.ok()) {
    *diagnostic = transition.diagnostic;
    return false;
  }
  entry->state = next;
  if (next == TransactionState::committed || next == TransactionState::rolled_back ||
      next == TransactionState::failed_terminal) {
    entry->final_unix_epoch_millis = recovery_unix_epoch_millis;
    entry->evidence_record_written = true;
  }
  return true;
}

bool ApplyRecoveryAction(TransactionInventoryEntry* entry,
                         TransactionRecoveryAction action,
                         u64 recovery_unix_epoch_millis,
                         DiagnosticRecord* diagnostic) {
  switch (action) {
    case TransactionRecoveryAction::no_action:
      return true;
    case TransactionRecoveryAction::complete_commit:
      return TransitionForRecovery(entry, TransactionState::committed, recovery_unix_epoch_millis, diagnostic);
    case TransactionRecoveryAction::complete_rollback:
      if (entry->state != TransactionState::rolling_back &&
          !TransitionForRecovery(entry, TransactionState::rolling_back, recovery_unix_epoch_millis, diagnostic)) {
        return false;
      }
      return TransitionForRecovery(entry, TransactionState::rolled_back, recovery_unix_epoch_millis, diagnostic);
    case TransactionRecoveryAction::prepared_waiting_local_decision:
    case TransactionRecoveryAction::limbo_requires_operator:
    case TransactionRecoveryAction::fail_closed_ambiguous:
    case TransactionRecoveryAction::unknown:
      return true;
  }
  return true;
}

TransactionInventoryEntry* FindRecoveryEntry(LocalTransactionInventory* inventory,
                                             LocalTransactionId local_id) {
  for (TransactionInventoryEntry& entry : inventory->entries) {
    if (entry.identity.local_id.value == local_id.value) {
      return &entry;
    }
  }
  return nullptr;
}

TransactionInventoryResult RecoveryInventoryError(std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail = {}) {
  TransactionInventoryResult result;
  result.status = RecoveryErrorStatus();
  result.diagnostic = MakeTransactionRecoveryDiagnostic(result.status,
                                                        std::move(diagnostic_code),
                                                        std::move(message_key),
                                                        std::move(detail));
  return result;
}

TransactionState TerminalStateForLimboDecision(LimboOperatorDecision decision) {
  switch (decision) {
    case LimboOperatorDecision::commit: return TransactionState::committed;
    case LimboOperatorDecision::rollback: return TransactionState::rolled_back;
    case LimboOperatorDecision::fail_terminal: return TransactionState::failed_terminal;
    case LimboOperatorDecision::unknown: return TransactionState::none;
  }
  return TransactionState::none;
}

}  // namespace

const char* TransactionRecoveryActionName(TransactionRecoveryAction action) {
  switch (action) {
    case TransactionRecoveryAction::no_action: return "no_action";
    case TransactionRecoveryAction::complete_commit: return "complete_commit";
    case TransactionRecoveryAction::complete_rollback: return "complete_rollback";
    case TransactionRecoveryAction::prepared_waiting_local_decision: return "prepared_waiting_local_decision";
    case TransactionRecoveryAction::limbo_requires_operator: return "limbo_requires_operator";
    case TransactionRecoveryAction::fail_closed_ambiguous: return "fail_closed_ambiguous";
    case TransactionRecoveryAction::unknown: return "unknown";
  }
  return "unknown";
}

const char* LimboOperatorDecisionName(LimboOperatorDecision decision) {
  switch (decision) {
    case LimboOperatorDecision::commit: return "commit";
    case LimboOperatorDecision::rollback: return "rollback";
    case LimboOperatorDecision::fail_terminal: return "fail_terminal";
    case LimboOperatorDecision::unknown: return "unknown";
  }
  return "unknown";
}

TransactionRecoveryClassification ClassifyLocalTransactionForRecovery(const TransactionInventoryEntry& entry) {
  TransactionRecoveryClassification classification;
  classification.local_id = entry.identity.local_id;
  classification.observed_state = entry.state;

  if (entry.rollback_only && entry.state != TransactionState::committed &&
      entry.state != TransactionState::committing &&
      entry.state != TransactionState::rolled_back &&
      entry.state != TransactionState::archived) {
    classification.action = TransactionRecoveryAction::complete_rollback;
    classification.stable_reason = "rollback_only_local_state";
    return classification;
  }

  switch (entry.state) {
    case TransactionState::none:
      classification.action = TransactionRecoveryAction::fail_closed_ambiguous;
      classification.fail_closed = true;
      classification.stable_reason = "missing_transaction_state";
      return classification;
    case TransactionState::created:
    case TransactionState::active:
    case TransactionState::read_only_active:
    case TransactionState::preparing:
      classification.action = TransactionRecoveryAction::complete_rollback;
      classification.stable_reason = "uncommitted_local_state";
      return classification;
    case TransactionState::prepared:
      classification.action = TransactionRecoveryAction::prepared_waiting_local_decision;
      classification.fail_closed = true;
      classification.stable_reason = "prepared_requires_local_decision";
      return classification;
    case TransactionState::committing:
      if (entry.evidence_record_required && !entry.evidence_record_written) {
        classification.action = TransactionRecoveryAction::fail_closed_ambiguous;
        classification.fail_closed = true;
        classification.stable_reason = "committing_without_durable_evidence";
        return classification;
      }
      classification.action = TransactionRecoveryAction::complete_commit;
      classification.stable_reason = "commit_publication_incomplete";
      return classification;
    case TransactionState::rolling_back:
      classification.action = TransactionRecoveryAction::complete_rollback;
      classification.stable_reason = "rollback_publication_incomplete";
      return classification;
    case TransactionState::committed:
    case TransactionState::rolled_back:
    case TransactionState::archived:
      classification.action = TransactionRecoveryAction::no_action;
      classification.stable_reason = "terminal_or_archived";
      return classification;
    case TransactionState::limbo:
    case TransactionState::recovering:
      classification.action = TransactionRecoveryAction::limbo_requires_operator;
      classification.fail_closed = true;
      classification.stable_reason = "limbo_or_recovering_without_cluster_authority";
      return classification;
    case TransactionState::failed_terminal:
      classification.action = TransactionRecoveryAction::fail_closed_ambiguous;
      classification.fail_closed = true;
      classification.stable_reason = "failed_terminal_requires_review";
      return classification;
  }

  classification.action = TransactionRecoveryAction::fail_closed_ambiguous;
  classification.fail_closed = true;
  classification.stable_reason = "unknown_state";
  return classification;
}

TransactionRecoveryResult ClassifyLocalTransactionInventoryForRecovery(const LocalTransactionInventory& inventory) {
  TransactionRecoveryResult result;
  result.status = RecoveryOkStatus();
  result.recovered_inventory = inventory;
  for (const TransactionInventoryEntry& entry : inventory.entries) {
    if (!entry.identity.valid()) {
      result.status = RecoveryErrorStatus();
      result.diagnostic = MakeTransactionRecoveryDiagnostic(result.status,
                                                            "SB-SNTXN-RECOVERY-STATE-AMBIGUOUS",
                                                            "transaction.recovery.invalid_identity");
      return result;
    }
    auto classification = ClassifyLocalTransactionForRecovery(entry);
    if (classification.fail_closed) {
      result.write_admission_must_remain_fenced = true;
    }
    result.classifications.push_back(std::move(classification));
  }
  return result;
}

TransactionRecoveryResult ApplyLocalTransactionInventoryRecovery(LocalTransactionInventory inventory,
                                                                u64 recovery_unix_epoch_millis) {
  TransactionRecoveryResult result = ClassifyLocalTransactionInventoryForRecovery(inventory);
  if (!result.ok()) {
    return result;
  }

  result.recovered_inventory = std::move(inventory);
  for (TransactionInventoryEntry& entry : result.recovered_inventory.entries) {
    const auto classification = ClassifyLocalTransactionForRecovery(entry);
    if (classification.fail_closed) {
      result.write_admission_must_remain_fenced = true;
      continue;
    }

    const TransactionState before = entry.state;
    DiagnosticRecord diagnostic;
    if (!ApplyRecoveryAction(&entry, classification.action, recovery_unix_epoch_millis, &diagnostic)) {
      result.status = RecoveryErrorStatus();
      result.diagnostic = diagnostic;
      result.write_admission_must_remain_fenced = true;
      return result;
    }
    if (entry.state != before) {
      result.inventory_changed = true;
    }
  }

  return result;
}

TransactionInventoryResult ResolveLimboLocalTransactionWithOperatorDecision(
    LocalTransactionInventory inventory,
    LocalTransactionId local_id,
    LimboOperatorDecision decision,
    u64 final_unix_epoch_millis,
    LimboOperatorResolutionPolicy policy) {
  if (!local_id.valid()) {
    return RecoveryInventoryError("SB-SNTXN-LIMBO-LOCAL-ID-INVALID",
                                  "transaction.recovery.limbo_local_id_invalid");
  }
  if (decision == LimboOperatorDecision::unknown ||
      TerminalStateForLimboDecision(decision) == TransactionState::none) {
    return RecoveryInventoryError("SB-SNTXN-LIMBO-OPERATOR-DECISION-INVALID",
                                  "transaction.recovery.limbo_operator_decision_invalid",
                                  LimboOperatorDecisionName(decision));
  }
  if (!policy.operator_decision_authoritative ||
      policy.operator_evidence_reference.empty()) {
    return RecoveryInventoryError("SB-SNTXN-LIMBO-OPERATOR-AUTHORITY-REQUIRED",
                                  "transaction.recovery.limbo_operator_authority_required",
                                  "authoritative operator evidence is required");
  }

  TransactionInventoryEntry* entry = FindRecoveryEntry(&inventory, local_id);
  if (entry == nullptr) {
    return RecoveryInventoryError("SB-SNTXN-LIMBO-LOCAL-ID-NOT-FOUND",
                                  "transaction.recovery.limbo_local_id_not_found",
                                  std::to_string(local_id.value));
  }
  if (entry->state != TransactionState::limbo) {
    return RecoveryInventoryError("SB-SNTXN-LIMBO-STATE-REQUIRED",
                                  "transaction.recovery.limbo_state_required",
                                  TransactionStateName(entry->state));
  }
  if (entry->identity.scope == TransactionScope::cluster_global &&
      !policy.external_cluster_provider_decision_authoritative) {
    return RecoveryInventoryError(
        "SB-SNTXN-LIMBO-EXTERNAL-PROVIDER-REQUIRED",
        "transaction.recovery.limbo_external_provider_required",
        "cluster-scope limbo resolution requires external cluster provider authority");
  }

  DiagnosticRecord diagnostic;
  if (!TransitionForRecovery(entry,
                             TransactionState::recovering,
                             final_unix_epoch_millis,
                             &diagnostic)) {
    TransactionInventoryResult result;
    result.status = RecoveryErrorStatus();
    result.diagnostic = diagnostic;
    return result;
  }
  entry->evidence_record_required = true;
  if (!TransitionForRecovery(entry,
                             TerminalStateForLimboDecision(decision),
                             final_unix_epoch_millis,
                             &diagnostic)) {
    TransactionInventoryResult result;
    result.status = RecoveryErrorStatus();
    result.diagnostic = diagnostic;
    return result;
  }

  const TransactionInventoryEntry resolved_entry = *entry;
  TransactionInventoryResult result;
  result.status = RecoveryOkStatus();
  result.inventory = std::move(inventory);
  result.entry = resolved_entry;
  return result;
}

DiagnosticRecord MakeTransactionRecoveryDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "transaction.mga.recovery");
}

}  // namespace scratchbird::transaction::mga
