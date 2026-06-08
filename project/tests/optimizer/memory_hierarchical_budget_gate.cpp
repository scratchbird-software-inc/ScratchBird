// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "resource_governance_admission.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool EvidenceHas(const std::vector<std::string>& evidence, std::string_view token) {
  for (const auto& item : evidence) {
    if (item.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

agents::HierarchicalMemoryBudgetScope Scope(
    std::string_view scope_id,
    std::string_view parent_scope_id,
    agents::HierarchicalMemoryBudgetScopeKind kind,
    std::uint64_t limit_bytes) {
  agents::HierarchicalMemoryBudgetScope scope;
  scope.scope_id = std::string(scope_id);
  scope.parent_scope_id = std::string(parent_scope_id);
  scope.kind = kind;
  scope.limit_bytes = limit_bytes;
  scope.active = true;
  return scope;
}

void RegisterCoreTree(agents::HierarchicalMemoryBudgetLedger* ledger) {
  Require(ledger->RegisterScope(Scope("db", "", agents::HierarchicalMemoryBudgetScopeKind::kDatabase, 1000)).ok,
          "MMCH-017 database scope registration failed");
  Require(ledger->RegisterScope(Scope("tenant", "db", agents::HierarchicalMemoryBudgetScopeKind::kTenant, 800)).ok,
          "MMCH-017 tenant scope registration failed");
  Require(ledger->RegisterScope(Scope("user", "tenant", agents::HierarchicalMemoryBudgetScopeKind::kUser, 700)).ok,
          "MMCH-017 user scope registration failed");
  Require(ledger->RegisterScope(Scope("role", "user", agents::HierarchicalMemoryBudgetScopeKind::kRole, 600)).ok,
          "MMCH-017 role scope registration failed");
  Require(ledger->RegisterScope(Scope("session", "role", agents::HierarchicalMemoryBudgetScopeKind::kSession, 500)).ok,
          "MMCH-017 session scope registration failed");
  Require(ledger->RegisterScope(Scope("txn", "session", agents::HierarchicalMemoryBudgetScopeKind::kTransaction, 400)).ok,
          "MMCH-017 transaction scope registration failed");
  Require(ledger->RegisterScope(Scope("stmt", "txn", agents::HierarchicalMemoryBudgetScopeKind::kStatement, 300)).ok,
          "MMCH-017 statement scope registration failed");
  Require(ledger->RegisterScope(Scope("query", "stmt", agents::HierarchicalMemoryBudgetScopeKind::kQuery, 250)).ok,
          "MMCH-017 query scope registration failed");
  Require(ledger->RegisterScope(Scope("operator-sort", "query", agents::HierarchicalMemoryBudgetScopeKind::kOperator, 120)).ok,
          "MMCH-017 operator scope registration failed");
  Require(ledger->RegisterScope(Scope("background-cleanup", "db", agents::HierarchicalMemoryBudgetScopeKind::kBackground, 100)).ok,
          "MMCH-017 background scope registration failed");
}

const agents::HierarchicalMemoryBudgetScopeSnapshot* FindScope(
    const std::vector<agents::HierarchicalMemoryBudgetScopeSnapshot>& snapshots,
    std::string_view scope_id) {
  for (const auto& snapshot : snapshots) {
    if (snapshot.scope_id == scope_id) {
      return &snapshot;
    }
  }
  return nullptr;
}

agents::HierarchicalMemoryBudgetReserveRequest ReserveRequest(
    std::string_view operation_id,
    std::string_view owner_scope,
    std::string_view leaf_scope,
    std::uint64_t bytes) {
  agents::HierarchicalMemoryBudgetReserveRequest request;
  request.operation_id = std::string(operation_id);
  request.owner_scope = std::string(owner_scope);
  request.leaf_scope_id = std::string(leaf_scope);
  request.bytes = bytes;
  return request;
}

void RequireAuthorityEvidence(const std::vector<std::string>& evidence) {
  Require(EvidenceHas(evidence, "MMCH_HIERARCHICAL_MEMORY_BUDGETS"),
          "MMCH-017 hierarchical budget marker missing");
  Require(EvidenceHas(
              evidence,
              "hierarchical_memory.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority"),
          "MMCH-017 authority evidence missing");
}

void ParentChildDebitRelease() {
  agents::HierarchicalMemoryBudgetLedger ledger("mmch017-budget");
  RegisterCoreTree(&ledger);

  auto sort = ledger.Reserve(
      ReserveRequest("sort-grant", "session-A", "operator-sort", 80));
  Require(sort.ok && sort.reservation_created,
          "MMCH-017 sort reservation failed");
  Require(sort.reservation.debited_scope_chain.size() == 9,
          "MMCH-017 sort reservation did not debit every ancestor");
  RequireAuthorityEvidence(sort.evidence);
  for (const auto* scope_id :
       {"operator-sort", "query", "stmt", "txn", "session", "role", "user",
        "tenant", "db"}) {
    const auto* scope = FindScope(sort.snapshots, scope_id);
    Require(scope != nullptr && scope->current_bytes == 80,
            "MMCH-017 parent-child debit did not reach expected scope");
  }

  auto refused = ledger.Reserve(
      ReserveRequest("sort-over", "session-A", "operator-sort", 50));
  Require(!refused.ok && refused.fail_closed,
          "MMCH-017 leaf over-budget reservation was accepted");
  Require(refused.diagnostic_code ==
              "SB_RESOURCE_GOVERNANCE.HIERARCHICAL_BUDGET_EXCEEDED",
          "MMCH-017 over-budget diagnostic changed");
  Require(refused.refused_scope_id == "operator-sort",
          "MMCH-017 over-budget refused wrong scope");
  RequireAuthorityEvidence(refused.evidence);

  auto released = ledger.Release(sort.reservation.token_id);
  Require(released.ok && released.released,
          "MMCH-017 reservation release failed");
  for (const auto* scope_id :
       {"operator-sort", "query", "stmt", "txn", "session", "role", "user",
        "tenant", "db"}) {
    const auto* scope = FindScope(released.snapshots, scope_id);
    Require(scope != nullptr && scope->current_bytes == 0,
            "MMCH-017 release did not debit expected scope");
  }
}

void BackgroundAndOwnerCleanup() {
  agents::HierarchicalMemoryBudgetLedger ledger("mmch017-background");
  RegisterCoreTree(&ledger);

  auto foreground = ledger.Reserve(
      ReserveRequest("query-grant", "session-B", "operator-sort", 60));
  auto background = ledger.Reserve(
      ReserveRequest("background-grant", "agent-cleanup", "background-cleanup", 90));
  Require(foreground.ok && background.ok,
          "MMCH-017 foreground/background setup failed");

  const auto* db = FindScope(background.snapshots, "db");
  Require(db != nullptr && db->current_bytes == 150,
          "MMCH-017 database parent did not include foreground and background");
  const auto* background_scope = FindScope(background.snapshots, "background-cleanup");
  Require(background_scope != nullptr && background_scope->current_bytes == 90,
          "MMCH-017 background scope current bytes mismatch");

  auto background_refused = ledger.Reserve(
      ReserveRequest("background-over", "agent-cleanup", "background-cleanup", 20));
  Require(!background_refused.ok &&
              background_refused.refused_scope_id == "background-cleanup",
          "MMCH-017 background over-budget reservation was not refused");

  auto cleanup = ledger.ReleaseOwnerReservations("agent-cleanup");
  Require(cleanup.ok && cleanup.released,
          "MMCH-017 owner cleanup did not release background reservation");
  const auto* cleanup_background = FindScope(cleanup.snapshots, "background-cleanup");
  Require(cleanup_background != nullptr && cleanup_background->current_bytes == 0,
          "MMCH-017 owner cleanup left background bytes");
  const auto* cleanup_db = FindScope(cleanup.snapshots, "db");
  Require(cleanup_db != nullptr && cleanup_db->current_bytes == 60,
          "MMCH-017 owner cleanup disturbed foreground reservation");

  auto foreground_release = ledger.Release(foreground.reservation.token_id);
  Require(foreground_release.ok,
          "MMCH-017 foreground cleanup failed");
}

void InvalidHierarchyFailsClosed() {
  agents::HierarchicalMemoryBudgetLedger ledger("mmch017-invalid");
  auto invalid = ledger.RegisterScope(
      Scope("orphan", "missing-parent", agents::HierarchicalMemoryBudgetScopeKind::kSession, 10));
  Require(!invalid.ok,
          "MMCH-017 orphan scope registration was accepted");

  auto invalid_request = ledger.Reserve(
      ReserveRequest("", "owner", "missing", 10));
  Require(!invalid_request.ok && invalid_request.fail_closed,
          "MMCH-017 invalid reservation request was accepted");
  Require(invalid_request.diagnostic_code ==
              "SB_RESOURCE_GOVERNANCE.HIERARCHICAL_REQUEST_INVALID",
          "MMCH-017 invalid request diagnostic changed");
  RequireAuthorityEvidence(invalid_request.evidence);
}

}  // namespace

int main() {
  std::cout << "MMCH-017 authority_note=hierarchical_memory_budget_evidence_only;"
               "not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority"
            << '\n';
  ParentChildDebitRelease();
  BackgroundAndOwnerCleanup();
  InvalidHierarchyFailsClosed();
  return EXIT_SUCCESS;
}
