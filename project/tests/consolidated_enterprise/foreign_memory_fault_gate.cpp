// SPDX-License-Identifier: MPL-2.0
#define main HistoricalForeignMemoryMain
#include "ceic_016_foreign_memory_reservation_gate.cpp"
#undef main
#include <new>
#include <array>
#include <barrier>
#include <thread>

namespace { thread_local long fail_after = -1; thread_local bool failed_allocation = false; }
void* operator new(std::size_t n) {
  if (fail_after == 0) { failed_allocation = true; throw std::bad_alloc(); }
  if (fail_after > 0) --fail_after;
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
auto BinaryId(unsigned n) {
  memory::MemoryBinaryUuid id{};
  id[6] = 0x70; id[8] = 0x80; id[15] = n;
  return id;
}
auto ForeignRequest(memory::HierarchicalMemoryBudgetLedger& ledger, bool binary) {
  auto request = Request(&ledger, memory::ForeignMemorySource::json,
                         "long-scope-key-for-allocation-fault-sweeps", 4096, 128);
  if (binary) {
    request.owner_id.clear(); request.owning_scope.clear();
    request.binary_owner_uuid = BinaryId(1);
    request.binary_owning_scope_uuid = BinaryId(2);
    unsigned scope_id = 10;
    for (auto& scope : request.scope_chain) {
      scope.scope_id.clear(); scope.binary_scope_uuid = BinaryId(scope_id++);
    }
  }
  return request;
}
void Empty(const auto& lower, const auto& foreign) {
  const auto l = lower.Snapshot(); const auto f = foreign.Snapshot();
  Require(l.current_bytes == 0 && l.active_allocation_count == 0 &&
          l.active_reservation_count == 0 && f.current_estimated_bytes == 0 &&
          f.current_observed_bytes == 0 && f.active_reservation_count == 0,
          "foreign cleanup lost a lower or owning charge");
}
void AllocationBoundaries(bool binary) {
  using Code = scratchbird::core::platform::StatusCode;
  for (unsigned mode = 0; mode != 3; ++mode) {
    bool complete = false;
    bool partial = false;
    for (long boundary = 0; boundary != 2048 && !complete; ++boundary) {
      memory::HierarchicalMemoryBudgetLedger lower;
      memory::ForeignMemoryReservationLedger foreign;
      auto request = ForeignRequest(lower, binary);
      memory::ForeignMemoryReservationAcquireResult acquired;
      std::array<memory::ForeignMemoryReservationAcquireResult, 2> additional;
      if (mode != 0) {
        acquired = foreign.Reserve(request);
        Require(acquired.ok(), "foreign release setup");
      }
      if (mode == 2) for (auto& extra : additional) {
        extra = foreign.Reserve(request);
        Require(extra.ok(), "foreign batch setup");
      }
      fail_after = boundary; failed_allocation = false;
      memory::ForeignMemoryReservationReleaseResult released;
      memory::ForeignMemoryReservationCleanupResult cleaned;
      if (mode == 0) acquired = foreign.Reserve(std::move(request));
      if (mode == 1) released = acquired.reservation->Release();
      if (mode == 2) cleaned = binary ? foreign.CleanupOwner(BinaryId(1)) :
                                      foreign.CleanupOwner(std::move(request.owner_id));
      const bool fault = failed_allocation; fail_after = -1;
      const auto snapshot = foreign.Snapshot();
      if (mode == 0 && fault) {
        Require(!acquired.ok() && acquired.status.code == Code::memory_allocation_failed,
                "foreign acquire must return typed allocation failure");
        Empty(lower, foreign);
        Require(snapshot.reservation_count == 0 && snapshot.sources.empty() &&
                snapshot.owning_scopes.empty(), "failed publication changed foreign accounting");
        acquired = foreign.Reserve(ForeignRequest(lower, binary));
        Require(acquired.ok(), "failed foreign admission prevents retry");
      } else if (mode == 0) {
        Require(acquired.ok() && snapshot.current_estimated_bytes == 4096 &&
                lower.Snapshot().current_bytes == 4096, "foreign publication disagrees with receipt");
        if (binary) Require(snapshot.active_reservations.front().binary_owner_uuid == BinaryId(1) &&
                            snapshot.owning_scopes.front().binary_owning_scope_uuid == BinaryId(2),
                            "binary identities changed during publication");
      } else {
        const auto count = mode == 1 ? (released.released ? 1u : 0u) : cleaned.cleaned_reservation_count;
        const auto status = mode == 1 ? released.status : cleaned.status;
        const unsigned target = mode == 1 ? 1 : 3;
        Require(count <= target && snapshot.release_count == count &&
                snapshot.current_estimated_bytes == (target-count)*4096 &&
                lower.Snapshot().current_bytes == snapshot.current_estimated_bytes &&
                acquired.reservation->active() == (count == 0),
                "foreign cleanup hid effects or published completion early");
        if (fault) Require(!status.ok() && status.code == Code::memory_allocation_failed,
                           "foreign cleanup failure escaped typed receipt");
        else Require(status.ok() && count == target, "foreign cleanup did not finish");
        partial = partial || (count > 0 && count < target);
      }
      fail_after = 0; failed_allocation = false;
      const auto drained = acquired.reservation->ReleaseNoAlloc();
      const auto repeated = acquired.reservation->ReleaseNoAlloc();
      acquired.reservation.reset();
      for (auto& extra : additional) if (extra.reservation) {
        const auto extra_release = extra.reservation->ReleaseNoAlloc();
        // No allocation in Require on its successful path.
        Require(extra_release.ok(), "foreign partial batch could not drain");
        extra.reservation.reset();
      }
      const bool cleanup_allocated = failed_allocation; fail_after = -1;
      Require(!cleanup_allocated && drained.ok() && drained.released && repeated.ok(),
              "foreign no-allocation release/destructor failed or allocated");
      Empty(lower, foreign);
      complete = !fault;
    }
    Require(complete && (mode != 2 || partial),
            "foreign allocation sweep missed completion or partial-batch failure");
  }
}
void DestructorAndRaces() {
  for (unsigned iteration = 0; iteration != 50; ++iteration) {
    memory::HierarchicalMemoryBudgetLedger lower;
    memory::ForeignMemoryReservationLedger foreign;
    auto acquired = foreign.Reserve(ForeignRequest(lower, true));
    Require(acquired.ok(), "foreign concurrent setup");
    if (iteration == 0) {
      fail_after = 0; failed_allocation = false;
      acquired.reservation.reset();
      const bool allocated = failed_allocation; fail_after = -1;
      Require(!allocated, "active foreign destructor allocated");
    } else {
      std::barrier start(3);
      std::array<bool, 2> good{};
      std::array<std::thread, 2> workers;
      for (unsigned i = 0; i != 2; ++i) workers[i] = std::thread([&, i] {
        start.arrive_and_wait(); fail_after = 0;
        const auto result = acquired.reservation->ReleaseNoAlloc(); fail_after = -1;
        good[i] = result.ok() && result.released && !acquired.reservation->active() &&
                  lower.Snapshot().current_bytes == 0;
      });
      start.arrive_and_wait();
      for (auto& worker : workers) worker.join();
      Require(good[0] && good[1] && foreign.Snapshot().release_count == 1,
              "concurrent release completion preceded effects or double-counted release");
    }
    Empty(lower, foreign);
  }
}
}
int main() {
  AllocationBoundaries(false); AllocationBoundaries(true); DestructorAndRaces();
  return EXIT_SUCCESS;
}
