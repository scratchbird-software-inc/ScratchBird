// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "resource_governance_admission.hpp"
#include <cstdlib>
#include <array>
#include <barrier>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>
#include <thread>

namespace fault { thread_local long remaining = -1; thread_local bool hit = false; }
void* operator new(std::size_t bytes) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = -1; fault::hit = true; throw std::bad_alloc();
  }
  if (void* p = std::malloc(bytes ? bytes : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
namespace a = scratchbird::core::agents;
unsigned checks = 0, failures = 0;
void Check(bool ok, std::string_view reason) {
  ++checks;
  if (!ok) { ++failures; std::cerr << "FAIL " << reason << '\n'; }
}
a::ResourceGovernanceReservationAcquireRequest Request(unsigned id, bool worker) {
  a::ResourceGovernanceReservationAcquireRequest r;
  r.admission.operation_id = "governor-fault-operation-" + std::to_string(id);
  r.owner_uuid.bytes[6] = 0x70; r.owner_uuid.bytes[8] = 0x80; r.owner_uuid.bytes[15] = 1;
  r.lease_deadline_tick = 42;
  auto& d = r.admission.descriptor;
  d.descriptor_id = "runtime-governor-fault-profile";
  d.family = r.admission.expected_family = a::ResourceGovernanceFamily::kBackgroundJob;
  d.source = a::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  d.source_path_or_label = "runtime.governor.actual.policy";
  d.descriptor_generation = d.expected_generation = 7;
  d.runtime_dependency_present = true;
  d.benchmark_clean = true;
  d.limits = {64,64,64,64,64,4,4,64,64,64,64,64,64};
  if (worker) r.admission.requested.worker_threads = 1;
  else r.admission.requested.backlog_items = 1;
  return r;
}
bool Same(const a::ResourceGovernanceReservationSnapshot& x,
          const a::ResourceGovernanceReservationSnapshot& y) {
  return x.active_reservation_count == y.active_reservation_count &&
      x.created_reservation_count == y.created_reservation_count &&
      x.released_reservation_count == y.released_reservation_count &&
      x.active.worker_threads == y.active.worker_threads &&
      x.active.backlog_items == y.active.backlog_items &&
      x.active.memory_bytes == y.active.memory_bytes;
}
void FaultSweep(bool worker, unsigned operation, bool seeded) {
  unsigned injected = 0; bool reached_end = false;
  for (long allocation = 0; allocation < 1024 && !reached_end; ++allocation) {
    a::ResourceGovernanceReservationLedger ledger("runtime-governor-actual-ledger");
    a::ResourceGovernanceReservationToken seed, target;
    if (seeded) {
      const auto r = ledger.Acquire(Request(1, !worker));
      Check(r.ok, "seed actual independent dimension"); seed = r.reservation;
    }
    if (operation != 0) {
      const auto r = ledger.Acquire(Request(2, worker));
      Check(r.ok, "seed actual release target"); target = r.reservation;
    }
    const auto before = ledger.Snapshot();
    auto request = Request(3, worker);
    bool threw = false, ok = false;
    fault::hit = false; fault::remaining = allocation;
    try {
      if (operation == 0) ok = ledger.Acquire(std::move(request)).ok;
      if (operation == 1) ok = ledger.Release(target.token_id).ok;
      if (operation == 2) ok = ledger.ReleaseOwnerReservations(target.owner_uuid).ok;
      if (operation == 3) ok = ledger.ExpireReservations(42).ok;
    } catch (const std::bad_alloc&) { threw = true; }
    const bool hit = fault::hit; fault::remaining = -1;
    const auto after = ledger.Snapshot();
    if (hit) {
      ++injected;
      Check(threw && Same(before, after), "failed operation preserves exact governor ownership and sequence");
      if (operation != 0) Check(ledger.Release(target.token_id).released,
                               "failed release leaves exact target usable for retry");
    } else {
      reached_end = true; Check(ok && !threw, "normal operation succeeds after complete allocation sweep");
      if (operation == 0) Check(after.active_reservation_count == before.active_reservation_count + 1 &&
          after.created_reservation_count == before.created_reservation_count + 1,
          "success publishes exactly one actual grant");
      else if (operation == 1) Check(after.active_reservation_count + 1 == before.active_reservation_count,
                                    "exact one-grant release effect");
      else Check(after.active_reservation_count == 0 && after.active.worker_threads == 0 &&
                 after.active.backlog_items == 0, "whole matching cleanup effect");
    }
  }
  Check(reached_end && injected > 0, "complete real allocation inventory");
  std::cout << "profile=" << (worker ? "worker" : "queue") << " operation=" << operation
            << " seeded=" << seeded << " allocation_faults=" << injected << '\n';
}
void NativeContentionAndCleanup(bool worker) {
  a::ResourceGovernanceReservationLedger ledger("actual-contended-governor");
  std::barrier begin(8);
  std::array<a::ResourceGovernanceReservationAcquireResult, 8> results;
  {
    std::array<std::jthread, 8> threads;
    for (unsigned n = 0; n < threads.size(); ++n) threads[n] = std::jthread([&,n] {
      auto request = Request(n, worker);
      request.admission.descriptor.limits.worker_threads = 1;
      request.admission.descriptor.limits.backlog_items = 1;
      begin.arrive_and_wait(); results[n] = ledger.Acquire(std::move(request));
    });
  } // Actual native joins precede all release oracles.
  unsigned granted = 0;
  for (const auto& result : results) if (result.ok) {
    ++granted;
    auto wrong = result.reservation.token_id + ":foreign";
    fault::hit = false; fault::remaining = 0;
    const auto rejected = ledger.ReleaseNoAlloc(wrong);
    const auto released = ledger.ReleaseNoAlloc(result.reservation.token_id);
    const auto duplicate = ledger.ReleaseNoAlloc(result.reservation.token_id);
    fault::remaining = -1;
    Check(!fault::hit && rejected == a::ResourceGovernanceReleaseCode::not_found &&
          released == a::ResourceGovernanceReleaseCode::released &&
          duplicate == a::ResourceGovernanceReleaseCode::not_found,
          "quiescent exact cleanup allocates nothing and refuses wrong or duplicate tokens");
  }
  const auto empty = ledger.Snapshot();
  Check(granted == 1 && empty.active_reservation_count == 0 &&
        empty.created_reservation_count == 1 && empty.released_reservation_count == 1 &&
        empty.active.worker_threads == 0 && empty.active.backlog_items == 0,
        "eight actual contenders share one actual governor slot and release it once");
}
void QuantityBoundary() {
  a::ResourceGovernanceReservationLedger ledger("actual-governor-quantity-boundary");
  auto request = Request(1,true);
  request.admission.requested.worker_threads = std::numeric_limits<std::int64_t>::max();
  request.admission.descriptor.limits.worker_threads = std::numeric_limits<std::int64_t>::max();
  const auto maximum = ledger.Acquire(request);
  Check(maximum.ok, "actual maximum representable quota charge");
  request.admission.requested.worker_threads = 1;
  const auto before = ledger.Snapshot();
  const auto denied = ledger.Acquire(request);
  Check(!denied.ok && denied.exceeded_quota == "worker_threads" && Same(before, ledger.Snapshot()),
        "cumulative quota addition cannot overflow or mutate existing grant");
  Check(ledger.ReleaseNoAlloc(maximum.reservation.token_id) == a::ResourceGovernanceReleaseCode::released,
        "maximum charge really released");
}
}
int main() {
  for (bool worker : {false,true}) for (unsigned operation = 0; operation < 4; ++operation)
    for (bool seeded : {false,true}) FaultSweep(worker, operation, seeded);
  NativeContentionAndCleanup(false); NativeContentionAndCleanup(true); QuantityBoundary();
  std::cout << "governor checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
