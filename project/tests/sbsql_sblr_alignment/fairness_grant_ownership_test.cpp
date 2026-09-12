// SPDX-License-Identifier: MPL-2.0
#include "memory_fairness_scheduler.hpp"
#include <atomic>
#include <array>
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
void Arm(long n) { remaining = n; hit = false; }
void Off() { remaining = -1; }
}
#ifndef SB_FAIRNESS_NO_ALLOC_OVERRIDE
void* operator new(std::size_t n) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::hit = true; fault::remaining = 0; throw std::bad_alloc();
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
void Check(bool good, std::string_view what) {
  ++checks; if (!good) { ++failures; std::cerr << "FAIL " << what << '\n'; }
}
mem::HierarchicalMemoryBudgetProvenance Provenance() {
  mem::HierarchicalMemoryBudgetProvenance p;
  p.source = mem::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  p.source_label = "fairness-owner-actual-runtime-policy"; return p;
}
mem::MemoryFairnessScopePolicy Policy(mem::u64 hard = 1024, mem::u64 soft = 0) {
  mem::MemoryFairnessScopePolicy p;
  p.scope = {mem::HierarchicalMemoryScopeKind::process, "fairness-owner-process-label"};
  p.hard_max_bytes = hard; p.soft_max_bytes = soft; p.provenance = Provenance(); return p;
}
mem::MemoryFairnessRequest Request(mem::u64 bytes = 64) {
  mem::MemoryFairnessRequest r;
  r.scope_chain = {Policy().scope, {mem::HierarchicalMemoryScopeKind::query, "fairness-owner-query-label"}};
  r.owner_id = "fairness-owner-request-label"; r.requested_bytes = bytes;
  r.memory_class = "fairness-owner-actual-class"; r.provenance = Provenance();
  r.lease_expires_at_ms = 42;
  r.category = mem::MemoryCategory::executor_query_reserved; return r;
}
template<class Scheduler>
auto Terminal(Scheduler& s, mem::MemoryFairnessGrantToken token) {
  if constexpr (requires { s.ReleaseNoAlloc(token); }) return s.ReleaseNoAlloc(token);
  else return s.Release(token).status;
}
void Charges(mem::MultiTenantMemoryFairnessScheduler& scheduler,
             mem::HierarchicalMemoryBudgetLedger& ledger, mem::u64 bytes, mem::u64 count) {
  const auto local = scheduler.Snapshot(); const auto parent = ledger.Snapshot();
  Check(local.active_bytes == bytes, "exact actual scheduler ownership bytes");
  Check(parent.current_bytes == bytes && parent.reserved_bytes == 0 &&
        parent.active_allocation_count == count && parent.active_reservation_count == 0,
        "exact actual committed parent ownership");
  for (const auto& scope : local.scopes)
    Check(scope.active_bytes == bytes && scope.active_grant_count == count,
          "exact live scope grant counters");
}
#ifndef SB_FAIRNESS_NO_ALLOC_OVERRIDE
void Faults() {
  for (bool seeded : {false, true}) for (unsigned mode = 0; mode != 3; ++mode) {
    bool complete = false;
    for (long n = 0; n != 2048 && !complete; ++n) {
      mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
      mem::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
      Check(scheduler.SetScopePolicy(Policy()).ok(), "fault initial policy");
      mem::MemoryFairnessGrantToken seed;
      if (seeded) {
        auto s = scheduler.Admit(Request(17)); seed = s.grant; Check(s.ok(), "fault seed actual owner");
      }
      auto request = Request(mode == 1 ? 1025 : 64);
      if (mode == 2) request.provenance.authorization_authority = true;
      mem::MemoryFairnessDecision result;
      bool threw = false;
      fault::Arm(n);
      try { result = scheduler.Admit(std::move(request)); } catch (const std::bad_alloc&) { threw = true; }
      const bool hit = fault::hit; fault::Off();
      Check(!threw, "admission exception translated without orphaning parent grant");
      if (hit) {
        ++injected;
        Check(!result.ok() && !result.grant.valid() && result.status.code == Code::memory_allocation_failed,
              "typed exhaustion no false grant");
      } else {
        complete = true;
        Check(mode == 0 ? result.ok() : !result.ok(), "normal grant or exact policy/authority refusal");
      }
      if (result.ok()) Check(Terminal(scheduler, result.grant).ok(), "successful fault-sweep grant releases");
      Charges(scheduler, ledger, seeded ? 17 : 0, seeded ? 1 : 0);
      if (seed.valid()) Check(Terminal(scheduler, seed).ok(), "fault preserved preexisting owner");
      // Old-source negative controls may have leaked an undisclosed parent token.
      // Observe it above, then dispose of the fixture without hiding that failure.
      (void)ledger.CleanupOwner("fairness-owner-request-label");
    }
    Check(complete, "all sustained admission allocation positions exhausted");
  }
}
#endif
void Ownership() {
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  {
    mem::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
    Check(scheduler.SetScopePolicy(Policy()).ok(), "owner initial policy");
    auto r = scheduler.Admit(Request()); Check(r.ok(), "real parent-backed grant");
    auto forged = r.grant; ++forged.reservation.token_id;
    Check(!Terminal(scheduler, forged).ok(), "wrong parent identity cannot release actual grant");
    Charges(scheduler, ledger, 64, 1);
    const auto revoked = ledger.Cancel(r.grant.reservation);
    Check(!revoked.ok() && revoked.retained && revoked.retained_bytes == 64,
          "external revocation retains scheduler-owned parent charge");
    Charges(scheduler, ledger, 64, 1);
    fault::Arm(0);
    bool threw = false; bool released = false;
    try { released = Terminal(scheduler, r.grant).ok(); } catch (const std::bad_alloc&) { threw = true; }
    const bool hit = fault::hit; fault::Off();
    Check(released && !threw && !hit, "actual revoked-grant cleanup allocation-free");
    Charges(scheduler, ledger, 0, 0);
    Check(!Terminal(scheduler, r.grant).ok(), "duplicate terminal cannot erase a different owner");
    auto left = scheduler.Admit(Request(31)); Check(left.ok(), "destructor-owned grant");
    fault::Arm(0);
  }
  const bool hit = fault::hit; fault::Off();
  Check(!hit && ledger.Snapshot().current_bytes == 0 && ledger.Snapshot().pending_revocation_count == 0,
        "scheduler destruction releases its actual grants without allocation");
}
void RevocationProfiles() {
  for (unsigned mode = 0; mode != 3; ++mode) {
    mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
    mem::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
    auto grant = scheduler.Admit(Request(64)); Check(grant.ok(), "revocation profile grant");
    if (!grant.ok()) continue;
    auto forged = grant.grant; ++forged.reservation.bytes;
    Check(!Terminal(scheduler, forged).ok(), "wrong parent byte identity refused");
    forged = grant.grant; ++forged.bytes;
    Check(!Terminal(scheduler, forged).ok(), "wrong local byte identity refused");
    bool retained = false;
    if (mode == 0) {
      auto r = ledger.CleanupOwner("fairness-owner-request-label");
      retained = r.cleaned_bytes == 0 && r.retained_bytes == 64;
    } else if (mode == 1) {
      auto r = ledger.CleanupExpiredLeases(42);
      retained = r.cleaned_bytes == 0 && r.retained_bytes == 64;
    } else {
      auto r = ledger.Release(grant.grant.reservation);
      retained = !r.ok() && r.retained && r.retained_bytes == 64;
    }
    Check(retained, "owner/expiry/nonowner release retains actual grant charge");
    Charges(scheduler, ledger, 64, 1);
    fault::Arm(0);
    bool released = false, threw = false;
    try { released = Terminal(scheduler, grant.grant).ok(); } catch (const std::bad_alloc&) { threw = true; }
    const bool hit = fault::hit; fault::Off();
    Check(released && !threw && !hit, "revocation profile terminal needs no allocation");
    Charges(scheduler, ledger, 0, 0);
  }
}
void Boundary() {
  constexpr auto max = std::numeric_limits<mem::u64>::max();
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  mem::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
  auto p = Policy(max, max - 10); p.burst_bytes = 20; p.burst_window_ms = 20;
  Check(scheduler.SetScopePolicy(p).ok(), "overflow-safe burst policy");
  auto request = Request(max - 5); request.now_ms = 1;
  auto burst = scheduler.Admit(request);
  Check(burst.ok() && burst.burst_used, "representable burst must not wrap soft-plus-burst ceiling");
  if (burst.ok()) Check(Terminal(scheduler, burst.grant).ok(), "maximal burst owner release");
  request.now_ms = max - 5;
  auto clock = scheduler.Admit(request);
  Check(!clock.ok() && !clock.grant.valid(), "burst deadline overflow refused");
  if (clock.ok()) (void)Terminal(scheduler, clock.grant);
  auto weight = Request(1); weight.weight = max; weight.priority = 1;
  Check(!scheduler.Admit(weight).ok(), "priority-weight overflow refuses");
  for (unsigned kind : {2u, 255u}) {
    auto invalid = Request(1); invalid.work_class = static_cast<mem::MemoryFairnessWorkClass>(kind);
    auto r = scheduler.Admit(invalid); Check(!r.ok(), "unknown work class refused");
    if (r.ok()) (void)Terminal(scheduler, r.grant);
  }
  auto invalid = Request(1); invalid.scope_chain.front().kind = static_cast<mem::HierarchicalMemoryScopeKind>(255);
  auto scope = scheduler.Admit(invalid); Check(!scope.ok(), "unknown scope kind refused");
  if (scope.ok()) (void)Terminal(scheduler, scope.grant);
  auto category = Request(1); category.category = static_cast<mem::MemoryCategory>(65535);
  auto cat = scheduler.Admit(category); Check(!cat.ok(), "unknown memory category refused");
  if (cat.ok()) (void)Terminal(scheduler, cat.grant);
  Check(scheduler.SetScopePolicy(Policy(max)).ok(), "headroom overflow policy");
  for (unsigned i = 0; i != 2; ++i) {
    auto guarantee = Policy(max); guarantee.scope = {mem::HierarchicalMemoryScopeKind::tenant, "guarantee-" + std::to_string(i)};
    guarantee.guarantee_bytes = (max / 2) + 1;
    Check(scheduler.SetScopePolicy(guarantee).ok(), "foreground guarantee");
  }
  auto background = Request(1); background.work_class = mem::MemoryFairnessWorkClass::background;
  auto protected_result = scheduler.Admit(background);
  Check(!protected_result.ok(), "sum of protected headroom cannot wrap into available capacity");
  if (protected_result.ok()) (void)Terminal(scheduler, protected_result.grant);
}
void ClusterCategories() {
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  mem::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
  Check(scheduler.SetScopePolicy(Policy(64)).ok(), "local cluster accounting policy");
  for (auto category : {mem::MemoryCategory::cluster_control_reserved, mem::MemoryCategory::cluster_decision_reserved}) {
    auto r = Request(64); r.category = category;
    auto granted = scheduler.Admit(r);
    Check(granted.ok(), "cluster-category bytes get actual local accounting not blanket feature refusal");
    if (granted.ok()) {
      Charges(scheduler, ledger, 64, 1);
      Check(!scheduler.Admit(Request(1)).ok(), "cluster-category bytes compete for same local parent capacity");
      Check(Terminal(scheduler, granted.grant).ok(), "cluster-category actual owner release");
    }
    r.provenance.cluster_authority = true;
    Check(!scheduler.Admit(r).ok(), "memory accounting does not grant cluster authority");
  }
}
void Races() {
  for (unsigned round = 0; round != 32; ++round) {
    mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
    mem::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
    Check(scheduler.SetScopePolicy(Policy(1024, 64)).ok(), "soft-limit race setup");
    std::array<mem::MemoryFairnessDecision, 8> results;
    std::array<std::thread, 8> threads;
    std::barrier start(9);
    for (unsigned i = 0; i != 8; ++i) threads[i] = std::thread([&, i] {
      auto r = Request(32); start.arrive_and_wait(); results[i] = scheduler.Admit(std::move(r));
    });
    start.arrive_and_wait(); for (auto& t : threads) t.join();
    unsigned granted = 0; for (auto& r : results) if (r.ok()) ++granted;
    Check(granted == 2, "concurrent soft-limit checks and grant publication are one admission");
    Charges(scheduler, ledger, granted * 32, granted);
    for (auto& r : results) if (r.ok()) (void)Terminal(scheduler, r.grant);
    Charges(scheduler, ledger, 0, 0);
    auto r = scheduler.Admit(Request(32)); Check(r.ok(), "competing release fixture");
    std::atomic<unsigned> released{0}; std::barrier terminal(10);
    std::thread revoker([&] { terminal.arrive_and_wait(); (void)ledger.Cancel(r.grant.reservation); });
    for (auto& t : threads) t = std::thread([&] {
      terminal.arrive_and_wait(); if (Terminal(scheduler, r.grant).ok()) ++released;
    });
    terminal.arrive_and_wait(); for (auto& t : threads) t.join(); revoker.join();
    Check(released == 1, "only actual grant owner release wins");
    Charges(scheduler, ledger, 0, 0);
  }
}
void LastId() {
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  mem::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
  auto last = scheduler.Admit(Request(1));
  Check(last.ok() && last.grant.grant_id == std::numeric_limits<mem::u64>::max(), "last nonzero grant identity");
  if (last.ok()) Check(Terminal(scheduler, last.grant).ok(), "last identity release");
  for (unsigned i = 0; i != 3; ++i) {
    auto exhausted = scheduler.Admit(Request(1));
    Check(!exhausted.ok() && !exhausted.grant.valid(), "exhausted grant IDs never wrap/reuse");
  }
  Charges(scheduler, ledger, 0, 0);
}
}
int main(int argc, char**) {
  if (argc > 1) LastId();
  else {
    Ownership(); RevocationProfiles(); Boundary(); ClusterCategories();
#ifndef SB_FAIRNESS_NO_ALLOC_OVERRIDE
    Faults();
#endif
    Races();
  }
  std::cout << "checks=" << checks << " allocation_faults=" << injected << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
