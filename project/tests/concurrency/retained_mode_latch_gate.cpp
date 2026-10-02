// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "retained_mode_latch.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <semaphore>
#include <vector>

#if defined(SB_MODE_NATIVE_FAULT_GATE)
struct Park {
  std::binary_semaphore entered{0}, committed{0}, resume{0};
  bool observed = false, pause_delivery = false;
};
thread_local Park* park = nullptr;
thread_local pthread_mutex_t* pause_unlock = nullptr;
thread_local bool fail_wait = false;
struct PreparationGap { std::binary_semaphore entered{0}, resume{0}; };
thread_local PreparationGap* preparation_gap = nullptr;
namespace mode_memory = scratchbird::core::memory;
extern "C" mode_memory::SafeRetirementStatus RealModeProtect(mode_memory::MemorySafeRetirement*,
    const mode_memory::SafeRetirementHandle&, mode_memory::SafeRetirementHazard, mode_memory::SafeRetirementGuard&)
    asm("__real__ZN11scratchbird4core6memory20MemorySafeRetirement7ProtectERKNS1_20SafeRetirementHandleENS1_20SafeRetirementHazardERNS1_19SafeRetirementGuardE");
extern "C" mode_memory::SafeRetirementStatus WrapModeProtect(mode_memory::MemorySafeRetirement*,
    const mode_memory::SafeRetirementHandle&, mode_memory::SafeRetirementHazard, mode_memory::SafeRetirementGuard&)
    asm("__wrap__ZN11scratchbird4core6memory20MemorySafeRetirement7ProtectERKNS1_20SafeRetirementHandleENS1_20SafeRetirementHazardERNS1_19SafeRetirementGuardE");
extern "C" mode_memory::SafeRetirementStatus WrapModeProtect(mode_memory::MemorySafeRetirement* domain,
    const mode_memory::SafeRetirementHandle& handle, mode_memory::SafeRetirementHazard hazard,
    mode_memory::SafeRetirementGuard& guard) {
  if (preparation_gap && hazard.hazard_id[15]==112) {
    auto* gap=std::exchange(preparation_gap,nullptr);
    gap->entered.release(); gap->resume.acquire();
  }
  return RealModeProtect(domain,handle,hazard,guard);
}
extern "C" int __real_pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
extern "C" int __real_pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const timespec*);
extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t*);
void EnterPark() {
  if (park && !park->observed) { park->observed=true; park->entered.release(); }
}
extern "C" int __wrap_pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
  if (fail_wait) return EINVAL;
  EnterPark();
  const auto result=__real_pthread_cond_wait(c,m);
  if (!result && park && park->pause_delivery) pause_unlock=m;
  return result;
}
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const timespec* t) {
  if (fail_wait) return EINVAL;
  EnterPark();
  const auto result=__real_pthread_cond_timedwait(c,m,t);
  if (!result && park && park->pause_delivery) pause_unlock=m;
  return result;
}
extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t* m) {
  const bool pause=pause_unlock==m;
  if (pause) pause_unlock=nullptr;
  const auto result=__real_pthread_mutex_unlock(m);
  if (pause) { park->committed.release(); park->resume.acquire(); }
  return result;
}
#endif

namespace {
namespace m = scratchbird::core::memory;
namespace c = scratchbird::core::concurrency;
using C = c::ModeLatchCode;
using S = m::SafeRetirementStatus;
using M = c::ModeLatchNative::Mode;
using namespace std::chrono_literals;
std::atomic<unsigned> checks{0};
void Check(bool good, const char* message) {
  ++checks;
  if (!good) { std::fprintf(stderr,"FAIL %s\n",message); std::abort(); }
}
m::MemoryBinaryUuid Id(unsigned n) {
  m::MemoryBinaryUuid id{}; id[6]=0x70; id[8]=0x80;
  id[14]=static_cast<unsigned char>(n>>8); id[15]=static_cast<unsigned char>(n); return id;
}
m::SafeRetirementHazard Hazard(unsigned n, unsigned task=99) { return {Id(n),Id(task)}; }
c::ModeLatchIdentity Identity() { return {Id(20),7}; }
c::ModeLatchRequest Request(unsigned n, M mode=M::shared_read, unsigned task=99) {
  return {Identity(),Id(task),Id(n),mode,0};
}
c::ModeLatchGrantMemory Memory(unsigned n, unsigned task=99) {
  return {Id(n),Hazard(n+1,task),Hazard(n+2,task)};
}
struct Fixture {
  static m::AllocationPolicy Policy() {
    auto p=m::DefaultLocalEngineMemoryPolicy();
    p.hard_limit_bytes=p.per_context_limit_bytes=131072; return p;
  }
  m::MemoryManager manager{Policy()};
  m::HierarchicalMemoryBudgetLedger ledger{3,5};
  std::unique_ptr<m::ReservationBackedMemoryResource> resource;
  Fixture() {
    m::ReservationBackedMemoryResourceRequest request;
    request.memory_manager=&manager; request.reservation_ledger=&ledger;
    request.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
    request.requested_bytes=65536; request.category=m::MemoryCategory::core_runtime;
    request.route_label="runtime.mode_latch.conformance"; request.purpose="retained multi-holder nodes";
    request.binary_operation_uuid=Id(1);
    request.binary_ownership[m::MemoryBinaryScopeKind::context]=Id(2);
    request.binary_ownership[m::MemoryBinaryScopeKind::owner]=Id(3);
    request.binary_ownership[m::MemoryBinaryScopeKind::database]=Id(4);
    request.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(5)},
                         {m::HierarchicalMemoryScopeKind::database,{},Id(4)}};
    request.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    request.provenance.source_label="multi-holder lifetime fixture";
    for (const auto& scope:request.scope_chain) {
      m::HierarchicalMemoryBudget budget;
      budget.scope=scope; budget.hard_limit_bytes=request.requested_bytes; budget.provenance=request.provenance;
      Check(ledger.SetBudget(budget).ok(),"parent budget");
    }
    auto acquired=m::AcquireReservationBackedMemoryResource(std::move(request));
    Check(acquired.ok(),"real governed resource"); resource=std::move(acquired.resource);
  }
  void Empty() {
    Check(manager.Snapshot().current_bytes==0,"real payload and metadata freed");
    Check(resource->ReleaseNoAlloc().ok(),"reservation released");
    Check(ledger.Snapshot().current_bytes==0 && manager.Snapshot().reserved_capacity_bytes==0,
          "all actual parent charges released");
  }
};
c::ModeLatchOperation Operation(c::ModeLatchOwner& owner, unsigned n, unsigned task=99) {
  c::ModeLatchOperation operation;
  Check(owner.AcquireOperation(Identity(),Hazard(n,task),operation)==S::ok,"retained operation admitted");
  return operation;
}
template<class F> void Run(F function, unsigned objects=16, unsigned readers=32) {
  Fixture fixture;
  {
    m::MemorySafeRetirement domain(*fixture.resource,Id(10),objects,readers);
    Check(domain.Initialize()==S::ok,"domain initialized");
    const auto metadata=fixture.manager.Snapshot().current_bytes;
    {
      c::ModeLatchOwner owner(domain);
      Check(owner.Initialize(Identity(),{4,4,8},Hazard(30))==S::ok,"retained native state published");
      Check(fixture.manager.Snapshot().current_bytes>metadata,"latch physically charged");
      function(owner,domain,fixture);
      Check(owner.Close(),"close");
      Check(owner.FenceAdmission()==S::ok,"retire actual state");
      Check(owner.Drain(c::ModeLatchClock::now()+2s).code==C::drained,"actual grants and operation drain");
    }
    Check(domain.Collect()==S::ok,"actual collection");
    Check(fixture.manager.Snapshot().current_bytes==metadata,"no native state or grant record remains");
  }
  fixture.Empty();
}
void Matrix() {
  constexpr const char* matrix[]{"YYYNNNNYNNY","YYYNNNNYNNY","YYYNNNNNNNN","NNNNNNNNNNN",
    "NNNNNNNNNNN","NNNNNYNNNNN","NNNNNNNNNNN","YYNNNNNYNNY","NNNNNNNNNNN","NNNNNNNNNNN","YYNNNNNYNNY"};
  for (unsigned a=0;a<11;++a) for (unsigned b=0;b<11;++b) Run([&](auto& owner,auto& domain,auto&) {
    auto operation=Operation(owner,40);
    c::ModeLatchGrant first;
    auto request=Request(50,static_cast<M>(a+1));
    Check(operation.Acquire(request,Memory(100),first,{}, {},true).code==C::acquired,"first real holder");
    std::thread second([&] {
      auto op=Operation(owner,41,98);
      c::ModeLatchGrant grant;
      auto r=Request(51,static_cast<M>(b+1),98);
      const auto code=op.Acquire(r,Memory(110,98),grant,{}, {},true).code;
      Check(code==(matrix[b][a]=='Y' ? C::acquired : C::busy),"independent held-mode compatibility");
      if (grant) Check(grant.Release(r).code==C::released,"second exact release");
    });
    second.join();
    Check(first.Release(request).code==C::released,"first exact release");
    Check(domain.Collect()==S::ok,"unused and released grant records collect");
  });
}
void Ownership() {
  Run([](auto& owner,auto& domain,auto& fixture) {
    auto operation=Operation(owner,40);
    auto request=Request(50);
    c::ModeLatchGrant grant;
    const auto baseline=fixture.manager.Snapshot().current_bytes;
    Check(operation.Acquire(request,Memory(100),grant,{}).code==C::acquired,"real holder granted");
    Check(fixture.manager.Snapshot().current_bytes>baseline,"grant record physically charged");
    auto changed=request; changed.request=Id(51);
    Check(grant.Release(changed).code==C::wrong_owner,"wrong request retains record");
    changed=request; changed.task=Id(98);
    Check(grant.Release(changed).code==C::wrong_owner,"wrong binary task retains record");
    changed=request; ++changed.identity.generation;
    Check(grant.Release(changed).code==C::wrong_owner,"wrong generation retains record");
    changed=request; changed.mode=M::exclusive_write;
    Check(grant.Release(changed).code==C::wrong_owner,"conversion is not release");
    std::thread wrong([&] { Check(grant.Release(request).code==C::wrong_owner,"wrong native thread"); });
    wrong.join();
    c::ModeLatchGrant moved(std::move(grant)), assigned;
    assigned=std::move(moved);
    Check(!grant && !moved && assigned,"wrapper moves preserve nonmoving record");
    operation.Reset();
    Check(domain.Snapshot().readers==3,"grant retains both actual state and record guards independently of operation");
    Check(owner.Close() && owner.FenceAdmission()==S::ok,"closed fenced while holder remains");
    Check(owner.Drain(c::ModeLatchClock::now()).code==C::timed_out,"holder blocks drain without operation");
    Check(domain.Collect()==S::ok && domain.Snapshot().reclamation_blocked_objects==2,
          "actual state and record both protected against collection");
    domain.Close();
    Check(assigned.Release(request).code==C::released,"closed domain still allows exact native release");
    Check(assigned.Release(request).code==C::no_grant,"no duplicate release");
    Check(domain.Collect()==S::ok,"released private record collects");
    Check(fixture.manager.Snapshot().current_bytes==baseline,"only owner retains latch payload");
  });
}
void Exhaustion() {
  for (unsigned readers : {2U,3U,32U}) Run([&](auto& owner,auto& domain,auto&) {
    auto operation=Operation(owner,40);
    c::ModeLatchGrant grant;
    auto result=operation.Acquire(Request(50),Memory(100),grant,{});
    Check(result.code==C::memory_failed && result.memory_status==S::exhausted,"real object or hazard bound refuses");
    Check(!grant && owner.Snapshot().native.holders==0,"memory failure cannot leave native holder");
    Check(domain.Collect()==S::ok && domain.Snapshot().retired==0,"failed private preparation collects");
    Check(domain.Snapshot().readers==2,"only owner and operation guards remain");
  },readers<=3 ? 4 : 1,readers);
  Run([](auto& owner,auto& domain,auto&) {
    auto operation=Operation(owner,40);
    c::ModeLatchGrant grant;
    auto bad=Request(50); bad.identity.generation++;
    Check(operation.Acquire(bad,Memory(100),grant,{}).code==C::invalid,"stale generation");
    bad=Request(50); bad.task=Id(98);
    Check(operation.Acquire(bad,Memory(100),grant,{}).code==C::invalid,"operation task binding");
    auto duplicate=Memory(100); duplicate.record_hazard.hazard_id=duplicate.latch_hazard.hazard_id;
    Check(operation.Acquire(Request(50),duplicate,grant,{}).code==C::invalid,"distinct hazards required");
    auto collision=Memory(100); collision.record_hazard.hazard_id=Id(40);
    Check(operation.Acquire(Request(50),collision,grant,{}).code==C::memory_failed,"actual existing hazard collision");
    Check(domain.Collect()==S::ok && domain.Snapshot().readers==2,"partial preparation unwinds");
    Check(owner.Close(),"close for preflight");
    Check(operation.Acquire(Request(50),Memory(100),grant,{}).code==C::closed,"closed before memory preparation");
    Check(domain.Snapshot().retired==0,"closed admission allocated no record");
  });
}
void MultipleRetainedHolders() {
  Run([](auto& owner,auto& domain,auto&) {
    std::latch granted(3), release(1);
    std::vector<std::thread> threads;
    for (unsigned i=0;i<3;++i) threads.emplace_back([&,i] {
      auto operation=Operation(owner,40+i,90+i);
      c::ModeLatchGrant grant;
      auto request=Request(50+i,M::shared_read,90+i);
      Check(operation.Acquire(request,Memory(100+10*i,90+i),grant,{}).code==C::acquired,"compatible real held grant");
      operation.Reset();
      c::ModeLatchGrant moved(std::move(grant));
      granted.count_down(); release.wait();
      Check(moved.Release(request).code==C::released,"each committed holder release after close");
    });
    granted.wait();
    Check(owner.Snapshot().grants==3 && owner.Snapshot().operations==0,"three independent retained owners");
    Check(domain.Snapshot().readers==7,"each holder has independent latch and record guards");
    Check(owner.Close() && owner.FenceAdmission()==S::ok,"close does not revoke batch");
    Check(domain.Collect()==S::ok && domain.Snapshot().reclamation_blocked_objects==4,"all three records and latch retained");
    Check(owner.Drain(c::ModeLatchClock::now()).code==C::timed_out,"native batch blocks retirement drain");
    release.count_down();
    for (auto& thread:threads) thread.join();
  });
}
void RetainedTerminals() {
  for (unsigned mode=1;mode<=11;++mode) for (unsigned state=0;state<8;++state)
    for (bool immediate : {false,true}) Run([&](auto& owner,auto& domain,auto&) {
      auto op=Operation(owner,40);
      c::ModeLatchGrant grant;
      auto r=Request(50,static_cast<M>(mode));
      std::stop_source stop;
      if (state&1) Check(owner.Close(),"terminal close");
      if (state&2) stop.request_stop();
      const auto deadline=state&4 ? c::ModeLatchClock::now() : c::ModeLatchClock::now()+2s;
      const auto result=op.Acquire(r,Memory(100),grant,deadline,stop.get_token(),immediate);
      Check(result.code==(state&1 ? C::closed : state&2 ? C::cancelled : state&4 ? C::timed_out : C::acquired),
            "retained acquisition terminal precedence before physical preparation");
      if (state) Check(domain.Snapshot().retired==0 && domain.Snapshot().readers==2,
                       "preexisting terminal allocated no grant or extra hazard");
      if (grant) Check(grant.Release(r).code==C::released,"successful terminal selection release");
    });
}
#if defined(SB_MODE_NATIVE_FAULT_GATE)
void ParkedLifetime() {
  for (unsigned ending=0;ending<4;++ending) Run([&](auto& owner,auto& domain,auto&) {
    auto operation=Operation(owner,40);
    auto request=Request(50,M::exclusive_write);
    c::ModeLatchGrant held;
    Check(operation.Acquire(request,Memory(100),held,{}).code==C::acquired,"held before native park");
    operation.Reset();
    Park p; p.pause_delivery=ending==3;
    std::stop_source stop;
    std::latch delivered(1), release(1);
    std::thread waiter([&] {
      auto op=Operation(owner,41,98);
      c::ModeLatchGrant grant;
      auto r=Request(51,M::shared_read,98);
      park=&p;
      const auto result=op.Acquire(r,Memory(110,98),grant,
          c::ModeLatchClock::now()+(ending==2 ? 50ms : 3s),stop.get_token());
      park=nullptr;
      Check(result.code==(ending==0 ? C::closed : ending==1 ? C::cancelled :
                         ending==2 ? C::timed_out : C::acquired),"retained native terminal result");
      op.Reset();
      delivered.count_down(); release.wait();
      if (grant) Check(grant.Release(r).code==C::released,"release grant delivered after close");
    });
    Check(p.entered.try_acquire_for(2s),"native wait entered");
    Check(domain.Collect()==S::ok && domain.Snapshot().reclamation_blocked_objects==2,
          "holder and wait-preparation records remain protected");
    if (ending==0) Check(owner.Close(),"close actual wait");
    if (ending==1) stop.request_stop();
    if (ending==3) {
      Check(held.Release(request).code==C::released,"wake successor");
      Check(p.committed.try_acquire_for(2s),"paused after native commit before delivery");
      const auto snapshot=owner.Snapshot();
      Check(snapshot.native.holders==1 && snapshot.native.calls==1 && snapshot.grants==0,
            "native commitment precedes retained wrapper delivery");
      Check(owner.Close() && owner.FenceAdmission()==S::ok,"close/fence during callback gap");
      Check(owner.Drain(c::ModeLatchClock::now()).code==C::timed_out,"whole operation retains commit gap");
      Check(domain.Collect()==S::ok && domain.Snapshot().reclamation_blocked_objects==2,
            "committed record and latch survive collection in delivery gap");
      p.resume.release();
    }
    delivered.wait();
    if (ending!=3) Check(held.Release(request).code==C::released,"existing holder not revoked");
    release.count_down(); waiter.join();
  });
  Run([](auto& owner,auto& domain,auto&) {
    auto operation=Operation(owner,40);
    c::ModeLatchGrant held;
    auto r=Request(50,M::exclusive_write);
    Check(operation.Acquire(r,Memory(100),held,{}).code==C::acquired,"held for fault");
    std::thread waiter([&] {
      auto op=Operation(owner,41,98); c::ModeLatchGrant grant;
      fail_wait=true;
      Check(op.Acquire(Request(51,M::shared_read,98),Memory(110,98),grant,{}).code==C::synchronization_failed,
            "native wait failure unwinds actual memory preparation");
      fail_wait=false;
    });
    waiter.join();
    Check(domain.Collect()==S::ok && domain.Snapshot().retired==1,"only original grant record retained");
    Check(owner.Snapshot().native.holders==1 && owner.Snapshot().native.waiters==0,"fault preserves owner and unlinks wait");
    Check(held.Release(r).code==C::released,"release after failed waiter");
  });
}
void DomainClosesBeforeProtection() {
  Run([](auto& owner,auto& domain,auto&) {
    PreparationGap gap;
    std::thread contender([&] {
      auto operation=Operation(owner,41,98);
      c::ModeLatchGrant grant;
      preparation_gap=&gap;
      const auto result=operation.Acquire(Request(51,M::shared_read,98),Memory(110,98),grant,{});
      Check(result.code==C::memory_failed && result.memory_status==S::closed,
            "domain close before record protection is a clean refusal");
      Check(!grant,"collected preparation cannot become a grant");
    });
    Check(gap.entered.try_acquire_for(2s),"record published but not yet protected");
    domain.Close();
    Check(domain.Collect()==S::ok && domain.Snapshot().retired==1,
          "real domain reclaims unprotected record while operation retains latch");
    gap.resume.release(); contender.join();
    Check(owner.Snapshot().grants==0 && owner.Snapshot().native.holders==0,"no effect after lost preparation");
  });
}
#endif
} // namespace
int main() {
  Matrix(); Ownership(); Exhaustion(); MultipleRetainedHolders(); RetainedTerminals();
#if defined(SB_MODE_NATIVE_FAULT_GATE)
  ParkedLifetime();
  DomainClosesBeforeProtection();
#endif
  std::printf("PASS retained mode latch: %u checks\n",checks.load());
}
