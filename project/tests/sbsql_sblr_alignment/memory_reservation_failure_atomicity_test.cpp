// SPDX-License-Identifier: MPL-2.0
#include "hierarchical_memory_budget_ledger.hpp"

#include <atomic>
#include <barrier>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>
#include <thread>

namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
thread_local bool sustained = false;
void Arm(long n, bool all = false) { remaining = n; hit = false; sustained = all; }
void Disarm() { remaining = -1; sustained = false; }
}
#ifndef SB_MEMORY_FAULT_NO_ALLOC_OVERRIDE
void* operator new(std::size_t n) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::hit = true;
    if (fault::sustained) fault::remaining = 0;
    throw std::bad_alloc();
  }
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try { return ::operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  return ::operator new(n, std::nothrow);
}
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
mem::HierarchicalMemoryBudgetProvenance Provenance() {
  mem::HierarchicalMemoryBudgetProvenance p;
  p.source = mem::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
  p.source_label = "real-ledger-failure-contract-runtime-policy";
  return p;
}
mem::HierarchicalMemoryReservationRequest Request(mem::u64 bytes = 128) {
  mem::HierarchicalMemoryReservationRequest r;
  r.scope_chain = {
    {mem::HierarchicalMemoryScopeKind::process, "process-accounting-long-scope-label"},
    {mem::HierarchicalMemoryScopeKind::session, "session-accounting-long-scope-label"},
    {mem::HierarchicalMemoryScopeKind::query, "query-accounting-long-scope-label"}};
  r.category = mem::MemoryCategory::executor_query_reserved;
  r.memory_class = "retained-result-test-memory-class";
  r.owner_id = "retained-result-test-owner-label";
  r.requested_bytes = bytes;
  r.provenance = Provenance();
  r.lease_expires_at_ms = 42;
  return r;
}
mem::ShardedMemoryAccountingEvent Event(mem::u64 bytes = 128) {
  mem::ShardedMemoryAccountingEvent e;
  e.bytes = bytes;
  e.tag.category = mem::MemoryCategory::page_buffer;
  e.tag.context_id = "accounting-context-long-label";
  e.tag.owner = "accounting-owner-long-label";
  e.scope_ids = {e.tag.context_id, "parent-scope-long-label", "", "parent-scope-long-label"};
  return e;
}
void Empty(const mem::ShardedMemoryAccountingSnapshot& s) {
  Check(s.current_bytes == 0 && s.reserved_bytes == 0 &&
        s.active_allocation_count == 0 && s.active_reservation_count == 0,
        "accounting failure/cleanup must leave zero live charges");
  for (const auto& c : s.contexts)
    Check(c.current_bytes == 0 && c.active_allocation_count == 0, "context charge leaked");
  for (const auto& o : s.owners)
    Check(o.current_bytes == 0 && o.active_allocation_count == 0, "owner charge leaked");
}
void Empty(const mem::HierarchicalMemoryBudgetSnapshot& s) {
  Check(s.current_bytes == 0 && s.reserved_bytes == 0 &&
        s.active_allocation_count == 0 && s.active_reservation_count == 0,
        "parent ledger failure/cleanup must leave zero live charges");
  for (const auto& x : s.scopes)
    Check(x.current_bytes == 0 && x.reserved_bytes == 0 && x.active_bytes == 0 &&
          x.active_reservation_count == 0 && x.active_allocation_count == 0 &&
          x.priority_weight_total == 0, "parent chain charge leaked");
  for (const auto& x : s.classes)
    Check(x.reserved_bytes == 0 && x.active_bytes == 0, "memory class charge leaked");
}
#ifndef SB_MEMORY_FAULT_NO_ALLOC_OVERRIDE
void ReserveFaults() {
  for (bool hierarchy : {false, true}) for (bool live_seed : {false, true}) {
    bool exhausted = false;
    for (long nth = 0; nth < 1024; ++nth) {
      bool threw = false, ok = false;
      if (hierarchy) {
        mem::HierarchicalMemoryBudgetLedger l(3, 5);
        mem::HierarchicalMemoryReservationToken seed;
        if (live_seed) {
          auto s = l.Reserve(Request(17)); seed = s.token;
          Check(s.ok() && l.Commit(seed).ok(), "seed parent reservation");
        }
        auto request = Request();
        mem::HierarchicalMemoryReservationResult r;
        fault::Arm(nth, true);
        try { r = l.Reserve(std::move(request)); } catch (const std::bad_alloc&) { threw = true; }
        fault::Disarm();
        ok = r.ok();
        if (ok) Check(l.Release(r.token).ok(), "successful parent reserve must be releasable");
        if (live_seed) {
          const auto s = l.Snapshot();
          Check(s.current_bytes == 17 && s.reserved_bytes == 0 &&
                s.active_allocation_count == 1 && s.active_reservation_count == 0,
                "failed reservation changed the existing parent owner");
          Check(l.Release(seed).ok(), "seed parent release");
        }
        Empty(l.Snapshot());
        if (fault::hit && !threw)
          Check(r.status.code == Code::memory_allocation_failed && !r.token.valid(),
                "failed parent admission must return typed allocation failure without token");
        auto retry = l.Reserve(Request());
        Check(retry.ok() && l.Commit(retry.token).ok() && l.Release(retry.token).ok(),
              "parent ledger could not retry after failed preparation");
        Empty(l.Snapshot());
      } else {
        mem::ShardedMemoryAccountingLedger l(3);
        mem::ShardedMemoryAccountingToken seed;
        if (live_seed) {
          auto s = l.Reserve(Event(17)); seed = s.token;
          Check(s.ok() && l.Commit(seed).ok(), "seed accounting reservation");
        }
        auto event = Event();
        mem::ShardedMemoryAccountingResult r;
        fault::Arm(nth, true);
        try { r = l.Reserve(std::move(event)); } catch (const std::bad_alloc&) { threw = true; }
        fault::Disarm();
        ok = r.ok();
        if (ok) Check(l.Release(r.token).ok(), "successful accounting reserve must be releasable");
        if (live_seed) {
          const auto s = l.Snapshot();
          Check(s.current_bytes == 17 && s.reserved_bytes == 0 &&
                s.active_allocation_count == 1 && s.active_reservation_count == 0,
                "failed reservation changed the existing accounting owner");
          Check(l.Release(seed).ok(), "seed accounting release");
        }
        Empty(l.Snapshot());
        if (fault::hit && !threw)
          Check(r.status.code == Code::memory_allocation_failed && !r.token.valid(),
                "failed accounting admission must return typed allocation failure without token");
        auto retry = l.Reserve(Event());
        Check(retry.ok() && l.Commit(retry.token).ok() && l.Release(retry.token).ok(),
              "accounting ledger could not retry after failed preparation");
        Empty(l.Snapshot());
      }
      Check(!threw, "reserve allocation failure escaped typed API");
      if (fault::hit) ++injected;
      else { Check(ok, "uninjected reserve failed"); exhausted = true; break; }
    }
    Check(exhausted, "allocation sweep did not reach successful terminal iteration");
  }
}
void TerminalBlackout() {
  for (bool commit : {false, true}) {
    mem::ShardedMemoryAccountingLedger l(3);
    auto r = l.Reserve(Event());
    Check(r.ok(), "blackout accounting reserve");
    bool threw = false, ok = false;
    fault::Arm(0, true);
    try { ok = (!commit || l.Commit(r.token).ok()) && l.Release(r.token).ok(); }
    catch (const std::bad_alloc&) { threw = true; }
    fault::Disarm();
    Check(!threw && ok && !fault::hit, "valid accounting commit/release must not allocate");
    Empty(l.Snapshot());
    for (unsigned op = 0; op != 4; ++op) {
      mem::HierarchicalMemoryBudgetLedger h(3, 5);
      auto q = h.Reserve(Request());
      Check(q.ok(), "blackout parent reserve");
      std::string owner = Request().owner_id;
      threw = false; ok = false;
      fault::Arm(0, true);
      try {
        ok = !commit || h.Commit(q.token).ok();
        if (op == 0) ok &= h.Release(q.token).ok();
        if (op == 1) ok &= h.Cancel(q.token).ok();
        if (op == 2) { auto c = h.CleanupOwner(std::move(owner)); ok &= c.ok() && c.cleaned_bytes == 128; }
        if (op == 3) { auto c = h.CleanupExpiredLeases(42); ok &= c.ok() && c.cleaned_bytes == 128; }
      } catch (const std::bad_alloc&) { threw = true; }
      fault::Disarm();
      Check(!threw && ok && !fault::hit, "valid parent commit/release/cancel/owner/expiry cleanup must not allocate");
      Empty(h.Snapshot());
    }
  }
}
#endif
void Overflow() {
  constexpr auto max = std::numeric_limits<mem::u64>::max();
  for (bool commit : {false, true}) {
    mem::ShardedMemoryAccountingLedger l(3);
    auto first = l.Reserve(Event(max - 1));
    Check(first.ok(), "near-max accounting reservation");
    if (commit) Check(l.Commit(first.token).ok(), "near-max accounting commit");
    auto other = Event(2); other.tag.context_id += "-different";
    auto second = l.Reserve(std::move(other));
    Check(!second.ok() && !second.token.valid(), "global accounting sum must not wrap across shards");
    if (second.ok()) (void)l.Release(second.token);
    Check(l.Release(first.token).ok(), "near-max accounting release");
    Empty(l.Snapshot());

    mem::HierarchicalMemoryBudgetLedger h(3, 5);
    auto request = Request(max - 1);
    mem::HierarchicalMemoryBudget budget;
    budget.scope = request.scope_chain.front(); budget.hard_limit_bytes = max;
    budget.provenance = Provenance();
    Check(h.SetBudget(std::move(budget)).ok(), "near-max parent budget");
    auto a = h.Reserve(request);
    Check(a.ok(), "near-max parent reservation");
    if (commit) Check(h.Commit(a.token).ok(), "near-max parent commit");
    request.requested_bytes = 2;
    auto b = h.Reserve(request);
    Check(!b.ok() && !b.token.valid(), "parent budget sum must not wrap");
    if (b.ok()) (void)h.Release(b.token);
    Check(h.Release(a.token).ok(), "near-max parent release");
    Empty(h.Snapshot());
  }
}
void WeightBoundaries() {
  constexpr auto max = std::numeric_limits<mem::u64>::max();
  for (mem::u64 weight : {mem::u64{0}, mem::u64{1}, max - 1, max})
    for (int priority : {-1, 0, 1, std::numeric_limits<int>::max()}) {
      mem::HierarchicalMemoryBudgetLedger l(3, 5);
      auto q = Request(1); q.weight = weight; q.priority = priority;
      const auto normalized = weight == 0 ? 1 : weight;
      const auto p = static_cast<mem::u64>(priority < 0 ? 0 : priority);
      const bool fits = normalized <= max - p;
      auto r = l.Reserve(q);
      Check(r.ok() == fits, "priority and weight arithmetic admission mismatch");
      if (r.ok()) {
        for (const auto& s : l.Snapshot().scopes)
          Check(s.priority_weight_total == normalized + p, "priority accounting mismatch");
        if (normalized + p == max) {
          auto next = l.Reserve(Request(1));
          Check(!next.ok(), "cumulative scope priority weight wrapped");
          if (next.ok()) (void)l.Release(next.token);
        }
        Check(l.Release(r.token).ok(), "weight boundary release");
      }
      Empty(l.Snapshot());
    }
}
void Concurrent() {
  for (unsigned iteration = 0; iteration < 32; ++iteration) {
    mem::HierarchicalMemoryBudgetLedger l(7, 11);
    mem::HierarchicalMemoryBudget b;
    b.scope = Request().scope_chain.front(); b.hard_limit_bytes = 128;
    b.provenance = Provenance();
    Check(l.SetBudget(std::move(b)).ok(), "concurrent parent budget");
    std::barrier gate(5);
    std::atomic<unsigned> admitted = 0, errors = 0;
    std::thread threads[4];
    for (auto& t : threads) t = std::thread([&] {
      auto request = Request();
      gate.arrive_and_wait();
      auto r = l.Reserve(std::move(request));
      if (r.ok()) ++admitted;
      gate.arrive_and_wait();
      if (r.ok() && (!l.Commit(r.token).ok() || !l.Release(r.token).ok())) ++errors;
    });
    gate.arrive_and_wait();
    gate.arrive_and_wait();
    for (auto& t : threads) t.join();
    Check(admitted == 1 && errors == 0, "contending parent reservations bypassed budget or lost owner");
    Empty(l.Snapshot());
  }
}
}
int main() {
#ifndef SB_MEMORY_FAULT_NO_ALLOC_OVERRIDE
  ReserveFaults();
  TerminalBlackout();
#endif
  Overflow();
  WeightBoundaries();
  Concurrent();
  std::cout << checks << " checks; " << injected << " allocation faults; " << failures << " failures\n";
  return failures ? 1 : 0;
}
