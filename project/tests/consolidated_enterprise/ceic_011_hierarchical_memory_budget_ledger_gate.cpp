// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-011 focused validation for HierarchicalMemoryBudgetLedger.
#include "hierarchical_memory_budget_ledger.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace mem = scratchbird::core::memory;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

mem::HierarchicalMemoryScopeRef Scope(mem::HierarchicalMemoryScopeKind kind,
                                      std::string id) {
  return {kind, std::move(id)};
}

mem::HierarchicalMemoryBudgetProvenance RuntimeProvenance(
    std::string label = "ceic_011_hierarchical_memory_budget_ledger_gate") {
  mem::HierarchicalMemoryBudgetProvenance provenance;
  provenance.source = mem::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  provenance.source_label = std::move(label);
  return provenance;
}

std::vector<mem::HierarchicalMemoryScopeRef> FullChain(std::string suffix = "a") {
  return {
      Scope(mem::HierarchicalMemoryScopeKind::process, "process-main"),
      Scope(mem::HierarchicalMemoryScopeKind::database, "database-main"),
      Scope(mem::HierarchicalMemoryScopeKind::tenant, "tenant-" + suffix),
      Scope(mem::HierarchicalMemoryScopeKind::session, "session-" + suffix),
      Scope(mem::HierarchicalMemoryScopeKind::transaction, "transaction-" + suffix),
      Scope(mem::HierarchicalMemoryScopeKind::statement, "statement-" + suffix),
      Scope(mem::HierarchicalMemoryScopeKind::query, "query-" + suffix),
      Scope(mem::HierarchicalMemoryScopeKind::operator_scope, "operator-hash-" + suffix),
      Scope(mem::HierarchicalMemoryScopeKind::page_cache, "page-cache-" + suffix),
      Scope(mem::HierarchicalMemoryScopeKind::background, "background-cleanup-" + suffix),
  };
}

mem::HierarchicalMemoryReservationRequest Request(std::vector<mem::HierarchicalMemoryScopeRef> chain,
                                                  mem::u64 bytes,
                                                  std::string owner = "owner-a") {
  mem::HierarchicalMemoryReservationRequest request;
  request.scope_chain = std::move(chain);
  request.category = mem::MemoryCategory::executor_query_reserved;
  request.memory_class = "query_scratch";
  request.requested_bytes = bytes;
  request.owner_id = std::move(owner);
  request.priority = 7;
  request.weight = 3;
  request.provenance = RuntimeProvenance();
  return request;
}

void SetBudget(mem::HierarchicalMemoryBudgetLedger* ledger,
               mem::HierarchicalMemoryScopeRef scope,
               mem::u64 hard,
               mem::u64 soft = 0) {
  mem::HierarchicalMemoryBudget budget;
  budget.scope = std::move(scope);
  budget.hard_limit_bytes = hard;
  budget.soft_limit_bytes = soft;
  budget.provenance = RuntimeProvenance();
  Require(ledger->SetBudget(std::move(budget)).ok(), "CEIC-011 SetBudget failed");
}

const mem::HierarchicalMemoryScopeSnapshot* FindScope(
    const mem::HierarchicalMemoryBudgetSnapshot& snapshot,
    mem::HierarchicalMemoryScopeKind kind,
    std::string_view scope_id) {
  for (const auto& scope : snapshot.scopes) {
    if (scope.kind == kind && scope.scope_id == scope_id) {
      return &scope;
    }
  }
  return nullptr;
}

const mem::HierarchicalMemoryClassSnapshot* FindClass(
    const mem::HierarchicalMemoryBudgetSnapshot& snapshot,
    mem::MemoryCategory category,
    std::string_view memory_class) {
  for (const auto& entry : snapshot.classes) {
    if (entry.category == category && entry.memory_class == memory_class) {
      return &entry;
    }
  }
  return nullptr;
}

std::string DiagnosticArgument(const mem::DiagnosticRecord& diagnostic,
                               std::string_view key) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == key) {
      return argument.value;
    }
  }
  return {};
}

std::string Serialize(const mem::HierarchicalMemoryBudgetSnapshot& snapshot) {
  std::ostringstream out;
  out << "reserved=" << snapshot.reserved_bytes
      << ";current=" << snapshot.current_bytes
      << ";peak=" << snapshot.peak_bytes
      << ";reservations=" << snapshot.reservation_count
      << ";commits=" << snapshot.commit_count
      << ";releases=" << snapshot.release_count
      << ";hard_refusals=" << snapshot.hard_limit_refusal_count
      << ";soft_recommendations=" << snapshot.soft_limit_recommendation_count
      << ";cancel_cleanup=" << snapshot.cancel_cleanup_count
      << ";owner_cleanup=" << snapshot.owner_cleanup_count
      << ";lease_cleanup=" << snapshot.lease_expiry_cleanup_count;
  for (const auto& scope : snapshot.scopes) {
    out << "|scope:" << mem::HierarchicalMemoryScopeKindName(scope.kind) << ':'
        << scope.scope_id << ':' << scope.reserved_bytes << ':'
        << scope.current_bytes << ':' << scope.peak_bytes << ':'
        << scope.reservation_count << ':' << scope.commit_count << ':'
        << scope.release_count << ':' << scope.cancel_cleanup_count << ':'
        << scope.owner_cleanup_count << ':' << scope.lease_expiry_cleanup_count;
  }
  for (const auto& entry : snapshot.classes) {
    out << "|class:" << mem::MemoryCategoryName(entry.category) << ':'
        << entry.memory_class << ':' << entry.reserved_bytes << ':'
        << entry.current_bytes << ':' << entry.peak_bytes << ':'
        << entry.reservation_count << ':' << entry.commit_count << ':'
        << entry.release_count;
  }
  return out.str();
}

void ReserveCommitReleaseAcrossFullChain() {
  mem::HierarchicalMemoryBudgetLedger ledger(16, 16);
  const auto chain = FullChain("nested");
  SetBudget(&ledger, chain.front(), 2048);
  SetBudget(&ledger, chain[2], 1024);

  const auto reservation = ledger.Reserve(Request(chain, 256));
  Require(reservation.ok(), "CEIC-011 reserve failed across nested chain");
  auto snapshot = ledger.Snapshot();
  Require(snapshot.reserved_bytes == 256, "CEIC-011 reserve bytes mismatch");
  Require(snapshot.active_reservation_count == 1, "CEIC-011 active reservation count mismatch");

  Require(ledger.Commit(reservation.token).ok(), "CEIC-011 commit failed");
  snapshot = ledger.Snapshot();
  Require(snapshot.current_bytes == 256, "CEIC-011 current bytes after commit mismatch");
  Require(snapshot.peak_bytes == 256, "CEIC-011 peak bytes after commit mismatch");
  Require(snapshot.reserved_bytes == 0, "CEIC-011 reserved bytes after commit mismatch");
  for (const auto& scope_ref : chain) {
    const auto* scope = FindScope(snapshot, scope_ref.kind, scope_ref.scope_id);
    Require(scope != nullptr, "CEIC-011 missing full-chain scope snapshot");
    Require(scope->current_bytes == 256, "CEIC-011 scope current bytes mismatch");
    Require(scope->peak_bytes == 256, "CEIC-011 scope peak bytes mismatch");
    Require(scope->active_allocation_count == 1, "CEIC-011 scope active allocation mismatch");
    Require(scope->priority_weight_total == 10,
            "CEIC-011 scope priority weight did not remain active after commit");
  }
  const auto* klass = FindClass(snapshot, mem::MemoryCategory::executor_query_reserved, "query_scratch");
  Require(klass != nullptr, "CEIC-011 class snapshot missing");
  Require(klass->current_bytes == 256, "CEIC-011 class current mismatch");

  Require(ledger.Release(reservation.token).ok(), "CEIC-011 release failed");
  snapshot = ledger.Snapshot();
  Require(snapshot.current_bytes == 0, "CEIC-011 current bytes leaked after release");
  Require(snapshot.peak_bytes == 256, "CEIC-011 peak bytes lost after release");
  Require(snapshot.release_count == 1, "CEIC-011 release count mismatch");
  for (const auto& scope_ref : chain) {
    const auto* scope = FindScope(snapshot, scope_ref.kind, scope_ref.scope_id);
    Require(scope != nullptr, "CEIC-011 missing full-chain scope after release");
    Require(scope->priority_weight_total == 0,
            "CEIC-011 scope priority weight leaked after release");
  }
}

void HardLimitRefusalAtParentAndChild() {
  mem::HierarchicalMemoryBudgetLedger parent_ledger(8, 8);
  auto chain = FullChain("parent-hard");
  SetBudget(&parent_ledger, chain.front(), 100);
  auto refused = parent_ledger.Reserve(Request(chain, 128));
  Require(!refused.ok(), "CEIC-011 parent hard limit did not refuse");
  Require(refused.diagnostic.diagnostic_code == "SB-MEMORY-BUDGET-HARD-LIMIT-REFUSED",
          "CEIC-011 parent hard diagnostic mismatch");
  Require(DiagnosticArgument(refused.diagnostic, "scope_kind") == "process",
          "CEIC-011 parent hard diagnostic scope mismatch");
  Require(parent_ledger.Snapshot().current_bytes == 0, "CEIC-011 parent refusal changed current bytes");
  Require(parent_ledger.Snapshot().reserved_bytes == 0, "CEIC-011 parent refusal committed reservation");

  mem::HierarchicalMemoryBudgetLedger child_ledger(8, 8);
  chain = FullChain("child-hard");
  SetBudget(&child_ledger, chain.front(), 1024);
  SetBudget(&child_ledger, chain[2], 64);
  refused = child_ledger.Reserve(Request(chain, 96));
  Require(!refused.ok(), "CEIC-011 child hard limit did not refuse");
  Require(DiagnosticArgument(refused.diagnostic, "scope_kind") == "tenant",
          "CEIC-011 child hard diagnostic scope mismatch");
  Require(DiagnosticArgument(refused.diagnostic, "reason") == "hard_limit_exceeded",
          "CEIC-011 child hard diagnostic reason mismatch");
}

void SoftLimitRecommendationsDoNotReserve() {
  mem::HierarchicalMemoryBudgetLedger ledger(8, 8);
  auto chain = FullChain("soft");
  SetBudget(&ledger, chain.front(), 1024, 128);

  auto spill_request = Request(chain, 256);
  spill_request.spillable = true;
  auto recommendation = ledger.Reserve(spill_request);
  Require(!recommendation.ok(), "CEIC-011 soft spill recommendation granted reservation");
  Require(recommendation.recommendation == mem::HierarchicalMemoryReservationRecommendation::spill,
          "CEIC-011 soft recommendation was not spill");
  Require(ledger.Snapshot().reserved_bytes == 0, "CEIC-011 spill recommendation changed reserved bytes");

  auto cancel_request = Request(chain, 256);
  cancel_request.cancelable = true;
  recommendation = ledger.Reserve(cancel_request);
  Require(recommendation.recommendation == mem::HierarchicalMemoryReservationRecommendation::cancel,
          "CEIC-011 soft recommendation was not cancel");

  auto degrade_request = Request(chain, 256);
  recommendation = ledger.Reserve(degrade_request);
  Require(recommendation.recommendation == mem::HierarchicalMemoryReservationRecommendation::degrade,
          "CEIC-011 soft recommendation was not degrade");
  Require(recommendation.diagnostic.diagnostic_code == "SB-MEMORY-BUDGET-SOFT-LIMIT-RECOMMENDATION",
          "CEIC-011 soft diagnostic mismatch");
  Require(ledger.Snapshot().soft_limit_recommendation_count == 3,
          "CEIC-011 soft recommendation count mismatch");
}

void CancelOwnerAndLeaseCleanup() {
  mem::HierarchicalMemoryBudgetLedger ledger(16, 16);
  auto cancel = ledger.Reserve(Request(FullChain("cancel"), 80, "owner-cancel"));
  Require(cancel.ok(), "CEIC-011 cancel reserve failed");
  Require(ledger.Commit(cancel.token).ok(), "CEIC-011 cancel commit failed");
  Require(ledger.Cancel(cancel.token).ok(), "CEIC-011 cancel cleanup failed");
  Require(ledger.Snapshot().current_bytes == 0, "CEIC-011 cancel cleanup leaked active bytes");
  Require(ledger.Snapshot().cancel_cleanup_count == 1, "CEIC-011 cancel cleanup count mismatch");

  auto owner_a = ledger.Reserve(Request(FullChain("owner-a"), 40, "owner-cleanup"));
  auto owner_b = ledger.Reserve(Request(FullChain("owner-b"), 60, "owner-cleanup"));
  auto owner_c = ledger.Reserve(Request(FullChain("owner-c"), 70, "owner-other"));
  Require(owner_a.ok() && owner_b.ok() && owner_c.ok(), "CEIC-011 owner cleanup setup failed");
  Require(ledger.Commit(owner_a.token).ok(), "CEIC-011 owner A commit failed");
  Require(ledger.Commit(owner_b.token).ok(), "CEIC-011 owner B commit failed");
  Require(ledger.Commit(owner_c.token).ok(), "CEIC-011 owner C commit failed");
  auto cleanup = ledger.CleanupOwner("owner-cleanup");
  Require(cleanup.ok(), "CEIC-011 owner cleanup failed");
  Require(cleanup.cleaned_reservation_count == 2, "CEIC-011 owner cleanup count mismatch");
  Require(cleanup.cleaned_bytes == 100, "CEIC-011 owner cleanup bytes mismatch");
  Require(ledger.Snapshot().current_bytes == 70, "CEIC-011 owner cleanup removed wrong bytes");
  Require(ledger.Release(owner_c.token).ok(), "CEIC-011 owner C release failed");

  auto old_lease_request = Request(FullChain("lease-old"), 30, "lease-old");
  old_lease_request.lease_expires_at_ms = 100;
  auto new_lease_request = Request(FullChain("lease-new"), 50, "lease-new");
  new_lease_request.lease_expires_at_ms = 300;
  auto old_lease = ledger.Reserve(old_lease_request);
  auto new_lease = ledger.Reserve(new_lease_request);
  Require(old_lease.ok() && new_lease.ok(), "CEIC-011 lease setup reserve failed");
  Require(ledger.Commit(old_lease.token).ok(), "CEIC-011 old lease commit failed");
  Require(ledger.Commit(new_lease.token).ok(), "CEIC-011 new lease commit failed");
  cleanup = ledger.CleanupExpiredLeases(200);
  Require(cleanup.ok(), "CEIC-011 lease expiry cleanup failed");
  Require(cleanup.cleaned_reservation_count == 1, "CEIC-011 lease cleanup count mismatch");
  Require(cleanup.cleaned_bytes == 30, "CEIC-011 lease cleanup bytes mismatch");
  Require(ledger.Snapshot().current_bytes == 50, "CEIC-011 lease cleanup removed wrong bytes");
  Require(ledger.Release(new_lease.token).ok(), "CEIC-011 new lease release failed");
  Require(ledger.Snapshot().current_bytes == 0, "CEIC-011 cleanup suite leaked bytes");
}

void PeakCurrentDoubleReleaseAndDeterministicSnapshot() {
  mem::HierarchicalMemoryBudgetLedger ledger(16, 16);
  auto first = ledger.Reserve(Request(FullChain("peak-a"), 100, "owner-peak"));
  auto second = ledger.Reserve(Request(FullChain("peak-b"), 250, "owner-peak"));
  Require(first.ok() && second.ok(), "CEIC-011 peak setup reserve failed");
  Require(ledger.Commit(first.token).ok(), "CEIC-011 peak first commit failed");
  Require(ledger.Release(first.token).ok(), "CEIC-011 peak first release failed");
  Require(ledger.Commit(second.token).ok(), "CEIC-011 peak second commit failed");
  Require(ledger.Release(second.token).ok(), "CEIC-011 peak second release failed");

  auto snapshot = ledger.Snapshot();
  Require(snapshot.current_bytes == 0, "CEIC-011 peak current leaked");
  Require(snapshot.peak_bytes == 250, "CEIC-011 peak did not track max active bytes");
  auto double_release = ledger.Release(second.token);
  Require(!double_release.ok(), "CEIC-011 double release was accepted");
  snapshot = ledger.Snapshot();
  Require(snapshot.failed_release_count == 1, "CEIC-011 double release failure count mismatch");
  Require(snapshot.current_bytes == 0 && snapshot.reserved_bytes == 0,
          "CEIC-011 double release changed counters");

  const auto serialized_a = Serialize(ledger.Snapshot());
  const auto serialized_b = Serialize(ledger.Snapshot());
  Require(serialized_a == serialized_b, "CEIC-011 snapshot output is not deterministic");
}

void AuthorityBoundaryEvidenceStrings() {
  mem::HierarchicalMemoryBudgetLedger ledger(4, 4);
  auto chain = FullChain("authority");
  SetBudget(&ledger, chain.front(), 128);
  auto refused = ledger.Reserve(Request(chain, 256));
  const auto authority = DiagnosticArgument(refused.diagnostic, "authority_scope");
  Require(authority.find("not_transaction_finality") != std::string::npos,
          "CEIC-011 authority string missing transaction finality boundary");
  Require(authority.find("visibility") != std::string::npos,
          "CEIC-011 authority string missing visibility boundary");
  Require(authority.find("recovery") != std::string::npos,
          "CEIC-011 authority string missing recovery boundary");
  Require(authority.find("parser") != std::string::npos,
          "CEIC-011 authority string missing parser boundary");
  Require(authority.find("donor") != std::string::npos,
          "CEIC-011 authority string missing donor boundary");
  Require(authority.find("benchmark") != std::string::npos,
          "CEIC-011 authority string missing benchmark boundary");
  Require(authority.find("cluster") != std::string::npos,
          "CEIC-011 authority string missing cluster boundary");
  Require(authority.find("authorization") != std::string::npos,
          "CEIC-011 authority string missing authorization boundary");
  Require(authority.find("optimizer_plan") != std::string::npos,
          "CEIC-011 authority string missing optimizer boundary");
  Require(authority.find("index_finality") != std::string::npos,
          "CEIC-011 authority string missing index boundary");
  Require(authority.find("agent_action") != std::string::npos,
          "CEIC-011 authority string missing agent boundary");
}

void UnsafeProvenanceFailsClosed() {
  mem::HierarchicalMemoryBudgetLedger ledger(4, 4);
  mem::HierarchicalMemoryBudget budget;
  budget.scope = Scope(mem::HierarchicalMemoryScopeKind::process, "process-unsafe");
  budget.hard_limit_bytes = 1024;
  budget.provenance = RuntimeProvenance();
  Require(ledger.SetBudget(budget).ok(), "CEIC-011 safe budget setup failed");

  auto request = Request({budget.scope}, 128, "owner-unsafe");
  request.provenance.source = mem::HierarchicalMemoryBudgetProvenanceSource::test_fixture;
  auto refused = ledger.Reserve(request);
  Require(!refused.ok(), "CEIC-011 unsafe request provenance was accepted");
  Require(refused.diagnostic.diagnostic_code == "SB-MEMORY-BUDGET-PROVENANCE-REFUSED",
          "CEIC-011 unsafe request provenance diagnostic mismatch");
  Require(DiagnosticArgument(refused.diagnostic, "reason") ==
              "runtime_policy_server_api_or_agent_runtime_source_required",
          "CEIC-011 unsafe request provenance reason mismatch");
  Require(ledger.Snapshot().reserved_bytes == 0,
          "CEIC-011 unsafe request provenance created a reservation");

  mem::HierarchicalMemoryBudget unsafe_budget;
  unsafe_budget.scope = Scope(mem::HierarchicalMemoryScopeKind::tenant, "tenant-unsafe");
  unsafe_budget.hard_limit_bytes = 1024;
  unsafe_budget.provenance = RuntimeProvenance();
  unsafe_budget.provenance.parser_authority = true;
  auto budget_status = ledger.SetBudget(unsafe_budget);
  Require(!budget_status.ok(), "CEIC-011 unsafe budget provenance was accepted");
  Require(budget_status.diagnostic.diagnostic_code == "SB-MEMORY-BUDGET-PROVENANCE-REFUSED",
          "CEIC-011 unsafe budget provenance diagnostic mismatch");
  Require(DiagnosticArgument(budget_status.diagnostic, "reason") ==
              "unsafe_authority_or_relaxed_provenance_refused",
          "CEIC-011 unsafe budget provenance reason mismatch");
}

void ConcurrentReservationsAcrossTenants() {
  mem::HierarchicalMemoryBudgetLedger ledger(32, 32);
  constexpr int kThreads = 8;
  constexpr int kIterations = 250;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    threads.emplace_back([thread_index, &ledger]() {
      const std::string suffix = "concurrent-" + std::to_string(thread_index);
      auto chain = FullChain(suffix);
      SetBudget(&ledger, chain[2], 1'000'000);
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        auto request = Request(chain, 32 + static_cast<mem::u64>(thread_index),
                               "owner-concurrent-" + std::to_string(thread_index));
        request.memory_class = iteration % 2 == 0 ? "query_scratch" : "operator_hash";
        const auto reservation = ledger.Reserve(request);
        Require(reservation.ok(), "CEIC-011 concurrent reserve failed");
        Require(ledger.Commit(reservation.token).ok(), "CEIC-011 concurrent commit failed");
        Require(ledger.Release(reservation.token).ok(), "CEIC-011 concurrent release failed");
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  const auto snapshot = ledger.Snapshot();
  Require(snapshot.current_bytes == 0, "CEIC-011 concurrent current bytes leaked");
  Require(snapshot.reserved_bytes == 0, "CEIC-011 concurrent reserved bytes leaked");
  Require(snapshot.reservation_count == static_cast<mem::u64>(kThreads * kIterations),
          "CEIC-011 concurrent reservation count mismatch");
  Require(snapshot.commit_count == static_cast<mem::u64>(kThreads * kIterations),
          "CEIC-011 concurrent commit count mismatch");
  Require(snapshot.release_count == static_cast<mem::u64>(kThreads * kIterations),
          "CEIC-011 concurrent release count mismatch");
}

}  // namespace

int main() {
  ReserveCommitReleaseAcrossFullChain();
  HardLimitRefusalAtParentAndChild();
  SoftLimitRecommendationsDoNotReserve();
  CancelOwnerAndLeaseCleanup();
  PeakCurrentDoubleReleaseAndDeterministicSnapshot();
  AuthorityBoundaryEvidenceStrings();
  UnsafeProvenanceFailsClosed();
  ConcurrentReservationsAcrossTenants();
  return EXIT_SUCCESS;
}
