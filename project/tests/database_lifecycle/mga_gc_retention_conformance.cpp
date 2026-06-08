// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_cleanup.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace mga = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

u64 NextMillis() {
  static u64 next = 1779501300000ull;
  return ++next;
}

TypedUuid NewUuid(UuidKind kind) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NextMillis());
  Require(generated.ok(), "DBLC-013AB UUID generation failed");
  return generated.value;
}

mga::TransactionIdentity NewTransactionIdentity(u64 local_id) {
  const auto identity = mga::MakeTransactionIdentity(mga::MakeLocalTransactionId(local_id),
                                                    NewUuid(UuidKind::transaction),
                                                    mga::TransactionScope::local_node);
  Require(identity.ok(), "DBLC-013AB transaction identity generation failed");
  return identity.identity;
}

mga::TransactionInventoryEntry InventoryEntry(u64 local_id, mga::TransactionState state) {
  mga::TransactionInventoryEntry entry;
  entry.identity = NewTransactionIdentity(local_id);
  entry.state = state;
  entry.begin_unix_epoch_millis = NextMillis();
  if (mga::IsTerminalTransactionState(state)) {
    entry.final_unix_epoch_millis = NextMillis();
    entry.evidence_record_written = true;
  }
  return entry;
}

mga::LocalTransactionInventory Inventory(std::vector<mga::TransactionInventoryEntry> entries) {
  mga::LocalTransactionInventory inventory;
  inventory.next_local_transaction_id = 20;
  inventory.entries = std::move(entries);
  return inventory;
}

mga::RowVersionMetadata Version(const mga::TransactionInventoryEntry& creator,
                                mga::RowVersionState state,
                                u64 sequence,
                                bool obsolete,
                                u64 successor_local_transaction_id = 0) {
  mga::RowVersionMetadata metadata;
  metadata.identity.row.row_uuid = NewUuid(UuidKind::row);
  metadata.identity.creator_transaction = creator.identity;
  metadata.identity.version_sequence = sequence;
  metadata.state = state;
  metadata.creator_transaction_state = creator.state;
  metadata.payload_present = state != mga::RowVersionState::rolled_back;
  if (obsolete) {
    metadata.chain.next_version_sequence = sequence + 100;
  }
  if (successor_local_transaction_id != 0) {
    metadata.successor_transaction_local_id =
        mga::MakeLocalTransactionId(successor_local_transaction_id);
  }
  return metadata;
}

mga::LocalCleanupWorksetRequest BaseWorkset(const mga::LocalTransactionInventory& inventory) {
  mga::LocalCleanupWorksetRequest request;
  request.inventory = inventory;
  request.inventory_authoritative = true;
  return request;
}

mga::LocalGarbageCollectionSweepResult Sweep(mga::LocalCleanupWorksetRequest workset,
                                             u64 max_candidates = 32,
                                             bool engine_authoritative = true) {
  mga::LocalGarbageCollectionSweepRequest request;
  request.workset = std::move(workset);
  request.family = mga::LocalCleanupSweepFamily::background;
  request.engine_mga_authoritative = engine_authoritative;
  request.max_candidate_row_versions = max_candidates;
  request.max_retained_row_versions = 0;
  request.retain_row_versions_in_result = false;
  return mga::RunLocalGarbageCollectionSweep(request);
}

void TestSuccessfulBoundedSweep(const mga::LocalTransactionInventory& inventory,
                                const mga::TransactionInventoryEntry& committed_old,
                                const mga::TransactionInventoryEntry& rolled_back,
                                const mga::TransactionInventoryEntry& committed_latest) {
  auto workset = BaseWorkset(inventory);
  workset.row_versions = {
      Version(rolled_back, mga::RowVersionState::rolled_back, 10, false),
      Version(committed_old,
              mga::RowVersionState::committed,
              20,
              true,
              committed_latest.identity.local_id.value),
      Version(committed_latest, mga::RowVersionState::committed, 30, false),
  };
  workset.history_retention_horizons.push_back(
      {mga::MakeLocalTransactionId(20), true, "release_complete_history_retention"});

  const auto result = Sweep(workset);
  Require(result.ok(), "DBLC-013AB bounded sweep should succeed");
  Require(result.scanned_row_version_count == 3, "DBLC-013AB sweep scan count mismatch");
  Require(result.cleanup.reclaimed_row_version_count == 2,
          "DBLC-013AB sweep did not reclaim eligible old versions");
  Require(result.cleanup.retained_row_version_count == 1,
          "DBLC-013AB sweep did not retain current visible version");
  Require(result.cleanup.retained_row_versions.empty(),
          "DBLC-013AB bounded sweep should not materialize retained vectors");
  Require(result.bounded_memory_enforced, "DBLC-013AB sweep did not enforce bounded memory");
}

void TestRetentionAndBackupArchiveHolds(const mga::LocalTransactionInventory& inventory,
                                        const mga::TransactionInventoryEntry& committed) {
  auto retention = BaseWorkset(inventory);
  retention.row_versions = {
      Version(committed,
              mga::RowVersionState::committed,
              40,
              true,
              committed.identity.local_id.value)};
  retention.history_retention_horizons.push_back(
      {committed.identity.local_id, true, "history_retention_hold"});
  const auto retention_result = Sweep(retention);
  Require(retention_result.ok(), "DBLC-013AB retention-blocked sweep should complete safely");
  Require(retention_result.cleanup.reclaimed_row_version_count == 0,
          "DBLC-013AB retention hold allowed reclaim");
  Require(retention_result.cleanup.retention_blocked_row_version_count == 1,
          "DBLC-013AB retention hold was not reported");

  auto archive = BaseWorkset(inventory);
  archive.row_versions = {
      Version(committed,
              mga::RowVersionState::committed,
              50,
              true,
              committed.identity.local_id.value)};
  archive.additional_holds.push_back(
      {mga::CleanupHoldKind::archive_required, committed.identity.local_id, true, "archive_hold"});
  const auto archive_result = Sweep(archive);
  Require(archive_result.ok(), "DBLC-013AB archive-blocked sweep should complete safely");
  Require(archive_result.cleanup.backup_archive_blocked_row_version_count == 1,
          "DBLC-013AB archive hold was not reported");
  Require(archive_result.cleanup.reclaimed_row_version_count == 0,
          "DBLC-013AB archive hold allowed reclaim");

  auto backup = BaseWorkset(inventory);
  backup.row_versions = {
      Version(committed,
              mga::RowVersionState::committed,
              60,
              true,
              committed.identity.local_id.value)};
  backup.additional_holds.push_back(
      {mga::CleanupHoldKind::backup_required, committed.identity.local_id, true, "backup_hold"});
  const auto backup_result = Sweep(backup);
  Require(backup_result.ok(), "DBLC-013AB backup-blocked sweep should complete safely");
  Require(backup_result.cleanup.backup_archive_blocked_row_version_count == 1,
          "DBLC-013AB backup hold was not reported");
  Require(backup_result.cleanup.reclaimed_row_version_count == 0,
          "DBLC-013AB backup hold allowed reclaim");
}

void TestLimboAndUnknownOutcomeProtection(const mga::LocalTransactionInventory& inventory,
                                          const mga::TransactionInventoryEntry& prepared,
                                          const mga::TransactionInventoryEntry& limbo) {
  const auto horizons = mga::ComputeLocalTransactionHorizons(inventory);
  Require(horizons.ok(), "DBLC-013AB horizon computation failed");

  const auto unknown = mga::EvaluateLocalCleanupWithHorizons(
      Version(prepared,
              mga::RowVersionState::committed,
              70,
              true,
              prepared.identity.local_id.value),
      horizons.horizons);
  Require(unknown.decision == mga::CleanupEligibilityDecision::blocked_by_recovery,
          "DBLC-013AB unknown outcome did not block cleanup");
  Require(unknown.diagnostic.diagnostic_code == "SB-SNTXN-CLEANUP-BLOCKED-BY-UNKNOWN-OUTCOME",
          "DBLC-013AB unknown outcome diagnostic mismatch");

  const auto limbo_decision = mga::EvaluateLocalCleanupWithHorizons(
      Version(limbo, mga::RowVersionState::limbo, 80, true),
      horizons.horizons);
  Require(limbo_decision.decision == mga::CleanupEligibilityDecision::blocked_by_limbo,
          "DBLC-013AB limbo version did not block cleanup");

  auto workset = BaseWorkset(inventory);
  workset.row_versions = {Version(prepared,
                                  mga::RowVersionState::committed,
                                  90,
                                  true,
                                  prepared.identity.local_id.value),
                          Version(limbo, mga::RowVersionState::limbo, 100, true)};
  const auto result = Sweep(workset);
  Require(result.ok(), "DBLC-013AB protected sweep should complete safely");
  Require(result.cleanup.reclaimed_row_version_count == 0,
          "DBLC-013AB unresolved outcomes allowed reclaim");
  Require(result.cleanup.limbo_or_unknown_outcome_blocked_row_version_count == 2,
          "DBLC-013AB unresolved outcome count mismatch");
}

void TestFailClosedDiagnostics(const mga::LocalTransactionInventory& inventory,
                               const mga::TransactionInventoryEntry& committed) {
  auto workset = BaseWorkset(inventory);
  workset.row_versions = {
      Version(committed,
              mga::RowVersionState::committed,
              110,
              true,
              committed.identity.local_id.value)};
  const auto external = Sweep(workset, 32, false);
  Require(!external.ok(), "DBLC-013AB external sweep authority was accepted");
  Require(external.diagnostic.diagnostic_code ==
              "SB-SNTXN-CLEANUP-SWEEP-NOT-ENGINE-AUTHORITATIVE",
          "DBLC-013AB external sweep diagnostic mismatch");

  auto too_many = BaseWorkset(inventory);
  too_many.row_versions = {
      Version(committed,
              mga::RowVersionState::committed,
              120,
              true,
              committed.identity.local_id.value),
      Version(committed,
              mga::RowVersionState::committed,
              130,
              true,
              committed.identity.local_id.value)};
  const auto bounded = Sweep(too_many, 1);
  Require(!bounded.ok(), "DBLC-013AB over-budget sweep was accepted");
  Require(bounded.diagnostic.diagnostic_code == "SB-SNTXN-CLEANUP-BOUNDED-SCAN-REQUIRED",
          "DBLC-013AB bounded scan diagnostic mismatch");

  mga::LocalGarbageCollectionSweepRequest retained_limit;
  retained_limit.workset = BaseWorkset(inventory);
  retained_limit.workset.row_versions = {
      Version(committed, mga::RowVersionState::committed, 140, false),
      Version(committed, mga::RowVersionState::committed, 150, false),
  };
  retained_limit.family = mga::LocalCleanupSweepFamily::background;
  retained_limit.engine_mga_authoritative = true;
  retained_limit.max_candidate_row_versions = 2;
  retained_limit.max_retained_row_versions = 1;
  retained_limit.retain_row_versions_in_result = true;
  const auto retained_limit_result = mga::RunLocalGarbageCollectionSweep(retained_limit);
  Require(!retained_limit_result.ok(), "DBLC-013AB retained-vector limit was accepted");
  Require(retained_limit_result.cleanup.bounded_memory_limit_hit,
          "DBLC-013AB retained-vector limit did not mark bounded memory hit");
  Require(retained_limit_result.diagnostic.diagnostic_code ==
              "SB-SNTXN-CLEANUP-RESULT-LIMIT-EXCEEDED",
          "DBLC-013AB retained-vector limit diagnostic mismatch");

  auto missing_creator = BaseWorkset(inventory);
  const auto outside_inventory = InventoryEntry(99, mga::TransactionState::committed);
  missing_creator.row_versions = {
      Version(outside_inventory, mga::RowVersionState::committed, 160, true),
  };
  const auto missing = Sweep(missing_creator);
  Require(!missing.ok(), "DBLC-013AB missing creator inventory was accepted");
  Require(missing.diagnostic.diagnostic_code == "SB-SNTXN-CLEANUP-CREATOR-NOT-IN-INVENTORY",
          "DBLC-013AB missing creator diagnostic mismatch");
}

}  // namespace

int main() {
  const auto committed_old = InventoryEntry(1, mga::TransactionState::committed);
  const auto rolled_back = InventoryEntry(2, mga::TransactionState::rolled_back);
  const auto committed_latest = InventoryEntry(3, mga::TransactionState::committed);
  const auto retained = InventoryEntry(4, mga::TransactionState::committed);
  const auto prepared = InventoryEntry(5, mga::TransactionState::prepared);
  const auto limbo = InventoryEntry(6, mga::TransactionState::limbo);
  const auto inventory = Inventory({committed_old, rolled_back, committed_latest, retained, prepared, limbo});

  TestSuccessfulBoundedSweep(inventory, committed_old, rolled_back, committed_latest);
  TestRetentionAndBackupArchiveHolds(inventory, retained);
  TestLimboAndUnknownOutcomeProtection(inventory, prepared, limbo);
  TestFailClosedDiagnostics(inventory, retained);
  return EXIT_SUCCESS;
}
