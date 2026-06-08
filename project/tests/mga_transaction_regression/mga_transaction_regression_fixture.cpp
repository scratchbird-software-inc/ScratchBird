// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "row_version.hpp"
#include "savepoint.hpp"
#include "transaction_lock.hpp"
#include "transaction_manager.hpp"
#include "transaction_recovery.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::u64;
using scratchbird::core::uuid::GenerateEngineIdentityV7;
using namespace scratchbird::transaction::mga;

struct CheckSink {
  bool ok = true;

  void Require(bool condition, std::string_view name) {
    std::cout << "  \"" << name << "\": " << (condition ? "true" : "false") << ",\n";
    ok = ok && condition;
  }
};

class RegressionSavepointUndoExecutor final : public SavepointPhysicalUndoExecutor {
 public:
  bool Supports(SavepointMutationKind kind) const override {
    return kind == SavepointMutationKind::data_page ||
           kind == SavepointMutationKind::index;
  }

  SavepointUndoResult ApplyUndo(const SavepointMutationRecord& mutation) override {
    SavepointUndoResult result;
    result.status = {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
    result.mutation = mutation;
    result.applied = true;
    result.executor_id = "mga_regression_savepoint_undo_executor";
    result.durable_evidence_id = "mga_regression:" + mutation.stable_operation_id;
    return result;
  }
};

TypedUuid GenerateTyped(UuidKind kind, u64 seed) {
  const auto generated = GenerateEngineIdentityV7(kind, seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

LocalTransactionManagerResult Begin(LocalTransactionManager* manager, u64 ordinal) {
  return manager->Begin(GenerateTyped(UuidKind::transaction, 1779001000000ull + ordinal),
                        1779002000000ull + ordinal);
}

RowIdentity GenerateRow(u64 ordinal) {
  const auto row = MakeRowIdentity(GenerateTyped(UuidKind::row, 1779003000000ull + ordinal));
  return row.identity;
}

RowVersionMetadata VersionFor(const TransactionInventoryEntry& entry,
                              const RowIdentity& row,
                              u64 sequence,
                              RowVersionState version_state,
                              TransactionState transaction_state,
                              bool payload_present = true) {
  RowVersionMetadata metadata;
  metadata.identity.row = row;
  metadata.identity.creator_transaction = entry.identity;
  metadata.identity.version_sequence = sequence;
  metadata.state = version_state;
  metadata.creator_transaction_state = transaction_state;
  metadata.payload_present = payload_present;
  return metadata;
}

TransactionInventoryEntry RecoveryEntry(u64 id,
                                        TransactionState state,
                                        bool evidence_written,
                                        bool rollback_only = false) {
  TransactionInventoryEntry entry;
  entry.identity.local_id = MakeLocalTransactionId(id);
  entry.identity.transaction_uuid = GenerateTyped(UuidKind::transaction, 1779004000000ull + id);
  entry.identity.scope = TransactionScope::local_node;
  entry.state = state;
  entry.evidence_record_required = true;
  entry.evidence_record_written = evidence_written;
  entry.rollback_only = rollback_only;
  return entry;
}

bool CaseInventoryVisibility() {
  CheckSink checks;
  LocalTransactionManager manager;

  const auto tx1 = Begin(&manager, 1);
  const auto tx2 = Begin(&manager, 2);
  const auto duplicate = manager.Begin(tx1.entry.identity.transaction_uuid, 1779002000100ull);
  const auto snapshot1 = manager.Snapshot(tx1.entry.identity.local_id);
  const auto snapshot2 = manager.Snapshot(tx2.entry.identity.local_id);
  const RowIdentity row = GenerateRow(1);
  const auto own_uncommitted = EvaluateVisibility(
      VersionFor(tx1.entry, row, 1, RowVersionState::uncommitted, TransactionState::active),
      snapshot1.visibility_snapshot);
  const auto peer_uncommitted = EvaluateVisibility(
      VersionFor(tx1.entry, row, 1, RowVersionState::uncommitted, TransactionState::active),
      snapshot2.visibility_snapshot);
  const auto commit1 = manager.Commit(tx1.entry.identity.local_id, 1779002000200ull);
  const auto tx3 = Begin(&manager, 3);
  const auto snapshot3 = manager.Snapshot(tx3.entry.identity.local_id);
  const auto committed_visible = EvaluateVisibility(
      VersionFor(commit1.entry, row, 1, RowVersionState::committed, TransactionState::committed),
      snapshot3.visibility_snapshot);
  const auto tx4 = Begin(&manager, 4);
  const auto rollback4 = manager.Rollback(tx4.entry.identity.local_id, 1779002000300ull);
  const auto tx5 = Begin(&manager, 5);
  const auto snapshot5 = manager.Snapshot(tx5.entry.identity.local_id);
  const auto rolled_back_invisible = EvaluateVisibility(
      VersionFor(rollback4.entry, GenerateRow(2), 2, RowVersionState::rolled_back, TransactionState::rolled_back, false),
      snapshot5.visibility_snapshot);
  const auto commit2 = manager.Commit(tx2.entry.identity.local_id, 1779002000400ull);
  const auto rollback3 = manager.Rollback(tx3.entry.identity.local_id, 1779002000500ull);
  const auto rollback5 = manager.Rollback(tx5.entry.identity.local_id, 1779002000600ull);
  const auto metrics = manager.Metrics();

  checks.Require(tx1.ok() && tx2.ok() && tx3.ok() && tx4.ok() && tx5.ok(), "begin_transactions");
  checks.Require(!duplicate.ok() && duplicate.diagnostic.diagnostic_code == "SB-TXN-DUPLICATE-TRANSACTION-UUID",
                 "duplicate_uuid_rejected");
  checks.Require(snapshot1.ok() && snapshot2.ok() && snapshot3.ok() && snapshot5.ok(), "snapshots_created");
  checks.Require(own_uncommitted.ok() && own_uncommitted.decision == VisibilityDecision::visible,
                 "reader_own_uncommitted_visible");
  checks.Require(peer_uncommitted.decision == VisibilityDecision::wait_for_transaction,
                 "peer_waits_for_uncommitted_creator");
  checks.Require(commit1.ok() && committed_visible.ok() && committed_visible.decision == VisibilityDecision::visible,
                 "committed_version_visible_to_later_reader");
  checks.Require(rollback4.ok() && rolled_back_invisible.ok() &&
                 rolled_back_invisible.decision == VisibilityDecision::invisible,
                 "rolled_back_version_invisible");
  checks.Require(commit2.ok() && rollback3.ok() && rollback5.ok(), "cleanup_transactions_closed");
  checks.Require(metrics.begin_total == 5 && metrics.commit_total == 2 && metrics.rollback_total == 3 &&
                 metrics.failure_total == 1,
                 "transaction_metrics_match");
  return checks.ok;
}

bool CaseSavepointRollback() {
  CheckSink checks;
  LocalTransactionManager manager;
  const auto tx = Begin(&manager, 10);
  SavepointStack savepoints;
  const auto created = savepoints.Create(tx.entry.identity.local_id, "s1", 10);
  const auto mutation_a = savepoints.RecordMutation(
      {tx.entry.identity.local_id, 11, SavepointMutationKind::data_page, "regression.data", true, true, false});
  const auto mutation_b = savepoints.RecordMutation(
      {tx.entry.identity.local_id, 12, SavepointMutationKind::index, "regression.index", true, true, false});
  const auto plan = savepoints.PlanRollbackTo(tx.entry.identity.local_id, "s1");
  RegressionSavepointUndoExecutor executor;
  const auto applied = savepoints.ExecuteRollbackTo(tx.entry.identity.local_id, "s1", &executor);
  const auto plan_after_apply = savepoints.PlanRollbackTo(tx.entry.identity.local_id, "s1");

  SavepointStack missing_undo;
  const auto missing_created = missing_undo.Create(tx.entry.identity.local_id, "s_missing", 20);
  const auto missing_mutation = missing_undo.RecordMutation(
      {tx.entry.identity.local_id, 21, SavepointMutationKind::catalog, "regression.catalog", true, false, false});
  const auto missing_plan = missing_undo.PlanRollbackTo(tx.entry.identity.local_id, "s_missing");

  SavepointStack release_stack;
  const auto release_s1 = release_stack.Create(tx.entry.identity.local_id, "outer", 1);
  const auto release_s2 = release_stack.Create(tx.entry.identity.local_id, "inner", 2);
  const auto released = release_stack.Release(tx.entry.identity.local_id, "outer");

  checks.Require(tx.ok(), "transaction_started");
  checks.Require(created.ok() && mutation_a.ok() && mutation_b.ok(), "mutations_recorded");
  checks.Require(plan.ok() && plan.affected_mutation_count == 2 && plan.rollback_actions.size() == 2 &&
                 plan.rollback_actions[0].mutation_sequence == 12 &&
                 plan.rollback_actions[1].mutation_sequence == 11,
                 "rollback_plan_reverse_mutation_order");
  checks.Require(applied.ok() && applied.affected_mutation_count == 2 &&
                 applied.undo_results.size() == 2,
                 "rollback_physical_undo_succeeds");
  checks.Require(plan_after_apply.ok() && plan_after_apply.affected_mutation_count == 0,
                 "rollback_apply_marks_mutations_rolled_back");
  checks.Require(missing_created.ok() && missing_mutation.ok() && !missing_plan.ok() &&
                 missing_plan.decision == SavepointRollbackDecision::rollback_refused_missing_undo &&
                 missing_plan.diagnostic.diagnostic_code == "SB-SNTXN-SAVEPOINT-ROLLBACK-MISSING-UNDO",
                 "missing_undo_refuses_savepoint_rollback");
  checks.Require(release_s1.ok() && release_s2.ok() && released.ok() && release_stack.size() == 0,
                 "release_discards_nested_savepoints");
  return checks.ok;
}

bool CaseLockConcurrency() {
  CheckSink checks;
  LocalTransactionLockTable shared_locks;
  const auto shared_a = shared_locks.Acquire({MakeLocalTransactionId(1), "row:shared", TransactionLockMode::shared, {0, true}});
  const auto shared_b = shared_locks.Acquire({MakeLocalTransactionId(2), "row:shared", TransactionLockMode::shared, {0, true}});
  const auto exclusive_wait = shared_locks.Acquire(
      {MakeLocalTransactionId(3), "row:shared", TransactionLockMode::exclusive, {100, false, 10, 10}});
  const auto no_wait_timeout = shared_locks.Acquire(
      {MakeLocalTransactionId(4), "row:shared", TransactionLockMode::exclusive, {0, true}});

  LocalTransactionLockTable deadlock_locks;
  const auto lock_a = deadlock_locks.Acquire({MakeLocalTransactionId(10), "row:a", TransactionLockMode::exclusive, {0, true}});
  const auto lock_b = deadlock_locks.Acquire({MakeLocalTransactionId(11), "row:b", TransactionLockMode::exclusive, {0, true}});
  const auto wait_a = deadlock_locks.Acquire(
      {MakeLocalTransactionId(10), "row:b", TransactionLockMode::exclusive, {100, false, 20, 20}});
  const auto deadlock = deadlock_locks.Acquire(
      {MakeLocalTransactionId(11), "row:a", TransactionLockMode::exclusive, {100, false, 20, 20}});

  LocalTransactionLockTable admission_locks;
  admission_locks.SetAdmissionPolicy({true, 1, 0});
  const auto admission_first = admission_locks.Acquire(
      {MakeLocalTransactionId(20), "row:admit-a", TransactionLockMode::exclusive, {0, true}});
  const auto admission_refused = admission_locks.Acquire(
      {MakeLocalTransactionId(21), "row:admit-b", TransactionLockMode::exclusive, {0, true}});

  checks.Require(shared_a.ok() && shared_b.ok(), "shared_locks_coexist");
  checks.Require(exclusive_wait.decision == TransactionLockDecision::wait_required &&
                 exclusive_wait.blocking_transaction.value == 1,
                 "exclusive_waits_behind_shared_lock");
  checks.Require(no_wait_timeout.decision == TransactionLockDecision::timeout &&
                 no_wait_timeout.diagnostic.diagnostic_code == "SB-SNTXN-LOCK-TIMEOUT",
                 "nowait_conflict_times_out");
  checks.Require(lock_a.ok() && lock_b.ok() && wait_a.decision == TransactionLockDecision::wait_required &&
                 deadlock.decision == TransactionLockDecision::deadlock_detected &&
                 deadlock.diagnostic.diagnostic_code == "SB-SNTXN-DEADLOCK-DETECTED",
                 "deadlock_detected");
  checks.Require(admission_first.ok() && admission_refused.decision == TransactionLockDecision::admission_refused &&
                 admission_refused.diagnostic.diagnostic_code == "SB-SNTXN-LOCK-ADMISSION-REFUSED",
                 "lock_admission_policy_enforced");
  return checks.ok;
}

bool CaseRecoveryClassification() {
  CheckSink checks;
  LocalTransactionInventory inventory;
  inventory.next_local_transaction_id = 10;
  inventory.entries.push_back(RecoveryEntry(1, TransactionState::active, false));
  inventory.entries.push_back(RecoveryEntry(2, TransactionState::prepared, true));
  inventory.entries.push_back(RecoveryEntry(3, TransactionState::committing, true));
  inventory.entries.push_back(RecoveryEntry(4, TransactionState::rolling_back, false));
  inventory.entries.push_back(RecoveryEntry(5, TransactionState::committed, true));
  inventory.entries.push_back(RecoveryEntry(6, TransactionState::rolled_back, true));
  inventory.entries.push_back(RecoveryEntry(7, TransactionState::limbo, false));
  inventory.entries.push_back(RecoveryEntry(8, TransactionState::committing, false));
  inventory.entries.push_back(RecoveryEntry(9, TransactionState::active, false, true));

  const auto classified = ClassifyLocalTransactionInventoryForRecovery(inventory);
  const auto recovered = ApplyLocalTransactionInventoryRecovery(inventory, 1779005000000ull);

  u64 complete_rollback = 0;
  u64 complete_commit = 0;
  u64 prepared_waiting = 0;
  u64 no_action = 0;
  u64 fail_closed = 0;
  for (const auto& item : classified.classifications) {
    if (item.action == TransactionRecoveryAction::complete_rollback) { ++complete_rollback; }
    if (item.action == TransactionRecoveryAction::complete_commit) { ++complete_commit; }
    if (item.action == TransactionRecoveryAction::prepared_waiting_local_decision) { ++prepared_waiting; }
    if (item.action == TransactionRecoveryAction::no_action) { ++no_action; }
    if (item.fail_closed) { ++fail_closed; }
  }

  const auto lookup1 = LookupLocalTransaction(recovered.recovered_inventory, MakeLocalTransactionId(1));
  const auto lookup3 = LookupLocalTransaction(recovered.recovered_inventory, MakeLocalTransactionId(3));
  const auto lookup8 = LookupLocalTransaction(recovered.recovered_inventory, MakeLocalTransactionId(8));
  const auto lookup9 = LookupLocalTransaction(recovered.recovered_inventory, MakeLocalTransactionId(9));

  checks.Require(classified.ok() && classified.classifications.size() == 9, "recovery_classifies_all_entries");
  checks.Require(complete_rollback == 3 && complete_commit == 1 && prepared_waiting == 1 &&
                 no_action == 2 && fail_closed == 3,
                 "recovery_action_counts_match");
  checks.Require(recovered.ok() && recovered.inventory_changed && recovered.write_admission_must_remain_fenced,
                 "recovery_applies_safe_actions_and_fences_ambiguous");
  checks.Require(lookup1.ok() && lookup1.entry.state == TransactionState::rolled_back,
                 "active_transaction_recovered_as_rolled_back");
  checks.Require(lookup3.ok() && lookup3.entry.state == TransactionState::committed,
                 "committing_with_evidence_recovered_as_committed");
  checks.Require(lookup8.ok() && lookup8.entry.state == TransactionState::committing,
                 "committing_without_evidence_remains_fenced");
  checks.Require(lookup9.ok() && lookup9.entry.state == TransactionState::rolled_back,
                 "rollback_only_transaction_recovered_as_rolled_back");
  return checks.ok;
}

bool CaseStressInvariant() {
  struct VersionDelta {
    RowVersionMetadata metadata;
    int delta = 0;
  };

  CheckSink checks;
  LocalTransactionManager manager;
  std::vector<VersionDelta> versions;
  u64 committed_transactions = 0;
  for (u64 i = 1; i <= 120; ++i) {
    const auto tx = Begin(&manager, 1000 + i);
    if (!tx.ok()) {
      checks.Require(false, "stress_begin_failed");
      return false;
    }
    versions.push_back({VersionFor(tx.entry, GenerateRow(1000 + (i * 2)), i * 2,
                                   RowVersionState::uncommitted, TransactionState::active),
                        -1});
    versions.push_back({VersionFor(tx.entry, GenerateRow(1001 + (i * 2)), i * 2 + 1,
                                   RowVersionState::uncommitted, TransactionState::active),
                        1});
    const bool roll_back = (i % 5) == 0;
    if (roll_back) {
      const auto rollback = manager.Rollback(tx.entry.identity.local_id, 1779006000000ull + i);
      if (!rollback.ok()) {
        checks.Require(false, "stress_rollback_failed");
        return false;
      }
      for (auto& version : versions) {
        if (version.metadata.identity.creator_transaction.local_id.value == tx.entry.identity.local_id.value) {
          version.metadata.state = RowVersionState::rolled_back;
          version.metadata.creator_transaction_state = TransactionState::rolled_back;
          version.metadata.payload_present = false;
        }
      }
    } else {
      const auto commit = manager.Commit(tx.entry.identity.local_id, 1779006000000ull + i);
      if (!commit.ok()) {
        checks.Require(false, "stress_commit_failed");
        return false;
      }
      ++committed_transactions;
      for (auto& version : versions) {
        if (version.metadata.identity.creator_transaction.local_id.value == tx.entry.identity.local_id.value) {
          version.metadata.state = RowVersionState::committed;
          version.metadata.creator_transaction_state = TransactionState::committed;
        }
      }
    }
  }

  const auto reader = Begin(&manager, 2000);
  const auto snapshot = manager.Snapshot(reader.entry.identity.local_id);
  u64 visible_versions = 0;
  u64 invisible_versions = 0;
  int visible_delta_sum = 0;
  for (const auto& version : versions) {
    const auto visibility = EvaluateVisibility(version.metadata, snapshot.visibility_snapshot);
    if (visibility.decision == VisibilityDecision::visible) {
      ++visible_versions;
      visible_delta_sum += version.delta;
    } else if (visibility.decision == VisibilityDecision::invisible) {
      ++invisible_versions;
    } else {
      checks.Require(false, "stress_unexpected_visibility_decision");
      return false;
    }
  }
  const auto reader_rollback = manager.Rollback(reader.entry.identity.local_id, 1779007000000ull);
  const auto horizons = manager.Horizons();

  checks.Require(snapshot.ok(), "stress_reader_snapshot_created");
  checks.Require(visible_versions == committed_transactions * 2, "stress_committed_versions_visible");
  checks.Require(invisible_versions == (120 - committed_transactions) * 2, "stress_rolled_back_versions_invisible");
  checks.Require(visible_delta_sum == 0, "stress_ledger_delta_invariant_preserved");
  checks.Require(reader_rollback.ok() && horizons.ok(), "stress_reader_closed_and_horizons_valid");
  return checks.ok;
}

bool RunCase(std::string_view name) {
  std::cout << "{\n";
  std::cout << "  \"case\": \"" << name << "\",\n";
  bool ok = false;
  if (name == "inventory_visibility") {
    ok = CaseInventoryVisibility();
  } else if (name == "savepoint_rollback") {
    ok = CaseSavepointRollback();
  } else if (name == "lock_concurrency") {
    ok = CaseLockConcurrency();
  } else if (name == "recovery_classification") {
    ok = CaseRecoveryClassification();
  } else if (name == "stress_invariant") {
    ok = CaseStressInvariant();
  } else {
    std::cout << "  \"unknown_case\": true,\n";
  }
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  std::string case_name = "all";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--case" && i + 1 < argc) {
      case_name = argv[++i];
    } else {
      std::cerr << "usage: mga_transaction_regression_fixture [--case CASE]\n";
      return 2;
    }
  }

  if (case_name != "all") {
    return RunCase(case_name) ? 0 : 1;
  }

  bool ok = true;
  for (std::string_view name : {"inventory_visibility",
                                "savepoint_rollback",
                                "lock_concurrency",
                                "recovery_classification",
                                "stress_invariant"}) {
    ok = RunCase(name) && ok;
  }
  return ok ? 0 : 1;
}
