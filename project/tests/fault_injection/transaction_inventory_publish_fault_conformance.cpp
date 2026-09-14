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

std::size_t assertion_count = 0;
bool Require(bool condition, std::string_view message) {
  ++assertion_count;
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
               std::to_string(pid) + "_" + UuidText(MakeUuid(UuidKind::object, 1)));
  std::filesystem::create_directories(root);
  return root;
}

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path database_path;
  u32 page_size = 0;
  TypedUuid database_uuid;

  ~Fixture() {
    if (!root.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
  }
};

Fixture CreateFixture(std::string_view name, u64 offset) {
  Fixture fixture;
  fixture.root = TempRoot();
  (void)name;
  std::filesystem::create_directories(fixture.root);
  fixture.database_path = fixture.root / "eler022_publish.sbdb";

  database::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = MakeUuid(UuidKind::database, offset);
  fixture.database_uuid = create.database_uuid;
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
    int count, txn::LocalTransactionInventory inventory) {
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

bool SameInventory(const txn::LocalTransactionInventory& lhs,
                   const txn::LocalTransactionInventory& rhs) {
  if (lhs.next_local_transaction_id != rhs.next_local_transaction_id ||
      lhs.next_commit_sequence != rhs.next_commit_sequence ||
      lhs.entries.size() != rhs.entries.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.entries.size(); ++i) {
    const auto& left = lhs.entries[i];
    const auto& right = rhs.entries[i];
    if (left.identity.local_id.value != right.identity.local_id.value ||
        left.identity.transaction_uuid.value != right.identity.transaction_uuid.value ||
        left.identity.scope != right.identity.scope ||
        left.state != right.state ||
        left.archived_from_state != right.archived_from_state ||
        left.begin_unix_epoch_millis != right.begin_unix_epoch_millis ||
        left.final_unix_epoch_millis != right.final_unix_epoch_millis ||
        left.begin_visible_through_local_transaction_id !=
            right.begin_visible_through_local_transaction_id ||
        left.begin_visible_through_commit_sequence != right.begin_visible_through_commit_sequence ||
        left.commit_sequence != right.commit_sequence ||
        left.evidence_record_required != right.evidence_record_required ||
        left.evidence_record_written != right.evidence_record_written ||
        left.rollback_only != right.rollback_only) {
      return false;
    }
  }
  return true;
}

// Independent byte oracle: deliberately uses explicit shifts, not production
// encoder helpers. Offsets come from Core TRANSACTION_INVENTORY_BINARY_PUBLICATION_V5.
void Put(std::string& bytes, std::size_t offset, u64 value, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i)
    bytes[offset + i] = static_cast<char>((value >> (8 * i)) & 255);
}

std::string WithDigest(std::string body) {
  const auto digest = hash::ComputeSha256Digest(
      reinterpret_cast<const byte*>(body.data()), body.size());
  if (!digest.ok()) { std::exit(EXIT_FAILURE); }
  body.append(reinterpret_cast<const char*>(digest.digest.data()), digest.digest.size());
  return body;
}

std::string BuildPublishJournalBody(const Fixture& fixture, std::string_view phase,
                                    const txn::LocalTransactionInventory& old_inventory,
                                    const txn::LocalTransactionInventory& new_inventory,
                                    u64 old_generation = 17, u64 new_generation = 23) {
  std::string bytes(112 + (old_inventory.entries.size() + new_inventory.entries.size()) * 72, '\0');
  bytes.replace(0, 8, "SBTXP005");
  Put(bytes, 8, 5, 2);
  Put(bytes, 10, 112, 2);
  Put(bytes, 12, phase == "publishing" ? 1 : 2, 4);
  Put(bytes, 16, new_generation, 8);
  Put(bytes, 24, bytes.size() + 32, 8);
  Put(bytes, 32, old_inventory.next_local_transaction_id, 8);
  Put(bytes, 40, new_inventory.next_local_transaction_id, 8);
  Put(bytes, 48, old_inventory.entries.size(), 8);
  Put(bytes, 56, new_inventory.entries.size(), 8);
  bytes.replace(64, 16, reinterpret_cast<const char*>(fixture.database_uuid.value.bytes.data()), 16);
  Put(bytes, 80, old_inventory.next_commit_sequence, 8);
  Put(bytes, 88, new_inventory.next_commit_sequence, 8);
  Put(bytes, 96, old_generation, 8);
  std::size_t offset = 112;
  for (const auto* inventory : {&old_inventory, &new_inventory}) {
    for (const auto& entry : inventory->entries) {
      Put(bytes, offset, entry.identity.local_id.value, 8);
      bytes.replace(offset + 8, 16,
          reinterpret_cast<const char*>(entry.identity.transaction_uuid.value.bytes.data()), 16);
      Put(bytes, offset + 24, static_cast<u16>(entry.identity.scope), 2);
      Put(bytes, offset + 26, static_cast<u16>(entry.state), 2);
      Put(bytes, offset + 28, (entry.evidence_record_required ? 1 : 0) |
                              (entry.evidence_record_written ? 2 : 0) |
                              (entry.rollback_only ? 4 : 0) |
                              ((entry.archived_from_state == txn::TransactionState::committed ? 1u :
                                entry.archived_from_state == txn::TransactionState::rolled_back ? 2u :
                                entry.archived_from_state == txn::TransactionState::failed_terminal ? 3u : 0u) << 3), 4);
      Put(bytes, offset + 32, entry.begin_unix_epoch_millis, 8);
      Put(bytes, offset + 40, entry.final_unix_epoch_millis, 8);
      Put(bytes, offset + 48, entry.begin_visible_through_local_transaction_id, 8);
      Put(bytes, offset + 56, entry.begin_visible_through_commit_sequence, 8);
      Put(bytes, offset + 64, entry.commit_sequence, 8);
      offset += 72;
    }
  }
  return bytes;
}

std::string BuildPublishJournal(const Fixture& fixture, std::string_view phase,
                                const txn::LocalTransactionInventory& old_inventory,
                                const txn::LocalTransactionInventory& new_inventory) {
  return WithDigest(BuildPublishJournalBody(fixture, phase, old_inventory, new_inventory));
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
  // This codec/recovery fixture installs independently constructed inventory
  // images; obtain the exact current publication base explicitly. Native stale
  // writers are exercised separately by the retained-device concurrency test.
  disk::FileDevice device;
  const auto opened = device.Open(fixture.database_path.string(), disk::FileOpenMode::open_existing);
  if (!Require(opened.ok(), "publication fixture open failed")) return false;
  const auto current = database::LoadLocalTransactionInventoryFromOpenDevice(&device, fixture.page_size);
  if (!Require(current.ok(), "publication fixture base load failed")) return false;
  auto replacement = inventory;
  replacement.publication_base = current.inventory.publication_base;
  const auto persisted =
      database::PersistLocalTransactionInventoryToOpenDevice(
          &device, fixture.page_size, std::move(replacement));
  if (!persisted.ok()) {
    PrintDiagnostic(persisted.diagnostic);
  }
  return Require(persisted.ok(), "ELER-022 inventory persist failed");
}

database::LocalTransactionStoreResult LoadInventory(const Fixture& fixture) {
  // Force a fresh storage read for every injected fault; no warm process cache.
  disk::FileDevice device;
  const auto opened = device.Open(fixture.database_path.string(), disk::FileOpenMode::open_existing);
  if (!opened.ok()) {
    database::LocalTransactionStoreResult result;
    result.status = opened.status;
    result.diagnostic = opened.diagnostic;
    return result;
  }
  return database::LoadLocalTransactionInventoryFromOpenDevice(&device, fixture.page_size);
}

bool TestPublicationGenerations() {
  bool ok = true;
  auto fixture = CreateFixture("publication_generations", 8500);
  auto selected = LoadInventory(fixture);
  ok = Require(selected.ok() && selected.inventory.publication_base &&
      selected.inventory.publication_base->generation == 1,
      "initial native publication generation is not one") && ok;
  if (!ok) return false;
  const auto inventory = selected.inventory;
  // Independent generations deliberately differ from all transaction counters.
  // Readable pages are stale; the complete carrier must control both state and base.
  for (const auto phase : {"publishing", "committed"}) {
    WriteTextAndSync(JournalPath(fixture), WithDigest(
        BuildPublishJournalBody(fixture, phase, inventory, inventory, 17, 23)));
    selected = LoadInventory(fixture);
    const u64 expected = std::string_view(phase) == "publishing" ? 17 : 23;
    ok = Require(selected.ok() && SameInventory(selected.inventory, inventory) &&
        selected.inventory.publication_base && selected.inventory.publication_base->generation == expected,
        "recovery did not select exact old/new generation") && ok;
    if (!selected.ok()) return false;
    disk::FileDevice device;
    ok = Require(device.Open(fixture.database_path.string(), disk::FileOpenMode::open_existing).ok(),
        "open generation retry fixture") && ok;
    const auto next = database::PersistLocalTransactionInventoryToOpenDevice(
        &device, fixture.page_size, selected.inventory);
    ok = Require(next.ok() && next.inventory.publication_base &&
        next.inventory.publication_base->generation == 24,
        "publication reused interrupted generation or derived it from transaction counter") && ok;
  }
  selected = LoadInventory(fixture);
  ok = Require(selected.ok() && selected.inventory.publication_base &&
      selected.inventory.publication_base->generation == 24,
      "fresh native reopen lost publication generation") && ok;
  {
    disk::FileDevice device;
    if (!Require(device.Open(fixture.database_path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
        "read-only generation reopen")) return false;
    const auto readonly = database::LoadLocalTransactionInventoryFromOpenDevice(&device, fixture.page_size);
    ok = Require(readonly.ok() && readonly.inventory.publication_base == selected.inventory.publication_base,
        "read-only reopen changed publication provenance") && ok;
  }
  // Exhaustion in either selected state or an uncommitted attempt must refuse
  // before replacing the carrier or changing any database byte.
  for (const auto phase : {"publishing", "committed"}) {
    const auto carrier = WithDigest(BuildPublishJournalBody(fixture, phase, inventory, inventory, 17, ~u64{0}));
    WriteTextAndSync(JournalPath(fixture), carrier);
    selected = LoadInventory(fixture);
    if (!Require(selected.ok(), "valid exhausted carrier failed recovery")) return false;
    const auto before = ReadText(fixture.database_path);
    disk::FileDevice device;
    if (!Require(device.Open(fixture.database_path.string(), disk::FileOpenMode::open_existing).ok(),
        "open exhausted fixture")) return false;
    const auto refused = database::PersistLocalTransactionInventoryToOpenDevice(
        &device, fixture.page_size, selected.inventory);
    ok = Require(!refused.ok() && refused.diagnostic.diagnostic_code == "SB-TXN-INVENTORY-PAGE-GENERATION-INVALID" &&
        ReadText(JournalPath(fixture)) == carrier && ReadText(fixture.database_path) == before,
        "exhausted publication wrapped, succeeded or wrote state") && ok;
  }
  return ok;
}

bool TestCommittedJournalRecoversNewSnapshot() {
  bool ok = true;
  auto fixture = CreateFixture("committed_new", 1000);
  const auto old_inventory = InventoryWithCommittedTransactions(1100, 1, LoadInventory(fixture).inventory);
  const auto new_inventory = InventoryWithCommittedTransactions(1200, 1, old_inventory);
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
  const auto followup_inventory = InventoryWithCommittedTransactions(1300, 1, new_inventory);
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
  const auto old_inventory = InventoryWithCommittedTransactions(2100, 1, LoadInventory(fixture).inventory);
  const auto new_inventory = InventoryWithCommittedTransactions(2200, 1, old_inventory);
  ok = PersistInventory(fixture, old_inventory) && ok;
  WriteTextAndSync(JournalPath(fixture),
                   BuildPublishJournal(fixture, "publishing", old_inventory, new_inventory));
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

bool TestCommittedJournalRejectsStaleTail() {
  bool ok = true;
  auto fixture = CreateFixture("stale_tail", 3000);
  const auto old_inventory = InventoryWithCommittedTransactions(3100, 1, LoadInventory(fixture).inventory);
  const auto new_inventory = InventoryWithCommittedTransactions(3200, 2, old_inventory);
  ok = PersistInventory(fixture, old_inventory) && ok;
  ok = PersistInventory(fixture, new_inventory) && ok;
  AppendTextAndSync(JournalPath(fixture),
                    "stale_tail_after_committed_checksum\tignored\n");
  CorruptPrimaryInventoryRoot(fixture);
  const auto loaded = LoadInventory(fixture);
  if (!loaded.ok()) {
    PrintDiagnostic(loaded.diagnostic);
  }
  ok = Require(!loaded.ok() && loaded.inventory.entries.empty(),
               "binary publication with trailing bytes must refuse all authority") && ok;
  return ok;
}

bool TestPartialJournalRequiresRecovery() {
  bool ok = true;
  auto fixture = CreateFixture("partial_journal", 4000);
  const auto old_inventory = InventoryWithCommittedTransactions(4100, 1, LoadInventory(fixture).inventory);
  const auto new_inventory = InventoryWithCommittedTransactions(4200, 1, old_inventory);
  ok = PersistInventory(fixture, old_inventory) && ok;
  WriteTextAndSync(JournalPath(fixture),
                   BuildPublishJournalBody(fixture, "publishing", old_inventory, new_inventory));
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
  const auto old_inventory = InventoryWithCommittedTransactions(5100, 1, LoadInventory(fixture).inventory);
  const auto new_inventory = InventoryWithCommittedTransactions(5200, 1, old_inventory);
  ok = PersistInventory(fixture, old_inventory) && ok;
  ok = PersistInventory(fixture, new_inventory) && ok;
  std::string journal = ReadText(JournalPath(fixture));
  if (!Require(journal.size() > 176, "binary publication missing")) return false;
  journal[112 + 32] ^= 1; // Timestamp byte: structurally valid but unauthenticated.
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

bool TestInventoryEvolutionJournal() {
  bool ok=true;
  auto fixture=CreateFixture("immutable_evolution", 9000);
  const auto previous=InventoryWithCommittedTransactions(9100, 1, LoadInventory(fixture).inventory);
  const auto successor=InventoryWithCommittedTransactions(9200, 1, previous);
  if (!PersistInventory(fixture,previous)) return false;
  CorruptPrimaryInventoryRoot(fixture);
  for (const auto phase : {"publishing", "committed"}) {
    for (unsigned field=0;field<12;++field) {
      auto old_inventory=previous, new_inventory=successor;
      auto& entry=new_inventory.entries[0];
      if(field==0)entry.identity.transaction_uuid=MakeUuid(UuidKind::transaction,9300);
      if(field==1)entry.identity.scope=txn::TransactionScope::cluster_global;
      if(field==2)++entry.begin_unix_epoch_millis;
      if(field==3)++entry.begin_visible_through_local_transaction_id;
      if(field==4)++entry.final_unix_epoch_millis;
      if(field==5){entry.state=txn::TransactionState::rolled_back;entry.commit_sequence=0;}
      if(field==6){entry.state=txn::TransactionState::active;entry.commit_sequence=0;}
      if(field==7)entry.evidence_record_required=false;
      if(field==8)entry.evidence_record_written=false;
      if(field==9){entry.commit_sequence=new_inventory.next_commit_sequence++;}
      if(field==10){++entry.begin_visible_through_commit_sequence;entry.commit_sequence=new_inventory.next_commit_sequence++;}
      if(field==11){new_inventory.entries.erase(new_inventory.entries.begin());new_inventory.entries[0].commit_sequence=1;}
      const auto bytes=BuildPublishJournal(fixture,phase,old_inventory,new_inventory);
      WriteTextAndSync(JournalPath(fixture),bytes);
      const auto before=ReadText(fixture.database_path);
      const auto result=LoadInventory(fixture);
      ok=Require(!result.ok()&&result.inventory.entries.empty(),
        "resealed journal admitted changed predecessor facts field="+std::to_string(field))&&ok;
      ok=Require(ReadText(JournalPath(fixture))==bytes&&ReadText(fixture.database_path)==before,
        "journal evolution refusal mutated durable state")&&ok;
    }
    WriteTextAndSync(JournalPath(fixture),BuildPublishJournal(fixture,phase,previous,successor));
    const auto valid=LoadInventory(fixture);
    ok=Require(valid.ok()&&SameInventory(valid.inventory,
        std::string_view(phase)=="publishing"?previous:successor),"valid successor journal recovery")&&ok;
  }
  return ok;
}

bool TestBinaryPublicationContract() {
  bool ok = true;
  auto fixture = CreateFixture("binary_contract", 7000);
  auto old_inventory = InventoryWithCommittedTransactions(7100, 1, LoadInventory(fixture).inventory);
  const auto read_only = txn::BeginLocalTransaction(old_inventory,
      MakeUuid(UuidKind::transaction, 7110), kBaseMillis + 7111);
  if (!Require(read_only.ok(), "begin mixed codec fixture")) return false;
  old_inventory = read_only.inventory;
  // Independent mixed-scope codec fixture, established before publication.
  // Retained identity, begin facts and outcomes must survive each successor.
  old_inventory.entries.back().identity.scope = txn::TransactionScope::cluster_global;
  old_inventory.entries.back().state = txn::TransactionState::read_only_active;
  old_inventory.entries.back().evidence_record_required = false;
  old_inventory.entries.back().rollback_only = true;
  auto new_inventory = InventoryWithCommittedTransactions(7200, 1, old_inventory);
  const auto archived = txn::ArchiveLocalTransaction(new_inventory, new_inventory.entries[0].identity.local_id);
  ok = Require(archived.ok(), "archive actual committed fixture transaction") && ok;
  if (!archived.ok()) return false;
  new_inventory = archived.inventory;
  const auto abort_begin = txn::BeginLocalTransaction(new_inventory, MakeUuid(UuidKind::transaction, 7301), kBaseMillis + 7301);
  if (!Require(abort_begin.ok(), "begin archived rollback fixture")) return false;
  const auto aborted = txn::RollbackLocalTransaction(abort_begin.inventory, abort_begin.entry.identity.local_id, kBaseMillis + 7302);
  if (!Require(aborted.ok(), "rollback archive fixture")) return false;
  const auto archived_abort = txn::ArchiveLocalTransaction(aborted.inventory, abort_begin.entry.identity.local_id);
  if (!Require(archived_abort.ok(), "archive actual rolled-back fixture transaction")) return false;
  new_inventory = archived_abort.inventory;
  ok = PersistInventory(fixture, old_inventory) && ok;
  ok = PersistInventory(fixture, new_inventory) && ok;
  // Creation publishes1. New read-only admission first publishes starting2,
  // then active3; the following terminal-state replacement publishes4.
  const auto golden = WithDigest(BuildPublishJournalBody(fixture, "committed", old_inventory, new_inventory, 3, 4));
  const auto actual = ReadText(JournalPath(fixture));
  ok = Require(actual == golden, "live writer differs from independent binary byte oracle") && ok;
  for (const auto& entry : new_inventory.entries)
    ok = Require(actual.find(UuidText(entry.identity.transaction_uuid)) == std::string::npos,
                 "transaction identity leaked as canonical UUID text") && ok;
  const auto before = ReadText(fixture.database_path);
  const auto invalid_write = [&](txn::LocalTransactionInventory inventory) {
    const auto result = database::PersistLocalTransactionInventoryToDatabase(
        fixture.database_path.string(), std::move(inventory));
    ok = Require(!result.ok(), "malformed writer inventory accepted") && ok;
    ok = Require(ReadText(JournalPath(fixture)) == golden &&
                 ReadText(fixture.database_path) == before,
                 "refused writer mutated durable authority") && ok;
  };
  { auto bad = new_inventory; bad.entries[0].identity.transaction_uuid.kind = UuidKind::object;
    invalid_write(bad); }
  { auto bad = new_inventory; bad.entries[0].identity.transaction_uuid.value.bytes[6] &= 15;
    invalid_write(bad); }
  { auto bad = new_inventory; bad.entries[1].identity.local_id = bad.entries[0].identity.local_id;
    invalid_write(bad); }
  { auto bad = new_inventory; bad.entries[1].identity.transaction_uuid = bad.entries[0].identity.transaction_uuid;
    invalid_write(bad); }
  { auto bad = new_inventory; bad.entries[0].state = static_cast<txn::TransactionState>(65535);
    invalid_write(bad); }
  { auto bad = new_inventory; bad.next_local_transaction_id = 0; invalid_write(bad); }
  { auto bad = new_inventory; bad.next_commit_sequence = 0; invalid_write(bad); }
  { auto bad = new_inventory; bad.entries[0].commit_sequence = 0; invalid_write(bad); }
  { auto bad = new_inventory; bad.entries[1].commit_sequence = 2; invalid_write(bad); }
  { auto bad = new_inventory; bad.entries[0].begin_visible_through_commit_sequence = bad.next_commit_sequence; invalid_write(bad); }

  // An unsupported journal must be refused before writing even when the
  // primary page chain is intact; no automatic prototype conversion.
  auto retired_body = golden.substr(0, 96) + golden.substr(112, golden.size() - 112 - 32);
  retired_body.replace(0, 8, "SBTXP004");
  Put(retired_body, 8, 4, 2); Put(retired_body, 10, 96, 2);
  Put(retired_body, 16, new_inventory.next_local_transaction_id - 1, 8);
  Put(retired_body, 24, retired_body.size() + 32, 8);
  for (const auto& legacy : {std::string("SBTXPUB002\n") + std::string(150, 'x'), WithDigest(retired_body)}) {
    WriteTextAndSync(JournalPath(fixture), legacy);
    const auto legacy_read = LoadInventory(fixture);
    const auto legacy_write = database::PersistLocalTransactionInventoryToDatabase(
        fixture.database_path.string(), new_inventory);
    ok = Require(!legacy_read.ok() && !legacy_write.ok() &&
                 ReadText(JournalPath(fixture)) == legacy &&
                 ReadText(fixture.database_path) == before,
                 "legacy journal silently converted or used as authority") && ok;
  }
  WriteTextAndSync(JournalPath(fixture), golden);
  CorruptPrimaryInventoryRoot(fixture);
  const auto refused = [&](const std::string& bytes) {
    WriteTextAndSync(JournalPath(fixture), bytes);
    const auto loaded = LoadInventory(fixture);
    ok = Require(!loaded.ok() && loaded.inventory.entries.empty(),
                 "malformed binary recovery returned complete or partial authority") && ok;
  };
  for (std::size_t size = 0; size < golden.size(); ++size)
    refused(golden.substr(0, size));
  refused(golden + "tail");
  refused(std::string("SBTXPUB002\n") + std::string(150, 'x'));
  const auto mutate = [&](std::size_t offset, u64 value, std::size_t width) {
    auto body = golden.substr(0, golden.size() - 32);
    Put(body, offset, value, width);
    refused(WithDigest(body)); // Valid integrity: exercise semantic admission.
  };
  mutate(8, 2, 2); mutate(8, 3, 2); mutate(10, 80, 2);
  mutate(12, 0, 4); mutate(12, 3, 4);
  mutate(16, 0, 8); mutate(16, 2, 8);
  mutate(96, 0, 8); mutate(96, 4, 8); // Old and new generations may not be equal.
  for (std::size_t reserved = 104; reserved < 112; ++reserved) mutate(reserved, 1, 1);
  mutate(8, 4, 2);
  mutate(24, 0, 8); mutate(24, ~u64{0}, 8);
  mutate(32, 0, 8); mutate(40, 0, 8);
  mutate(48, ~u64{0}, 8); mutate(56, ~u64{0}, 8);
  mutate(80, 0, 8); mutate(88, 0, 8);
  // Every persisted UUID slot rejects all other version/variant combinations,
  // including the database binding and the unselected old snapshot.
  std::vector<std::size_t> uuid_offsets{64};
  for (std::size_t row = 112; row < golden.size() - 32; row += 72)
    uuid_offsets.push_back(row + 8);
  for (const auto offset : uuid_offsets) {
    for (unsigned version = 0; version < 16; ++version)
      for (unsigned variant = 0; variant < 4; ++variant) {
        if (version == 7 && variant == 2) continue;
        auto body = golden.substr(0, golden.size() - 32);
        body[offset + 6] = static_cast<char>((static_cast<unsigned char>(body[offset + 6]) & 15) | version << 4);
        body[offset + 8] = static_cast<char>((static_cast<unsigned char>(body[offset + 8]) & 63) | variant << 6);
        refused(WithDigest(body));
      }
  }
  for (std::size_t row = 112; row < golden.size() - 32; row += 72) {
    mutate(row, 0, 8);
    mutate(row + 24, 2, 2);
    mutate(row + 26, 0, 2); mutate(row + 26, 14, 2);
    mutate(row + 28, 32, 4);
    mutate(row + 56, ~u64{0}, 8);
    mutate(row + 64, ~u64{0}, 8);
  }
  mutate(112 + 72, 1, 8); // Duplicate local number in old snapshot.
  {
    auto body = golden.substr(0, golden.size() - 32);
    body.replace(112 + 72 + 8, 16, body.substr(112 + 8, 16));
    refused(WithDigest(body)); // Duplicate UUID with different local number.
  }
  {
    auto body = golden.substr(0, golden.size() - 32);
    const auto foreign = MakeUuid(UuidKind::database, 7999);
    body.replace(64, 16, reinterpret_cast<const char*>(foreign.value.bytes.data()), 16);
    refused(WithDigest(body));
  }
  // Refusals must not poison recovery of the valid exact payload.
  WriteTextAndSync(JournalPath(fixture), golden);
  const auto recovered = LoadInventory(fixture);
  ok = Require(recovered.ok() && SameInventory(recovered.inventory, new_inventory),
               "valid binary recovery after refusal did not preserve exact inventory") && ok;
  return ok;
}

bool TestStatementInventoryFenceIgnoresUnrelatedDatabaseGrowth() {
  bool ok = true;
  auto fixture = CreateFixture("statement_fence", 6000);
  const auto inventory = InventoryWithCommittedTransactions(6100, 1, LoadInventory(fixture).inventory);
  ok = PersistInventory(fixture, inventory) && ok;

  const auto acquired = database::AcquireStrongLocalTransactionInventorySnapshot(
      fixture.database_path.string());
  if (!acquired.ok()) {
    PrintDiagnostic(acquired.diagnostic);
  }
  ok = Require(acquired.ok(),
               "statement inventory authority was not acquired") && ok;
  if (!acquired.ok()) {
    return false;
  }

  // Relation/catalog publication can extend the shared database file without
  // changing the transaction inventory. That must not invalidate a retained
  // statement authority merely because the database-wide size/mtime changed.
  {
    std::ofstream out(fixture.database_path,
                      std::ios::binary | std::ios::app);
    const std::vector<char> unrelated_page(fixture.page_size, 0);
    out.write(unrelated_page.data(),
              static_cast<std::streamsize>(unrelated_page.size()));
    out.close();
    ok = Require(static_cast<bool>(out),
                 "unrelated database growth write failed") && ok;
  }
  const auto database_sync =
      disk::SyncFilesystemPath(fixture.database_path.string(), true);
  if (!database_sync.ok()) {
    PrintDiagnostic(database_sync.diagnostic);
  }
  ok = Require(database_sync.ok(),
               "unrelated database growth sync failed") && ok;

  const auto after_growth =
      database::RevalidateLocalTransactionInventorySnapshot(
          *acquired.snapshot);
  ok = Require(after_growth.ok(),
               "unrelated database growth invalidated transaction inventory") &&
       ok;

  // The inventory-owned publish-journal identity remains the cheap TOCTOU
  // fence. Any change there must invalidate the retained statement authority.
  AppendTextAndSync(JournalPath(fixture), "statement_fence_change\n");
  const auto after_journal_change =
      database::RevalidateLocalTransactionInventorySnapshot(
          *acquired.snapshot);
  ok = Require(!after_journal_change.ok(),
               "transaction journal change did not invalidate inventory") &&
       ok;
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
  ok = TestPublicationGenerations() && ok;
  ok = TestPublishingJournalRecoversOldSnapshot() && ok;
  ok = TestCommittedJournalRejectsStaleTail() && ok;
  ok = TestPartialJournalRequiresRecovery() && ok;
  ok = TestChecksumTamperFailsClosed() && ok;
  ok = TestInventoryEvolutionJournal() && ok;
  ok = TestBinaryPublicationContract() && ok;
  ok = TestStatementInventoryFenceIgnoresUnrelatedDatabaseGrowth() && ok;
  if (!ok) {
    return EXIT_FAILURE;
  }
  std::cout << "transaction_inventory_publish_fault_conformance=passed checks=" << assertion_count << "\n";
  return EXIT_SUCCESS;
}
