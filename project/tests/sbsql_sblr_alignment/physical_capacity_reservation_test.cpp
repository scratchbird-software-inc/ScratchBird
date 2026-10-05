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
void GenerationBoundLimitReduction() {
  auto policy = Policy(); policy.hard_limit_bytes = 1024;
  policy.soft_limit_bytes = 0;
  policy.per_context_limit_bytes = 0;
  policy.page_buffer_pool_limit_bytes = 0;
  mem::MemoryManager manager(policy);
  auto capacity = manager.allocator()->ReserveCapacity(768, BinaryTag());
  Check(capacity.ok(), "activation existing grant setup");
  if (!capacity.ok()) return;
  mem::MemoryLimitReduction change;
  change.policy_uuid = BinaryTag().binary_ownership[mem::MemoryBinaryScopeKind::owner];
  change.hard_limit_bytes = 512;
  change.existing_grants = mem::MemoryExistingGrantRule::reject_change;
  auto receipt = manager.allocator()->ReduceLimits(change);
  Check(receipt.status == mem::MemoryLimitActivationStatus::existing_grants_exceed_limit &&
        receipt.generation == 0 && manager.policy().hard_limit_bytes == 1024,
        "reject-change must preserve policy and generation");
  change.existing_grants = mem::MemoryExistingGrantRule::grandfather;
  fault::Arm(0);
  receipt = manager.allocator()->ReduceLimits(change);
  fault::Off();
  Check(receipt.ok() && receipt.generation == 1 && receipt.policy_uuid == change.policy_uuid,
        "allocation-free binary generation activation");
  Check(capacity.reservation->policy_binding() == mem::MemoryPolicyBinding{},
        "activation relabeled original bootstrap grant");
  const auto stale = manager.allocator()->ReduceLimits(change);
  Check(stale.status == mem::MemoryLimitActivationStatus::stale_generation && stale.generation == 1,
        "stale activation changed generation");
  auto refused = manager.Allocate(1, 0, BinaryTag(2));
  Check(!refused.ok(), "new work bypassed tightened hard limit");
  if (refused.ok()) manager.allocator()->DeallocateNoAlloc(refused.pointer);
  auto retained = capacity.reservation->Allocate(768);
  Check(retained.ok(), "tightening revoked previously admitted credit");
  Check(retained.policy_binding == capacity.reservation->policy_binding(),
        "credit conversion relabeled old admission with current generation");
  if (retained.ok()) manager.allocator()->DeallocateNoAlloc(retained.pointer);
  capacity.reservation.reset();
  auto available = manager.Allocate(512, 0, BinaryTag(2));
  Check(available.ok(), "new limit unavailable after old grant retirement");
  Check(available.policy_binding == manager.allocator()->PolicyBinding(),
        "ordinary allocation lacks current policy binding");
  if (available.ok()) manager.allocator()->DeallocateNoAlloc(available.pointer);
  change.expected_generation = 1; change.hard_limit_bytes = 1024;
  receipt = manager.allocator()->ReduceLimits(change);
  Check(receipt.status == mem::MemoryLimitActivationStatus::expansion_forbidden,
        "limit reduction expanded governor envelope");
  change.hard_limit_bytes = 256;
  std::atomic<unsigned> successes{0};
  std::atomic<unsigned> invalid_snapshots{0};
  std::vector<std::thread> workers;
  for (unsigned i = 0; i != 8; ++i) workers.emplace_back([&] {
    if (manager.allocator()->ReduceLimits(change).ok()) ++successes;
    const auto copy = manager.policy();
    if (copy.hard_limit_bytes != 256) ++invalid_snapshots;
  });
  for (auto& worker : workers) worker.join();
  Check(successes == 1, "concurrent activation published more than one generation");
  Check(invalid_snapshots == 0, "policy snapshot torn during activation");
  auto new_grant = manager.allocator()->ReserveCapacity(256, BinaryTag());
  Check(new_grant.ok() && new_grant.reservation->policy_binding() ==
        manager.allocator()->PolicyBinding() &&
        new_grant.reservation->policy_binding().generation == 2,
        "new grant lacks current binary policy binding");
  new_grant.reservation.reset();
  const auto binding = manager.allocator()->PolicyBinding();
  change.expected_generation = binding.generation;
  for (unsigned invalid = 0; invalid != 5; ++invalid) {
    auto bad = change;
    if (invalid == 0) bad.policy_uuid = {};
    if (invalid == 1) bad.hard_limit_bytes = 0;
    if (invalid == 2) bad.soft_limit_bytes = bad.hard_limit_bytes + 1;
    if (invalid == 3) bad.policy_uuid[6] = 0x40;
    if (invalid == 4) bad.existing_grants = static_cast<mem::MemoryExistingGrantRule>(99);
    Check(manager.allocator()->ReduceLimits(bad).status ==
          mem::MemoryLimitActivationStatus::invalid_request &&
          manager.allocator()->PolicyBinding() == binding &&
          manager.policy().hard_limit_bytes == 256,
          "invalid activation changed active generation or limits");
  }
  PhysicalCounters(manager, 0, 0, 0);
}

void AllocationGenerationLifetime() {
  auto policy = Policy();
  policy.hard_limit_bytes = 4096;
  policy.soft_limit_bytes = policy.per_context_limit_bytes = policy.page_buffer_pool_limit_bytes = 0;
  mem::MemoryManager manager(policy);
  auto tag = BinaryTag();
  mem::MemoryLimitReduction change;
  change.policy_uuid = tag.binary_ownership[mem::MemoryBinaryScopeKind::owner];
  change.hard_limit_bytes = 4096;
  Check(manager.allocator()->ReduceLimits(change).ok(), "first generation setup");
  auto original = manager.Allocate(512, 64, tag);
  Check(original.ok() && original.policy_binding && original.policy_binding->generation == 1,
        "ordinary allocation did not capture first policy generation");
  if (!original.ok()) return;
  std::memset(original.pointer, 0xa5, original.bytes);
  auto grant = manager.allocator()->ReserveCapacity(512, tag);
  Check(grant.ok(), "generation-pinned capacity setup");
  change.expected_generation = 1;
  change.hard_limit_bytes = 2048;
  change.policy_uuid[15] = 99;
  Check(manager.allocator()->ReduceLimits(change).ok(), "second generation activation");
  auto fresh = manager.Allocate(128, 0, tag);
  Check(fresh.ok() && fresh.policy_binding && fresh.policy_binding->generation == 2 &&
        fresh.policy_binding->policy_uuid == change.policy_uuid &&
        original.policy_binding && original.policy_binding->generation == 1,
        "activation rewrote old receipt or failed to bind new storage");
  if (fresh.ok()) manager.allocator()->DeallocateNoAlloc(fresh.pointer);
  if (grant.ok()) {
    auto converted = grant.reservation->Allocate(512);
    Check(converted.ok() && converted.policy_binding == grant.reservation->policy_binding() &&
          converted.policy_binding->generation == 1,
          "typed old grant conversion adopted new policy identity");
    grant.reservation.reset();
    Check(converted.policy_binding && converted.policy_binding->generation == 1,
          "closing capacity owner relabeled outstanding storage");
    if (converted.ok()) manager.allocator()->DeallocateNoAlloc(converted.pointer);
  }
  auto denied = manager.allocator()->Reallocate(original.pointer, 2048, 64, tag);
  Check(!denied.ok() && !denied.policy_binding && original.policy_binding &&
        original.policy_binding->generation == 1 &&
        static_cast<unsigned char*>(original.pointer)[511] == 0xa5,
        "failed reallocation lost original storage/binding");
  if (denied.ok()) { manager.allocator()->DeallocateNoAlloc(denied.pointer); return; }
  auto fault_tag = tag;
  fault::Arm(0);
  auto allocation_failed = manager.allocator()->Reallocate(original.pointer, 768, 64, std::move(fault_tag));
  fault::Off();
  Check(!allocation_failed.ok() && static_cast<unsigned char*>(original.pointer)[511] == 0xa5,
        "allocation failure during reallocation lost original bytes");
  if (allocation_failed.ok()) {
    manager.allocator()->DeallocateNoAlloc(allocation_failed.pointer);
    return;
  }
  auto replacement = manager.allocator()->Reallocate(original.pointer, 768, 64, tag);
  Check(replacement.ok() && replacement.policy_binding && replacement.policy_binding->generation == 2 &&
        replacement.policy_binding->policy_uuid == change.policy_uuid,
        "replacement storage did not receive its own admitted binding");
  if (replacement.ok()) {
    Check(static_cast<unsigned char*>(replacement.pointer)[511] == 0xa5,
          "reallocation failed to preserve initialized bytes");
    manager.allocator()->DeallocateNoAlloc(replacement.pointer);
  } else manager.allocator()->DeallocateNoAlloc(original.pointer);
  PhysicalCounters(manager, 0, 0, 0);
}

void LimitReductionSublimits() {
  for (unsigned mode = 0; mode != 3; ++mode) {
    auto policy = Policy();
    policy.hard_limit_bytes = 4096;
    policy.soft_limit_bytes = 0;
    policy.per_context_limit_bytes = 0;
    policy.page_buffer_pool_limit_bytes = 0;
    policy.reject_over_soft_limit = true;
    mem::MemoryManager manager(policy);
    auto tag = BinaryTag();
    if (mode == 2) tag.category = mem::MemoryCategory::page_buffer;
    auto grant = manager.allocator()->ReserveCapacity(1024, tag);
    Check(grant.ok(), "sublimit activation setup");
    if (!grant.ok()) continue;
    mem::MemoryLimitReduction change;
    change.policy_uuid = tag.binary_ownership[mem::MemoryBinaryScopeKind::owner];
    change.hard_limit_bytes = 4096;
    if (mode == 0) change.soft_limit_bytes = 512;
    if (mode == 1) change.per_context_limit_bytes = 512;
    if (mode == 2) change.page_buffer_pool_limit_bytes = 512;
    change.existing_grants = mem::MemoryExistingGrantRule::reject_change;
    Check(manager.allocator()->ReduceLimits(change).status ==
          mem::MemoryLimitActivationStatus::existing_grants_exceed_limit,
          "reject-change ignored reserved sublimit credit");
    change.existing_grants = mem::MemoryExistingGrantRule::grandfather;
    Check(manager.allocator()->ReduceLimits(change).ok(), "sublimit grandfather activation");
    auto block = grant.reservation->Allocate(1024);
    Check(block.ok(), "sublimit activation revoked admitted credit");
    change.expected_generation = 1;
    change.existing_grants = mem::MemoryExistingGrantRule::reject_change;
    Check(manager.allocator()->ReduceLimits(change).status ==
          mem::MemoryLimitActivationStatus::existing_grants_exceed_limit,
          "reject-change ignored physically committed sublimit bytes");
    auto denied = manager.allocator()->ReserveCapacity(1, tag);
    Check(!denied.ok(), "new reservation bypassed activated sublimit");
    if (block.ok()) manager.allocator()->DeallocateNoAlloc(block.pointer);
    grant.reservation.reset();
    auto admitted = manager.allocator()->ReserveCapacity(512, tag);
    Check(admitted.ok(), "sublimit headroom not restored after retirement");
    admitted.reservation.reset();
    PhysicalCounters(manager, 0, 0, 0);
    change.soft_limit_bytes = change.per_context_limit_bytes =
        change.page_buffer_pool_limit_bytes = 0;
    Check(manager.allocator()->ReduceLimits(change).status ==
          mem::MemoryLimitActivationStatus::expansion_forbidden,
          "zero sublimit silently expanded finite activated policy");
  }
}

void ConcurrentAdmissionAndActivation() {
  for (unsigned iteration = 0; iteration != 24; ++iteration) {
    auto policy = Policy();
    policy.hard_limit_bytes = 1024;
    policy.soft_limit_bytes = policy.per_context_limit_bytes =
        policy.page_buffer_pool_limit_bytes = 0;
    mem::MemoryManager manager(policy);
    mem::MemoryLimitReduction change;
    change.policy_uuid = BinaryTag().binary_ownership[mem::MemoryBinaryScopeKind::owner];
    change.hard_limit_bytes = 256;
    change.existing_grants = mem::MemoryExistingGrantRule::reject_change;
    mem::MemoryLimitActivationReceipt activation;
    mem::MemoryCapacityReservationResult grant;
    std::barrier start(3);
    std::thread admit([&] {
      start.arrive_and_wait();
      grant = manager.allocator()->ReserveCapacity(512, BinaryTag());
    });
    std::thread activate([&] {
      start.arrive_and_wait();
      activation = manager.allocator()->ReduceLimits(change);
    });
    start.arrive_and_wait();
    admit.join(); activate.join();
    Check(grant.ok() != activation.ok(), "admission/activation were not serialized");
    if (grant.ok()) {
      Check(activation.status == mem::MemoryLimitActivationStatus::existing_grants_exceed_limit &&
            grant.reservation->policy_binding().generation == 0,
            "winning admission lost its original policy binding");
    } else {
      Check(manager.allocator()->PolicyBinding().generation == 1,
            "winning activation did not publish generation");
    }
    grant.reservation.reset();
    PhysicalCounters(manager, 0, 0, 0);
  }
}

void ConcurrentAllocationBinding() {
  for (unsigned iteration = 0; iteration != 64; ++iteration) {
    auto policy = Policy();
    policy.hard_limit_bytes = 4096;
    policy.soft_limit_bytes = policy.per_context_limit_bytes =
        policy.page_buffer_pool_limit_bytes = 0;
    mem::MemoryManager manager(policy);
    mem::MemoryLimitReduction change;
    change.policy_uuid = BinaryTag(1).binary_ownership[mem::MemoryBinaryScopeKind::owner];
    change.hard_limit_bytes = 2048;
    Check(manager.allocator()->ReduceLimits(change).ok(), "racing allocation first policy");
    const auto original = manager.allocator()->PolicyBinding();
    change.policy_uuid = BinaryTag(2).binary_ownership[mem::MemoryBinaryScopeKind::owner];
    change.expected_generation = original.generation;
    change.hard_limit_bytes = 1024;
    change.existing_grants = mem::MemoryExistingGrantRule::reject_change;
    mem::MemoryLimitActivationReceipt activation;
    mem::AllocationResult allocation;
    std::barrier start(3);
    std::thread allocate([&] {
      start.arrive_and_wait();
      allocation = manager.Allocate(512, 0, BinaryTag());
    });
    std::thread activate([&] {
      start.arrive_and_wait();
      activation = manager.allocator()->ReduceLimits(change);
    });
    start.arrive_and_wait();
    allocate.join(); activate.join();
    const auto current = manager.allocator()->PolicyBinding();
    Check(activation.ok() && current.generation == 2 &&
          current.policy_uuid == change.policy_uuid,
          "racing compatible activation failed");
    Check(allocation.ok() && allocation.policy_binding &&
          (*allocation.policy_binding == original || *allocation.policy_binding == current),
          "allocation receipt contains torn or absent admission binding");
    if (allocation.ok()) {
      std::memset(allocation.pointer, 0xa5, 512);
      const auto retained_binding = allocation.policy_binding;
      change.expected_generation = current.generation;
      change.hard_limit_bytes = 512;
      Check(manager.allocator()->ReduceLimits(change).ok(), "post-race narrowing failed");
      Check(allocation.policy_binding == retained_binding &&
            static_cast<unsigned char*>(allocation.pointer)[511] == 0xa5,
            "later activation relabeled or altered admitted storage");
      PhysicalCounters(manager, 512, 0, 0);
      Check(manager.allocator()->DeallocateNoAlloc(allocation.pointer).ok(),
            "racing allocation release failed");
    }
    PhysicalCounters(manager, 0, 0, 0);
  }
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
  GenerationBoundLimitReduction();
  AllocationGenerationLifetime();
  LimitReductionSublimits();
  ConcurrentAdmissionAndActivation();
  ConcurrentAllocationBinding();
  SharedPhysicalAdmission();
  ExactCreditLifetime(); ScopeAndPolicyLimits(); CapacityFaults(); CapacityOverflow(); ConcurrentCapacity();
  ReservationPolicySeverity();
  std::cout << "physical capacity checks=" << checks << " allocation_faults=" << injected
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
