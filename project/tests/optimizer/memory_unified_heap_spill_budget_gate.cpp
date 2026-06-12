// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"
#include "query_memory_arena.hpp"
#include "temp_workspace_lifecycle.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
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

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& row : evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireUnifiedAuthorityEvidence(const std::vector<std::string>& evidence) {
  Require(EvidenceHas(evidence, "MMCH_UNIFIED_MEMORY_SPILL_BUDGET"),
          "MMCH-031 unified budget evidence marker missing");
  Require(EvidenceHas(
              evidence,
              "unified_memory_spill.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority"),
          "MMCH-031 unified budget authority boundary evidence missing");
}

mem::AllocationPolicy AllocationPolicy() {
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "mmch031_allocator";
  policy.hard_limit_bytes = 1024 * 1024;
  policy.soft_limit_bytes = 1024 * 1024;
  policy.per_context_limit_bytes = 1024 * 1024;
  policy.reject_over_soft_limit = false;
  return policy;
}

mem::QueryMemoryContext Context() {
  mem::QueryMemoryContext context;
  context.query_id = "mmch031-query";
  context.statement_id = "mmch031-statement";
  context.session_id = "mmch031-session";
  context.transaction_id = "mmch031-transaction";
  context.database_id = "mmch031-database";
  context.engine_id = "mmch031-engine";
  context.operation_id = "mmch031-operation";
  context.engine_mga_authoritative = true;
  return context;
}

mem::QueryMemoryArenaLimits ArenaLimits() {
  mem::QueryMemoryArenaLimits limits;
  limits.hard_limit_bytes = 200;
  limits.soft_limit_bytes = 80;
  limits.family_limit_bytes = 200;
  limits.query_limit_bytes = 200;
  limits.spill_limit_bytes = 200;
  limits.allow_spill = true;
  return limits;
}

mem::TempWorkspacePolicy TempPolicy(const std::filesystem::path& root) {
  mem::TempWorkspacePolicy policy;
  policy.policy_name = "mmch031_temp_workspace";
  policy.root_path = root;
  policy.filespace_quota_bytes = 200;
  policy.session_quota_bytes = 200;
  policy.transaction_quota_bytes = 200;
  policy.statement_quota_bytes = 200;
  policy.operation_quota_bytes = 200;
  policy.create_root_path = true;
  policy.cleanup_files_on_release = true;
  return policy;
}

mem::UnifiedMemorySpillBudgetRequest UnifiedRequest(
    std::string operation_id,
    mem::UnifiedMemorySpillBudgetKind kind,
    std::uint64_t bytes) {
  mem::UnifiedMemorySpillBudgetRequest request;
  request.operation_id = std::move(operation_id);
  request.owner_scope = "mmch031-query";
  request.kind = kind;
  request.bytes = bytes;
  return request;
}

mem::QueryMemoryGrantRequest GrantRequest(std::uint64_t bytes,
                                          bool spillable,
                                          std::string purpose) {
  mem::QueryMemoryGrantRequest request;
  request.family = mem::QueryMemoryFamily::relational;
  request.bytes = bytes;
  request.spillable = spillable;
  request.purpose = std::move(purpose);
  return request;
}

void DirectUnifiedLedger() {
  mem::UnifiedMemorySpillBudgetLedger ledger("mmch031-direct-ledger", 100);

  auto heap = ledger.Reserve(UnifiedRequest(
      "heap-grant",
      mem::UnifiedMemorySpillBudgetKind::heap,
      60));
  Require(heap.ok() && heap.reservation_created,
          "MMCH-031 direct heap reservation failed");
  Require(heap.snapshot.heap_bytes == 60 && heap.snapshot.spill_bytes == 0,
          "MMCH-031 direct heap accounting mismatch");
  RequireUnifiedAuthorityEvidence(heap.evidence);

  auto spill = ledger.Reserve(UnifiedRequest(
      "spill-grant",
      mem::UnifiedMemorySpillBudgetKind::spill,
      30));
  Require(spill.ok() && spill.reservation_created,
          "MMCH-031 direct spill reservation failed");
  Require(spill.snapshot.heap_bytes == 60 && spill.snapshot.spill_bytes == 30 &&
              spill.snapshot.total_bytes == 90,
          "MMCH-031 direct mixed heap/spill accounting mismatch");

  auto denied = ledger.Reserve(UnifiedRequest(
      "mixed-over-budget",
      mem::UnifiedMemorySpillBudgetKind::heap,
      20));
  Require(!denied.ok() && denied.fail_closed,
          "MMCH-031 unified budget accepted an over-budget mixed reservation");
  Require(denied.diagnostic.diagnostic_code ==
              "SB_UNIFIED_MEMORY_SPILL_BUDGET.LIMIT_EXCEEDED",
          "MMCH-031 unified budget denial diagnostic changed");
  Require(denied.snapshot.total_bytes == 90,
          "MMCH-031 denied reservation changed active accounting");
  RequireUnifiedAuthorityEvidence(denied.evidence);

  auto heap_release = ledger.Release(heap.reservation->reservation_id);
  Require(heap_release.ok() && heap_release.released,
          "MMCH-031 direct heap release failed");
  Require(heap_release.snapshot.heap_bytes == 0 &&
              heap_release.snapshot.spill_bytes == 30,
          "MMCH-031 heap release disturbed spill accounting");

  auto after_release = ledger.Reserve(UnifiedRequest(
      "heap-after-release",
      mem::UnifiedMemorySpillBudgetKind::heap,
      20));
  Require(after_release.ok() && after_release.reservation_created,
          "MMCH-031 reserve after release failed");

  auto owner_cleanup = ledger.ReleaseOwnerReservations("mmch031-query");
  Require(owner_cleanup.ok() && owner_cleanup.released,
          "MMCH-031 owner cleanup failed");
  Require(owner_cleanup.snapshot.total_bytes == 0 &&
              owner_cleanup.snapshot.active_reservation_count == 0,
          "MMCH-031 owner cleanup leaked unified reservations");
}

void QueryArenaUnifiedBudget() {
  const auto root =
      std::filesystem::temp_directory_path() / "sb_mmch031_unified_budget";
  std::filesystem::remove_all(root);

  mem::BoundedAllocator allocator(AllocationPolicy());
  mem::TempWorkspaceLifecycleManager temp(TempPolicy(root));
  mem::UnifiedMemorySpillBudgetLedger ledger("mmch031-arena-ledger", 100);
  mem::QueryMemoryArena arena(Context(), ArenaLimits(), &allocator, &temp, &ledger);

  auto heap60 = arena.Grant(GrantRequest(60, false, "heap60"));
  Require(heap60.ok() && heap60.grant.has_value() &&
              !heap60.grant->unified_budget_reservation_id.empty(),
          "MMCH-031 arena heap grant did not reserve unified budget");
  Require(heap60.counters.current_bytes == 60,
          "MMCH-031 arena heap current bytes mismatch");
  RequireUnifiedAuthorityEvidence(heap60.evidence);

  auto spill30 = arena.Grant(GrantRequest(30, true, "spill30"));
  Require(spill30.ok() && spill30.grant.has_value() &&
              spill30.grant->spilled &&
              !spill30.grant->unified_budget_reservation_id.empty(),
          "MMCH-031 arena spill grant did not reserve unified budget");
  Require(temp.Snapshot().active_bytes == 30,
          "MMCH-031 temp workspace did not reserve spill bytes");
  Require(ledger.Snapshot().heap_bytes == 60 &&
              ledger.Snapshot().spill_bytes == 30,
          "MMCH-031 arena unified ledger did not account heap and spill");
  RequireUnifiedAuthorityEvidence(spill30.evidence);

  auto denied20 = arena.Grant(GrantRequest(20, false, "heap20-denied"));
  Require(!denied20.ok() && denied20.fail_closed,
          "MMCH-031 mixed heap/spill budget bypass was accepted");
  Require(denied20.diagnostic.diagnostic_code ==
              "SB_QUERY_MEMORY_ARENA.UNIFIED_BUDGET_DENIED",
          "MMCH-031 arena denial diagnostic changed");
  Require(ledger.Snapshot().total_bytes == 90,
          "MMCH-031 denied arena grant changed unified budget");
  RequireUnifiedAuthorityEvidence(denied20.evidence);

  auto released60 = arena.Release(heap60.grant->grant_id);
  Require(released60.ok(), "MMCH-031 arena heap release failed");
  Require(ledger.Snapshot().heap_bytes == 0 &&
              ledger.Snapshot().spill_bytes == 30,
          "MMCH-031 arena heap release did not release unified budget");
  RequireUnifiedAuthorityEvidence(released60.evidence);

  auto heap20 = arena.Grant(GrantRequest(20, false, "heap20-after-release"));
  Require(heap20.ok() && heap20.grant.has_value() &&
              !heap20.grant->unified_budget_reservation_id.empty(),
          "MMCH-031 heap grant after release failed");
  Require(ledger.Snapshot().heap_bytes == 20 &&
              ledger.Snapshot().spill_bytes == 30 &&
              ledger.Snapshot().total_bytes == 50,
          "MMCH-031 heap grant after release accounting mismatch");

  auto release_spill = arena.Release(spill30.grant->grant_id);
  Require(release_spill.ok(), "MMCH-031 arena spill release failed");
  Require(temp.Snapshot().active_bytes == 0,
          "MMCH-031 arena spill release leaked temp workspace bytes");
  auto release_heap20 = arena.Release(heap20.grant->grant_id);
  Require(release_heap20.ok(), "MMCH-031 arena final heap release failed");

  Require(ledger.Snapshot().total_bytes == 0 &&
              ledger.Snapshot().active_reservation_count == 0,
          "MMCH-031 arena leaked unified reservations");
  Require(arena.Snapshot().current_bytes == 0 &&
              arena.Snapshot().leak_count == 0,
          "MMCH-031 arena leaked query memory grants");
  Require(allocator.Snapshot().leak_candidate_count == 0,
          "MMCH-031 allocator leaked after unified budget test");
}

}  // namespace

int main() {
  std::cout << "MMCH-031 authority_note=unified_heap_spill_budget_evidence_only;"
               "not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority"
            << '\n';
  DirectUnifiedLedger();
  QueryArenaUnifiedBudget();
  return EXIT_SUCCESS;
}
