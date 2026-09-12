// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "query_memory_arena.hpp"
#include "ipar_memory_resource_services.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mem = scratchbird::core::memory;
using Uuid = mem::QueryMemoryUuid;
void Require(bool condition, const char* why) {
  if (!condition) throw std::runtime_error(why);
}
Uuid Id(unsigned char value) {
  return Uuid{{1,2,3,4,5,6,0x70,8,0x80,10,11,12,13,14,15,value}};
}
struct Directory {
  std::filesystem::path path;
  Directory() {
    std::string pattern = "/tmp/sb-query-arena-owner-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const auto created = ::mkdtemp(buffer.data());
    Require(created != nullptr, "mkdtemp");
    path = created;
  }
  ~Directory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};
mem::QueryMemoryContext Context() {
  mem::QueryMemoryContext c;
  c.database_id = Id(1); c.engine_id = Id(2); c.session_id = Id(3);
  c.transaction_id = Id(4); c.statement_id = Id(5); c.query_id = Id(6);
  c.operation_id = Id(7); c.snapshot_boundary = Id(8);
  c.metadata_boundary = Id(9); c.resource_budget_reference = Id(10);
  c.policy_generation = 37; c.security_generation = 59;
  return c;
}
int main() {
  try {
    Directory root;
    const auto context = Context();
    mem::IparStatementPoolRequest pools;
    pools.context = context;
    pools.statement_limit_bytes = 4096;
    pools.max_batch_rows = 2;
    pools.row_version_bytes = 32;
    pools.diagnostic_bytes = 1;
    Require(mem::PlanIparStatementMemoryPools(pools).ok(), "actual typed pool planning");
    auto invalid_pools = pools;
    invalid_pools.context.snapshot_boundary = {};
    Require(!mem::PlanIparStatementMemoryPools(invalid_pools).ok(),
            "pool planner retains complete query identity requirements");
    invalid_pools = pools;
    invalid_pools.row_version_bytes = std::uint64_t{1} << 63;
    Require(!mem::PlanIparStatementMemoryPools(invalid_pools).ok(),
            "pool-size multiplication cannot wrap into accepted small allocation");
    invalid_pools = pools;
    invalid_pools.max_batch_rows = 1;
    invalid_pools.statement_limit_bytes = ~std::uint64_t{0};
    invalid_pools.row_version_bytes = ~std::uint64_t{0};
    invalid_pools.key_buffer_bytes = 1;
    Require(!mem::PlanIparStatementMemoryPools(invalid_pools).ok(),
            "pool-size sum cannot wrap at maximum budget");
    mem::TempWorkspacePolicy policy;
    policy.root_path = root.path;
    policy.database_uuid = context.database_id;
    policy.engine_uuid = context.engine_id;
    policy.filespace_quota_bytes = 4096;
    mem::TempWorkspaceLifecycleManager workspace(policy);
    auto allocator_policy = mem::DefaultLocalEngineMemoryPolicy();
    allocator_policy.hard_limit_bytes = 1024 * 1024;
    allocator_policy.per_context_limit_bytes = 1024 * 1024;
    mem::BoundedAllocator allocator(allocator_policy);
    mem::UnifiedMemorySpillBudgetLedger budget(context.resource_budget_reference, 4096);
    mem::QueryMemoryArenaLimits limits;
    limits.hard_limit_bytes = 4096; limits.soft_limit_bytes = 1;
    limits.query_limit_bytes = 4096; limits.family_limit_bytes = 4096;
    limits.spill_limit_bytes = 4096; limits.allow_spill = true;
    mem::QueryMemoryArena arena(context, limits, &allocator, &workspace, &budget);
    mem::QueryMemoryGrantRequest request;
    request.family = mem::QueryMemoryFamily::relational;
    request.bytes = 64; request.spillable = true;
    const auto grant = arena.Grant(request);
    Require(grant.ok() && grant.grant && grant.grant->spilled, "actual arena spill grant");
    Require(scratchbird::core::uuid::IsEngineIdentityUuid(grant.grant->grant_id) &&
            scratchbird::core::uuid::IsEngineIdentityUuid(grant.grant->spill_operation_id) &&
            scratchbird::core::uuid::IsEngineIdentityUuid(grant.grant->unified_budget_reservation_id),
            "issued binary grant, spill and reservation UUIDv7");
    const auto records = workspace.ActiveRecords();
    Require(records.size() == 1, "actual spill file created");
    const auto& owner = records[0].owner;
    Require(owner.database_id == context.database_id && owner.engine_id == context.engine_id &&
            owner.session_id == context.session_id && owner.transaction_id == context.transaction_id &&
            owner.statement_id == context.statement_id &&
            owner.snapshot_boundary == context.snapshot_boundary &&
            owner.metadata_boundary == context.metadata_boundary &&
            owner.resource_budget_reference == context.resource_budget_reference &&
            owner.policy_generation == 37 && owner.security_generation == 59,
            "exact query ownership propagated to actual spill");
    Require(owner.temp_object_uuid == grant.grant->spill_object_id &&
            owner.temp_object_uuid != grant.grant->grant_id &&
            owner.temp_object_uuid != owner.operation_id &&
            owner.operation_id == grant.grant->spill_operation_id,
            "distinct owned runtime objects use distinct issued identities");
    Require(budget.Snapshot().spill_bytes == 64, "actual unified spill reservation");
    const auto link = root.path / "held-link";
    std::filesystem::create_hard_link(records[0].path, link);
    const auto failed = arena.Release(grant.grant->grant_id);
    Require(!failed.ok(), "unsafe linked spill cleanup refused");
    Require(failed.counters.active_grant_count == 1 && failed.counters.leak_count == 1 &&
            workspace.ActiveRecords().size() == 1 && budget.Snapshot().spill_bytes == 64,
            "cleanup failure retains actual owner and reservation for retry");
    std::filesystem::remove(link);
    const auto retried = arena.Release(grant.grant->grant_id);
    Require(retried.ok() && retried.counters.active_grant_count == 0 &&
            workspace.ActiveRecords().empty() && budget.Snapshot().total_bytes == 0,
            "real cleanup retry releases exact owner");
    Require(!arena.Release(grant.grant->grant_id).ok(), "duplicate release is not successful");
    for (bool cancel : {false, true}) {
      mem::QueryMemoryArena retry_arena(context, limits, &allocator, &workspace, &budget);
      const auto next = retry_arena.Grant(request);
      Require(next.ok() && next.grant, "cancel/reset setup spill");
      const auto held = workspace.ActiveRecords();
      Require(held.size() == 1, "cancel/reset setup owner");
      std::filesystem::create_hard_link(held[0].path, link);
      const auto refused = cancel ? retry_arena.Cancel("test cancellation") : retry_arena.Reset();
      Require(!refused.ok() && refused.counters.active_grant_count == 1 &&
              refused.counters.leak_count == 1 && budget.Snapshot().spill_bytes == 64,
              "cancel/reset keeps failed cleanup owner and budget");
      Require(!retry_arena.Grant(request).ok(), "cancel/reset stops new grants");
      std::filesystem::remove(link);
      const auto retry = retry_arena.Reset();
      Require(retry.ok() && retry.counters.leak_count == 0 &&
              retry.counters.spilled_bytes == 0 && budget.Snapshot().total_bytes == 0 &&
              workspace.ActiveRecords().empty(), "cancel/reset actual cleanup retry");
    }
    Uuid mem::QueryMemoryContext::* identities[] = {
      &mem::QueryMemoryContext::database_id, &mem::QueryMemoryContext::engine_id,
      &mem::QueryMemoryContext::session_id, &mem::QueryMemoryContext::transaction_id,
      &mem::QueryMemoryContext::statement_id, &mem::QueryMemoryContext::query_id,
      &mem::QueryMemoryContext::operation_id, &mem::QueryMemoryContext::snapshot_boundary,
      &mem::QueryMemoryContext::metadata_boundary, &mem::QueryMemoryContext::resource_budget_reference};
    for (auto member : identities) {
      for (unsigned version = 0; version != 16; ++version) {
        if (version == 7) continue;
        auto invalid = context;
        auto value = invalid.*member;
        value.bytes[6] = static_cast<unsigned char>(version << 4);
        invalid.*member = value;
        mem::QueryMemoryArena rejected(invalid, limits, &allocator, &workspace, &budget);
        const auto result = rejected.Grant(request);
        Require(!result.ok() && !result.grant && result.counters.active_grant_count == 0 &&
                workspace.ActiveRecords().empty() && budget.Snapshot().total_bytes == 0,
                "non-v7 context identity refused before resource mutation");
      }
    }
    mem::UnifiedMemorySpillBudgetRequest reservation;
    reservation.owner_scope = Id(40); reservation.operation_id = Id(41); reservation.bytes = 64;
    const auto first = budget.Reserve(reservation);
    reservation.owner_scope = Id(42);
    const auto second = budget.Reserve(reservation);
    Require(first.ok() && second.ok() && first.reservation && second.reservation &&
            first.reservation->reservation_id != second.reservation->reservation_id,
            "actual distinct unified budget reservations");
    Require(!budget.ReleaseOwnerReservations({}).ok(), "nil unified owner cannot release");
    Require(budget.ReleaseOwnerReservations(Id(40)).released &&
            budget.Snapshot().total_bytes == 64, "exact binary unified owner release");
    Require(!budget.Release(first.reservation->reservation_id).ok(), "released owner cannot release twice");
    Require(budget.Release(second.reservation->reservation_id).ok() &&
            budget.Snapshot().total_bytes == 0, "other owner's reservation preserved");
    reservation.kind = static_cast<mem::UnifiedMemorySpillBudgetKind>(255);
    Require(!budget.Reserve(reservation).ok() && budget.Snapshot().total_bytes == 0,
            "invalid budget kind does not masquerade as spill");
    {
      auto heap_limits = limits;
      heap_limits.soft_limit_bytes = 4096;
      heap_limits.allow_spill = false;
      mem::QueryMemoryArena heap(context, heap_limits, &allocator, nullptr, &budget);
      std::vector<Uuid> grants;
      const auto before = allocator.Snapshot();
      for (unsigned i = 0; i != 32; ++i) {
        const auto allocated = heap.Grant(request);
        Require(allocated.ok() && allocated.grant && !allocated.grant->spilled,
                "actual heap grant");
        grants.push_back(allocated.grant->grant_id);
      }
      const auto physical = allocator.Snapshot();
      Require(physical.current_bytes >= 2048 &&
              physical.allocation_count - before.allocation_count < grants.size(),
              "actual heap uses shared bump backing");
      Require(budget.Snapshot().heap_bytes == physical.current_bytes - before.current_bytes &&
              budget.Snapshot().retained_bytes == budget.Snapshot().heap_bytes,
              "query budget retains actual chunk capacity, not logical payload bytes");
      bool query_scope_found = false;
      for (const auto& scope : physical.contexts) {
        if (scope.binary_scope &&
            scope.binary_scope->kind == mem::MemoryBinaryScopeKind::query &&
            scope.binary_scope->uuid == context.query_id.bytes) {
          Require(scope.scope_id.empty() && scope.current_bytes == physical.current_bytes,
                  "actual physical backing retains binary query ownership");
          query_scope_found = true;
        }
      }
      Require(query_scope_found, "binary physical query scope exists");
      Require(heap.Release(grants.front()).ok(), "first logical heap release");
      Require(budget.Snapshot().heap_bytes == physical.current_bytes - before.current_bytes &&
              allocator.Snapshot().current_bytes == physical.current_bytes,
              "logical release cannot erase retained capacity charges");
      for (std::size_t i = 1; i != grants.size(); ++i)
        Require(heap.Release(grants[i]).ok(), "heap grant release");
      Require(allocator.Snapshot().current_bytes == before.current_bytes &&
              budget.Snapshot().total_bytes == 0 && heap.Snapshot().active_grant_count == 0,
              "last grant actually frees backing and reservations");
    }
    std::cout << "actual query arena binary spill owner and cleanup retry PASS\n";
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
