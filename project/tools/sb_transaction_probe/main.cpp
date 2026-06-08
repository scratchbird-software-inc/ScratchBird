// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-TXN-PROBE-ANCHOR
#include "copy_on_write.hpp"
#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "row_version.hpp"
#include "transaction_inventory_page.hpp"
#include "transaction_manager.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using scratchbird::core::memory::DefaultMemoryManager;
using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::core::uuid::GenerateEngineIdentityV7;
using scratchbird::core::uuid::ParseTypedUuid;
using scratchbird::storage::database::CreateDatabaseFile;
using scratchbird::storage::database::DatabaseCreateConfig;
using scratchbird::storage::database::DatabaseOpenConfig;
using scratchbird::storage::database::OpenDatabaseFile;
using scratchbird::storage::page::BuildTransactionInventoryPageBody;
using scratchbird::storage::page::ParseTransactionInventoryPageBody;
using scratchbird::storage::page::TransactionInventoryPageBody;
using scratchbird::transaction::mga::CopyOnWriteMutationKind;
using scratchbird::transaction::mga::LocalTransactionManager;
using scratchbird::transaction::mga::MakeLocalTransactionId;
using scratchbird::transaction::mga::MakeRowIdentity;
using scratchbird::transaction::mga::PlanLocalCopyOnWriteMutationForTransaction;
using scratchbird::transaction::mga::TransactionState;
using scratchbird::transaction::mga::TransactionStateName;

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
  if (end == nullptr || *end != '\0') {
    return false;
  }
  *value = static_cast<u64>(parsed);
  return true;
}

bool ParseU32(const std::string& text, u32* value) {
  u64 parsed = 0;
  if (!ParseU64(text, &parsed) || parsed > 0xffffffffull) {
    return false;
  }
  *value = static_cast<u32>(parsed);
  return true;
}

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") {
      args->overwrite = true;
      continue;
    }
    if (i + 1 >= argc) {
      return false;
    }
    const std::string value = argv[++i];
    if (key == "--path") {
      args->path = value;
    } else if (key == "--seed-pack-root") {
      args->seed_pack_root = value;
    } else if (key == "--creation-ms") {
      if (!ParseU64(value, &args->creation_millis)) { return false; }
    } else if (key == "--page-size") {
      if (!ParseU32(value, &args->page_size)) { return false; }
    } else {
      return false;
    }
  }
  return !args->path.empty() && !args->seed_pack_root.empty() && args->creation_millis != 0;
}

void PrintDiagnostic(const DiagnosticRecord& diagnostic) {
  std::cerr << diagnostic.diagnostic_code << ":" << diagnostic.message_key << "\n";
}

TypedUuid GenerateTyped(UuidKind kind, u64 millis) {
  const auto generated = GenerateEngineIdentityV7(kind, millis);
  if (!generated.ok()) {
    return {};
  }
  return generated.value;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_transaction_probe --path PATH --seed-pack-root PATH --creation-ms MILLIS [--page-size BYTES] [--overwrite]\n";
    return 2;
  }

  const TypedUuid database_uuid = GenerateTyped(UuidKind::database, args.creation_millis + 10);
  const TypedUuid filespace_uuid = GenerateTyped(UuidKind::filespace, args.creation_millis + 11);
  const TypedUuid txn_uuid = GenerateTyped(UuidKind::transaction, args.creation_millis + 20);
  const TypedUuid txn_uuid_2 = GenerateTyped(UuidKind::transaction, args.creation_millis + 21);
  const TypedUuid row_uuid = GenerateTyped(UuidKind::row, args.creation_millis + 30);
  if (!database_uuid.valid() || !filespace_uuid.valid() || !txn_uuid.valid() || !txn_uuid_2.valid() || !row_uuid.valid()) {
    std::cerr << "SB-TXN-PROBE-UUID-GENERATION-FAILED\n";
    return 1;
  }

  LocalTransactionManager manager;
  const auto begin = manager.Begin(txn_uuid, args.creation_millis + 100);
  if (!begin.ok()) { PrintDiagnostic(begin.diagnostic); return 1; }
  const auto snapshot = manager.Snapshot(begin.entry.identity.local_id);
  if (!snapshot.ok()) { PrintDiagnostic(snapshot.diagnostic); return 1; }
  const auto duplicate = manager.Begin(txn_uuid, args.creation_millis + 101);
  const bool duplicate_failed = !duplicate.ok() && duplicate.diagnostic.diagnostic_code == "SB-TXN-DUPLICATE-TRANSACTION-UUID";
  const auto invalid_transition = scratchbird::transaction::mga::CheckTransactionStateTransition(
      TransactionState::committed,
      TransactionState::active,
      false);
  const bool invalid_transition_failed = !invalid_transition.ok() &&
                                         invalid_transition.diagnostic.diagnostic_code == "SB-TXN-STATE-INVALID-TRANSITION";

  const auto row_identity = MakeRowIdentity(row_uuid);
  if (!row_identity.ok()) { PrintDiagnostic(row_identity.diagnostic); return 1; }
  const auto cow = PlanLocalCopyOnWriteMutationForTransaction(begin.entry,
                                                             row_identity.identity,
                                                             CopyOnWriteMutationKind::insert,
                                                             0,
                                                             1);
  if (!cow.ok()) { PrintDiagnostic(cow.diagnostic); return 1; }

  const auto commit = manager.Commit(begin.entry.identity.local_id, args.creation_millis + 200);
  if (!commit.ok()) { PrintDiagnostic(commit.diagnostic); return 1; }
  const auto rollback_begin = manager.Begin(txn_uuid_2, args.creation_millis + 210);
  if (!rollback_begin.ok()) { PrintDiagnostic(rollback_begin.diagnostic); return 1; }
  const auto rollback = manager.Rollback(rollback_begin.entry.identity.local_id, args.creation_millis + 211);
  if (!rollback.ok()) { PrintDiagnostic(rollback.diagnostic); return 1; }
  const auto horizons = manager.Horizons();
  if (!horizons.ok()) { PrintDiagnostic(horizons.diagnostic); return 1; }

  TransactionInventoryPageBody inv_body;
  inv_body.page_number = 4;
  inv_body.inventory = manager.inventory();
  inv_body.horizons = horizons.horizons;
  const auto inv_page = BuildTransactionInventoryPageBody(inv_body, args.page_size);
  if (!inv_page.ok()) { PrintDiagnostic(inv_page.diagnostic); return 1; }
  const auto parsed_inv = ParseTransactionInventoryPageBody(inv_page.serialized, inv_body.page_number);
  if (!parsed_inv.ok()) { PrintDiagnostic(parsed_inv.diagnostic); return 1; }

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
  DatabaseOpenConfig open;
  open.path = args.path;
  const auto opened = OpenDatabaseFile(open);
  if (!opened.ok()) { PrintDiagnostic(opened.diagnostic); return 1; }

  const auto metrics = manager.Metrics();
  const auto memory = DefaultMemoryManager().Snapshot();
  const bool ok = duplicate_failed && invalid_transition_failed &&
                  parsed_inv.body.inventory.entries.size() == 2 && opened.state.local_transaction_inventory_present;

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"local_entries\": " << parsed_inv.body.inventory.entries.size() << ",\n";
  std::cout << "  \"next_local_transaction_id\": " << parsed_inv.body.inventory.next_local_transaction_id << ",\n";
  std::cout << "  \"oit\": " << horizons.horizons.oldest_interesting_transaction.value << ",\n";
  std::cout << "  \"oat\": " << horizons.horizons.oldest_active_transaction.value << ",\n";
  std::cout << "  \"ost\": " << horizons.horizons.oldest_snapshot_transaction.value << ",\n";
  std::cout << "  \"snapshot_visible_through\": " << snapshot.snapshot.visible_through_local_transaction.value << ",\n";
  std::cout << "  \"cow_phase\": \"" << scratchbird::transaction::mga::CopyOnWriteMutationPhaseName(cow.mutation.phase) << "\",\n";
  std::cout << "  \"commit_state\": \"" << TransactionStateName(commit.entry.state) << "\",\n";
  std::cout << "  \"rollback_state\": \"" << TransactionStateName(rollback.entry.state) << "\",\n";
  std::cout << "  \"begin_total\": " << metrics.begin_total << ",\n";
  std::cout << "  \"commit_total\": " << metrics.commit_total << ",\n";
  std::cout << "  \"rollback_total\": " << metrics.rollback_total << ",\n";
  std::cout << "  \"failure_total\": " << metrics.failure_total << ",\n";
  std::cout << "  \"transaction_memory_peak_bytes\": " << memory.peak_bytes << ",\n";
  std::cout << "  \"cluster_path_traversed\": false,\n";
  std::cout << "  \"database_inventory_present\": " << (opened.state.local_transaction_inventory_present ? "true" : "false") << ",\n";
  std::cout << "  \"database_inventory_next_id\": " << opened.state.local_transaction_inventory.next_local_transaction_id << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
