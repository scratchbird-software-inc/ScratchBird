// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "hierarchical_memory_budget_ledger.hpp"
#include "memory.hpp"
#include "query_memory_arena.hpp"
#include "temp_workspace_lifecycle.hpp"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

namespace memory = scratchbird::core::memory;

bool Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool ExpectEq(memory::u64 actual, memory::u64 expected, const char* message) {
  if (actual != expected) {
    std::cerr << message << " actual=" << actual << " expected=" << expected << '\n';
    return false;
  }
  return true;
}

memory::AllocationPolicy AllocatorPolicy(memory::u64 limit = 1024 * 1024) {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_query_memory_reservation_gate";
  policy.hard_limit_bytes = limit;
  policy.soft_limit_bytes = limit;
  policy.per_context_limit_bytes = limit;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

memory::QueryMemoryContext Context(const char* suffix) {
  memory::QueryMemoryContext context;
  context.engine_id = "public-engine";
  context.database_id = "public-db";
  context.session_id = std::string("session-") + suffix;
  context.transaction_id = std::string("transaction-") + suffix;
  context.statement_id = std::string("statement-") + suffix;
  context.query_id = std::string("query-") + suffix;
  context.operation_id = std::string("operator-") + suffix;
  return context;
}

memory::QueryMemoryArenaLimits Limits() {
  memory::QueryMemoryArenaLimits limits;
  limits.hard_limit_bytes = 4096;
  limits.soft_limit_bytes = 4096;
  limits.family_limit_bytes = 4096;
  limits.query_limit_bytes = 4096;
  limits.spill_limit_bytes = 4096;
  limits.allow_spill = false;
  limits.require_hierarchical_reservation = true;
  return limits;
}

bool HeapGrantReleaseReconcilesLedger() {
  memory::BoundedAllocator allocator(AllocatorPolicy());
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::UnifiedMemorySpillBudgetLedger unified("heap-release", 4096);
  memory::QueryMemoryArena arena(Context("heap"), Limits(), &allocator, nullptr, &unified, &ledger);

  memory::QueryMemoryGrantRequest request;
  request.family = memory::QueryMemoryFamily::relational;
  request.bytes = 512;
  request.purpose = "heap_release";
  const auto grant = arena.Grant(request);
  bool ok = true;
  ok = Expect(grant.ok(), "heap grant should succeed") && ok;
  ok = Expect(grant.grant.has_value(), "heap grant should return a grant token") && ok;

  const auto after_grant = ledger.Snapshot();
  ok = ExpectEq(after_grant.current_bytes, 512, "ledger current bytes after heap grant") && ok;
  ok = ExpectEq(after_grant.reserved_bytes, 0, "ledger reserved bytes after heap commit") && ok;
  ok = ExpectEq(after_grant.active_allocation_count, 1, "ledger active allocations after heap grant") && ok;
  ok = ExpectEq(arena.Snapshot().current_bytes, 512, "arena current bytes after heap grant") && ok;

  const auto release = arena.Release(grant.grant->grant_id);
  ok = Expect(release.ok(), "heap grant release should succeed") && ok;
  const auto after_release = ledger.Snapshot();
  ok = ExpectEq(after_release.current_bytes, 0, "ledger current bytes after heap release") && ok;
  ok = ExpectEq(after_release.active_allocation_count, 0, "ledger active allocations after heap release") && ok;
  ok = ExpectEq(arena.Snapshot().current_bytes, 0, "arena current bytes after heap release") && ok;
  ok = ExpectEq(allocator.Snapshot().current_bytes, 0, "allocator current bytes after heap release") && ok;
  ok = ExpectEq(unified.Snapshot().total_bytes, 0, "unified budget bytes after heap release") && ok;
  return ok;
}

bool CancelRollsBackLedgerReservations() {
  memory::BoundedAllocator allocator(AllocatorPolicy());
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::UnifiedMemorySpillBudgetLedger unified("cancel", 4096);
  memory::QueryMemoryArena arena(Context("cancel"), Limits(), &allocator, nullptr, &unified, &ledger);

  memory::QueryMemoryGrantRequest first;
  first.family = memory::QueryMemoryFamily::relational;
  first.bytes = 256;
  first.purpose = "cancel_first";
  memory::QueryMemoryGrantRequest second = first;
  second.family = memory::QueryMemoryFamily::document;
  second.bytes = 128;
  second.purpose = "cancel_second";

  const auto grant_first = arena.Grant(first);
  const auto grant_second = arena.Grant(second);
  bool ok = true;
  ok = Expect(grant_first.ok(), "first cancel grant should succeed") && ok;
  ok = Expect(grant_second.ok(), "second cancel grant should succeed") && ok;
  ok = ExpectEq(ledger.Snapshot().current_bytes, 384, "ledger current bytes before cancel") && ok;

  const auto cancel = arena.Cancel("public_query_memory_reservation_gate");
  ok = Expect(cancel.ok(), "arena cancel should succeed") && ok;
  const auto after_cancel = ledger.Snapshot();
  ok = ExpectEq(after_cancel.current_bytes, 0, "ledger current bytes after cancel") && ok;
  ok = ExpectEq(after_cancel.active_allocation_count, 0, "ledger active allocations after cancel") && ok;
  ok = ExpectEq(arena.Snapshot().active_grant_count, 0, "arena active grants after cancel") && ok;
  ok = ExpectEq(allocator.Snapshot().current_bytes, 0, "allocator bytes after cancel") && ok;
  ok = ExpectEq(unified.Snapshot().total_bytes, 0, "unified budget bytes after cancel") && ok;
  return ok;
}

bool SpillReleaseReconcilesLedger(const std::filesystem::path& temp_root) {
  std::filesystem::remove_all(temp_root);

  memory::BoundedAllocator allocator(AllocatorPolicy());
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::UnifiedMemorySpillBudgetLedger unified("spill", 4096);

  memory::TempWorkspacePolicy temp_policy;
  temp_policy.policy_name = "public_query_memory_reservation_spill";
  temp_policy.root_path = temp_root;
  temp_policy.filespace_quota_bytes = 4096;
  temp_policy.session_quota_bytes = 4096;
  temp_policy.transaction_quota_bytes = 4096;
  temp_policy.statement_quota_bytes = 4096;
  temp_policy.operation_quota_bytes = 4096;
  memory::TempWorkspaceLifecycleManager temp_workspace(temp_policy);

  auto limits = Limits();
  limits.soft_limit_bytes = 128;
  limits.allow_spill = true;
  memory::QueryMemoryArena arena(Context("spill"), limits, &allocator, &temp_workspace, &unified, &ledger);

  memory::QueryMemoryGrantRequest request;
  request.family = memory::QueryMemoryFamily::relational;
  request.bytes = 512;
  request.spillable = true;
  request.purpose = "spill_release";

  const auto grant = arena.Grant(request);
  bool ok = true;
  ok = Expect(grant.ok(), "spill grant should succeed") && ok;
  ok = Expect(grant.grant.has_value(), "spill grant should return a grant token") && ok;
  ok = Expect(grant.grant->spilled, "grant should be represented as spill") && ok;
  ok = ExpectEq(arena.Snapshot().current_bytes, 0, "arena heap bytes after spill grant") && ok;
  ok = ExpectEq(arena.Snapshot().spilled_bytes, 512, "arena spilled bytes after spill grant") && ok;
  ok = ExpectEq(ledger.Snapshot().current_bytes, 512, "ledger current bytes after spill grant") && ok;

  const auto release = arena.Release(grant.grant->grant_id);
  ok = Expect(release.ok(), "spill grant release should succeed") && ok;
  ok = ExpectEq(ledger.Snapshot().current_bytes, 0, "ledger current bytes after spill release") && ok;
  ok = ExpectEq(ledger.Snapshot().active_allocation_count, 0, "ledger active allocations after spill release") && ok;
  ok = ExpectEq(unified.Snapshot().total_bytes, 0, "unified budget bytes after spill release") && ok;

  std::filesystem::remove_all(temp_root);
  return ok;
}

bool AllocatorFailureRollsBackLedger() {
  memory::BoundedAllocator allocator(AllocatorPolicy(64));
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::UnifiedMemorySpillBudgetLedger unified("allocator-failure", 4096);
  memory::QueryMemoryArena arena(Context("allocator_failure"), Limits(), &allocator, nullptr, &unified, &ledger);

  memory::QueryMemoryGrantRequest request;
  request.family = memory::QueryMemoryFamily::relational;
  request.bytes = 256;
  request.purpose = "allocator_failure";
  const auto grant = arena.Grant(request);
  bool ok = true;
  ok = Expect(!grant.ok(), "allocator failure grant should fail closed") && ok;
  const auto snapshot = ledger.Snapshot();
  ok = ExpectEq(snapshot.reserved_bytes, 0, "ledger reserved bytes after allocator rollback") && ok;
  ok = ExpectEq(snapshot.current_bytes, 0, "ledger current bytes after allocator rollback") && ok;
  ok = ExpectEq(snapshot.active_allocation_count, 0, "ledger active allocations after allocator rollback") && ok;
  ok = ExpectEq(unified.Snapshot().total_bytes, 0, "unified bytes after allocator rollback") && ok;
  ok = ExpectEq(allocator.Snapshot().current_bytes, 0, "allocator bytes after failed grant") && ok;
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: public_query_memory_reservation_gate <temp-root>\n";
    return EXIT_FAILURE;
  }

  bool ok = true;
  ok = HeapGrantReleaseReconcilesLedger() && ok;
  ok = CancelRollsBackLedgerReservations() && ok;
  ok = SpillReleaseReconcilesLedger(argv[1]) && ok;
  ok = AllocatorFailureRollsBackLedger() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
