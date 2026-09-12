// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "query_memory_arena.hpp"
#include "uuid.hpp"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <new>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <atomic>
#include <thread>
#include <cerrno>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
thread_local unsigned long calls = 0;
void Arm(long position) { remaining = position; hit = false; }
void Off() { remaining = -1; }
}
void* operator new(std::size_t bytes) {
  ++fault::calls;
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = 0;
    fault::hit = true;
    throw std::bad_alloc();
  }
  if (auto pointer = std::malloc(bytes ? bytes : 1)) return pointer;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
  try { return ::operator new(bytes); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
  return ::operator new(bytes, std::nothrow);
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace mem = scratchbird::core::memory;
using Uuid = mem::QueryMemoryUuid;
unsigned failures = 0, injected = 0;
void Check(bool condition, const char* why, unsigned mode, long position) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL mode=%u allocation=%ld %s\n", mode, position, why);
  }
}
Uuid Id(unsigned char value) {
  return Uuid{{1,2,3,4,5,6,0x70,8,0x80,10,11,12,13,14,15,value}};
}
mem::UnifiedMemorySpillBudgetRequest Request(unsigned char owner) {
  mem::UnifiedMemorySpillBudgetRequest request;
  request.owner_scope = Id(owner); request.operation_id = Id(40);
  request.bytes = 17;
  return request;
}
mem::QueryMemoryContext Context() {
  mem::QueryMemoryContext c;
  c.database_id = Id(1); c.engine_id = Id(2); c.session_id = Id(3);
  c.transaction_id = Id(4); c.statement_id = Id(5); c.query_id = Id(6);
  c.operation_id = Id(7); c.snapshot_boundary = Id(8);
  c.metadata_boundary = Id(9); c.resource_budget_reference = Id(10);
  return c;
}
mem::MemoryTag ArenaTag(unsigned char identity) {
  mem::MemoryTag tag;
  tag.category = mem::MemoryCategory::executor_query_reserved;
  tag.purpose = "real arena capacity admission";
  for (unsigned i = 0; i != tag.binary_ownership.scopes.size(); ++i) {
    tag.binary_ownership.scopes[i] = Id(identity).bytes;
    tag.binary_ownership.scopes[i][14] = static_cast<unsigned char>(i);
  }
  return tag;
}
struct SpillDirectory {
  std::filesystem::path path;
  SpillDirectory() {
    char pattern[] = "/tmp/sb-spill-allocation-fault-XXXXXX";
    const auto created = ::mkdtemp(pattern);
    if (!created) std::abort();
    path = created;
  }
  ~SpillDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
// Independent file/content oracle, always outside the injection window.
std::map<std::string, std::string> Files(const std::filesystem::path& path) {
  std::map<std::string, std::string> files;
  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    std::ifstream in(entry.path(), std::ios::binary);
    files.emplace(entry.path().filename().string(),
                  std::string(std::istreambuf_iterator<char>(in), {}));
  }
  return files;
}
std::size_t OpenDescriptorCount() {
  return static_cast<std::size_t>(std::distance(
      std::filesystem::directory_iterator("/proc/self/fd"),
      std::filesystem::directory_iterator{}));
}
mem::TempWorkspaceAllocationRequest SpillRequest(unsigned char identity) {
  const auto c = Context();
  mem::TempWorkspaceAllocationRequest request;
  request.owner.temp_object_uuid = Id(identity);
  request.owner.database_id = c.database_id; request.owner.engine_id = c.engine_id;
  request.owner.session_id = c.session_id; request.owner.transaction_id = c.transaction_id;
  request.owner.statement_id = c.statement_id; request.owner.operation_id = Id(identity + 1);
  request.owner.snapshot_boundary = c.snapshot_boundary;
  request.owner.metadata_boundary = c.metadata_boundary;
  request.owner.resource_budget_reference = c.resource_budget_reference;
  request.lifetime = mem::TempWorkspaceLifetime::operation_lifetime;
  request.bytes = 64;
  request.purpose = "actual spill failure-atomic publication";
  return request;
}
int main() {
  (void)scratchbird::core::uuid::IssueRuntimeIdentityV7();
  for (unsigned mode = 0; mode != 3; ++mode) {
    bool reached_success = false;
    for (long position = 0; position != 256; ++position) {
      mem::UnifiedMemorySpillBudgetLedger ledger(Id(1), 4096);
      const auto untouched = ledger.Reserve(Request(2));
      Check(untouched.ok() && untouched.reservation.has_value(), "setup other owner", mode, position);
      auto target = mem::UnifiedMemorySpillBudgetResult{};
      if (mode != 0) target = ledger.Reserve(Request(3));
      if (mode == 2) (void)ledger.Reserve(Request(3));
      const auto before = ledger.Snapshot();
      bool threw = false;
      mem::UnifiedMemorySpillBudgetResult result;
      fault::Arm(position);
      try {
        if (mode == 0) result = ledger.Reserve(Request(3));
        if (mode == 1) result = ledger.Release(target.reservation->reservation_id);
        if (mode == 2) result = ledger.ReleaseOwnerReservations(Id(3));
      } catch (const std::bad_alloc&) { threw = true; }
      const bool hit = fault::hit;
      fault::Off();
      injected += hit;
      const auto after = ledger.Snapshot();
      if (threw || !result.ok()) {
        Check(after.total_bytes == before.total_bytes &&
              after.heap_bytes == before.heap_bytes && after.spill_bytes == before.spill_bytes &&
              after.active_reservation_count == before.active_reservation_count &&
              after.peak_total_bytes == before.peak_total_bytes,
              "failed operation changed retained owners or authoritative accounting", mode, position);
      } else {
        Check((mode == 0 && result.reservation_created && result.reservation &&
               after.total_bytes == 34 && after.active_reservation_count == 2) ||
              (mode != 0 && result.released && after.total_bytes == 17 &&
               after.active_reservation_count == 1),
              "successful operation must match actual ledger state", mode, position);
      }
      Check(ledger.Release(untouched.reservation->reservation_id).ok(),
            "other owner remains releasable", mode, position);
      (void)ledger.ReleaseOwnerReservations(Id(3));
      Check(ledger.Snapshot().total_bytes == 0, "test releases actual reservations", mode, position);
      if (!hit) {
        reached_success = !threw && result.ok();
        Check(reached_success, "uninjected operation must actually succeed", mode, position);
        break;
      }
    }
    Check(reached_success, "fault enumeration reached successful operation", mode, -1);
  }
  for (unsigned scenario = 0; scenario != 5; ++scenario) {
    bool reached_heap_success = false;
    long heap_positions = 2048;
    for (long position = -1; position != heap_positions; ++position) {
      auto policy = mem::DefaultLocalEngineMemoryPolicy();
      policy.hard_limit_bytes = 4 * 1024 * 1024;
      policy.per_context_limit_bytes = 4 * 1024 * 1024;
      mem::BoundedAllocator allocator(policy);
      mem::HierarchicalMemoryBudgetLedger hierarchy(3, 5);
      mem::HierarchicalMemoryBudgetLedger temp_hierarchy(7, 9);
      SpillDirectory owned;
      mem::TempWorkspacePolicy temp_policy;
      temp_policy.database_uuid = Context().database_id;
      temp_policy.engine_uuid = Context().engine_id;
      temp_policy.root_path = owned.path;
      temp_policy.reservation_ledger = &temp_hierarchy;
      temp_policy.require_ceic_011_reservation = true;
      mem::TempWorkspaceLifecycleManager workspace(temp_policy);
      mem::UnifiedMemorySpillBudgetLedger ledger(Id(10), 2 * 1024 * 1024);
      mem::QueryMemoryArenaLimits limits;
      limits.hard_limit_bytes = 2 * 1024 * 1024;
      limits.query_limit_bytes = 2 * 1024 * 1024;
      limits.family_limit_bytes = 2 * 1024 * 1024;
      limits.require_hierarchical_reservation = true;
      const bool spill = scenario >= 3;
      if (spill) {
        limits.soft_limit_bytes = 1;
        limits.allow_spill = true;
        limits.spill_limit_bytes = 4096;
      }
      mem::QueryMemoryArena arena(Context(), limits, &allocator, &workspace, &ledger, &hierarchy);
      mem::QueryMemoryGrantRequest request;
      request.family = mem::QueryMemoryFamily::relational; request.bytes = 64;
      request.spillable = spill;
      std::optional<Uuid> existing_grant;
      if (scenario == 1 || scenario == 2 || scenario == 4) {
        const auto existing = arena.Grant(request);
        Check(existing.ok() && existing.grant.has_value(), "existing heap grant", 3+scenario, position);
        if (!existing.grant) return 2;
        existing_grant = existing.grant->grant_id;
      }
      if (scenario == 2) request.bytes = 128 * 1024;
      const auto before = arena.Snapshot();
      const auto before_physical = allocator.Snapshot().current_bytes;
      const auto before_hierarchy = hierarchy.Snapshot().current_bytes;
      const auto before_unified = ledger.Snapshot().total_bytes;
      const auto before_files = Files(owned.path);
      const auto before_temp = temp_hierarchy.Snapshot().current_bytes;
      const auto before_records = workspace.ActiveRecords().size();
      const auto before_descriptors = OpenDescriptorCount();
      mem::QueryMemoryArenaResult result;
      bool threw = false;
      fault::Arm(position);
      const auto calls_before = fault::calls;
      try { result = arena.Grant(request); }
      catch (const std::bad_alloc&) { threw = true; }
      const bool hit = fault::hit;
      const auto calls_during = fault::calls - calls_before;
      fault::Off();
      injected += hit;
      const auto snapshot = arena.Snapshot();
      Check(OpenDescriptorCount() == before_descriptors,
            "arena allocation leaked a file or directory descriptor", 3+scenario, position);
      if (threw || !result.ok()) {
        Check(snapshot.current_bytes == before.current_bytes &&
              snapshot.active_grant_count == before.active_grant_count &&
              snapshot.grant_count == before.grant_count &&
              snapshot.spilled_bytes == before.spilled_bytes &&
              snapshot.retained_heap_bytes == before.retained_heap_bytes &&
              snapshot.consumed_heap_bytes == before.consumed_heap_bytes &&
              snapshot.heap_chunk_count == before.heap_chunk_count &&
              allocator.Snapshot().current_bytes == before_physical &&
              ledger.Snapshot().total_bytes == before_unified &&
              hierarchy.Snapshot().current_bytes == before_hierarchy,
              "failed heap grant changed live owners, physical backing or reservations",
              3+scenario, position);
        Check(Files(owned.path) == before_files &&
              workspace.ActiveRecords().size() == before_records &&
              temp_hierarchy.Snapshot().current_bytes == before_temp,
              "failed arena grant changed actual spill files, manifest or owners",
              3+scenario, position);
      } else {
        Check(result.grant.has_value() &&
              result.grant->spilled == spill &&
              snapshot.current_bytes == before.current_bytes + (spill ? 0 : request.bytes) &&
              snapshot.spilled_bytes == before.spilled_bytes + (spill ? request.bytes : 0) &&
              snapshot.active_grant_count == before.active_grant_count + 1 &&
              allocator.Snapshot().current_bytes >= snapshot.current_bytes &&
              snapshot.retained_heap_bytes == allocator.Snapshot().current_bytes &&
              ledger.Snapshot().total_bytes == allocator.Snapshot().current_bytes + snapshot.spilled_bytes &&
              hierarchy.Snapshot().current_bytes == allocator.Snapshot().current_bytes + snapshot.spilled_bytes,
              "successful heap grant retains actual resources", 3+scenario, position);
        Check(workspace.ActiveRecords().size() == before_records + (spill ? 1 : 0) &&
              temp_hierarchy.Snapshot().current_bytes == before_temp + (spill ? request.bytes : 0),
              "successful arena grant matches temp owners and reservations", 3+scenario, position);
      }
      if (existing_grant)
        Check(arena.Release(*existing_grant).ok(), "preexisting grant remains releasable",
              3+scenario, position);
      (void)arena.Reset();
      // Explicit fixture teardown is not credited as failed-call rollback.
      (void)hierarchy.CleanupOwner(Context().query_id.bytes);
      (void)ledger.ReleaseOwnerReservations(Context().query_id);
      for (const auto& record : workspace.ActiveRecords()) {
        (void)workspace.CleanupOperation(record.owner.operation_id);
        (void)temp_hierarchy.CleanupOwner(record.owner.temp_object_uuid.bytes);
      }
      Check(allocator.Snapshot().current_bytes == 0 && ledger.Snapshot().total_bytes == 0 &&
            hierarchy.Snapshot().current_bytes == 0 && temp_hierarchy.Snapshot().current_bytes == 0,
            "fixture releases actual resources",
            3+scenario, position);
      if (position == -1) {
        Check(!threw && result.ok() && result.grant.has_value(),
              "healthy heap preflight actually succeeds", 3+scenario, position);
        std::printf("healthy arena scenario=%u operator-new calls=%lu\n", scenario, calls_during);
        heap_positions = static_cast<long>(calls_during + 64);
      } else if (!hit) {
        reached_heap_success = !threw && result.ok() && result.grant.has_value();
        Check(reached_heap_success, "uninjected heap grant must actually succeed", 3+scenario, position);
        break;
      }
    }
    Check(reached_heap_success, "heap fault enumeration reached success", 3+scenario, -1);
  }
  for (unsigned scenario = 0; scenario != 2; ++scenario) {
    bool reached_success = false;
    long positions = 2048;
    for (long position = -1; position != positions; ++position) {
      const unsigned mode = 8 + scenario;
      SpillDirectory owned;
      mem::HierarchicalMemoryBudgetLedger hierarchy(3, 5);
      mem::TempWorkspacePolicy policy;
      policy.database_uuid = Context().database_id;
      policy.engine_uuid = Context().engine_id;
      policy.root_path = owned.path;
      policy.filespace_quota_bytes = 4096;
      policy.reservation_ledger = &hierarchy;
      policy.require_ceic_011_reservation = true;
      mem::TempWorkspaceLifecycleManager manager(policy);
      std::optional<mem::TempWorkspaceRecord> existing;
      if (scenario != 0) {
        auto setup = manager.AllocateSpillFile(SpillRequest(30));
        Check(setup.ok() && setup.record.has_value(), "existing real spill", mode, position);
        if (!setup.record) return 2;
        existing = std::move(setup.record);
        std::ofstream data(existing->path, std::ios::binary | std::ios::trunc);
        data << "preexisting spill payload must survive failed allocation";
        data.close();
        Check(static_cast<bool>(data), "write preexisting spill data", mode, position);
      }
      const auto before = manager.Snapshot();
      const auto before_records = manager.ActiveRecords().size();
      const auto before_files = Files(owned.path);
      const auto before_budget = hierarchy.Snapshot().current_bytes;
      const auto before_descriptors = OpenDescriptorCount();
      mem::TempWorkspaceResult result;
      bool threw = false;
      auto request = SpillRequest(40);
      fault::Arm(position);
      const auto calls_before = fault::calls;
      try { result = manager.AllocateSpillFile(std::move(request)); }
      catch (const std::bad_alloc&) { threw = true; }
      const bool hit = fault::hit;
      const auto calls_during = fault::calls - calls_before;
      fault::Off();
      injected += hit;
      const auto after = manager.Snapshot();
      Check(OpenDescriptorCount() == before_descriptors,
            "temp allocation leaked a file or directory descriptor", mode, position);
      if (threw || !result.ok()) {
        Check(after.active_bytes == before.active_bytes &&
              after.session_bytes == before.session_bytes &&
              after.transaction_bytes == before.transaction_bytes &&
              after.statement_bytes == before.statement_bytes &&
              after.operation_bytes == before.operation_bytes &&
              manager.ActiveRecords().size() == before_records,
              "failed spill changed authoritative owners or quota accounting", mode, position);
        Check(Files(owned.path) == before_files,
              "failed spill changed files or persisted owner manifest", mode, position);
        Check(hierarchy.Snapshot().current_bytes == before_budget,
              "failed spill retained an unreported budget reservation", mode, position);
      } else {
        Check(result.record && after.active_bytes == before.active_bytes + 64 &&
              manager.ActiveRecords().size() == before_records + 1 &&
              hierarchy.Snapshot().current_bytes == before_budget + 64 &&
              std::filesystem::is_regular_file(result.record->path),
              "successful spill owns real file and reservation", mode, position);
      }
      if (existing)
        Check(manager.Find(existing->allocation_id).has_value() &&
              manager.CleanupOperation(existing->owner.operation_id).ok(),
              "preexisting spill remains owned and releasable", mode, position);
      (void)manager.CleanupOperation(Id(41));
      // Teardown is never credited as rollback of the allocation under test.
      (void)hierarchy.CleanupOwner(Id(30).bytes);
      (void)hierarchy.CleanupOwner(Id(40).bytes);
      if (position == -1) {
        Check(!threw && result.ok() && result.record.has_value(),
              "healthy spill preflight actually succeeds", mode, position);
        positions = static_cast<long>(calls_during + 64);
        std::printf("healthy spill scenario=%u operator-new calls=%lu\n", scenario, calls_during);
      } else if (!hit) {
        reached_success = !threw && result.ok() && result.record.has_value();
        Check(reached_success, "uninjected spill must actually succeed", mode, position);
        break;
      }
    }
    Check(reached_success, "spill fault enumeration reached success", 8+scenario, -1);
  }
  {
    SpillDirectory owned;
    mem::TempWorkspacePolicy policy;
    policy.database_uuid = Context().database_id;
    policy.engine_uuid = Context().engine_id;
    policy.root_path = owned.path;
    policy.disk_reservation_mode = mem::TempWorkspaceDiskReservationMode::logical_quota_only;
    mem::TempWorkspaceLifecycleManager manager(policy);
    auto request = SpillRequest(50);
    request.bytes = ~std::uint64_t{0};
    const auto first = manager.AllocateSpillFile(request);
    Check(first.ok() && first.record && manager.Snapshot().active_bytes == request.bytes &&
          std::filesystem::file_size(first.record->path) == 0,
          "maximum logical quota does not claim physical preallocation", 10, -1);
    const auto before_files = Files(owned.path);
    auto extra = SpillRequest(60);
    extra.bytes = 1;
    const auto refused = manager.AllocateSpillFile(extra);
    Check(!refused.ok() && !refused.record && Files(owned.path) == before_files &&
          manager.ActiveRecords().size() == 1 && manager.Snapshot().active_bytes == request.bytes,
          "unlimited quota cannot wrap physical owner accounting", 10, -1);
    (void)manager.CleanupOperation(request.owner.operation_id);
    (void)manager.CleanupOperation(extra.owner.operation_id);
  }
  {
    SpillDirectory owned;
    mem::TempWorkspacePolicy policy;
    policy.database_uuid = Context().database_id;
    policy.engine_uuid = Context().engine_id;
    policy.root_path = owned.path;
    policy.manifest_generation = ~std::uint64_t{0} - 1;
    mem::TempWorkspaceLifecycleManager manager(policy);
    const auto first = manager.AllocateSpillFile(SpillRequest(70));
    Check(first.ok() && first.record, "last representable manifest generation", 11, -1);
    const auto before_files = Files(owned.path);
    const auto extra = manager.AllocateSpillFile(SpillRequest(80));
    Check(!extra.ok() && !extra.record && Files(owned.path) == before_files &&
          manager.ActiveRecords().size() == 1 && manager.Snapshot().active_bytes == 64,
          "manifest generation cannot wrap or reuse earlier generation", 11, -1);
    (void)manager.CleanupOperation(Id(71));
    (void)manager.CleanupOperation(Id(81));
  }
  for (unsigned records = 1; records != 3; ++records) {
    const unsigned mode = 11 + records;
    SpillDirectory owned;
    mem::TempWorkspacePolicy policy;
    policy.database_uuid = Context().database_id;
    policy.engine_uuid = Context().engine_id;
    policy.root_path = owned.path;
    policy.require_ceic_011_reservation = true;
    {
      mem::HierarchicalMemoryBudgetLedger source_ledger(13, 15);
      policy.reservation_ledger = &source_ledger;
      mem::TempWorkspaceLifecycleManager source(policy);
      for (unsigned i = 0; i != records; ++i) {
        const auto created = source.AllocateSpillFile(SpillRequest(90 + 2*i));
        Check(created.ok() && created.record, "real recovery source allocation", mode, -1);
      }
    }
    const auto before_files = Files(owned.path);
    bool reached_success = false;
    long positions = 4096;
    for (long position = -1; position != positions; ++position) {
      mem::HierarchicalMemoryBudgetLedger restored_ledger(17, 19);
      policy.reservation_ledger = &restored_ledger;
      const auto before_descriptors = OpenDescriptorCount();
      std::unique_ptr<mem::TempWorkspaceLifecycleManager> recovered;
      bool threw = false;
      fault::Arm(position);
      const auto calls_before = fault::calls;
      try { recovered = std::make_unique<mem::TempWorkspaceLifecycleManager>(policy); }
      catch (const std::bad_alloc&) { threw = true; }
      const bool hit = fault::hit;
      const auto calls_during = fault::calls - calls_before;
      fault::Off();
      injected += hit;
      const auto active = recovered ? recovered->ActiveRecords().size() : 0;
      const bool succeeded = !threw && active == records;
      Check(OpenDescriptorCount() == before_descriptors && Files(owned.path) == before_files,
            "recovery failure changed source files or leaked descriptors", mode, position);
      if (!succeeded) {
        Check(active == 0 && restored_ledger.Snapshot().current_bytes == 0,
              "failed recovery retained unreported reservations or partial owners", mode, position);
      } else {
        Check(recovered->Snapshot().active_bytes == records * 64 &&
              restored_ledger.Snapshot().current_bytes == records * 64,
              "recovery publishes real owners and newly acquired reservations", mode, position);
        for (const auto& record : recovered->ActiveRecords())
          Check(record.owner == SpillRequest(record.owner.temp_object_uuid.bytes[15]).owner,
                "recovered binary owner remains exact", mode, position);
      }
      // Check rollback above, then release process-local test reservations.
      recovered.reset();
      for (unsigned i = 0; i != records; ++i)
        (void)restored_ledger.CleanupOwner(Id(90 + 2*i).bytes);
      if (position == -1) {
        Check(succeeded, "healthy real-file recovery succeeds", mode, position);
        positions = static_cast<long>(calls_during + 64);
        std::printf("healthy recovery records=%u operator-new calls=%lu\n", records, calls_during);
      } else if (!hit) {
        reached_success = succeeded;
        Check(reached_success, "uninjected recovery actually succeeds", mode, position);
        break;
      }
    }
    Check(reached_success, "recovery fault enumeration reached success", mode, -1);
  }
  for (unsigned spill = 0; spill != 2; ++spill) {
    for (unsigned action = 0; action != 3; ++action) {
      const unsigned mode = 14 + spill * 3 + action;
      bool reached_success = false;
      long positions = 4096;
      for (long position = -1; position != positions; ++position) {
        SpillDirectory owned;
        mem::HierarchicalMemoryBudgetLedger hierarchy(3, 5), temp_hierarchy(7, 9);
        auto allocator_policy = mem::DefaultLocalEngineMemoryPolicy();
        allocator_policy.hard_limit_bytes = 1024 * 1024;
        allocator_policy.per_context_limit_bytes = 1024 * 1024;
        mem::BoundedAllocator allocator(allocator_policy);
        mem::TempWorkspacePolicy policy;
        policy.database_uuid = Context().database_id;
        policy.engine_uuid = Context().engine_id;
        policy.root_path = owned.path;
        policy.reservation_ledger = &temp_hierarchy;
        policy.require_ceic_011_reservation = true;
        mem::TempWorkspaceLifecycleManager workspace(policy);
        mem::UnifiedMemorySpillBudgetLedger ledger(Id(10), 4096);
        mem::QueryMemoryArenaLimits limits;
        limits.hard_limit_bytes = 4096; limits.query_limit_bytes = 4096;
        limits.family_limit_bytes = 4096; limits.spill_limit_bytes = 4096;
        limits.require_hierarchical_reservation = true;
        if (spill) { limits.soft_limit_bytes = 1; limits.allow_spill = true; }
        mem::QueryMemoryArena arena(Context(), limits, &allocator, &workspace, &ledger, &hierarchy);
        mem::QueryMemoryGrantRequest request;
        request.family = mem::QueryMemoryFamily::relational;
        request.bytes = 64; request.spillable = spill;
        const auto first = arena.Grant(request);
        const auto second = arena.Grant(request);
        Check(first.ok() && first.grant && second.ok() && second.grant,
              "actual cleanup grants", mode, position);
        if (!first.grant || !second.grant) return 2;
        const auto before_descriptors = OpenDescriptorCount();
        mem::QueryMemoryArenaReleaseResult result;
        bool threw = false;
        fault::Arm(position);
        const auto calls_before = fault::calls;
        try {
          if (action == 0) result = arena.Release(first.grant->grant_id);
          if (action == 1) result = arena.Cancel("allocation failure cleanup test");
          if (action == 2) result = arena.Reset();
        } catch (const std::bad_alloc&) { threw = true; }
        const bool hit = fault::hit;
        const auto calls_during = fault::calls - calls_before;
        fault::Off();
        injected += hit;
        Check(OpenDescriptorCount() == before_descriptors,
              "cleanup leaked file or directory descriptors", mode, position);
        const auto retained = arena.Snapshot().active_grant_count;
        if (action == 0) {
          if (threw || !result.ok()) {
            Check(retained == 2, "failed exact release lost its retryable query owner", mode, position);
            if (retained == 2)
              Check(arena.Release(first.grant->grant_id).ok(),
                    "failed exact release supports successful retry", mode, position);
          } else {
            Check(retained == 1, "successful exact release preserves other grant", mode, position);
          }
          Check(arena.Release(second.grant->grant_id).ok(),
                "other grant survives exact-owner cleanup", mode, position);
        } else if (!threw && result.ok()) {
          Check(retained == 0, "successful bulk cleanup has no retained grants", mode, position);
        }
        // Retry the actual cleanup API, not ledger cleanup or fixture deletion.
        Check(arena.Reset().ok() && arena.Snapshot().active_grant_count == 0 &&
              allocator.Snapshot().current_bytes == 0 && ledger.Snapshot().total_bytes == 0 &&
              hierarchy.Snapshot().current_bytes == 0 && temp_hierarchy.Snapshot().current_bytes == 0 &&
              workspace.ActiveRecords().empty() && Files(owned.path).empty(),
              "cleanup retry releases real files, manifests, backing and all reservations",
              mode, position);
        if (position == -1) {
          Check(!threw && result.ok(), "healthy real cleanup succeeds", mode, position);
          positions = static_cast<long>(calls_during + 64);
          std::printf("healthy cleanup spill=%u action=%u operator-new calls=%lu\n",
                      spill, action, calls_during);
        } else if (!hit) {
          reached_success = !threw && result.ok();
          Check(reached_success, "uninjected cleanup actually succeeds", mode, position);
          break;
        }
      }
      Check(reached_success, "cleanup fault enumeration reached success", mode, -1);
    }
  }
  {
    auto policy = mem::DefaultLocalEngineMemoryPolicy();
    policy.hard_limit_bytes = 2 * 1024 * 1024;
    policy.per_context_limit_bytes = policy.hard_limit_bytes;
    mem::BoundedAllocator allocator(policy);
    const auto foreign = allocator.Allocate(256, 64, ArenaTag(100));
    Check(foreign.ok(), "foreign backing allocation", 20, -1);
    mem::ArenaAllocator first(&allocator, ArenaTag(101)), second(&allocator, ArenaTag(102));
    fault::Arm(0);
    const auto plan = first.PlanAllocation(64, 64, 256);
    const auto empty = first.CapacitySnapshot();
    const auto invalid_alignment = first.PlanAllocation(64, 3, 256);
    const auto invalid_large_alignment = first.PlanAllocation(64, 48, 256);
    const auto invalid_zero = first.PlanAllocation(0, 64, 256);
    const auto invalid_capacity = first.PlanAllocation(64, 64, 63);
    const bool planning_allocated = fault::hit;
    fault::Off();
    Check(!planning_allocated && plan.ok() && plan.growth_bytes == 256 &&
          empty.retained_bytes == 0 && empty.consumed_bytes == 0,
          "planning and own-capacity inspection are nonallocating and nonmutating", 20, -1);
    Check(!invalid_alignment.ok() && !invalid_large_alignment.ok() &&
          !invalid_zero.ok() && !invalid_capacity.ok(),
          "invalid alignment, zero request and insufficient growth are refused without mutation", 20, -1);
    const auto a = first.AllocateWithinCapacity(64, 64, 256);
    const auto b = second.AllocateWithinCapacity(128, 128, 256);
    Check(a.ok() && b.ok(), "real independent arena chunks", 20, -1);
    if (!a.ok() || !b.ok()) return 2;
    std::memset(a.pointer, 0x5a, 64);
    const auto reuse_plan = first.PlanAllocation(64, 64, 0);
    const auto reused = first.AllocateWithinCapacity(64, 64, 0);
    Check(reuse_plan.ok() && reuse_plan.growth_bytes == 0 && reused.ok() &&
          reused.pointer != a.pointer && first.CapacitySnapshot().retained_bytes == 256 &&
          first.CapacitySnapshot().consumed_bytes == 128 &&
          second.CapacitySnapshot().retained_bytes == 256 && allocator.Snapshot().current_bytes == 768,
          "arena capacity excludes other arenas and unrelated backing; zero-growth reuse is allowed", 20, -1);
    Check(!first.PlanAllocation(256, 64, 128).ok() &&
          !first.AllocateWithinCapacity(256, 64, 128).ok() &&
          first.CapacitySnapshot().consumed_bytes == 128,
          "growth ceiling refusal leaves the bump cursor intact", 20, -1);
    const auto grown = first.AllocateWithinCapacity(256, 256, 256);
    Check(grown.ok() && reinterpret_cast<std::uintptr_t>(grown.pointer) % 256 == 0 &&
          first.CapacitySnapshot().retained_bytes == 512 &&
          first.CapacitySnapshot().consumed_bytes == 384 &&
          first.CapacitySnapshot().chunk_count == 2,
          "new aligned chunk matches exact planned physical growth", 20, -1);
    for (unsigned i = 0; i != 64; ++i)
      Check(static_cast<unsigned char*>(a.pointer)[i] == 0x5a,
            "growth preserves existing payload", 20, -1);
    mem::ArenaAllocator moved(std::move(first));
    Check(first.CapacitySnapshot().retained_bytes == 0 &&
          moved.CapacitySnapshot().retained_bytes == 512, "move transfers exact capacity owner", 20, -1);
    second = std::move(moved);
    Check(moved.CapacitySnapshot().retained_bytes == 0 &&
          second.CapacitySnapshot().retained_bytes == 512 && allocator.Snapshot().current_bytes == 768,
          "move assignment frees old destination backing and transfers source chunks", 20, -1);
    Check(second.Reset().ok() && second.CapacitySnapshot().retained_bytes == 0 &&
          allocator.Snapshot().current_bytes == 256,
          "arena reset frees only its own physical chunks", 20, -1);
    Check(allocator.DeallocateNoAlloc(foreign.pointer).ok(), "release foreign allocation", 20, -1);
  }
  {
    auto policy = mem::DefaultLocalEngineMemoryPolicy();
    policy.hard_limit_bytes = 64; policy.per_context_limit_bytes = 64;
    mem::BoundedAllocator allocator(policy);
    mem::ArenaAllocator arena(&allocator, ArenaTag(103));
    Check(!arena.AllocateWithinCapacity(64, 64, 256).ok() &&
          arena.CapacitySnapshot().retained_bytes == 0 && allocator.Snapshot().current_bytes == 0,
          "exact growth cannot silently substitute a smaller chunk", 21, -1);
    const auto minimal = arena.Allocate(64, 64);
    Check(minimal.ok() && arena.CapacitySnapshot().retained_bytes == 64 &&
          reinterpret_cast<std::uintptr_t>(minimal.pointer) % 64 == 0,
          "generic aligned fallback fits an exact 64-byte backing budget", 21, -1);
    Check(arena.Reset().ok() && allocator.Snapshot().current_bytes == 0,
          "minimal fallback owns and releases real backing", 21, -1);
  }
  for (unsigned scenario = 0; scenario != 3; ++scenario) {
    const unsigned mode = 22 + scenario;
    bool reached_success = false;
    long positions = 4096;
    for (long position = -1; position != positions; ++position) {
      auto policy = mem::DefaultLocalEngineMemoryPolicy();
      policy.hard_limit_bytes = 1024 * 1024; policy.per_context_limit_bytes = policy.hard_limit_bytes;
      mem::BoundedAllocator allocator(policy);
      mem::ArenaAllocator arena(&allocator, ArenaTag(104));
      mem::AllocationResult existing;
      if (scenario != 0) {
        existing = arena.AllocateWithinCapacity(scenario == 1 ? 64 : 128, 64, 128);
        Check(existing.ok(), "capacity fault setup backing", mode, position);
        if (!existing.ok()) return 2;
        std::memset(existing.pointer, 0x6b, 64);
      }
      const auto before = arena.CapacitySnapshot();
      const auto before_backend = allocator.Snapshot().current_bytes;
      const std::size_t bytes = scenario == 1 ? 32 : 256;
      const std::size_t ceiling = scenario == 1 ? 0 : 256;
      const auto plan = arena.PlanAllocation(bytes, 64, ceiling);
      mem::AllocationResult result;
      bool threw = false;
      fault::Arm(position);
      const auto calls_before = fault::calls;
      try { result = arena.AllocateWithinCapacity(bytes, 64, ceiling); }
      catch (const std::bad_alloc&) { threw = true; }
      const auto calls_during = fault::calls - calls_before;
      const bool hit = fault::hit;
      fault::Off();
      injected += hit;
      const auto after = arena.CapacitySnapshot();
      if (threw || !result.ok()) {
        Check(after.retained_bytes == before.retained_bytes &&
              after.consumed_bytes == before.consumed_bytes && after.chunk_count == before.chunk_count &&
              allocator.Snapshot().current_bytes == before_backend,
              "failed exact arena growth preserves chunks, cursor and physical allocation", mode, position);
      } else {
        Check(plan.ok() && after.retained_bytes == before.retained_bytes + plan.growth_bytes &&
              allocator.Snapshot().current_bytes == before_backend + plan.growth_bytes,
              "successful exact arena growth matches pre-admission plan", mode, position);
      }
      if (existing.ok())
        for (unsigned i = 0; i != 64; ++i)
          Check(static_cast<unsigned char*>(existing.pointer)[i] == 0x6b,
                "allocation failure preserves prior arena data", mode, position);
      Check(arena.Reset().ok() && allocator.Snapshot().current_bytes == 0,
            "capacity fault teardown releases actual chunks", mode, position);
      if (position == -1) {
        Check(!threw && result.ok(), "healthy exact capacity allocation succeeds", mode, position);
        positions = static_cast<long>(calls_during + 64);
        std::printf("healthy exact capacity scenario=%u operator-new calls=%lu\n", scenario, calls_during);
      } else if (!hit) {
        reached_success = !threw && result.ok();
        Check(reached_success, "uninjected exact capacity allocation succeeds", mode, position);
        break;
      }
    }
    Check(reached_success, "capacity fault enumeration reaches success", mode, -1);
  }
  for (unsigned scenario = 0; scenario != 4; ++scenario) {
    const unsigned mode = 25 + scenario;
    auto policy = mem::DefaultLocalEngineMemoryPolicy();
    policy.hard_limit_bytes = 4096; policy.per_context_limit_bytes = 4096;
    mem::BoundedAllocator allocator(policy);
    mem::UnifiedMemorySpillBudgetLedger ledger(Id(10), 256);
    auto request = Request(3); request.bytes = 64;
    const auto reserved = ledger.Reserve(request);
    Check(reserved.ok() && reserved.reservation, "reserve exact physical capacity", mode, -1);
    if (!reserved.reservation) return 2;
    auto retained = ledger.Retain(reserved.reservation->reservation_id);
    Check(retained.ok() && retained.lease.live() &&
          !ledger.Retain(reserved.reservation->reservation_id).ok(),
          "one retained owner per actual binary reservation", mode, -1);
    auto tag = ArenaTag(120);
    tag.binary_ownership[mem::MemoryBinaryScopeKind::query] = request.owner_scope.bytes;
    mem::ArenaAllocator arena(&allocator, tag);
    const auto block = arena.AllocateWithinCapacity(64, 64, 64);
    Check(block.ok(), "real lease-backed physical chunk", mode, -1);
    if (!block.ok()) return 2;
    const auto untouched = ledger.Reserve(Request(4));
    auto other = request;
    std::optional<Uuid> unretained;
    if (scenario == 2) {
      const auto raw = ledger.Reserve(other);
      Check(raw.ok() && raw.reservation, "mixed raw and retained same-owner reservations", mode, -1);
      if (!raw.reservation) return 2;
      unretained = raw.reservation->reservation_id;
    }
    mem::UnifiedMemorySpillBudgetResult released;
    if (scenario == 0) released = ledger.Release(reserved.reservation->reservation_id);
    if (scenario == 1) {
      fault::Arm(0);
      const auto status = ledger.ReleaseNoAlloc(reserved.reservation->reservation_id);
      const bool allocated = fault::hit;
      fault::Off();
      Check(!status.ok() && !allocated, "raw allocation-free release revokes but retains charge", mode, -1);
    }
    if (scenario == 2) released = ledger.ReleaseOwnerReservations(request.owner_scope);
    if (scenario == 3) {
      std::atomic<bool> entered{false}, done{false};
      std::thread revoke;
      {
        auto use = retained.lease.Use();
        Check(use.live(), "live guard before concurrent revocation", mode, -1);
        revoke = std::thread([&] {
          entered.store(true, std::memory_order_release);
          released = ledger.ReleaseOwnerReservations(request.owner_scope);
          done.store(true, std::memory_order_release);
        });
        while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned i = 0; i != 1000; ++i) {
          static_cast<unsigned char*>(block.pointer)[i % 64] = static_cast<unsigned char>(i);
          Check(use.live() && !done.load(std::memory_order_acquire),
                "revocation cannot complete while guarded physical use is active", mode, -1);
        }
      }
      revoke.join();
      Check(done.load(std::memory_order_acquire), "revocation completes after use guard retires", mode, -1);
    }
    if (scenario != 1)
      Check(!released.ok() && released.retained && released.retained_bytes == 64 &&
            released.released == (scenario == 2),
            "public cleanup truthfully distinguishes retained and released reservations", mode, -1);
    Check(retained.lease.valid() && !retained.lease.live() &&
          ledger.Snapshot().total_bytes == 81 && ledger.Snapshot().retained_bytes == 64 &&
          ledger.Snapshot().retained_reservation_count == 1 &&
          allocator.Snapshot().current_bytes == 64,
          "revocation preserves real storage charge and prevents new use", mode, -1);
    if (unretained)
      Check(!ledger.Release(*unretained).ok(), "mixed cleanup actually removed raw reservation", mode, -1);
    mem::UnifiedMemorySpillBudgetLease moved(std::move(retained.lease));
    Check(!retained.lease.valid() && moved.valid() && !moved.live(),
          "move preserves revoked retained ownership", mode, -1);
    Check(arena.Reset().ok() && allocator.Snapshot().current_bytes == 0,
          "retire actual backing before releasing its charge", mode, -1);
    fault::Arm(0);
    const auto reset = moved.Reset();
    const bool allocated = fault::hit;
    fault::Off();
    Check(reset.ok() && !allocated && !moved.valid() && ledger.Snapshot().total_bytes == 17 &&
          ledger.Snapshot().retained_bytes == 0,
          "exact retained owner releases charge without allocation after storage retirement", mode, -1);
    Check(untouched.reservation && ledger.Release(untouched.reservation->reservation_id).ok(),
          "unrelated owner remains releasable", mode, -1);
  }
  for (unsigned scenario = 0; scenario != 3; ++scenario) {
    bool completed = false;
    for (long position = 0; position != 128; ++position) {
      const unsigned mode = 29 + scenario;
      mem::UnifiedMemorySpillBudgetLedger ledger(Id(10), 256);
      auto request = Request(3); request.bytes = 64;
      const auto reserved = ledger.Reserve(request);
      if (!reserved.reservation) return 2;
      mem::UnifiedMemorySpillBudgetRetainResult retained;
      if (scenario != 0) retained = ledger.Retain(reserved.reservation->reservation_id);
      const auto before = ledger.Snapshot();
      mem::UnifiedMemorySpillBudgetResult released;
      fault::Arm(position);
      if (scenario == 0) retained = ledger.Retain(reserved.reservation->reservation_id);
      if (scenario == 1) released = ledger.Release(reserved.reservation->reservation_id);
      if (scenario == 2) released = ledger.ReleaseOwnerReservations(request.owner_scope);
      const bool hit = fault::hit;
      fault::Off();
      injected += hit;
      const auto after = ledger.Snapshot();
      Check(after.total_bytes == before.total_bytes && after.active_reservation_count == 1,
            "retain or revoke fault cannot drop a storage charge", mode, position);
      const bool success = scenario == 0 ? retained.ok() : (!released.ok() && released.retained);
      if (!success)
        Check(after.retained_bytes == before.retained_bytes &&
              (scenario == 0 ? !retained.lease.valid() : retained.lease.live()),
              "failed retain/revoke metadata construction leaves original ownership intact", mode, position);
      if (retained.lease.valid()) Check(retained.lease.Reset().ok(), "release actual retained test owner", mode, position);
      else (void)ledger.ReleaseNoAlloc(reserved.reservation->reservation_id);
      if (!hit) { completed = success; break; }
    }
    Check(completed, "retained-owner fault enumeration reaches intended outcome", 29+scenario, -1);
  }
  // Real query backing, two independently owned queries and an unrelated
  // allocator user. These fixed byte counts are not derived from arena counters.
  for (unsigned authority = 0; authority != 2; ++authority) {
    const unsigned mode = 32 + authority;
    auto policy = mem::DefaultLocalEngineMemoryPolicy();
    policy.hard_limit_bytes = policy.per_context_limit_bytes = 1024 * 1024;
    mem::BoundedAllocator allocator(policy);
    mem::HierarchicalMemoryBudgetLedger hierarchy(3, 5);
    mem::UnifiedMemorySpillBudgetLedger ledger(Id(20), 1024 * 1024);
    mem::QueryMemoryArenaLimits limits;
    limits.hard_limit_bytes = 1024;
    limits.query_limit_bytes = limits.family_limit_bytes = 512;
    limits.require_hierarchical_reservation = true;
    auto unrelated = allocator.Allocate(32, 0, ArenaTag(60));
    Check(unrelated.ok(), "unrelated physical owner", mode, -1);
    auto other_context = Context(); other_context.query_id = Id(61);
    other_context.operation_id = Id(62);
    mem::QueryMemoryArena arena(Context(), limits, &allocator, nullptr, &ledger, &hierarchy);
    mem::QueryMemoryArena other(other_context, limits, &allocator, nullptr, &ledger, &hierarchy);
    mem::QueryMemoryGrantRequest request;
    request.family = mem::QueryMemoryFamily::relational; request.bytes = 64;
    auto first = arena.Grant(request);
    auto second = arena.Grant(request);
    auto survivor = other.Grant(request);
    Check(first.ok() && second.ok() && survivor.ok() && first.grant && second.grant && survivor.grant,
          "actual isolated query grants", mode, -1);
    if (!first.grant || !second.grant || !survivor.grant) return 2;
    Check(arena.Snapshot().current_bytes == 128 && arena.Snapshot().retained_heap_bytes == 512 &&
          arena.Snapshot().consumed_heap_bytes == 128 && arena.Snapshot().heap_chunk_count == 1 &&
          allocator.Snapshot().current_bytes == 1056 && hierarchy.Snapshot().current_bytes == 1024 &&
          ledger.Snapshot().retained_bytes == 1024 && ledger.Snapshot().retained_reservation_count == 2,
          "exact growth and zero-growth reuse fund physical capacity", mode, -1);
    Check(arena.Release(first.grant->grant_id).ok() && arena.Snapshot().current_bytes == 64 &&
          arena.Snapshot().retained_heap_bytes == 512 && ledger.Snapshot().heap_bytes == 1024 &&
          hierarchy.Snapshot().current_bytes == 1024 && allocator.Snapshot().current_bytes == 1056,
          "first logical release preserves both queries' physical charges", mode, -1);
    request.bytes = 448;  // Fits logical quota but not the remaining bump space.
    auto denied = arena.Grant(request);
    Check(!denied.ok() && !denied.grant && arena.Snapshot().consumed_heap_bytes == 128 &&
          arena.Snapshot().current_bytes == 64 && allocator.Snapshot().current_bytes == 1056,
          "retained capacity limit cannot be bypassed by logical release", mode, -1);
    if (authority == 0) {
      auto revoked = ledger.ReleaseOwnerReservations(Context().query_id);
      Check(!revoked.ok() && revoked.retained && revoked.retained_bytes == 512,
            "query physical owner retained under unified revocation", mode, -1);
    } else {
      auto revoked = hierarchy.CleanupOwner(Context().query_id.bytes);
      Check(revoked.retained_bytes == 512 && hierarchy.Snapshot().retained_revoked_bytes == 512,
            "query physical owner retained under hierarchical revocation", mode, -1);
    }
    request.bytes = 64;
    denied = arena.Grant(request);
    auto other_reuse = other.Grant(request);
    Check(!denied.ok() && !denied.grant && other_reuse.ok() && other_reuse.grant &&
          arena.Snapshot().consumed_heap_bytes == 128 && allocator.Snapshot().current_bytes == 1056 &&
          ledger.Snapshot().heap_bytes == 1024 && hierarchy.Snapshot().current_bytes == 1024,
          "revoked query cannot reuse backing while unrelated query remains usable", mode, -1);
    Check(arena.Release(second.grant->grant_id).ok() && arena.Snapshot().retained_heap_bytes == 0 &&
          arena.Snapshot().current_bytes == 0 && allocator.Snapshot().current_bytes == 544 &&
          hierarchy.Snapshot().current_bytes == 512 && ledger.Snapshot().heap_bytes == 512,
          "last query grant retires exact revoked capacity and leases", mode, -1);
    Check(other.Reset().ok() && allocator.Snapshot().current_bytes == 32 &&
          hierarchy.Snapshot().current_bytes == 0 && ledger.Snapshot().heap_bytes == 0,
          "query teardown preserves unrelated physical owner", mode, -1);
    Check(allocator.DeallocateNoAlloc(unrelated.pointer).ok(), "unrelated owner cleanup", mode, -1);
  }
  for (unsigned constraint = 0; constraint != 4; ++constraint) {
    const unsigned mode = 34 + constraint;
    auto policy = mem::DefaultLocalEngineMemoryPolicy();
    policy.hard_limit_bytes = policy.per_context_limit_bytes = 1024 * 1024;
    if (constraint == 0) policy.hard_limit_bytes = 96;
    if (constraint == 1) policy.per_context_limit_bytes = 96;
    if (constraint == 2) { policy.soft_limit_bytes = 96; policy.reject_over_soft_limit = true; }
    if (constraint == 3) policy.hard_limit_bytes = 224;
    mem::BoundedAllocator allocator(policy);
    auto tag = ArenaTag(70);
    if (constraint == 1)
      tag.binary_ownership[mem::MemoryBinaryScopeKind::query] = Context().query_id.bytes;
    auto foreign = allocator.Allocate(32, 0, tag);
    Check(foreign.ok(), "physical availability preexisting owner", mode, -1);
    std::unique_ptr<mem::MemoryCapacityReservation> credit;
    if (constraint == 3) {
      auto reserved = allocator.ReserveCapacity(128, ArenaTag(71));
      Check(reserved.ok(), "physical availability unused credit", mode, -1);
      credit = std::move(reserved.reservation);
    }
    mem::HierarchicalMemoryBudgetLedger hierarchy(3, 5);
    mem::UnifiedMemorySpillBudgetLedger ledger(Id(20), 1024);
    mem::QueryMemoryArenaLimits limits;
    limits.hard_limit_bytes = limits.query_limit_bytes = limits.family_limit_bytes = 1024;
    limits.require_hierarchical_reservation = true;
    mem::QueryMemoryArena arena(Context(), limits, &allocator, nullptr, &ledger, &hierarchy);
    mem::QueryMemoryGrantRequest request;
    request.family = mem::QueryMemoryFamily::relational; request.bytes = 64;
    auto granted = arena.Grant(request);
    Check(granted.ok() && granted.grant && arena.Snapshot().retained_heap_bytes == 64 &&
          allocator.Snapshot().current_bytes == 96 && hierarchy.Snapshot().current_bytes == 64 &&
          ledger.Snapshot().heap_bytes == 64 &&
          allocator.Snapshot().reserved_capacity_bytes == (constraint == 3 ? 128 : 0),
          "query plans exact available physical capacity without consuming other credit", mode, -1);
    Check(arena.Reset().ok() && allocator.Snapshot().current_bytes == 32 &&
          hierarchy.Snapshot().current_bytes == 0 && ledger.Snapshot().heap_bytes == 0,
          "bounded physical query cleanup preserves other owners", mode, -1);
    if (credit) {
      auto payload = credit->Allocate(128);
      Check(payload.ok(), "unrelated reserved physical credit remains usable", mode, -1);
      if (payload.ok()) (void)allocator.DeallocateNoAlloc(payload.pointer);
      credit.reset();
    }
    Check(allocator.DeallocateNoAlloc(foreign.pointer).ok(), "physical foreign cleanup", mode, -1);
  }
  {
    const unsigned mode = 38;
    auto policy = mem::DefaultLocalEngineMemoryPolicy();
    policy.hard_limit_bytes = 256; policy.per_context_limit_bytes = 0;
    mem::BoundedAllocator allocator(policy);
    auto tag = ArenaTag(80);
    auto live = allocator.Allocate(64, 0, tag);
    auto credit = allocator.ReserveCapacity(128, ArenaTag(81));
    Check(live.ok() && credit.ok(), "physical availability live and reserved setup", mode, -1);
    const auto before = allocator.Snapshot();
    fault::Arm(0);
    const auto calls = fault::calls;
    const auto hint = allocator.AvailableCapacity(tag);
    const bool allocated = fault::hit || calls != fault::calls;
    fault::Off();
    const auto after = allocator.Snapshot();
    Check(hint.ok() && hint.available_bytes == 64 && !allocated &&
          after.current_bytes == before.current_bytes &&
          after.reserved_capacity_bytes == before.reserved_capacity_bytes &&
          after.allocation_count == before.allocation_count && after.failure_count == before.failure_count,
          "headroom is allocation-free and cannot mutate admission or failure counters", mode, -1);
    std::thread competitor([&] {
      auto claimed = allocator.ReserveCapacity(64, ArenaTag(82));
      Check(claimed.ok(), "competing real reservation after headroom hint", mode, -1);
      auto refused = allocator.Allocate(hint.available_bytes, 0, tag);
      Check(!refused.ok() && allocator.AvailableCapacity(tag).available_bytes == 0,
            "stale headroom cannot override another owner's admission", mode, -1);
      if (refused.ok()) (void)allocator.DeallocateNoAlloc(refused.pointer);
    });
    competitor.join();
    auto own = credit.reservation->Allocate(128);
    Check(own.ok() && allocator.AvailableCapacity(tag).available_bytes == 64,
          "converting reserved credit does not change admitted headroom", mode, -1);
    if (own.ok()) (void)allocator.DeallocateNoAlloc(own.pointer);
    credit.reservation.reset();
    (void)allocator.DeallocateNoAlloc(live.pointer);
  }
  for (unsigned scope = 0; scope != 7; ++scope) {
    const unsigned mode = 39 + scope;
    auto policy = mem::DefaultLocalEngineMemoryPolicy();
    policy.hard_limit_bytes = 4096; policy.per_context_limit_bytes = 128;
    mem::BoundedAllocator allocator(policy);
    auto target = ArenaTag(83), same_scope = ArenaTag(84);
    same_scope.binary_ownership.scopes[scope] = target.binary_ownership.scopes[scope];
    auto live = allocator.Allocate(64, 0, same_scope);
    auto credit = allocator.ReserveCapacity(32, same_scope);
    Check(live.ok() && credit.ok(), "each binary scope real admission", mode, -1);
    auto fresh = ArenaTag(85);
    fault::Arm(0);
    const auto calls = fault::calls;
    auto shared = allocator.AvailableCapacity(target);
    auto separate = allocator.AvailableCapacity(fresh);
    const bool allocated = fault::hit || fault::calls != calls;
    fault::Off();
    Check(shared.ok() && shared.available_bytes == 32 && separate.ok() &&
          separate.available_bytes == 128 && !allocated,
          "every exact binary owner scope counts live plus reserved without charging fresh owners", mode, -1);
    credit.reservation.reset();
    (void)allocator.DeallocateNoAlloc(live.pointer);
  }
  {
    const unsigned mode = 46;
    auto policy = mem::DefaultLocalEngineMemoryPolicy();
    policy.hard_limit_bytes = 4096; policy.per_context_limit_bytes = 0;
    policy.page_buffer_pool_limit_bytes = 160;
    mem::BoundedAllocator allocator(policy);
    auto page = ArenaTag(86); page.category = mem::MemoryCategory::page_buffer;
    auto live = allocator.Allocate(64, 0, page);
    auto credit = allocator.ReserveCapacity(32, page);
    auto regular = ArenaTag(87);
    fault::Arm(0);
    const auto calls = fault::calls;
    auto page_hint = allocator.AvailableCapacity(page);
    auto regular_hint = allocator.AvailableCapacity(regular);
    const bool allocated = fault::hit || fault::calls != calls;
    fault::Off();
    Check(live.ok() && credit.ok() && page_hint.ok() && page_hint.available_bytes == 64 &&
          regular_hint.ok() && regular_hint.available_bytes == 4000 && !allocated,
          "page-pool headroom includes page credit without imposing page limits on query memory", mode, -1);
    credit.reservation.reset(); (void)allocator.DeallocateNoAlloc(live.pointer);
  }
  {
    const unsigned mode = 47;
    auto policy = mem::DefaultLocalEngineMemoryPolicy();
    policy.hard_limit_bytes = 4096; policy.per_context_limit_bytes = 128;
    mem::BoundedAllocator allocator(policy);
    mem::MemoryTag legacy; legacy.owner = std::string(200, 'a');
    auto live = allocator.Allocate(32, 0, legacy);
    auto fresh = legacy; fresh.owner.back() = 'b';
    auto invalid = ArenaTag(88); invalid.owner = "mixed authority";
    fault::Arm(0);
    const auto calls = fault::calls;
    auto owned_hint = allocator.AvailableCapacity(legacy);
    auto fresh_hint = allocator.AvailableCapacity(fresh);
    auto invalid_hint = allocator.AvailableCapacity(invalid);
    const bool allocated = fault::hit || fault::calls != calls;
    fault::Off();
    Check(live.ok() && owned_hint.ok() && owned_hint.available_bytes == 96 &&
          fresh_hint.ok() && fresh_hint.available_bytes == 128 && !invalid_hint.ok() &&
          invalid_hint.available_bytes == 0 && !allocated,
          "legacy nonbinary labels stay separate and invalid mixed authority refuses without allocation", mode, -1);
    (void)allocator.DeallocateNoAlloc(live.pointer);
  }
  {
    const unsigned mode = 48;
    auto policy = mem::DefaultLocalEngineMemoryPolicy();
    policy.byte_limit = policy.hard_limit_bytes = policy.per_context_limit_bytes = 0;
    policy.soft_limit_bytes = 1; policy.reject_over_soft_limit = false;
    mem::BoundedAllocator allocator(policy);
    auto tag = ArenaTag(89);
    const auto maximum = std::numeric_limits<mem::usize>::max();
    auto credit = allocator.ReserveCapacity(maximum, tag);
    Check(credit.ok() && allocator.AvailableCapacity(tag).available_bytes == 0,
          "unlimited policy cannot overflow maximum admitted capacity", mode, -1);
    credit.reservation.reset();
    Check(allocator.AvailableCapacity(tag).available_bytes == maximum,
          "nonrejecting soft policy is not a hard admission ceiling", mode, -1);
    policy.refuse_all_allocations = true;
    mem::BoundedAllocator refused(policy);
    fault::Arm(0);
    const auto hint = refused.AvailableCapacity(tag);
    const bool allocated = fault::hit;
    fault::Off();
    Check(!hint.ok() && hint.available_bytes == 0 && !allocated,
          "unconfigured physical allocator cannot advertise usable capacity", mode, -1);
  }
  {
    const unsigned mode = 49;
    auto policy = mem::DefaultLocalEngineMemoryPolicy();
    policy.hard_limit_bytes = policy.per_context_limit_bytes = 4096;
    mem::BoundedAllocator allocator(policy);
    auto tag = ArenaTag(90);
    auto credit = allocator.ReserveCapacity(64, tag);
    Check(credit.ok(), "alignment reserved capacity setup", mode, -1);
    for (const auto alignment : {3u, 5u, 6u, 7u, 9u, 12u, 15u, 24u, 48u}) {
      auto ordinary = allocator.Allocate(32, alignment, tag);
      Check(!ordinary.ok() && ordinary.pointer == nullptr && allocator.Snapshot().current_bytes == 0 &&
            allocator.Snapshot().reserved_capacity_bytes == 64,
            "raw allocator refuses every non-power-of-two alignment before normalization", mode, alignment);
      if (ordinary.ok()) (void)allocator.DeallocateNoAlloc(ordinary.pointer);
      auto reserved = credit.reservation->Allocate(32, alignment);
      Check(!reserved.ok() && reserved.pointer == nullptr && allocator.Snapshot().current_bytes == 0 &&
            allocator.Snapshot().reserved_capacity_bytes == 64,
            "reserved allocator invalid alignment preserves unused credit", mode, alignment);
      if (reserved.ok()) (void)allocator.DeallocateNoAlloc(reserved.pointer);
    }
    for (const auto alignment : {0u, 1u, 2u, 4u, 8u, 16u, 32u, 64u}) {
      auto valid = credit.reservation->Allocate(32, alignment);
      Check(valid.ok() && (alignment == 0 || reinterpret_cast<std::uintptr_t>(valid.pointer) % alignment == 0),
            "default and power-of-two reserved alignments remain supported", mode, alignment);
      if (valid.ok()) (void)allocator.DeallocateNoAlloc(valid.pointer);
    }
    credit.reservation.reset();
  }
  for (unsigned scenario = 0; scenario != 3; ++scenario) {
    const unsigned mode = 50 + scenario;
    const pid_t child = ::fork();
    Check(child >= 0, "fork real heap teardown allocation blackout", mode, -1);
    if (child == 0) {
      const rlimit no_core{0, 0}; (void)::setrlimit(RLIMIT_CORE, &no_core);
      auto policy = mem::DefaultLocalEngineMemoryPolicy();
      policy.hard_limit_bytes = policy.per_context_limit_bytes = 1024 * 1024;
      policy.zero_memory_on_release = true;
      mem::BoundedAllocator allocator(policy);
      mem::HierarchicalMemoryBudgetLedger hierarchy(3, 5);
      mem::UnifiedMemorySpillBudgetLedger ledger(Id(20), 1024 * 1024);
      auto foreign = allocator.Allocate(32, 0, ArenaTag(91));
      auto foreign_budget = ledger.Reserve(Request(92));
      mem::QueryMemoryArenaLimits limits;
      limits.hard_limit_bytes = limits.query_limit_bytes = limits.family_limit_bytes = 512 * 1024;
      limits.require_hierarchical_reservation = true;
      auto arena = std::make_unique<mem::QueryMemoryArena>(Context(), limits, &allocator,
                                                         nullptr, &ledger, &hierarchy);
      if (!foreign.ok() || !foreign_budget.ok()) ::_exit(71);
      if (scenario != 0) {
        mem::QueryMemoryGrantRequest request;
        request.family = mem::QueryMemoryFamily::relational; request.bytes = 64;
        auto first = arena->Grant(request);
        request.bytes = 128 * 1024;
        auto second = arena->Grant(request);
        if (!first.ok() || !first.grant || !second.ok() || !second.grant ||
            arena->Snapshot().heap_chunk_count != 2) ::_exit(72);
        if (scenario == 2) {
          if (!arena->Release(first.grant->grant_id).ok()) ::_exit(73);
          (void)hierarchy.CleanupOwner(Context().query_id.bytes);
          (void)ledger.ReleaseOwnerReservations(Context().query_id);
        }
      }
      fault::Arm(0);
      const auto calls = fault::calls;
      arena.reset();
      const bool allocated = fault::hit || fault::calls != calls;
      fault::Off();
      if (allocated || allocator.Snapshot().current_bytes != 32 ||
          hierarchy.Snapshot().current_bytes != 0 || ledger.Snapshot().heap_bytes != 17 ||
          ledger.Snapshot().retained_bytes != 0) ::_exit(74);
      if (!allocator.DeallocateNoAlloc(foreign.pointer).ok() ||
          !ledger.ReleaseNoAlloc(foreign_budget.reservation->reservation_id).ok()) ::_exit(75);
      ::_exit(0);
    }
    if (child > 0) {
      int status = 0;
      pid_t waited;
      do { waited = ::waitpid(child, &status, 0); } while (waited == -1 && errno == EINTR);
      Check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "heap destructor under blackout must exit normally and retire exact backing before charges", mode, -1);
    }
  }
  std::printf("query memory allocation failure injected=%u failures=%u\n", injected, failures);
  return failures ? 1 : 0;
}
