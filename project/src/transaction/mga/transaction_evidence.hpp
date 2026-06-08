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

struct TransactionLineageEvidenceRecord {
  LocalTransactionId local_id;
  std::string transaction_uuid;
  std::string event_class;
  std::string observed_state;
  std::string terminal_state;
  std::string schema_epoch;
  std::string snapshot_capsule;
  std::string restore_classification;
  std::string refusal_condition;
  bool terminal = false;
  bool evidence_written = false;
  bool wal_required = false;
};

struct TransactionRestoreClassificationResult {
  Status status;
  std::vector<TransactionLineageEvidenceRecord> records;
  DiagnosticRecord diagnostic;
  bool restore_allowed = true;
  bool wal_required = false;

  bool ok() const {
    return status.ok();
  }
};

std::vector<TransactionLineageEvidenceRecord> BuildTransactionLineageEvidence(
    const LocalTransactionInventory& inventory,
    std::string schema_epoch,
    std::string snapshot_capsule);

TransactionRestoreClassificationResult ClassifyTransactionInventoryForRestore(
    const LocalTransactionInventory& inventory,
    std::string schema_epoch,
    std::string snapshot_capsule,
    bool caller_requires_wal);

DiagnosticRecord MakeTransactionEvidenceDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail = {});

}  // namespace scratchbird::transaction::mga
