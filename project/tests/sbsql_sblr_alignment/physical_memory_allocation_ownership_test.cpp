// SPDX-License-Identifier: MPL-2.0
#include "memory.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <string_view>
#include <thread>

namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
void Arm(long n) { remaining = n; hit = false; }
void Off() { remaining = -1; }
void BeforeAllocation() {
  if (remaining >= 0 && remaining-- == 0) {
    remaining = 0; hit = true; throw std::bad_alloc();
  }
}
struct Aligned { void* pointer = nullptr; void* base = nullptr; std::size_t bytes = 0; bool zero = false; };
std::mutex tracked_mutex;
std::array<Aligned, 16384> tracked{};
std::size_t nonzero_frees = 0;
std::size_t Live() {
  std::lock_guard lock(tracked_mutex);
  std::size_t n = 0; for (const auto& p : tracked) if (p.pointer) ++n; return n;
}
void ExpectZero(void* p) {
  std::lock_guard lock(tracked_mutex);
  for (auto& slot : tracked) if (slot.pointer == p) { slot.zero = true; return; }
}
// Only after a failed assertion and after the fixture allocator has died.
// This keeps the deliberately broken baseline from exhausting the test host;
// it is never used as evidence that the product cleaned up.
void CleanupFailedFixture() {
  std::lock_guard lock(tracked_mutex);
  for (auto& slot : tracked) if (slot.pointer) { std::free(slot.base); slot = {}; }
}
}
#ifndef SB_PHYSICAL_NO_ALLOC_OVERRIDE
void* operator new(std::size_t n) {
  fault::BeforeAllocation();
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
void* operator new(std::size_t n, std::align_val_t a) {
  fault::BeforeAllocation();
  std::size_t alignment = static_cast<std::size_t>(a);
  if (alignment < alignof(void*)) alignment = alignof(void*);
  if (alignment == 0 || (alignment & (alignment - 1)) ||
      alignment > std::numeric_limits<std::size_t>::max() - sizeof(void*) ||
      n > std::numeric_limits<std::size_t>::max() - alignment - sizeof(void*)) throw std::bad_alloc();
  auto base = std::malloc(n + alignment + sizeof(void*));
  if (!base) throw std::bad_alloc();
  const auto raw = reinterpret_cast<std::uintptr_t>(base) + sizeof(void*);
  void* p = reinterpret_cast<void*>((raw + alignment - 1) & ~(alignment - 1));
  std::lock_guard lock(fault::tracked_mutex);
  for (auto& slot : fault::tracked) if (!slot.pointer) { slot = {p, base, n, false}; return p; }
  std::free(base); throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  try { return ::operator new(n, a); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return ::operator new(n, a, std::nothrow);
}
void operator delete(void* p, std::align_val_t) noexcept {
  if (!p) return;
  std::lock_guard lock(fault::tracked_mutex);
  for (auto& slot : fault::tracked) if (slot.pointer == p) {
    if (slot.zero) for (std::size_t i = 0; i < slot.bytes; ++i)
      if (static_cast<unsigned char*>(p)[i] != 0) { ++fault::nonzero_frees; break; }
    std::free(slot.base); slot = {}; return;
  }
  std::abort(); // The interposed allocator never owns an unrecorded aligned block.
}
void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
#endif

namespace {
namespace mem = scratchbird::core::memory;
using Code = scratchbird::core::platform::StatusCode;
unsigned checks = 0, failures = 0, injected = 0;
void Check(bool good, std::string_view why) {
  ++checks; if (!good) { ++failures; std::cerr << "FAIL " << why << '\n'; }
}
mem::AllocationPolicy Policy() {
  auto p = mem::DefaultLocalEngineMemoryPolicy();
  p.hard_limit_bytes = 1024 * 1024; p.soft_limit_bytes = 0;
  p.per_context_limit_bytes = 1024 * 1024;
  p.zero_memory_on_allocate = true; p.zero_memory_on_release = true;
  return p;
}
mem::MemoryTag Tag() {
  mem::MemoryTag t;
  t.purpose = "physical-memory-owner-failure-contract";
  t.owner = "physical-memory-test-owner-label";
  t.context_id = "physical-memory-test-context-label";
  t.database_id = "physical-memory-test-database-label";
  t.session_id = "physical-memory-test-session-label";
  t.query_id = "physical-memory-test-query-label";
  return t;
}
void CheckAccounting(const mem::MemoryAccountingSnapshot& s, std::size_t bytes, std::size_t owners) {
  Check(s.current_bytes == bytes && s.active_allocation_count == owners, "allocator live charges mismatch");
  Check(s.sharded_accounting_current_bytes == bytes &&
        s.sharded_accounting_active_allocation_count == owners, "shared accounting diverged from physical ownership");
  for (const auto& c : s.contexts) {
    const bool original_owner = c.scope_id == "physical-memory-test-" + c.scope_kind + "-label";
    Check(c.current_bytes == (original_owner ? bytes : 0), "context physical charges mismatch");
  }
}
void WarmMetrics() {
  mem::BoundedAllocator a(Policy());
  auto r = a.Allocate(16, 64, Tag());
  Check(r.ok() && a.Deallocate(r.pointer, Tag()).ok(), "actual metric/allocator warmup");
}
#ifndef SB_PHYSICAL_NO_ALLOC_OVERRIDE
void AllocationSweep() {
  bool complete = false;
  for (long n = 0; n != 8192; ++n) {
    {
      mem::BoundedAllocator a(Policy());
      auto tag = Tag();
      mem::AllocationResult r;
      bool threw = false;
      fault::Arm(n);
      try { r = a.Allocate(137, 256, std::move(tag)); } catch (const std::bad_alloc&) { threw = true; }
      fault::Off();
      Check(!threw, "allocation failure escaped typed allocator API");
      if (r.ok()) {
        if (fault::hit) Check(a.Snapshot().telemetry_truncation_count > 0,
                             "successful pressure allocation silently dropped telemetry detail");
        CheckAccounting(a.Snapshot(), 137, 1);
        Check(fault::Live() == 1, "successful allocation has no unique real block");
        bool zero = true;
        for (unsigned i = 0; i != 137; ++i) zero &= static_cast<unsigned char*>(r.pointer)[i] == 0;
        Check(zero && reinterpret_cast<std::uintptr_t>(r.pointer) % 256 == 0, "physical zeroing/alignment mismatch");
        std::memset(r.pointer, 0xa5, 137); fault::ExpectZero(r.pointer);
        Check(a.Deallocate(r.pointer, Tag()).ok(), "returned pointer not releasable");
      } else {
        CheckAccounting(a.Snapshot(), 0, 0);
        Check(a.Snapshot().failure_count == 1, "one refused allocation must count exactly one failure");
        Check(fault::Live() == 0, "failed allocation orphaned an actual aligned block");
        if (!threw) Check(r.status.code == Code::memory_allocation_failed, "allocation refusal lacks typed OOM");
      }
      CheckAccounting(a.Snapshot(), 0, 0);
      if (fault::hit) ++injected;
      else { Check(r.ok(), "uninjected allocation failed"); complete = true; }
    }
    Check(fault::Live() == 0, "fixture allocator destruction left actual physical storage");
    fault::CleanupFailedFixture();
    if (complete) break;
  }
  Check(complete, "physical allocation fault sweep incomplete");
}
void DeallocationSweep() {
  bool complete = false;
  for (long n = 0; n != 2048; ++n) {
    {
      mem::BoundedAllocator a(Policy());
      auto r = a.Allocate(137, 256, Tag());
      Check(r.ok(), "deallocation fixture actual allocation");
      std::memset(r.pointer, 0xa5, 137); fault::ExpectZero(r.pointer);
      auto tag = Tag();
      mem::DeallocationResult d;
      bool threw = false;
      fault::Arm(n);
      try { d = a.Deallocate(r.pointer, std::move(tag)); } catch (const std::bad_alloc&) { threw = true; }
      fault::Off();
      Check(!threw, "deallocation failure escaped API");
      if (d.ok() && !threw) {
        CheckAccounting(a.Snapshot(), 0, 0);
        Check(fault::Live() == 0, "successful deallocation did not free memory");
      } else {
        CheckAccounting(a.Snapshot(), 137, 1);
        Check(fault::Live() == 1, "failed deallocation lost real owner");
        Check(a.Deallocate(r.pointer, Tag()).ok(), "failed deallocation not retryable");
      }
      if (fault::hit) ++injected;
      else { Check(d.ok() && !threw, "uninjected deallocation failed"); complete = true; }
    }
    Check(fault::Live() == 0, "deallocation fixture left real memory");
    fault::CleanupFailedFixture();
    if (complete) break;
  }
  Check(complete, "deallocation fault sweep incomplete");
}
void ScopedResetSweep() {
  bool complete = false;
  for (long n = 0; n != 2048; ++n) {
    {
      mem::MemoryManager m(Policy());
      auto r = m.AllocateScoped(137, 256, Tag());
      Check(r.ok(), "scoped fixture actual allocation");
      void* original = r.allocation.data();
      std::memset(original, 0xa5, 137); fault::ExpectZero(original);
      bool threw = false;
      mem::DeallocationResult d;
      fault::Arm(n);
      try { d = r.allocation.Reset(); } catch (const std::bad_alloc&) { threw = true; }
      fault::Off();
      Check(!threw, "scoped reset allocation failure escaped");
      if (d.ok() && !threw) {
        Check(!r.allocation.valid(), "successful scoped reset retained owner");
        CheckAccounting(m.Snapshot(), 0, 0);
      } else {
        Check(r.allocation.valid() && r.allocation.data() == original,
              "failed scoped reset erased its own pointer");
        CheckAccounting(m.Snapshot(), 137, 1);
        if (r.allocation.valid()) Check(r.allocation.Reset().ok(), "scoped reset could not retry");
        else (void)m.Deallocate(original, Tag()); // Cleanup only after the failed owner assertion.
      }
      if (fault::hit) ++injected;
      else { complete = true; Check(d.ok() && !threw, "uninjected scoped reset failed"); }
    }
    Check(fault::Live() == 0, "scoped fixture left real memory");
    fault::CleanupFailedFixture();
    if (complete) break;
  }
  Check(complete, "scoped reset fault sweep incomplete");
}
#endif
}
namespace {
bool BytesEqual(void* pointer, std::size_t bytes, unsigned char value) {
  for (std::size_t i = 0; i < bytes; ++i)
    if (static_cast<unsigned char*>(pointer)[i] != value) return false;
  return true;
}
#ifndef SB_PHYSICAL_NO_ALLOC_OVERRIDE
void ReallocationSweep() {
  bool complete = false;
  for (long n = 0; n < 8192 && !complete; ++n) {
    mem::BoundedAllocator a(Policy());
    auto original = a.Allocate(137, 256, Tag());
    Check(original.ok(), "reallocation original owner");
    std::memset(original.pointer, 0xa5, 137);
    fault::ExpectZero(original.pointer);
    auto tag = Tag();
    mem::AllocationResult r;
    bool threw = false;
    fault::Arm(n);
    try { r = a.Reallocate(original.pointer, 269, 512, std::move(tag)); }
    catch (const std::bad_alloc&) { threw = true; }
    fault::Off();
    Check(!threw, "reallocation escaped typed failure");
    if (r.ok() && !threw) {
      CheckAccounting(a.Snapshot(), 269, 1);
      Check(BytesEqual(r.pointer, 137, 0xa5), "reallocation lost original bytes");
      Check(BytesEqual(static_cast<unsigned char*>(r.pointer) + 137, 132, 0),
            "zeroed reallocation extension is not zero");
      Check(a.Deallocate(r.pointer, Tag()).ok(), "reallocation final release");
    } else {
      CheckAccounting(a.Snapshot(), 137, 1);
      Check(BytesEqual(original.pointer, 137, 0xa5), "failed reallocation changed original bytes");
      Check(a.Deallocate(original.pointer, Tag()).ok(), "failed reallocation lost original owner");
    }
    Check(fault::Live() == 0, "reallocation orphaned replacement");
    if (fault::hit) ++injected; else complete = true;
  }
  Check(complete, "reallocation sweep incomplete");
}

void CompositeAllocationSweeps() {
  for (int profile = 0; profile != 4; ++profile) {
    bool complete = false;
    for (long n = 0; n < 8192 && !complete; ++n) {
      {
        mem::MemoryManager m(Policy());
        auto arena = m.CreateArena(Tag());
        auto tag = Tag();
        mem::PageBufferRequest page; page.page_size = 4096; page.tag = Tag();
        mem::ProtectedMemoryRequest protected_request;
        protected_request.bytes = 137; protected_request.alignment = 4096;
        protected_request.tag = Tag();
        mem::ScopedAllocationResult scoped;
        mem::ScopedPageBufferResult pages;
        mem::ProtectedBufferResult protected_result;
        mem::AllocationResult chunk;
        bool threw = false, success = false;
        fault::Arm(n);
        try {
          if (profile == 0) { scoped = m.AllocateScoped(137, 256, std::move(tag)); success = scoped.ok(); }
          if (profile == 1) { pages = m.AllocateScopedPageBuffer(std::move(page)); success = pages.ok(); }
          if (profile == 2) { protected_result = m.AllocateProtected(std::move(protected_request)); success = protected_result.ok(); }
          if (profile == 3) { chunk = arena.Allocate(137, 256); success = chunk.ok(); }
        } catch (const std::bad_alloc&) { threw = true; }
        fault::Off();
        Check(!threw, "composite allocation escaped typed failure");
        Check(m.Snapshot().active_allocation_count == (success ? 1u : 0u),
              "composite allocation publication differs from actual parent charge");
        Check(fault::Live() == (success ? 1u : 0u), "composite allocation orphaned actual storage");
        if (success) {
          Check(scoped.allocation.Reset().ok(), "scoped composite cleanup");
          Check(pages.buffer.Reset().ok(), "page composite cleanup");
          Check(protected_result.buffer.Reset().ok(), "protected composite cleanup");
          Check(arena.Reset().ok(), "arena composite cleanup");
        }
        CheckAccounting(m.Snapshot(), 0, 0);
        if (fault::hit) ++injected; else { Check(success, "uninjected composite allocation failed"); complete = true; }
      }
      Check(fault::Live() == 0, "composite fixture leaked actual storage");
      fault::CleanupFailedFixture();
    }
    Check(complete, "composite fault sweep incomplete");
  }
}

void CompositeResetSweeps() {
  for (int profile = 0; profile != 3; ++profile) {
    bool complete = false;
    for (long n = 0; n < 2048 && !complete; ++n) {
      mem::MemoryManager m(Policy());
      auto arena = m.CreateArena(Tag());
      mem::PageBufferRequest page; page.page_size = 4096; page.tag = Tag();
      mem::ProtectedMemoryRequest request; request.bytes = 137; request.alignment = 4096; request.tag = Tag();
      mem::ScopedPageBufferResult pages;
      mem::ProtectedBufferResult protected_result;
      void* owner = nullptr;
      if (profile == 0) { pages = m.AllocateScopedPageBuffer(std::move(page)); owner = pages.buffer.data(); }
      if (profile == 1) { protected_result = m.AllocateProtected(std::move(request)); owner = protected_result.buffer.data(); }
      if (profile == 2) { auto r = arena.Allocate(70000, 256); owner = r.pointer; }
      Check(owner != nullptr, "composite reset fixture");
      std::memset(owner, 0xa5, 137);
      const auto before = m.Snapshot().current_bytes;
      mem::DeallocationResult d;
      bool threw = false;
      fault::Arm(n);
      try {
        if (profile == 0) d = pages.buffer.Reset();
        if (profile == 1) d = protected_result.buffer.Reset();
        if (profile == 2) d = arena.Reset();
      } catch (const std::bad_alloc&) { threw = true; }
      fault::Off();
      Check(!threw, "composite reset escaped typed failure");
      if (d.ok() && !threw) {
        CheckAccounting(m.Snapshot(), 0, 0);
        Check(fault::Live() == 0, "composite successful release retained real storage");
      } else {
        CheckAccounting(m.Snapshot(), before, 1);
        Check(fault::Live() == 1, "failed composite reset lost actual storage");
        Check(BytesEqual(owner, 137, 0xa5), "failed protected/composite reset modified live bytes");
        Check(pages.buffer.Reset().ok() && protected_result.buffer.Reset().ok() && arena.Reset().ok(),
              "composite reset could not retry");
      }
      if (fault::hit) ++injected; else complete = true;
    }
    Check(complete, "composite reset sweep incomplete");
  }
}

void ForcedCleanupBlackouts() {
  mem::MemoryManager m(Policy());
  {
    auto scoped = m.AllocateScoped(137, 256, Tag());
    auto replacement = m.AllocateScoped(67, 128, Tag());
    auto arena = m.CreateArena(Tag());
    Check(arena.Allocate(70000, 256).ok(), "blackout arena actual first chunk");
    Check(arena.Allocate(70000, 256).ok(), "blackout arena actual second chunk");
    mem::PageBufferRequest page; page.page_size = 4096; page.tag = Tag();
    auto pages = m.AllocateScopedPageBuffer(std::move(page));
    mem::ProtectedMemoryRequest request; request.bytes = 137; request.alignment = 4096; request.tag = Tag();
    auto protected_result = m.AllocateProtected(std::move(request));
    Check(scoped.ok() && replacement.ok() && pages.ok() && protected_result.ok(), "blackout live owners");
    std::memset(protected_result.buffer.data(), 0xa5, 137);
    fault::ExpectZero(protected_result.buffer.data());
    fault::Arm(0);
    scoped.allocation = std::move(replacement.allocation);
    // All five actual owner destructors run while every allocation fails.
  }
  fault::Off();
  Check(!fault::hit, "forced owner cleanup attempted dynamic allocation");
  CheckAccounting(m.Snapshot(), 0, 0);
  Check(fault::Live() == 0, "forced cleanup left physical memory");
  auto tag = Tag();
  int foreign = 0;
  fault::Arm(0);
  const auto null_status = m.allocator()->DeallocateNoAlloc(nullptr);
  const auto foreign_status = m.allocator()->DeallocateNoAlloc(&foreign);
  fault::Off();
  Check(null_status.ok() && foreign_status.code == Code::memory_unknown_pointer && !fault::hit,
        "nonalloc terminal invalid-pointer result");
  {
    mem::BoundedAllocator a(Policy());
    Check(a.Allocate(137, 256, std::move(tag)).ok(), "allocator context-teardown fixture");
    fault::Arm(0);
  }
  fault::Off();
  Check(!fault::hit && fault::Live() == 0, "allocator context teardown did not reclaim raw owners");
}
#endif

void ConcurrentOwners() {
  mem::BoundedAllocator a(Policy());
  std::atomic<unsigned> errors{0};
  std::array<std::thread, 4> workers;
  std::barrier start(4);
  for (auto& worker : workers) worker = std::thread([&] {
    start.arrive_and_wait();
    for (unsigned i = 0; i != 96; ++i) {
      auto original = a.Allocate(137, 256, Tag());
      if (!original.ok()) { ++errors; continue; }
      std::memset(original.pointer, 0xa5, 137);
      auto replacement = a.Reallocate(original.pointer, 269, 512, Tag());
      if (!replacement.ok()) { ++errors; (void)a.DeallocateNoAlloc(original.pointer); continue; }
      if (!BytesEqual(replacement.pointer, 137, 0xa5)) ++errors;
      if (!a.DeallocateNoAlloc(replacement.pointer).ok()) ++errors;
    }
  });
  for (auto& worker : workers) worker.join();
  Check(errors == 0, "concurrent actual allocation/reallocation/release history");
  CheckAccounting(a.Snapshot(), 0, 0);
}

void ReallocationOwnerIsolation() {
  mem::BoundedAllocator a(Policy());
  auto original = a.Allocate(137, 256, Tag());
  Check(original.ok(), "owner-isolation original allocation");
  std::memset(original.pointer, 0xa5, 137);
  for (unsigned profile = 0; profile != 10; ++profile) {
    auto tag = Tag();
    switch (profile) {
      case 0: tag.owner += "-foreign"; break;
      case 1: tag.context_id += "-foreign"; break;
      case 2: tag.database_id += "-foreign"; break;
      case 3: tag.session_id += "-foreign"; break;
      case 4: tag.transaction_id = "foreign-transaction"; break;
      case 5: tag.statement_id = "foreign-statement"; break;
      case 6: tag.query_id += "-foreign"; break;
      case 7: tag.category = mem::MemoryCategory::page_buffer; break;
      case 8: tag.lifetime = mem::MemoryLifetime::process; break;
      case 9: tag.subsystem = static_cast<mem::Subsystem>(65535); break;
    }
    auto r = a.Reallocate(original.pointer, 269, 512, std::move(tag));
    Check(!r.ok() && r.status.code == Code::memory_invalid_request,
          "reallocation silently transferred owner/scope/class");
    CheckAccounting(a.Snapshot(), 137, 1);
    Check(BytesEqual(original.pointer, 137, 0xa5), "foreign reallocation changed bytes");
  }
  Check(a.DeallocateNoAlloc(original.pointer).ok(), "owner-isolation final cleanup");
}

void ConcurrentReplacement() {
  for (unsigned history = 0; history != 96; ++history) {
    mem::BoundedAllocator a(Policy());
    auto original = a.Allocate(137, 256, Tag());
    Check(original.ok(), "competing reallocation original");
    std::memset(original.pointer, 0xa5, 137);
    std::array<mem::AllocationResult, 2> results;
    std::barrier start(2);
    std::thread first([&] { start.arrive_and_wait(); results[0] = a.Reallocate(original.pointer, 269, 512, Tag()); });
    std::thread second([&] { start.arrive_and_wait(); results[1] = a.Reallocate(original.pointer, 269, 512, Tag()); });
    first.join(); second.join();
    Check(static_cast<unsigned>(results[0].ok()) + static_cast<unsigned>(results[1].ok()) == 1,
          "competing reallocation consumed original twice");
    CheckAccounting(a.Snapshot(), 269, 1);
    for (auto& r : results) if (r.ok()) {
      Check(BytesEqual(r.pointer, 137, 0xa5), "competing replacement copied stale storage");
      Check(a.DeallocateNoAlloc(r.pointer).ok(), "competing replacement final cleanup");
    }
    CheckAccounting(a.Snapshot(), 0, 0);
  }
}
}
int main() {
  WarmMetrics();
#ifndef SB_PHYSICAL_NO_ALLOC_OVERRIDE
  AllocationSweep(); DeallocationSweep(); ScopedResetSweep();
  ReallocationSweep(); CompositeAllocationSweeps(); CompositeResetSweeps(); ForcedCleanupBlackouts();
  Check(fault::nonzero_frees == 0, "released physical memory was not zeroized");
#endif
  ConcurrentOwners();
  ReallocationOwnerIsolation(); ConcurrentReplacement();
  std::cout << checks << " checks; " << injected << " allocation faults; " << failures << " failures\n";
  return failures ? 1 : 0;
}
