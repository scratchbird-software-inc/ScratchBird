// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_store.hpp"
#include "row_data_page.hpp"
#include "row_data_physical_sweep.hpp"
#include "transaction_cleanup.hpp"
#include "uuid.hpp"

#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace page = scratchbird::storage::page;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1770400000000ull;
constexpr scratchbird::core::platform::u32 kPageSize = 16384;

bool Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

TypedUuid TransactionUuid(u64 local_id) {
  static std::map<u64, TypedUuid> transaction_uuids;
  const auto found = transaction_uuids.find(local_id);
  if (found != transaction_uuids.end()) {
    return found->second;
  }
  auto inserted = transaction_uuids.emplace(
      local_id, MakeUuid(UuidKind::transaction, 100 + local_id));
  return inserted.first->second;
}

txn::TransactionIdentity TransactionIdentity(u64 local_id) {
  txn::TransactionIdentity identity;
  identity.local_id = txn::MakeLocalTransactionId(local_id);
  identity.transaction_uuid = TransactionUuid(local_id);
  identity.scope = txn::TransactionScope::local_node;
  return identity;
}

txn::TransactionInventoryEntry InventoryEntry(u64 local_id,
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
  txn::LocalTransactionInventory inventory = txn::MakeEmptyLocalTransactionInventory();
  inventory.next_local_transaction_id = 4;
  inventory.entries.push_back(InventoryEntry(1, txn::TransactionState::committed));
  inventory.entries.push_back(InventoryEntry(2, txn::TransactionState::committed));
  inventory.entries.push_back(InventoryEntry(3, txn::TransactionState::rolled_back));
  return inventory;
}

txn::RowVersionMetadata Metadata(TypedUuid row_uuid,
                                 u64 creator_tx,
                                 u64 version_sequence,
                                 txn::RowVersionState row_state,
                                 txn::TransactionState creator_state,
                                 u64 successor_tx = 0,
                                 u64 next_sequence = 0) {
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
    metadata.chain.next_version_uuid = MakeUuid(UuidKind::row, 900 + next_sequence);
  }
  return metadata;
}

struct Fixture {
  TypedUuid relation_uuid;
  TypedUuid old_row_uuid;
  TypedUuid current_row_uuid;
  TypedUuid rolled_back_row_uuid;
  std::vector<txn::RowVersionMetadata> row_versions;
};

Fixture MakeFixture() {
  Fixture fixture;
  fixture.relation_uuid = MakeUuid(UuidKind::object, 10);
  fixture.old_row_uuid = MakeUuid(UuidKind::row, 11);
  fixture.current_row_uuid = MakeUuid(UuidKind::row, 12);
  fixture.rolled_back_row_uuid = MakeUuid(UuidKind::row, 13);
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
                            scratchbird::core::platform::u32 stable_slot_id) {
  page::RowDataRecord row;
  row.row_uuid = metadata.identity.row.row_uuid;
  row.transaction_uuid =
      metadata.identity.creator_transaction.transaction_uuid;
  row.local_transaction_id =
      metadata.identity.creator_transaction.local_id.value;
  row.stable_slot_id = stable_slot_id;
  row.row_version =
      static_cast<scratchbird::core::platform::u32>(
          metadata.identity.version_sequence);
  row.deleted = metadata.state == txn::RowVersionState::delete_marker ||
                metadata.state == txn::RowVersionState::rolled_back;
  return row;
}

page::RowDataPageBody PageBody(const Fixture& fixture) {
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
  return body;
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
  row.previous_sequence = metadata.chain.previous_version_sequence;
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
  const std::string index_uuid = uuid::UuidToString(MakeUuid(UuidKind::object, 20).value);
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

bool HasVersion(const std::vector<api::CrudRowVersionRecord>& rows,
                std::string_view version_uuid) {
  for (const auto& row : rows) {
    if (row.version_uuid == version_uuid) {
      return true;
    }
  }
  return false;
}

bool HasIndexVersion(const std::vector<api::CrudIndexEntryRecord>& entries,
                     std::string_view version_uuid) {
  for (const auto& entry : entries) {
    if (entry.version_uuid == version_uuid) {
      return true;
    }
  }
  return false;
}

bool PhysicalPageSweepProof() {
  bool ok = true;
  const auto fixture = MakeFixture();
  const auto sweep = AuthoritativeSweep(fixture);
  if (!sweep.ok()) {
    std::cerr << "PCR-083 sweep diagnostic: "
              << sweep.diagnostic.diagnostic_code << ':'
              << sweep.diagnostic.message_key << ':'
              << sweep.diagnostic.remediation_hint << '\n';
  }
  ok = Expect(sweep.ok(), "PCR-083 authoritative sweep should succeed") && ok;
  ok = Expect(sweep.cleanup.reclaimed_row_version_count == 2,
              "PCR-083 sweep should identify two reclaimable row versions") &&
       ok;
  ok = Expect(sweep.cleanup.reclaim_evidence_records.size() == 2,
              "PCR-083 sweep should emit exact reclaim evidence") &&
       ok;
  ok = Expect(!sweep.cleanup.physical_storage_mutated,
              "PCR-083 cleanup decision stage must not mutate storage") &&
       ok;

  const auto built = page::BuildRowDataPageBody(PageBody(fixture), kPageSize);
  ok = Expect(built.ok(), "PCR-083 row page fixture should build") && ok;
  if (!ok) {
    return false;
  }

  page::RowDataPhysicalSweepRequest request;
  request.page = built.body;
  request.sweep = sweep;
  request.page_size = kPageSize;
  request.engine_mga_authoritative = true;
  request.max_reclaim_rows = 8;
  const auto reclaimed = page::ApplyRowDataPhysicalSweep(request);
  ok = Expect(reclaimed.ok(), "PCR-083 row page physical sweep should apply") && ok;
  ok = Expect(reclaimed.physical_storage_mutated,
              "PCR-083 row page physical sweep should mutate page body") &&
       ok;
  ok = Expect(reclaimed.removed_row_count == 2,
              "PCR-083 row page should remove two versions") &&
       ok;
  ok = Expect(reclaimed.retained_row_count == 1,
              "PCR-083 row page should retain current version") &&
       ok;
  ok = Expect(reclaimed.free_space_after > reclaimed.free_space_before,
              "PCR-083 row page free space should increase after compaction") &&
       ok;
  const auto parsed = page::ParseRowDataPageBody(reclaimed.serialized,
                                                 reclaimed.page.page_number);
  ok = Expect(parsed.ok(), "PCR-083 compacted row page should parse") && ok;
  ok = Expect(parsed.body.rows.size() == 1,
              "PCR-083 compacted row page should contain one row") &&
       ok;
  if (parsed.ok() && parsed.body.rows.size() == 1) {
    ok = Expect(parsed.body.rows.front().stable_slot_id == 12,
                "PCR-083 compacted row should preserve stable slot id") &&
         ok;
    ok = Expect(parsed.body.rows.front().row_version == 2,
                "PCR-083 compacted row should retain current version") &&
         ok;
  }

  page::RowDataPhysicalSweepRequest unauth = request;
  unauth.engine_mga_authoritative = false;
  const auto refused = page::ApplyRowDataPhysicalSweep(unauth);
  ok = Expect(!refused.ok(),
              "PCR-083 row page sweep should fail without MGA authority") &&
       ok;
  ok = Expect(refused.diagnostic.diagnostic_code ==
                  "SB-ROW-DATA-PHYSICAL-SWEEP-MGA-AUTHORITY-REQUIRED",
              "PCR-083 row page authority refusal should be stable") &&
       ok;
  return ok;
}

bool RelationIndexSweepProof() {
  bool ok = true;
  const auto fixture = MakeFixture();
  const auto sweep = AuthoritativeSweep(fixture);
  if (!sweep.ok()) {
    std::cerr << "PCR-083 relation sweep diagnostic: "
              << sweep.diagnostic.diagnostic_code << ':'
              << sweep.diagnostic.message_key << ':'
              << sweep.diagnostic.remediation_hint << '\n';
  }
  api::MgaRelationPhysicalSweepRequest request;
  request.state = RelationState(fixture);
  request.reclaim_evidence_records = sweep.cleanup.reclaim_evidence_records;
  request.engine_mga_authoritative = true;
  request.cleanup_horizon_authoritative =
      sweep.cleanup.cleanup_horizon_authoritative;
  request.authoritative_cleanup_horizon_local_transaction_id =
      sweep.cleanup.authoritative_cleanup_horizon_local_transaction_id;
  request.max_row_versions_to_scan = 8;
  request.max_index_entries_to_scan = 8;
  const auto result = api::ApplyMgaRelationPhysicalSweepToState(request);
  ok = Expect(result.ok, "PCR-083 relation physical sweep should apply") && ok;
  ok = Expect(result.physical_state_mutated,
              "PCR-083 relation physical sweep should mutate state") &&
       ok;
  ok = Expect(result.removed_row_version_count == 2,
              "PCR-083 relation sweep should remove two row versions") &&
       ok;
  ok = Expect(result.removed_index_entry_count == 2,
              "PCR-083 relation sweep should remove matching index entries") &&
       ok;
  ok = Expect(result.retained_row_version_count == 1,
              "PCR-083 relation sweep should retain current row version") &&
       ok;
  ok = Expect(HasVersion(result.state.row_versions, "version-current"),
              "PCR-083 relation sweep should retain current version") &&
       ok;
  ok = Expect(!HasVersion(result.state.row_versions, "version-old"),
              "PCR-083 relation sweep should remove obsolete version") &&
       ok;
  ok = Expect(!HasVersion(result.state.row_versions, "version-rolled-back"),
              "PCR-083 relation sweep should remove rolled-back version") &&
       ok;
  ok = Expect(HasIndexVersion(result.state.index_entries, "version-current"),
              "PCR-083 relation sweep should retain current index entry") &&
       ok;
  ok = Expect(!HasIndexVersion(result.state.index_entries, "version-old"),
              "PCR-083 relation sweep should remove obsolete index entry") &&
       ok;
  ok = Expect(!HasIndexVersion(result.state.index_entries, "version-rolled-back"),
              "PCR-083 relation sweep should remove rolled-back index entry") &&
       ok;

  api::MgaRelationPhysicalSweepRequest unauth = request;
  unauth.engine_mga_authoritative = false;
  const auto refused = api::ApplyMgaRelationPhysicalSweepToState(unauth);
  ok = Expect(!refused.ok && refused.fail_closed,
              "PCR-083 relation sweep should fail closed without authority") &&
       ok;
  ok = Expect(refused.diagnostic.detail.find("engine_mga_authority_required") !=
                  std::string::npos,
              "PCR-083 relation authority refusal should be exact") &&
       ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = PhysicalPageSweepProof() && ok;
  ok = RelationIndexSweepProof() && ok;
  return ok ? 0 : 1;
}
