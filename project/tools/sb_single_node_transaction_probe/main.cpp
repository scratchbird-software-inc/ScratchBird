// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-SNTXN-PROBE-ANCHOR
#include "copy_on_write.hpp"
#include "database_lifecycle.hpp"
#include "isolation.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "savepoint.hpp"
#include "transaction_cleanup.hpp"
#include "transaction_lock.hpp"
#include "transaction_manager.hpp"
#include "transaction_policy.hpp"
#include "transaction_recovery.hpp"
#include "transaction_inventory_page.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::core::uuid::GenerateEngineIdentityV7;
using scratchbird::core::memory::ConfigureDefaultMemoryManager;
using scratchbird::core::memory::DefaultLocalEngineMemoryPolicy;
using scratchbird::storage::database::CreateDatabaseFile;
using scratchbird::storage::database::DatabaseCreateConfig;
using scratchbird::storage::database::DatabaseOpenConfig;
using scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase;
using scratchbird::storage::database::OpenDatabaseFile;
using scratchbird::storage::database::PersistLocalTransactionInventoryToDatabase;
using scratchbird::storage::page::MaxTransactionInventoryEntriesPerPage;
using namespace scratchbird::transaction::mga;

struct Args {
  std::string path;
  std::string seed_pack_root;
  u64 creation_millis = 0;
  u32 page_size = 16384;
  bool overwrite = false;
};

bool ParseU64(const std::string& text, u64* value) {
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') { return false; }
  *value = static_cast<u64>(parsed);
  return true;
}

bool ParseU32(const std::string& text, u32* value) {
  u64 parsed = 0;
  if (!ParseU64(text, &parsed) || parsed > 0xffffffffull) { return false; }
  *value = static_cast<u32>(parsed);
  return true;
}

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") { args->overwrite = true; continue; }
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--path") { args->path = value; }
    else if (key == "--seed-pack-root") { args->seed_pack_root = value; }
    else if (key == "--creation-ms") { if (!ParseU64(value, &args->creation_millis)) { return false; } }
    else if (key == "--page-size") { if (!ParseU32(value, &args->page_size)) { return false; } }
    else { return false; }
  }
  return !args->path.empty() && !args->seed_pack_root.empty() && args->creation_millis != 0;
}

TypedUuid GenerateTyped(UuidKind kind, u64 millis) {
  const auto generated = GenerateEngineIdentityV7(kind, millis);
  return generated.ok() ? generated.value : TypedUuid{};
}

void PrintDiagnostic(const DiagnosticRecord& diagnostic) {
  std::cerr << diagnostic.diagnostic_code << ":" << diagnostic.message_key << "\n";
}

class ProbeUndoExecutor final : public SavepointPhysicalUndoExecutor {
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
    result.executor_id = "sb_single_node_transaction_probe.undo_executor";
    result.durable_evidence_id =
        "probe.undo:" + std::to_string(mutation.local_id.value) + ":" +
        std::to_string(mutation.mutation_sequence);
    return result;
  }
};

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_single_node_transaction_probe --path PATH --seed-pack-root PATH --creation-ms MILLIS [--page-size BYTES] [--overwrite]\n";
    return 2;
  }

  auto memory_policy = DefaultLocalEngineMemoryPolicy();
  memory_policy.policy_name = "sb_single_node_transaction_probe";
  const auto memory_configured =
      ConfigureDefaultMemoryManager(memory_policy, "sb_single_node_transaction_probe");
  if (!memory_configured.ok()) {
    PrintDiagnostic(memory_configured.diagnostic);
    return 1;
  }

  const TypedUuid database_uuid = GenerateTyped(UuidKind::database, args.creation_millis + 7000);
  const TypedUuid filespace_uuid = GenerateTyped(UuidKind::filespace, args.creation_millis + 7001);
  const TypedUuid txn_uuid = GenerateTyped(UuidKind::transaction, args.creation_millis + 7010);
  const TypedUuid txn_uuid_2 = GenerateTyped(UuidKind::transaction, args.creation_millis + 7011);
  const TypedUuid row_uuid = GenerateTyped(UuidKind::row, args.creation_millis + 7020);

  DatabaseCreateConfig create;
  create.path = args.path;
  create.database_uuid = database_uuid;
  create.filespace_uuid = filespace_uuid;
  create.page_size = args.page_size;
  create.creation_unix_epoch_millis = args.creation_millis;
  create.resource_seed_pack_root = args.seed_pack_root;
  create.allow_overwrite = args.overwrite;
  const auto created = CreateDatabaseFile(create);
  if (!created.ok()) { PrintDiagnostic(created.diagnostic); return 1; }

  DatabaseOpenConfig activation_open;
  activation_open.path = args.path;
  const auto activated = OpenDatabaseFile(activation_open);
  if (!activated.ok()) { PrintDiagnostic(activated.diagnostic); return 1; }

  LocalTransactionManager manager(activated.state.local_transaction_inventory);
  const auto begin = manager.Begin(txn_uuid, args.creation_millis + 10);
  if (!begin.ok()) { PrintDiagnostic(begin.diagnostic); return 1; }
  const auto persisted_begin = PersistLocalTransactionInventoryToDatabase(args.path, manager.inventory());
  if (!persisted_begin.ok()) { PrintDiagnostic(persisted_begin.diagnostic); return 1; }
  const auto loaded_begin = LoadLocalTransactionInventoryFromDatabase(args.path);
  if (!loaded_begin.ok()) { PrintDiagnostic(loaded_begin.diagnostic); return 1; }

  const auto snapshot = manager.Snapshot(begin.entry.identity.local_id);
  if (!snapshot.ok()) { PrintDiagnostic(snapshot.diagnostic); return 1; }
  const auto isolation = ValidateLocalIsolationLevel(IsolationLevel::serializable);
  if (!isolation.ok()) { PrintDiagnostic(isolation.diagnostic); return 1; }

  const auto row_identity = MakeRowIdentity(row_uuid);
  if (!row_identity.ok()) { PrintDiagnostic(row_identity.diagnostic); return 1; }
  const auto cow = PlanLocalCopyOnWriteMutationForTransaction(begin.entry,
                                                             row_identity.identity,
                                                             CopyOnWriteMutationKind::insert,
                                                             0,
                                                             1);
  if (!cow.ok()) { PrintDiagnostic(cow.diagnostic); return 1; }

  RowVersionMetadata metadata;
  metadata.identity.row = row_identity.identity;
  metadata.identity.creator_transaction = begin.entry.identity;
  metadata.identity.version_sequence = 1;
  metadata.state = RowVersionState::uncommitted;
  metadata.creator_transaction_state = TransactionState::active;
  metadata.payload_present = true;
  const auto visibility = EvaluateVisibility(metadata, snapshot.visibility_snapshot);
  if (!visibility.ok()) { PrintDiagnostic(visibility.diagnostic); return 1; }

  LocalTransactionLockTable locks;
  const auto lock_a = locks.Acquire({begin.entry.identity.local_id, "row:1", TransactionLockMode::exclusive, {0, true}});
  if (!lock_a.ok()) { PrintDiagnostic(lock_a.diagnostic); return 1; }
  const auto begin_2 = manager.Begin(txn_uuid_2, args.creation_millis + 11);
  if (!begin_2.ok()) { PrintDiagnostic(begin_2.diagnostic); return 1; }
  const auto lock_timeout = locks.Acquire({begin_2.entry.identity.local_id, "row:1", TransactionLockMode::exclusive, {0, true}});
  const bool timeout_ok = !lock_timeout.ok() && lock_timeout.diagnostic.diagnostic_code == "SB-SNTXN-LOCK-TIMEOUT";
  const LocalTransactionId lock_waiter = MakeLocalTransactionId(100);
  const auto lock_wait = locks.Acquire(
      {lock_waiter, "row:1", TransactionLockMode::exclusive, {100, false, args.creation_millis + 30, args.creation_millis + 50}});
  const bool wait_required_ok = !lock_wait.ok() &&
                                lock_wait.decision == TransactionLockDecision::wait_required &&
                                lock_wait.blocking_transaction.value == begin.entry.identity.local_id.value &&
                                lock_wait.wait_elapsed_millis == 20 &&
                                lock_wait.retry_after_millis == 80 &&
                                locks.waiting_lock_count() == 1;
  const auto lock_release = locks.Release(begin.entry.identity.local_id, "row:1");
  if (!lock_release.ok()) { PrintDiagnostic(lock_release.diagnostic); return 1; }
  const auto lock_retry = locks.Retry(lock_waiter);
  const bool retry_ok = lock_retry.ok() && lock_retry.decision == TransactionLockDecision::granted;
  const bool release_all_ok = locks.ReleaseAll(lock_waiter) == 1 && locks.held_lock_count() == 0;
  LocalTransactionLockTable shared_locks;
  const LocalTransactionId lock_t10 = MakeLocalTransactionId(110);
  const LocalTransactionId lock_t11 = MakeLocalTransactionId(111);
  const LocalTransactionId lock_t12 = MakeLocalTransactionId(112);
  const auto shared_a = shared_locks.Acquire({lock_t10, "row:shared", TransactionLockMode::shared, {0, true}});
  const auto shared_b = shared_locks.Acquire({lock_t11, "row:shared", TransactionLockMode::shared, {0, true}});
  const auto shared_exclusive_wait = shared_locks.Acquire(
      {lock_t12, "row:shared", TransactionLockMode::exclusive, {50, false, args.creation_millis + 60, args.creation_millis + 60}});
  const bool shared_mode_ok = shared_a.ok() && shared_b.ok() &&
                              !shared_exclusive_wait.ok() &&
                              shared_exclusive_wait.decision == TransactionLockDecision::wait_required;
  LocalTransactionLockTable deadlock_locks;
  const LocalTransactionId deadlock_a = MakeLocalTransactionId(120);
  const LocalTransactionId deadlock_b = MakeLocalTransactionId(121);
  if (!deadlock_locks.Acquire({deadlock_a, "row:a", TransactionLockMode::exclusive, {0, true}}).ok()) { return 1; }
  if (!deadlock_locks.Acquire({deadlock_b, "row:b", TransactionLockMode::exclusive, {0, true}}).ok()) { return 1; }
  const auto deadlock_wait = deadlock_locks.Acquire(
      {deadlock_a, "row:b", TransactionLockMode::exclusive, {100, false, args.creation_millis + 70, args.creation_millis + 70}});
  const auto deadlock_detected = deadlock_locks.Acquire(
      {deadlock_b, "row:a", TransactionLockMode::exclusive, {100, false, args.creation_millis + 70, args.creation_millis + 70}});
  const bool deadlock_ok = deadlock_wait.decision == TransactionLockDecision::wait_required &&
                           deadlock_detected.decision == TransactionLockDecision::deadlock_detected &&
                           deadlock_detected.diagnostic.diagnostic_code == "SB-SNTXN-DEADLOCK-DETECTED";
  LocalTransactionLockTable admission_locks;
  admission_locks.SetAdmissionPolicy({true, 1, 0});
  const bool admission_first_ok = admission_locks.Acquire({lock_t10, "row:admit-a", TransactionLockMode::exclusive, {0, true}}).ok();
  const auto admission_refused = admission_locks.Acquire({lock_t11, "row:admit-b", TransactionLockMode::exclusive, {0, true}});
  const bool admission_refused_ok = admission_first_ok &&
                                    admission_refused.decision == TransactionLockDecision::admission_refused &&
                                    admission_refused.diagnostic.diagnostic_code == "SB-SNTXN-LOCK-ADMISSION-REFUSED";
  LocalTransactionLockTable wait_admission_locks;
  wait_admission_locks.SetAdmissionPolicy({false, 0, 0});
  if (!wait_admission_locks.Acquire({lock_t10, "row:wait-admit", TransactionLockMode::exclusive, {0, true}}).ok()) { return 1; }
  const auto wait_admission_refused = wait_admission_locks.Acquire(
      {lock_t11, "row:wait-admit", TransactionLockMode::exclusive, {100, false, args.creation_millis + 80, args.creation_millis + 80}});
  const bool wait_admission_refused_ok =
      wait_admission_refused.decision == TransactionLockDecision::admission_refused &&
      wait_admission_refused.diagnostic.diagnostic_code == "SB-SNTXN-LOCK-ADMISSION-REFUSED";

  SavepointStack savepoints;
  const auto savepoint = savepoints.Create(begin.entry.identity.local_id, "s1", 1);
  if (!savepoint.ok()) { PrintDiagnostic(savepoint.diagnostic); return 1; }
  const auto data_mutation = savepoints.RecordMutation(
      {begin.entry.identity.local_id, 2, SavepointMutationKind::data_page, "probe.data.insert", true, true, false});
  if (!data_mutation.ok()) { PrintDiagnostic(data_mutation.diagnostic); return 1; }
  const auto index_mutation = savepoints.RecordMutation(
      {begin.entry.identity.local_id, 3, SavepointMutationKind::index, "probe.index.insert", true, true, false});
  if (!index_mutation.ok()) { PrintDiagnostic(index_mutation.diagnostic); return 1; }
  const auto rollback_plan = savepoints.PlanRollbackTo(begin.entry.identity.local_id, "s1");
  const bool savepoint_plan_ok = rollback_plan.ok() &&
                                 rollback_plan.affected_mutation_count == 2 &&
                                 rollback_plan.rollback_actions.size() == 2 &&
                                 rollback_plan.rollback_actions.front().mutation_sequence == 3 &&
                                 rollback_plan.rollback_actions.back().mutation_sequence == 2;
  ProbeUndoExecutor undo_executor;
  const auto rollback_apply =
      savepoints.ExecuteRollbackTo(begin.entry.identity.local_id, "s1", &undo_executor);
  const bool savepoint_apply_ok = rollback_apply.ok() &&
                                  rollback_apply.affected_mutation_count == 2 &&
                                  rollback_apply.undo_results.size() == 2;
  const auto rollback_to = savepoints.RollbackTo(begin.entry.identity.local_id, "s1");
  if (!rollback_to.ok()) { PrintDiagnostic(rollback_to.diagnostic); return 1; }
  SavepointStack missing_undo_savepoints;
  if (!missing_undo_savepoints.Create(begin.entry.identity.local_id, "missing_undo", 10).ok()) { return 1; }
  const auto missing_undo_recorded = missing_undo_savepoints.RecordMutation(
      {begin.entry.identity.local_id, 11, SavepointMutationKind::catalog, "probe.catalog.partial_failure", true, false, false});
  if (!missing_undo_recorded.ok()) { PrintDiagnostic(missing_undo_recorded.diagnostic); return 1; }
  const auto missing_undo_plan = missing_undo_savepoints.PlanRollbackTo(begin.entry.identity.local_id, "missing_undo");
  const bool savepoint_missing_undo_refused_ok =
      !missing_undo_plan.ok() &&
      missing_undo_plan.decision == SavepointRollbackDecision::rollback_refused_missing_undo &&
      missing_undo_plan.diagnostic.diagnostic_code == "SB-SNTXN-SAVEPOINT-ROLLBACK-MISSING-UNDO";
  SavepointStack invalid_mutation_savepoints;
  const auto invalid_mutation = invalid_mutation_savepoints.RecordMutation(
      {begin.entry.identity.local_id, 12, SavepointMutationKind::overflow, "probe.overflow.no_evidence", false, true, false});
  const bool savepoint_invalid_mutation_refused_ok =
      !invalid_mutation.ok() &&
      invalid_mutation.diagnostic.diagnostic_code == "SB-SNTXN-SAVEPOINT-MUTATION-INVALID";

  TransactionRuntimePolicy policy;
  policy.max_active_millis = 1;
  const auto policy_result = EvaluateTransactionRuntimePolicy(begin.entry, policy, args.creation_millis + 1000, args.creation_millis + 999);
  const bool policy_violation_ok = !policy_result.ok() &&
                                   policy_result.diagnostic.diagnostic_code == "SB-SNTXN-LONG-RUNNING-POLICY-VIOLATION";
  TransactionRuntimePolicy idle_policy;
  idle_policy.max_idle_millis = 1;
  const auto idle_policy_result = EvaluateTransactionRuntimePolicy(begin.entry, idle_policy, args.creation_millis + 1000, args.creation_millis + 10);
  const bool idle_policy_violation_ok = !idle_policy_result.ok() &&
                                        idle_policy_result.diagnostic.diagnostic_code == "SB-SNTXN-IDLE-POLICY-VIOLATION";

  const auto commit = manager.Commit(begin.entry.identity.local_id, args.creation_millis + 20);
  if (!commit.ok()) { PrintDiagnostic(commit.diagnostic); return 1; }
  const auto rollback = manager.Rollback(begin_2.entry.identity.local_id, args.creation_millis + 21);
  if (!rollback.ok()) { PrintDiagnostic(rollback.diagnostic); return 1; }
  const auto persisted_final = PersistLocalTransactionInventoryToDatabase(args.path, manager.inventory());
  if (!persisted_final.ok()) { PrintDiagnostic(persisted_final.diagnostic); return 1; }
  const auto loaded_final = LoadLocalTransactionInventoryFromDatabase(args.path);
  if (!loaded_final.ok()) { PrintDiagnostic(loaded_final.diagnostic); return 1; }

  const u32 inventory_page_capacity = MaxTransactionInventoryEntriesPerPage(args.page_size);
  const std::size_t overflow_target = static_cast<std::size_t>(inventory_page_capacity) + 8;
  while (manager.inventory().entries.size() < overflow_target) {
    const u64 ordinal = static_cast<u64>(manager.inventory().entries.size());
    const TypedUuid extra_txn_uuid = GenerateTyped(UuidKind::transaction, args.creation_millis + 8100 + ordinal);
    const auto extra_begin = manager.Begin(extra_txn_uuid, args.creation_millis + 100 + ordinal);
    if (!extra_begin.ok()) { PrintDiagnostic(extra_begin.diagnostic); return 1; }
    const auto extra_commit = manager.Commit(extra_begin.entry.identity.local_id, args.creation_millis + 200 + ordinal);
    if (!extra_commit.ok()) { PrintDiagnostic(extra_commit.diagnostic); return 1; }
  }
  const auto persisted_multipage = PersistLocalTransactionInventoryToDatabase(args.path, manager.inventory());
  if (!persisted_multipage.ok()) { PrintDiagnostic(persisted_multipage.diagnostic); return 1; }
  const auto loaded_multipage = LoadLocalTransactionInventoryFromDatabase(args.path);
  if (!loaded_multipage.ok()) { PrintDiagnostic(loaded_multipage.diagnostic); return 1; }
  TransactionInventoryCompactionRequest compaction_request;
  compaction_request.inventory = loaded_multipage.inventory;
  compaction_request.inventory_authoritative = true;
  compaction_request.oldest_required_local_transaction_id = MakeLocalTransactionId(3);
  if (compaction_request.inventory.entries.size() >= 2) {
    compaction_request.inventory.entries[0].state = TransactionState::archived;
    compaction_request.inventory.entries[1].state = TransactionState::archived;
  }
  const auto compacted_inventory = CompactLocalTransactionInventory(compaction_request);
  if (!compacted_inventory.ok()) { PrintDiagnostic(compacted_inventory.diagnostic); return 1; }
  TransactionInventoryCompactionRequest non_authoritative_compaction = compaction_request;
  non_authoritative_compaction.inventory_authoritative = false;
  const auto refused_compaction = CompactLocalTransactionInventory(non_authoritative_compaction);
  const bool inventory_compaction_ok =
      compacted_inventory.compacted_entry_count == 2 &&
      compacted_inventory.inventory.next_local_transaction_id == loaded_multipage.inventory.next_local_transaction_id &&
      compacted_inventory.inventory.entries.size() + 2 == loaded_multipage.inventory.entries.size();
  const bool inventory_compaction_refused_ok =
      !refused_compaction.ok() &&
      refused_compaction.diagnostic.diagnostic_code == "SB-TXN-INVENTORY-COMPACTION-NOT-AUTHORITATIVE";

  metadata.state = RowVersionState::committed;
  metadata.creator_transaction_state = TransactionState::committed;
  const auto cleanup = EvaluateLocalCleanupWithHorizons(metadata, loaded_final.horizons);
  if (!cleanup.ok()) { PrintDiagnostic(cleanup.diagnostic); return 1; }
  RowVersionMetadata rolled_back_metadata = metadata;
  rolled_back_metadata.state = RowVersionState::rolled_back;
  rolled_back_metadata.creator_transaction_state = TransactionState::rolled_back;
  rolled_back_metadata.payload_present = false;
  RowVersionMetadata obsolete_metadata = metadata;
  obsolete_metadata.chain.next_version_sequence = 2;
  obsolete_metadata.successor_transaction_local_id = commit.entry.identity.local_id;
  LocalCleanupWorksetRequest cleanup_request;
  cleanup_request.inventory = loaded_final.inventory;
  cleanup_request.inventory_authoritative = true;
  cleanup_request.row_versions = {rolled_back_metadata, obsolete_metadata, metadata};
  const auto cleanup_workset = ApplyLocalCleanupWithAuthoritativeInventory(cleanup_request);
  if (!cleanup_workset.ok()) { PrintDiagnostic(cleanup_workset.diagnostic); return 1; }
  LocalCleanupWorksetRequest snapshot_blocked_cleanup = cleanup_request;
  snapshot_blocked_cleanup.active_snapshot_horizons = {begin.entry.identity.local_id};
  const auto snapshot_cleanup_workset = ApplyLocalCleanupWithAuthoritativeInventory(snapshot_blocked_cleanup);
  if (!snapshot_cleanup_workset.ok()) { PrintDiagnostic(snapshot_cleanup_workset.diagnostic); return 1; }
  const bool cleanup_snapshot_hold_blocks_ok =
      snapshot_cleanup_workset.reclaimed_row_version_count == 0 &&
      snapshot_cleanup_workset.retained_row_version_count == cleanup_request.row_versions.size() &&
      snapshot_cleanup_workset.horizon_blocked_row_version_count == cleanup_request.row_versions.size();
  LocalCleanupWorksetRequest archive_blocked_cleanup = cleanup_request;
  archive_blocked_cleanup.additional_holds.push_back(
      {CleanupHoldKind::archive_required, begin.entry.identity.local_id, true, "probe_archive_hold"});
  const auto archive_cleanup_workset = ApplyLocalCleanupWithAuthoritativeInventory(archive_blocked_cleanup);
  if (!archive_cleanup_workset.ok()) { PrintDiagnostic(archive_cleanup_workset.diagnostic); return 1; }
  const bool cleanup_archive_hold_blocks_ok =
      archive_cleanup_workset.reclaimed_row_version_count == 0 &&
      archive_cleanup_workset.retained_row_version_count == cleanup_request.row_versions.size() &&
      archive_cleanup_workset.horizon_blocked_row_version_count == cleanup_request.row_versions.size();
  LocalCleanupWorksetRequest non_authoritative_hold_cleanup = cleanup_request;
  non_authoritative_hold_cleanup.additional_holds.push_back(
      {CleanupHoldKind::admin_hold, begin.entry.identity.local_id, false, "probe_projected_admin_hold"});
  const auto non_authoritative_hold_refused = ApplyLocalCleanupWithAuthoritativeInventory(non_authoritative_hold_cleanup);
  const bool cleanup_non_authoritative_hold_refused_ok = !non_authoritative_hold_refused.ok() &&
      non_authoritative_hold_refused.diagnostic.diagnostic_code == "SB-SNTXN-CLEANUP-HOLD-NOT-AUTHORITATIVE";
  cleanup_request.inventory_authoritative = false;
  const auto refused_cleanup = ApplyLocalCleanupWithAuthoritativeInventory(cleanup_request);
  const bool cleanup_authority_refused_ok = !refused_cleanup.ok() &&
      refused_cleanup.diagnostic.diagnostic_code == "SB-SNTXN-CLEANUP-HORIZON-NOT-AUTHORITATIVE";

  const auto recovery = ClassifyLocalTransactionInventoryForRecovery(manager.inventory());
  if (!recovery.ok()) { PrintDiagnostic(recovery.diagnostic); return 1; }

  TransactionMetrics metrics;
  metrics.RecordBegin();
  metrics.RecordCommit();
  const auto samples = metrics.MetricSamples();

  DatabaseOpenConfig open;
  open.path = args.path;
  const auto opened = OpenDatabaseFile(open);
  if (!opened.ok()) { PrintDiagnostic(opened.diagnostic); return 1; }

  const bool ok = loaded_begin.inventory.entries.size() == 3 &&
                  loaded_final.inventory.entries.size() == 4 &&
                  loaded_multipage.inventory.entries.size() > inventory_page_capacity &&
                  inventory_compaction_ok &&
                  inventory_compaction_refused_ok &&
                  cleanup_workset.reclaimed_row_version_count == 2 &&
                  cleanup_workset.retained_row_version_count == 1 &&
                  cleanup_snapshot_hold_blocks_ok &&
                  cleanup_archive_hold_blocks_ok &&
                  cleanup_non_authoritative_hold_refused_ok &&
                  cleanup_authority_refused_ok &&
                  visibility.decision == VisibilityDecision::visible &&
                  timeout_ok && wait_required_ok && retry_ok && release_all_ok && shared_mode_ok &&
                  deadlock_ok && admission_refused_ok && wait_admission_refused_ok &&
                  savepoint_plan_ok && savepoint_apply_ok && savepoint_missing_undo_refused_ok &&
                  savepoint_invalid_mutation_refused_ok &&
                  policy_violation_ok && idle_policy_violation_ok &&
                  opened.state.local_transaction_inventory_present &&
                  samples.size() >= 7;

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"durable_begin_entries\": " << loaded_begin.inventory.entries.size() << ",\n";
  std::cout << "  \"durable_final_entries\": " << loaded_final.inventory.entries.size() << ",\n";
  std::cout << "  \"inventory_page_capacity\": " << inventory_page_capacity << ",\n";
  std::cout << "  \"durable_multipage_entries\": " << loaded_multipage.inventory.entries.size() << ",\n";
  std::cout << "  \"inventory_compacted_entries\": " << compacted_inventory.compacted_entry_count << ",\n";
  std::cout << "  \"inventory_compaction_refused\": " << (inventory_compaction_refused_ok ? "true" : "false") << ",\n";
  std::cout << "  \"visibility\": \"" << VisibilityDecisionName(visibility.decision) << "\",\n";
  std::cout << "  \"lock_timeout\": " << (timeout_ok ? "true" : "false") << ",\n";
  std::cout << "  \"lock_wait_required\": " << (wait_required_ok ? "true" : "false") << ",\n";
  std::cout << "  \"lock_retry_after_release\": " << (retry_ok ? "true" : "false") << ",\n";
  std::cout << "  \"lock_release_all\": " << (release_all_ok ? "true" : "false") << ",\n";
  std::cout << "  \"lock_shared_mode\": " << (shared_mode_ok ? "true" : "false") << ",\n";
  std::cout << "  \"lock_deadlock_detected\": " << (deadlock_ok ? "true" : "false") << ",\n";
  std::cout << "  \"lock_admission_refused\": " << (admission_refused_ok ? "true" : "false") << ",\n";
  std::cout << "  \"lock_wait_admission_refused\": " << (wait_admission_refused_ok ? "true" : "false") << ",\n";
  std::cout << "  \"savepoint_rollback_plan\": " << (savepoint_plan_ok ? "true" : "false") << ",\n";
  std::cout << "  \"savepoint_apply_rollback\": " << (savepoint_apply_ok ? "true" : "false") << ",\n";
  std::cout << "  \"savepoint_missing_undo_refused\": " << (savepoint_missing_undo_refused_ok ? "true" : "false") << ",\n";
  std::cout << "  \"savepoint_invalid_mutation_refused\": " << (savepoint_invalid_mutation_refused_ok ? "true" : "false") << ",\n";
  std::cout << "  \"policy_violation\": " << (policy_violation_ok ? "true" : "false") << ",\n";
  std::cout << "  \"idle_policy_violation\": " << (idle_policy_violation_ok ? "true" : "false") << ",\n";
  std::cout << "  \"savepoints\": " << savepoints.size() << ",\n";
  std::cout << "  \"cleanup_decision\": \"" << CleanupEligibilityDecisionName(cleanup.decision) << "\",\n";
  std::cout << "  \"cleanup_reclaimed_row_versions\": " << cleanup_workset.reclaimed_row_version_count << ",\n";
  std::cout << "  \"cleanup_retained_row_versions\": " << cleanup_workset.retained_row_version_count << ",\n";
  std::cout << "  \"cleanup_snapshot_hold_blocks\": " << (cleanup_snapshot_hold_blocks_ok ? "true" : "false") << ",\n";
  std::cout << "  \"cleanup_archive_hold_blocks\": " << (cleanup_archive_hold_blocks_ok ? "true" : "false") << ",\n";
  std::cout << "  \"cleanup_non_authoritative_hold_refused\": " << (cleanup_non_authoritative_hold_refused_ok ? "true" : "false") << ",\n";
  std::cout << "  \"cleanup_authority_refused\": " << (cleanup_authority_refused_ok ? "true" : "false") << ",\n";
  std::cout << "  \"recovery_classifications\": " << recovery.classifications.size() << ",\n";
  std::cout << "  \"metric_samples\": " << samples.size() << ",\n";
  std::cout << "  \"cluster_path_traversed\": false,\n";
  std::cout << "  \"database_inventory_present\": " << (opened.state.local_transaction_inventory_present ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
