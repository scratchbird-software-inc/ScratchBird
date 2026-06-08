// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-SNTXN-RECOVERY-ANCHOR
#include "transaction_inventory.hpp"

#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

enum class TransactionRecoveryAction : u16 {
  no_action,
  complete_commit,
  complete_rollback,
  prepared_waiting_local_decision,
  limbo_requires_operator,
  fail_closed_ambiguous,
  unknown
};

enum class LimboOperatorDecision : u16 {
  commit,
  rollback,
  fail_terminal,
  unknown
};

struct LimboOperatorResolutionPolicy {
  bool operator_decision_authoritative = false;
  bool external_cluster_provider_decision_authoritative = false;
  std::string operator_evidence_reference;
};

struct TransactionRecoveryClassification {
  LocalTransactionId local_id;
  TransactionState observed_state = TransactionState::none;
  TransactionRecoveryAction action = TransactionRecoveryAction::unknown;
  bool fail_closed = false;
  std::string stable_reason;
};

struct TransactionRecoveryResult {
  Status status;
  std::vector<TransactionRecoveryClassification> classifications;
  LocalTransactionInventory recovered_inventory;
  DiagnosticRecord diagnostic;
  bool inventory_changed = false;
  bool write_admission_must_remain_fenced = false;

  bool ok() const {
    return status.ok();
  }
};

const char* TransactionRecoveryActionName(TransactionRecoveryAction action);
const char* LimboOperatorDecisionName(LimboOperatorDecision decision);
TransactionRecoveryClassification ClassifyLocalTransactionForRecovery(const TransactionInventoryEntry& entry);
TransactionRecoveryResult ClassifyLocalTransactionInventoryForRecovery(const LocalTransactionInventory& inventory);
TransactionRecoveryResult ApplyLocalTransactionInventoryRecovery(LocalTransactionInventory inventory,
                                                                u64 recovery_unix_epoch_millis);
TransactionInventoryResult ResolveLimboLocalTransactionWithOperatorDecision(
    LocalTransactionInventory inventory,
    LocalTransactionId local_id,
    LimboOperatorDecision decision,
    u64 final_unix_epoch_millis,
    LimboOperatorResolutionPolicy policy);
DiagnosticRecord MakeTransactionRecoveryDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail = {});

}  // namespace scratchbird::transaction::mga
