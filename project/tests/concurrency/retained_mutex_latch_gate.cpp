// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "retained_mutex_latch.hpp"
#include <atomic>
#include <iostream>
#include <semaphore>
#include <vector>
#if defined(__unix__)
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(SB_MUTEX_NATIVE_FAULT_GATE)
struct Park {
  std::binary_semaphore entered{0}, committed{0}, resume{0};
  bool observed = false;
  bool pause_delivery = false;
};
thread_local Park* park = nullptr;
thread_local pthread_mutex_t* pause_unlock = nullptr;
thread_local bool fail_wait = false;
thread_local unsigned fail_lock = 0;
extern "C" int __real_pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
extern "C" int __real_pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const timespec*);
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t*);
void ObservePark() {
  if (park && !park->observed) { park->observed = true; park->entered.release(); }
}
extern "C" int __wrap_pthread_cond_wait(pthread_cond_t* condition, pthread_mutex_t* mutex) {
  if (std::exchange(fail_wait, false)) return EINVAL;
  ObservePark();
  const auto result=__real_pthread_cond_wait(condition, mutex);
  if (!result && park && park->pause_delivery) pause_unlock=mutex;
  return result;
}
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* condition, pthread_mutex_t* mutex,
                                           const timespec* deadline) {
  if (std::exchange(fail_wait, false)) return EINVAL;
  ObservePark();
  const auto result=__real_pthread_cond_timedwait(condition, mutex, deadline);
  if (!result && park && park->pause_delivery) pause_unlock=mutex;
  return result;
}
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (fail_lock && --fail_lock == 0) return EINVAL;
  return __real_pthread_mutex_lock(mutex);
}
extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) {
  const bool pause=pause_unlock==mutex;
  if (pause) pause_unlock=nullptr;
  const auto result=__real_pthread_mutex_unlock(mutex);
  if (pause) { park->committed.release(); park->resume.acquire(); }
  return result;
}
#endif

namespace {
namespace m = scratchbird::core::memory;
namespace c = scratchbird::core::concurrency;
using S = m::SafeRetirementStatus;
using C = c::MutexLatchCode;
using namespace std::chrono_literals;
std::atomic<unsigned> checks{0};
void Check(bool value, const char* reason) {
  ++checks;
  if (!value) { std::cerr << "FAIL " << reason << '\n'; std::abort(); }
}
m::MemoryBinaryUuid Id(unsigned n) {
  m::MemoryBinaryUuid id{};
  id[6]=0x70; id[8]=0x80;
  id[14]=static_cast<unsigned char>(n>>8); id[15]=static_cast<unsigned char>(n);
  return id;
}
m::SafeRetirementHazard Hazard(unsigned n, unsigned task=99) { return {Id(n), Id(task)}; }
c::MutexLatchDescriptor Descriptor() {
  return {Id(20), Id(22), c::MutexOwnerScope::database, 7, 11, Id(23)};
}
c::MutexLatchRequest Request(unsigned n=100, unsigned task=99) {
  return {Id(20),7,Id(task),Id(n)};
}
struct Fixture {
  m::MemoryManager manager;
  m::HierarchicalMemoryBudgetLedger ledger{3,5};
  std::unique_ptr<m::ReservationBackedMemoryResource> resource;
  static auto Policy() {
    auto p=m::DefaultLocalEngineMemoryPolicy();
    p.hard_limit_bytes=p.per_context_limit_bytes=131072;
    return p;
  }
  Fixture() : manager(Policy()) {
    m::ReservationBackedMemoryResourceRequest r;
    r.memory_manager=&manager; r.reservation_ledger=&ledger;
    r.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.requested_bytes=65536; r.category=m::MemoryCategory::core_runtime;
    r.route_label="runtime.mutex.conformance"; r.purpose="governed mutex lifetime";
    r.binary_operation_uuid=Id(1);
    r.binary_ownership[m::MemoryBinaryScopeKind::context]=Id(2);
    r.binary_ownership[m::MemoryBinaryScopeKind::owner]=Id(3);
    r.binary_ownership[m::MemoryBinaryScopeKind::database]=Id(4);
    r.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(5)},
                   {m::HierarchicalMemoryScopeKind::database,{},Id(4)}};
    r.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label="mutex component fixture";
    for (const auto& scope:r.scope_chain) {
      m::HierarchicalMemoryBudget budget;
      budget.scope=scope; budget.hard_limit_bytes=r.requested_bytes; budget.provenance=r.provenance;
      Check(ledger.SetBudget(budget).ok(),"actual parent budget");
    }
    auto acquired=m::AcquireReservationBackedMemoryResource(std::move(r));
    Check(acquired.ok(),"actual governed reservation"); resource=std::move(acquired.resource);
  }
  void Empty() {
    Check(manager.Snapshot().current_bytes==0,"actual native payload and metadata freed");
    Check(resource->ReleaseNoAlloc().ok(),"actual reservation release");
    Check(ledger.Snapshot().current_bytes==0 && manager.Snapshot().reserved_capacity_bytes==0,
          "all actual parent charges gone");
  }
};
c::MutexLatchOperation Operation(c::MutexLatchOwner& owner, unsigned hazard, unsigned task=99) {
  c::MutexLatchOperation op;
  Check(owner.AcquireOperation(Id(20),7,Hazard(hazard,task),op)==S::ok,"admit real operation guard");
  return op;
}
void Finish(c::MutexLatchOwner& owner) {
  Check(owner.Close(Id(24)),"close retained mutex");
  Check(owner.FenceAdmission()==S::ok,"actual retirement fence");
  Check(owner.Drain(c::MutexClock::now()+5s).code==C::drained,"actual reference and native holder drain");
}
template<class Function>
void Run(Function function, c::MutexLatchLimits limits={16,32}, unsigned readers=64) {
  Fixture fixture;
  {
    m::MemorySafeRetirement domain(*fixture.resource,Id(10),2,readers);
    Check(domain.Initialize()==S::ok,"real memory domain initialized");
    const auto metadata=fixture.manager.Snapshot().current_bytes;
    {
      c::MutexLatchOwner owner(domain);
      Check(owner.Initialize(Descriptor(),limits,Hazard(30))==S::ok,"publish real mutex payload");
      Check(fixture.manager.Snapshot().current_bytes>metadata,"mutex payload physically charged");
      function(owner,domain);
      Finish(owner);
    }
    Check(domain.Collect()==S::ok && domain.Snapshot().retired==0,"actual memory owner collects drained mutex");
    Check(fixture.manager.Snapshot().current_bytes==metadata,"only memory domain metadata remains");
  }
  fixture.Empty();
}
void Descriptors() {
  Fixture f;
  {
    m::MemorySafeRetirement domain(*f.resource,Id(10),2,8);
    Check(domain.Initialize()==S::ok,"descriptor domain initialized");
    for (unsigned test=0;test<12;++test) {
      auto d=Descriptor(); auto h=Hazard(30); c::MutexLatchLimits limits{2,2};
      switch (test) {
        case 0:d.primitive_id={};break;
        case 1:d.last_transition={};break;
        case 2:d.owner_uuid=m::MemoryBinaryUuid{};break;
        case 3:d.owner_uuid.reset();break;
        case 4:d.owner_scope=static_cast<c::MutexOwnerScope>(255);break;
        case 5:d.ownership_profile=static_cast<c::MutexOwnershipProfile>(255);break;
        case 6:d.ownership_profile=c::MutexOwnershipProfile::process_global;break;
        case 7:limits.waiters=0;break;
        case 8:limits.operation_references=0;break;
        case 9:h.hazard_id={};break;
        case 10:h.owner_task={};break;
        case 11:h.release_required_by=static_cast<m::SafeRetirementBoundary>(255);break;
      }
      const auto before=f.manager.Snapshot().current_bytes;
      c::MutexLatchOwner owner(domain);
      Check(owner.Initialize(d,limits,h)==S::invalid_request,"invalid descriptor refused before publication");
      Check(!owner.Snapshot().initialized && domain.Snapshot().published==0 &&
            f.manager.Snapshot().current_bytes==before,"invalid descriptor has no physical effects");
    }
    for (const auto generation:{std::uint64_t{0},std::numeric_limits<std::uint64_t>::max()}) {
      {
        c::MutexLatchOwner owner(domain); auto d=Descriptor();
        d.owner_scope=c::MutexOwnerScope::engine; d.ownership_profile=c::MutexOwnershipProfile::process_global;
        d.owner_uuid.reset(); d.generation=generation;
        Check(owner.Initialize(d,{1,1},Hazard(30))==S::ok,"declared global ownership and generation boundaries");
        Finish(owner);
      }
      Check(domain.Collect()==S::ok,"descriptor fixture collected");
    }
  }
  f.Empty();
}
void IdentityAndModes() {
  Run([](auto& owner,auto& domain) {
    auto op=Operation(owner,31);
    auto request=Request(); c::MutexLatchGrant grant;
    for (unsigned held=0;held<2;++held) {
      if (held) Check(op.Acquire(request,Hazard(32),grant,{}, {},true).code==C::acquired,"valid exclusive mode");
      for (unsigned mode=0;mode<=12;++mode) {
        if (mode==static_cast<unsigned>(c::MutexLatchMode::exclusive_write)) continue;
        auto invalid=request; invalid.mode=static_cast<c::MutexLatchMode>(mode);
        c::MutexLatchGrant rejected;
        Check(op.Acquire(invalid,Hazard(33),rejected,{}, {},true).code==C::invalid_mode,"all other modes and conversions refused");
        Check(!rejected && owner.Snapshot().native.held==bool(held),"mode rejection preserves holder");
      }
    }
    for (unsigned field=0;field<4;++field) {
      auto different=request;
      if (field==0) different.primitive_id=Id(123);
      if (field==1) ++different.generation;
      if (field==2) different.task_id=Id(124);
      if (field==3) different.request_id=Id(125);
      Check(grant.Release(different).code==C::wrong_owner && grant,"release requires complete exact binary request");
    }
    for (unsigned bit=0;bit<128;++bit) {
      auto changed=request; changed.request_id[bit/8]^=static_cast<std::uint8_t>(1u<<(bit%8));
      Check(grant.Release(changed).code==C::wrong_owner,"release compares every binary request bit including embedded zeroes");
    }
    c::MutexLatchGrant rejected;
    Check(op.Acquire(request,Hazard(33),rejected,{}, {},true).code==C::recursive,"same request cannot recurse");
    auto next=request; next.request_id=Id(101);
    Check(op.Acquire(next,Hazard(33),rejected,{}, {},true).code==C::recursive,"new request cannot authorize recursion");
    std::thread moved([&] {
      Check(op.Acquire(next,Hazard(33),rejected,{}, {},true).code==C::recursive,"binary task recursion across native threads");
      Check(grant.Release(request).code==C::wrong_owner && grant,"foreign native release retains real guard");
    }); moved.join();
    const auto readers=domain.Snapshot().readers;
    c::MutexLatchGrant moved_grant(std::move(grant));
    Check(!grant && moved_grant && domain.Snapshot().readers==readers,"move transfers real guard without re-admission");
    Check(grant.Release(request).code==C::no_grant,"moved-from release cannot affect holder");
    Check(moved_grant.Release(request).code==C::released,"exact owner release");
    Check(moved_grant.Release(request).code==C::no_grant,"duplicate release refused");
    Check(domain.Snapshot().readers==2,"release removes actual grant reader only");
  });
}
void LifetimeAndExhaustion() {
  Run([](auto& owner,auto& domain) {
    auto op=Operation(owner,31); c::MutexLatchOperation other;
    Check(owner.AcquireOperation(Id(20),7,Hazard(32),other)==S::exhausted && !other,"operation bound before grant");
    c::MutexLatchGrant grant; auto request=Request();
    Check(op.Acquire(request,Hazard(33),grant,{}, {},true).code==C::acquired,"retained live grant");
    op.Reset();
    Check(owner.Snapshot().operation_references==0 && domain.Snapshot().readers==2,"grant survives operation release");
    Check(owner.Drain(c::MutexClock::now()).code==C::invalid_request,"drain before fence is not a witness");
    Check(owner.FenceAdmission()==S::invalid_request,"fence before close forbidden");
    Check(owner.Close(Id(24)) && owner.Close(Id(25)),"close idempotent");
    Check(owner.Snapshot().descriptor.last_transition==Id(24),"first close transition retained");
    Check(owner.FenceAdmission()==S::ok,"retire with retained holder");
    Check(domain.Collect()==S::ok && domain.Snapshot().retired==1 &&
          domain.Snapshot().reclamation_blocked_objects==1,"real hazard blocks physical reclamation");
    Check(owner.Drain(c::MutexClock::now()).code==C::timed_out,"no false zero drain with holder");
    Check(owner.AcquireOperation(Id(20),7,Hazard(34),other)==S::closed,"fenced operation admission");
    Check(grant.Release(request).code==C::released,"valid release after memory retirement");
    Check(owner.Drain(c::MutexClock::now()+1s).code==C::drained,"drain after real holder release");
    Check(domain.Collect()==S::ok && domain.Snapshot().retired==1,"owner observer itself retains retired storage");
  },{1,1});
  Run([](auto& owner,auto& domain) {
    auto op=Operation(owner,31); c::MutexLatchGrant grant;
    const auto result=op.Acquire(Request(),Hazard(32),grant,{}, {},true);
    Check(result.code==C::memory_failed && result.memory_status==S::exhausted,"real reader exhaustion before grant");
    Check(!grant && !owner.Snapshot().native.held && domain.Snapshot().readers==2,"exhaustion produces no hidden holder");
    std::stop_source stop; stop.request_stop();
    Check(op.Acquire(Request(),Hazard(32),grant,c::MutexClock::now(),{},true).code==C::timed_out,
          "expired acquisition needs no additional reader slot");
    Check(op.Acquire(Request(),Hazard(32),grant,c::MutexClock::now(),stop.get_token(),true).code==C::cancelled,
          "cancellation precedes timeout without reader capacity");
    Check(owner.Close(Id(24)),"close at exhausted memory admission");
    Check(op.Acquire(Request(),Hazard(32),grant,c::MutexClock::now(),stop.get_token(),true).code==C::closed,
          "closed wins without reader capacity");
  },{1,2},2);
}
void RequestValidationAndTerminals() {
  Run([](auto& owner,auto& domain) {
    c::MutexLatchOperation empty; c::MutexLatchGrant grant;
    Check(empty.Acquire(Request(),Hazard(32),grant,{}).code==C::invalid_request,"empty operation refused");
    for (unsigned bit=0;bit<128;++bit) {
      auto primitive=Id(20); primitive[bit/8]^=static_cast<std::uint8_t>(1u<<(bit%8));
      Check(owner.AcquireOperation(primitive,7,Hazard(31),empty)==S::invalid_request,"operation compares every binary primitive bit");
    }
    Check(owner.AcquireOperation(Id(20),8,Hazard(31),empty)==S::invalid_request,"stale generation refused");
    auto op=Operation(owner,31);
    c::MutexLatchOperation moved(std::move(op));
    Check(!op && moved && domain.Snapshot().readers==2,"operation move retains one real guard");
    for (unsigned field=0;field<7;++field) {
      auto request=Request(); auto hazard=Hazard(32);
      if (field==0) request.primitive_id=Id(21);
      if (field==1) ++request.generation;
      if (field==2) request.task_id=Id(98);
      if (field==3) request.request_id={};
      if (field==4) hazard.owner_task=Id(98);
      if (field==5) hazard.hazard_id={};
      if (field==6) hazard.release_required_by=static_cast<m::SafeRetirementBoundary>(255);
      Check(moved.Acquire(request,hazard,grant,{}, {},true).code==C::invalid_request,"invalid acquire request before grant");
      Check(!grant && !owner.Snapshot().native.held && domain.Snapshot().readers==2,"invalid acquire leaves actual memory and ownership unchanged");
    }
    const auto duplicate=moved.Acquire(Request(),Hazard(31),grant,{}, {},true);
    Check(duplicate.code==C::memory_failed && duplicate.memory_status==S::invalid_request,
          "duplicate live hazard refused by actual memory service");
  });
  for (unsigned immediate=0;immediate<2;++immediate)
    for (unsigned signals=0;signals<8;++signals) Run([=](auto& owner,auto& domain) {
      auto op=Operation(owner,31); c::MutexLatchGrant grant;
      std::stop_source stop;
      if (signals&1) Check(owner.Close(Id(24)),"terminal matrix close");
      if (signals&2) stop.request_stop();
      const auto deadline=(signals&4)?c::MutexClock::now():c::MutexClock::now()+1s;
      const auto result=op.Acquire(Request(),Hazard(32),grant,deadline,stop.get_token(),immediate);
      const auto expected=(signals&1)?C::closed:(signals&2)?C::cancelled:(signals&4)?C::timed_out:C::acquired;
      Check(result.code==expected,"retained initial terminal precedence even when idle");
      Check(bool(grant)==(expected==C::acquired),"only acquired result retains a grant");
      Check(domain.Snapshot().readers==(grant?3u:2u),"terminal outcome has exact real retained reader count");
    });
}
void Publication() {
  for (unsigned threads:{1u,2u,8u}) Run([threads](auto& owner,auto&) {
    unsigned payload=0;
    std::vector<std::thread> workers;
    for (unsigned t=0;t<threads;++t) workers.emplace_back([&,t] {
      auto op=Operation(owner,200+t,300+t);
      for (unsigned n=0;n<50;++n) {
        c::MutexLatchGrant grant;
        Check(op.Acquire(Request(1000+t*50+n,300+t),Hazard(400+t,300+t),grant,
                         c::MutexClock::now()+5s).code==C::acquired,"retained real exclusive contention");
        ++payload; // Deliberately non-atomic: publication must come from mutex.
      }
    });
    for (auto& worker:workers) worker.join();
    Check(payload==threads*50,"exclusive release/acquire publishes exact payload");
  });
}
#if defined(SB_MUTEX_NATIVE_FAULT_GATE)
void NativeBoundaries() {
  for (unsigned terminal=0;terminal<3;++terminal) Run([terminal](auto& owner,auto& domain) {
    auto op=Operation(owner,31); c::MutexLatchGrant holder;
    Check(op.Acquire(Request(),Hazard(32),holder,{}, {},true).code==C::acquired,"native boundary holder");
    Park probe; std::stop_source stop;
    std::thread waiter([&] {
      auto waiting=Operation(owner,33,98); c::MutexLatchGrant grant;
      park=&probe;
      const auto result=waiting.Acquire(Request(101,98),Hazard(34,98),grant,
          terminal==2 ? std::optional(c::MutexClock::now()+100ms) : std::nullopt,stop.get_token());
      Check(result.code==(terminal==0?C::closed:terminal==1?C::cancelled:C::timed_out),"actual native terminal selection");
      Check(!grant,"terminal return owns no grant");
    });
    Check(probe.entered.try_acquire_for(5s),"actual retained native waiter parked");
    Check(owner.Snapshot().native.waiters==1 && domain.Snapshot().readers==5,"park retains operation prepared grant and holder guards");
    std::array<scratchbird::core::platform::CheckedFifoMutex::WaiterObservation,1> waits;
    const auto live=owner.SnapshotWaiters(waits);
    Check(live.wait.complete && live.latch.initialized && live.latch.native.waiters==1 &&
          live.wait.owner==Id(99) && waits[0].owner==Id(98) &&
          live.wait.holder==std::this_thread::get_id() && waits[0].thread==waiter.get_id(),
          "retained observation binds actual native owners and FIFO wait to live descriptor");
    Check(live.latch.descriptor.primitive_id==Request().primitive_id &&
          live.latch.descriptor.generation==Request().generation && domain.Snapshot().readers==5,
          "observation preserves binary primitive generation without extra hazard admission");
    if (terminal==0) { Check(owner.Close(Id(24)),"close during park"); Check(owner.FenceAdmission()==S::ok,"fence parked call"); }
    if (terminal==1) stop.request_stop();
    waiter.join();
    Check(owner.Snapshot().native.calls==0 && domain.Snapshot().readers==3,"return joins callback and drops prepared guard");
    Check(holder.Release(Request()).code==C::released,"terminal waiter cannot forgive prior ownership");
  });
  Run([](auto& owner,auto& domain) {
    auto op=Operation(owner,31); c::MutexLatchGrant holder;
    Check(op.Acquire(Request(),Hazard(32),holder,{}, {},true).code==C::acquired,"release fault holder");
    for (unsigned stage:{1u,2u}) {
      fail_lock=stage;
      Check(holder.Release(Request()).code==C::synchronization_failed,"pre-effect native lock failure retained");
      Check(holder && owner.Snapshot().retained_grants==1 && domain.Snapshot().readers==3,"failed release retains original charge and grant");
    }
    std::thread waiter([&] {
      auto waiting=Operation(owner,33,98); c::MutexLatchGrant rejected;
      fail_wait=true;
      Check(waiting.Acquire(Request(101,98),Hazard(34,98),rejected,c::MutexClock::now()+5s).code==C::synchronization_failed,
            "native parking error cannot become grant");
    }); waiter.join();
    Check(owner.Snapshot().native.waiters==0 && domain.Snapshot().readers==3,"failed wait unlinks and drops real prepared guard");
    Check(owner.Close(Id(24)) && owner.FenceAdmission()==S::ok,"fault drain fence");
    fail_wait=true;
    Check(owner.Drain(c::MutexClock::now()+5s).code==C::synchronization_failed,"native drain failure is not a receipt");
    std::stop_source stopped; stopped.request_stop();
    Check(owner.Drain(c::MutexClock::now()+5s,stopped.get_token()).code==C::cancelled,"cancelled drain retains storage");
    Check(holder.Release(Request()).code==C::released,"retry actual release after failure and fence");
  });
  Run([](auto& owner,auto& domain) {
    auto op=Operation(owner,31); c::MutexLatchGrant grant;
    for (unsigned stage:{1u,2u,3u,4u}) {
      fail_lock=stage;
      Check(op.Acquire(Request(),Hazard(32),grant,{}, {},true).code==C::synchronization_failed,
            "pre-grant native synchronization error returns no grant");
      Check(!grant && domain.Snapshot().readers==2 && !owner.Snapshot().native.held,
            "failed native admission leaves no hidden holder or reader");
    }
  });
  Run([](auto& owner,auto& domain) {
    auto op=Operation(owner,31); c::MutexLatchGrant holder;
    Check(op.Acquire(Request(),Hazard(32),holder,{}, {},true).code==C::acquired,"blocking drain live holder");
    op.Reset();
    Check(owner.Close(Id(24)) && owner.FenceAdmission()==S::ok,"blocking drain fenced");
    Park probe;
    std::thread drain([&] {
      park=&probe;
      Check(owner.Drain(c::MutexClock::now()+5s).code==C::drained,"actual drain park wakes after release");
    });
    Check(probe.entered.try_acquire_for(5s),"drain enters actual native park");
    Check(domain.Collect()==S::ok && domain.Snapshot().retired==1,"parked drain cannot free holder");
    Check(holder.Release(Request()).code==C::released,"real release wakes drain");
    drain.join();
  });
  Run([](auto& owner,auto& domain) {
    auto op=Operation(owner,31); c::MutexLatchGrant holder;
    Check(op.Acquire(Request(),Hazard(32),holder,{}, {},true).code==C::acquired,"late delivery original holder");
    Park probe; probe.pause_delivery=true; std::stop_source stop;
    std::thread waiter([&] {
      auto waiting=Operation(owner,33,98); c::MutexLatchGrant grant;
      park=&probe;
      const auto result=waiting.Acquire(Request(101,98),Hazard(34,98),grant,{},stop.get_token());
      Check(result.code==C::acquired && grant,"late close cannot rewrite committed retained acquisition");
      Check(grant.Release(Request(101,98)).code==C::released,"late delivered grant has real release path after fence");
    });
    Check(probe.entered.try_acquire_for(5s),"late delivery waiter actually parked");
    Check(holder.Release(Request()).code==C::released,"release to pending retained task");
    op.Reset();
    Check(probe.committed.try_acquire_for(5s),"pause after native grant before callback join and retained delivery");
    Check(owner.Close(Id(24)) && owner.FenceAdmission()==S::ok,"close and fence delivery gap");
    stop.request_stop();
    const auto snapshot=owner.Snapshot();
    Check(snapshot.native.held && snapshot.native.calls==1 && snapshot.retained_grants==0 &&
          snapshot.operation_references==1,"whole operation covers grant delivery and callback join gap");
    const auto live=owner.SnapshotWaiters({});
    Check(live.wait.complete && live.wait.owner==Id(98) && live.wait.holder==waiter.get_id() &&
          live.wait.state.waiters==0 && live.wait.state.calls==1 && live.latch.retained_grants==0,
          "observation distinguishes committed native ownership from retained grant delivery");
    Check(owner.Drain(c::MutexClock::now()).code==C::timed_out,"delivery gap cannot produce false zero drain");
    Check(domain.Collect()==S::ok && domain.Snapshot().readers==3 && domain.Snapshot().retired==1,
          "real owner operation and prepared grant guards retain late result storage");
    probe.resume.release(); waiter.join();
    Check(owner.Snapshot().retained_grants==0 && domain.Snapshot().readers==1,"actual late release drops exact operation and grant guards");
  });
}
#endif
} // namespace
int main(int argc,char** argv) {
  if (argc==2 && std::string_view(argv[1])=="--unsafe-owner") {
    std::set_terminate([] { std::_Exit(73); });
    Fixture f; m::MemorySafeRetirement domain(*f.resource,Id(10),1,2);
    Check(domain.Initialize()==S::ok,"child real domain");
    { c::MutexLatchOwner owner(domain);
      Check(owner.Initialize(Descriptor(),{1,1},Hazard(30))==S::ok,"child real owner"); }
    return 9;
  }
  Descriptors(); IdentityAndModes(); LifetimeAndExhaustion(); RequestValidationAndTerminals(); Publication();
#if defined(SB_MUTEX_NATIVE_FAULT_GATE)
  NativeBoundaries();
#endif
#if defined(__unix__)
  const auto child=fork(); Check(child>=0,"fresh process lifetime probe");
  if (child==0) { execl(argv[0],argv[0],"--unsafe-owner",nullptr); std::_Exit(127); }
  int status=0; Check(waitpid(child,&status,0)==child,"join unsafe owner child");
  Check(WIFEXITED(status) && WEXITSTATUS(status)==73,"unsafe owner fails before releasing reachable storage");
#endif
  std::cout << "PASS " << checks << " retained mutex checks; full runtime authority and diagnostics remain separate\n";
}
