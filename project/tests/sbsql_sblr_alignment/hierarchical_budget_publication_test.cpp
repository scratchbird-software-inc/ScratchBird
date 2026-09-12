// SPDX-License-Identifier: MPL-2.0
#include "reservation_backed_memory_resource.hpp"
#include "memory_fairness_scheduler.hpp"
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <cstring>
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
#ifndef SB_BUDGET_NO_ALLOC_OVERRIDE
void* operator new(std::size_t n) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = 0; fault::hit = true; throw std::bad_alloc();
  }
  if (void* p = std::malloc(n ? n : 1)) return p;
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
mem::HierarchicalMemoryScopeRef Scope() {
  return {mem::HierarchicalMemoryScopeKind::session, "policy-publication-session-long-label"};
}
mem::HierarchicalMemoryBudgetProvenance Provenance() {
  mem::HierarchicalMemoryBudgetProvenance p;
  p.source = mem::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
  p.source_label = "budget-publication-contract-runtime-policy";
  return p;
}
mem::HierarchicalMemoryBudget Budget(mem::u64 hard = 128, mem::u64 soft = 0) {
  mem::HierarchicalMemoryBudget b;
  b.scope = Scope(); b.hard_limit_bytes = hard; b.soft_limit_bytes = soft;
  b.provenance = Provenance(); return b;
}
mem::HierarchicalMemoryReservationRequest Request(mem::u64 bytes) {
  mem::HierarchicalMemoryReservationRequest r;
  r.scope_chain = {Scope()}; r.requested_bytes = bytes;
  r.owner_id = "policy-publication-live-owner-label";
  r.memory_class = "policy-publication-result-memory-class";
  r.category = mem::MemoryCategory::executor_query_reserved;
  r.provenance = Provenance(); return r;
}
void Same(const mem::HierarchicalMemoryBudgetSnapshot& a,
          const mem::HierarchicalMemoryBudgetSnapshot& b) {
  Check(a.current_bytes == b.current_bytes && a.reserved_bytes == b.reserved_bytes &&
        a.active_bytes == b.active_bytes && a.reservation_count == b.reservation_count &&
        a.commit_count == b.commit_count && a.release_count == b.release_count &&
        a.active_reservation_count == b.active_reservation_count &&
        a.active_allocation_count == b.active_allocation_count &&
        a.retained_revoked_bytes == b.retained_revoked_bytes &&
        a.pending_revocation_count == b.pending_revocation_count, "failed policy changed live accounting");
  Check(a.scopes.size() == b.scopes.size(), "failed policy published a scope node");
  for (std::size_t i = 0; i < a.scopes.size() && i < b.scopes.size(); ++i) {
    const auto& x = a.scopes[i]; const auto& y = b.scopes[i];
    Check(x.kind == y.kind && x.scope_id == y.scope_id &&
          x.hard_limit_bytes == y.hard_limit_bytes && x.soft_limit_bytes == y.soft_limit_bytes &&
          x.current_bytes == y.current_bytes && x.reserved_bytes == y.reserved_bytes &&
          x.active_bytes == y.active_bytes && x.peak_bytes == y.peak_bytes &&
          x.reservation_count == y.reservation_count && x.commit_count == y.commit_count &&
          x.release_count == y.release_count &&
          x.active_allocation_count == y.active_allocation_count &&
          x.active_reservation_count == y.active_reservation_count &&
          x.priority_weight_total == y.priority_weight_total, "failed policy changed scope identity/limits/owners");
  }
}
void Limit(const mem::HierarchicalMemoryBudgetLedger& ledger, mem::u64 hard, mem::u64 soft,
           mem::u64 reserved, mem::u64 active) {
  const auto s = ledger.Snapshot();
  Check(s.scopes.size() == 1, "one exact configured scope");
  if (s.scopes.size() != 1) return;
  const auto& x = s.scopes.front();
  Check(x.kind == Scope().kind && x.scope_id == Scope().scope_id, "exact scope identity");
  Check(x.hard_limit_bytes == hard && x.soft_limit_bytes == soft, "published exact policy");
  Check(x.reserved_bytes == reserved && x.active_bytes == active &&
        x.current_bytes == active, "policy retained exact live charge");
}
void LivePolicy() {
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  Check(ledger.SetBudget(Budget()).ok(), "initial budget");
  auto a = ledger.Reserve(Request(40)), b = ledger.Reserve(Request(30));
  Check(a.ok() && b.ok() && ledger.Commit(a.token).ok(), "active and reserved owners");
  const auto before = ledger.Snapshot();
  auto refused = ledger.SetBudget(Budget(69, 25));
  Check(!refused.ok() && refused.status.code == Code::memory_limit_exceeded &&
        refused.diagnostic.diagnostic_code == "SB-MEMORY-BUDGET-LIVE-SHRINK-REFUSED",
        "explicit reject_change for reserved plus active ownership");
  Same(before, ledger.Snapshot());
  Check(ledger.SetBudget(Budget(70)).ok(), "exact live-byte limit allowed");
  Check(!ledger.Reserve(Request(1)).ok(), "new reservation obeys newly activated hard limit");
  Check(ledger.Commit(b.token).ok(), "admitted reservation still commits after policy change");
  Limit(ledger, 70, 0, 0, 70);
  Check(ledger.ReleaseNoAlloc(a.token).ok() && ledger.ReleaseNoAlloc(b.token).ok(), "old owners release");
  Check(ledger.SetBudget(Budget(32, 16)).ok(), "quiescent lower budget activates");
  auto c = ledger.Reserve(Request(16)); Check(c.ok(), "soft limit equality grants");
  auto soft = ledger.Reserve(Request(1));
  Check(!soft.ok() && soft.recommendation == mem::HierarchicalMemoryReservationRecommendation::degrade,
        "new work uses newly activated soft policy");
  Check(ledger.ReleaseNoAlloc(c.token).ok(), "soft-policy owner release");
  Check(ledger.SetBudget(Budget(0)).ok(), "existing zero hard means unbounded scope");
  const auto maximum = std::numeric_limits<mem::u64>::max();
  auto max = ledger.Reserve(Request(maximum)); Check(max.ok(), "near-overflow real reservation");
  auto lower = ledger.SetBudget(Budget(maximum - 1));
  Check(!lower.ok() && lower.status.code == Code::memory_limit_exceeded, "overflow-safe live shrink check");
  Check(ledger.ReleaseNoAlloc(max.token).ok(), "maximal reservation releases");
}
void RetainedPayload() {
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  auto p = mem::DefaultLocalEngineMemoryPolicy();
  p.hard_limit_bytes = 1024; p.per_context_limit_bytes = 1024;
  mem::MemoryManager manager(p);
  Check(ledger.SetBudget(Budget()).ok(), "physical owner budget");
  mem::ReservationBackedMemoryResourceRequest r;
  r.reservation_ledger = &ledger; r.memory_manager = &manager;
  r.scope_chain = {Scope()}; r.owner_id = "retained-policy-owner-label";
  r.route_label = "retained-policy-route-label"; r.operation_id = "retained-policy-operation";
  r.requested_bytes = 128; r.provenance = Provenance();
  auto owner = mem::AcquireReservationBackedMemoryResource(std::move(r));
  Check(owner.ok(), "actual physical resource acquisition");
  if (!owner.ok()) return;
  auto payload = owner.resource->Allocate({64, 64, "retained-policy-payload"});
  Check(payload.ok(), "actual retained physical allocation");
  if (!payload.ok()) return;
  std::memset(payload.pointer, 0x5a, 64);
  const auto cancel = ledger.Cancel(owner.resource->reservation_token());
  Check(!cancel.ok() && cancel.retained && cancel.newly_revoked && cancel.retained_bytes == 128,
        "revocation retains payload grant and does not falsely report completed cleanup");
  auto before = ledger.Snapshot();
  Check(!ledger.SetBudget(Budget(64)).ok(), "revoked but still owned grant blocks unsafe shrink");
  Same(before, ledger.Snapshot());
  Check(manager.Snapshot().current_bytes == 64, "policy refuses without freeing still-owned storage");
  bool intact = true;
  for (unsigned i = 0; i < 64; ++i)
    intact = intact && static_cast<unsigned char*>(payload.pointer)[i] == 0x5a;
  Check(intact, "revoked payload remains usable until owner cleanup");
  fault::Arm(0);
  const auto status = owner.resource->ReleaseNoAlloc();
  fault::Off();
  Check(status.ok() && !fault::hit, "physical-first forced owner cleanup needs no allocation");
  Check(manager.Snapshot().current_bytes == 0 && ledger.Snapshot().current_bytes == 0,
        "both actual physical and parent ownership released");
  Check(ledger.SetBudget(Budget(64)).ok(), "shrink succeeds only after actual owner release");
}
#ifndef SB_BUDGET_NO_ALLOC_OVERRIDE
void Faults() {
  for (unsigned mode = 0; mode != 7; ++mode) {
    bool complete = false;
    for (long n = 0; n != 1024 && !complete; ++n) {
      mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
      mem::HierarchicalMemoryReservationToken owner;
      if (mode != 0) {
        Check(ledger.SetBudget(Budget()).ok(), "fault baseline policy");
        auto live = ledger.Reserve(Request(48)); owner = live.token;
        Check(live.ok() && ledger.Commit(owner).ok(), "fault live owner");
      }
      auto next = Budget(mode == 2 ? 47 : 96, 20);
      if (mode == 3) next.provenance.source = mem::HierarchicalMemoryBudgetProvenanceSource::unknown;
      if (mode == 4) next.soft_limit_bytes = 97;
      if (mode == 5) next.scope.scope_id.clear();
      if (mode == 6) next.scope.kind = static_cast<mem::HierarchicalMemoryScopeKind>(255);
      const auto before = ledger.Snapshot();
      bool threw = false;
      mem::HierarchicalMemoryBudgetOperationResult result;
      fault::Arm(n);
      try { result = ledger.SetBudget(std::move(next)); } catch (const std::bad_alloc&) { threw = true; }
      const bool hit = fault::hit;
      fault::Off();
      Check(!threw, "SetBudget must return typed refusal on sustained allocation exhaustion");
      if (hit) {
        ++injected;
        Check(!result.ok() && result.status.code == Code::memory_allocation_failed,
              "OOM has typed status and never a success result");
        Same(before, ledger.Snapshot());
      } else {
        complete = true;
        if (mode >= 2) {
          Check(!result.ok(), "invalid or live-shrinking policy refuses");
          Same(before, ledger.Snapshot());
        } else Check(result.ok(), "valid fully prepared policy publishes");
      }
      if (owner.valid()) Check(ledger.ReleaseNoAlloc(owner).ok(), "unchanged owner can always release");
      Check(ledger.SetBudget(Budget(96)).ok(), "same identity retries after failure");
      Limit(ledger, 96, 0, 0, 0);
      auto retry = ledger.Reserve(Request(96));
      Check(retry.ok() && ledger.ReleaseNoAlloc(retry.token).ok(), "repaired/retried budget governs actual admission");
    }
    Check(complete, "every allocation position through terminal success/refusal exercised");
  }
}
#endif
mem::MemoryFairnessScopePolicy FairPolicy(mem::u64 hard = 128) {
  mem::MemoryFairnessScopePolicy p;
  p.scope = Scope(); p.hard_max_bytes = hard; p.soft_max_bytes = hard;
  p.guarantee_bytes = hard / 2; p.priority_weight = 3;
  p.provenance = Provenance(); return p;
}
void FairSame(const mem::MemoryFairnessSnapshot& a, const mem::MemoryFairnessSnapshot& b) {
  Check(a.active_bytes == b.active_bytes && a.grant_count == b.grant_count &&
        a.release_count == b.release_count && a.decision_count == b.decision_count,
        "failed fairness policy changed live scheduler counters");
  Check(a.scopes.size() == b.scopes.size(), "failed fairness policy published a node");
  for (std::size_t i = 0; i < a.scopes.size() && i < b.scopes.size(); ++i) {
    const auto& x = a.scopes[i]; const auto& y = b.scopes[i];
    Check(x.kind == y.kind && x.scope_id == y.scope_id &&
          x.hard_max_bytes == y.hard_max_bytes && x.soft_max_bytes == y.soft_max_bytes &&
          x.guarantee_bytes == y.guarantee_bytes && x.burst_bytes == y.burst_bytes &&
          x.active_bytes == y.active_bytes && x.active_grant_count == y.active_grant_count &&
          x.priority_weight_total == y.priority_weight_total && x.grant_count == y.grant_count,
          "failed fairness update changed policy or owned grant");
  }
}
mem::MemoryFairnessRequest FairRequest(mem::u64 bytes) {
  mem::MemoryFairnessRequest r;
  r.scope_chain = {Scope()}; r.requested_bytes = bytes;
  r.owner_id = "fairness-policy-live-owner-label";
  r.category = mem::MemoryCategory::executor_query_reserved;
  r.provenance = Provenance(); return r;
}
void FairLive() {
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  mem::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
  Check(scheduler.SetScopePolicy(FairPolicy()).ok(), "fair policy initial");
  auto owner = scheduler.Admit(FairRequest(80));
  Check(owner.ok(), "actual fair admission owns parent grant");
  const auto parent = ledger.Snapshot(); const auto local = scheduler.Snapshot();
  auto refused = scheduler.SetScopePolicy(FairPolicy(64));
  Check(!refused.ok() && refused.status.code == Code::memory_limit_exceeded,
        "fair policy propagates real parent live-shrink refusal");
  Same(parent, ledger.Snapshot()); FairSame(local, scheduler.Snapshot());
  Check(scheduler.SetScopePolicy(FairPolicy(96)).ok(), "safe fair policy preserves live counters");
  Check(scheduler.Release(owner.grant).ok(), "policy change preserves actual grant release");
  Check(ledger.Snapshot().current_bytes == 0 && scheduler.Snapshot().active_bytes == 0,
        "actual fair and parent grant charges quiesce");
  mem::MultiTenantMemoryFairnessScheduler missing(nullptr);
  Check(!missing.SetScopePolicy(FairPolicy()).ok(), "missing parent is a refusal");
}
#ifndef SB_BUDGET_NO_ALLOC_OVERRIDE
void FairFaults() {
  for (unsigned mode = 0; mode != 5; ++mode) {
    bool complete = false;
    for (long n = 0; n != 1024 && !complete; ++n) {
      mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
      mem::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
      mem::MemoryFairnessGrantToken owner;
      if (mode != 0) {
        Check(scheduler.SetScopePolicy(FairPolicy()).ok(), "fair fault initial policy");
        auto admitted = scheduler.Admit(FairRequest(48)); owner = admitted.grant;
        Check(admitted.ok(), "fair fault actual existing grant");
      }
      auto next = FairPolicy(mode == 2 ? 47 : 96);
      if (mode == 3) next.provenance.source = mem::HierarchicalMemoryBudgetProvenanceSource::unknown;
      if (mode == 4) next.scope.kind = static_cast<mem::HierarchicalMemoryScopeKind>(255);
      const auto parent = ledger.Snapshot(); const auto local = scheduler.Snapshot();
      mem::HierarchicalMemoryBudgetOperationResult result;
      bool threw = false;
      fault::Arm(n);
      try { result = scheduler.SetScopePolicy(std::move(next)); } catch (const std::bad_alloc&) { threw = true; }
      const bool hit = fault::hit;
      fault::Off();
      Check(!threw, "fair policy must return typed exhaustion");
      if (hit) {
        ++injected;
        Check(!result.ok() && result.status.code == Code::memory_allocation_failed,
              "fair policy no false success on OOM");
        Same(parent, ledger.Snapshot()); FairSame(local, scheduler.Snapshot());
      } else {
        complete = true;
        if (mode >= 2) {
          Check(!result.ok(), "fair invalid/live-shrink policy refusal");
          Same(parent, ledger.Snapshot()); FairSame(local, scheduler.Snapshot());
        } else Check(result.ok(), "valid fair policy publication");
      }
      if (owner.valid()) Check(scheduler.Release(owner).ok(), "fair failed policy retained old owner");
      Check(scheduler.SetScopePolicy(FairPolicy(96)).ok(), "fair policy retries same identity");
      const auto final = scheduler.Snapshot();
      Check(final.scopes.size() == 1 && final.scopes.front().hard_max_bytes == 96 &&
            final.scopes.front().soft_max_bytes == 96 && final.scopes.front().guarantee_bytes == 48,
            "exact fair policy published after retry");
      Limit(ledger, 96, 0, 0, 0);
      auto retry = scheduler.Admit(FairRequest(96));
      Check(retry.ok() && scheduler.Release(retry.grant).ok(), "fair retried policy governs actual grant");
    }
    Check(complete, "all fair publication failure positions exercised");
  }
}
#endif
void FairRace() {
  for (unsigned iteration = 0; iteration != 96; ++iteration) {
    mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
    mem::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
    auto small = FairPolicy(64), large = FairPolicy(128);
    mem::HierarchicalMemoryBudgetOperationResult a, b;
    std::barrier start(3);
    std::thread first([&] { start.arrive_and_wait(); a = scheduler.SetScopePolicy(std::move(small)); });
    std::thread second([&] { start.arrive_and_wait(); b = scheduler.SetScopePolicy(std::move(large)); });
    start.arrive_and_wait(); first.join(); second.join();
    Check(a.ok() && b.ok(), "competing fair policies both complete");
    const auto local = scheduler.Snapshot(); const auto parent = ledger.Snapshot();
    Check(local.scopes.size() == 1 && parent.scopes.size() == 1, "competing policy one exact owner scope");
    if (local.scopes.size() == 1 && parent.scopes.size() == 1)
      Check(local.scopes.front().hard_max_bytes == parent.scopes.front().hard_limit_bytes,
            "concurrent fair policy and parent hard limit publish in the same order");
  }
}
void Race() {
  for (unsigned iteration = 0; iteration != 96; ++iteration) {
    mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
    Check(ledger.SetBudget(Budget()).ok(), "race initial policy");
    auto budget = Budget(64); auto request = Request(96);
    mem::HierarchicalMemoryBudgetOperationResult changed;
    mem::HierarchicalMemoryReservationResult reserved;
    std::barrier start(3);
    std::thread policy([&] { start.arrive_and_wait(); changed = ledger.SetBudget(std::move(budget)); });
    std::thread admission([&] { start.arrive_and_wait(); reserved = ledger.Reserve(std::move(request)); });
    start.arrive_and_wait(); policy.join(); admission.join();
    Check(changed.ok() != reserved.ok(), "policy/admission linearization permits exactly one winner");
    Limit(ledger, changed.ok() ? 64 : 128, 0, reserved.ok() ? 96 : 0, 0);
    Check(!reserved.ok() || ledger.ReleaseNoAlloc(reserved.token).ok(), "race owner terminal cleanup");
    Check(ledger.Snapshot().current_bytes == 0, "race all charges quiescent");
  }
}
}
int main() {
  LivePolicy(); RetainedPayload(); FairLive();
#ifndef SB_BUDGET_NO_ALLOC_OVERRIDE
  Faults();
  FairFaults();
#endif
  Race(); FairRace();
  std::cout << "checks=" << checks << " allocation_faults=" << injected << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
