// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "memory_safe_retirement.hpp"

#include <atomic>
#include <barrier>
#include <future>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <string_view>
#include <thread>

#if defined(__linux__)
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(SB_RETIRE_NATIVE_FAULT_GATE)
#include <cerrno>
#include <pthread.h>
thread_local bool fail_retirement_wait = false;
thread_local unsigned retirement_wait_calls = 0;
extern "C" int __real_pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const timespec*);
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const timespec* t) {
  ++retirement_wait_calls;
  if (std::exchange(fail_retirement_wait, false)) return EINVAL;
  return __real_pthread_cond_timedwait(c, m, t);
}
#if defined(_GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT)
extern "C" int __real_pthread_cond_clockwait(pthread_cond_t*, pthread_mutex_t*, clockid_t, const timespec*);
extern "C" int __wrap_pthread_cond_clockwait(pthread_cond_t* c, pthread_mutex_t* m, clockid_t clock, const timespec* t) {
  ++retirement_wait_calls;
  if (std::exchange(fail_retirement_wait, false)) return EINVAL;
  return __real_pthread_cond_clockwait(c, m, clock, t);
}
#endif
#endif

namespace {
namespace m = scratchbird::core::memory;
using S = m::SafeRetirementStatus;
constexpr auto kind = m::SafeRetirementObjectKind::temporary_descriptor;
unsigned checks = 0;
void Check(bool condition, const char* why) {
  ++checks;
  if (!condition) throw std::runtime_error(why);
}
m::MemoryBinaryUuid Id(unsigned n) {
  m::MemoryBinaryUuid value{};
  value[6] = 0x70; value[8] = 0x80;
  value[14] = static_cast<unsigned char>(n >> 8);
  value[15] = static_cast<unsigned char>(n);
  return value;
}
m::SafeRetirementHazard Hazard(unsigned n) { return {Id(n), Id(99)}; }
struct Fixture {
  m::MemoryManager manager;
  m::HierarchicalMemoryBudgetLedger ledger{3, 5};
  std::unique_ptr<m::ReservationBackedMemoryResource> resource;
  static auto Policy() {
    auto policy = m::DefaultLocalEngineMemoryPolicy();
    policy.hard_limit_bytes = policy.per_context_limit_bytes = 131072;
    return policy;
  }
  explicit Fixture(m::u64 bytes = 65536) : manager(Policy()) {
    m::ReservationBackedMemoryResourceRequest request;
    request.memory_manager = &manager; request.reservation_ledger = &ledger;
    request.consumer_kind = m::ReservationBackedMemoryConsumerKind::background_maintenance;
    request.requested_bytes = bytes;
    request.category = m::MemoryCategory::core_runtime;
    request.route_label = "runtime.retirement.conformance";
    request.purpose = "governed runtime object lifetime";
    request.binary_operation_uuid = Id(1);
    request.binary_ownership[m::MemoryBinaryScopeKind::context] = Id(2);
    request.binary_ownership[m::MemoryBinaryScopeKind::owner] = Id(3);
    request.binary_ownership[m::MemoryBinaryScopeKind::database] = Id(4);
    request.scope_chain = {{m::HierarchicalMemoryScopeKind::process, {}, Id(5)},
                          {m::HierarchicalMemoryScopeKind::database, {}, Id(4)}};
    request.provenance.source = m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    request.provenance.source_label = "safe retirement component fixture";
    for (const auto& scope : request.scope_chain) {
      m::HierarchicalMemoryBudget budget;
      budget.scope = scope; budget.hard_limit_bytes = bytes; budget.provenance = request.provenance;
      Check(ledger.SetBudget(budget).ok(), "real parent budget");
    }
    auto result = m::AcquireReservationBackedMemoryResource(std::move(request));
    Check(result.ok(), "real binary-scoped reservation");
    resource = std::move(result.resource);
  }
  void Empty() {
    Check(manager.Snapshot().current_bytes == 0, "actual payload and metadata reclaimed");
    Check(resource->ReleaseNoAlloc().ok(), "release parent reservation");
    Check(ledger.Snapshot().current_bytes == 0 && manager.Snapshot().reserved_capacity_bytes == 0,
          "parent and capacity charges released");
  }
};
struct alignas(64) Payload {
  unsigned value;
  std::atomic<unsigned>* destroyed;
  m::MemorySafeRetirement* domain;
  Payload(unsigned v, std::atomic<unsigned>& d, m::MemorySafeRetirement& owner)
      : value(v), destroyed(&d), domain(&owner) {}
  ~Payload() noexcept {
    // Reentrant read proves destructors do not execute under the domain lock.
    if (domain->Snapshot().reclaiming == 0) std::terminate();
    destroyed->fetch_add(1);
  }
};

void ProtectedLifetimeAndHandles() {
  Fixture f;
  std::atomic<unsigned> destroyed{0};
  {
    m::MemorySafeRetirement domain(*f.resource, Id(10), 2, 2);
    Check(domain.Initialize() == S::ok, "initialize governed metadata");
    const auto metadata = domain.Snapshot().metadata_bytes;
    Check(metadata != 0 && f.manager.Snapshot().current_bytes == metadata, "metadata physically charged");
    const auto created = domain.Emplace<Payload>(Id(11), kind, 42, destroyed, domain);
    Check(created.ok(), "construct and publish");
    Check(!domain.Emplace<unsigned>(Id(11), kind, 0).ok(), "duplicate live object refused");
    m::SafeRetirementGuard first, second, extra;
    Check(domain.Protect(created.handle, Hazard(12), first) == S::ok, "protect actual pointer");
    Check(reinterpret_cast<std::uintptr_t>(first.get()) % 64 == 0, "overalignment preserved");
    Check(domain.Protect(created.handle, Hazard(12), second) == S::invalid_request, "duplicate hazard refused");
    Check(domain.Protect(created.handle, Hazard(13), second) == S::ok, "second reader");
    Check(domain.Protect(created.handle, Hazard(14), extra) == S::exhausted, "bounded reader admission");
    Check(domain.Retire(created.handle) == S::ok && domain.Retire(created.handle) == S::ok, "idempotent retirement");
    Check(domain.Protect(created.handle, Hazard(14), extra) == S::stale_handle, "retirement fences new readers");
    Check(domain.Collect() == S::ok && destroyed.load() == 0, "protected object cannot be destroyed");
    Check(f.manager.Snapshot().current_bytes == metadata + sizeof(Payload), "retired bytes remain charged");
    Check(static_cast<Payload*>(first.get())->value == 42, "retired payload readable");
    auto moved = std::move(first);
    first.Reset(); second.Reset();
    Check(domain.Snapshot().readers == 1, "move transfers exactly one protection");
    Check(domain.Collect() == S::ok && destroyed.load() == 0, "last reader still blocks");
    moved.Reset(); moved.Reset();
    Check(domain.Collect() == S::ok && destroyed.load() == 1, "last release enables exactly one destruction");
    Check(f.manager.Snapshot().current_bytes == metadata, "actual payload release removes charge");
    const auto replacement = domain.Emplace<unsigned>(Id(11), kind, 77);
    Check(replacement.ok() && replacement.handle.slot == created.handle.slot &&
          replacement.handle.generation != created.handle.generation, "slot reuse gets new generation");
    Check(domain.Protect(created.handle, Hazard(15), extra) == S::stale_handle, "stale generation cannot protect reused slot");
    auto foreign = replacement.handle; foreign.domain = Id(88);
    Check(domain.Retire(foreign) == S::stale_handle, "cross-domain handle refused");
    const auto next = domain.Emplace<unsigned>(Id(16), kind, 88);
    Check(next.ok() && domain.Emplace<unsigned>(Id(17), kind, 99).status == S::exhausted,
          "object slots bounded");
    Check(domain.Drain(std::chrono::steady_clock::now()) == S::ok, "drain unprotected published objects");
    Check(domain.Emplace<unsigned>(Id(18), kind, 99).status == S::closed, "close fences allocation");
  }
  f.Empty();
}

void TimeoutCancellationAndRevocation() {
  Fixture f;
  {
    m::MemorySafeRetirement domain(*f.resource, Id(20), 2, 2);
    Check(domain.Initialize() == S::ok, "timeout metadata");
    const auto created = domain.Emplace<unsigned>(Id(21), kind, 123);
    Check(created.ok(), "timeout payload");
    m::SafeRetirementGuard guard;
    Check(domain.Protect(created.handle, Hazard(22), guard) == S::ok, "timeout protection");
    const auto charged = f.manager.Snapshot().current_bytes;
    Check(domain.Drain(std::chrono::steady_clock::now()) == S::timed_out, "deadline does not release live reader");
    std::stop_source cancellation;
    std::latch started(1);
    auto drain = std::async(std::launch::async, [&] {
      started.count_down();
      return domain.Drain(std::chrono::steady_clock::now() + std::chrono::seconds(30), cancellation.get_token());
    });
    started.wait(); cancellation.request_stop();
    Check(drain.get() == S::cancelled, "cancellation wakes or preempts drain");
    const auto revoked = f.ledger.Cancel(f.resource->reservation_token());
    Check(revoked.retained && f.manager.Snapshot().current_bytes == charged,
          "revocation and cancelled drain retain charges");
    Check(*static_cast<unsigned*>(guard.get()) == 123, "payload survives close cancel and revocation");
    guard.Reset();
    Check(domain.Drain(std::chrono::steady_clock::now() + std::chrono::seconds(1)) == S::ok,
          "existing holder release and actual reclaim work after revocation");
  }
  f.Empty();
}

struct PausedConstruction {
  std::atomic<unsigned>* destroyed;
  PausedConstruction(std::latch& entered, std::latch& release, std::atomic<unsigned>& count)
      : destroyed(&count) { entered.count_down(); release.wait(); }
  ~PausedConstruction() noexcept { ++*destroyed; }
};
struct FailedConstruction { FailedConstruction() { throw std::runtime_error("constructor failure"); } };

void ConstructionAndClose() {
  Fixture f;
  std::atomic<unsigned> destroyed{0};
  {
    m::MemorySafeRetirement domain(*f.resource, Id(30), 2, 2);
    Check(domain.Initialize() == S::ok, "construction metadata");
    Check(domain.Emplace<FailedConstruction>(Id(31), kind).status == S::construction_failed,
          "constructor failure reported");
    Check(domain.Snapshot().retired == 1 && domain.Collect() == S::ok, "failed construction backing retained until release");
    std::latch entered(1), release(1);
    auto creating = std::async(std::launch::async, [&] {
      return domain.Emplace<PausedConstruction>(Id(32), kind, entered, release, destroyed);
    });
    entered.wait();
    const auto construction = domain.Snapshot();
    const bool constructing = construction.constructing == 1 &&
        construction.by_kind[static_cast<m::usize>(kind)].constructing == 1 &&
        construction.reclamation_blocked_objects == 0;
    const auto result = domain.Drain(std::chrono::steady_clock::now());
    release.count_down();
    const auto created = creating.get();
    Check(constructing && result == S::timed_out, "close cannot reclaim constructor in flight");
    Check(created.status == S::closed && destroyed.load() == 0, "close prevents late publication");
    Check(domain.Drain(std::chrono::steady_clock::now() + std::chrono::seconds(1)) == S::ok &&
          destroyed.load() == 1, "late constructed object retired and destroyed once");
  }
  f.Empty();
}

void ConcurrentProtectRetire() {
  Fixture f;
  {
    m::MemorySafeRetirement domain(*f.resource, Id(40), 2, 8);
    Check(domain.Initialize() == S::ok, "concurrency metadata");
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
      const auto created = domain.Emplace<unsigned>(Id(41), kind, iteration);
      Check(created.ok(), "race publish");
      std::barrier begin(3), acquired(3), release(3);
      std::array<bool, 2> good{};
      std::array<std::thread, 2> readers;
      for (unsigned i = 0; i < readers.size(); ++i) readers[i] = std::thread([&, i] {
        m::SafeRetirementGuard guard;
        begin.arrive_and_wait();
        const auto status = domain.Protect(created.handle, Hazard(50 + i), guard);
        acquired.arrive_and_wait();
        good[i] = status == S::stale_handle || (status == S::ok && *static_cast<unsigned*>(guard.get()) == iteration);
        release.arrive_and_wait();
      });
      begin.arrive_and_wait();
      const auto retired = domain.Retire(created.handle);
      acquired.arrive_and_wait();
      const auto collected = domain.Collect();
      release.arrive_and_wait();
      for (auto& thread : readers) thread.join();
      Check(retired == S::ok && collected == S::ok && good[0] && good[1], "protect versus retire linearizes safely");
      Check(domain.Collect() == S::ok && domain.Snapshot().retained_payload_bytes == 0,
            "race leaves no live storage");
    }
  }
  f.Empty();
}

void InvalidAndExhaustedInitialization() {
  Fixture f;
  {
    m::MemorySafeRetirement nil(*f.resource, {}, 2, 2);
    Check(nil.Initialize() == S::invalid_request, "nil binary domain refused");
    m::MemorySafeRetirement huge(*f.resource, Id(60), std::numeric_limits<m::usize>::max(), 2);
    Check(huge.Initialize() == S::invalid_request, "metadata overflow refused");
    m::MemorySafeRetirement partial(*f.resource, Id(61), 1, 100000);
    Check(partial.Initialize() == S::allocation_failed, "real capacity denies metadata expansion");
    Check(partial.Snapshot().metadata_bytes > 0 && !partial.Snapshot().initialized,
          "partial initialization retains exact owned metadata");
    Check(partial.Emplace<unsigned>(Id(62), kind, 0).status == S::not_initialized, "partial domain cannot publish");
  }
  f.Empty();
}

void FailedReleaseRetainsCharge() {
  Fixture f;
  std::atomic<unsigned> destroyed{0};
  {
    m::MemorySafeRetirement domain(*f.resource, Id(70), 2, 2);
    Check(domain.Initialize() == S::ok, "release failure metadata");
    const auto created = domain.Emplace<Payload>(Id(71), kind, 13, destroyed, domain);
    Check(created.ok() && domain.Retire(created.handle) == S::ok, "release failure retire");
    const auto charged = f.manager.Snapshot().current_bytes;
    m::MemoryFailureInjectionConfiguration config{m::MakeMemoryFailureInjectionTestGuard(), false, {}, {}, {}};
    config.fixture_enabled = true;
    config.fixture_name = "safe retirement physical release failure";
    config.evidence_note = "real allocator pre-release failure retains physical allocation";
    m::MemoryFailureInjectionRule rule;
    rule.rule_id = "retired payload release";
    rule.purpose = "safe retirement payload";
    rule.callsite = "core.memory.reservation_backed_resource";
    config.rules.push_back(rule);
    Check(f.manager.allocator()->EnableAllocationFailureInjection(std::move(config)).ok(), "arm owning allocator fault");
    const auto collected = domain.Collect();
    Check(f.manager.allocator()->DisableAllocationFailureInjection().ok(), "disarm owning allocator fault");
    Check(collected == S::release_failed && destroyed.load() == 1,
          "physical release failure follows exactly one destructor");
    Check(domain.Snapshot().retired == 1 && domain.Snapshot().retained_payload_bytes == sizeof(Payload) &&
          f.manager.Snapshot().current_bytes == charged, "failed release retains actual backing and charge");
    const auto failed_release = domain.Snapshot();
    Check(failed_release.reclamation_blocked_objects == 0 &&
          failed_release.by_kind[static_cast<m::usize>(kind)].retired == 1 &&
          failed_release.by_kind[static_cast<m::usize>(kind)].reclamation_blocked_objects == 0,
          "physical release failure is not a hazard blocker");
    m::SafeRetirementGuard guard;
    Check(domain.Protect(created.handle, Hazard(72), guard) == S::stale_handle,
          "destroyed but unreleased object cannot be republished");
    Check(domain.Collect() == S::ok && destroyed.load() == 1 && domain.Snapshot().retained_payload_bytes == 0,
          "release retry does not repeat destructor");
  }
  f.Empty();
}

struct PausedDestruction {
  std::latch* entered;
  std::latch* release;
  std::atomic<unsigned>* destroyed;
  ~PausedDestruction() noexcept {
    entered->count_down(); release->wait(); ++*destroyed;
  }
};

void ConcurrentCollectorsAndDrain() {
  Fixture f;
  {
    m::MemorySafeRetirement domain(*f.resource, Id(80), 2, 2);
    Check(domain.Initialize() == S::ok, "collector metadata");
    std::latch entered(1), release(1);
    std::atomic<unsigned> destroyed{0};
    const auto created = domain.Emplace<PausedDestruction>(Id(81), kind, &entered, &release, &destroyed);
    Check(created.ok() && domain.Retire(created.handle) == S::ok, "collector retire");
    const auto charged = f.manager.Snapshot().current_bytes;
    auto collector = std::async(std::launch::async, [&] { return domain.Collect(); });
    entered.wait();
    const auto snapshot = domain.Snapshot();
    const auto still_charged = f.manager.Snapshot().current_bytes;
    const auto second = domain.Collect();
    const auto drain = domain.Drain(std::chrono::steady_clock::now());
    release.count_down();
    const auto first = collector.get();
    Check(snapshot.reclaiming == 1 && snapshot.retained_payload_bytes == sizeof(PausedDestruction) &&
          still_charged == charged, "in-flight destructor retains backing and charge");
    Check(snapshot.reclamation_blocked_objects == 0 &&
          snapshot.by_kind[static_cast<m::usize>(kind)].reclaiming == 1 &&
          snapshot.by_kind[static_cast<m::usize>(kind)].retained_payload_bytes == sizeof(PausedDestruction),
          "in-flight destructor remains measured without inventing a hazard");
    Check(second == S::ok && drain == S::timed_out && first == S::ok && destroyed.load() == 1,
          "second collector and drain cannot release destructor-in-flight backing");
    Check(domain.Drain(std::chrono::steady_clock::now()) == S::ok, "completed collector permits drain");
  }
  f.Empty();
}

void InvalidAdmissionAndPayloadCapacity() {
  Fixture f(4096);
  {
    m::MemorySafeRetirement domain(*f.resource, Id(90), 2, 2);
    Check(domain.Initialize() == S::ok, "invalid admission metadata");
    const auto metadata = f.manager.Snapshot().current_bytes;
    Check(domain.Emplace<std::array<unsigned char, 65536>>(Id(91), kind).status == S::allocation_failed,
          "physical reservation denies oversized payload");
    Check(domain.Snapshot().constructing == 0 && f.manager.Snapshot().current_bytes == metadata,
          "failed payload admission leaves slots and charges unchanged");
    for (unsigned mode = 0; mode < 3; ++mode) {
      auto invalid = Id(92);
      if (mode == 0) invalid = {};
      if (mode == 1) invalid[6] = 0x40;
      if (mode == 2) invalid[8] = 0;
      Check(domain.Emplace<unsigned>(invalid, kind, 1).status == S::invalid_request,
            "malformed binary object refused before effects");
    }
    const auto created = domain.Emplace<unsigned>(Id(93), kind, 17);
    Check(created.ok(), "valid admission after capacity and identity failure");
    m::SafeRetirementGuard guard;
    auto hazard = Hazard(94); hazard.owner_task = {};
    Check(domain.Protect(created.handle, hazard, guard) == S::invalid_request && !guard,
          "invalid task cannot acquire reader slot");
    hazard = Hazard(94); hazard.hazard_id[6] = 0x40;
    Check(domain.Protect(created.handle, hazard, guard) == S::invalid_request && !guard,
          "invalid hazard cannot acquire reader slot");
    auto forged = created.handle; forged.slot = std::numeric_limits<m::usize>::max();
    Check(domain.Protect(forged, Hazard(94), guard) == S::stale_handle,
          "out of bounds handle cannot index metadata");
    Check(domain.Protect(created.handle, Hazard(94), guard) == S::ok, "valid protection after refusal");
  }
  f.Empty();
}
void BoundedReaderInspection() {
  Fixture f;
  {
    m::MemorySafeRetirement domain(*f.resource, Id(110), 2, 4);
    std::array<m::SafeRetirementReaderRecord, 3> records{};
    using B = m::SafeRetirementBoundary;
    Check(domain.InspectRetainedReaders(Id(99), B::task_completion, records).status ==
              S::not_initialized, "inspection before initialized metadata");
    Check(domain.Initialize() == S::ok, "inspection metadata");
    const auto object = domain.Emplace<unsigned>(Id(111), kind, 17);
    Check(object.ok(), "inspection real payload");
    std::array<m::SafeRetirementGuard, 4> guards;
    Check(domain.Protect(object.handle, {Id(112), Id(99), B::operation_completion}, guards[0]) == S::ok &&
          domain.Protect(object.handle, {Id(113), Id(99), B::task_completion}, guards[1]) == S::ok &&
          domain.Protect(object.handle, {Id(114), Id(99), B::runtime_shutdown}, guards[2]) == S::ok &&
          domain.Protect(object.handle, {Id(115), Id(98), B::operation_completion}, guards[3]) == S::ok,
          "four real scoped reader holds");
    const auto charged = f.manager.Snapshot().current_bytes;
    const auto allocations = f.resource->Snapshot().allocation_count;
    const auto operation = domain.InspectRetainedReaders(Id(99), B::operation_completion, records);
    Check(operation.status == S::ok && operation.matching_readers == 1 &&
              operation.records_written == 1 && !operation.truncated,
          "operation boundary excludes later deadlines and another owner");
    Check(records[0].hazard.hazard_id == Id(112) && records[0].hazard.owner_task == Id(99) &&
              records[0].object == object.handle && records[0].object_uuid == Id(111) &&
              records[0].object_kind == kind && records[0].object_bytes == sizeof(unsigned) &&
              !records[0].retirement_requested,
          "exact binary protected record without payload exposure");
    const auto task = domain.InspectRetainedReaders(Id(99), B::task_completion, records);
    Check(task.matching_readers == 2 && task.records_written == 2 && !task.truncated &&
              records[1].hazard.hazard_id == Id(113), "task boundary includes earlier holds");
    const auto bounded = domain.InspectRetainedReaders(Id(99), B::runtime_shutdown,
        std::span(records).first(1));
    Check(bounded.matching_readers == 3 && bounded.records_written == 1 && bounded.truncated,
          "bounded output counts omitted records and explicitly truncates");
    const auto count = domain.InspectRetainedReaders(Id(99), B::runtime_shutdown, {});
    Check(count.matching_readers == 3 && count.records_written == 0 && count.truncated,
          "empty output cannot hide outstanding holds");
    records[0].object_uuid = Id(120);
    Check(domain.InspectRetainedReaders({}, B::runtime_shutdown, records).status == S::invalid_request &&
              records[0].object_uuid == Id(120), "invalid task leaves caller output untouched");
    Check(domain.InspectRetainedReaders(Id(99), static_cast<B>(99), records).status == S::invalid_request &&
              records[0].object_uuid == Id(120), "invalid boundary leaves output untouched");
    const auto other = domain.InspectRetainedReaders(Id(97), B::runtime_shutdown, records);
    Check(other.status == S::ok && other.matching_readers == 0 && !other.truncated &&
              records[0].object_uuid == Id(120), "no nonmatching task records");
    domain.Close();
    const auto closed = domain.InspectRetainedReaders(Id(99), B::runtime_shutdown, records);
    Check(closed.status == S::ok && closed.matching_readers == 3 && records[0].retirement_requested,
          "close retains inspection and all outstanding hazards");
    Check(domain.Collect() == S::ok && domain.Snapshot().readers == 4 &&
              f.manager.Snapshot().current_bytes == charged &&
              f.resource->Snapshot().allocation_count == allocations &&
              *static_cast<unsigned*>(guards[0].get()) == 17,
          "inspection allocates nothing and cannot reclaim or revoke holds");
    guards[0].Reset(); guards[1].Reset(); guards[2].Reset();
    const auto own_clear = domain.InspectRetainedReaders(Id(99), B::runtime_shutdown, records);
    Check(own_clear.matching_readers == 0 && domain.Snapshot().readers == 1 &&
              domain.Drain(std::chrono::steady_clock::now()) == S::timed_out,
          "zero task records is not domain drain or authority to free");
    guards[3].Reset();
    Check(domain.Drain(std::chrono::steady_clock::now()) == S::ok,
          "all actual releases permit safe drain");
  }
  f.Empty();
}

void ConcurrentReaderInspection() {
  Fixture f;
  {
    m::MemorySafeRetirement domain(*f.resource, Id(121), 1, 1);
    Check(domain.Initialize() == S::ok, "concurrent inspection metadata");
    std::barrier boundary(2);
    m::SafeRetirementHandle expected;
    bool consistent = true;
    std::thread inspector([&] {
      for (unsigned i = 0; i != 100; ++i) {
        boundary.arrive_and_wait();
        std::array<m::SafeRetirementReaderRecord, 1> record{};
        const auto result = domain.InspectRetainedReaders(
            Id(99), m::SafeRetirementBoundary::runtime_shutdown, record);
        const auto metrics = domain.Snapshot();
        const auto& kind_metrics = metrics.by_kind[static_cast<m::usize>(kind)];
        consistent = consistent && metrics.retired == 1 && metrics.readers <= 1 &&
            metrics.reclamation_blocked_objects == metrics.readers &&
            metrics.retained_payload_bytes == sizeof(unsigned) &&
            kind_metrics.retired == metrics.retired && kind_metrics.readers == metrics.readers &&
            kind_metrics.reclamation_blocked_objects == metrics.reclamation_blocked_objects &&
            metrics.reclamation_blocked_bytes == metrics.readers * sizeof(unsigned);
        consistent = consistent && result.status == S::ok && !result.truncated &&
            result.matching_readers <= 1 && result.records_written == result.matching_readers;
        if (result.records_written)
          consistent = consistent && record[0].object == expected &&
              record[0].object_uuid == Id(130 + i) && record[0].hazard.hazard_id == Id(230 + i) &&
              record[0].hazard.owner_task == Id(99) && record[0].retirement_requested &&
              record[0].object_bytes == sizeof(unsigned);
        boundary.arrive_and_wait();
      }
    });
    bool publications = true;
    for (unsigned i = 0; i != 100; ++i) {
      const auto object = domain.Emplace<unsigned>(Id(130 + i), kind, i);
      m::SafeRetirementGuard guard;
      publications = publications && object.ok();
      publications = (domain.Protect(object.handle, Hazard(230 + i), guard) == S::ok) && publications;
      publications = (domain.Retire(object.handle) == S::ok) && publications;
      expected = object.handle;
      boundary.arrive_and_wait();
      guard.Reset();
      boundary.arrive_and_wait();
      publications = (domain.Collect() == S::ok) && publications;
    }
    inspector.join();
    Check(publications && consistent, "release/inspection race never mixes reused reader/object identities");
    Check(domain.Snapshot().readers == 0 && domain.Snapshot().retained_payload_bytes == 0,
          "inspection creates no retained reader or payload charge");
  }
  f.Empty();
}
void ReclamationGaugesByKind() {
  Fixture f;
  constexpr auto count = m::kSafeRetirementObjectKindCount;
  {
    m::MemorySafeRetirement domain(*f.resource, Id(500), count, count * 2);
    Check(domain.Initialize() == S::ok, "kind gauge metadata");
    std::array<m::SafeRetirementHandle, count> handles;
    std::array<m::SafeRetirementGuard, count * 2> guards;
    for (unsigned i = 0; i != count; ++i) {
      const auto created = domain.Emplace<unsigned>(Id(501 + i),
          static_cast<m::SafeRetirementObjectKind>(i), i);
      Check(created.ok(), "all object kinds actually allocated");
      handles[i] = created.handle;
      Check(domain.Protect(handles[i], Hazard(520 + i * 2), guards[i * 2]) == S::ok &&
            domain.Protect(handles[i], Hazard(521 + i * 2), guards[i * 2 + 1]) == S::ok,
            "two real readers for each object kind");
    }
    const auto published = domain.Snapshot();
    Check(published.published == count && published.readers == count * 2 &&
          published.reclamation_blocked_objects == 0 && published.reclamation_blocked_bytes == 0,
          "live published readers are not deferred-reclamation objects");
    for (unsigned i = 0; i != count; ++i) {
      const auto& gauge = published.by_kind[i];
      Check(gauge.published == 1 && gauge.readers == 2 &&
            gauge.retained_payload_bytes == sizeof(unsigned) && gauge.reclamation_blocked_objects == 0,
            "per-kind publication/reader/byte gauges");
      Check(domain.Retire(handles[i]) == S::ok, "retire each protected kind");
    }
    const auto retired = domain.Snapshot();
    Check(retired.retired == count && retired.reclamation_blocked_objects == count &&
          retired.reclamation_blocked_bytes == count * sizeof(unsigned),
          "blocked object and byte gauges do not double count readers");
    for (unsigned i = 0; i != count; ++i) {
      const auto& gauge = retired.by_kind[i];
      Check(gauge.retired == 1 && gauge.published == 0 && gauge.readers == 2 &&
            gauge.reclamation_blocked_objects == 1 && gauge.reclamation_blocked_bytes == sizeof(unsigned),
            "exact retired object kind and retained bytes");
      guards[i * 2].Reset();
    }
    Check(domain.Snapshot().reclamation_blocked_objects == count,
          "first reader release does not remove last-reader blocker");
    for (unsigned i = 0; i != count; ++i) {
      guards[i * 2 + 1].Reset();
      const auto released = domain.Snapshot();
      Check(released.reclamation_blocked_objects == count - i - 1 &&
            released.reclamation_blocked_bytes == (count - i - 1) * sizeof(unsigned) &&
            released.by_kind[i].reclamation_blocked_objects == 0 &&
            released.by_kind[i].retired == 1 && released.by_kind[i].retained_payload_bytes == sizeof(unsigned),
            "last reader removes blocker but does not fabricate physical release");
    }
    Check(domain.Collect() == S::ok, "collect all unprotected kinds");
    const auto collected = domain.Snapshot();
    Check(collected.retired == 0 && collected.retained_payload_bytes == 0 &&
          collected.reclamation_blocked_objects == 0, "real collection clears payload gauges");
    for (const auto& gauge : collected.by_kind)
      Check(gauge.constructing == 0 && gauge.published == 0 && gauge.retired == 0 &&
            gauge.reclaiming == 0 && gauge.readers == 0 && gauge.retained_payload_bytes == 0 &&
            gauge.reclamation_blocked_objects == 0 && gauge.reclamation_blocked_bytes == 0,
            "reclaimed slot leaves no stale kind gauge");
  }
  f.Empty();
}
void NativeDrainFailure() {
#if defined(SB_RETIRE_NATIVE_FAULT_GATE)
  Fixture f;
  std::atomic<unsigned> destroyed{0};
  {
    m::MemorySafeRetirement domain(*f.resource, Id(10), 2, 2);
    Check(domain.Initialize() == S::ok, "native failure domain");
    const auto made = domain.Emplace<Payload>(Id(11), kind, 42, destroyed, domain);
    Check(made.ok(), "native failure actual payload");
    m::SafeRetirementGuard guard;
    Check(domain.Protect(made.handle, Hazard(12), guard) == S::ok, "native failure actual reader");
    const auto charged = f.manager.Snapshot().current_bytes;
    retirement_wait_calls = 0; fail_retirement_wait = true;
    const auto result = domain.Drain(std::chrono::steady_clock::now() + std::chrono::milliseconds(20));
    const bool injected = !fail_retirement_wait;
    fail_retirement_wait = false;
    const auto calls = retirement_wait_calls;
    Check(result != S::ok && destroyed == 0 && domain.Snapshot().readers == 1 &&
          f.manager.Snapshot().current_bytes == charged &&
          static_cast<Payload*>(guard.get())->value == 42,
          "native failure preserves live payload and charge");
    guard.Reset();
    Check(domain.Drain(std::chrono::steady_clock::now() + std::chrono::seconds(1)) == S::ok &&
          destroyed == 1, "real last-reader release permits retry and exact destruction");
    Check(injected && calls == 1 && result == S::wait_failed,
          "retirement native error is not timeout or another park");
  }
  f.Empty();
#endif
}

#if defined(__linux__)
// Fresh exec children deliberately violate the owning destruction contract.
// The terminate observer checks the real allocator, not only an exit code:
// payload AND metadata must remain charged at the fail-fast boundary.
Fixture* destruction_fixture = nullptr;
std::atomic<unsigned>* destruction_count = nullptr;
m::u64 destruction_charge = 0;
bool destruction_armed = false;

[[noreturn]] void PrematureDestructionChild(std::string_view mode) {
  alarm(20);  // Watchdog only; latch/state observations establish the schedule.
  std::set_terminate([] {
    const bool retained = destruction_armed && destruction_fixture && destruction_count &&
        destruction_count->load() == 0 &&
        destruction_fixture->manager.Snapshot().current_bytes == destruction_charge &&
        destruction_fixture->resource->Snapshot().allocation_count == 3;
    _exit(retained ? 86 : 87);
  });
  Fixture f;
  std::atomic<unsigned> destroyed{0};
  auto domain = std::make_unique<m::MemorySafeRetirement>(*f.resource, Id(600), 1, 1);
  Check(domain->Initialize() == S::ok, "premature destruction metadata");
  m::SafeRetirementGuard guard;
  std::latch entered(1), release(1);
  std::thread active;
  if (mode == "reader") {
    const auto made = domain->Emplace<Payload>(Id(601), kind, 42, destroyed, *domain);
    Check(made.ok() && domain->Protect(made.handle, Hazard(602), guard) == S::ok,
          "premature destruction actual protected payload");
    Check(domain->Snapshot().readers == 1, "live reader destruction boundary");
  } else if (mode == "constructor") {
    active = std::thread([&] {
      (void)domain->Emplace<PausedConstruction>(Id(601), kind, entered, release, destroyed);
    });
    entered.wait();
    Check(domain->Snapshot().constructing == 1, "live constructor destruction boundary");
  } else if (mode == "collector") {
    const auto made = domain->Emplace<PausedDestruction>(Id(601), kind, &entered, &release, &destroyed);
    Check(made.ok() && domain->Retire(made.handle) == S::ok, "premature destruction retired payload");
    active = std::thread([&] { (void)domain->Collect(); });
    entered.wait();
    Check(domain->Snapshot().reclaiming == 1, "live collector destruction boundary");
  } else {
    _exit(88);
  }
  destruction_fixture = &f;
  destruction_count = &destroyed;
  destruction_charge = f.manager.Snapshot().current_bytes;
  Check(destruction_charge > domain->Snapshot().metadata_bytes &&
        f.resource->Snapshot().allocation_count == 3, "actual payload and both metadata allocations");
  destruction_armed = true;
  domain.reset();
  // Do not let a joinable thread's destructor masquerade as the domain guard.
  _exit(89);
}

void PrematureDestruction() {
  for (const char* mode : {"reader", "constructor", "collector"}) {
    const auto child = fork();
    Check(child >= 0, "launch isolated lifetime child");
    if (child == 0) {
      execl("/proc/self/exe", "memory_safe_retirement_gate", "--premature-destruction", mode,
            static_cast<char*>(nullptr));
      _exit(90);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    Check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 86,
          "domain destruction fails fast before freeing reachable backing or metadata");
  }
}
#endif
}  // namespace

int main(int argc, char** argv) {
  try {
#if defined(__linux__)
    if (argc == 3 && std::string_view(argv[1]) == "--premature-destruction")
      PrematureDestructionChild(argv[2]);
    Check(argc == 1, "recognized invocation");
    PrematureDestruction();
#else
    (void)argc; (void)argv;
#endif
    ProtectedLifetimeAndHandles(); TimeoutCancellationAndRevocation();
    ConstructionAndClose(); ConcurrentProtectRetire(); InvalidAndExhaustedInitialization();
    FailedReleaseRetainsCharge();
    ConcurrentCollectorsAndDrain();
    InvalidAdmissionAndPayloadCapacity();
    BoundedReaderInspection();
    ConcurrentReaderInspection();
    ReclamationGaugesByKind(); NativeDrainFailure();
    std::cout << "PASS safe retirement checks=" << checks << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
