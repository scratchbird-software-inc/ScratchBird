// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/storage_version_cleanup_agent.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents::implemented_agents;
namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey =
    "DPC_STORAGE_VERSION_CLEANUP_AGENT_GATE";

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
  static platform::u64 next = 1779517000000ull;
  return ++next;
}

platform::TypedUuid NewUuid(platform::UuidKind kind) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NextMillis());
  Require(generated.ok(), "DPC-032 generated UUID creation failed");
  return generated.value;
}

mga::TransactionIdentity NewIdentity(platform::u64 local_id) {
  const auto identity = mga::MakeTransactionIdentity(
      mga::MakeLocalTransactionId(local_id),
      NewUuid(platform::UuidKind::transaction),
      mga::TransactionScope::local_node);
  Require(identity.ok(), "DPC-032 transaction identity creation failed");
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
  metadata.payload_present = state != mga::RowVersionState::rolled_back;
  if (next_sequence != 0) {
    metadata.chain.next_version_sequence = next_sequence;
  }
  if (successor_local_id != 0) {
    metadata.successor_transaction_local_id =
        mga::MakeLocalTransactionId(successor_local_id);
  }
  return metadata;
}

agents::StorageVersionCleanupAgentRequest Request(
    mga::AuthoritativeCleanupHorizonRequest horizon,
    std::vector<mga::RowVersionMetadata> row_versions,
    platform::u64 max_candidates = 32) {
  agents::StorageVersionCleanupAgentRequest request;
  request.horizon_request = std::move(horizon);
  request.row_versions = std::move(row_versions);
  request.max_candidate_row_versions = max_candidates;
  request.engine_mga_authoritative = true;
  return request;
}

bool HasEvidence(const agents::StorageVersionCleanupAgentResult& result,
                 std::string_view key,
                 std::string_view value) {
  for (const auto& field : result.evidence) {
    if (field.key == key && field.value == value) {
      return true;
    }
  }
  return false;
}

void RequireDecision(const agents::StorageVersionCleanupAgentResult& result,
                     agents::StorageVersionCleanupDecisionKind decision,
                     std::string_view code) {
  Require(result.decision == decision, "DPC-032 decision mismatch");
  Require(result.diagnostic.diagnostic_code == code,
          "DPC-032 diagnostic code mismatch");
  Require(HasEvidence(result,
                      "decision",
                      agents::StorageVersionCleanupDecisionKindName(decision)),
          "DPC-032 decision evidence missing");
  Require(HasEvidence(result,
                      "cleanup_horizon_service",
                      "dpc030_authoritative_cleanup_horizon_v1"),
          "DPC-032 DPC-030 cleanup horizon evidence missing");
  Require(HasEvidence(result, "parser_finality_authority", "false"),
          "DPC-032 parser authority evidence missing");
  Require(HasEvidence(result, "client_state_authority", "false"),
          "DPC-032 client authority evidence missing");
}

void TestSuccessfulCleanupRecordsBeforeAfterPressure() {
  const auto old = Entry(1, mga::TransactionState::committed);
  const auto successor = Entry(2, mga::TransactionState::committed);
  const auto rolled_back = Entry(3, mga::TransactionState::rolled_back);
  const auto inventory = Inventory({old, successor, rolled_back}, 4);
  const auto row = Row();
  const auto result = agents::RunStorageVersionCleanupAgentBatch(Request(
      HorizonRequest(inventory),
      {Version(row, old, mga::RowVersionState::committed, 10, 20, 2),
       Version(row, successor, mga::RowVersionState::committed, 20),
       Version(Row(), rolled_back, mga::RowVersionState::rolled_back, 30)}));

  Require(result.ok(), "DPC-032 successful cleanup failed");
  RequireDecision(result,
                  agents::StorageVersionCleanupDecisionKind::success,
                  "STORAGE_VERSION_CLEANUP.SUCCESS");
  Require(result.sweep.cleanup.reclaimed_row_version_count == 2,
          "DPC-032 success did not reclaim eligible versions");
  Require(result.before.total_row_versions == 3 &&
              result.before.cleanup_candidate_row_versions == 2 &&
              result.before.current_visible_row_versions == 1,
          "DPC-032 before pressure metrics mismatch");
  Require(result.after.total_row_versions == 1 &&
              result.after.reclaimed_row_versions == 2,
          "DPC-032 after pressure metrics mismatch");
  Require(HasEvidence(result, "before_total_row_versions", "3"),
          "DPC-032 before total evidence missing");
  Require(HasEvidence(result, "after_reclaimed_row_versions", "2"),
          "DPC-032 after reclaimed evidence missing");
}

void TestActiveTransactionPreservesVisibleOldVersion() {
  const auto old = Entry(1, mga::TransactionState::committed);
  const auto active = Entry(2, mga::TransactionState::active);
  const auto successor = Entry(3, mga::TransactionState::committed);
  const auto inventory = Inventory({old, active, successor}, 4);
  auto horizon = HorizonRequest(inventory);
  horizon.always_in_transaction_policy = true;
  horizon.always_active_session_inventory_authoritative = true;
  horizon.always_active_sessions.push_back(
      {"session:primary", active.identity.local_id, true});
  const auto row = Row();

  const auto result = agents::RunStorageVersionCleanupAgentBatch(Request(
      horizon,
      {Version(row, old, mga::RowVersionState::committed, 10, 20, 3),
       Version(row, successor, mga::RowVersionState::committed, 20)}));

  Require(result.ok(), "DPC-032 active blocker cleanup should complete safely");
  RequireDecision(result,
                  agents::StorageVersionCleanupDecisionKind::blocked_by_active_transactions,
                  "STORAGE_VERSION_CLEANUP.BLOCKED_BY_ACTIVE_TRANSACTIONS");
  Require(result.sweep.cleanup.reclaimed_row_version_count == 0,
          "DPC-032 active blocker allowed visible old version reclaim");
  Require(result.sweep.cleanup.retained_row_version_count == 1,
          "DPC-032 active blocker did not retain candidate");
  Require(result.before.active_cleanup_blockers != 0,
          "DPC-032 active blocker pressure metric missing");
}

void TestBudgetExhaustedUsesBoundedBatch() {
  const auto old1 = Entry(1, mga::TransactionState::committed);
  const auto old2 = Entry(2, mga::TransactionState::committed);
  const auto rolled_back = Entry(3, mga::TransactionState::rolled_back);
  const auto successor1 = Entry(4, mga::TransactionState::committed);
  const auto successor2 = Entry(5, mga::TransactionState::committed);
  const auto inventory = Inventory({old1, old2, rolled_back, successor1, successor2}, 6);
  const auto row1 = Row();
  const auto row2 = Row();

  const auto result = agents::RunStorageVersionCleanupAgentBatch(Request(
      HorizonRequest(inventory),
      {Version(row1, old1, mga::RowVersionState::committed, 10, 20, 4),
       Version(row2, old2, mga::RowVersionState::committed, 30, 40, 5),
       Version(Row(), rolled_back, mga::RowVersionState::rolled_back, 50),
       Version(row1, successor1, mga::RowVersionState::committed, 20),
       Version(row2, successor2, mga::RowVersionState::committed, 40)},
      2));

  Require(result.ok(), "DPC-032 budget-exhausted batch should be accepted");
  RequireDecision(result,
                  agents::StorageVersionCleanupDecisionKind::budget_exhausted,
                  "STORAGE_VERSION_CLEANUP.BUDGET_EXHAUSTED");
  Require(result.budget_exhausted, "DPC-032 budget exhausted flag missing");
  Require(result.sweep.scanned_row_version_count == 2,
          "DPC-032 bounded batch scanned beyond budget");
  Require(result.sweep.cleanup.reclaimed_row_version_count == 2,
          "DPC-032 bounded batch did not clean selected candidates");
}

void TestRolledBackSuccessorDoesNotRemoveCurrentVisibleVersion() {
  const auto old = Entry(1, mga::TransactionState::committed);
  const auto rolled_back_successor = Entry(2, mga::TransactionState::rolled_back);
  const auto inventory = Inventory({old, rolled_back_successor}, 3);
  const auto row = Row();

  const auto result = agents::RunStorageVersionCleanupAgentBatch(Request(
      HorizonRequest(inventory),
      {Version(row,
               old,
               mga::RowVersionState::committed,
               10,
               20,
               rolled_back_successor.identity.local_id.value)}));

  Require(result.ok(), "DPC-032 rolled-back successor retention failed");
  RequireDecision(result,
                  agents::StorageVersionCleanupDecisionKind::no_op,
                  "STORAGE_VERSION_CLEANUP.NO_OP");
  Require(result.sweep.cleanup.reclaimed_row_version_count == 0,
          "DPC-032 rolled-back successor allowed current visible reclaim");
  Require(result.sweep.cleanup.limbo_or_unknown_outcome_blocked_row_version_count == 1,
          "DPC-032 rolled-back successor was not reported as blocked");
}

void TestRetentionHorizonBelowSuccessorPreservesVisibleOldVersion() {
  const auto old = Entry(1, mga::TransactionState::committed);
  const auto successor = Entry(3, mga::TransactionState::committed);
  const auto inventory = Inventory({old, successor}, 4);
  const auto row = Row();

  auto request = Request(
      HorizonRequest(inventory),
      {Version(row,
               old,
               mga::RowVersionState::committed,
               10,
               20,
               successor.identity.local_id.value)});
  request.history_retention_horizons.push_back(
      {mga::MakeLocalTransactionId(2), true, "retention_before_successor"});

  const auto result = agents::RunStorageVersionCleanupAgentBatch(request);
  Require(result.ok(), "DPC-032 retention successor guard failed");
  RequireDecision(result,
                  agents::StorageVersionCleanupDecisionKind::no_op,
                  "STORAGE_VERSION_CLEANUP.NO_OP");
  Require(result.sweep.cleanup.reclaimed_row_version_count == 0,
          "DPC-032 retention horizon allowed successor-visible reclaim");
  Require(result.sweep.cleanup.horizon_blocked_row_version_count == 1,
          "DPC-032 retention successor guard was not reported as blocked");
}

void TestNoOpAndNonAuthoritativeRefusals() {
  const auto current = Entry(1, mga::TransactionState::committed);
  const auto inventory = Inventory({current}, 2);
  const auto no_op = agents::RunStorageVersionCleanupAgentBatch(Request(
      HorizonRequest(inventory),
      {Version(Row(), current, mga::RowVersionState::committed, 10)}));
  Require(no_op.ok(), "DPC-032 no-op cleanup failed");
  RequireDecision(no_op,
                  agents::StorageVersionCleanupDecisionKind::no_op,
                  "STORAGE_VERSION_CLEANUP.NO_OP");

  auto non_authoritative = HorizonRequest(inventory);
  non_authoritative.inventory_authoritative = false;
  const auto refused = agents::RunStorageVersionCleanupAgentBatch(Request(
      non_authoritative,
      {Version(Row(), current, mga::RowVersionState::committed, 20)}));
  Require(!refused.ok(), "DPC-032 non-authoritative inventory was accepted");
  RequireDecision(refused,
                  agents::StorageVersionCleanupDecisionKind::refused_non_authoritative,
                  "STORAGE_VERSION_CLEANUP.NON_AUTHORITATIVE_REFUSAL");

  auto missing_active = HorizonRequest(inventory);
  missing_active.always_in_transaction_policy = true;
  missing_active.always_active_sessions.push_back(
      {"session:inactive", current.identity.local_id, true});
  const auto inactive_refused = agents::RunStorageVersionCleanupAgentBatch(
      Request(missing_active,
              {Version(Row(), current, mga::RowVersionState::committed, 30)}));
  Require(!inactive_refused.ok(),
          "DPC-032 inactive always-in-transaction shape was accepted");
  RequireDecision(inactive_refused,
                  agents::StorageVersionCleanupDecisionKind::refused_non_authoritative,
                  "STORAGE_VERSION_CLEANUP.NON_AUTHORITATIVE_REFUSAL");
}

}  // namespace

int main() {
  std::cout << kGateSearchKey << '\n';
  TestSuccessfulCleanupRecordsBeforeAfterPressure();
  TestActiveTransactionPreservesVisibleOldVersion();
  TestBudgetExhaustedUsesBoundedBatch();
  TestRolledBackSuccessorDoesNotRemoveCurrentVisibleVersion();
  TestRetentionHorizonBelowSuccessorPreservesVisibleOldVersion();
  TestNoOpAndNonAuthoritativeRefusals();
  return EXIT_SUCCESS;
}
