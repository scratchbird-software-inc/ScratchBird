// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "runtime_platform.hpp"
#include "transaction_inventory_page.hpp"
#include "transaction_lock.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace page = scratchbird::storage::page;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1770100000000ull;
constexpr scratchbird::core::platform::u32 kPageSize = 16384;
constexpr std::size_t kOffsetChecksumDigest = 64;
constexpr std::size_t kOffsetInventoryGeneration = 96;
constexpr std::size_t kOffsetChainDigest = 112;

bool Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_transaction_inventory_lock_table_gate";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

bool ConfigureMemoryFixture() {
  const auto configured =
      memory::ConfigureDefaultMemoryManagerForFixture(
          MemoryPolicy(), "public_transaction_inventory_lock_table_gate");
  return Expect(configured.ok(),
                "transaction inventory/lock gate memory should configure") &&
         Expect(configured.fixture_mode,
                "transaction inventory/lock gate must use fixture memory");
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

void RewritePageChecksum(std::vector<byte>* serialized) {
  const auto digest = page::ComputeTransactionInventoryPageChecksumDigest(*serialized);
  std::copy(digest.begin(),
            digest.end(),
            serialized->begin() + static_cast<std::ptrdiff_t>(kOffsetChecksumDigest));
}

txn::LocalTransactionInventory InventoryWithSnapshotHighWater(bool* ok) {
  txn::LocalTransactionInventory inventory = txn::MakeEmptyLocalTransactionInventory();
  const auto first =
      txn::BeginLocalTransaction(inventory, MakeUuid(UuidKind::transaction, 10),
                                 kBaseMillis + 10);
  *ok = Expect(first.ok(), "first transaction begin should succeed") && *ok;
  const auto first_commit =
      txn::CommitLocalTransaction(first.inventory, first.entry.identity.local_id,
                                  kBaseMillis + 20);
  *ok = Expect(first_commit.ok(), "first transaction commit should succeed") && *ok;
  const auto second =
      txn::BeginLocalTransaction(first_commit.inventory,
                                 MakeUuid(UuidKind::transaction, 30),
                                 kBaseMillis + 30);
  *ok = Expect(second.ok(), "second transaction begin should succeed") && *ok;
  *ok = Expect(second.entry.begin_visible_through_local_transaction_id ==
                   first_commit.entry.identity.local_id.value,
               "second transaction should capture begin high-water") && *ok;
  const auto second_commit =
      txn::CommitLocalTransaction(second.inventory,
                                  second.entry.identity.local_id,
                                  kBaseMillis + 40);
  *ok = Expect(second_commit.ok(), "second transaction commit should succeed") && *ok;
  return second_commit.inventory;
}

bool InventoryPageCodecProof() {
  bool ok = true;
  page::TransactionInventoryPageBody body;
  body.page_number = 7;
  body.previous_page_number = 6;
  body.next_page_number = 8;
  body.inventory_generation = 4;
  body.inventory = InventoryWithSnapshotHighWater(&ok);
  if (!ok) {
    return false;
  }

  const auto built = page::BuildTransactionInventoryPageBody(body, kPageSize);
  ok = Expect(built.ok(), "transaction inventory page should build") && ok;
  ok = Expect(page::TransactionInventoryPageDigestPresent(built.body.chain_digest),
              "transaction inventory page should carry a chain digest") && ok;
  ok = Expect(built.body.chain_digest.size() == page::kTransactionInventoryPageDigestBytes,
              "transaction inventory page should carry full SHA-256 chain digest material") && ok;
  const auto parsed =
      page::ParseTransactionInventoryPageBody(built.serialized, body.page_number);
  ok = Expect(parsed.ok(), "transaction inventory page should parse") && ok;
  if (!ok) {
    return false;
  }

  ok = Expect(parsed.body.previous_page_number == body.previous_page_number,
              "previous page number should round trip") && ok;
  ok = Expect(parsed.body.next_page_number == body.next_page_number,
              "next page number should round trip") && ok;
  ok = Expect(parsed.body.inventory_generation == body.inventory_generation,
              "inventory generation should round trip") && ok;
  ok = Expect(parsed.body.chain_digest == built.body.chain_digest,
              "chain digest should round trip") && ok;
  ok = Expect(parsed.body.inventory.entries.size() == body.inventory.entries.size(),
              "inventory entry count should round trip") && ok;
  ok = Expect(parsed.body.inventory.entries[1]
                      .begin_visible_through_local_transaction_id ==
                  body.inventory.entries[1].begin_visible_through_local_transaction_id,
              "transaction-start high-water should be durable") && ok;

  auto chain_corrupt = built.serialized;
  chain_corrupt[kOffsetChainDigest] ^= 0x55u;
  RewritePageChecksum(&chain_corrupt);
  const auto chain_parse =
      page::ParseTransactionInventoryPageBody(chain_corrupt, body.page_number);
  ok = Expect(!chain_parse.ok(),
              "chain digest corruption should fail closed") && ok;
  ok = Expect(chain_parse.diagnostic.diagnostic_code ==
                  "SB-TXN-INVENTORY-PAGE-CHAIN-DIGEST-MISMATCH",
              "chain digest corruption should use stable diagnostic") && ok;

  auto weak_v1 = built.serialized;
  weak_v1[7] = static_cast<byte>('1');
  const auto weak_v1_parse =
      page::ParseTransactionInventoryPageBody(weak_v1, body.page_number);
  ok = Expect(!weak_v1_parse.ok(),
              "v1 weak FNV transaction inventory page should fail closed") && ok;
  ok = Expect(weak_v1_parse.diagnostic.diagnostic_code ==
                  "SB-TXN-INVENTORY-PAGE-WEAK-DIGEST-REFUSED",
              "v1 weak digest refusal should use stable diagnostic") && ok;

  auto generation_corrupt = built.serialized;
  StoreLittle64(generation_corrupt.data() + kOffsetInventoryGeneration, 0);
  RewritePageChecksum(&generation_corrupt);
  const auto generation_parse =
      page::ParseTransactionInventoryPageBody(generation_corrupt,
                                              body.page_number);
  ok = Expect(!generation_parse.ok(),
              "zero generation should fail closed") && ok;
  ok = Expect(generation_parse.diagnostic.diagnostic_code ==
                  "SB-TXN-INVENTORY-PAGE-GENERATION-INVALID",
              "generation corruption should use stable diagnostic") && ok;

  auto checksum_corrupt = built.serialized;
  checksum_corrupt.back() ^= 0x01u;
  const auto checksum_parse =
      page::ParseTransactionInventoryPageBody(checksum_corrupt, body.page_number);
  ok = Expect(!checksum_parse.ok(),
              "body checksum corruption should fail closed") && ok;
  return ok;
}

bool HasCommittedEvidence(const txn::LocalTransactionInventory& inventory,
                          u64 local_transaction_id) {
  for (const auto& entry : inventory.entries) {
    if (entry.identity.local_id.value == local_transaction_id &&
        entry.state == txn::TransactionState::committed &&
        entry.evidence_record_written) {
      return true;
    }
  }
  return false;
}

bool DurableDatabaseStoreProof(const std::filesystem::path& work_dir) {
  bool ok = true;
  std::filesystem::create_directories(work_dir);
  const auto database_path = work_dir / "pcr071_inventory.sbdb";
  std::filesystem::remove(database_path);

  db::DatabaseCreateConfig create;
  create.path = database_path.string();
  create.database_uuid = MakeUuid(UuidKind::database, 1000);
  create.filespace_uuid = MakeUuid(UuidKind::filespace, 1001);
  create.page_size = kPageSize;
  create.creation_unix_epoch_millis = kBaseMillis + 1000;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  ok = Expect(created.ok(), "database create should succeed") && ok;
  if (!ok) {
    if (!created.ok()) {
      std::cerr << created.diagnostic.diagnostic_code << ':'
                << created.diagnostic.message_key << '\n';
    }
    return false;
  }

  const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(
      database_path.string());
  ok = Expect(loaded.ok(), "created database inventory should load") && ok;
  ok = Expect(HasCommittedEvidence(loaded.inventory, 1),
              "bootstrap transaction should be committed with evidence") && ok;

  txn::LocalTransactionInventory inventory = loaded.inventory;
  u64 previous_committed = 1;
  for (u64 i = 0; i < 260; ++i) {
    const auto begun =
        txn::BeginLocalTransaction(inventory,
                                   MakeUuid(UuidKind::transaction, 2000 + i),
                                   kBaseMillis + 2000 + i);
    ok = Expect(begun.ok(), "bulk transaction begin should succeed") && ok;
    ok = Expect(begun.entry.begin_visible_through_local_transaction_id ==
                    previous_committed,
                "bulk transaction begin high-water should follow inventory") && ok;
    const auto committed =
        txn::CommitLocalTransaction(begun.inventory,
                                    begun.entry.identity.local_id,
                                    kBaseMillis + 3000 + i);
    ok = Expect(committed.ok(), "bulk transaction commit should succeed") && ok;
    previous_committed = committed.entry.identity.local_id.value;
    inventory = committed.inventory;
  }
  if (!ok) {
    return false;
  }

  const auto persisted =
      db::PersistLocalTransactionInventoryToDatabase(database_path.string(),
                                                     inventory);
  ok = Expect(persisted.ok(), "multi-page inventory persist should succeed") && ok;
  const auto reopened =
      db::LoadLocalTransactionInventoryFromDatabase(database_path.string());
  ok = Expect(reopened.ok(), "multi-page inventory should reopen") && ok;
  ok = Expect(reopened.inventory.entries.size() == inventory.entries.size(),
              "reopened inventory entry count should match") && ok;
  ok = Expect(reopened.inventory.next_local_transaction_id ==
                  inventory.next_local_transaction_id,
              "reopened next transaction id should match") && ok;
  ok = Expect(reopened.inventory.entries.back()
                      .begin_visible_through_local_transaction_id ==
                  previous_committed - 1,
              "reopened final begin high-water should remain durable") && ok;
  return ok;
}

txn::TransactionLockRequest LockRequest(u64 requester,
                                        std::string resource,
                                        txn::TransactionLockMode mode,
                                        bool no_wait = true) {
  txn::TransactionLockRequest request;
  request.requester = txn::MakeLocalTransactionId(requester);
  request.resource_key = std::move(resource);
  request.mode = mode;
  request.wait_policy.no_wait = no_wait;
  request.wait_policy.timeout_millis = no_wait ? 0 : 100;
  request.wait_policy.wait_start_unix_epoch_millis = 10;
  request.wait_policy.now_unix_epoch_millis = 20;
  request.transaction_active = true;
  return request;
}

bool LockTableFairnessAndDeadlockProof() {
  bool ok = true;
  txn::LocalTransactionLockTable locks;
  const auto shared_a = locks.Acquire(
      LockRequest(1, "row:fair", txn::TransactionLockMode::shared));
  const auto shared_b = locks.Acquire(
      LockRequest(2, "row:fair", txn::TransactionLockMode::shared));
  ok = Expect(shared_a.ok() && shared_b.ok(),
              "shared owners should acquire compatible locks") && ok;

  const auto exclusive_wait = locks.Acquire(
      LockRequest(3, "row:fair", txn::TransactionLockMode::exclusive, false));
  ok = Expect(exclusive_wait.decision == txn::TransactionLockDecision::wait_required,
              "exclusive waiter should queue behind shared owners") && ok;

  const auto shared_late = locks.Acquire(
      LockRequest(4, "row:fair", txn::TransactionLockMode::shared, false));
  ok = Expect(shared_late.decision == txn::TransactionLockDecision::wait_required,
              "late shared reader should queue behind older exclusive waiter") && ok;
  ok = Expect(shared_late.blocking_transaction.value == 3,
              "late shared reader should identify fairness blocker") && ok;
  ok = Expect(shared_late.diagnostic.diagnostic_code ==
                  "SB-SNTXN-LOCK-FAIRNESS-WAIT",
              "fairness wait should use stable diagnostic") && ok;

  ok = Expect(locks.Release(txn::MakeLocalTransactionId(1), "row:fair").ok(),
              "first shared release should succeed") && ok;
  ok = Expect(locks.Release(txn::MakeLocalTransactionId(2), "row:fair").ok(),
              "second shared release should succeed") && ok;
  const auto exclusive_retry = locks.Retry(txn::MakeLocalTransactionId(3));
  ok = Expect(exclusive_retry.ok(),
              "older exclusive waiter should acquire after blockers release") && ok;
  const auto shared_retry = locks.Retry(txn::MakeLocalTransactionId(4));
  ok = Expect(shared_retry.decision == txn::TransactionLockDecision::wait_required,
              "later shared waiter should still wait behind exclusive owner") && ok;
  ok = Expect(locks.ReleaseAll(txn::MakeLocalTransactionId(3)) == 1,
              "exclusive owner release-all should release one resource") && ok;

  txn::LocalTransactionLockTable upgrade_locks;
  ok = Expect(upgrade_locks.Acquire(
                  LockRequest(10, "row:upgrade", txn::TransactionLockMode::shared))
                  .ok(),
              "first upgrade shared lock should acquire") && ok;
  ok = Expect(upgrade_locks.Acquire(
                  LockRequest(11, "row:upgrade", txn::TransactionLockMode::shared))
                  .ok(),
              "second upgrade shared lock should acquire") && ok;
  const auto upgrade_wait = upgrade_locks.Acquire(
      LockRequest(10, "row:upgrade", txn::TransactionLockMode::exclusive, false));
  ok = Expect(upgrade_wait.decision == txn::TransactionLockDecision::wait_required,
              "first upgrade should wait on peer shared owner") && ok;
  const auto upgrade_deadlock = upgrade_locks.Acquire(
      LockRequest(11, "row:upgrade", txn::TransactionLockMode::exclusive, false));
  ok = Expect(upgrade_deadlock.decision ==
                  txn::TransactionLockDecision::deadlock_detected,
              "opposing upgrades should diagnose deadlock") && ok;
  ok = Expect(upgrade_deadlock.diagnostic.diagnostic_code ==
                  "SB-SNTXN-DEADLOCK-DETECTED",
              "upgrade deadlock should use stable diagnostic") && ok;

  txn::LocalTransactionLockTable admission_locks;
  txn::LocalTransactionLockTable::AdmissionPolicy policy;
  policy.max_held_locks = 1;
  admission_locks.SetAdmissionPolicy(policy);
  ok = Expect(admission_locks.Acquire(
                  LockRequest(20, "row:one", txn::TransactionLockMode::exclusive))
                  .ok(),
              "first admitted lock should acquire") && ok;
  const auto refused = admission_locks.Acquire(
      LockRequest(21, "row:two", txn::TransactionLockMode::exclusive));
  ok = Expect(refused.decision == txn::TransactionLockDecision::admission_refused,
              "held-lock admission limit should fail closed") && ok;
  return ok;
}

bool LockTableConcurrentSmoke() {
  txn::LocalTransactionLockTable locks;
  std::atomic<bool> ok{true};
  auto worker = [&locks, &ok](u64 id, const char* resource) {
    for (u64 i = 0; i < 64; ++i) {
      const auto acquired = locks.Acquire(
          LockRequest(id, resource, txn::TransactionLockMode::exclusive));
      if (!acquired.ok()) {
        ok = false;
        return;
      }
      const auto released = locks.Release(txn::MakeLocalTransactionId(id), resource);
      if (!released.ok()) {
        ok = false;
        return;
      }
    }
  };
  std::thread left(worker, 101, "row:thread-left");
  std::thread right(worker, 102, "row:thread-right");
  left.join();
  right.join();
  return Expect(ok.load(), "concurrent lock smoke should complete") &&
         Expect(locks.held_lock_count() == 0,
                "concurrent lock smoke should release all locks") &&
         Expect(locks.waiting_lock_count() == 0,
                "concurrent lock smoke should leave no waiters");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: public_transaction_inventory_lock_table_gate WORK_DIR\n";
    return EXIT_FAILURE;
  }
  bool ok = true;
  ok = ConfigureMemoryFixture() && ok;
  ok = InventoryPageCodecProof() && ok;
  ok = DurableDatabaseStoreProof(argv[1]) && ok;
  ok = LockTableFairnessAndDeadlockProof() && ok;
  ok = LockTableConcurrentSmoke() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
