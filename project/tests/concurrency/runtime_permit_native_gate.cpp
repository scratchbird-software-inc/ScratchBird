// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "resource_governance_admission.hpp"
#include "reservation_backed_memory_resource.hpp"
#include "node_uuid_issuer.hpp"
#include <array>
#include <barrier>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>
#include <thread>
#include <type_traits>
#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

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
void* operator new(std::size_t bytes, std::align_val_t alignment) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = -1; fault::hit = true; throw std::bad_alloc();
  }
  const auto align = static_cast<std::size_t>(alignment);
  const auto requested = bytes ? bytes : 1;
  if (requested > std::numeric_limits<std::size_t>::max() - (align - 1))
    throw std::bad_alloc();
  const auto rounded = ((requested + align - 1) / align) * align;
  if (void* p = std::aligned_alloc(align, rounded)) return p;
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
bool Same(const a::ResourceGovernanceReservationSnapshot& x,
          const a::ResourceGovernanceReservationSnapshot& y) {
  return x.native_ledger_uuid == y.native_ledger_uuid &&
    x.active_reservation_count == y.active_reservation_count &&
    x.created_reservation_count == y.created_reservation_count &&
    x.released_reservation_count == y.released_reservation_count &&
    x.active.worker_threads == y.active.worker_threads && x.active.backlog_items == y.active.backlog_items &&
    x.retained_runtime_permits == y.retained_runtime_permits &&
    x.quiescence_requested_permits == y.quiescence_requested_permits;
}
void PolicyAndExistingCharges() {
  for (unsigned mutation = 0; mutation < 13; ++mutation) {
    Fixture f(false);
    auto policy = f.policy; auto workers = f.workers; auto queue = f.queue;
    auto governor_id = Id(11);
    switch (mutation) {
      case 0: governor_id = {}; break;
      case 1: policy.authority.database = {}; break;
      case 2: policy.authority.incarnation.bytes[8] = 0; break;
      case 3: policy.authority.policy.bytes[6] = 0x40; break;
      case 4: policy.authority.policy_generation = 0; break;
      case 5: policy.worker_capacity = 0; break;
      case 6: policy.queue_capacity = 0; break;
      case 7: workers.semaphore = {}; break;
      case 8: workers.generation = 0; break;
      case 9: queue.semaphore.bytes[8] = 0; break;
      case 10: queue.generation = 0; break;
      case 11: queue.semaphore = workers.semaphore; break;
      case 12: policy.authority.database = Id(99); break; // Wrong actual issuer database.
    }
    const auto before = f.governor.Snapshot();
    Check(f.governor.AcquireRuntimePermit(f.Request(true)).code == Code::policy_unbound,
          "no implicit runtime policy");
    Check(f.governor.BindRuntimePermits(governor_id, policy, workers, queue, *f.metadata, f.issuer) == Code::invalid_binding &&
          Same(before, f.governor.Snapshot()), "invalid policy or instance binding has no effects");
    Check(f.Bind() == Code::bound, "failed binding leaves one-time admission available");
    f.Empty();
  }
  Fixture f(false);
  a::ResourceGovernanceReservationAcquireRequest legacy;
  legacy.owner_uuid = Id(15); legacy.admission.operation_id = "existing-operation-label";
  auto& d = legacy.admission.descriptor;
  d.descriptor_id = "existing-policy-label";
  d.family = legacy.admission.expected_family = a::ResourceGovernanceFamily::kBackgroundJob;
  d.source = a::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  d.source_path_or_label = "runtime.test.policy"; d.descriptor_generation = d.expected_generation = 1;
  d.limits = {64,64,64,64,64,64,64,64,64,64,64,64,64};
  d.benchmark_clean = d.runtime_dependency_present = true;
  legacy.admission.requested.worker_threads = 2;
  auto old = f.governor.Acquire(legacy);
  Check(old.ok, "real preexisting node worker charges");
  f.policy.worker_capacity = 1;
  const auto old_state = f.governor.Snapshot();
  Check(f.Bind() == Code::exhausted && Same(old_state, f.governor.Snapshot()),
        "native binding cannot forget existing node usage");
  f.policy.worker_capacity = 2;
  Check(f.Bind() == Code::bound, "selected capacity includes prior two units");
  f.policy.worker_capacity = std::numeric_limits<std::uint32_t>::max();
  Check(f.governor.AcquireRuntimePermit(f.Request(true)).code == Code::exhausted,
        "mutating caller policy cannot enlarge pinned capacity");
  auto queued = f.governor.AcquireRuntimePermit(f.Request(false));
  Check(queued.ok(), "independent actual queue dimension");
  const auto before = f.governor.Snapshot();
  legacy.admission.requested.worker_threads = 1; d.limits.worker_threads = 1000;
  Check(!f.governor.Acquire(legacy).ok && Same(before, f.governor.Snapshot()),
        "legacy request cannot override selected native admission");
  Check(f.governor.ReleaseNoAlloc(old.reservation.token_id) == a::ResourceGovernanceReleaseCode::released,
        "preexisting legacy release remains available");
  auto worker = f.governor.AcquireRuntimePermit(f.Request(true));
  Check(worker.ok(), "native grant uses truly released shared capacity");
  Check(worker.permit.Release(f.Request(true)) == Code::released &&
        queued.permit.Release(f.Request(false)) == Code::released, "shared node ledger ends quiescent");
  f.Empty();
  for (bool short_memory : {false,true}) {
    Fixture maximum(false, short_memory ? 1 : 65536);
    maximum.policy.worker_capacity = maximum.policy.queue_capacity = std::numeric_limits<std::uint32_t>::max();
    Check(maximum.Bind() == Code::bound, "maximum exact u32 capacities do not preallocate phantom units");
    auto grant = maximum.governor.AcquireRuntimePermit(maximum.Request(true));
    if (short_memory) Check(!grant.ok() && grant.code == Code::allocation_failed &&
        maximum.governor.Snapshot().created_reservation_count == 0,
        "real metadata exhaustion refuses before governor publication");
    else Check(grant.ok() && grant.permit.Release(maximum.Request(true)) == Code::released,
               "one exact unit under maximum policy");
    maximum.Empty();
  }
}
void BindingAndRelease() {
  static_assert(!std::is_copy_constructible_v<a::RuntimePermitGrant>);
  static_assert(std::is_nothrow_move_constructible_v<a::RuntimePermitGrant>);
  Fixture f;
  for (bool worker : {false,true}) {
    auto request = f.Request(worker);
    for (unsigned mutation = 0; mutation < 16; ++mutation) {
      auto bad = request;
      switch (mutation) {
        case 0: bad.authority.database = Id(99); break;
        case 1: bad.authority.incarnation = Id(99); break;
        case 2: bad.authority.policy = Id(99); break;
        case 3: ++bad.authority.policy_generation; break;
        case 4: bad.task = {}; break;
        case 5: bad.attempt.bytes[6] = 0x40; break;
        case 6: bad.semaphore = Id(99); break;
        case 7: ++bad.semaphore_generation; break;
        case 8: bad.quantity = 0; break;
        case 9: bad.quantity = 2; break;
        case 10: if (worker) bad.worker.reset(); else bad.worker = Id(99); break;
        case 11: bad.profile = static_cast<a::RuntimePermitProfile>(99); break;
        case 12: bad.task.bytes[8] = 0; break;
        case 13: bad.attempt = {}; break;
        case 14: bad.worker = Uuid{}; break;
        case 15: bad.worker = Id(99); bad.worker->bytes[8] = 0; break;
      }
      const auto before = f.governor.Snapshot();
      auto refused = f.governor.AcquireRuntimePermit(bad);
      Check(!refused.ok() && refused.code == Code::invalid_binding && Same(before, f.governor.Snapshot()),
            "binding mismatch refused before real effects");
    }
    auto acquired = f.governor.AcquireRuntimePermit(request);
    Check(acquired.ok(), "real native grant");
    const auto view = *acquired.permit.view();
    Check(view.governor == Id(11) && u::IsEngineIdentityUuid(view.grant) && view.binding == request,
          "fresh actual binary UUID and exact retained binding");
    Check(f.manager.Snapshot().current_bytes > 0 && f.metadata->Snapshot().allocation_count > 0,
          "native grant index uses actual governed physical memory");
    auto duplicate = f.governor.AcquireRuntimePermit(request);
    Check(!duplicate.ok(), "same holder cannot duplicate its live unit");
    Check(f.governor.ExpireReservations(41).retained_count == 0 &&
          f.governor.ReleaseOwnerReservations(Id(99)).retained_count == 0 &&
          !acquired.permit.QuiescenceRequested(), "early expiry and foreign owner do not cancel holder");
    const auto expired = f.governor.ExpireReservations(42);
    Check(expired.ok && expired.released_count == 0 && expired.retained_count == 1 &&
          acquired.permit.QuiescenceRequested(), "expiry requests quiescence but retains live charge");
    const auto cleanup = f.governor.ReleaseOwnerReservations(request.task);
    Check(cleanup.released_count == 0 && cleanup.retained_count == 1 &&
          cleanup.snapshot.retained_runtime_permits == 1, "owner cleanup cannot revoke owning permit");
    for (unsigned n = 0; n < 11; ++n) {
      auto wrong = request;
      switch (n) {
        case 0: wrong.authority.database = Id(99); break;
        case 1: wrong.authority.incarnation = Id(99); break;
        case 2: wrong.authority.policy = Id(99); break;
        case 3: ++wrong.authority.policy_generation; break;
        case 4: wrong.task = Id(99); break;
        case 5: wrong.attempt = Id(99); break;
        case 6: wrong.worker = Id(99); break;
        case 7: wrong.semaphore = Id(99); break;
        case 8: ++wrong.semaphore_generation; break;
        case 9: ++wrong.quantity; break;
        case 10: ++wrong.lease_deadline_tick; break;
      }
      Check(acquired.permit.Release(wrong) == Code::invalid_binding && bool(acquired.permit),
            "wrong release preserves owning capability");
    }
    a::RuntimePermitGrant moved(std::move(acquired.permit));
    Check(!acquired.permit && moved.view()->grant == view.grant, "move transfers exact sole ownership");
    const std::string copied_bytes(reinterpret_cast<const char*>(view.grant.bytes.data()), 16);
    fault::hit = false; fault::remaining = 0;
    const auto raw = f.governor.ReleaseNoAlloc(copied_bytes);
    const auto released = moved.Release(request);
    const auto repeated = moved.Release(request);
    fault::remaining = -1;
    Check(!fault::hit && raw == a::ResourceGovernanceReleaseCode::not_found &&
          released == Code::released && repeated == Code::no_grant, "copied UUID is not capability; exact release allocates nothing");
    auto retry = f.governor.AcquireRuntimePermit(request);
    Check(retry.ok() && retry.permit.view()->grant != view.grant &&
          retry.permit.view()->issuance > view.issuance && moved.Release(request) == Code::no_grant &&
          f.governor.Snapshot().retained_runtime_permits == 1,
          "released predecessor cannot release retry's fresh binary grant");
    a::RuntimePermitGrant destination;
    destination = std::move(retry.permit);
    Check(destination && !retry.permit && destination.Release(request) == Code::released,
          "move assignment preserves the sole retry release capability");
  }
  Check(f.Bind() == Code::invalid_binding, "immutable policy cannot be rebound");
  {
    auto worker = f.governor.AcquireRuntimePermit(f.Request(true));
    auto queue = f.governor.AcquireRuntimePermit(f.Request(false));
    Check(worker.ok() && queue.ok(), "two owned units before quiescent reassignment");
    worker.permit = std::move(queue.permit);
    Check(worker.permit.view()->binding.profile == a::RuntimePermitProfile::queued_task &&
          !f.governor.Snapshot().active.worker_threads && f.governor.Snapshot().active.backlog_items == 1,
          "quiescent assignment releases only the replaced actual unit");
  }
  f.Empty();
}
void ContentionAndClose() {
  for (bool worker : {false,true}) {
    Fixture f(false); f.policy.worker_capacity = f.policy.queue_capacity = 1;
    Check(f.Bind() == Code::bound, "one actual unit selected");
    std::array<a::RuntimePermitAcquireResult, 8> outcomes;
    std::barrier start(8);
    {
      std::array<std::jthread, 8> threads;
      for (unsigned i = 0; i < threads.size(); ++i) threads[i] = std::jthread([&,i] {
        const auto request = f.Request(worker, 30+i);
        start.arrive_and_wait(); outcomes[i] = f.governor.AcquireRuntimePermit(request);
      });
    }
    unsigned granted = 0;
    Check(f.governor.CloseRuntimePermits() == Code::closed &&
          f.governor.CloseRuntimePermits() == Code::closed, "idempotent real close");
    for (auto& outcome : outcomes) if (outcome.ok()) {
      ++granted;
      Check(f.governor.Snapshot().retained_runtime_permits == 1, "close retains committed winner");
      Check(outcome.permit.Release(outcome.permit.view()->binding) == Code::released,
            "actual holder releases after close");
    }
    Check(granted == 1 && f.governor.AcquireRuntimePermit(f.Request(worker)).code == Code::closed,
          "eight contenders cannot overgrant or reopen a closed governor");
    f.Empty();
  }
}
void WorkerLifetimeAndRaces() {
  {
    Fixture f;
    const auto request = f.Request(true);
    auto grant = f.governor.AcquireRuntimePermit(request);
    Check(grant.ok(), "actual worker slot acquired");
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, finish = false;
    unsigned payload = 0;
    std::jthread worker([&, permit = std::move(grant.permit)] {
      std::unique_lock lock(mutex);
      payload = permit.view()->binding.task.bytes[15];
      entered = true; changed.notify_all();
      changed.wait(lock, [&] { return finish; });
      payload += 100; // Final callback work still uses the execution slot.
    }); // Actual moved grant is released by the worker's invocation destruction.
    {
      std::unique_lock lock(mutex); changed.wait(lock, [&] { return entered; });
    }
    const auto cleanup = f.governor.ReleaseOwnerReservations(request.task);
    Check(cleanup.retained_count == 1 && !cleanup.released_count &&
          f.governor.CloseRuntimePermits() == Code::closed &&
          f.governor.Snapshot().active.worker_threads == 1,
          "blocked live worker remains physically and logically owned through cleanup/close");
    { std::lock_guard lock(mutex); finish = true; changed.notify_all(); }
    worker.join();
    Check(payload == 120, "real worker callback completes before final owning release");
    f.Empty();
  }
  for (bool worker : {false,true}) for (unsigned iteration = 0; iteration < 32; ++iteration) {
    Fixture f;
    auto request = f.Request(worker);
    std::barrier start(2);
    a::RuntimePermitAcquireResult outcome;
    Code closed = Code::invalid_binding;
    {
      std::jthread acquirer([&] { start.arrive_and_wait(); outcome = f.governor.AcquireRuntimePermit(request); });
      std::jthread closer([&] { start.arrive_and_wait(); closed = f.governor.CloseRuntimePermits(); });
    }
    Check(closed == Code::closed && (outcome.ok() || outcome.code == Code::closed),
          "close and actual grant share one serialization boundary");
    if (outcome.ok()) {
      a::ResourceGovernanceReservationCleanupResult cleanup;
      Code released = Code::invalid_binding;
      std::barrier release_start(2);
      {
        std::jthread cleaner([&] { release_start.arrive_and_wait(); cleanup = f.governor.ExpireReservations(42); });
        std::jthread releaser([&] { release_start.arrive_and_wait(); released = outcome.permit.Release(request); });
      }
      Check(cleanup.ok && cleanup.released_count == 0 && cleanup.retained_count <= 1 &&
            released == Code::released, "expiry and actual owning release never duplicate a credit");
    }
    fault::hit = false; fault::remaining = 0;
    auto refused = f.governor.AcquireRuntimePermit(request);
    fault::remaining = -1;
    Check(!fault::hit && refused.code == Code::closed && !refused.permit,
          "closed entry allocates nothing and cannot mint a replacement grant");
    f.Empty();
  }
}
#if defined(__linux__)
void PrematureDestruction(const char* executable) {
  const auto child = ::fork();
  Check(child >= 0, "fresh-exec lifetime child launch");
  if (child == 0) { ::execl(executable, executable, "--destroy-live", nullptr); std::_Exit(99); }
  int status = 0;
  Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 73,
        "ledger destruction cannot reclaim a still-owned permit node");
}
#endif
void MetadataOwnerCleanup() {
  for (bool worker : {false, true}) {
    Fixture f;
    const auto request = f.Request(worker);
    auto grant = f.governor.AcquireRuntimePermit(request);
    Check(grant.ok(), "metadata cleanup starts with actual owned grant");
    const auto before = f.governor.Snapshot();
    const auto physical = f.manager.Snapshot().current_bytes;
    const auto cleanup = f.memory_ledger.CleanupOwner(Id(9).bytes);
    Check(cleanup.revoked_reservation_count == 1 && cleanup.retained_bytes > 0 &&
          cleanup.cleaned_reservation_count == 0 && cleanup.cleaned_bytes == 0,
          "actual memory owner cleanup retains the charged backing grant");
    Check(Same(before, f.governor.Snapshot()) && physical > 0 &&
          f.manager.Snapshot().current_bytes == physical,
          "memory owner cleanup cannot erase live permit or its physical metadata");
    auto next = f.governor.AcquireRuntimePermit(f.Request(worker, 21));
    Check(!next.ok() && !next.permit && Same(before, f.governor.Snapshot()) &&
          f.manager.Snapshot().current_bytes == physical,
          "revoked metadata denies new permit without unit debit");
    Check(grant.permit.Release(request) == Code::released,
          "existing owning permit releases through revoked metadata provider");
    f.Empty();
  }
}
void FaultSweep() {
  for (bool worker : {false,true}) for (unsigned operation = 0; operation < 3; ++operation)
  for (bool seeded : {false,true}) {
    if (!seeded && operation != 0) continue;
    bool complete = false; unsigned faults = 0, telemetry_successes = 0;
    for (long point = 0; point < 2048 && !complete; ++point) {
      Fixture f;
      a::RuntimePermitAcquireResult seed;
      if (seeded) seed = f.governor.AcquireRuntimePermit(f.Request(!worker));
      auto request = f.Request(worker);
      Check(!seeded || seed.ok(), "preexisting real grant retained during faults");
      const auto before = f.governor.Snapshot();
      const auto physical_before = f.manager.Snapshot();
      const auto physical = physical_before.current_bytes;
      bool ok = false, threw = false;
      a::RuntimePermitAcquireResult result;
      fault::hit = false; fault::remaining = point;
      try {
        if (operation == 0) { result = f.governor.AcquireRuntimePermit(request); ok = result.ok(); }
        if (operation == 1) ok = f.governor.ReleaseOwnerReservations(request.task).ok;
        if (operation == 2) ok = f.governor.ExpireReservations(42).ok;
      } catch (const std::bad_alloc&) { threw = true; }
      const bool hit = fault::hit; fault::remaining = -1;
      if (hit) {
        ++faults;
        if (ok) {
          // The physical allocator already owns its backing before optional
          // telemetry publication. Its documented telemetry truncation path
          // returns that real allocation, not a failed or unowned operation.
          // GDB qualification traces the first such fault to AllocateImpl's
          // post-allocation Snapshot. Require the complete successful effect.
          const auto after = f.governor.Snapshot();
          const auto physical_after = f.manager.Snapshot();
          Check(operation == 0 && result.ok() && !threw &&
                after.active_reservation_count == before.active_reservation_count + 1 &&
                after.created_reservation_count == before.created_reservation_count + 1 &&
                after.active.worker_threads == before.active.worker_threads + (worker ? 1 : 0) &&
                after.active.backlog_items == before.active.backlog_items + (worker ? 0 : 1) &&
                physical_after.current_bytes > physical &&
                physical_after.current_bytes == f.metadata->Snapshot().allocated_bytes &&
                physical_after.active_allocation_count == physical_before.active_allocation_count + 1 &&
                physical_after.telemetry_truncation_count > physical_before.telemetry_truncation_count,
                "handled telemetry loss retains one fully owned actual successful grant");
          ++telemetry_successes;
        } else {
          Check(Same(before, f.governor.Snapshot()) && physical == f.manager.Snapshot().current_bytes,
                "failed operation preserves real unit and metadata ownership");
          Check(!seed.permit.QuiescenceRequested(), "failed cleanup cannot publish revocation");
          if (operation == 0) Check(!threw && !result.permit, "native acquisition failure is typed and unowned");
        }
      } else { complete = true; Check(ok && !threw, "allocation sweep reaches actual success"); }
      if (result.permit) Check(result.permit.Release(request) == Code::released, "new grant actual cleanup");
      if (seeded) Check(seed.permit.Release(f.Request(!worker)) == Code::released, "preexisting grant actual cleanup");
      f.Empty();
    }
    Check(complete && faults > 0, "complete native allocation-position inventory");
    std::cout << "native profile=" << worker << " operation=" << operation << " seeded=" << seeded << " allocation_faults=" << faults
              << " owned_telemetry_successes=" << telemetry_successes << '\n';
  }
}
}
int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--destroy-live") {
    Fixture f;
    auto grant = f.governor.AcquireRuntimePermit(f.Request(true));
    Check(grant.ok(), "live destruction fixture owns actual grant");
    std::set_terminate([] { std::_Exit(73); });
    f.governor.~ResourceGovernanceReservationLedger();
    std::_Exit(99);
  }
  PolicyAndExistingCharges(); BindingAndRelease(); ContentionAndClose(); WorkerLifetimeAndRaces();
  MetadataOwnerCleanup(); FaultSweep();
#if defined(__linux__)
  PrematureDestruction(argv[0]);
#endif
  std::cout << "native permit checks=" << checks << '\n';
}
