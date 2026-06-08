// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "hash_digest.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "page_manager.hpp"
#include "startup_state.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef UuidToString
#undef UuidToString
#endif
#else
#include <unistd.h>
#endif

namespace {

namespace database = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace hash = scratchbird::core::hash;
namespace memory = scratchbird::core::memory;
namespace page = scratchbird::storage::page;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1770000000000ull;

bool Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

void PrintDiagnostic(const scratchbird::core::platform::DiagnosticRecord& diagnostic) {
  if (!diagnostic.diagnostic_code.empty()) {
    std::cerr << diagnostic.diagnostic_code << ':' << diagnostic.message_key;
    for (const auto& argument : diagnostic.arguments) {
      std::cerr << ' ' << argument.key << '=' << argument.value;
    }
    std::cerr << '\n';
  }
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  if (!generated.ok()) {
    std::cerr << "ELER-022 UUID generation failed\n";
    std::exit(EXIT_FAILURE);
  }
  return generated.value;
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "transaction_inventory_publish_fault_conformance";
  policy.hard_limit_bytes = 8 * 1024 * 1024;
  policy.soft_limit_bytes = 8 * 1024 * 1024;
  policy.per_context_limit_bytes = 8 * 1024 * 1024;
  policy.page_buffer_pool_limit_bytes = 8 * 1024 * 1024;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

bool ConfigureMemoryFixture() {
  const auto configured =
      memory::ConfigureDefaultMemoryManagerForFixture(
          MemoryPolicy(),
          "transaction_inventory_publish_fault_conformance");
  if (!configured.ok()) {
    PrintDiagnostic(configured.diagnostic);
  }
  return Require(configured.ok(),
                 "ELER-022 memory fixture should configure") &&
         Require(configured.fixture_mode,
                 "ELER-022 memory fixture should run in fixture mode");
}

std::string UuidText(const TypedUuid& typed) {
  return uuid::UuidToString(typed.value);
}

std::filesystem::path TempRoot() {
  std::string scope = std::filesystem::current_path().filename().string();
  if (scope.empty()) {
    scope = "default";
  }
#ifdef _WIN32
  const auto pid = static_cast<unsigned long long>(::GetCurrentProcessId());
#else
  const auto pid = static_cast<unsigned long long>(::getpid());
#endif
  auto root = std::filesystem::temp_directory_path() /
              ("scratchbird_txn_publish_fault_" + scope + "_" +
               std::to_string(pid));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  return root;
}

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path database_path;
  u32 page_size = 0;

  ~Fixture() {
    if (!root.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
  }
};

Fixture CreateFixture(std::string_view name, u64 offset) {
  Fixture fixture;
  fixture.root = TempRoot() / std::string(name);
  std::filesystem::create_directories(fixture.root);
  fixture.database_path = fixture.root / "eler022_publish.sbdb";

  database::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = MakeUuid(UuidKind::database, offset);
  create.filespace_uuid = MakeUuid(UuidKind::filespace, offset + 1);
  create.creation_unix_epoch_millis = kBaseMillis + offset + 2;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = database::CreateDatabaseFile(create);
  if (!created.ok()) {
    PrintDiagnostic(created.diagnostic);
  }
  Require(created.ok(), "ELER-022 fixture database create failed");
  fixture.page_size = created.state.header.page_size;
  return fixture;
}

txn::LocalTransactionInventory InventoryWithCommittedTransactions(u64 offset,
                                                                  int count) {
  auto inventory = txn::MakeEmptyLocalTransactionInventory();
  for (int i = 0; i < count; ++i) {
    const auto begun = txn::BeginLocalTransaction(
        inventory,
        MakeUuid(UuidKind::transaction, offset + static_cast<u64>(i * 10)),
        kBaseMillis + offset + static_cast<u64>(i * 10 + 1));
    if (!begun.ok()) {
      PrintDiagnostic(begun.diagnostic);
      std::exit(EXIT_FAILURE);
    }
    const auto committed = txn::CommitLocalTransaction(
        begun.inventory,
        begun.entry.identity.local_id,
        kBaseMillis + offset + static_cast<u64>(i * 10 + 2));
    if (!committed.ok()) {
      PrintDiagnostic(committed.diagnostic);
      std::exit(EXIT_FAILURE);
    }
    inventory = committed.inventory;
  }
  return inventory;
}

u64 PublishGeneration(const txn::LocalTransactionInventory& inventory) {
  return std::max<u64>(1, inventory.next_local_transaction_id == 0
                              ? 1
                              : inventory.next_local_transaction_id - 1);
}

bool SameInventory(const txn::LocalTransactionInventory& lhs,
                   const txn::LocalTransactionInventory& rhs) {
  if (lhs.next_local_transaction_id != rhs.next_local_transaction_id ||
      lhs.entries.size() != rhs.entries.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.entries.size(); ++i) {
    const auto& left = lhs.entries[i];
    const auto& right = rhs.entries[i];
    if (left.identity.local_id.value != right.identity.local_id.value ||
        UuidText(left.identity.transaction_uuid) !=
            UuidText(right.identity.transaction_uuid) ||
        left.identity.scope != right.identity.scope ||
        left.state != right.state ||
        left.begin_unix_epoch_millis != right.begin_unix_epoch_millis ||
        left.final_unix_epoch_millis != right.final_unix_epoch_millis ||
        left.begin_visible_through_local_transaction_id !=
            right.begin_visible_through_local_transaction_id ||
        left.evidence_record_required != right.evidence_record_required ||
        left.evidence_record_written != right.evidence_record_written ||
        left.rollback_only != right.rollback_only) {
      return false;
    }
  }
  return true;
}

std::string Sha256Hex(std::string_view payload) {
  const auto digest = hash::ComputeSha256Digest(
      reinterpret_cast<const byte*>(payload.data()), payload.size());
  return digest.ok() ? hash::HexLower(digest.digest) : std::string{};
}

std::string SerializeInventorySnapshot(std::string_view label,
                                       const txn::LocalTransactionInventory& inventory) {
  std::ostringstream out;
  out << "snapshot\t" << label << '\t'
      << inventory.next_local_transaction_id << '\t'
      << inventory.entries.size() << '\n';
  for (const auto& entry : inventory.entries) {
    out << "entry\t"
        << entry.identity.local_id.value << '\t'
        << UuidText(entry.identity.transaction_uuid) << '\t'
        << static_cast<u16>(entry.identity.scope) << '\t'
        << static_cast<u16>(entry.state) << '\t'
        << entry.begin_unix_epoch_millis << '\t'
        << entry.final_unix_epoch_millis << '\t'
        << entry.begin_visible_through_local_transaction_id << '\t'
        << (entry.evidence_record_required ? "1" : "0") << '\t'
        << (entry.evidence_record_written ? "1" : "0") << '\t'
        << (entry.rollback_only ? "1" : "0") << '\n';
  }
  out << "endsnapshot\t" << label << '\n';
  return out.str();
}

std::string BuildPublishJournalBody(std::string_view phase,
                                    const txn::LocalTransactionInventory& old_inventory,
                                    const txn::LocalTransactionInventory& new_inventory) {
  std::ostringstream out;
  out << "SBTXPUB002\n"
      << "phase\t" << phase << '\n'
      << "generation\t" << PublishGeneration(new_inventory) << '\n'
      << "authority\tdurable_transaction_inventory\n"
      << "checksum_algorithm\tsha256\n";
  out << SerializeInventorySnapshot("old", old_inventory);
  out << SerializeInventorySnapshot("new", new_inventory);
  out << "end\n";
  return out.str();
}

std::string BuildPublishJournal(std::string_view phase,
                                const txn::LocalTransactionInventory& old_inventory,
                                const txn::LocalTransactionInventory& new_inventory) {
  const std::string body =
      BuildPublishJournalBody(phase, old_inventory, new_inventory);
  return body + "checksum_sha256\t" + Sha256Hex(body) + '\n';
}

std::filesystem::path JournalPath(const Fixture& fixture) {
  return fixture.database_path.string() + ".sb.txn_publish";
}

void WriteTextAndSync(const std::filesystem::path& path,
                      const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(out), "ELER-022 text write open failed");
  out << content;
  out.close();
  Require(static_cast<bool>(out), "ELER-022 text write failed");
  const auto synced = disk::SyncFilesystemPath(path.string(), true);
  if (!synced.ok()) {
    PrintDiagnostic(synced.diagnostic);
  }
  Require(synced.ok(), "ELER-022 text sync failed");
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(static_cast<bool>(in), "ELER-022 text read open failed");
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

void AppendTextAndSync(const std::filesystem::path& path,
                       const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::app);
  Require(static_cast<bool>(out), "ELER-022 text append open failed");
  out << content;
  out.close();
  Require(static_cast<bool>(out), "ELER-022 text append failed");
  const auto synced = disk::SyncFilesystemPath(path.string(), true);
  if (!synced.ok()) {
    PrintDiagnostic(synced.diagnostic);
  }
  Require(synced.ok(), "ELER-022 text append sync failed");
}

void CorruptPrimaryInventoryRoot(const Fixture& fixture) {
  disk::FileDevice device;
  const auto opened =
      device.Open(fixture.database_path.string(), disk::FileOpenMode::open_existing);
  if (!opened.ok()) {
    PrintDiagnostic(opened.diagnostic);
  }
  Require(opened.ok(), "ELER-022 corrupt root open failed");
  const auto body_offset = page::CheckedPageBodyOffset(
      fixture.page_size,
      database::kTransactionInventoryPageNumber,
      disk::kPageHeaderSerializedBytes);
  if (!body_offset.ok()) {
    PrintDiagnostic(body_offset.diagnostic);
  }
  Require(body_offset.ok(), "ELER-022 root body offset failed");
  const std::vector<byte> corruption(96, static_cast<byte>(0x5a));
  const auto written =
      device.WriteAt(body_offset.offset, corruption.data(), corruption.size());
  if (!written.ok()) {
    PrintDiagnostic(written.diagnostic);
  }
  Require(written.ok(), "ELER-022 corrupt root write failed");
  const auto synced = device.Sync();
  if (!synced.ok()) {
    PrintDiagnostic(synced.diagnostic);
  }
  Require(synced.ok(), "ELER-022 corrupt root sync failed");
}

bool PersistInventory(const Fixture& fixture,
                      const txn::LocalTransactionInventory& inventory) {
  const auto persisted =
      database::PersistLocalTransactionInventoryToDatabase(
          fixture.database_path.string(), inventory);
  if (!persisted.ok()) {
    PrintDiagnostic(persisted.diagnostic);
  }
  return Require(persisted.ok(), "ELER-022 inventory persist failed");
}

database::LocalTransactionStoreResult LoadInventory(const Fixture& fixture) {
  return database::LoadLocalTransactionInventoryFromDatabase(
      fixture.database_path.string());
}

bool TestCommittedJournalRecoversNewSnapshot() {
  bool ok = true;
  auto fixture = CreateFixture("committed_new", 1000);
  const auto old_inventory = InventoryWithCommittedTransactions(1100, 1);
  const auto new_inventory = InventoryWithCommittedTransactions(1200, 2);
  ok = PersistInventory(fixture, old_inventory) && ok;
  ok = PersistInventory(fixture, new_inventory) && ok;
  CorruptPrimaryInventoryRoot(fixture);
  const auto loaded = LoadInventory(fixture);
  if (!loaded.ok()) {
    PrintDiagnostic(loaded.diagnostic);
  }
  ok = Require(loaded.ok(),
               "committed publish journal did not recover after primary corruption") && ok;
  ok = Require(SameInventory(loaded.inventory, new_inventory),
               "committed publish journal did not recover the new snapshot") && ok;
  const auto followup_inventory = InventoryWithCommittedTransactions(1300, 3);
  ok = PersistInventory(fixture, followup_inventory) && ok;
  const auto followup_loaded = LoadInventory(fixture);
  if (!followup_loaded.ok()) {
    PrintDiagnostic(followup_loaded.diagnostic);
  }
  ok = Require(followup_loaded.ok(),
               "recovered inventory did not admit follow-up publish") && ok;
  ok = Require(SameInventory(followup_loaded.inventory, followup_inventory),
               "follow-up publish after recovery did not survive") && ok;
  return ok;
}

bool TestPublishingJournalRecoversOldSnapshot() {
  bool ok = true;
  auto fixture = CreateFixture("publishing_old", 2000);
  const auto old_inventory = InventoryWithCommittedTransactions(2100, 1);
  const auto new_inventory = InventoryWithCommittedTransactions(2200, 2);
  ok = PersistInventory(fixture, old_inventory) && ok;
  WriteTextAndSync(JournalPath(fixture),
                   BuildPublishJournal("publishing", old_inventory, new_inventory));
  CorruptPrimaryInventoryRoot(fixture);
  const auto loaded = LoadInventory(fixture);
  if (!loaded.ok()) {
    PrintDiagnostic(loaded.diagnostic);
  }
  ok = Require(loaded.ok(),
               "publishing journal did not recover after primary corruption") && ok;
  ok = Require(SameInventory(loaded.inventory, old_inventory),
               "publishing journal did not recover the old snapshot") && ok;
  return ok;
}

bool TestCommittedJournalIgnoresStaleTail() {
  bool ok = true;
  auto fixture = CreateFixture("stale_tail", 3000);
  const auto old_inventory = InventoryWithCommittedTransactions(3100, 1);
  const auto new_inventory = InventoryWithCommittedTransactions(3200, 3);
  ok = PersistInventory(fixture, old_inventory) && ok;
  ok = PersistInventory(fixture, new_inventory) && ok;
  AppendTextAndSync(JournalPath(fixture),
                    "stale_tail_after_committed_checksum\tignored\n");
  CorruptPrimaryInventoryRoot(fixture);
  const auto loaded = LoadInventory(fixture);
  if (!loaded.ok()) {
    PrintDiagnostic(loaded.diagnostic);
  }
  ok = Require(loaded.ok(),
               "committed journal with stale tail did not recover") && ok;
  ok = Require(SameInventory(loaded.inventory, new_inventory),
               "committed journal stale tail changed recovered snapshot") && ok;
  return ok;
}

bool TestPartialJournalRequiresRecovery() {
  bool ok = true;
  auto fixture = CreateFixture("partial_journal", 4000);
  const auto old_inventory = InventoryWithCommittedTransactions(4100, 1);
  const auto new_inventory = InventoryWithCommittedTransactions(4200, 2);
  ok = PersistInventory(fixture, old_inventory) && ok;
  WriteTextAndSync(JournalPath(fixture),
                   BuildPublishJournalBody("publishing", old_inventory, new_inventory));
  CorruptPrimaryInventoryRoot(fixture);
  const auto loaded = LoadInventory(fixture);
  ok = Require(!loaded.ok(),
               "partial publish journal was accepted after primary corruption") && ok;
  ok = Require(loaded.diagnostic.diagnostic_code ==
                   "SB-TXN-INVENTORY-PUBLISH-RECOVERY-REQUIRED",
               "partial publish journal did not return recovery-required") && ok;
  return ok;
}

bool TestChecksumTamperFailsClosed() {
  bool ok = true;
  auto fixture = CreateFixture("checksum_tamper", 5000);
  const auto old_inventory = InventoryWithCommittedTransactions(5100, 1);
  const auto new_inventory = InventoryWithCommittedTransactions(5200, 2);
  ok = PersistInventory(fixture, old_inventory) && ok;
  ok = PersistInventory(fixture, new_inventory) && ok;
  std::string journal = ReadText(JournalPath(fixture));
  const std::size_t pos = journal.find("authority\tdurable_transaction_inventory");
  Require(pos != std::string::npos, "ELER-022 journal authority row missing");
  journal[pos + 10] = 'X';
  WriteTextAndSync(JournalPath(fixture), journal);
  CorruptPrimaryInventoryRoot(fixture);
  const auto loaded = LoadInventory(fixture);
  ok = Require(!loaded.ok(),
               "tampered publish journal was accepted after primary corruption") && ok;
  ok = Require(loaded.diagnostic.diagnostic_code ==
                   "SB-TXN-INVENTORY-PUBLISH-JOURNAL-CHECKSUM-MISMATCH",
               "tampered publish journal did not return checksum mismatch") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = ConfigureMemoryFixture() && ok;
  if (!ok) {
    return EXIT_FAILURE;
  }
  ok = TestCommittedJournalRecoversNewSnapshot() && ok;
  ok = TestPublishingJournalRecoversOldSnapshot() && ok;
  ok = TestCommittedJournalIgnoresStaleTail() && ok;
  ok = TestPartialJournalRequiresRecovery() && ok;
  ok = TestChecksumTamperFailsClosed() && ok;
  if (!ok) {
    return EXIT_FAILURE;
  }
  std::cout << "transaction_inventory_publish_fault_conformance=passed\n";
  return EXIT_SUCCESS;
}
