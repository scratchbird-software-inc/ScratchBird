// SPDX-License-Identifier: MPL-2.0
// Shared physical admission must protect a grant from ordinary allocations.
#define main LegacyReservationOwnershipFixtureMain
#include "reservation_backed_resource_ownership_test.cpp"
#undef main

namespace {
mem::MemoryTag BinaryTag(unsigned identity = 1) {
  mem::MemoryTag tag;
  tag.category = mem::MemoryCategory::executor_query_reserved;
  tag.purpose = "physical capacity payload";
  for (unsigned i = 0; i != 7; ++i) {
    auto& id = tag.binary_ownership.scopes[i];
    id[0] = 1; id[6] = 0x70; id[8] = 0x80;
    id[14] = static_cast<unsigned char>(i);
    id[15] = static_cast<unsigned char>(identity);
  }
  return tag;
}
void PhysicalCounters(mem::MemoryManager& manager, mem::u64 live, mem::u64 unused,
                      mem::u64 owners) {
  const auto snapshot = manager.Snapshot();
  Check(snapshot.current_bytes == live && snapshot.resident_committed_bytes == live &&
        snapshot.sharded_accounting_current_bytes == live,
        "reservation counted as resident storage or actual bytes missing");
  Check(snapshot.reserved_capacity_bytes == unused &&
        snapshot.active_capacity_reservation_count == owners,
        "shared unused-credit/owner count mismatch");
}
void ExactCreditLifetime() {
  auto policy = Policy(); policy.hard_limit_bytes = 1024;
  mem::MemoryManager manager(policy);
  auto tag = BinaryTag();
  auto capacity = manager.allocator()->ReserveCapacity(768, tag);
  Check(capacity.ok(), "binary capacity owner admitted");
  if (!capacity.ok()) return;
  PhysicalCounters(manager, 0, 768, 1);
  auto other = manager.Allocate(256, 0, BinaryTag(2));
  Check(other.ok(), "unreserved remainder not usable");
  auto rejected = manager.Allocate(1, 0, BinaryTag(3));
  Check(!rejected.ok(), "exact root capacity bypass");
  if (rejected.ok()) manager.allocator()->DeallocateNoAlloc(rejected.pointer);
  auto invalid = capacity.reservation->Allocate(0);
  Check(!invalid.ok(), "zero reservation allocation accepted");
  invalid = capacity.reservation->Allocate(1, 48);
  Check(!invalid.ok(), "invalid aligned reservation allocation accepted");
  invalid = capacity.reservation->Allocate(769);
  Check(!invalid.ok(), "reservation size widened");
  auto block = capacity.reservation->Allocate(768, 256);
  Check(block.ok() && reinterpret_cast<std::uintptr_t>(block.pointer) % 256 == 0,
        "exact credit conversion/alignment");
  if (!block.ok()) return;
  std::memset(block.pointer, 0xab, 768);
  PhysicalCounters(manager, 1024, 0, 1);
  auto moved = manager.allocator()->Reallocate(block.pointer, 128, 256, tag);
  Check(!moved.ok() && static_cast<unsigned char*>(block.pointer)[767] == 0xab,
        "ordinary reallocation detached capacity ownership");
  Check(manager.allocator()->DeallocateNoAlloc(block.pointer).ok(), "reserved buffer release");
  PhysicalCounters(manager, 256, 768, 1);
  for (const auto& context : manager.Snapshot().contexts) {
    if (context.binary_scope && context.binary_scope->uuid[15] == 1)
      Check(context.current_bytes == 0 && context.reserved_capacity_bytes == 768,
            "binary physical context did not retain unused capacity");
  }
  block = capacity.reservation->Allocate(512);
  Check(block.ok(), "freed credit not reusable");
  fault::Arm(0);
  capacity.reservation.reset();
  fault::Off();
  PhysicalCounters(manager, 768, 0, 0);
  auto tail = manager.Allocate(256, 0, BinaryTag(3));
  Check(tail.ok(), "closing lease did not release unused tail");
  if (block.ok()) manager.allocator()->DeallocateNoAlloc(block.pointer);
  PhysicalCounters(manager, 512, 0, 0);
  if (tail.ok()) manager.allocator()->DeallocateNoAlloc(tail.pointer);
  if (other.ok()) manager.allocator()->DeallocateNoAlloc(other.pointer);
  PhysicalCounters(manager, 0, 0, 0);
}

void ScopeAndPolicyLimits() {
  for (unsigned mode = 0; mode != 3; ++mode) {
    auto policy = Policy(); policy.hard_limit_bytes = 8192;
    if (mode == 0) policy.per_context_limit_bytes = 1024;
    if (mode == 1) policy.page_buffer_pool_limit_bytes = 1024;
    if (mode == 2) { policy.soft_limit_bytes = 1024; policy.reject_over_soft_limit = true; }
    mem::MemoryManager manager(policy);
    auto tag = BinaryTag();
    if (mode == 1) tag.category = mem::MemoryCategory::page_buffer;
    auto reserved = manager.allocator()->ReserveCapacity(1024, tag);
    Check(reserved.ok(), "policy capacity setup");
    if (!reserved.ok()) continue;
    auto same_scope = BinaryTag(2);
    same_scope.binary_ownership[mem::MemoryBinaryScopeKind::session] =
        tag.binary_ownership[mem::MemoryBinaryScopeKind::session];
    same_scope.category = tag.category;
    auto refused = manager.allocator()->ReserveCapacity(1, same_scope);
    Check(!refused.ok(), "second reservation bypassed shared context/page/soft limit");
    auto ordinary = manager.Allocate(1, 0, same_scope);
    Check(!ordinary.ok(), "ordinary allocation bypassed reserved context/page/soft limit");
    if (ordinary.ok()) manager.allocator()->DeallocateNoAlloc(ordinary.pointer);
    auto block = reserved.reservation->Allocate(1024);
    Check(block.ok(), "policy counted credit conversion twice");
    if (block.ok()) manager.allocator()->DeallocateNoAlloc(block.pointer);
    reserved.reservation.reset();
    PhysicalCounters(manager, 0, 0, 0);
  }
  mem::MemoryManager manager(Policy());
  auto valid = BinaryTag();
  Check(!manager.allocator()->ReserveCapacity(0, valid).ok(), "zero capacity accepted");
  for (unsigned slot = 0; slot != 7; ++slot) {
    for (unsigned version = 0; version != 16; ++version) {
      if (version == 7) continue;
      auto bad = valid; bad.binary_ownership.scopes[slot][6] = version << 4;
      Check(!manager.allocator()->ReserveCapacity(16, bad).ok(), "non-v7 capacity owner admitted");
    }
    for (unsigned variant : {0u, 0x40u, 0xc0u}) {
      auto bad = valid; bad.binary_ownership.scopes[slot][8] = variant;
      Check(!manager.allocator()->ReserveCapacity(16, bad).ok(), "non-RFC capacity owner admitted");
    }
  }
  auto mixed = valid; mixed.owner = "text is not binary owner authority";
  Check(!manager.allocator()->ReserveCapacity(16, mixed).ok(), "mixed binary/text capacity owner admitted");
  PhysicalCounters(manager, 0, 0, 0);
}

void CapacityFaults() {
#ifndef SB_ADAPTER_NO_ALLOC_OVERRIDE
  for (unsigned operation = 0; operation != 2; ++operation) {
    bool reached_end = false;
    for (long point = 0; point != 4096; ++point) {
      mem::MemoryManager manager(Policy());
      auto tag = BinaryTag();
      mem::MemoryCapacityReservationResult reserved;
      if (operation == 1) reserved = manager.allocator()->ReserveCapacity(4096, tag);
      mem::AllocationResult allocated;
      bool escaped = false;
      // Avoid the caller's by-value copy when injecting inside ReserveCapacity.
      auto call_tag = tag;
      fault::Arm(point);
      try {
        if (operation == 0) reserved = manager.allocator()->ReserveCapacity(4096, std::move(call_tag));
        else allocated = reserved.reservation->Allocate(257, 64);
      } catch (const std::bad_alloc&) { escaped = true; }
      const bool hit = fault::hit; fault::Off();
      injected += hit;
      Check(!escaped, "capacity API allocation failure escaped typed refusal");
      if (operation == 0)
        PhysicalCounters(manager, 0, reserved.ok() ? 4096 : 0, reserved.ok() ? 1 : 0);
      else {
        PhysicalCounters(manager, allocated.ok() ? 257 : 0, allocated.ok() ? 3839 : 4096, 1);
        if (allocated.ok()) {
          std::memset(allocated.pointer, 0xce, 257);
          fault::Arm(0);
          const auto released = manager.allocator()->DeallocateNoAlloc(allocated.pointer);
          fault::Off();
          Check(released.ok(), "capacity buffer cleanup allocated under pressure");
        }
      }
      fault::Arm(0); reserved.reservation.reset(); fault::Off();
      PhysicalCounters(manager, 0, 0, 0);
      auto retry = manager.allocator()->ReserveCapacity(4096, tag);
      Check(retry.ok(), "failed capacity attempt poisoned retry");
      if (!hit) { reached_end = true; break; }
    }
    Check(reached_end, "capacity allocation fault sweep did not reach success");
  }
#endif
}

void CapacityOverflow() {
  auto policy = Policy();
  policy.hard_limit_bytes = std::numeric_limits<mem::usize>::max();
  policy.per_context_limit_bytes = 0;
  mem::MemoryManager manager(policy);
  const auto ceiling = policy.hard_limit_bytes;
  auto reservation = manager.allocator()->ReserveCapacity(ceiling, BinaryTag());
  Check(reservation.ok(), "maximum representable admission credit");
  if (!reservation.ok()) return;
  Check(!manager.allocator()->ReserveCapacity(1, BinaryTag(2)).ok(), "capacity sum overflow admitted owner");
  auto rejected = manager.Allocate(1, 0, BinaryTag(2));
  Check(!rejected.ok(), "capacity sum overflow admitted ordinary allocation");
  if (rejected.ok()) manager.allocator()->DeallocateNoAlloc(rejected.pointer);
  auto block = reservation.reservation->Allocate(64);
  Check(block.ok(), "maximum credit conversion overflow");
  PhysicalCounters(manager, 64, ceiling - 64, 1);
  if (block.ok()) manager.allocator()->DeallocateNoAlloc(block.pointer);
  PhysicalCounters(manager, 0, ceiling, 1);
  reservation.reservation.reset();
  PhysicalCounters(manager, 0, 0, 0);
}

void ReservationPolicySeverity() {
  for (unsigned mode = 0; mode != 4; ++mode) {
    auto policy = Policy();
    policy.failure_mode = mem::AllocationFailureMode::fatal_status;
    auto tag = BinaryTag();
    if (mode == 0) policy.hard_limit_bytes = 1;
    if (mode == 1) policy.per_context_limit_bytes = 1;
    if (mode == 2) { policy.soft_limit_bytes = 1; policy.reject_over_soft_limit = true; }
    if (mode == 3) { policy.page_buffer_pool_limit_bytes = 1; tag.category = mem::MemoryCategory::page_buffer; }
    mem::MemoryManager manager(policy);
    const auto reservation = manager.allocator()->ReserveCapacity(2, tag);
    const auto ordinary = manager.Allocate(2, 0, tag);
    Check(!reservation.ok() && !ordinary.ok(), "policy severity refusal setup");
    Check(reservation.status.code == ordinary.status.code &&
          reservation.status.severity == ordinary.status.severity,
          "reservation refusal changed configured physical policy severity");
    mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
    Configure(ledger, manager);
    auto request = Request(ledger, manager);
    request.category = tag.category;
    request.requested_bytes = 2;
    const auto adapter = mem::AcquireReservationBackedMemoryResource(request);
    Check(!adapter.ok() && adapter.status.code == ordinary.status.code &&
          adapter.status.severity == ordinary.status.severity &&
          adapter.diagnostic.status.severity == ordinary.status.severity,
          "resource acquisition erased physical policy refusal severity");
    PhysicalCounters(manager, 0, 0, 0);
  }
}

void ConcurrentCapacity() {
  auto policy = Policy(); policy.hard_limit_bytes = 4096;
  mem::MemoryManager manager(policy);
  std::barrier held(17), release(17);
  std::array<bool, 16> admitted{}, allocated{};
  std::array<std::thread, 16> workers;
  for (unsigned i = 0; i != workers.size(); ++i) workers[i] = std::thread([&, i] {
    auto reservation = manager.allocator()->ReserveCapacity(1024, BinaryTag(i + 1));
    admitted[i] = reservation.ok();
    mem::AllocationResult block;
    if (reservation.ok()) {
      block = reservation.reservation->Allocate(1024, 64);
      allocated[i] = block.ok();
      if (block.ok()) std::memset(block.pointer, i + 1, 1024);
    }
    held.arrive_and_wait(); release.arrive_and_wait();
    if (block.ok()) manager.allocator()->DeallocateNoAlloc(block.pointer);
  });
  held.arrive_and_wait();
  unsigned successes = 0;
  for (unsigned i = 0; i != admitted.size(); ++i) {
    successes += admitted[i];
    Check(admitted[i] == allocated[i], "concurrent capacity grant not physically usable");
  }
  Check(successes == 4, "concurrent reservation over/under-admitted shared physical root");
  PhysicalCounters(manager, 4096, 0, 4);
  release.arrive_and_wait();
  for (auto& worker : workers) worker.join();
  PhysicalCounters(manager, 0, 0, 0);
}

void SharedPhysicalAdmission() {
  auto policy = Policy();
  policy.hard_limit_bytes = 4096;
  mem::MemoryManager manager(policy);
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  auto request = Request(ledger, manager);
  Configure(ledger, manager);
  auto acquired = mem::AcquireReservationBackedMemoryResource(request);
  Check(acquired.ok(), "actual physical grant acquisition");
  if (!acquired.ok()) return;
  mem::MemoryTag outsider;
  outsider.category = mem::MemoryCategory::test_probe;
  outsider.owner = "unrelated";
  auto stolen = manager.Allocate(1, 0, outsider);
  Check(!stolen.ok(), "ordinary allocation stole admitted physical grant capacity");
  if (stolen.ok()) manager.allocator()->DeallocateNoAlloc(stolen.pointer);
  auto block = acquired.resource->Allocate({4096, 64, "actual-payload"});
  Check(block.ok(), "admitted grant cannot allocate its actual payload");
  if (block.ok()) std::memset(block.pointer, 0x5a, 4096);
  Check(acquired.resource->ReleaseNoAlloc().ok(), "physical grant final release");
  auto after = manager.Allocate(4096, 0, outsider);
  Check(after.ok(), "released capacity not reusable by ordinary allocation");
  if (after.ok()) manager.allocator()->DeallocateNoAlloc(after.pointer);

  auto occupied = manager.Allocate(1, 0, outsider);
  Check(occupied.ok(), "physical competition setup");
  auto refused = mem::AcquireReservationBackedMemoryResource(request);
  Check(!refused.ok(), "hierarchical grant published without sufficient physical capacity");
  if (refused.ok()) refused.resource->ReleaseNoAlloc();
  Check(ledger.Snapshot().current_bytes == 0, "physical refusal leaked parent reservation");
  if (occupied.ok()) manager.allocator()->DeallocateNoAlloc(occupied.pointer);
}
}

int main() {
  WarmMetrics();
  SharedPhysicalAdmission();
  ExactCreditLifetime(); ScopeAndPolicyLimits(); CapacityFaults(); CapacityOverflow(); ConcurrentCapacity();
  ReservationPolicySeverity();
  std::cout << "physical capacity checks=" << checks << " allocation_faults=" << injected
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
