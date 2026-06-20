// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_cleanup_horizon_service.hpp"
#include "transaction_support_services.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok()) {
    std::cerr << result.diagnostic.diagnostic_code << '\n';
  }
  Require(result.ok(), message);
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewTransactionUuid(platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(platform::UuidKind::transaction,
                                                        NowMillis() + salt);
  Require(generated.ok(), "transaction UUID generation failed");
  return generated.value;
}

bool HasEvidence(const std::vector<mga::TransactionSupportEvidenceField>& evidence,
                 std::string_view key,
                 std::string_view value) {
  for (const auto& item : evidence) {
    if (item.key == key && item.value == value) { return true; }
  }
  return false;
}

bool HasCleanupEvidence(const std::vector<mga::CleanupHorizonEvidenceField>& evidence,
                        std::string_view key,
                        std::string_view value) {
  for (const auto& item : evidence) {
    if (item.key == key && item.value == value) { return true; }
  }
  return false;
}

bool HasBlocker(const std::vector<mga::CleanupHorizonBlocker>& blockers,
                mga::CleanupHorizonBlockerKind kind,
                mga::LocalTransactionId local_transaction_id) {
  for (const auto& blocker : blockers) {
    if (blocker.kind == kind &&
        blocker.local_transaction_id.value == local_transaction_id.value &&
        blocker.authoritative) {
      return true;
    }
  }
  return false;
}

struct InventoryFixture {
  mga::LocalTransactionInventory inventory;
  mga::LocalTransactionId committed_first;
  mga::LocalTransactionId committed_second;
  mga::LocalTransactionId rolled_back;
  mga::LocalTransactionId old_reader;
  mga::LocalTransactionId active_writer;
  platform::TypedUuid active_writer_uuid;
};

InventoryFixture BuildInventoryFixture() {
  InventoryFixture fixture;
  fixture.inventory = mga::MakeEmptyLocalTransactionInventory();
  const platform::u64 base = NowMillis();

  auto first = mga::BeginLocalTransaction(fixture.inventory, NewTransactionUuid(10), base + 10);
  RequireOk(first, "begin first transaction failed");
  fixture.inventory = first.inventory;
  fixture.committed_first = first.entry.identity.local_id;
  auto first_commit = mga::CommitLocalTransaction(fixture.inventory,
                                                  fixture.committed_first,
                                                  base + 20);
  RequireOk(first_commit, "commit first transaction failed");
  fixture.inventory = first_commit.inventory;

  auto second = mga::BeginLocalTransaction(fixture.inventory, NewTransactionUuid(30), base + 30);
  RequireOk(second, "begin second transaction failed");
  fixture.inventory = second.inventory;
  fixture.committed_second = second.entry.identity.local_id;
  auto second_commit = mga::CommitLocalTransaction(fixture.inventory,
                                                   fixture.committed_second,
                                                   base + 40);
  RequireOk(second_commit, "commit second transaction failed");
  fixture.inventory = second_commit.inventory;

  auto rolled_back = mga::BeginLocalTransaction(fixture.inventory,
                                                NewTransactionUuid(45),
                                                base + 45);
  RequireOk(rolled_back, "begin rolled-back transaction failed");
  fixture.inventory = rolled_back.inventory;
  fixture.rolled_back = rolled_back.entry.identity.local_id;
  auto rollback = mga::RollbackLocalTransaction(fixture.inventory,
                                                fixture.rolled_back,
                                                base + 46);
  RequireOk(rollback, "rollback transaction failed");
  fixture.inventory = rollback.inventory;

  auto reader = mga::BeginLocalReadOnlyTransaction(fixture.inventory,
                                                   NewTransactionUuid(50),
                                                   base + 50);
  RequireOk(reader, "begin old reader failed");
  fixture.inventory = reader.inventory;
  fixture.old_reader = reader.entry.identity.local_id;

  auto writer = mga::BeginLocalTransaction(fixture.inventory, NewTransactionUuid(60), base + 60);
  RequireOk(writer, "begin active writer failed");
  fixture.inventory = writer.inventory;
  fixture.active_writer = writer.entry.identity.local_id;
  fixture.active_writer_uuid = writer.entry.identity.transaction_uuid;
  return fixture;
}

void ProveCommitFenceCoalescing(const InventoryFixture& fixture) {
  mga::TransactionCommitFenceCoalescingRequest request;
  request.inventory = fixture.inventory;
  request.inventory_authoritative = true;
  request.policy_enabled = true;
  request.policy_required = true;
  request.allow_commit_fence_coalescing = true;
  request.fence_generation = 1;
  request.max_transactions_per_fence = 4;
  request.local_transaction_ids = {fixture.committed_first, fixture.committed_second};
  const auto accepted = mga::PlanTransactionCommitFenceCoalescing(request);
  RequireOk(accepted, "committed transactions should coalesce behind one fence");
  Require(accepted.coalesced_fence_count == 1, "coalesced fence count should be one");
  Require(accepted.durable_inventory_remains_authority,
          "coalescing must leave durable inventory authoritative");
  Require(!accepted.coalesced_fence_is_finality_authority,
          "coalesced fence must not become finality authority");
  Require(HasEvidence(accepted.evidence, "durable_inventory_remains_authority", "true"),
          "coalescing evidence should name durable inventory authority");

  request.local_transaction_ids = {fixture.active_writer};
  const auto active_refused = mga::PlanTransactionCommitFenceCoalescing(request);
  Require(!active_refused.status.ok(),
          "active transaction must not be accepted for commit fence coalescing");

  request.local_transaction_ids = {fixture.rolled_back};
  const auto rollback_refused = mga::PlanTransactionCommitFenceCoalescing(request);
  Require(!rollback_refused.status.ok(),
          "rolled-back transaction must not be accepted for commit fence coalescing");

  request.local_transaction_ids = {fixture.committed_first};
  request.inventory_authoritative = false;
  const auto unauthoritative = mga::PlanTransactionCommitFenceCoalescing(request);
  Require(!unauthoritative.status.ok(),
          "non-authoritative inventory must fail closed for fence coalescing");
}

void ProveCleanupHorizonBlocksOldReaders(const InventoryFixture& fixture) {
  mga::AuthoritativeCleanupHorizonRequest request;
  request.inventory = fixture.inventory;
  request.inventory_authoritative = true;
  request.inventory_complete = true;
  request.active_snapshot_inventory_authoritative = true;
  request.active_snapshot_horizons = {fixture.old_reader};
  const auto horizon = mga::ComputeAuthoritativeCleanupHorizon(request);
  RequireOk(horizon, "cleanup horizon should be authoritative with durable inventory");
  Require(horizon.cleanup_horizon.value <= fixture.old_reader.value,
          "old reader must hold cleanup horizon");
  Require(HasBlocker(horizon.blockers,
                     mga::CleanupHorizonBlockerKind::active_snapshot,
                     fixture.old_reader),
          "old reader snapshot blocker should be reported");
  Require(HasBlocker(horizon.blockers,
                     mga::CleanupHorizonBlockerKind::active_transaction,
                     fixture.active_writer),
          "active writer blocker should be reported");
  Require(HasCleanupEvidence(horizon.evidence, "parser_finality_authority", "false"),
          "parser state must not be cleanup authority");
  Require(HasCleanupEvidence(horizon.evidence, "cluster_private_implementation", "false"),
          "provider state must not be cleanup authority");

  request.inventory_authoritative = false;
  const auto refused = mga::ComputeAuthoritativeCleanupHorizon(request);
  Require(!refused.status.ok(), "cleanup horizon must fail without authoritative inventory");
}

void ProveSavepointDeltaAndReadAfterWriteRollback(const InventoryFixture& fixture) {
  mga::SavepointStateDeltaManager deltas;

  mga::SavepointStateDelta base_delta;
  base_delta.local_transaction_id = fixture.active_writer;
  base_delta.savepoint_ordinal = 0;
  base_delta.mutation_sequence = 1;
  base_delta.object_uuid = "object-alpha";
  base_delta.record_uuid = "record-alpha";
  base_delta.before_image_hash = "hash:before:0";
  base_delta.after_image_hash = "hash:after:1";
  base_delta.durable_evidence_written = true;
  RequireOk(deltas.RecordDelta(base_delta), "base savepoint delta should record");

  mga::SavepointStateDelta savepoint_delta = base_delta;
  savepoint_delta.savepoint_ordinal = 1;
  savepoint_delta.mutation_sequence = 2;
  savepoint_delta.before_image_hash = "hash:after:1";
  savepoint_delta.after_image_hash = "hash:after:2";
  RequireOk(deltas.RecordDelta(savepoint_delta), "savepoint delta should record");

  const auto latest_before = deltas.LatestActiveDelta(fixture.active_writer, "record-alpha");
  Require(latest_before.has_value(), "latest savepoint delta should be visible before rollback");
  Require(latest_before->mutation_sequence == 2,
          "latest active delta should be the post-savepoint mutation");

  const auto rolled_back = deltas.RollbackToSavepoint(fixture.active_writer, 1);
  RequireOk(rolled_back, "savepoint delta rollback should pass");
  Require(rolled_back.affected_delta_count == 1, "one savepoint delta should roll back");
  Require(rolled_back.transaction_identity_unchanged,
          "savepoint rollback must not allocate transaction authority");
  const auto latest_after = deltas.LatestActiveDelta(fixture.active_writer, "record-alpha");
  Require(latest_after.has_value(), "base delta should remain active after rollback");
  Require(latest_after->mutation_sequence == 1,
          "savepoint rollback should reveal the prior local delta");

  mga::ReadAfterWriteLocalOverlayCache overlay;
  mga::ReadAfterWriteOverlayEntry base_overlay;
  base_overlay.local_transaction_id = fixture.active_writer;
  base_overlay.savepoint_ordinal = 0;
  base_overlay.mutation_sequence = 1;
  base_overlay.object_uuid = "object-alpha";
  base_overlay.record_uuid = "record-alpha";
  base_overlay.value_hash = "hash:value:1";
  RequireOk(overlay.Put(base_overlay), "base overlay put should pass");
  mga::ReadAfterWriteOverlayEntry savepoint_overlay = base_overlay;
  savepoint_overlay.savepoint_ordinal = 1;
  savepoint_overlay.mutation_sequence = 2;
  savepoint_overlay.value_hash = "hash:value:2";
  RequireOk(overlay.Put(savepoint_overlay), "savepoint overlay put should pass");

  const auto own_hit = overlay.Lookup(fixture.active_writer, "record-alpha");
  RequireOk(own_hit, "own transaction should see read-after-write overlay");
  Require(own_hit.entry.mutation_sequence == 2, "overlay should return newest own write");
  Require(!own_hit.entry.visible_to_other_transactions,
          "overlay must not publish uncommitted local writes to other transactions");
  const auto other_miss = overlay.Lookup(fixture.old_reader, "record-alpha");
  Require(other_miss.status.ok() && !other_miss.accepted,
          "other transaction should not see local overlay");

  const auto overlay_rollback = overlay.RollbackToSavepoint(fixture.active_writer, 1);
  RequireOk(overlay_rollback, "overlay rollback should pass");
  Require(overlay_rollback.removed_entry_count == 1,
          "overlay rollback should remove post-savepoint entry");
  const auto own_after = overlay.Lookup(fixture.active_writer, "record-alpha");
  RequireOk(own_after, "own overlay should still expose base write");
  Require(own_after.entry.mutation_sequence == 1,
          "overlay rollback should reveal base write");
}

void ProveSnapshotTemplateCache(const InventoryFixture& fixture) {
  mga::TransactionSnapshotTemplateStaticInputs inputs;
  inputs.isolation_profile = "read_committed";
  inputs.catalog_epoch = 1;
  inputs.security_epoch = 2;
  inputs.policy_epoch = 3;
  inputs.metadata_epoch = 4;

  mga::TransactionSnapshotTemplateCache cache(11, 1);
  mga::TransactionSnapshotTemplateCacheRequest request;
  request.inventory_authoritative = true;
  request.inventory_generation = 9;
  request.static_inputs = inputs;
  request.next_local_transaction_id =
      mga::MakeLocalTransactionId(fixture.inventory.next_local_transaction_id);
  request.visible_through_local_transaction_id = fixture.committed_second;
  RequireOk(cache.Store(request), "snapshot template should store exact static inputs");
  const auto hit = cache.Lookup(request);
  RequireOk(hit, "snapshot template should hit with exact inventory generation");
  Require(hit.cache_hit, "snapshot template lookup should be a cache hit");
  Require(hit.snapshot_template.static_inputs_only,
          "snapshot template must contain only static inputs");
  Require(!hit.snapshot_template.active_transaction_set_cached,
          "snapshot template must not cache active transaction set");

  auto stale = request;
  stale.inventory_generation = 10;
  const auto stale_result = cache.Lookup(stale);
  Require(!stale_result.status.ok(), "stale snapshot template must fail closed");

  cache.Invalidate(2);
  const auto invalidated = cache.Lookup(request);
  Require(!invalidated.status.ok(), "invalidated snapshot template must fail closed");
}

void ProveVisibilityAccelerationExactValidation(const InventoryFixture& fixture) {
  mga::MgaVisibilityAccelerationBuildRequest build;
  build.inventory = fixture.inventory;
  build.inventory_authoritative = true;
  build.inventory_generation = 17;
  build.table_generation = 1;
  const auto built = mga::BuildMgaVisibilityAccelerationTable(build);
  RequireOk(built, "visibility acceleration table should build from inventory");
  Require(!built.table.acceleration_table_is_finality_authority,
          "visibility acceleration table must not be finality authority");

  mga::MgaVisibilityAccelerationProbeRequest probe;
  probe.table = built.table;
  probe.current_inventory = fixture.inventory;
  probe.current_inventory_authoritative = true;
  probe.inventory_generation = 17;
  probe.first_local_transaction_id = fixture.committed_first;
  probe.last_local_transaction_id = fixture.committed_second;
  const auto committed = mga::ProbeMgaVisibilityAccelerationTable(probe);
  RequireOk(committed, "committed range should validate against inventory");
  Require(committed.all_committed, "committed range should be reported as committed");
  Require(!committed.acceleration_table_is_finality_authority,
          "probe must not promote acceleration table to finality authority");

  probe.last_local_transaction_id = fixture.active_writer;
  const auto mixed = mga::ProbeMgaVisibilityAccelerationTable(probe);
  RequireOk(mixed, "mixed range should still validate exactly against inventory");
  Require(!mixed.all_committed, "active transaction should prevent all-committed proof");

  probe.first_local_transaction_id = fixture.rolled_back;
  probe.last_local_transaction_id = fixture.rolled_back;
  const auto rolled_back = mga::ProbeMgaVisibilityAccelerationTable(probe);
  RequireOk(rolled_back, "rolled-back transaction should validate against inventory");
  Require(!rolled_back.all_committed, "rolled-back transaction must not be all-committed");

  probe.inventory_generation = 18;
  const auto stale = mga::ProbeMgaVisibilityAccelerationTable(probe);
  Require(!stale.status.ok(), "visibility acceleration table must reject stale generation");

  probe.inventory_generation = 17;
  probe.first_local_transaction_id = fixture.committed_second;
  probe.last_local_transaction_id = fixture.committed_second;
  probe.current_inventory.entries[1].state = mga::TransactionState::active;
  const auto mismatch = mga::ProbeMgaVisibilityAccelerationTable(probe);
  Require(!mismatch.status.ok(), "visibility acceleration table must reject inventory mismatch");
}

void ProveStatementIdReservation(const InventoryFixture& fixture) {
  mga::TransactionStatementIdReservationService service;
  mga::TransactionStatementIdReservationRequest request;
  request.inventory = fixture.inventory;
  request.inventory_authoritative = true;
  request.local_transaction_id = fixture.active_writer;
  request.transaction_uuid = fixture.active_writer_uuid;
  request.statement_count = 2;

  const auto first = service.Reserve(request);
  RequireOk(first, "statement IDs should reserve for active transaction");
  Require(first.statement_ids.size() == 2, "two statement IDs should be reserved");
  Require(first.statement_ids[0].statement_sequence == 1 &&
          first.statement_ids[1].statement_sequence == 2,
          "statement IDs should be monotonic inside transaction");
  Require(!first.statement_id_is_transaction_finality_authority,
          "statement ID service must not become transaction finality authority");

  request.statement_count = 1;
  const auto second = service.Reserve(request);
  RequireOk(second, "statement ID service should continue sequence");
  Require(second.statement_ids.front().statement_sequence == 3,
          "statement ID sequence should continue after reservation");

  request.inventory_authoritative = false;
  const auto unauthoritative = service.Reserve(request);
  Require(!unauthoritative.status.ok(),
          "statement ID reservation must fail without inventory authority");

  request.inventory_authoritative = true;
  request.transaction_uuid = NewTransactionUuid(200);
  const auto wrong_uuid = service.Reserve(request);
  Require(!wrong_uuid.status.ok(), "statement ID reservation must bind exact transaction UUID");

  request.transaction_uuid = fixture.active_writer_uuid;
  auto committed_inventory = mga::CommitLocalTransaction(fixture.inventory,
                                                         fixture.active_writer,
                                                         NowMillis() + 300);
  RequireOk(committed_inventory, "active writer commit should update inventory");
  request.inventory = committed_inventory.inventory;
  const auto after_commit = service.Reserve(request);
  Require(!after_commit.status.ok(),
          "statement ID reservation must reject committed transactions");
}

}  // namespace

int main() {
  const auto fixture = BuildInventoryFixture();
  ProveCommitFenceCoalescing(fixture);
  ProveCleanupHorizonBlocksOldReaders(fixture);
  ProveSavepointDeltaAndReadAfterWriteRollback(fixture);
  ProveSnapshotTemplateCache(fixture);
  ProveVisibilityAccelerationExactValidation(fixture);
  ProveStatementIdReservation(fixture);
  return EXIT_SUCCESS;
}
