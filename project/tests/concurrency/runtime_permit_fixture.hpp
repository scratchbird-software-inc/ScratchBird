// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// Include in one fixture translation unit per executable: actual allocation hooks.
#include "resource_governance_admission.hpp"
#include "reservation_backed_memory_resource.hpp"
#include "node_uuid_issuer.hpp"
#include <array>
#include <barrier>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <latch>
#include <new>
#include <string_view>
#include <thread>
#include <type_traits>
#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
struct AlignedPause {
  std::latch entered{1}, resume{1};
  bool reached = false;
};
thread_local AlignedPause* aligned_pause = nullptr;
}
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
void* operator new(std::size_t bytes, std::align_val_t alignment) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = -1; fault::hit = true; throw std::bad_alloc();
  }
  const auto align = static_cast<std::size_t>(alignment);
  const auto requested = bytes ? bytes : 1;
  if (requested > std::numeric_limits<std::size_t>::max() - (align - 1))
    throw std::bad_alloc();
  const auto rounded = ((requested + align - 1) / align) * align;
  if (void* p = std::aligned_alloc(align, rounded)) {
    if (auto* pause = std::exchange(fault::aligned_pause, nullptr)) {
      pause->reached = true;
      pause->entered.count_down();
      pause->resume.wait();
    }
    return p;
  }
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
  return ::operator new(bytes, alignment);
}
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

namespace {
namespace a = scratchbird::core::agents;
namespace m = scratchbird::core::memory;
namespace u = scratchbird::core::uuid;
using scratchbird::core::platform::Uuid;
using Code = a::RuntimePermitCode;
unsigned checks = 0;
void Check(bool good, const char* why) {
  ++checks;
  if (!good) { std::cerr << "FAIL " << why << '\n'; std::exit(1); }
}
Uuid Id(unsigned n) {
  Uuid id; id.bytes[0] = 1; id.bytes[6] = 0x70; id.bytes[8] = 0x80;
  id.bytes[14] = n >> 8; id.bytes[15] = n; return id;
}
struct Fixture {
  static m::AllocationPolicy MemoryPolicy() {
    auto p = m::DefaultLocalEngineMemoryPolicy();
    p.hard_limit_bytes = p.per_context_limit_bytes = 1048576; return p;
  }
  m::MemoryManager manager{MemoryPolicy()};
  m::HierarchicalMemoryBudgetLedger memory_ledger{3,5};
  std::unique_ptr<m::ReservationBackedMemoryResource> resource;
  std::unique_ptr<m::ReservationBackedPmrMemoryResource> metadata;
  u::StandaloneUuidV7Issuer issuer{{Id(1),Id(2)}, {{},0,10}};
  a::ResourceGovernanceReservationLedger governor{"runtime-permit-fixture-label"};
  a::RuntimePermitPolicy policy{{Id(1),Id(3),Id(4),5},2,2};
  a::RuntimePermitInstanceBinding workers{Id(5),6}, queue{Id(6),7};
  explicit Fixture(bool bind = true, std::uint64_t bytes = 65536) {
    m::ReservationBackedMemoryResourceRequest request;
    request.memory_manager = &manager; request.reservation_ledger = &memory_ledger;
    request.consumer_kind = m::ReservationBackedMemoryConsumerKind::background_maintenance;
    request.category = m::MemoryCategory::core_runtime; request.requested_bytes = bytes;
    request.memory_class = "runtime_permit_metadata"; request.route_label = "runtime.permit.fixture";
    request.binary_operation_uuid = Id(7).bytes;
    request.binary_ownership[m::MemoryBinaryScopeKind::context] = Id(8).bytes;
    request.binary_ownership[m::MemoryBinaryScopeKind::owner] = Id(9).bytes;
    request.binary_ownership[m::MemoryBinaryScopeKind::database] = Id(1).bytes;
    request.scope_chain = {{m::HierarchicalMemoryScopeKind::process,{},Id(10).bytes},
                          {m::HierarchicalMemoryScopeKind::database,{},Id(1).bytes}};
    request.provenance.source = m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    request.provenance.source_label = "actual runtime permit test";
    for (const auto& scope : request.scope_chain) {
      m::HierarchicalMemoryBudget budget;
      budget.scope = scope; budget.hard_limit_bytes = bytes; budget.provenance = request.provenance;
      Check(memory_ledger.SetBudget(budget).ok(), "actual parent budget");
    }
    auto grant = m::AcquireReservationBackedMemoryResource(std::move(request));
    Check(grant.ok(), "actual metadata grant"); resource = std::move(grant.resource);
    metadata = std::make_unique<m::ReservationBackedPmrMemoryResource>(resource.get(), "native-permit-nodes");
    if (bind) Check(Bind() == Code::bound, "immutable native profile bound");
  }
  Code Bind() { return governor.BindRuntimePermits(Id(11), policy, workers, queue, *metadata, issuer); }
  a::RuntimePermitRequest Request(bool worker, unsigned task = 20) const {
    a::RuntimePermitRequest r;
    r.authority = policy.authority;
    r.profile = worker ? a::RuntimePermitProfile::worker_slot : a::RuntimePermitProfile::queued_task;
    r.task = Id(task); r.attempt = Id(task + 100);
    if (worker) r.worker = Id(task + 200);
    const auto& instance = worker ? workers : queue;
    r.semaphore = instance.semaphore; r.semaphore_generation = instance.generation;
    r.quantity = 1; r.lease_deadline_tick = 42; return r;
  }
  void Empty() {
    const auto s = governor.Snapshot();
    Check(s.active_reservation_count == 0 && !s.active.worker_threads && !s.active.backlog_items,
          "zero actual governor charges");
    Check(resource->Snapshot().allocated_bytes == 0 && metadata->Snapshot().allocated_bytes == 0,
          "actual native index backing freed");
    Check(resource->ReleaseNoAlloc().ok(), "actual parent/physical grant cleanup");
    const auto physical = manager.Snapshot();
    Check(!physical.current_bytes && !physical.reserved_capacity_bytes &&
          !memory_ledger.Snapshot().current_bytes, "zero actual parent and physical charges");
  }
};
[[maybe_unused]] bool Same(const a::ResourceGovernanceReservationSnapshot& x,
          const a::ResourceGovernanceReservationSnapshot& y) {
  return x.native_ledger_uuid == y.native_ledger_uuid &&
    x.active_reservation_count == y.active_reservation_count &&
    x.created_reservation_count == y.created_reservation_count &&
    x.released_reservation_count == y.released_reservation_count &&
    x.active.worker_threads == y.active.worker_threads && x.active.backlog_items == y.active.backlog_items &&
    x.retained_runtime_permits == y.retained_runtime_permits &&
    x.quiescence_requested_permits == y.quiescence_requested_permits &&
    x.worker_waiters == y.worker_waiters && x.queue_waiters == y.queue_waiters &&
    x.worker_wait_calls == y.worker_wait_calls && x.queue_wait_calls == y.queue_wait_calls;
  // This compares ownership/accounting. Closing deliberately changes the
  // admission state without changing any charge; close oracles check it separately.
}
} // namespace
