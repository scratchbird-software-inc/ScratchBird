// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB_MGA_LINEAGE_RESTORE_EVIDENCE_AUTHORITY
#include "transaction_inventory.hpp"
#include "transaction_recovery.hpp"

#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

// Data provenance supplied by the owning admitted context. Structural validity
// is not snapshot ownership, policy authorization or permission to restore.
struct TransactionEvidenceContext {
  core::platform::Uuid database_uuid, snapshot_uuid, policy_snapshot_uuid;
  u64 snapshot_generation = 0, catalog_generation = 0;
  u64 security_generation = 0, policy_generation = 0;
};
enum class TransactionEvidenceError {
  none, invalid_context, wal_not_authority, invalid_inventory,
  restore_refused, resource_exhausted, internal_failure
};
const char* TransactionEvidenceErrorCode(TransactionEvidenceError) noexcept;

struct TransactionLineageEvidenceRecord {
  LocalTransactionId local_id;
  core::platform::Uuid transaction_uuid;
  TransactionEvidenceContext context;
  std::string event_class;
  std::string observed_state;
  std::string terminal_state;
  std::string restore_classification;
  std::string refusal_condition;
  bool terminal = false;
  bool evidence_written = false;
  bool wal_required = false;
};

struct TransactionLineageEvidenceResult {
  TransactionEvidenceError error = TransactionEvidenceError::invalid_context;
  std::vector<TransactionLineageEvidenceRecord> records;
  bool ok() const noexcept { return error == TransactionEvidenceError::none; }
};

struct TransactionRestoreClassificationResult {
  TransactionEvidenceError error = TransactionEvidenceError::invalid_context;
  std::vector<TransactionLineageEvidenceRecord> records;
  bool restore_allowed = false;
  bool wal_required = false;

  bool ok() const {
    return error == TransactionEvidenceError::none;
  }
};

TransactionLineageEvidenceResult BuildTransactionLineageEvidence(
    const LocalTransactionInventory& inventory,
    const TransactionEvidenceContext& context) noexcept;

TransactionRestoreClassificationResult ClassifyTransactionInventoryForRestore(
    const LocalTransactionInventory& inventory,
    const TransactionEvidenceContext& context,
    bool caller_requires_wal) noexcept;

}  // namespace scratchbird::transaction::mga
