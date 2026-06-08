// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_physical_cleanup_coordinator.hpp"

#include "disk_device.hpp"
#include "row_data_page.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
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

namespace api = scratchbird::engine::internal_api;
namespace disk = scratchbird::storage::disk;
namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

constexpr platform::u64 kBaseMillis = 1770410000000ull;
constexpr platform::u32 kRowPageSize = 16384;
constexpr platform::u32 kBlobPageSize = 8192;
constexpr platform::u32 kAllocationMapPageSize = 8192;
constexpr platform::u64 kFirstBlobPage = 512;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid MakeUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + seed);
  if (!generated.ok()) {
    Fail("uuid generation failed");
  }
  return generated.value;
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
              ("scratchbird_eler024_integrated_cleanup_" + scope + "_" +
               std::to_string(pid));
  std::filesystem::create_directories(root);
  return root;
}

void RemoveDeviceArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + ".sb.owner.lock", ignored);
  std::filesystem::remove(path.string() + ".sb.route.owner.lock", ignored);
}

void RemoveTempRootIfEmpty() {
  std::error_code ignored;
  std::filesystem::remove(TempRoot(), ignored);
}

platform::TypedUuid TransactionUuid(platform::u64 local_id) {
  static std::map<platform::u64, platform::TypedUuid> transaction_uuids;
  const auto found = transaction_uuids.find(local_id);
  if (found != transaction_uuids.end()) {
    return found->second;
  }
  auto inserted = transaction_uuids.emplace(
      local_id, MakeUuid(platform::UuidKind::transaction, 100 + local_id));
  return inserted.first->second;
}

txn::TransactionIdentity TransactionIdentity(platform::u64 local_id) {
  txn::TransactionIdentity identity;
  identity.local_id = txn::MakeLocalTransactionId(local_id);
  identity.transaction_uuid = TransactionUuid(local_id);
  identity.scope = txn::TransactionScope::local_node;
  return identity;
}

txn::TransactionInventoryEntry InventoryEntry(platform::u64 local_id,
                                              txn::TransactionState state) {
  txn::TransactionInventoryEntry entry;
  entry.identity = TransactionIdentity(local_id);
  entry.state = state;
  entry.begin_unix_epoch_millis = kBaseMillis + local_id;
  entry.final_unix_epoch_millis = kBaseMillis + 1000 + local_id;
  entry.begin_visible_through_local_transaction_id =
      local_id == 1 ? 0 : local_id - 1;
  entry.evidence_record_required = true;
  entry.evidence_record_written = true;
  return entry;
}

txn::LocalTransactionInventory Inventory() {
  txn::LocalTransactionInventory inventory =
      txn::MakeEmptyLocalTransactionInventory();
  inventory.next_local_transaction_id = 4;
  inventory.entries.push_back(
      InventoryEntry(1, txn::TransactionState::committed));
  inventory.entries.push_back(
      InventoryEntry(2, txn::TransactionState::committed));
  inventory.entries.push_back(
      InventoryEntry(3, txn::TransactionState::rolled_back));
  return inventory;
}

txn::RowVersionMetadata Metadata(platform::TypedUuid row_uuid,
                                 platform::u64 creator_tx,
                                 platform::u64 version_sequence,
                                 txn::RowVersionState row_state,
                                 txn::TransactionState creator_state,
                                 platform::u64 successor_tx = 0,
                                 platform::u64 next_sequence = 0) {
  txn::RowVersionMetadata metadata;
  metadata.identity.row.row_uuid = row_uuid;
  metadata.identity.creator_transaction = TransactionIdentity(creator_tx);
  metadata.identity.version_sequence = version_sequence;
  metadata.state = row_state;
  metadata.creator_transaction_state = creator_state;
  metadata.payload_present = true;
  if (successor_tx != 0) {
    metadata.successor_transaction_local_id =
        txn::MakeLocalTransactionId(successor_tx);
  }
  if (next_sequence != 0) {
    metadata.chain.next_version_sequence = next_sequence;
    metadata.chain.next_version_uuid =
        MakeUuid(platform::UuidKind::row, 900 + next_sequence);
  }
  return metadata;
}

struct Fixture {
  platform::TypedUuid relation_uuid;
  platform::TypedUuid index_uuid;
  platform::TypedUuid allocation_uuid;
  platform::TypedUuid old_row_uuid;
  platform::TypedUuid current_row_uuid;
  platform::TypedUuid rolled_back_row_uuid;
  platform::TypedUuid current_version_uuid;
  std::vector<txn::RowVersionMetadata> row_versions;
};

Fixture MakeFixture() {
  Fixture fixture;
  fixture.relation_uuid = MakeUuid(platform::UuidKind::object, 10);
  fixture.index_uuid = MakeUuid(platform::UuidKind::object, 11);
  fixture.allocation_uuid = MakeUuid(platform::UuidKind::object, 12);
  fixture.old_row_uuid = MakeUuid(platform::UuidKind::row, 21);
  fixture.current_row_uuid = MakeUuid(platform::UuidKind::row, 22);
  fixture.rolled_back_row_uuid = MakeUuid(platform::UuidKind::row, 23);
  fixture.current_version_uuid = MakeUuid(platform::UuidKind::row, 24);
  fixture.row_versions.push_back(
      Metadata(fixture.old_row_uuid,
               1,
               1,
               txn::RowVersionState::committed,
               txn::TransactionState::committed,
               2,
               2));
  fixture.row_versions.push_back(
      Metadata(fixture.current_row_uuid,
               2,
               2,
               txn::RowVersionState::committed,
               txn::TransactionState::committed));
  fixture.row_versions.push_back(
      Metadata(fixture.rolled_back_row_uuid,
               3,
               3,
               txn::RowVersionState::rolled_back,
               txn::TransactionState::rolled_back));
  return fixture;
}

txn::LocalGarbageCollectionSweepResult AuthoritativeSweep(
    const Fixture& fixture) {
  txn::LocalCleanupWorksetRequest workset;
  workset.inventory = Inventory();
  workset.inventory_authoritative = true;
  workset.inventory_complete = true;
  workset.active_snapshot_inventory_authoritative = true;
  workset.row_versions = fixture.row_versions;
  workset.retain_row_versions_in_result = false;
  workset.emit_reclaim_evidence_records = true;
  workset.max_reclaim_evidence_records = 8;

  txn::LocalGarbageCollectionSweepRequest request;
  request.workset = workset;
  request.family = txn::LocalCleanupSweepFamily::explicit_request;
  request.engine_mga_authoritative = true;
  request.max_candidate_row_versions = 8;
  request.max_retained_row_versions = 0;
  request.retain_row_versions_in_result = false;
  return txn::RunLocalGarbageCollectionSweep(request);
}

page::RowDataRecord PageRow(const txn::RowVersionMetadata& metadata,
                            platform::u32 stable_slot_id) {
  page::RowDataRecord row;
  row.row_uuid = metadata.identity.row.row_uuid;
  row.transaction_uuid = metadata.identity.creator_transaction.transaction_uuid;
  row.local_transaction_id = metadata.identity.creator_transaction.local_id.value;
  row.stable_slot_id = stable_slot_id;
  row.row_version =
      static_cast<platform::u32>(metadata.identity.version_sequence);
  row.deleted = metadata.state == txn::RowVersionState::delete_marker ||
                metadata.state == txn::RowVersionState::rolled_back;
  return row;
}

page::RowDataPageBody RowPageBody(const Fixture& fixture) {
  page::RowDataPageBody body;
  body.relation_uuid = fixture.relation_uuid;
  body.segment_id = 1;
  body.segment_generation = 1;
  body.page_number = 5;
  body.page_generation = 7;
  body.compaction_generation = 7;
  body.rows.push_back(PageRow(fixture.row_versions[0], 11));
  body.rows.push_back(PageRow(fixture.row_versions[1], 12));
  body.rows.push_back(PageRow(fixture.row_versions[2], 13));
  const auto built = page::BuildRowDataPageBody(body, kRowPageSize);
  if (!built.ok()) {
    std::cerr << built.diagnostic.diagnostic_code << '\n';
  }
  Require(built.ok(), "ELER-024 row page fixture did not build");
  return built.body;
}

api::CrudRowVersionRecord CrudRow(const txn::RowVersionMetadata& metadata,
                                  const std::string& table_uuid,
                                  const std::string& version_uuid) {
  api::CrudRowVersionRecord row;
  row.creator_tx = metadata.identity.creator_transaction.local_id.value;
  row.event_sequence = metadata.identity.version_sequence;
  row.sequence = metadata.identity.version_sequence;
  row.table_uuid = table_uuid;
  row.row_uuid = uuid::UuidToString(metadata.identity.row.row_uuid.value);
  row.version_uuid = version_uuid;
  row.deleted = metadata.state == txn::RowVersionState::delete_marker ||
                metadata.state == txn::RowVersionState::rolled_back;
  row.values = {{"id", std::to_string(metadata.identity.version_sequence)}};
  return row;
}

api::CrudIndexEntryRecord IndexEntry(const api::CrudRowVersionRecord& row,
                                     const std::string& index_uuid) {
  api::CrudIndexEntryRecord entry;
  entry.creator_tx = row.creator_tx;
  entry.event_sequence = row.event_sequence;
  entry.sequence = row.sequence;
  entry.index_uuid = index_uuid;
  entry.table_uuid = row.table_uuid;
  entry.column_name = "id";
  entry.family = api::kCrudIndexFamilyBtree;
  entry.entry_kind = "exact";
  entry.key_value = std::to_string(row.sequence);
  entry.payload_value = entry.key_value;
  entry.row_uuid = row.row_uuid;
  entry.version_uuid = row.version_uuid;
  return entry;
}

api::MgaRelationStoreState RelationState(const Fixture& fixture) {
  api::MgaRelationStoreState state;
  const std::string table_uuid = uuid::UuidToString(fixture.relation_uuid.value);
  const std::string index_uuid = uuid::UuidToString(fixture.index_uuid.value);
  state.row_versions.push_back(
      CrudRow(fixture.row_versions[0], table_uuid, "version-old"));
  state.row_versions.push_back(
      CrudRow(fixture.row_versions[1], table_uuid, "version-current"));
  state.row_versions.push_back(
      CrudRow(fixture.row_versions[2], table_uuid, "version-rolled-back"));
  for (const auto& row : state.row_versions) {
    state.index_entries.push_back(IndexEntry(row, index_uuid));
  }
  state.max_row_event_sequence = 3;
  state.max_index_event_sequence = 3;
  return state;
}

idx::SecondaryIndexGarbageCleanupRequest SecondaryIndexCleanup(
    const Fixture& fixture) {
  idx::SecondaryIndexGarbageCleanupRequest request;
  request.index_uuid = fixture.index_uuid;
  request.table_uuid = fixture.relation_uuid;
  request.cleanup_horizon_authoritative = false;
  request.max_records_to_scan = 8;
  request.max_records_to_clean = 8;

  idx::SecondaryIndexDeltaLedgerRecord garbage;
  garbage.delta.delta_id = MakeUuid(platform::UuidKind::object, 30);
  garbage.delta.index_uuid = fixture.index_uuid;
  garbage.delta.table_uuid = fixture.relation_uuid;
  garbage.delta.row_uuid = fixture.old_row_uuid;
  garbage.delta.version_uuid = MakeUuid(platform::UuidKind::row, 31);
  garbage.delta.transaction_uuid = TransactionUuid(1);
  garbage.delta.local_transaction_id = 1;
  garbage.delta.delta_kind = idx::SecondaryIndexDeltaKind::insert;
  garbage.delta.key_payload = "old-row-key";
  garbage.delta.cleanup_horizon_token = "durable_mga_cleanup_horizon";
  garbage.delta.committed = true;
  garbage.commit_state = idx::SecondaryIndexDeltaLedgerCommitState::merged_cleaned;
  garbage.source_evidence_reference = "ELER-024 merged-cleaned index delta";
  request.ledger.records.push_back(garbage);

  idx::SecondaryIndexBaseEntry current_base;
  current_base.index_uuid = fixture.index_uuid;
  current_base.table_uuid = fixture.relation_uuid;
  current_base.row_uuid = fixture.current_row_uuid;
  current_base.version_uuid = fixture.current_version_uuid;
  current_base.key_payload = "current-row-key";
  current_base.committed_local_transaction_id = 2;
  request.base_entries.push_back(current_base);

  idx::SecondaryIndexTableSnapshotEntry current_snapshot;
  current_snapshot.index_uuid = fixture.index_uuid;
  current_snapshot.table_uuid = fixture.relation_uuid;
  current_snapshot.row_uuid = fixture.current_row_uuid;
  current_snapshot.version_uuid = fixture.current_version_uuid;
  current_snapshot.key_payload = current_base.key_payload;
  request.table_snapshot.push_back(current_snapshot);
  return request;
}

std::vector<platform::byte> Payload(std::size_t bytes) {
  std::vector<platform::byte> payload;
  payload.reserve(bytes);
  for (std::size_t index = 0; index < bytes; ++index) {
    payload.push_back(static_cast<platform::byte>((index * 17 + 3) & 0xffu));
  }
  return payload;
}

struct OverflowFixture {
  page::OverflowLedger ledger;
  page::OverflowValueRecord record;
};

OverflowFixture OverflowValue() {
  OverflowFixture fixture;
  page::OverflowPersistRequest persist;
  persist.row_uuid = MakeUuid(platform::UuidKind::row, 40);
  persist.object_uuid = MakeUuid(platform::UuidKind::object, 41);
  persist.transaction_uuid = TransactionUuid(1);
  persist.chunk_policy_uuid = MakeUuid(platform::UuidKind::object, 42);
  persist.local_transaction_id = 1;
  persist.generation = 1;
  persist.value_descriptor = "ELER-024 integrated overflow cleanup";
  persist.payload_bytes = Payload(6000);
  persist.chunk_size = 1200;
  const auto persisted = page::PersistOverflowValue(&fixture.ledger, persist);
  Require(persisted.ok(), "ELER-024 overflow persist failed");

  page::OverflowCommitRequest commit;
  commit.overflow_value_uuid = persisted.overflow_value_uuid;
  commit.transaction_uuid = persist.transaction_uuid;
  commit.local_transaction_id = persist.local_transaction_id;
  commit.reason = "ELER-024 integrated cleanup fixture";
  const auto committed = page::CommitOverflowValue(&fixture.ledger, commit);
  Require(committed.ok(), "ELER-024 overflow commit failed");
  fixture.record = committed.record;
  return fixture;
}

page::OverflowValueRecord WriteOverflowBlobPages(
    disk::FileDevice* device,
    const std::filesystem::path& path,
    platform::TypedUuid database_uuid,
    platform::TypedUuid filespace_uuid,
    const page::OverflowValueRecord& record) {
  RemoveDeviceArtifacts(path);
  Require(device->Open(path.string(), disk::FileOpenMode::create_new).ok(),
          "ELER-024 overflow device create failed");
  page::OverflowBlobPageWriteRequest write;
  write.device = device;
  write.database_uuid = database_uuid;
  write.filespace_uuid = filespace_uuid;
  write.record = record;
  write.page_size = kBlobPageSize;
  write.first_page_number = kFirstBlobPage;
  write.page_generation = 9;
  const auto written = page::WriteOverflowValueBlobPages(write);
  Require(written.ok(), "ELER-024 overflow blob page write failed");
  Require(device->Close().ok(), "ELER-024 overflow close after write failed");
  return written.record;
}

txn::CurrentRowMapRebuildRequest CurrentRowMapRequest(const Fixture& fixture) {
  txn::CurrentRowMapRebuildRequest request;
  request.relation_uuid = uuid::UuidToString(fixture.relation_uuid.value);
  request.relation_epoch = 1;
  request.catalog_epoch = 1;
  request.security_epoch = 1;
  request.redaction_epoch = 1;
  request.map_generation = 2;
  request.invalidation_generation = 2;
  request.authoritative_base_rows_proof = true;
  request.durable_mga_inventory_proof = true;
  request.map_self_authoritative = false;
  txn::CurrentRowAuthoritativeBaseRow row;
  row.row_uuid = uuid::UuidToString(fixture.current_row_uuid.value);
  row.version_uuid = uuid::UuidToString(fixture.current_version_uuid.value);
  row.row_generation = 2;
  row.visible_through_local_transaction_id = txn::MakeLocalTransactionId(2);
  row.visible = true;
  row.deleted = false;
  request.base_rows.push_back(row);
  return request;
}

void PageFinality(const Fixture& fixture,
                  txn::PageFinalityMapEntry* entry,
                  txn::PageFinalityObservedFacts* observed) {
  entry->scope = txn::PageFinalityScope::extent;
  entry->status = txn::PageFinalityMapStatus::current;
  entry->provenance = txn::PageFinalityProvenance::engine_mga_cleanup_horizon;
  entry->relation_uuid = uuid::UuidToString(fixture.relation_uuid.value);
  entry->page_number = 5;
  entry->page_generation = 7;
  entry->extent_id = 1;
  entry->extent_epoch = 1;
  entry->relation_epoch = 1;
  entry->catalog_epoch = 1;
  entry->final_through_local_transaction_id = txn::MakeLocalTransactionId(2);
  entry->map_generation = 2;
  entry->persisted_record_present = true;
  entry->checksum_valid = true;
  entry->all_visible = true;
  entry->all_final = true;

  observed->requested_scope = txn::PageFinalityScope::extent;
  observed->relation_uuid = entry->relation_uuid;
  observed->page_number = entry->page_number;
  observed->page_generation = entry->page_generation;
  observed->extent_id = entry->extent_id;
  observed->extent_epoch = entry->extent_epoch;
  observed->relation_epoch = entry->relation_epoch;
  observed->catalog_epoch = entry->catalog_epoch;
  observed->reader_visible_through_local_transaction_id =
      txn::MakeLocalTransactionId(4);
  observed->oldest_active_local_transaction_id = txn::MakeLocalTransactionId(5);
  observed->transaction_horizon_authoritative = true;
  observed->transaction_inventory_authoritative = true;
  observed->normal_mga_visibility_authority_available = true;
}

page::PageAllocationLedger AllocationLedger(const Fixture& fixture) {
  page::PageAllocationLedger ledger;
  ledger.database_uuid = MakeUuid(platform::UuidKind::database, 50);
  ledger.filespace_uuid = MakeUuid(platform::UuidKind::filespace, 51);
  page::PageAllocationEntry allocation;
  allocation.allocation_uuid = fixture.allocation_uuid;
  allocation.database_uuid = ledger.database_uuid;
  allocation.filespace_uuid = ledger.filespace_uuid;
  allocation.owner_object_uuid = fixture.relation_uuid;
  allocation.creator_transaction_uuid = TransactionUuid(1);
  allocation.creator_local_transaction_id = 1;
  allocation.start_page = 20;
  allocation.page_count = 2;
  allocation.durable_page_generation = 3;
  allocation.published_page_generation = 3;
  allocation.page_family = "data";
  allocation.filespace_class = "user_data";
  allocation.filespace_class_reason = "ELER-024 cleanup fixture";
  allocation.state = page::PageAllocationLifecycleState::reusable_pending_mga;
  allocation.reusable_after_local_transaction_id = 1;
  allocation.durability_fence_satisfied = true;
  ledger.allocations.push_back(allocation);
  return ledger;
}

page::AllocationMapExtent AllocationExtent(
    const Fixture& fixture,
    platform::u64 start_page,
    platform::u64 page_count,
    page::PageAllocationLifecycleState state) {
  page::AllocationMapExtent extent;
  extent.start_page = start_page;
  extent.page_count = page_count;
  extent.state = state;
  extent.page_type = state == page::PageAllocationLifecycleState::free
                         ? disk::PageType::unknown
                         : disk::PageType::row_data;
  extent.page_family = state == page::PageAllocationLifecycleState::free
                           ? page::PageFamily::unknown
                           : page::PageFamily::data;
  extent.page_generation = state == page::PageAllocationLifecycleState::free ? 0 : 3;
  if (state != page::PageAllocationLifecycleState::free) {
    extent.allocation_uuid = fixture.allocation_uuid;
    extent.owner_object_uuid = fixture.relation_uuid;
    extent.creator_transaction_uuid = TransactionUuid(1);
  }
  if (state == page::PageAllocationLifecycleState::reusable_pending_mga) {
    extent.reusable_after_local_transaction_id = 1;
  }
  return extent;
}

page::AllocationMapPageBody AllocationMap(const Fixture& fixture) {
  page::AllocationMapPageBody body;
  body.database_uuid = MakeUuid(platform::UuidKind::database, 60);
  body.filespace_uuid = MakeUuid(platform::UuidKind::filespace, 61);
  body.file_member_uuid = MakeUuid(platform::UuidKind::object, 62);
  body.allocation_map_page_number = 2;
  body.map_generation = 1;
  body.capacity_generation = 1;
  body.page_size_bytes = kAllocationMapPageSize;
  body.filespace_start_page = 1;
  body.total_pages = 32;
  body.extents.push_back(AllocationExtent(
      fixture, 1, 19, page::PageAllocationLifecycleState::allocated));
  body.extents.push_back(AllocationExtent(
      fixture, 20, 2, page::PageAllocationLifecycleState::reusable_pending_mga));
  body.extents.push_back(AllocationExtent(
      fixture, 22, 11, page::PageAllocationLifecycleState::free));
  return body;
}

api::MgaIntegratedPhysicalCleanupRequest IntegratedRequest(
    const Fixture& fixture,
    const txn::LocalGarbageCollectionSweepResult& sweep,
    const page::OverflowLedger& overflow_ledger,
    const page::OverflowValueRecord& overflow_record,
    disk::FileDevice* blob_device,
    platform::TypedUuid database_uuid,
    platform::TypedUuid filespace_uuid) {
  api::MgaIntegratedPhysicalCleanupRequest request;
  request.sweep = sweep;
  request.engine_mga_authoritative = true;
  request.row_page = RowPageBody(fixture);
  request.row_page_size = kRowPageSize;
  request.max_reclaim_rows = 8;
  request.relation_state = RelationState(fixture);
  request.max_row_versions_to_scan = 8;
  request.max_index_entries_to_scan = 8;
  request.secondary_index_cleanup = SecondaryIndexCleanup(fixture);
  request.overflow_ledger = overflow_ledger;
  page::OverflowBlobPageReclaimRequest reclaim;
  reclaim.device = blob_device;
  reclaim.database_uuid = database_uuid;
  reclaim.filespace_uuid = filespace_uuid;
  reclaim.record = overflow_record;
  reclaim.page_size = kBlobPageSize;
  request.overflow_blob_reclaims.push_back(reclaim);
  request.current_row_map_rebuild = CurrentRowMapRequest(fixture);
  PageFinality(fixture,
               &request.page_finality_entry,
               &request.page_finality_observed);
  request.allocation_ledger = AllocationLedger(fixture);
  request.allocation_reclaim_uuids.push_back(fixture.allocation_uuid);
  request.allocation_map = AllocationMap(fixture);
  request.allocation_map_page_size = kAllocationMapPageSize;
  return request;
}

bool HasVersion(const std::vector<api::CrudRowVersionRecord>& rows,
                std::string_view version_uuid) {
  for (const auto& row : rows) {
    if (row.version_uuid == version_uuid) {
      return true;
    }
  }
  return false;
}

void ProveIntegratedCleanup() {
  const auto fixture = MakeFixture();
  const auto sweep = AuthoritativeSweep(fixture);
  if (!sweep.ok()) {
    std::cerr << sweep.diagnostic.diagnostic_code << '\n';
  }
  Require(sweep.ok(), "ELER-024 authoritative sweep failed");
  Require(sweep.cleanup.reclaimed_row_version_count == 2,
          "ELER-024 cleanup decision did not identify row garbage");
  Require(sweep.cleanup.reclaim_evidence_records.size() == 2,
          "ELER-024 cleanup decision did not emit reclaim evidence");

  auto overflow = OverflowValue();
  const auto database_uuid = MakeUuid(platform::UuidKind::database, 70);
  const auto filespace_uuid = MakeUuid(platform::UuidKind::filespace, 71);
  const auto path = TempRoot() / "integrated_overflow_pages.sbdb";
  disk::FileDevice writer;
  overflow.record = WriteOverflowBlobPages(
      &writer, path, database_uuid, filespace_uuid, overflow.record);

  disk::FileDevice reclaim_device;
  Require(reclaim_device.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
          "ELER-024 overflow device reopen for reclaim failed");
  const auto request = IntegratedRequest(fixture,
                                         sweep,
                                         overflow.ledger,
                                         overflow.record,
                                         &reclaim_device,
                                         database_uuid,
                                         filespace_uuid);
  const auto result = api::ApplyMgaIntegratedPhysicalCleanup(request);
  if (!result.ok) {
    std::cerr << result.diagnostic.code << ':' << result.diagnostic.detail
              << '\n';
  }
  Require(result.ok, "ELER-024 integrated physical cleanup failed");
  Require(result.cleanup_horizon_authoritative,
          "ELER-024 cleanup horizon was not authoritative");
  Require(result.authoritative_cleanup_horizon_local_transaction_id ==
              sweep.cleanup.authoritative_cleanup_horizon_local_transaction_id,
          "ELER-024 surfaces did not use the sweep cleanup horizon");
  Require(result.row_page_mutated && result.row_page_sweep.removed_row_count == 2,
          "ELER-024 row page was not physically reclaimed");
  Require(result.relation_state_mutated &&
              result.relation_sweep.removed_row_version_count == 2 &&
              result.relation_sweep.removed_index_entry_count == 2,
          "ELER-024 relation row/index state was not pruned");
  Require(HasVersion(result.relation_sweep.state.row_versions, "version-current"),
          "ELER-024 current relation version was not retained");
  Require(!HasVersion(result.relation_sweep.state.row_versions, "version-old"),
          "ELER-024 old relation version was not reclaimed");
  Require(result.secondary_index_cleaned &&
              result.secondary_index_cleanup.validation_before_ok &&
              result.secondary_index_cleanup.validation_after_ok &&
              result.secondary_index_cleanup.before.base_index_entries == 1 &&
              result.secondary_index_cleanup.before.table_snapshot_entries == 1 &&
              result.secondary_index_cleanup.after.cleaned_garbage_records == 1,
          "ELER-024 secondary-index garbage was not cleaned");
  Require(result.overflow_cleaned &&
              result.overflow_cleanup.cleaned_count == 1,
          "ELER-024 overflow logical cleanup did not run");
  Require(result.overflow_pages_reclaimed &&
              result.overflow_blob_reclaims.size() == 1 &&
              result.overflow_blob_reclaims.front().record.state ==
                  page::OverflowValueState::cleanup_reclaimed,
          "ELER-024 overflow blob pages were not reclaimed");
  Require(result.current_row_map_rebuilt &&
              result.current_row_map_rebuild.rebuilt_entry_count == 1,
          "ELER-024 current-row map was not rebuilt");
  Require(result.page_finality_accepted &&
              result.exact_index_cleanup_authority.accepted,
          "ELER-024 page-finality cleanup evidence was not accepted");
  Require(result.allocation_reclaimed &&
              result.allocation_reclaims.size() == 1 &&
              result.allocation_reclaims.front().allocation.state ==
                  page::PageAllocationLifecycleState::reusable_free,
          "ELER-024 page allocation was not reclaimed");
  Require(result.allocation_map_mutated &&
              result.allocation_map.validation.counts.reusable_pending_mga_pages == 0,
          "ELER-024 allocation map extent was not mutated");
  Require(reclaim_device.Close().ok(),
          "ELER-024 overflow device close after reclaim failed");
  RemoveDeviceArtifacts(path);
}

void ProveMissingAuthorityFailsClosed() {
  const auto fixture = MakeFixture();
  auto sweep = AuthoritativeSweep(fixture);
  Require(sweep.ok(), "ELER-024 authoritative sweep setup failed");
  sweep.cleanup.cleanup_horizon_authoritative = false;

  auto overflow = OverflowValue();
  const auto database_uuid = MakeUuid(platform::UuidKind::database, 80);
  const auto filespace_uuid = MakeUuid(platform::UuidKind::filespace, 81);
  const auto path = TempRoot() / "integrated_overflow_refused.sbdb";
  disk::FileDevice writer;
  overflow.record = WriteOverflowBlobPages(
      &writer, path, database_uuid, filespace_uuid, overflow.record);

  disk::FileDevice reclaim_device;
  Require(reclaim_device.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
          "ELER-024 overflow device reopen for refused reclaim failed");
  const auto request = IntegratedRequest(fixture,
                                         sweep,
                                         overflow.ledger,
                                         overflow.record,
                                         &reclaim_device,
                                         database_uuid,
                                         filespace_uuid);
  const auto refused = api::ApplyMgaIntegratedPhysicalCleanup(request);
  Require(!refused.ok && refused.fail_closed,
          "ELER-024 non-authoritative cleanup did not fail closed");
  Require(refused.diagnostic.detail.find(
              "authoritative_mga_cleanup_sweep_required") !=
              std::string::npos,
          "ELER-024 non-authoritative refusal was not stable");
  Require(!refused.row_page_sweep.physical_storage_mutated &&
              !refused.relation_sweep.physical_state_mutated &&
              !refused.secondary_index_cleaned &&
              !refused.overflow_cleaned &&
              !refused.overflow_pages_reclaimed &&
              !refused.current_row_map_rebuilt &&
              !refused.page_finality_accepted &&
              !refused.allocation_reclaimed &&
              !refused.allocation_map_mutated,
          "ELER-024 refused cleanup mutated an evidence surface");
  Require(reclaim_device.Close().ok(),
          "ELER-024 overflow device close after refused reclaim failed");
  RemoveDeviceArtifacts(path);
}

void ProveCurrentRowProofIsNotInvented() {
  const auto fixture = MakeFixture();
  const auto sweep = AuthoritativeSweep(fixture);
  Require(sweep.ok(), "ELER-024 authoritative sweep setup failed");

  auto overflow = OverflowValue();
  const auto database_uuid = MakeUuid(platform::UuidKind::database, 90);
  const auto filespace_uuid = MakeUuid(platform::UuidKind::filespace, 91);
  const auto path = TempRoot() / "integrated_current_row_refused.sbdb";
  disk::FileDevice writer;
  overflow.record = WriteOverflowBlobPages(
      &writer, path, database_uuid, filespace_uuid, overflow.record);

  disk::FileDevice reclaim_device;
  Require(reclaim_device.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
          "ELER-024 overflow device reopen for current-row refusal failed");
  auto request = IntegratedRequest(fixture,
                                   sweep,
                                   overflow.ledger,
                                   overflow.record,
                                   &reclaim_device,
                                   database_uuid,
                                   filespace_uuid);
  request.current_row_map_rebuild.authoritative_base_rows_proof = false;
  const auto refused = api::ApplyMgaIntegratedPhysicalCleanup(request);
  Require(!refused.ok && refused.fail_closed,
          "ELER-024 missing current-row proof did not fail closed");
  Require(refused.diagnostic.detail.find(
              "current_row_map_authoritative_base_rows_required") !=
              std::string::npos,
          "ELER-024 current-row proof refusal was not stable");
  page::OverflowBlobPageReadRequest read;
  read.device = &reclaim_device;
  read.database_uuid = database_uuid;
  read.filespace_uuid = filespace_uuid;
  read.record = overflow.record;
  read.page_size = kBlobPageSize;
  const auto still_readable = page::ReadOverflowValueBlobPages(read);
  Require(still_readable.ok(),
          "ELER-024 current-row proof refusal reclaimed blob pages");
  Require(reclaim_device.Close().ok(),
          "ELER-024 overflow device close after current-row refusal failed");
  RemoveDeviceArtifacts(path);
}

}  // namespace

int main() {
  ProveIntegratedCleanup();
  ProveMissingAuthorityFailsClosed();
  ProveCurrentRowProofIsNotInvented();
  RemoveTempRootIfEmpty();
  std::cout << "engine_listener_mga_integrated_physical_cleanup_conformance=passed\n";
  return EXIT_SUCCESS;
}
