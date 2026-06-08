// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-013 focused validation for typed slab pools and size-class allocators.
#include "hierarchical_memory_budget_ledger.hpp"
#include "typed_slab_executor_work_area.hpp"
#include "typed_slab_index_work_area.hpp"
#include "typed_slab_planner_work_area.hpp"
#include "typed_slab_pool.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace memory = scratchbird::core::memory;
namespace executor = scratchbird::engine::executor;
namespace index = scratchbird::core::index;
namespace optimizer = scratchbird::engine::optimizer;

using scratchbird::core::platform::u64;

struct DiagnosticRecordWork {
  u64 diagnostic_id = 0;
  std::array<char, 96> payload{};
};

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bool Contains(const std::vector<std::string>& evidence, std::string_view needle) {
  for (const auto& entry : evidence) {
    if (entry.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

memory::HierarchicalMemoryBudgetProvenance Provenance() {
  memory::HierarchicalMemoryBudgetProvenance provenance;
  provenance.source = memory::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  provenance.source_label = "ceic_013_typed_slab_pool_gate_runtime_policy";
  provenance.engine_mga_authoritative = true;
  provenance.memory_evidence_only = true;
  return provenance;
}

void SetBudget(memory::HierarchicalMemoryBudgetLedger* ledger,
               memory::HierarchicalMemoryScopeKind kind,
               const std::string& scope_id,
               u64 hard_limit) {
  memory::HierarchicalMemoryBudget budget;
  budget.scope = {kind, scope_id};
  budget.hard_limit_bytes = hard_limit;
  budget.provenance = Provenance();
  Require(ledger->SetBudget(std::move(budget)).ok(), "CEIC-013 budget setup failed");
}

std::vector<memory::HierarchicalMemoryScopeRef> ScopeChain(
    memory::HierarchicalMemoryScopeKind leaf_kind,
    const std::string& leaf_id) {
  return {{memory::HierarchicalMemoryScopeKind::process, "ceic-013-process"},
          {memory::HierarchicalMemoryScopeKind::database, "ceic-013-database"},
          {memory::HierarchicalMemoryScopeKind::session, "ceic-013-session"},
          {memory::HierarchicalMemoryScopeKind::transaction, "ceic-013-transaction"},
          {memory::HierarchicalMemoryScopeKind::statement, "ceic-013-statement"},
          {memory::HierarchicalMemoryScopeKind::query, "ceic-013-query"},
          {leaf_kind, leaf_id}};
}

memory::SizeClassAllocatorRequest PoolRequest(
    memory::HierarchicalMemoryBudgetLedger* ledger,
    memory::MemoryManager* manager,
    memory::TypedSlabPoolObjectKind kind,
    memory::HierarchicalMemoryScopeKind leaf_kind,
    std::string route,
    u64 reservation_bytes) {
  SetBudget(ledger, leaf_kind, route, reservation_bytes * 2);

  memory::SizeClassAllocatorRequest request;
  request.reservation_ledger = ledger;
  request.memory_manager = manager;
  request.scope_chain = ScopeChain(leaf_kind, route);
  request.category = memory::TypedSlabPoolDefaultCategory(kind);
  request.memory_class = "ceic_013." + std::string(memory::TypedSlabPoolObjectKindName(kind));
  request.reservation_bytes = reservation_bytes;
  request.owner_id = route;
  request.route_label = route;
  request.operation_id = route + ".operation";
  request.purpose = route + ".typed_slab";
  request.object_kind = kind;
  request.size_classes = {{64, 4}, {128, 4}, {256, 2}, {512, 2}};
  request.provenance = Provenance();
  request.authority.engine_mga_authoritative = true;
  request.authority.transaction_inventory_authoritative = true;
  request.authority.security_or_policy_checked = true;
  return request;
}

std::unique_ptr<memory::SizeClassAllocator> MakePool(
    memory::HierarchicalMemoryBudgetLedger* ledger,
    memory::MemoryManager* manager,
    memory::TypedSlabPoolObjectKind kind,
    memory::HierarchicalMemoryScopeKind leaf_kind,
    const std::string& route,
    u64 reservation_bytes = 16384) {
  auto acquired = memory::CreateSizeClassAllocator(
      PoolRequest(ledger, manager, kind, leaf_kind, route, reservation_bytes));
  Require(acquired.ok(), "CEIC-013 pool acquisition failed for " + route);
  Require(Contains(acquired.evidence, "typed_slab_pool.reservation_first=true"),
          "CEIC-013 missing reservation-first evidence");
  return std::move(acquired.allocator);
}

void ValidateSizeClassesTypedReuseAndMetrics(memory::HierarchicalMemoryBudgetLedger* ledger,
                                             memory::MemoryManager* manager) {
  auto pool = MakePool(ledger,
                       manager,
                       memory::TypedSlabPoolObjectKind::executor_frame,
                       memory::HierarchicalMemoryScopeKind::operator_scope,
                       "ceic-013-size-class");

  auto small = pool->Allocate({24, 0, "small"});
  auto medium = pool->Allocate({96, 0, "medium"});
  auto large = pool->Allocate({300, 0, "large"});
  Require(small.ok() && small.size_class_bytes == 64, "CEIC-013 64-byte class failed");
  Require(medium.ok() && medium.size_class_bytes == 128, "CEIC-013 128-byte class failed");
  Require(large.ok() && large.size_class_bytes == 512, "CEIC-013 512-byte class failed");

  std::memset(small.pointer, 0x11, small.requested_bytes);
  Require(pool->Free(small.pointer).ok(), "CEIC-013 small free failed");
  auto reused = pool->Allocate({24, 0, "small-reuse"});
  Require(reused.ok() && reused.reused, "CEIC-013 slot reuse was not observed");
  Require(static_cast<unsigned char*>(reused.pointer)[0] == 0xa5,
          "CEIC-013 allocated poison pattern missing on reused slot");

  const auto snapshot = pool->Snapshot();
  Require(snapshot.total_slots >= 8, "CEIC-013 total slot snapshot missing");
  Require(snapshot.active_slots == 3, "CEIC-013 active slot accounting mismatch");
  Require(snapshot.reuse_count >= 1, "CEIC-013 reuse count missing");
  Require(snapshot.occupancy_basis_points > 0, "CEIC-013 occupancy not reported");
  Require(snapshot.fragmentation_basis_points > 0, "CEIC-013 fragmentation not reported");
  Require(snapshot.allocation_latency_p50_ns > 0 ||
              snapshot.allocation_latency_p95_ns > 0 ||
              snapshot.allocation_latency_p99_ns > 0,
          "CEIC-013 latency percentile fields missing");
  Require(snapshot.owner_id == "ceic-013-size-class",
          "CEIC-013 owner evidence missing from snapshot");
  Require(snapshot.category == memory::MemoryCategory::executor_query_reserved,
          "CEIC-013 category evidence mismatch");

  Require(pool->Release().ok(), "CEIC-013 pool release failed");
}

void ValidateCorruptionRefusal(memory::HierarchicalMemoryBudgetLedger* ledger,
                               memory::MemoryManager* manager) {
  auto pool = MakePool(ledger,
                       manager,
                       memory::TypedSlabPoolObjectKind::row_batch,
                       memory::HierarchicalMemoryScopeKind::operator_scope,
                       "ceic-013-corruption");
  auto allocated = pool->Allocate({32, 0, "corrupt-tail"});
  Require(allocated.ok(), "CEIC-013 corruption allocation failed");
  auto* payload = static_cast<unsigned char*>(allocated.pointer);
  payload[allocated.size_class_bytes] ^= 0x7u;
  auto freed = pool->Free(allocated.pointer);
  Require(!freed.ok() && freed.corruption_refused,
          "CEIC-013 corrupted tail canary was not refused");
  const auto snapshot = pool->Snapshot();
  Require(snapshot.corruption_refusal_count == 1,
          "CEIC-013 corruption refusal count missing");
  Require(snapshot.quarantined_slots == 1,
          "CEIC-013 corrupted slot was not quarantined");
  Require(pool->Reset().ok(), "CEIC-013 reset after corruption failed");
  Require(pool->Release().ok(), "CEIC-013 corruption pool release failed");
}

void ValidateBoundedReservationRefusal(memory::HierarchicalMemoryBudgetLedger* ledger,
                                       memory::MemoryManager* manager) {
  auto pool = MakePool(ledger,
                       manager,
                       memory::TypedSlabPoolObjectKind::row_locator,
                       memory::HierarchicalMemoryScopeKind::operator_scope,
                       "ceic-013-bounded-refusal",
                       512);
  auto denied = pool->Allocate({24, 0, "must-deny"});
  Require(!denied.ok(), "CEIC-013 allocation exceeded bounded reservation");
  Require(denied.status.code == scratchbird::core::platform::StatusCode::memory_limit_exceeded,
          "CEIC-013 bounded refusal did not report limit exceeded");
  Require(pool->Release().ok(), "CEIC-013 bounded refusal pool release failed");
}

void ValidateAdapters(memory::HierarchicalMemoryBudgetLedger* ledger,
                      memory::MemoryManager* manager) {
  auto executor_pool = MakePool(ledger,
                                manager,
                                memory::TypedSlabPoolObjectKind::executor_frame,
                                memory::HierarchicalMemoryScopeKind::operator_scope,
                                "ceic-013-executor-adapter");
  executor::ExecutorTypedSlabWorkAreaRequest executor_request;
  executor_request.allocator = executor_pool.get();
  executor_request.route_label = "ceic-013-executor-adapter";
  executor_request.frame_count = 4;
  auto executor_result = executor::BuildExecutorTypedSlabWorkArea(executor_request);
  if (!executor_result.ok()) {
    std::cerr << "executor diagnostic: "
              << executor_result.diagnostic.diagnostic_code << '\n';
    for (const auto& entry : executor_result.evidence) {
      std::cerr << "  " << entry << '\n';
    }
  }
  Require(executor_result.ok(), "CEIC-013 executor adapter failed");
  Require(Contains(executor_result.evidence, "executor_frame=true") &&
              Contains(executor_result.evidence, "row_batch=true") &&
              Contains(executor_result.evidence, "row_locator=true") &&
              Contains(executor_result.evidence, "vector_scratch=true"),
          "CEIC-013 executor adapter category evidence missing");
  Require(executor_result.reuse_count >= 1, "CEIC-013 executor adapter reuse missing");
  Require(executor_pool->Release().ok(), "CEIC-013 executor adapter pool release failed");

  auto planner_pool = MakePool(ledger,
                               manager,
                               memory::TypedSlabPoolObjectKind::planner_node,
                               memory::HierarchicalMemoryScopeKind::query,
                               "ceic-013-planner-adapter");
  optimizer::PlannerTypedSlabWorkAreaRequest planner_request;
  planner_request.allocator = planner_pool.get();
  planner_request.route_label = "ceic-013-planner-adapter";
  planner_request.candidate_count = 3;
  auto planner_result = optimizer::BuildPlannerTypedSlabWorkArea(planner_request);
  if (!planner_result.ok()) {
    std::cerr << "planner diagnostic: "
              << planner_result.diagnostic.diagnostic_code << '\n';
  }
  Require(planner_result.ok(), "CEIC-013 planner adapter failed");
  Require(Contains(planner_result.evidence, "planner_node=true") &&
              Contains(planner_result.evidence, "candidate_chunk=true"),
          "CEIC-013 planner adapter evidence missing");
  Require(planner_pool->Release().ok(), "CEIC-013 planner adapter pool release failed");

  auto index_pool = MakePool(ledger,
                             manager,
                             memory::TypedSlabPoolObjectKind::index_cursor,
                             memory::HierarchicalMemoryScopeKind::operator_scope,
                             "ceic-013-index-adapter");
  index::IndexTypedSlabWorkAreaRequest index_request;
  index_request.allocator = index_pool.get();
  index_request.route_label = "ceic-013-index-adapter";
  index_request.cursor_count = 3;
  auto index_result = index::BuildIndexTypedSlabWorkArea(index_request);
  if (!index_result.ok()) {
    std::cerr << "index diagnostic: "
              << index_result.diagnostic.diagnostic_code << '\n';
  }
  Require(index_result.ok(), "CEIC-013 index adapter failed");
  Require(Contains(index_result.evidence, "index_cursor=true") &&
              Contains(index_result.evidence, "candidate_chunk=true"),
          "CEIC-013 index adapter evidence missing");
  Require(index_pool->Release().ok(), "CEIC-013 index adapter pool release failed");
}

void ValidateDiagnosticRecordCategory(memory::HierarchicalMemoryBudgetLedger* ledger,
                                      memory::MemoryManager* manager) {
  auto pool = MakePool(ledger,
                       manager,
                       memory::TypedSlabPoolObjectKind::diagnostic_record,
                       memory::HierarchicalMemoryScopeKind::background,
                       "ceic-013-diagnostic-record");
  memory::TypedSlabPool<DiagnosticRecordWork> diagnostics(
      pool.get(),
      memory::TypedSlabPoolObjectKind::diagnostic_record,
      "diagnostic_record");
  auto record = diagnostics.Make();
  Require(record.ok(), "CEIC-013 diagnostic record allocation failed");
  record.pointer->diagnostic_id = 13;
  const auto snapshot = pool->Snapshot();
  Require(snapshot.category == memory::MemoryCategory::diagnostics,
          "CEIC-013 diagnostic record category mismatch");
  Require(pool->Release().ok(), "CEIC-013 diagnostic pool release failed");
}

}  // namespace

int main() {
  memory::AllocationPolicy policy;
  policy.policy_name = "ceic_013_typed_slab_pool_gate";
  policy.byte_limit = 8ull * 1024ull * 1024ull;
  policy.hard_limit_bytes = 8ull * 1024ull * 1024ull;
  policy.track_allocations = true;

  memory::MemoryManager manager(policy);
  memory::HierarchicalMemoryBudgetLedger ledger;
  SetBudget(&ledger,
            memory::HierarchicalMemoryScopeKind::process,
            "ceic-013-process",
            8ull * 1024ull * 1024ull);

  ValidateSizeClassesTypedReuseAndMetrics(&ledger, &manager);
  ValidateCorruptionRefusal(&ledger, &manager);
  ValidateBoundedReservationRefusal(&ledger, &manager);
  ValidateAdapters(&ledger, &manager);
  ValidateDiagnosticRecordCategory(&ledger, &manager);

  const auto ledger_snapshot = ledger.Snapshot();
  Require(ledger_snapshot.current_bytes == 0, "CEIC-013 leaked ledger current bytes");
  Require(ledger_snapshot.active_allocation_count == 0,
          "CEIC-013 leaked active reservations");
  const auto memory_snapshot = manager.Snapshot();
  Require(memory_snapshot.current_bytes == 0, "CEIC-013 leaked manager bytes");
  std::cout << "CEIC-013 typed slab pool gate passed\n";
  return 0;
}
