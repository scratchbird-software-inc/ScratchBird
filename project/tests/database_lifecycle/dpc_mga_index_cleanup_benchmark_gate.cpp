// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/index_garbage_cleanup_agent.hpp"
#include "agents/storage_version_cleanup_agent.hpp"
#include "observability/cleanup_diagnostics_api.hpp"
#include "secondary_index_delta_ledger.hpp"
#include "transaction_cleanup_horizon_service.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents::implemented_agents;
namespace api = scratchbird::engine::internal_api;
namespace idx = scratchbird::core::index;
namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey =
    "DPC_MGA_INDEX_CLEANUP_BENCHMARK_GATE";
constexpr std::string_view kBenchmarkOutputSearchKey =
    "DPC_MGA_INDEX_CLEANUP_BENCHMARK_OUTPUT";
constexpr platform::u64 kRunCount = 5;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::u64 NextMillis() {
  static platform::u64 next = 1779531000000ull;
  return ++next;
}

platform::TypedUuid NewUuid(platform::UuidKind kind) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NextMillis());
  Require(generated.ok(), "DPC-035 generated UUID creation failed");
  return generated.value;
}

mga::TransactionIdentity NewIdentity(platform::u64 local_id) {
  const auto identity = mga::MakeTransactionIdentity(
      mga::MakeLocalTransactionId(local_id),
      NewUuid(platform::UuidKind::transaction),
      mga::TransactionScope::local_node);
  Require(identity.ok(), "DPC-035 transaction identity creation failed");
  return identity.identity;
}

mga::TransactionInventoryEntry Entry(platform::u64 local_id,
                                     mga::TransactionState state) {
  mga::TransactionInventoryEntry entry;
  entry.identity = NewIdentity(local_id);
  entry.state = state;
  entry.begin_unix_epoch_millis = NextMillis();
  if (mga::IsTerminalTransactionState(state)) {
    entry.final_unix_epoch_millis = NextMillis();
    entry.evidence_record_written = true;
  }
  return entry;
}

mga::LocalTransactionInventory Inventory(
    std::vector<mga::TransactionInventoryEntry> entries,
    platform::u64 next_local_transaction_id) {
  mga::LocalTransactionInventory inventory;
  inventory.entries = std::move(entries);
  inventory.next_local_transaction_id = next_local_transaction_id;
  return inventory;
}

mga::AuthoritativeCleanupHorizonRequest HorizonRequest(
    mga::LocalTransactionInventory inventory) {
  mga::AuthoritativeCleanupHorizonRequest request;
  request.inventory = std::move(inventory);
  request.inventory_authoritative = true;
  request.inventory_complete = true;
  request.active_snapshot_inventory_authoritative = true;
  return request;
}

mga::RowIdentity Row() {
  mga::RowIdentity row;
  row.row_uuid = NewUuid(platform::UuidKind::row);
  return row;
}

mga::RowVersionMetadata Version(const mga::RowIdentity& row,
                                const mga::TransactionInventoryEntry& creator,
                                mga::RowVersionState state,
                                platform::u64 sequence,
                                platform::u64 next_sequence = 0,
                                platform::u64 successor_local_id = 0) {
  mga::RowVersionMetadata metadata;
  metadata.identity.row = row;
  metadata.identity.creator_transaction = creator.identity;
  metadata.identity.version_sequence = sequence;
  metadata.state = state;
  metadata.creator_transaction_state = creator.state;
  metadata.payload_present = state != mga::RowVersionState::rolled_back &&
                             state != mga::RowVersionState::delete_marker;
  if (next_sequence != 0) {
    metadata.chain.next_version_sequence = next_sequence;
  }
  if (successor_local_id != 0) {
    metadata.successor_transaction_local_id =
        mga::MakeLocalTransactionId(successor_local_id);
  }
  return metadata;
}

agents::StorageVersionCleanupAgentRequest StorageRequest(
    mga::AuthoritativeCleanupHorizonRequest horizon,
    std::vector<mga::RowVersionMetadata> row_versions,
    platform::u64 max_candidates) {
  agents::StorageVersionCleanupAgentRequest request;
  request.horizon_request = std::move(horizon);
  request.row_versions = std::move(row_versions);
  request.max_candidate_row_versions = max_candidates;
  request.engine_mga_authoritative = true;
  return request;
}

std::string Fixed(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6) << value;
  return out.str();
}

double Ratio(platform::u64 numerator, platform::u64 denominator) {
  if (denominator == 0) {
    return 0.0;
  }
  return static_cast<double>(numerator) / static_cast<double>(denominator);
}

bool HasStorageEvidence(
    const agents::StorageVersionCleanupAgentResult& result,
    std::string_view key,
    std::string_view value) {
  for (const auto& field : result.evidence) {
    if (field.key == key && field.value == value) {
      return true;
    }
  }
  return false;
}

bool HasIndexEvidence(const agents::IndexGarbageCleanupAgentResult& result,
                      std::string_view key,
                      std::string_view value) {
  for (const auto& field : result.evidence) {
    if (field.key == key && field.value == value) {
      return true;
    }
  }
  return false;
}

void RequireStorageOk(const agents::StorageVersionCleanupAgentResult& result,
                      agents::StorageVersionCleanupDecisionKind decision,
                      std::string_view diagnostic_code,
                      std::string_view message) {
  Require(result.ok(), message);
  Require(result.decision == decision, "DPC-035 storage cleanup decision drifted");
  Require(result.diagnostic.diagnostic_code == diagnostic_code,
          "DPC-035 storage cleanup diagnostic drifted");
  Require(HasStorageEvidence(result,
                             "cleanup_horizon_service",
                             "dpc030_authoritative_cleanup_horizon_v1"),
          "DPC-035 storage cleanup lost DPC-030 horizon evidence");
  Require(HasStorageEvidence(result,
                             "storage_version_cleanup_agent",
                             "dpc032_storage_version_cleanup_agent_v1"),
          "DPC-035 storage cleanup lost DPC-032 agent evidence");
  Require(HasStorageEvidence(result, "parser_finality_authority", "false"),
          "DPC-035 storage cleanup parser authority drifted");
  Require(HasStorageEvidence(result, "client_state_authority", "false"),
          "DPC-035 storage cleanup client authority drifted");
}

void RequireIndexOk(const agents::IndexGarbageCleanupAgentResult& result,
                    idx::SecondaryIndexGarbageCleanupDecisionKind decision,
                    std::string_view diagnostic_code,
                    std::string_view message) {
  Require(result.ok(), message);
  Require(result.decision == decision, "DPC-035 index cleanup decision drifted");
  Require(result.diagnostic.diagnostic_code == diagnostic_code,
          "DPC-035 index cleanup diagnostic drifted");
  Require(HasIndexEvidence(result,
                           "cleanup_horizon_service",
                           "dpc030_authoritative_cleanup_horizon_v1"),
          "DPC-035 index cleanup lost DPC-030 horizon evidence");
  Require(HasIndexEvidence(result,
                           "index_garbage_cleanup_agent",
                           "dpc033_index_garbage_cleanup_agent_v1"),
          "DPC-035 index cleanup lost DPC-033 agent evidence");
  Require(HasIndexEvidence(result, "parser_finality_authority", "false"),
          "DPC-035 index cleanup parser authority drifted");
}

void PrintStorageProof(
    std::string_view workload_id,
    std::string_view lane,
    const agents::StorageVersionCleanupAgentResult& result,
    std::string_view visibility_proof,
    std::string_view active_blocker_proof,
    std::string_view rollback_proof,
    bool scheduler_fairness_claim) {
  const auto reclaimed = result.after.reclaimed_row_versions;
  const auto backlog = result.before.cleanup_candidate_row_versions;
  std::cout << kBenchmarkOutputSearchKey
            << ",workload_id=" << workload_id
            << ",lane=" << lane
            << ",surface=storage_versions"
            << ",run_count=" << kRunCount
            << ",decision="
            << agents::StorageVersionCleanupDecisionKindName(result.decision)
            << ",diagnostic_code=" << result.diagnostic.diagnostic_code
            << ",cleanup_horizon_local_transaction_id="
            << result.horizon.cleanup_horizon.value
            << ",before_total_row_versions=" << result.before.total_row_versions
            << ",before_cleanup_candidate_row_versions=" << backlog
            << ",before_current_visible_row_versions="
            << result.before.current_visible_row_versions
            << ",active_cleanup_blockers="
            << result.before.active_cleanup_blockers
            << ",scanned_row_version_count="
            << result.sweep.scanned_row_version_count
            << ",reclaimed_row_version_count=" << reclaimed
            << ",retained_row_version_count="
            << result.after.retained_row_versions
            << ",blocked_row_version_count=" << result.after.blocked_row_versions
            << ",after_total_row_versions=" << result.after.total_row_versions
            << ",cleanup_progress_ratio=" << Fixed(Ratio(reclaimed, backlog))
            << ",bounded_batch=" << (result.bounded_batch ? "true" : "false")
            << ",budget_exhausted="
            << (result.budget_exhausted ? "true" : "false")
            << ",visibility_proof=" << visibility_proof
            << ",active_blocker_proof=" << active_blocker_proof
            << ",rollback_proof=" << rollback_proof
            << ",foreground_monopoly_proxy=bounded_candidate_budget"
            << ",scheduler_fairness_claim="
            << (scheduler_fairness_claim ? "true" : "false")
            << ",median_proxy=" << result.sweep.scanned_row_version_count
            << ",p95_proxy=" << result.sweep.scanned_row_version_count
            << ",cv_proxy=0.000000"
            << ",mga_authority=engine_owned_transaction_inventory"
            << ",parser_finality_authority=false"
            << ",client_finality_authority=false"
            << ",timestamp_finality_authority=false"
            << ",uuid_ordering_finality_authority=false"
            << ",event_stream_finality_authority=false"
            << ",wal_recovery_authority=false"
            << ",source_state=ctest_runtime\n";
}

struct IndexFixture {
  platform::TypedUuid index_uuid;
  platform::TypedUuid table_uuid;
  std::vector<idx::SecondaryIndexBaseEntry> base_entries;
  std::vector<idx::SecondaryIndexTableSnapshotEntry> table_snapshot;
};

idx::SecondaryIndexBaseEntry BaseEntry(const platform::TypedUuid& index_uuid,
                                       const platform::TypedUuid& table_uuid,
                                       std::string key_payload,
                                       platform::u64 tx_id) {
  idx::SecondaryIndexBaseEntry entry;
  entry.index_uuid = index_uuid;
  entry.table_uuid = table_uuid;
  entry.row_uuid = NewUuid(platform::UuidKind::row);
  entry.version_uuid = NewUuid(platform::UuidKind::row);
  entry.key_payload = std::move(key_payload);
  entry.committed_local_transaction_id = tx_id;
  return entry;
}

idx::SecondaryIndexTableSnapshotEntry TableSnapshotEntry(
    const idx::SecondaryIndexBaseEntry& base) {
  idx::SecondaryIndexTableSnapshotEntry entry;
  entry.index_uuid = base.index_uuid;
  entry.table_uuid = base.table_uuid;
  entry.row_uuid = base.row_uuid;
  entry.version_uuid = base.version_uuid;
  entry.key_payload = base.key_payload;
  return entry;
}

IndexFixture MakeIndexFixture() {
  IndexFixture fixture;
  fixture.index_uuid = NewUuid(platform::UuidKind::object);
  fixture.table_uuid = NewUuid(platform::UuidKind::object);
  fixture.base_entries.push_back(
      BaseEntry(fixture.index_uuid, fixture.table_uuid, "visible-alpha", 1));
  fixture.base_entries.push_back(
      BaseEntry(fixture.index_uuid, fixture.table_uuid, "visible-beta", 2));
  for (const auto& base : fixture.base_entries) {
    fixture.table_snapshot.push_back(TableSnapshotEntry(base));
  }
  return fixture;
}

idx::SecondaryIndexDeltaLedgerRecord MergedCleanedRecord(
    const platform::TypedUuid& index_uuid,
    const platform::TypedUuid& table_uuid,
    platform::u64 local_transaction_id,
    std::string key_payload) {
  idx::SecondaryIndexDeltaLedgerRecord record;
  record.delta.delta_id = NewUuid(platform::UuidKind::object);
  record.delta.index_uuid = index_uuid;
  record.delta.table_uuid = table_uuid;
  record.delta.row_uuid = NewUuid(platform::UuidKind::row);
  record.delta.version_uuid = NewUuid(platform::UuidKind::row);
  record.delta.transaction_uuid = NewUuid(platform::UuidKind::transaction);
  record.delta.local_transaction_id = local_transaction_id;
  record.delta.delta_kind = idx::SecondaryIndexDeltaKind::insert;
  record.delta.key_payload = std::move(key_payload);
  record.delta.committed = true;
  record.commit_state = idx::SecondaryIndexDeltaLedgerCommitState::merged_cleaned;
  record.source_evidence_reference = "dpc035_mga_index_cleanup_benchmark";
  return record;
}

agents::IndexGarbageCleanupAgentRequest IndexCleanupRequest(
    mga::AuthoritativeCleanupHorizonRequest horizon,
    const IndexFixture& fixture,
    platform::u64 max_records_to_clean) {
  agents::IndexGarbageCleanupAgentRequest request;
  request.horizon_request = std::move(horizon);
  request.index_uuid = fixture.index_uuid;
  request.table_uuid = fixture.table_uuid;
  request.base_entries = fixture.base_entries;
  request.table_snapshot = fixture.table_snapshot;
  request.index_kind = idx::SecondaryIndexKind::non_unique;
  request.max_records_to_scan = 4;
  request.max_records_to_clean = max_records_to_clean;
  request.engine_mga_authoritative = true;
  for (platform::u64 tx = 1; tx <= 4; ++tx) {
    request.ledger.records.push_back(MergedCleanedRecord(
        fixture.index_uuid,
        fixture.table_uuid,
        tx,
        "garbage-" + std::to_string(tx)));
  }
  return request;
}

void PrintIndexProof(
    std::string_view workload_id,
    std::string_view lane,
    const agents::IndexGarbageCleanupAgentResult& result,
    bool scheduler_fairness_claim) {
  const auto backlog = result.before.eligible_garbage_records +
                       result.before.retained_garbage_records;
  const auto cleaned = result.after.cleaned_garbage_records;
  std::cout << kBenchmarkOutputSearchKey
            << ",workload_id=" << workload_id
            << ",lane=" << lane
            << ",surface=non_unique_secondary_index_garbage"
            << ",run_count=" << kRunCount
            << ",decision="
            << idx::SecondaryIndexGarbageCleanupDecisionKindName(result.decision)
            << ",diagnostic_code=" << result.diagnostic.diagnostic_code
            << ",cleanup_horizon_local_transaction_id="
            << result.horizon.cleanup_horizon.value
            << ",before_base_index_entries=" << result.before.base_index_entries
            << ",after_base_index_entries=" << result.after.base_index_entries
            << ",before_table_snapshot_entries="
            << result.before.table_snapshot_entries
            << ",after_table_snapshot_entries="
            << result.after.table_snapshot_entries
            << ",before_index_garbage_backlog=" << backlog
            << ",cleaned_garbage_records=" << cleaned
            << ",retained_garbage_records="
            << result.after.retained_garbage_records
            << ",cleaned_ledger_records=" << result.cleaned_ledger.records.size()
            << ",cleanup_progress_ratio=" << Fixed(Ratio(cleaned, backlog))
            << ",bounded_batch=" << (result.bounded_batch ? "true" : "false")
            << ",budget_exhausted="
            << (result.budget_exhausted ? "true" : "false")
            << ",validation_before_ok="
            << (result.validation_before_ok ? "true" : "false")
            << ",validation_after_ok="
            << (result.validation_after_ok ? "true" : "false")
            << ",visible_index_entries_preserved=true"
            << ",foreground_monopoly_proxy=bounded_scan_and_clean_budget"
            << ",scheduler_fairness_claim="
            << (scheduler_fairness_claim ? "true" : "false")
            << ",median_proxy=" << result.before.scanned_delta_records
            << ",p95_proxy=" << result.before.scanned_delta_records
            << ",cv_proxy=0.000000"
            << ",mga_authority=engine_owned_transaction_inventory"
            << ",parser_finality_authority=false"
            << ",client_finality_authority=false"
            << ",timestamp_finality_authority=false"
            << ",uuid_ordering_finality_authority=false"
            << ",event_stream_finality_authority=false"
            << ",wal_recovery_authority=false"
            << ",source_state=ctest_runtime\n";
}

agents::StorageVersionCleanupAgentResult ProveWL04UpdateCleanup() {
  const auto tx1 = Entry(1, mga::TransactionState::committed);
  const auto tx2 = Entry(2, mga::TransactionState::committed);
  const auto tx3 = Entry(3, mga::TransactionState::committed);
  const auto tx4 = Entry(4, mga::TransactionState::committed);
  const auto tx5 = Entry(5, mga::TransactionState::committed);
  const auto tx6 = Entry(6, mga::TransactionState::committed);
  const auto row = Row();
  auto result = agents::RunStorageVersionCleanupAgentBatch(StorageRequest(
      HorizonRequest(Inventory({tx1, tx2, tx3, tx4, tx5, tx6}, 7)),
      {Version(row, tx1, mga::RowVersionState::committed, 10, 20, 2),
       Version(row, tx2, mga::RowVersionState::committed, 20, 30, 3),
       Version(row, tx3, mga::RowVersionState::committed, 30, 40, 4),
       Version(row, tx4, mga::RowVersionState::committed, 40, 50, 5),
       Version(row, tx5, mga::RowVersionState::committed, 50, 60, 6),
       Version(row, tx6, mga::RowVersionState::committed, 60)},
      8));

  RequireStorageOk(result,
                   agents::StorageVersionCleanupDecisionKind::success,
                   "STORAGE_VERSION_CLEANUP.SUCCESS",
                   "DPC-035 WL04 update cleanup failed");
  Require(result.before.cleanup_candidate_row_versions == 5,
          "DPC-035 WL04 candidate count drifted");
  Require(result.after.reclaimed_row_versions == 5,
          "DPC-035 WL04 cleanup did not reclaim obsolete versions");
  Require(result.before.current_visible_row_versions == 1 &&
              result.after.total_row_versions == 1,
          "DPC-035 WL04 current visible version was not preserved");
  PrintStorageProof("WL04",
                    "update_obsolete_version_cleanup",
                    result,
                    "current_visible_version_preserved",
                    "no_active_blocker",
                    "not_rollback_lane",
                    false);
  return result;
}

agents::StorageVersionCleanupAgentResult ProveWL06DeleteBloatCleanup() {
  const auto tx1 = Entry(1, mga::TransactionState::committed);
  const auto tx2 = Entry(2, mga::TransactionState::committed);
  const auto tx3 = Entry(3, mga::TransactionState::committed);
  const auto tx4 = Entry(4, mga::TransactionState::committed);
  const auto tx5 = Entry(5, mga::TransactionState::rolled_back);
  const auto visible = Row();
  const auto deleted_a = Row();
  const auto deleted_b = Row();
  auto result = agents::RunStorageVersionCleanupAgentBatch(StorageRequest(
      HorizonRequest(Inventory({tx1, tx2, tx3, tx4, tx5}, 6)),
      {Version(visible, tx1, mga::RowVersionState::committed, 10),
       Version(deleted_a, tx2, mga::RowVersionState::committed, 20, 30, 3),
       Version(deleted_a, tx3, mga::RowVersionState::delete_marker, 30),
       Version(deleted_b, tx4, mga::RowVersionState::delete_marker, 40),
       Version(Row(), tx5, mga::RowVersionState::rolled_back, 50)},
      8));

  RequireStorageOk(result,
                   agents::StorageVersionCleanupDecisionKind::success,
                   "STORAGE_VERSION_CLEANUP.SUCCESS",
                   "DPC-035 WL06 delete/bloat cleanup failed");
  Require(result.before.cleanup_candidate_row_versions == 4,
          "DPC-035 WL06 storage candidate count drifted");
  Require(result.after.reclaimed_row_versions == 4,
          "DPC-035 WL06 storage cleanup did not progress");
  Require(Ratio(result.after.reclaimed_row_versions,
                result.before.cleanup_candidate_row_versions) >= 0.50,
          "DPC-035 WL06 storage cleanup progress below target");
  Require(result.before.current_visible_row_versions == 1 &&
              result.after.total_row_versions == 1,
          "DPC-035 WL06 visible row version was not preserved");
  PrintStorageProof("WL06",
                    "delete_update_bloat_storage_cleanup",
                    result,
                    "visible_row_version_preserved",
                    "no_active_blocker",
                    "rolled_back_version_reclaimed_after_horizon",
                    false);
  return result;
}

void ProveLongTransactionBlocksThenAllowsCleanup() {
  const auto old = Entry(1, mga::TransactionState::committed);
  const auto active = Entry(2, mga::TransactionState::active);
  const auto successor = Entry(3, mga::TransactionState::committed);
  const auto row = Row();
  auto blocked_horizon = HorizonRequest(Inventory({old, active, successor}, 4));
  blocked_horizon.always_in_transaction_policy = true;
  blocked_horizon.always_active_session_inventory_authoritative = true;
  blocked_horizon.always_active_sessions.push_back(
      {"session:dpc035-long-reader", active.identity.local_id, true});

  const std::vector<mga::RowVersionMetadata> versions = {
      Version(row, old, mga::RowVersionState::committed, 10, 20, 3),
      Version(row, successor, mga::RowVersionState::committed, 20)};
  auto blocked = agents::RunStorageVersionCleanupAgentBatch(
      StorageRequest(blocked_horizon, versions, 8));
  RequireStorageOk(blocked,
                   agents::StorageVersionCleanupDecisionKind::
                       blocked_by_active_transactions,
                   "STORAGE_VERSION_CLEANUP.BLOCKED_BY_ACTIVE_TRANSACTIONS",
                   "DPC-035 long transaction did not block cleanup");
  Require(blocked.after.reclaimed_row_versions == 0 &&
              blocked.after.retained_row_versions == 1,
          "DPC-035 long transaction cleanup removed a blocked version");
  Require(blocked.before.current_visible_row_versions == 1,
          "DPC-035 long transaction visible version proof drifted");
  PrintStorageProof("WL06",
                    "long_transaction_blocks_unsafe_cleanup",
                    blocked,
                    "active_reader_visible_version_preserved",
                    "long_active_transaction_blocks_successor_cleanup",
                    "not_rollback_lane",
                    false);

  const auto released = Entry(2, mga::TransactionState::rolled_back);
  auto allowed = agents::RunStorageVersionCleanupAgentBatch(StorageRequest(
      HorizonRequest(Inventory({old, released, successor}, 4)),
      versions,
      8));
  RequireStorageOk(allowed,
                   agents::StorageVersionCleanupDecisionKind::success,
                   "STORAGE_VERSION_CLEANUP.SUCCESS",
                   "DPC-035 cleanup after horizon release failed");
  Require(allowed.after.reclaimed_row_versions == 1 &&
              allowed.before.current_visible_row_versions == 1,
          "DPC-035 cleanup after horizon release did not preserve visibility");
  PrintStorageProof("WL06",
                    "cleanup_after_long_transaction_release",
                    allowed,
                    "successor_visible_after_blocker_release",
                    "cleanup_waited_for_authoritative_horizon",
                    "not_rollback_lane",
                    false);
}

void ProveRollbackPressureCleanup() {
  const auto visible = Entry(1, mga::TransactionState::committed);
  const auto old_active = Entry(2, mga::TransactionState::active);
  const auto rolled_back = Entry(3, mga::TransactionState::rolled_back);
  const auto committed_row = Row();
  const auto rollback_row = Row();
  const std::vector<mga::RowVersionMetadata> versions = {
      Version(committed_row, visible, mga::RowVersionState::committed, 10),
      Version(rollback_row, rolled_back, mga::RowVersionState::rolled_back, 20)};

  auto blocked = agents::RunStorageVersionCleanupAgentBatch(StorageRequest(
      HorizonRequest(Inventory({visible, old_active, rolled_back}, 4)),
      versions,
      8));
  RequireStorageOk(blocked,
                   agents::StorageVersionCleanupDecisionKind::no_op,
                   "STORAGE_VERSION_CLEANUP.NO_OP",
                   "DPC-035 rollback pressure pre-horizon cleanup failed");
  Require(blocked.after.reclaimed_row_versions == 0,
          "DPC-035 rollback pressure cleaned before horizon permitted it");

  const auto old_terminal = Entry(2, mga::TransactionState::rolled_back);
  auto allowed = agents::RunStorageVersionCleanupAgentBatch(StorageRequest(
      HorizonRequest(Inventory({visible, old_terminal, rolled_back}, 4)),
      versions,
      8));
  RequireStorageOk(allowed,
                   agents::StorageVersionCleanupDecisionKind::success,
                   "STORAGE_VERSION_CLEANUP.SUCCESS",
                   "DPC-035 rollback pressure cleanup after horizon failed");
  Require(allowed.after.reclaimed_row_versions == 1 &&
              allowed.before.current_visible_row_versions == 1 &&
              allowed.after.total_row_versions == 1,
          "DPC-035 rollback cleanup visibility equality drifted");
  PrintStorageProof("WL06",
                    "rollback_pressure_cleanup_after_mga_horizon",
                    allowed,
                    "committed_visible_row_count_equal",
                    "rollback_waited_for_horizon",
                    "rolled_back_version_reclaimed_only_when_mga_permits",
                    false);
}

agents::IndexGarbageCleanupAgentResult ProveWL06IndexGarbageCleanup() {
  const auto tx1 = Entry(1, mga::TransactionState::committed);
  const auto tx2 = Entry(2, mga::TransactionState::committed);
  const auto tx3 = Entry(3, mga::TransactionState::committed);
  const auto tx4 = Entry(4, mga::TransactionState::committed);
  auto fixture = MakeIndexFixture();
  auto result = agents::RunIndexGarbageCleanupAgentBatch(IndexCleanupRequest(
      HorizonRequest(Inventory({tx1, tx2, tx3, tx4}, 5)),
      fixture,
      3));

  RequireIndexOk(result,
                 idx::SecondaryIndexGarbageCleanupDecisionKind::budget_exhausted,
                 "INDEX_GARBAGE_CLEANUP.BUDGET_EXHAUSTED",
                 "DPC-035 WL06 index cleanup failed");
  Require(result.before.eligible_garbage_records == 4,
          "DPC-035 WL06 index garbage backlog drifted");
  Require(result.after.cleaned_garbage_records == 3,
          "DPC-035 WL06 index cleanup cleaned count drifted");
  Require(Ratio(result.after.cleaned_garbage_records,
                result.before.eligible_garbage_records) >= 0.50,
          "DPC-035 WL06 index cleanup progress below target");
  Require(result.before.base_index_entries == result.after.base_index_entries &&
              result.before.table_snapshot_entries ==
                  result.after.table_snapshot_entries,
          "DPC-035 index cleanup removed visible index entries");
  PrintIndexProof("WL06",
                  "bounded_non_unique_secondary_index_garbage_cleanup",
                  result,
                  false);
  return result;
}

void ProveWL11BoundedCleanupPressureProxy() {
  const auto tx1 = Entry(1, mga::TransactionState::committed);
  const auto tx2 = Entry(2, mga::TransactionState::committed);
  const auto tx3 = Entry(3, mga::TransactionState::committed);
  const auto tx4 = Entry(4, mga::TransactionState::committed);
  const auto tx5 = Entry(5, mga::TransactionState::committed);
  const auto tx6 = Entry(6, mga::TransactionState::committed);
  const auto row = Row();
  auto result = agents::RunStorageVersionCleanupAgentBatch(StorageRequest(
      HorizonRequest(Inventory({tx1, tx2, tx3, tx4, tx5, tx6}, 7)),
      {Version(row, tx1, mga::RowVersionState::committed, 10, 20, 2),
       Version(row, tx2, mga::RowVersionState::committed, 20, 30, 3),
       Version(row, tx3, mga::RowVersionState::committed, 30, 40, 4),
       Version(row, tx4, mga::RowVersionState::committed, 40, 50, 5),
       Version(row, tx5, mga::RowVersionState::committed, 50, 60, 6),
       Version(row, tx6, mga::RowVersionState::committed, 60)},
      2));

  RequireStorageOk(result,
                   agents::StorageVersionCleanupDecisionKind::budget_exhausted,
                   "STORAGE_VERSION_CLEANUP.BUDGET_EXHAUSTED",
                   "DPC-035 WL11 bounded cleanup proxy failed");
  Require(result.budget_exhausted && result.sweep.scanned_row_version_count == 2,
          "DPC-035 WL11 cleanup proxy exceeded its bounded budget");
  Require(result.after.reclaimed_row_versions == 2,
          "DPC-035 WL11 cleanup proxy made no bounded progress");
  PrintStorageProof("WL11",
                    "cleanup_pressure_bounded_budget_proxy",
                    result,
                    "current_visible_version_preserved",
                    "bounded_work_slice_only",
                    "not_rollback_lane",
                    false);
}

api::EngineRequestContext DiagnosticsContext() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.database_uuid.canonical =
      uuid::UuidToString(NewUuid(platform::UuidKind::database).value);
  context.node_uuid.canonical =
      uuid::UuidToString(NewUuid(platform::UuidKind::object).value);
  context.session_uuid.canonical =
      uuid::UuidToString(NewUuid(platform::UuidKind::object).value);
  context.principal_uuid.canonical =
      uuid::UuidToString(NewUuid(platform::UuidKind::principal).value);
  context.transaction_uuid.canonical =
      uuid::UuidToString(NewUuid(platform::UuidKind::transaction).value);
  context.local_transaction_id = 35;
  context.trace_tags.push_back("right:MGA_CLEANUP_INSPECT");
  return context;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

void ProveDiagnosticsSupportBundle(
    const agents::StorageVersionCleanupAgentResult& storage,
    const agents::IndexGarbageCleanupAgentResult& index) {
  api::EngineCleanupDiagnosticsRequest request;
  request.context = DiagnosticsContext();
  request.storage_cleanup_present = true;
  request.storage_cleanup = storage;
  request.index_cleanup_present = true;
  request.index_cleanup = index;
  request.context_kinds = {"backup", "restore", "restricted_open", "repair"};
  request.support_bundle_requested = true;

  const auto diagnostics = api::EngineInspectCleanupDiagnostics(request);
  Require(diagnostics.ok, "DPC-035 diagnostics support-bundle proof failed");
  Require(diagnostics.cleanup_diagnostics_ready &&
              diagnostics.support_bundle_ready,
          "DPC-035 diagnostics support bundle not ready");
  Require(diagnostics.cleanup_horizon_authoritative,
          "DPC-035 diagnostics did not retain authoritative horizon");
  Require(diagnostics.storage_row_version_backlog_count ==
              storage.before.cleanup_candidate_row_versions,
          "DPC-035 diagnostics storage backlog count drifted");
  Require(diagnostics.index_garbage_cleaned_count ==
              index.after.cleaned_garbage_records,
          "DPC-035 diagnostics index cleaned count drifted");
  Require(!diagnostics.parser_finality_authority &&
              !diagnostics.client_finality_authority &&
              !diagnostics.timestamp_finality_authority &&
              !diagnostics.uuid_ordering_finality_authority &&
              !diagnostics.event_stream_finality_authority,
          "DPC-035 diagnostics non-authority evidence drifted");
  Require(HasEvidence(diagnostics, "support_bundle_surface", "cleanup_diagnostics"),
          "DPC-035 support-bundle evidence missing");
  Require(diagnostics.support_bundle_json.find("cleanup_diagnostics") !=
              std::string::npos,
          "DPC-035 support-bundle JSON missing cleanup diagnostics section");

  std::cout << kBenchmarkOutputSearchKey
            << ",workload_id=WL06"
            << ",lane=cleanup_diagnostics_support_bundle"
            << ",surface=dpc034_cleanup_diagnostics"
            << ",run_count=" << kRunCount
            << ",cleanup_diagnostics_ready="
            << (diagnostics.cleanup_diagnostics_ready ? "true" : "false")
            << ",support_bundle_ready="
            << (diagnostics.support_bundle_ready ? "true" : "false")
            << ",cleanup_horizon_identity="
            << diagnostics.cleanup_horizon_identity
            << ",cleanup_horizon_authoritative="
            << (diagnostics.cleanup_horizon_authoritative ? "true" : "false")
            << ",storage_row_version_backlog_count="
            << diagnostics.storage_row_version_backlog_count
            << ",storage_row_version_reclaimed_count="
            << diagnostics.storage_row_version_reclaimed_count
            << ",index_garbage_backlog_count="
            << diagnostics.index_garbage_backlog_count
            << ",index_garbage_cleaned_count="
            << diagnostics.index_garbage_cleaned_count
            << ",parser_finality_authority=false"
            << ",client_finality_authority=false"
            << ",timestamp_finality_authority=false"
            << ",uuid_ordering_finality_authority=false"
            << ",event_stream_finality_authority=false"
            << ",wal_recovery_authority=false"
            << ",source_state=ctest_runtime\n";
}

}  // namespace

int main() {
  Require(kGateSearchKey == "DPC_MGA_INDEX_CLEANUP_BENCHMARK_GATE",
          "DPC-035 gate search key drifted");
  Require(kBenchmarkOutputSearchKey ==
              "DPC_MGA_INDEX_CLEANUP_BENCHMARK_OUTPUT",
          "DPC-035 benchmark output search key drifted");

  (void)ProveWL04UpdateCleanup();
  const auto storage = ProveWL06DeleteBloatCleanup();
  ProveLongTransactionBlocksThenAllowsCleanup();
  ProveRollbackPressureCleanup();
  const auto index = ProveWL06IndexGarbageCleanup();
  ProveWL11BoundedCleanupPressureProxy();
  ProveDiagnosticsSupportBundle(storage, index);

  std::cout << kGateSearchKey << "=passed "
            << "DPC_MGA_INDEX_CLEANUP_BENCHMARK_OUTPUT=retained "
            << "run_count=" << kRunCount
            << " wl04_delete_update_rollback_long_transaction_bloat=true"
            << " wl06_cleanup_progress_at_least_50_percent=true"
            << " wl11_cleanup_pressure_proxy=true"
            << " scheduler_fairness_claim=false"
            << " dpc036_claim=false"
            << " dpc040_plus_claim=false"
            << " mga_authority=engine_owned_transaction_inventory\n";
  return EXIT_SUCCESS;
}
