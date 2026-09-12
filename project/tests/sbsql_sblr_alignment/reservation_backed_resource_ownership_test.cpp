// SPDX-License-Identifier: MPL-2.0
#include "reservation_backed_memory_resource.hpp"
#include <cstdlib>
#include <array>
#include <atomic>
#include <barrier>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <new>
#include <string_view>
#include <thread>
namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
long current_index = -1;
void Arm(long n) { current_index = n; remaining = n; hit = false; }
void Off() { remaining = -1; }
}
#ifndef SB_ADAPTER_NO_ALLOC_OVERRIDE
void* operator new(std::size_t n) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = 0; fault::hit = true; throw std::bad_alloc();
  }
  if (auto p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try { return ::operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return ::operator new(n, std::nothrow); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif
namespace {
namespace mem = scratchbird::core::memory;
using Code = scratchbird::core::platform::StatusCode;
unsigned checks = 0, failures = 0, injected = 0;
void Check(bool good, std::string_view why) {
  ++checks;
  if (!good) { ++failures; std::cerr << "FAIL " << why << '\n'; }
}
mem::AllocationPolicy Policy() {
  auto p = mem::DefaultLocalEngineMemoryPolicy();
  p.hard_limit_bytes = 1024 * 1024;
  p.per_context_limit_bytes = 1024 * 1024;
  p.zero_memory_on_allocate = true; p.zero_memory_on_release = true;
  return p;
}
void WarmMetrics() {
  mem::MemoryManager manager(Policy());
  mem::MemoryTag tag; tag.purpose = "adapter-contract-metric-warmup";
  auto allocated = manager.Allocate(16, 64, tag);
  Check(allocated.ok() && manager.Deallocate(allocated.pointer, std::move(tag)).ok(),
        "actual process allocator/metric warmup");
}
mem::HierarchicalMemoryBudgetProvenance Provenance() {
  mem::HierarchicalMemoryBudgetProvenance p;
  p.source = mem::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
  p.source_label = "adapter-owner-contract-runtime-policy";
  return p;
}
mem::ReservationBackedMemoryResourceRequest Request(
    mem::HierarchicalMemoryBudgetLedger& ledger, mem::MemoryManager& manager) {
  mem::ReservationBackedMemoryResourceRequest r;
  r.reservation_ledger = &ledger; r.memory_manager = &manager;
  r.scope_chain = {{mem::HierarchicalMemoryScopeKind::process, "adapter-process-scope-label"},
                  {mem::HierarchicalMemoryScopeKind::session, "adapter-session-scope-label"},
                  {mem::HierarchicalMemoryScopeKind::query, "adapter-query-scope-label"}};
  r.owner_id = "adapter-resource-owner-label";
  r.route_label = "adapter-resource-route-label";
  r.operation_id = "adapter-resource-operation-label";
  r.purpose = "adapter-resource-purpose-label";
  r.requested_bytes = 4096;
  r.provenance = Provenance();
  return r;
}
void Configure(mem::HierarchicalMemoryBudgetLedger& ledger, mem::MemoryManager& manager) {
  for (const auto& scope : Request(ledger, manager).scope_chain) {
    mem::HierarchicalMemoryBudget budget;
    budget.scope = scope; budget.hard_limit_bytes = 16384; budget.provenance = Provenance();
    Check(ledger.SetBudget(std::move(budget)).ok(), "actual shared parent budget setup");
  }
}
void Charges(mem::HierarchicalMemoryBudgetLedger& ledger, mem::MemoryManager& manager,
             mem::u64 reserved, mem::u64 bytes, mem::u64 physical_owners) {
  const auto parent = ledger.Snapshot();
  Check(parent.current_bytes == reserved && parent.reserved_bytes == 0,
        "adapter parent reservation does not match live owner");
  Check(parent.active_allocation_count == (reserved ? 1u : 0u) &&
        parent.active_reservation_count == 0, "adapter parent token ownership mismatch");
  const auto physical = manager.Snapshot();
  Check(physical.current_bytes == bytes && physical.active_allocation_count == physical_owners,
        "adapter lost/retained actual physical owner");
  Check(physical.sharded_accounting_current_bytes == bytes &&
        physical.sharded_accounting_active_allocation_count == physical_owners,
        "adapter physical accounting not backed by actual shared allocator");
}
#ifndef SB_ADAPTER_NO_ALLOC_OVERRIDE
void Sweep(std::string_view mode) {
  bool complete = false;
  for (long n = 0; n != 8192 && !complete; ++n) {
    mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
    mem::MemoryManager manager(Policy());
    Configure(ledger, manager);
    auto request = Request(ledger, manager);
    mem::ReservationBackedMemoryResourceAcquireResult acquired;
    if (mode != "acquire") acquired = mem::AcquireReservationBackedMemoryResource(request);
    if (mode != "acquire") Check(acquired.ok(), "actual adapter fixture acquire");
    if (mode == "release") {
      Check(acquired.resource->Allocate({137, 256, "first-real-block"}).ok(), "first release block");
      Check(acquired.resource->Allocate({269, 512, "second-real-block"}).ok(), "second release block");
    }
    mem::AllocationResult existing;
    if (mode == "deallocate") {
      existing = acquired.resource->Allocate({137, 256, {}});
      Check(existing.ok(), "actual explicit deallocation fixture");
      std::memset(existing.pointer, 0xa5, 137);
    }
    mem::AllocationResult allocation;
    mem::DeallocationResult deallocated;
    mem::ReservationBackedMemoryResourceReleaseResult released;
    bool threw = false, success = false;
    fault::Arm(n);
    try {
      if (mode == "acquire") { acquired = mem::AcquireReservationBackedMemoryResource(std::move(request)); success = acquired.ok(); }
      if (mode == "allocate") { allocation = acquired.resource->Allocate({137, 256, {}}); success = allocation.ok(); }
      if (mode == "release") { released = acquired.resource->Release(); success = released.ok() && released.released; }
      if (mode == "deallocate") { deallocated = acquired.resource->Deallocate(existing.pointer, 137, 256); success = deallocated.ok(); }
    } catch (const std::bad_alloc&) { threw = true; }
    fault::Off();
    Check(!threw, "adapter allocation failure escaped typed API");
    if (mode == "acquire") Charges(ledger, manager, success ? 4096 : 0, 0, 0);
    if (mode == "allocate") {
      Charges(ledger, manager, 4096, success ? 137 : 0, success ? 1 : 0);
      if (success) {
        std::memset(allocation.pointer, 0xa5, 137);
        Check(acquired.resource->Deallocate(allocation.pointer, 137, 256).ok(),
              "published adapter physical owner not releasable");
      }
    }
    if (mode == "release") Charges(ledger, manager, success ? 0 : 4096, success ? 0 : 406, success ? 0 : 2);
    if (mode == "deallocate") {
      Charges(ledger, manager, 4096, success ? 0 : 137, success ? 0 : 1);
      if (!success) {
        bool intact = true;
        for (unsigned i = 0; i < 137; ++i) intact &= static_cast<unsigned char*>(existing.pointer)[i] == 0xa5;
        Check(intact, "failed explicit deallocation changed live data");
        Check(acquired.resource->Deallocate(existing.pointer, 137, 256).ok(), "explicit deallocation could not retry");
      }
    }
    if (fault::hit) ++injected; else { complete = true; Check(success, "uninjected adapter operation failed"); }
    acquired.resource.reset();
    Charges(ledger, manager, 0, 0, 0);
  }
  Check(complete, "adapter fault sweep incomplete");
}

void PmrFaults() {
  bool complete = false;
  for (long n = 0; n < 8192 && !complete; ++n) {
    mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
    mem::MemoryManager manager(Policy());
    Configure(ledger, manager);
    auto acquired = mem::AcquireReservationBackedMemoryResource(Request(ledger, manager));
    Check(acquired.ok(), "PMR real reservation fixture");
    mem::ReservationBackedPmrMemoryResource pmr(acquired.resource.get(), "adapter-pmr-long-purpose");
    void* pointer = nullptr;
    fault::Arm(n);
    try { pointer = pmr.allocate(137, 256); } catch (const std::bad_alloc&) {}
    fault::Off();
    Charges(ledger, manager, 4096, pointer ? 137 : 0, pointer ? 1 : 0);
    const auto snapshot = pmr.Snapshot();
    Check(snapshot.allocated_bytes == (pointer ? 137u : 0u), "PMR publication count mismatch");
    Check(snapshot.failed_allocation_count == (pointer ? 0u : 1u), "PMR failure counted incorrectly");
    if (fault::hit) ++injected; else complete = true;
    if (pointer) {
      fault::Arm(0);
      pmr.deallocate(pointer, 137, 256);
      fault::Off();
      Check(!fault::hit, "PMR deallocation attempted allocation");
      Check(pmr.Snapshot().allocated_bytes == 0 && pmr.Snapshot().deallocation_count == 1,
            "PMR teardown counters do not reflect physical release");
    }
    acquired.resource.reset();
    Charges(ledger, manager, 0, 0, 0);
  }
  Check(complete, "PMR allocation fault sweep incomplete");
}

void ForcedCleanup() {
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  mem::MemoryManager manager(Policy());
  Configure(ledger, manager);
  auto request = Request(ledger, manager); request.requested_bytes = 8192;
  auto acquired = mem::AcquireReservationBackedMemoryResource(std::move(request));
  Check(acquired.ok(), "nested PMR cleanup fixture");
  {
    mem::ReservationBackedPmrMemoryResource pmr(acquired.resource.get(), "nested-pmr-container");
    {
      std::pmr::vector<std::pmr::string> values(&pmr);
      values.reserve(16);
      for (unsigned i = 0; i < 16; ++i) values.emplace_back(256, 'x');
      Check(manager.Snapshot().active_allocation_count == 17, "nested PMR actual buffers");
      fault::Arm(0);
    }
  }
  acquired.resource.reset();
  fault::Off();
  Check(!fault::hit, "nested PMR/resource destructor allocated under blackout");
  Charges(ledger, manager, 0, 0, 0);
  auto raw_owner = mem::AcquireReservationBackedMemoryResource(Request(ledger, manager));
  Check(raw_owner.ok() && raw_owner.resource->Allocate({137, 256, {}}).ok(), "raw adapter blackout owner");
  fault::Arm(0);
  raw_owner.resource.reset();
  fault::Off();
  Check(!fault::hit, "raw resource destructor allocated");
  Charges(ledger, manager, 0, 0, 0);
}
#endif

void ProfilesAndOwnership() {
  for (unsigned kind = 0; kind != 8; ++kind) {
    mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
    mem::MemoryManager manager(Policy()); Configure(ledger, manager);
    auto request = Request(ledger, manager);
    request.consumer_kind = static_cast<mem::ReservationBackedMemoryConsumerKind>(kind);
    auto acquired = mem::AcquireReservationBackedMemoryResource(std::move(request));
    Check(acquired.ok(), "defined consumer profile admission");
    auto block = acquired.resource->Allocate({137, 256, {}});
    Check(block.ok(), "defined consumer actual physical allocation");
    auto snapshot = manager.Snapshot();
    bool query_bound = false, session_bound = false, false_query = false;
    for (const auto& context : snapshot.contexts) {
      query_bound |= context.scope_kind == "query" && context.scope_id == "adapter-query-scope-label" && context.current_bytes == 137;
      session_bound |= context.scope_kind == "session" && context.scope_id == "adapter-session-scope-label" && context.current_bytes == 137;
      false_query |= context.scope_kind == "query" && context.scope_id == "adapter-resource-route-label";
    }
    Check(query_bound && session_bound && !false_query, "physical identities fabricated from route label");
    Check(!acquired.resource->Deallocate(block.pointer, 136, 256).ok(), "wrong-size deallocation accepted");
    Check(!acquired.resource->Deallocate(block.pointer, 137, 512).ok(), "wrong-alignment deallocation accepted");
    Charges(ledger, manager, 4096, 137, 1);
    Check(acquired.resource->DeallocateNoAlloc(block.pointer, 137, 256).ok(), "exact nonallocating owner release");
    Check(!acquired.resource->DeallocateNoAlloc(block.pointer, 137, 256).ok(), "stale pointer manufactured release");
    Check(acquired.resource->ReleaseNoAlloc().ok() && acquired.resource->ReleaseNoAlloc().ok(), "idempotent resource teardown");
    Check(acquired.resource->Snapshot().reserved_bytes == 0, "released adapter reports nominal reservation as live");
    Charges(ledger, manager, 0, 0, 0);
  }
  for (unsigned profile : {8u, 255u, 65535u}) {
    mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
    mem::MemoryManager manager(Policy()); Configure(ledger, manager);
    auto request = Request(ledger, manager);
    request.consumer_kind = static_cast<mem::ReservationBackedMemoryConsumerKind>(profile);
    auto r = mem::AcquireReservationBackedMemoryResource(std::move(request));
    Check(!r.ok(), "unknown consumer profile admitted");
    Charges(ledger, manager, 0, 0, 0);
  }
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  mem::MemoryManager manager(Policy()); Configure(ledger, manager);
  auto request = Request(ledger, manager);
  request.category = static_cast<mem::MemoryCategory>(65535);
  auto r = mem::AcquireReservationBackedMemoryResource(std::move(request));
  Check(!r.ok(), "unknown memory category admitted");
  Charges(ledger, manager, 0, 0, 0);
}

void SharedResourceConcurrency() {
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  mem::MemoryManager manager(Policy()); Configure(ledger, manager);
  auto acquired = mem::AcquireReservationBackedMemoryResource(Request(ledger, manager));
  Check(acquired.ok(), "shared resource fixture");
  mem::ReservationBackedPmrMemoryResource pmr(acquired.resource.get(), "concurrent-pmr");
  std::array<std::thread, 4> workers;
  std::barrier start(4);
  std::atomic<unsigned> errors{0};
  for (auto& worker : workers) worker = std::thread([&] {
    start.arrive_and_wait();
    for (unsigned i = 0; i != 96; ++i) {
      try {
        auto p = pmr.allocate(137, 256);
        std::memset(p, 0xa5, 137);
        pmr.deallocate(p, 137, 256);
        auto raw = acquired.resource->Allocate({137, 256, {}});
        if (!raw.ok()) { ++errors; continue; }
        std::memset(raw.pointer, 0x5a, 137);
        if (!acquired.resource->DeallocateNoAlloc(raw.pointer, 137, 256).ok()) ++errors;
      } catch (...) { ++errors; }
    }
  });
  for (auto& worker : workers) worker.join();
  Check(errors == 0, "concurrent adapter/PMR actual owners failed");
  const auto snapshot = pmr.Snapshot();
  Check(snapshot.allocation_count == 384 && snapshot.deallocation_count == 384 && snapshot.allocated_bytes == 0,
        "concurrent PMR counter history mismatch");
  Charges(ledger, manager, 4096, 0, 0);
  Check(acquired.resource->ReleaseNoAlloc().ok(), "shared resource final release");
  Charges(ledger, manager, 0, 0, 0);
}

void ParentRevocation() {
  for (unsigned mode = 0; mode != 5; ++mode) {
    mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
    mem::MemoryManager manager(Policy()); Configure(ledger, manager);
    auto request = Request(ledger, manager); request.lease_expires_at_ms = 42;
    mem::HierarchicalMemoryBudget root;
    root.scope = request.scope_chain.front(); root.hard_limit_bytes = 4096; root.provenance = Provenance();
    Check(ledger.SetBudget(std::move(root)).ok(), "retained root capacity fixture");
    auto acquired = mem::AcquireReservationBackedMemoryResource(request);
    Check(acquired.ok(), "retained parent fixture acquire");
    auto block = acquired.resource->Allocate({137, 256, {}});
    Check(block.ok(), "retained parent actual buffer");
    std::memset(block.pointer, 0xa5, 137);
    const auto token = acquired.resource->reservation_token();
    if (mode == 0) {
      auto r = ledger.Cancel(token);
      Check(!r.ok() && r.retained && r.newly_revoked && r.retained_bytes == 4096,
            "cancel did not report retained actual owner");
      auto again = ledger.Cancel(token);
      Check(!again.ok() && again.retained && !again.newly_revoked, "cancel revocation not idempotent");
    }
    if (mode == 1 || mode == 2) {
      auto r = mode == 1 ? ledger.CleanupOwner(request.owner_id) : ledger.CleanupExpiredLeases(42);
      Check(!r.ok() && r.cleaned_bytes == 0 && r.cleaned_reservation_count == 0 &&
            r.revoked_reservation_count == 1 && r.retained_bytes == 4096,
            "cleanup reported live retained buffers as cleaned");
      auto again = mode == 1 ? ledger.CleanupOwner(request.owner_id) : ledger.CleanupExpiredLeases(42);
      Check(!again.ok() && again.cleaned_bytes == 0 && again.revoked_reservation_count == 0 &&
            again.retained_bytes == 4096, "pending cleanup repeated incorrectly");
    }
    if (mode == 3) {
      auto r = ledger.Release(token);
      Check(!r.ok() && r.retained && r.newly_revoked && r.retained_bytes == 4096,
            "non-owner token release uncharged retained storage");
    }
    if (mode == 4) {
      Check(!ledger.ReleaseNoAlloc(token).ok(), "non-owner noalloc release uncharged retained storage");
    }
    auto parent = ledger.Snapshot();
    Check(parent.current_bytes == 4096 && parent.pending_revocation_count == 1 &&
          parent.retained_revoked_bytes == 4096, "pending parent revocation lost real capacity");
    Check(!acquired.resource->active(), "revoked adapter still active");
    Check(acquired.resource->Snapshot().reserved_bytes == 4096, "revoked owner hid its retained charge");
    auto denied = acquired.resource->Allocate({269, 512, {}});
    Check(!denied.ok() && denied.diagnostic.diagnostic_code == "SB_CEIC_012_MEMORY_RESOURCE.REVOKED",
          "revoked parent accepted new allocation or falsely reported payload released");
    bool intact = true;
    for (unsigned i = 0; i < 137; ++i) intact &= static_cast<unsigned char*>(block.pointer)[i] == 0xa5;
    Check(intact, "asynchronous parent cleanup freed/changed still-owned payload");
    auto competitor = request; competitor.owner_id = "competing-owner-label";
    auto blocked = mem::AcquireReservationBackedMemoryResource(competitor);
    Check(!blocked.ok(), "new grant bypassed capacity held by revoked owner");
    Charges(ledger, manager, 4096, 137, 1);
    Check(acquired.resource->DeallocateNoAlloc(block.pointer, 137, 256).ok(),
          "revoked owner could not release its own buffer");
    Charges(ledger, manager, 4096, 0, 0);
    Check(acquired.resource->ReleaseNoAlloc().ok(), "retained parent final owner release");
    parent = ledger.Snapshot();
    Check(parent.pending_revocation_count == 0 && parent.retained_revoked_bytes == 0,
          "final owner teardown left pending revocation accounting");
    Charges(ledger, manager, 0, 0, 0);
    auto retry = mem::AcquireReservationBackedMemoryResource(std::move(competitor));
    Check(retry.ok() && retry.resource->ReleaseNoAlloc().ok(), "actual root capacity not reusable after teardown");
  }
}

void RetainedLeaseOwnership() {
  for (unsigned mode = 0; mode != 2; ++mode) {
    mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
    mem::MemoryManager manager(Policy()); Configure(ledger, manager);
    mem::HierarchicalMemoryReservationRequest request;
    request.scope_chain = Request(ledger, manager).scope_chain;
    request.owner_id = "mixed-retained-owner";
    request.requested_bytes = 1024;
    request.category = mem::MemoryCategory::executor_query_reserved;
    request.provenance = Provenance(); request.lease_expires_at_ms = 42;
    auto first = ledger.Reserve(request);
    Check(first.ok(), "retained lease actual reservation");
    Check(!ledger.Retain(first.token).ok(), "uncommitted reservation acquired owning lease");
    Check(ledger.Commit(first.token).ok(), "retained lease commit");
    auto retained = ledger.Retain(first.token);
    Check(retained.ok() && retained.lease.live(), "committed reservation owning lease");
    Check(!ledger.Retain(first.token).ok(), "duplicate owning lease accepted");
    Check(!ledger.Retain({first.token.token_id, 1025}).ok() && !ledger.Retain({}).ok(),
          "invalid retained owner token accepted");
    auto second = ledger.Reserve(request);
    Check(second.ok() && ledger.Commit(second.token).ok(), "mixed plain reservation");
    auto cleanup = mode == 0 ? ledger.CleanupOwner(request.owner_id) : ledger.CleanupExpiredLeases(42);
    Check(!cleanup.ok() && cleanup.cleaned_bytes == 1024 && cleanup.cleaned_reservation_count == 1 &&
          cleanup.retained_bytes == 1024 && cleanup.revoked_reservation_count == 1,
          "mixed cleanup lost actual progress or falsely completed retained owner");
    auto snapshot = ledger.Snapshot();
    Check(snapshot.current_bytes == 1024 && snapshot.active_allocation_count == 1 &&
          snapshot.pending_revocation_count == 1, "mixed cleanup parent accounting");
    mem::HierarchicalMemoryReservationLease moved(std::move(retained.lease));
    Check(!retained.lease.valid() && moved.valid() && !moved.live(), "revoked owner move lost ownership state");
    fault::Arm(0);
    auto status = moved.Reset();
    const bool allocated = fault::hit;
    fault::Off();
    Check(status.ok() && !allocated && !moved.valid(), "retained owner reset allocated or lost charge");
    snapshot = ledger.Snapshot();
    Check(snapshot.current_bytes == 0 && snapshot.pending_revocation_count == 0 &&
          snapshot.retained_revoked_bytes == 0, "mixed final owner cleanup left real charge");
    Check((mode == 0 ? snapshot.owner_cleanup_count : snapshot.lease_expiry_cleanup_count) == 2,
          "retained final cleanup lost original revocation reason");
    Check(!ledger.Retain(first.token).ok(), "stale token acquired retained owner");
  }
}

void RevocationPublicationRace() {
  for (unsigned history = 0; history != 96; ++history) {
    mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
    mem::MemoryManager manager(Policy()); Configure(ledger, manager);
    auto acquired = mem::AcquireReservationBackedMemoryResource(Request(ledger, manager));
    Check(acquired.ok(), "revocation race actual owner");
    auto original = acquired.resource->Allocate({137, 256, {}});
    Check(original.ok(), "revocation race original buffer");
    mem::AllocationResult later;
    mem::HierarchicalMemoryBudgetOperationResult cancelled;
    std::barrier start(2);
    std::thread allocating([&] {
      start.arrive_and_wait(); later = acquired.resource->Allocate({269, 512, {}});
      if (later.ok()) std::memset(later.pointer, 0xa5, 269);
    });
    std::thread cancelling([&] { start.arrive_and_wait(); cancelled = ledger.Cancel(acquired.resource->reservation_token()); });
    allocating.join(); cancelling.join();
    Check(!cancelled.ok() && cancelled.retained, "racing revocation failed to retain parent capacity");
    Charges(ledger, manager, 4096, later.ok() ? 406 : 137, later.ok() ? 2 : 1);
    Check(!acquired.resource->active() && !acquired.resource->Allocate({1, 64, {}}).ok(),
          "publication admitted work after completed revocation");
    Check(acquired.resource->ReleaseNoAlloc().ok(), "racing retained owner final teardown");
    Charges(ledger, manager, 0, 0, 0);
  }
}
}
int main(int argc, char** argv) {
  std::set_terminate([] {
    std::fprintf(stderr, "TERMINATE fault-index=%ld checks=%u failures=%u\n",
                 fault::current_index, checks, failures);
    std::abort();
  });
  WarmMetrics();
#ifndef SB_ADAPTER_NO_ALLOC_OVERRIDE
  if (argc > 1) Sweep(argv[1]);
  else { Sweep("acquire"); Sweep("allocate"); Sweep("deallocate"); Sweep("release"); PmrFaults(); ForcedCleanup(); }
#else
  (void)argc; (void)argv;
#endif
  ProfilesAndOwnership(); SharedResourceConcurrency();
  ParentRevocation(); RetainedLeaseOwnership(); RevocationPublicationRace();
  std::cout << checks << " checks; " << injected << " allocation faults; " << failures << " failures\n";
  return failures ? 1 : 0;
}
