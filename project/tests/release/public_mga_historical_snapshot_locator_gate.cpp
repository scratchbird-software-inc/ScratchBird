// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "historical_snapshot_locator.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "row_version.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1770130000000ull;
constexpr u32 kPageSize = 16384;

bool Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_mga_historical_snapshot_locator_gate";
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
          MemoryPolicy(), "public_mga_historical_snapshot_locator_gate");
  return Expect(configured.ok(),
                "PCR-079 memory fixture should configure") &&
         Expect(configured.fixture_mode,
                "PCR-079 should use fixture memory");
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

struct Fixture {
  std::filesystem::path database_path;
};

Fixture MakeFixture(const std::filesystem::path& work_dir) {
  std::filesystem::create_directories(work_dir);
  Fixture fixture;
  fixture.database_path = work_dir / "pcr079_historical_snapshot.sbdb";
  std::filesystem::remove(fixture.database_path);

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = MakeUuid(UuidKind::database, 1000);
  create.filespace_uuid = MakeUuid(UuidKind::filespace, 1001);
  create.page_size = kPageSize;
  create.creation_unix_epoch_millis = kBaseMillis + 1000;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Expect(created.ok(), "PCR-079 fixture database should create");
  return fixture;
}

struct SeededInventory {
  txn::LocalTransactionInventory inventory;
  txn::TransactionInventoryEntry archive_entry;
  txn::TransactionInventoryEntry hot_entry;
  txn::TransactionInventoryEntry audit_entry;
};

bool PersistInventory(const Fixture& fixture,
                      const txn::LocalTransactionInventory& inventory,
                      const char* message) {
  const auto persisted =
      db::PersistLocalTransactionInventoryToDatabase(fixture.database_path.string(),
                                                     inventory);
  if (!persisted.ok()) {
    std::cerr << persisted.diagnostic.diagnostic_code << ':'
              << persisted.diagnostic.message_key << '\n';
  }
  return Expect(persisted.ok(), message);
}

SeededInventory SeedInventory(const Fixture& fixture, bool* ok) {
  SeededInventory seeded;
  const auto loaded =
      db::LoadLocalTransactionInventoryFromDatabase(fixture.database_path.string());
  *ok = Expect(loaded.ok(), "PCR-079 initial inventory should load") && *ok;
  txn::LocalTransactionInventory inventory = loaded.inventory;

  const auto archive_begin =
      txn::BeginLocalTransaction(inventory,
                                 MakeUuid(UuidKind::transaction, 2000),
                                 kBaseMillis + 2000);
  *ok = Expect(archive_begin.ok(), "PCR-079 archive target should begin") && *ok;
  const auto archive_commit =
      txn::CommitLocalTransaction(archive_begin.inventory,
                                  archive_begin.entry.identity.local_id,
                                  kBaseMillis + 2010);
  *ok = Expect(archive_commit.ok(), "PCR-079 archive target should commit") && *ok;
  const auto archived =
      txn::ArchiveLocalTransaction(archive_commit.inventory,
                                   archive_commit.entry.identity.local_id);
  *ok = Expect(archived.ok(), "PCR-079 archive target should archive") && *ok;
  inventory = archived.inventory;
  seeded.archive_entry = archived.entry;

  const auto hot_begin =
      txn::BeginLocalTransaction(inventory,
                                 MakeUuid(UuidKind::transaction, 2100),
                                 kBaseMillis + 2100);
  *ok = Expect(hot_begin.ok(), "PCR-079 local hot target should begin") && *ok;
  const auto hot_commit =
      txn::CommitLocalTransaction(hot_begin.inventory,
                                  hot_begin.entry.identity.local_id,
                                  kBaseMillis + 2110);
  *ok = Expect(hot_commit.ok(), "PCR-079 local hot target should commit") && *ok;
  inventory = hot_commit.inventory;
  seeded.hot_entry = hot_commit.entry;

  auto audit =
      txn::BeginLocalReadOnlyTransaction(inventory,
                                         MakeUuid(UuidKind::transaction, 2200),
                                         kBaseMillis + 2200);
  *ok = Expect(audit.ok(), "PCR-079 audit reader should begin read-only") && *ok;
  for (auto& entry : audit.inventory.entries) {
    if (entry.identity.local_id.value == audit.entry.identity.local_id.value) {
      entry.begin_visible_through_local_transaction_id =
          seeded.archive_entry.identity.local_id.value;
      audit.entry = entry;
      break;
    }
  }
  inventory = audit.inventory;
  seeded.audit_entry = audit.entry;
  seeded.inventory = inventory;
  *ok = PersistInventory(fixture,
                         inventory,
                         "PCR-079 seeded inventory should persist") && *ok;
  return seeded;
}

txn::HistoricalAuditSnapshotEvidence FullEvidence() {
  txn::HistoricalAuditSnapshotEvidence evidence;
  evidence.transaction_inventory_authoritative = true;
  evidence.archive_manifest_authoritative = true;
  evidence.security_policy_authoritative = true;
  evidence.catalog_epoch_authoritative = true;
  evidence.cluster_epoch_authoritative = true;
  evidence.external_cluster_provider_available = false;
  evidence.security_epoch = 7;
  evidence.catalog_generation_id = 11;
  evidence.cluster_epoch = 1;
  return evidence;
}

txn::HistoricalAuditSnapshotRequest RequestFor(
    const SeededInventory& seeded,
    const txn::TransactionInventoryEntry& target,
    txn::HistoricalAuditLocationClass location_class) {
  txn::HistoricalAuditSnapshotRequest request;
  request.inventory = seeded.inventory;
  request.audit_reader_transaction = seeded.audit_entry.identity.local_id;
  request.target_local_transaction_id = target.identity.local_id;
  request.requested_location_class = location_class;
  request.evidence = FullEvidence();
  return request;
}

txn::RowIdentity RowIdentity(u64 offset, bool* ok) {
  const auto row = txn::MakeRowIdentity(MakeUuid(UuidKind::row, offset));
  *ok = Expect(row.ok(), "PCR-079 row identity should validate") && *ok;
  return row.identity;
}

txn::RowVersionMetadata Version(txn::RowIdentity row,
                                const txn::TransactionInventoryEntry& creator,
                                u64 sequence,
                                txn::RowVersionState state,
                                txn::TransactionState creator_state,
                                bool payload_present) {
  txn::RowVersionMetadata metadata;
  metadata.identity.row = row;
  metadata.identity.creator_transaction = creator.identity;
  metadata.identity.version_sequence = sequence;
  metadata.state = state;
  metadata.creator_transaction_state = creator_state;
  metadata.payload_present = payload_present;
  return metadata;
}

bool LocalArchiveHistoricalVisibilityProof(const SeededInventory& seeded) {
  bool ok = true;
  const auto result = txn::CreateHistoricalAuditSnapshot(
      RequestFor(seeded,
                 seeded.archive_entry,
                 txn::HistoricalAuditLocationClass::local_archive));
  ok = Expect(result.ok(), "PCR-079 archive historical snapshot should succeed") && ok;
  ok = Expect(result.location_class ==
                  txn::HistoricalAuditLocationClass::local_archive,
              "PCR-079 archive location class should remain exact") && ok;
  ok = Expect(result.writes_refused && !result.fail_closed,
              "PCR-079 archive snapshot should be read-only and queryable") && ok;
  ok = Expect(result.visibility_snapshot.visible_through_local_transaction_id ==
                  seeded.archive_entry.identity.local_id.value,
              "PCR-079 archive snapshot should bind target as high-water") && ok;
  ok = Expect(!result.visibility_snapshot.allow_reader_own_uncommitted,
              "PCR-079 historical snapshot should not expose own uncommitted writes") && ok;

  const auto archived_row = RowIdentity(3000, &ok);
  const auto later_row = RowIdentity(3001, &ok);
  if (!ok) {
    return false;
  }
  const auto archived_version =
      Version(archived_row,
              seeded.archive_entry,
              1,
              txn::RowVersionState::committed,
              txn::TransactionState::archived,
              true);
  const auto later_version =
      Version(later_row,
              seeded.hot_entry,
              1,
              txn::RowVersionState::committed,
              txn::TransactionState::committed,
              true);
  const auto own_uncommitted =
      Version(archived_row,
              seeded.audit_entry,
              2,
              txn::RowVersionState::uncommitted,
              txn::TransactionState::read_only_active,
              true);
  ok = Expect(txn::EvaluateVisibility(archived_version,
                                      result.visibility_snapshot).decision ==
                  txn::VisibilityDecision::visible,
              "PCR-079 archived target version should be visible") && ok;
  ok = Expect(txn::EvaluateVisibility(later_version,
                                      result.visibility_snapshot).decision ==
                  txn::VisibilityDecision::invisible,
              "PCR-079 later hot version should be invisible to archive as-of") && ok;
  ok = Expect(txn::EvaluateVisibility(own_uncommitted,
                                      result.visibility_snapshot).decision ==
                  txn::VisibilityDecision::wait_for_transaction,
              "PCR-079 historical snapshot should refuse own uncommitted visibility") && ok;
  return ok;
}

bool LocalHotHistoricalVisibilityProof(const SeededInventory& seeded) {
  bool ok = true;
  const auto result = txn::CreateHistoricalAuditSnapshot(
      RequestFor(seeded,
                 seeded.hot_entry,
                 txn::HistoricalAuditLocationClass::local_hot));
  ok = Expect(result.ok(), "PCR-079 local hot historical snapshot should succeed") && ok;
  ok = Expect(result.location_class == txn::HistoricalAuditLocationClass::local_hot,
              "PCR-079 local hot location class should remain exact") && ok;
  ok = Expect(result.visibility_snapshot.visible_through_local_transaction_id ==
                  seeded.hot_entry.identity.local_id.value,
              "PCR-079 hot snapshot should bind target as high-water") && ok;
  const auto hot_row = RowIdentity(3100, &ok);
  if (!ok) {
    return false;
  }
  const auto hot_version =
      Version(hot_row,
              seeded.hot_entry,
              1,
              txn::RowVersionState::committed,
              txn::TransactionState::committed,
              true);
  ok = Expect(txn::EvaluateVisibility(hot_version,
                                      result.visibility_snapshot).decision ==
                  txn::VisibilityDecision::visible,
              "PCR-079 hot target version should be visible") && ok;
  return ok;
}

bool FailClosedProofs(const SeededInventory& seeded) {
  bool ok = true;
  auto missing_archive =
      RequestFor(seeded,
                 seeded.archive_entry,
                 txn::HistoricalAuditLocationClass::local_archive);
  missing_archive.evidence.archive_manifest_authoritative = false;
  const auto missing_archive_result =
      txn::CreateHistoricalAuditSnapshot(missing_archive);
  ok = Expect(!missing_archive_result.ok() &&
                  missing_archive_result.diagnostic.diagnostic_code ==
                      "SB-MGA-HISTORICAL-SNAPSHOT-ARCHIVE-EVIDENCE-REQUIRED",
              "PCR-079 missing archive evidence should fail closed") && ok;

  auto missing_authority =
      RequestFor(seeded,
                 seeded.hot_entry,
                 txn::HistoricalAuditLocationClass::local_hot);
  missing_authority.evidence.cluster_epoch_authoritative = false;
  const auto missing_authority_result =
      txn::CreateHistoricalAuditSnapshot(missing_authority);
  ok = Expect(!missing_authority_result.ok() &&
                  missing_authority_result.diagnostic.diagnostic_code ==
                      "SB-MGA-HISTORICAL-SNAPSHOT-AUTHORITY-EVIDENCE-REQUIRED",
              "PCR-079 missing cluster epoch evidence should fail closed") && ok;

  auto retired =
      RequestFor(seeded,
                 seeded.archive_entry,
                 txn::HistoricalAuditLocationClass::retired);
  retired.retired_history_evidence_present = true;
  const auto retired_result = txn::CreateHistoricalAuditSnapshot(retired);
  ok = Expect(!retired_result.ok() &&
                  retired_result.retired_history_exact &&
                  retired_result.location_class ==
                      txn::HistoricalAuditLocationClass::retired &&
                  retired_result.diagnostic.diagnostic_code ==
                      "SB-MGA-HISTORICAL-SNAPSHOT-RETIRED-EXACT",
              "PCR-079 retired history should be exact and fail closed") && ok;

  auto remote =
      RequestFor(seeded,
                 seeded.hot_entry,
                 txn::HistoricalAuditLocationClass::remote);
  remote.evidence.external_cluster_provider_available = false;
  const auto remote_result = txn::CreateHistoricalAuditSnapshot(remote);
  ok = Expect(!remote_result.ok() &&
                  remote_result.no_cluster_remote_fail_closed &&
                  remote_result.location_class ==
                      txn::HistoricalAuditLocationClass::remote &&
                  remote_result.diagnostic.diagnostic_code ==
                      "SB-MGA-HISTORICAL-SNAPSHOT-REMOTE-NO-CLUSTER",
              "PCR-079 no-cluster remote snapshot should fail closed") && ok;

  auto remote_external =
      RequestFor(seeded,
                 seeded.hot_entry,
                 txn::HistoricalAuditLocationClass::remote);
  remote_external.evidence.external_cluster_provider_available = true;
  const auto remote_external_result =
      txn::CreateHistoricalAuditSnapshot(remote_external);
  ok = Expect(!remote_external_result.ok() &&
                  remote_external_result.diagnostic.diagnostic_code ==
                      "SB-MGA-HISTORICAL-SNAPSHOT-REMOTE-EXTERNAL-PROVIDER-REQUIRED",
              "PCR-079 remote cluster snapshot should require external provider") && ok;

  auto write_intent =
      RequestFor(seeded,
                 seeded.hot_entry,
                 txn::HistoricalAuditLocationClass::local_hot);
  write_intent.write_intent = true;
  const auto write_result = txn::CreateHistoricalAuditSnapshot(write_intent);
  ok = Expect(!write_result.ok() &&
                  write_result.writes_refused &&
                  write_result.diagnostic.diagnostic_code ==
                      "SB-MGA-HISTORICAL-SNAPSHOT-WRITE-REFUSED",
              "PCR-079 historical snapshot writes should be refused") && ok;
  return ok;
}

bool DurableReloadProof(const Fixture& fixture, const SeededInventory& seeded) {
  const auto loaded =
      db::LoadLocalTransactionInventoryFromDatabase(fixture.database_path.string());
  if (!Expect(loaded.ok(), "PCR-079 durable inventory should reload")) {
    return false;
  }
  auto request = RequestFor(seeded,
                            seeded.archive_entry,
                            txn::HistoricalAuditLocationClass::local_archive);
  request.inventory = loaded.inventory;
  const auto result = txn::CreateHistoricalAuditSnapshot(request);
  return Expect(result.ok(),
                "PCR-079 historical snapshot should run from reloaded inventory") &&
         Expect(result.target_transaction_state == "archived",
                "PCR-079 archive target state should survive reload");
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path work_dir =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::path("public_mga_historical_snapshot_locator_gate_tmp");
  std::filesystem::remove_all(work_dir);

  bool ok = ConfigureMemoryFixture();
  const Fixture fixture = MakeFixture(work_dir);
  const SeededInventory seeded = SeedInventory(fixture, &ok);
  ok = LocalArchiveHistoricalVisibilityProof(seeded) && ok;
  ok = LocalHotHistoricalVisibilityProof(seeded) && ok;
  ok = FailClosedProofs(seeded) && ok;
  ok = DurableReloadProof(fixture, seeded) && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
