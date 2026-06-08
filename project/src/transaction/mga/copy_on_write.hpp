// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "row_version.hpp"
#include "runtime_platform.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"

#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u64;

enum class CopyOnWriteMutationKind : u16 {
  insert,
  update,
  delete_row,
  system_catalog_update,
  unknown
};

enum class CopyOnWriteMutationPhase : u16 {
  planned,
  base_version_locked,
  new_version_allocated,
  payload_written_unpublished,
  publish_pending_transaction,
  published,
  rollback_pending,
  rollback_complete,
  recovery_required,
  unknown
};

enum class CleanupHoldKind : u16 {
  none,
  oldest_interesting_transaction,
  oldest_active_transaction,
  oldest_snapshot_transaction,
  limbo_transaction,
  recovery_required,
  archive_required,
  backup_required,
  management_operation,
  legal_hold,
  admin_hold,
  unknown
};

enum class CleanupEligibilityDecision : u16 {
  eligible_requires_authority,
  eligible_authoritative,
  blocked_by_horizon,
  blocked_by_limbo,
  blocked_by_recovery,
  blocked_by_archive_or_backup,
  unknown
};

struct CopyOnWriteMutationIntent {
  CopyOnWriteMutationKind kind = CopyOnWriteMutationKind::unknown;
  TransactionIdentity transaction;
  RowIdentity row;
  u64 base_version_sequence = kInvalidRowVersionSequence;
  u64 new_version_sequence = kInvalidRowVersionSequence;
  bool has_base_version = false;
  bool payload_required = true;
  bool system_catalog_mutation = false;
};

struct CopyOnWriteMutationState {
  CopyOnWriteMutationIntent intent;
  CopyOnWriteMutationPhase phase = CopyOnWriteMutationPhase::unknown;
  RowVersionState resulting_row_state = RowVersionState::unknown;
  bool evidence_record_required = true;
  bool evidence_record_written = false;
};

struct CleanupHorizon {
  CleanupHoldKind hold_kind = CleanupHoldKind::unknown;
  LocalTransactionId horizon_transaction;
  bool authoritative = false;
  std::string stable_name;
};

struct CleanupHorizonVector {
  std::vector<CleanupHorizon> horizons;
};

struct CopyOnWriteMutationResult {
  Status status;
  CopyOnWriteMutationState mutation;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct CleanupEligibilityResult {
  Status status;
  CleanupEligibilityDecision decision = CleanupEligibilityDecision::unknown;
  CleanupHoldKind blocking_hold = CleanupHoldKind::unknown;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* CopyOnWriteMutationKindName(CopyOnWriteMutationKind kind);
const char* CopyOnWriteMutationPhaseName(CopyOnWriteMutationPhase phase);
const char* CleanupHoldKindName(CleanupHoldKind kind);
const char* CleanupEligibilityDecisionName(CleanupEligibilityDecision decision);
CopyOnWriteMutationResult PlanCopyOnWriteMutation(const CopyOnWriteMutationIntent& intent);
CopyOnWriteMutationResult PlanLocalCopyOnWriteMutationForTransaction(const TransactionInventoryEntry& entry,
                                                                     RowIdentity row,
                                                                     CopyOnWriteMutationKind kind,
                                                                     u64 base_version_sequence,
                                                                     u64 new_version_sequence);
CopyOnWriteMutationResult ValidateCopyOnWriteMutationState(const CopyOnWriteMutationState& mutation);
CopyOnWriteMutationResult AdvanceCopyOnWriteMutationPhase(const CopyOnWriteMutationState& mutation,
                                                          CopyOnWriteMutationPhase next_phase);
CleanupEligibilityResult EvaluateCleanupEligibility(
    const RowVersionMetadata& metadata,
    const CleanupHorizonVector& horizons);
DiagnosticRecord MakeCopyOnWriteDiagnostic(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {});

}  // namespace scratchbird::transaction::mga
