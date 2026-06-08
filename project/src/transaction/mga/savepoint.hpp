// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-SNTXN-SAVEPOINT-ANCHOR
#include "transaction_inventory.hpp"

#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

struct SavepointRecord {
  LocalTransactionId local_id;
  std::string name;
  u64 ordinal = 0;
  u64 mutation_sequence = 0;
};

enum class SavepointMutationKind : u16 {
  data_page,
  catalog,
  index,
  overflow,
  metrics,
  unknown
};

enum class SavepointRollbackDecision : u16 {
  rollback_actions_ready,
  rollback_refused_missing_undo,
  rollback_refused_executor_missing,
  rollback_refused_executor_unavailable,
  rollback_refused_executor_failed,
  savepoint_not_found,
  invalid_request
};

struct SavepointMutationRecord {
  LocalTransactionId local_id;
  u64 mutation_sequence = 0;
  SavepointMutationKind kind = SavepointMutationKind::unknown;
  std::string stable_operation_id;
  bool durable_evidence_written = false;
  bool undo_evidence_available = false;
  bool rolled_back = false;
};

struct SavepointResult {
  Status status;
  SavepointRecord savepoint;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct SavepointMutationResult {
  Status status;
  SavepointMutationRecord mutation;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct SavepointRollbackPlan {
  Status status;
  SavepointRollbackDecision decision = SavepointRollbackDecision::invalid_request;
  SavepointRecord savepoint;
  std::vector<SavepointMutationRecord> rollback_actions;
  u64 affected_mutation_count = 0;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && decision == SavepointRollbackDecision::rollback_actions_ready;
  }
};

struct SavepointUndoResult {
  Status status;
  SavepointMutationRecord mutation;
  bool applied = false;
  bool already_applied = false;
  std::string executor_id;
  std::string durable_evidence_id;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && (applied || already_applied);
  }
};

struct SavepointRollbackExecutionResult {
  Status status;
  SavepointRollbackDecision decision = SavepointRollbackDecision::invalid_request;
  SavepointRecord savepoint;
  std::vector<SavepointUndoResult> undo_results;
  u64 affected_mutation_count = 0;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && decision == SavepointRollbackDecision::rollback_actions_ready;
  }
};

class SavepointPhysicalUndoExecutor {
 public:
  virtual ~SavepointPhysicalUndoExecutor() = default;
  virtual bool Supports(SavepointMutationKind kind) const = 0;
  virtual SavepointUndoResult ApplyUndo(const SavepointMutationRecord& mutation) = 0;
};

class SavepointStack {
 public:
  SavepointResult Create(LocalTransactionId local_id, std::string name, u64 mutation_sequence);
  SavepointResult Release(LocalTransactionId local_id, std::string name);
  SavepointResult RollbackTo(LocalTransactionId local_id, std::string name);
  SavepointMutationResult RecordMutation(SavepointMutationRecord mutation);
  SavepointRollbackPlan PlanRollbackTo(LocalTransactionId local_id, std::string name) const;
  SavepointRollbackPlan ApplyRollbackTo(LocalTransactionId local_id, std::string name);
  SavepointRollbackExecutionResult ExecuteRollbackTo(LocalTransactionId local_id,
                                                     std::string name,
                                                     SavepointPhysicalUndoExecutor* executor);
  u64 size() const;
  u64 mutation_count() const;

 private:
  std::vector<SavepointRecord> savepoints_;
  std::vector<SavepointMutationRecord> mutations_;
  u64 next_ordinal_ = 1;

  void MarkRollbackApplied(LocalTransactionId local_id, u64 savepoint_mutation_sequence);
};

const char* SavepointMutationKindName(SavepointMutationKind kind);
const char* SavepointRollbackDecisionName(SavepointRollbackDecision decision);
DiagnosticRecord MakeSavepointDiagnostic(Status status,
                                         std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail = {});

}  // namespace scratchbird::transaction::mga
