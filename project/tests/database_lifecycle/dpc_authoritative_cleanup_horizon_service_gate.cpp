// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_cleanup_horizon_service.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey =
    "DPC_AUTHORITATIVE_CLEANUP_HORIZON_SERVICE_GATE";

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
  static platform::u64 next = 1779503000000ull;
  return ++next;
}

platform::TypedUuid NewUuid(platform::UuidKind kind) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NextMillis());
  Require(generated.ok(), "DPC-030 generated UUID creation failed");
  return generated.value;
}

mga::TransactionIdentity NewIdentity(platform::u64 local_id) {
  const auto identity = mga::MakeTransactionIdentity(
      mga::MakeLocalTransactionId(local_id),
      NewUuid(platform::UuidKind::transaction),
      mga::TransactionScope::local_node);
  Require(identity.ok(), "DPC-030 transaction identity creation failed");
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

mga::AuthoritativeCleanupHorizonRequest Request(
    mga::LocalTransactionInventory inventory) {
  mga::AuthoritativeCleanupHorizonRequest request;
  request.inventory = std::move(inventory);
  request.inventory_authoritative = true;
  request.inventory_complete = true;
  request.active_snapshot_inventory_authoritative = true;
  return request;
}

bool HasEvidence(const mga::AuthoritativeCleanupHorizonResult& result,
                 std::string_view key,
                 std::string_view value) {
  for (const auto& field : result.evidence) {
    if (field.key == key && field.value == value) {
      return true;
    }
  }
  return false;
}

bool HasBlocker(const mga::AuthoritativeCleanupHorizonResult& result,
                mga::CleanupHorizonBlockerKind kind,
                platform::u64 local_transaction_id,
                std::string_view stable_name = {}) {
  for (const auto& blocker : result.blockers) {
    if (blocker.kind == kind &&
        blocker.local_transaction_id.value == local_transaction_id &&
        (stable_name.empty() || blocker.stable_name == stable_name)) {
      return true;
    }
  }
  return false;
}

void TestNoActiveBlockers() {
  const auto inventory = Inventory({
      Entry(1, mga::TransactionState::committed),
      Entry(2, mga::TransactionState::rolled_back),
      Entry(3, mga::TransactionState::committed),
  }, 4);

  const auto result = mga::ComputeAuthoritativeCleanupHorizon(Request(inventory));
  Require(result.ok(), "DPC-030 no-active horizon should be authoritative");
  Require(result.horizons.oldest_interesting_transaction.value == 4,
          "DPC-030 no-active OIT mismatch");
  Require(result.horizons.oldest_active_transaction.value == 4,
          "DPC-030 no-active OAT mismatch");
  Require(result.horizons.oldest_snapshot_transaction.value == 4,
          "DPC-030 no-active OST mismatch");
  Require(result.cleanup_horizon.value == 4,
          "DPC-030 no-active cleanup horizon mismatch");
  Require(result.blockers.empty(), "DPC-030 no-active blockers should be empty");
}

void TestActiveOldTransactionBlocksCleanup() {
  const auto inventory = Inventory({
      Entry(1, mga::TransactionState::committed),
      Entry(2, mga::TransactionState::active),
      Entry(3, mga::TransactionState::committed),
  }, 4);

  const auto result = mga::ComputeAuthoritativeCleanupHorizon(Request(inventory));
  Require(result.ok(), "DPC-030 active-old horizon should be authoritative");
  Require(result.cleanup_horizon.value == 2,
          "DPC-030 active-old cleanup horizon must stop at active transaction");
  Require(result.horizons.oldest_active_transaction.value == 2,
          "DPC-030 active-old OAT mismatch");
  Require(HasBlocker(result,
                     mga::CleanupHorizonBlockerKind::active_transaction,
                     2),
          "DPC-030 active-old blocker missing");
}

void TestNewerFinalTransactionsDoNotAdvancePastOldActive() {
  const auto inventory = Inventory({
      Entry(2, mga::TransactionState::active),
      Entry(3, mga::TransactionState::committed),
      Entry(4, mga::TransactionState::rolled_back),
  }, 5);

  const auto result = mga::ComputeAuthoritativeCleanupHorizon(Request(inventory));
  Require(result.ok(), "DPC-030 newer-final horizon should be authoritative");
  Require(result.cleanup_horizon.value == 2,
          "DPC-030 newer final transactions advanced past old active transaction");
  Require(!HasBlocker(result,
                      mga::CleanupHorizonBlockerKind::active_transaction,
                      3),
          "DPC-030 committed newer transaction was treated as active blocker");
  Require(!HasBlocker(result,
                      mga::CleanupHorizonBlockerKind::active_transaction,
                      4),
          "DPC-030 rolled-back newer transaction was treated as active blocker");
}

void TestAlwaysInTransactionReplacementBehavior() {
  const auto committed = Entry(1, mga::TransactionState::committed);
  const auto rolled_back = Entry(2, mga::TransactionState::rolled_back);
  const auto replacement = Entry(3, mga::TransactionState::active);
  const auto inventory = Inventory({committed, rolled_back, replacement}, 4);

  auto refused = Request(inventory);
  refused.always_in_transaction_policy = true;
  refused.always_active_sessions.push_back(
      {"session:primary", committed.identity.local_id, true});
  const auto terminal_binding =
      mga::ComputeAuthoritativeCleanupHorizon(refused);
  Require(!terminal_binding.ok(),
          "DPC-030 terminal session transaction binding should fail closed");
  Require(terminal_binding.diagnostic.diagnostic_code ==
              "SB-MGA-CLEANUP-HORIZON-SESSION-TX-NOT-ACTIVE",
          "DPC-030 terminal session binding diagnostic mismatch");

  auto request = Request(inventory);
  request.always_in_transaction_policy = true;
  request.always_active_session_inventory_authoritative = true;
  request.always_active_sessions.push_back(
      {"session:primary", replacement.identity.local_id, true});

  const auto result = mga::ComputeAuthoritativeCleanupHorizon(request);
  Require(result.ok(), "DPC-030 replacement session horizon should be authoritative");
  Require(result.cleanup_horizon.value == 3,
          "DPC-030 replacement active transaction was not represented in horizon");
  Require(HasBlocker(result,
                     mga::CleanupHorizonBlockerKind::always_active_session,
                     3,
                     "session:primary"),
          "DPC-030 always-active session blocker missing");
  Require(HasEvidence(result, "always_in_transaction_policy", "true"),
          "DPC-030 always-in-transaction policy evidence missing");
  Require(HasEvidence(result, "always_active_session_count", "1"),
          "DPC-030 always-active session count evidence missing");
}

void TestNonAuthoritativeAndMissingInventoryRefusals() {
  const auto inventory = Inventory({
      Entry(1, mga::TransactionState::committed),
  }, 2);

  auto non_authoritative = Request(inventory);
  non_authoritative.inventory_authoritative = false;
  const auto non_authoritative_result =
      mga::ComputeAuthoritativeCleanupHorizon(non_authoritative);
  Require(!non_authoritative_result.ok(),
          "DPC-030 non-authoritative inventory should fail closed");
  Require(non_authoritative_result.diagnostic.diagnostic_code ==
              "SB-MGA-CLEANUP-HORIZON-NOT-AUTHORITATIVE",
          "DPC-030 non-authoritative inventory diagnostic mismatch");
  Require(HasEvidence(non_authoritative_result, "fail_closed", "true"),
          "DPC-030 non-authoritative refusal evidence missing");

  auto missing = Request(inventory);
  missing.inventory_complete = false;
  const auto missing_result = mga::ComputeAuthoritativeCleanupHorizon(missing);
  Require(!missing_result.ok(),
          "DPC-030 missing inventory should fail closed");
  Require(missing_result.diagnostic.diagnostic_code ==
              "SB-MGA-CLEANUP-HORIZON-INVENTORY-MISSING",
          "DPC-030 missing inventory diagnostic mismatch");

  auto snapshot_not_authoritative = Request(inventory);
  snapshot_not_authoritative.active_snapshot_inventory_authoritative = false;
  const auto snapshot_result =
      mga::ComputeAuthoritativeCleanupHorizon(snapshot_not_authoritative);
  Require(!snapshot_result.ok(),
          "DPC-030 non-authoritative snapshot inventory should fail closed");
  Require(snapshot_result.diagnostic.diagnostic_code ==
              "SB-MGA-CLEANUP-HORIZON-SNAPSHOT-INVENTORY-NOT-AUTHORITATIVE",
          "DPC-030 snapshot inventory diagnostic mismatch");
}

void TestSupportEvidenceIsStable() {
  const auto inventory = Inventory({
      Entry(1, mga::TransactionState::committed),
      Entry(2, mga::TransactionState::active),
  }, 3);
  auto request = Request(inventory);
  request.active_snapshot_horizons.push_back(mga::MakeLocalTransactionId(2));

  const auto result = mga::ComputeAuthoritativeCleanupHorizon(request);
  Require(result.ok(), "DPC-030 support evidence horizon should be authoritative");
  Require(result.diagnostic.diagnostic_code ==
              "SB-MGA-CLEANUP-HORIZON-AUTHORITATIVE",
          "DPC-030 authoritative diagnostic mismatch");
  Require(HasEvidence(result,
                      "cleanup_horizon_service",
                      "dpc030_authoritative_cleanup_horizon_v1"),
          "DPC-030 service evidence missing");
  Require(HasEvidence(result,
                      "authority_source",
                      "durable_mga_transaction_inventory"),
          "DPC-030 authority evidence missing");
  Require(HasEvidence(result, "parser_finality_authority", "false"),
          "DPC-030 parser authority evidence missing");
  Require(HasEvidence(result, "client_state_authority", "false"),
          "DPC-030 client-state authority evidence missing");
  Require(HasEvidence(result, "timestamp_ordering_authority", "false"),
          "DPC-030 timestamp authority evidence missing");
  Require(HasEvidence(result, "uuid_ordering_authority", "false"),
          "DPC-030 UUID-order authority evidence missing");
  Require(HasEvidence(result, "crud_event_stream_authority", "false"),
          "DPC-030 event-stream authority evidence missing");
  Require(HasEvidence(result, "cluster_private_implementation", "false"),
          "DPC-030 cluster-boundary evidence missing");
  Require(HasEvidence(result, "oit_local_transaction_id", "2"),
          "DPC-030 OIT evidence missing");
  Require(HasEvidence(result, "oat_local_transaction_id", "2"),
          "DPC-030 OAT evidence missing");
  Require(HasEvidence(result, "ost_local_transaction_id", "2"),
          "DPC-030 OST evidence missing");
  Require(HasEvidence(result, "cleanup_horizon_local_transaction_id", "2"),
          "DPC-030 cleanup horizon evidence missing");
  Require(HasEvidence(result, "blocker_count", "2"),
          "DPC-030 blocker count evidence missing");
}

}  // namespace

int main() {
  std::cout << kGateSearchKey << '\n';
  TestNoActiveBlockers();
  TestActiveOldTransactionBlocksCleanup();
  TestNewerFinalTransactionsDoNotAdvancePastOldActive();
  TestAlwaysInTransactionReplacementBehavior();
  TestNonAuthoritativeAndMissingInventoryRefusals();
  TestSupportEvidenceIsStable();
  return EXIT_SUCCESS;
}
