// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "hierarchical_memory_budget_ledger.hpp"
#include "memory.hpp"
#include "operator_typed_arena_work_area.hpp"
#include "optimizer_typed_arena_work_area.hpp"
#include "query_memory_arena.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace memory = scratchbird::core::memory;
namespace opt = scratchbird::engine::optimizer;
using scratchbird::tests::FixtureUuid;

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

memory::AllocationPolicy Policy(memory::u64 bytes = 1024 * 1024) {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "public_query_bump_region_arena_gate";
  policy.hard_limit_bytes = bytes;
  policy.soft_limit_bytes = bytes;
  policy.per_context_limit_bytes = bytes;
  policy.page_buffer_pool_limit_bytes = bytes;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  policy.reject_over_soft_limit = false;
  return policy;
}

memory::MemoryTag Tag(std::string purpose) {
  memory::MemoryTag tag;
  tag.subsystem = scratchbird::core::platform::Subsystem::engine;
  tag.purpose = std::move(purpose);
  tag.category = memory::MemoryCategory::executor_query_reserved;
  tag.lifetime = memory::MemoryLifetime::arena;
  tag.binary_ownership[memory::MemoryBinaryScopeKind::owner] = FixtureUuid(0x200, 1).bytes;
  tag.binary_ownership[memory::MemoryBinaryScopeKind::context] = FixtureUuid(0x200, 2).bytes;
  tag.binary_ownership[memory::MemoryBinaryScopeKind::session] = FixtureUuid(0x200, 3).bytes;
  tag.binary_ownership[memory::MemoryBinaryScopeKind::transaction] = FixtureUuid(0x200, 4).bytes;
  tag.binary_ownership[memory::MemoryBinaryScopeKind::statement] = FixtureUuid(0x200, 5).bytes;
  tag.binary_ownership[memory::MemoryBinaryScopeKind::query] = FixtureUuid(0x200, 6).bytes;
  return tag;
}

memory::QueryMemoryContext QueryContext() {
  memory::QueryMemoryContext context;
  context.engine_id = FixtureUuid(0x201, 1);
  context.database_id = FixtureUuid(0x201, 2);
  context.session_id = FixtureUuid(0x201, 3);
  context.transaction_id = FixtureUuid(0x201, 4);
  context.statement_id = FixtureUuid(0x201, 5);
  context.query_id = FixtureUuid(0x201, 6);
  context.operation_id = FixtureUuid(0x201, 7);
  context.snapshot_boundary = FixtureUuid(0x201, 9);
  context.metadata_boundary = FixtureUuid(0x201, 10);
  context.resource_budget_reference = FixtureUuid(0x201, 11);
  return context;
}

memory::QueryMemoryArenaLimits QueryLimits() {
  memory::QueryMemoryArenaLimits limits;
  limits.hard_limit_bytes = 16 * 1024;
  limits.soft_limit_bytes = 16 * 1024;
  limits.family_limit_bytes = 16 * 1024;
  limits.query_limit_bytes = 16 * 1024;
  limits.spill_limit_bytes = 16 * 1024;
  limits.allow_spill = false;
  limits.require_hierarchical_reservation = true;
  return limits;
}

void RawArenaUsesChunkedBumpAllocationAndRejectsEscapedPointers() {
  memory::MemoryManager manager(Policy());
  const auto before = manager.Snapshot();
  auto arena = manager.CreateArena(Tag("raw_bump_region"));

  std::vector<void*> allocations;
  for (int i = 0; i < 64; ++i) {
    auto allocated = arena.Allocate(32, alignof(std::max_align_t));
    Require(allocated.ok(), "raw bump arena allocation failed");
    std::memset(allocated.pointer, i, allocated.bytes);
    allocations.push_back(allocated.pointer);
  }

  const auto after_allocations = manager.Snapshot();
  const auto chunk_count =
      after_allocations.allocation_count - before.allocation_count;
  Require(chunk_count > 0, "raw bump arena did not allocate a backing chunk");
  Require(chunk_count < allocations.size(),
          "raw bump arena allocated one backing block per request");
  Require(after_allocations.active_allocation_count ==
              before.active_allocation_count + chunk_count,
          "raw bump arena active backing chunk accounting changed");

  auto escaped = manager.Deallocate(allocations[1], Tag("escaped_suballocation"));
  Require(!escaped.ok(),
          "suballocated arena pointer was accepted as a standalone allocation");
  const auto after_escape = manager.Snapshot();
  Require(after_escape.unknown_pointer_failure_count ==
              after_allocations.unknown_pointer_failure_count + 1,
          "escaped suballocation did not record an unknown pointer failure");

  const auto reset = arena.Reset();
  Require(reset.ok(), "raw bump arena reset failed");
  const auto after_reset = manager.Snapshot();
  Require(after_reset.current_bytes == before.current_bytes,
          "raw bump arena reset did not return current bytes");
  Require(after_reset.active_allocation_count == before.active_allocation_count,
          "raw bump arena reset did not release backing chunks");
  Require(after_reset.deallocation_count - before.deallocation_count == chunk_count,
          "raw bump arena reset did not deallocate by backing chunk count");
}

void QueryHeapGrantsUseBumpBackingAndResetByChunks() {
  memory::BoundedAllocator allocator(Policy());
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::UnifiedMemorySpillBudgetLedger unified(FixtureUuid(0x201, 8), 16 * 1024);
  memory::QueryMemoryArena arena(
      QueryContext(), QueryLimits(), &allocator, nullptr, &unified, &ledger);

  const auto before = allocator.Snapshot();
  std::vector<memory::QueryMemoryUuid> grant_ids;
  for (int i = 0; i < 32; ++i) {
    memory::QueryMemoryGrantRequest request;
    request.family = i % 2 == 0 ? memory::QueryMemoryFamily::relational
                                : memory::QueryMemoryFamily::candidate_set;
    request.bytes = 64;
    request.purpose = "query_heap_bump_" + std::to_string(i);
    const auto grant = arena.Grant(request);
    Require(grant.ok(), "query heap bump grant failed");
    Require(grant.grant.has_value(), "query heap bump grant token missing");
    Require(EvidenceHas(grant.evidence, "query_memory_arena.heap_backing=bump_region"),
            "query heap grant did not report bump backing");
    grant_ids.push_back(grant.grant->grant_id);
  }

  const auto after_grants = allocator.Snapshot();
  const auto chunk_count = after_grants.allocation_count - before.allocation_count;
  Require(chunk_count > 0, "query bump arena did not allocate backing chunks");
  Require(chunk_count < grant_ids.size(),
          "query bump arena allocated one backing block per grant");
  Require(arena.Snapshot().current_bytes == 32 * 64,
          "query arena logical current bytes after grants changed");
  const auto retained = arena.Snapshot().retained_heap_bytes;
  Require(retained >= 32 * 64 && retained <= 16 * 1024,
          "query backing outside admitted capacity bounds");
  Require(retained == after_grants.current_bytes - before.current_bytes,
          "query retained capacity does not match actual backing allocations");
  Require(ledger.Snapshot().current_bytes == retained,
          "query hierarchical ledger did not charge retained backing");
  Require(unified.Snapshot().total_bytes == retained,
          "query unified budget did not charge retained backing");

  const auto first_release = arena.Release(grant_ids.front());
  Require(first_release.ok(), "first query bump grant release failed");
  Require(EvidenceHas(first_release.evidence,
                      "heap_backing_retained_until_reset=true"),
          "query bump release did not retain backing while grants remain");
  Require(allocator.Snapshot().current_bytes == after_grants.current_bytes,
          "query bump backing chunks were released before the last heap grant");
  Require(arena.Snapshot().current_bytes == 31 * 64,
          "query arena logical bytes after first release changed");
  Require(ledger.Snapshot().current_bytes == retained &&
              unified.Snapshot().total_bytes == retained &&
              arena.Snapshot().retained_heap_bytes == retained,
          "logical release prematurely returned physical backing credits");

  for (std::size_t i = 1; i < grant_ids.size(); ++i) {
    const auto released = arena.Release(grant_ids[i]);
    Require(released.ok(), "query bump grant release failed");
  }

  const auto after_release = allocator.Snapshot();
  Require(after_release.current_bytes == before.current_bytes,
          "query bump arena did not release backing chunks after last grant");
  Require(after_release.active_allocation_count == before.active_allocation_count,
          "query bump arena active allocation count leaked after releases");
  Require(after_release.deallocation_count - before.deallocation_count == chunk_count,
          "query bump arena releases did not free by backing chunk count");
  Require(arena.Snapshot().current_bytes == 0,
          "query arena logical current bytes after releases changed");
  Require(ledger.Snapshot().current_bytes == 0,
          "query arena ledger bytes after releases changed");
  Require(unified.Snapshot().total_bytes == 0,
          "query arena unified budget bytes after releases changed");
}

void ExecutorAndOptimizerTypedWorkAreasRemainBumpBackedEvidenceOnly() {
  memory::MemoryManager manager(Policy());

  exec::ExecutorTypedArenaWorkAreaRequest executor_request;
  executor_request.memory_manager = &manager;
  executor_request.route_label = "public.release.executor.bump_region";
  executor_request.row_count = 128;
  auto executor = exec::BuildExecutorTypedArenaWorkArea(executor_request);
  Require(executor.ok(), "executor typed arena work area failed");
  Require(executor.typed_arena_allocation_count < executor.baseline_allocation_count,
          "executor typed arena did not reduce backing allocation count");
  Require(EvidenceHas(executor.evidence, "allocation_count_reduced=true"),
          "executor typed arena allocation reduction evidence missing");
  Require(EvidenceHas(
              executor.evidence,
              "executor_typed_arena.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_wal_or_benchmark_authority"),
          "executor typed arena authority boundary evidence missing");
  Require(manager.Snapshot().current_bytes == 0,
          "executor typed arena leaked backing memory after reset");

  opt::OptimizerTypedArenaWorkAreaRequest optimizer_request;
  optimizer_request.memory_manager = &manager;
  optimizer_request.planning_route_label = "public.release.optimizer.bump_region";
  optimizer_request.candidate_count = 96;
  auto optimizer = opt::BuildOptimizerTypedArenaWorkArea(optimizer_request);
  Require(optimizer.ok(), "optimizer typed arena work area failed");
  Require(optimizer.typed_arena_allocation_count < optimizer.baseline_allocation_count,
          "optimizer typed arena did not reduce backing allocation count");
  Require(EvidenceHas(optimizer.evidence, "allocation_count_reduced=true"),
          "optimizer typed arena allocation reduction evidence missing");
  Require(EvidenceHas(
              optimizer.evidence,
              "optimizer_typed_arena.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_wal_or_benchmark_authority"),
          "optimizer typed arena authority boundary evidence missing");
  Require(manager.Snapshot().current_bytes == 0,
          "optimizer typed arena leaked backing memory after reset");

  executor_request.parser_or_reference_authority = true;
  executor = exec::BuildExecutorTypedArenaWorkArea(executor_request);
  Require(!executor.ok() && executor.fail_closed,
          "executor typed arena unsafe parser authority did not fail closed");

  optimizer_request.memory_benchmark_authority = true;
  optimizer = opt::BuildOptimizerTypedArenaWorkArea(optimizer_request);
  Require(!optimizer.ok() && optimizer.fail_closed,
          "optimizer typed arena benchmark authority did not fail closed");
}

}  // namespace

int main() {
  RawArenaUsesChunkedBumpAllocationAndRejectsEscapedPointers();
  QueryHeapGrantsUseBumpBackingAndResetByChunks();
  ExecutorAndOptimizerTypedWorkAreasRemainBumpBackedEvidenceOnly();
  std::cout << "PCR-014 bump-region arenas are chunked, ownership-safe, and evidence-only\n";
  return EXIT_SUCCESS;
}
