// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "retained_mode_latch.hpp"
#include "retained_mutex_latch.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
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
struct PreparationReentry {
  scratchbird::core::concurrency::ModeLatchOperation* operation;
  scratchbird::core::concurrency::ModeLatchGrant* grant;
  scratchbird::core::concurrency::ModeLatchRequest request;
  scratchbird::core::concurrency::ModeLatchGrantMemory memory;
  bool reached = false;
};
thread_local PreparationReentry* preparation_reentry = nullptr;
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
  if (preparation_reentry && hazard.hazard_id[15]==112) {
    auto* reentry=std::exchange(preparation_reentry,nullptr);
    reentry->reached=true;
    if (reentry->operation->Acquire(reentry->request,reentry->memory,*reentry->grant,{}).code !=
        scratchbird::core::concurrency::ModeLatchCode::acquired) std::abort();
  }
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
c::ModeLatchIdentity Identity() { return {Id(20),7,c::LatchClass::page}; }
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
      auto op=Operation(owner,41,99);
      c::ModeLatchGrant grant;
      auto r=Request(51,static_cast<M>(b+1),99);
      const auto code=op.Acquire(r,Memory(110,99),grant,{}, {},true).code;
      Check(code==(matrix[b][a]=='Y' ? C::acquired : C::busy),"independent held-mode compatibility");
      if (grant) Check(grant.Release(r).code==C::released,"second exact release");
    });
    second.join();
    Check(first.Release(request).code==C::released,"first exact release");
    Check(domain.Collect()==S::ok,"unused and released grant records collect");
  });
}
// Exercise both mechanisms through real owners, operations, grants and memory;
// the expected order comes from canonical names, not numeric enum comparisons.
constexpr c::LatchClass classes[]{c::LatchClass::engine_lifecycle,c::LatchClass::database_state,
  c::LatchClass::transaction_inventory,c::LatchClass::catalog_metadata,
  c::LatchClass::relation_index_descriptor,c::LatchClass::filespace_descriptor,
  c::LatchClass::allocation_map,c::LatchClass::page_cache_bucket,c::LatchClass::page,
  c::LatchClass::record_lineage,c::LatchClass::archive_descriptor,
  c::LatchClass::temporary_storage,c::LatchClass::metrics_evidence};
struct MixedLatch {
  bool mutex;
  c::ModeLatchOwner modes;
  c::MutexLatchOwner exclusive;
  c::ModeLatchOperation mode_op;
  c::MutexLatchOperation mutex_op;
  c::ModeLatchRequest mode_request;
  c::MutexLatchRequest mutex_request;
  c::ModeLatchGrant mode_grant;
  c::MutexLatchGrant mutex_grant;
  MixedLatch(m::MemorySafeRetirement& domain, bool is_mutex, unsigned slot, c::LatchClass rank, M mode)
      : mutex(is_mutex), modes(domain), exclusive(domain),
        mode_request{{Id(20+slot),7,rank},Id(99),Id(50+slot),mode,0},
        mutex_request{Id(20+slot),7,Id(99),Id(50+slot),c::MutexLatchMode::exclusive_write} {
    if (mutex) {
      c::MutexLatchDescriptor d;
      d.primitive_id=Id(20+slot); d.owner_uuid=Id(99); d.last_transition=Id(60+slot);
      d.generation=7; d.latch_class=rank;
      Check(exclusive.Initialize(d,{4,8},Hazard(30+slot))==S::ok,"mixed mutex published");
      Check(exclusive.AcquireOperation(d.primitive_id,7,Hazard(40+slot),mutex_op)==S::ok,"mixed mutex operation");
    } else {
      Check(modes.Initialize(mode_request.identity,{4,4,8},Hazard(30+slot))==S::ok,"mixed mode published");
      Check(modes.AcquireOperation(mode_request.identity,Hazard(40+slot),mode_op)==S::ok,"mixed mode operation");
    }
  }
  ~MixedLatch() {
    Release(); mode_op.Reset(); mutex_op.Reset();
    if (mutex) {
      Check(exclusive.Close(Id(80)),"mixed mutex close");
      Check(exclusive.FenceAdmission()==S::ok,"mixed mutex fence");
      Check(exclusive.Drain(c::MutexClock::now()+2s).code==c::MutexLatchCode::drained,"mixed mutex drain");
    } else {
      Check(modes.Close(),"mixed mode close");
      Check(modes.FenceAdmission()==S::ok,"mixed mode fence");
      Check(modes.Drain(c::ModeLatchClock::now()+2s).code==C::drained,"mixed mode drain");
    }
  }
  c::ModeLatchResult Acquire(unsigned storage, bool immediate=true) {
    if (!mutex) return mode_op.Acquire(mode_request,Memory(storage),mode_grant,{}, {},immediate);
    const auto r=mutex_op.Acquire(mutex_request,Hazard(storage),mutex_grant,{}, {},immediate);
    if (r.code==c::MutexLatchCode::acquired) return {C::acquired,r.memory_status,r.order_conflict};
    if (r.code==c::MutexLatchCode::order_violation) return {C::order_violation,r.memory_status,r.order_conflict};
    if (r.code==c::MutexLatchCode::closed) return {C::closed};
    if (r.code==c::MutexLatchCode::wrong_owner) return {C::wrong_owner};
    return {C::invalid};
  }
  void Release() {
    if (mode_grant) Check(mode_grant.Release(mode_request).code==C::released,"mixed mode exact release");
    if (mutex_grant) Check(mutex_grant.Release(mutex_request).code==c::MutexLatchCode::released,"mixed mutex exact release");
  }
};
void SharedOrderMatrix() {
  // mode->mode, mode->mutex and mutex->mode, including every native mode.
  for (unsigned kinds=0;kinds<3;++kinds) for (unsigned mode=1;mode<=11;++mode)
    for (unsigned held=0;held<13;++held) for (unsigned requested=0;requested<13;++requested) {
      Fixture fixture;
      {
        m::MemorySafeRetirement domain(*fixture.resource,Id(10),8,16);
        Check(domain.Initialize()==S::ok,"mixed order domain");
        {
          MixedLatch first(domain,kinds==2,0,classes[held],static_cast<M>(mode));
          MixedLatch second(domain,kinds==1,1,classes[requested],static_cast<M>(mode));
          Check(first.Acquire(100).code==C::acquired,"mixed prior grant");
          const auto before=fixture.manager.Snapshot().current_bytes;
          const auto readers=domain.Snapshot().readers;
          const auto r=second.Acquire(110,(held+requested+mode)%2==0);
          Check(r.code==(requested<held ? C::order_violation : C::acquired),
                "all named class pairs across both mechanisms obey shared order");
          if (requested<held) {
            Check(r.order_conflict && r.order_conflict->held_primitive==Id(20) &&
                  r.order_conflict->held_generation==7 && r.order_conflict->held_class==classes[held],
                  "mixed refusal names actual held binary identity generation and class");
            Check(before==fixture.manager.Snapshot().current_bytes && readers==domain.Snapshot().readers,
                  "mixed refusal before any extra physical allocation or guard");
            Check(!second.mode_grant && !second.mutex_grant,"mixed refusal has no ownership effect");
          }
          second.Release(); first.Release();
          Check(domain.Collect()==S::ok,"mixed unused records collected");
          Check(second.Acquire(120).code==C::acquired,"shared order obstruction removed by actual release");
        }
        Check(domain.Collect()==S::ok,"mixed states collected");
      }
      fixture.Empty();
    }
}
void SharedOrderLifetime() {
  Fixture fixture;
  {
    m::MemorySafeRetirement domain(*fixture.resource,Id(10),16,32);
    Check(domain.Initialize()==S::ok,"mixed lifetime domain");
    {
      MixedLatch low(domain,false,0,classes[0],M::shared_read);
      MixedLatch middle(domain,true,1,classes[4],M::exclusive_write);
      MixedLatch high(domain,false,2,classes[12],M::verification);
      MixedLatch target(domain,true,3,classes[8],M::exclusive_write);
      Check(low.Acquire(100).code==C::acquired && middle.Acquire(110).code==C::acquired &&
            high.Acquire(120).code==C::acquired,"three interleaved real held grants");
      auto moved=std::move(high.mode_grant);
      middle.Release();
      Check(target.Acquire(130).code==C::order_violation,"mixed non-LIFO removal and move preserve highest order");
      auto wrong=high.mode_request; wrong.request=Id(200);
      Check(moved.Release(wrong).code==C::wrong_owner && target.Acquire(140).code==C::order_violation,
            "mixed wrong request preserves order obstruction");
      std::thread other([&] {
        Check(moved.Release(high.mode_request).code==C::wrong_owner,"mixed wrong thread preserves grant");
        Check(target.Acquire(150).code==C::acquired,"same task on another native thread has independent order");
        target.Release();
      });
      other.join();
      Check(high.modes.Close() && high.modes.FenceAdmission()==S::ok,"held high closed and retired");
      high.mode_op.Reset();
      Check(domain.Collect()==S::ok && target.Acquire(160).code==C::order_violation,
            "closed retired state and dropped operation retain real held order node");
      low.mode_grant=std::move(moved);
      Check(target.Acquire(170).code==C::order_violation,"move assignment releases lower but adopts higher obstruction");
      Check(low.mode_grant.Release(high.mode_request).code==C::released,"moved governed record exact release");
      Check(target.Acquire(180).code==C::acquired,"last higher release clears shared order");
    }
    Check(domain.Collect()==S::ok,"mixed lifetime objects collected");
  }
  fixture.Empty();
}
void ClassBinding() {
  Fixture fixture;
  {
    m::MemorySafeRetirement domain(*fixture.resource,Id(10),8,16);
    Check(domain.Initialize()==S::ok,"class binding domain");
    const auto baseline=fixture.manager.Snapshot().current_bytes;
    for (const auto rank:{c::LatchClass::unspecified,static_cast<c::LatchClass>(255)}) {
      c::ModeLatchOwner owner(domain);
      auto identity=Identity(); identity.latch_class=rank;
      Check(owner.Initialize(identity,{4,4,8},Hazard(30))==S::invalid_request,
            "missing or invalid class cannot publish mode latch");
      Check(fixture.manager.Snapshot().current_bytes==baseline,"invalid class has no physical effect");
    }
    {
      MixedLatch latch(domain,false,0,classes[8],M::shared_read);
      auto request=latch.mode_request; request.identity.latch_class=classes[0];
      Check(latch.mode_op.Acquire(request,Memory(100),latch.mode_grant,{}).code==C::invalid,
            "request cannot reclassify existing owning latch");
    }
    Check(domain.Collect()==S::ok,"class binding cleanup");
  }
  fixture.Empty();
}
void IntentUpgrade() {
  for (unsigned mode=1;mode<=11;++mode) if (static_cast<M>(mode)!=M::intent_write)
    Run([&](auto& owner,auto&,auto&) {
      auto op=Operation(owner,40); c::ModeLatchGrant grant;
      const auto original=Request(50,static_cast<M>(mode));
      Check(op.Acquire(original,Memory(100),grant,{}).code==C::acquired,"other-mode original");
      const auto result=op.UpgradeIntent(grant,original,0,{});
      Check(result.result.code==C::invalid && !result.original_released && grant.request()==original,
            "intent conversion cannot release any other original mode");
    });
  for (unsigned flags=0;flags<8;++flags) Run([&](auto& owner,auto&,auto& fixture) {
    auto op=Operation(owner,40);
    auto original=Request(50,M::intent_write);
    c::ModeLatchGrant grant;
    Check(op.Acquire(original,Memory(100),grant,{}).code==C::acquired,"intent conversion original grant");
    const auto before=fixture.manager.Snapshot().current_bytes;
    std::stop_source stop;
    if (flags&1) Check(owner.Close(),"intent pre-release close");
    if (flags&2) stop.request_stop();
    const auto deadline=flags&4?std::optional(c::ModeLatchClock::now()-1s):std::nullopt;
    const auto r=op.UpgradeIntent(grant,original,0,deadline,stop.get_token());
    const auto expected=flags&1?C::closed:flags&2?C::cancelled:flags&4?C::timed_out:C::acquired;
    Check(r.result.code==expected && r.original_released==(flags==0),"intent release terminal precedence and ownership outcome");
    Check(fixture.manager.Snapshot().current_bytes==before,"conversion reuses actual governed grant backing");
    Check(grant.request().mode==(flags?M::intent_write:M::upgrade),"returned mode distinguishes upgrade from exclusive authority");
    if (!flags) Check(grant.Release(original).code==C::wrong_owner,"old intent request cannot release upgraded binding");
    Check(grant.Release(grant.request()).code==C::released,"exact post-conversion release path");
  });
  Run([](auto& owner,auto&,auto&) {
    auto op=Operation(owner,40);
    auto original=Request(50,M::intent_write);
    c::ModeLatchGrant grant;
    Check(op.Acquire(original,Memory(100),grant,{}).code==C::acquired,"sole-writer original");
    auto wrong=original; wrong.request=Id(51);
    auto r=op.UpgradeIntent(grant,wrong,0,{});
    Check(r.result.code==C::invalid && !r.original_released && grant,"foreign request preserves intent");
    std::binary_semaphore ready{0},release{0};
    std::thread writer([&] {
      auto other=Operation(owner,41);
      c::ModeLatchGrant second;
      auto request=Request(51,M::intent_write);
      Check(other.Acquire(request,Memory(120),second,{}).code==C::acquired,"actual concurrent intent writer");
      ready.release(); release.acquire();
      Check(second.Release(request).code==C::released,"second writer release");
    });
    ready.acquire(); r=op.UpgradeIntent(grant,original,0,{});
    Check(r.result.code==C::busy && !r.original_released && grant,"nonsole writer cannot release for conversion");
    release.release(); writer.join();
    std::thread foreign([&] {
      auto other=Operation(owner,42);
      auto result=other.UpgradeIntent(grant,original,0,{});
      Check(result.result.code==C::invalid && !result.original_released,"wrong native execution preserves original intent");
    });
    foreign.join();
    r=op.UpgradeIntent(grant,original,0,{});
    Check(r.result.code==C::acquired && r.original_released,"sole writer retry acquires upgrade");
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
void Nonrecursive() {
  for (unsigned held=1;held<=11;++held) for (unsigned requested=1;requested<=11;++requested)
    for (unsigned state=0;state<8;++state) Run([&](auto& owner,auto& domain,auto& fixture) {
      auto op=Operation(owner,40);
      c::ModeLatchGrant grant, refused;
      auto original=Request(50,static_cast<M>(held));
      Check(op.Acquire(original,Memory(100),grant,{}).code==C::acquired,"original grant before recursive attempt");
      std::stop_source stop;
      if (state&1) Check(owner.Close(),"close before recursion validation");
      if (state&2) stop.request_stop();
      const auto deadline=state&4 ? c::ModeLatchClock::now() : c::ModeLatchClock::now()+1s;
      const auto before=domain.Snapshot();
      const auto physical=fixture.manager.Snapshot().current_bytes;
      for (bool immediate : {false,true}) for (unsigned id : {50U,51U}) {
        auto request=Request(id,static_cast<M>(requested));
        Check(op.Acquire(request,Memory(110),refused,deadline,stop.get_token(),immediate).code==C::recursive,
              "all recursive mode pairs refused before terminals and physical preparation");
        Check(!refused && grant && grant.request()==original,"recursive refusal preserves exact original grant");
        const auto after=domain.Snapshot();
        Check(before.readers==after.readers && before.retired==after.retired &&
              before.retained_payload_bytes==after.retained_payload_bytes &&
              physical==fixture.manager.Snapshot().current_bytes,"recursive refusal unchanged real memory ownership");
      }
      const auto snapshot=owner.Snapshot();
      Check(snapshot.grants==1 && snapshot.native.holders==1 && !snapshot.native.waiters && !snapshot.native.calls,
            "recursive refusal registers no waiter or additional holder");
      Check(grant.Release(original).code==C::released,"original release after recursive refusal");
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
void PreparationOwnershipRecheck() {
  for (unsigned held=1;held<=11;++held) for (unsigned requested=1;requested<=11;++requested)
    Run([&](auto& owner,auto& domain,auto&) {
      auto outer=Operation(owner,40);
      auto inner=Operation(owner,41);
      c::ModeLatchGrant first, refused;
      PreparationReentry hook{&inner,&first,Request(50,static_cast<M>(held)),Memory(120)};
      preparation_reentry=&hook;
      const auto result=outer.Acquire(Request(51,static_cast<M>(requested)),Memory(110),refused,{});
      Check(hook.reached && result.code==C::recursive && !refused && first,
            "ownership rechecked after actual record preparation for every mode pair");
      Check(domain.Collect()==S::ok && domain.Snapshot().retired==1,"refused outer record collected without touching nested holder");
      Check(first.Release(hook.request).code==C::released,"prepared nested holder retains exact release path");
    });
}
void PreparationOrderRecheck() {
  for (const bool mutex:{false,true}) for (unsigned mode=1;mode<=11;++mode) {
    Fixture fixture;
    {
      m::MemorySafeRetirement domain(*fixture.resource,Id(10),8,16);
      Check(domain.Initialize()==S::ok,"preparation order domain");
      {
        MixedLatch lower(domain,mutex,0,classes[0],M::shared_read);
        MixedLatch higher(domain,false,1,classes[12],static_cast<M>(mode));
        PreparationReentry hook{&higher.mode_op,&higher.mode_grant,higher.mode_request,Memory(120)};
        preparation_reentry=&hook;
        const auto result=lower.Acquire(mutex?112:110,false);
        Check(hook.reached && result.code==C::order_violation &&
              !lower.mode_grant && !lower.mutex_grant && higher.mode_grant,
              "shared order rechecked after actual memory preparation in both mechanisms");
        Check(result.order_conflict && result.order_conflict->held_primitive==Id(21) &&
              result.order_conflict->held_class==classes[12],"preparation refusal identifies nested actual holder");
        higher.Release();
        Check(domain.Collect()==S::ok && lower.Acquire(140).code==C::acquired,
              "preparation refusal preserves original release and leaves no phantom order node");
      }
      Check(domain.Collect()==S::ok,"preparation order cleanup");
    }
    fixture.Empty();
  }
}
void IntentUpgradeDeliveryLifetime() {
  for (const bool selected:{false,true}) Run([&](auto& owner,auto& domain,auto&) {
    auto reader_op=Operation(owner,41);
    c::ModeLatchGrant reader;
    const auto reader_request=Request(51);
    Check(reader_op.Acquire(reader_request,Memory(120),reader,{}).code==C::acquired,
          "delivery gap blocking reader");
    Park paused; paused.pause_delivery=true;
    std::stop_source stop;
    std::binary_semaphore delivered{0},finish{0};
    std::thread converter([&] {
      auto op=Operation(owner,40);
      c::ModeLatchGrant grant;
      const auto original=Request(50,M::intent_write);
      Check(op.Acquire(original,Memory(100),grant,{}).code==C::acquired,
            "delivery gap original intent");
      park=&paused;
      const auto result=op.UpgradeIntent(grant,original,0,{},stop.get_token());
      park=nullptr;
      Check(result.original_released && result.result.code==(selected?C::acquired:C::closed),
            "selected conversion outcome survives later cancellation and close");
      Check(selected ? grant && grant.request().mode==M::upgrade : !grant,
            "delivered conversion owns only its selected upgrade");
      delivered.release(); finish.acquire();
      if (grant) Check(grant.Release(grant.request()).code==C::released,
                       "upgrade selected before close retains exact release path");
    });
    Check(paused.entered.try_acquire_for(2s),"conversion entered actual native wait");
    if (!selected) Check(owner.Close(),"close selects conversion refusal");
    if (selected) Check(reader.Release(reader_request).code==C::released,
                        "reader release selects native upgrade");
    Check(paused.committed.try_acquire_for(2s),"conversion paused before native result delivery");
    if (!selected) Check(reader.Release(reader_request).code==C::released,
                         "reader survives conversion refusal");
    reader_op.Reset();
    Check(owner.Close() && owner.FenceAdmission()==S::ok,"fence in conversion delivery gap");
    domain.Close();
    const auto pending=owner.Snapshot();
    Check(pending.operations==1 && pending.grants==0 && pending.native.calls==1 &&
          pending.native.waiters==0 && pending.native.holders==(selected?1u:0u),
          "undelivered conversion retains whole call with no delivered grant");
    Check(domain.Snapshot().readers==4,"conversion delivery retains all four actual guards");
    Check(domain.Collect()==S::ok && domain.Snapshot().retired==2 &&
          domain.Snapshot().reclamation_blocked_objects==2,
          "closed domain cannot reclaim conversion state or record before delivery");
    stop.request_stop();
    Check(owner.Drain(c::ModeLatchClock::now()+20ms).code==C::timed_out,
          "shutdown cannot drain an undelivered conversion result");
    paused.resume.release();
    Check(delivered.try_acquire_for(2s),"conversion result delivered after domain close");
    const auto complete=owner.Snapshot();
    Check(complete.operations==1 && complete.native.calls==0 &&
          complete.grants==(selected?1u:0u) && complete.native.holders==(selected?1u:0u),
          "delivered conversion separates operation and grant ownership");
    Check(domain.Collect()==S::ok && domain.Snapshot().retired==(selected?2u:1u),
          "only refused conversion record becomes reclaimable at delivery");
    Check(owner.Drain(c::ModeLatchClock::now()+20ms).code==C::timed_out,
          "delivered conversion still requires operation and grant release");
    finish.release(); converter.join();
    Check(owner.Drain(c::ModeLatchClock::now()+2s).code==C::drained,
          "conversion shutdown drains after actual worker cleanup");
  });
}
void IntentUpgradeParking() {
  // An earlier exclusive waiter must run first; retaining the original intent
  // while queued would deadlock this actual two-execution path.
  Run([](auto& owner,auto&,auto&) {
    auto op=Operation(owner,40); c::ModeLatchGrant grant;
    const auto original=Request(50,M::intent_write);
    Check(op.Acquire(original,Memory(100),grant,{}).code==C::acquired,"queued conversion original");
    Park earlier; std::atomic<bool> exclusive_ran=false;
    std::thread writer([&] {
      auto other=Operation(owner,41); c::ModeLatchGrant exclusive;
      park=&earlier; const auto request=Request(51,M::exclusive_write);
      Check(other.Acquire(request,Memory(120),exclusive,c::ModeLatchClock::now()+5s).code==C::acquired,
            "earlier exclusive waiter makes actual progress");
      exclusive_ran=true;
      Check(exclusive.Release(request).code==C::released,"earlier exclusive released"); park=nullptr;
    });
    earlier.entered.acquire();
    const auto result=op.UpgradeIntent(grant,original,0,c::ModeLatchClock::now()+5s);
    Check(result.result.code==C::acquired && result.original_released && exclusive_ran,
          "conversion releases before normal queue without bypass or self-blocking");
    writer.join();
  });
  for (unsigned outcome=0;outcome<3;++outcome) Run([&](auto& owner,auto& domain,auto&) {
    std::binary_semaphore reader_ready{0},reader_release{0};
    std::thread reader([&] {
      auto op=Operation(owner,41); c::ModeLatchGrant grant; const auto request=Request(51);
      Check(op.Acquire(request,Memory(120),grant,{}).code==C::acquired,"reader blocks upgrade after intent release");
      reader_ready.release(); reader_release.acquire();
    });
    reader_ready.acquire();
    auto op=Operation(owner,40); c::ModeLatchGrant grant; const auto original=Request(50,M::intent_write);
    Check(op.Acquire(original,Memory(100),grant,{}).code==C::acquired,"intent alongside reader");
    Park parked; std::stop_source stop;
    std::thread signal([&] {
      parked.entered.acquire();
      Check(owner.Snapshot().native.holders==1 && owner.Snapshot().grants==1,
            "parked conversion no longer holds intent");
      Check(domain.Snapshot().readers>=6,"conversion gap retains actual operation state and record guards");
      if (outcome==0) stop.request_stop();
      if (outcome==1) Check(owner.Close(),"close after original release");
    });
    park=&parked;
    auto result=op.UpgradeIntent(grant,original,0,c::ModeLatchClock::now()+100ms,stop.get_token());
    park=nullptr; signal.join();
    Check(result.original_released && !grant &&
          result.result.code==(outcome==0?C::cancelled:outcome==1?C::closed:C::timed_out),
          "post-release terminal explicitly reports original ownership gone");
    Check(grant.Release(original).code==C::no_grant,"no silent original grant recreation");
    reader_release.release(); reader.join();
  });
  for (const bool full:{false,true}) Run([&](auto& owner,auto& domain,auto&) {
    std::binary_semaphore ready{0},release{0};
    std::thread reader([&] {
      auto op=Operation(owner,41); c::ModeLatchGrant grant;
      Check(op.Acquire(Request(51),Memory(120),grant,{}).code==C::acquired,"failure path blocking reader");
      ready.release(); release.acquire();
    });
    ready.acquire(); auto op=Operation(owner,40); c::ModeLatchGrant grant;
    const auto original=Request(50,M::intent_write);
    Check(op.Acquire(original,Memory(100),grant,{}).code==C::acquired,"failure path original intent");
    std::array<Park,4> parks;
    std::vector<std::thread> waiters;
    if (full) for (unsigned n=0;n<4;++n) {
      waiters.emplace_back([&,n] {
        auto other=Operation(owner,42+n); c::ModeLatchGrant pending;
        park=&parks[n];
        Check(other.Acquire(Request(52+n,M::exclusive_write),Memory(140+3*n),pending,{}).code==C::closed,
              "full queue waiter wakes on actual close");
        park=nullptr;
      });
      parks[n].entered.acquire();
    }
    fail_wait=!full;
    const auto result=op.UpgradeIntent(grant,original,0,{});
    fail_wait=false;
    Check(result.original_released && !grant &&
          result.result.code==(full?C::exhausted:C::synchronization_failed),
          "post-release capacity and native faults explicitly lose original ownership");
    Check(owner.Close(),"failure cleanup close");
    for (auto& thread:waiters) thread.join();
    release.release(); reader.join();
    Check(domain.Collect()==S::ok && owner.Snapshot().grants==0 && owner.Snapshot().native.waiters==0,
          "failed conversion cleans real grant records and wait registrations");
  });
}
#endif
// SEARCH_KEY: RETAINED_MODE_LATCH_MIXED_COST_MEASUREMENT
// Measure actual grants, ordinary protected payload and governed reclamation.
// This is not a scheduler benchmark, an activated metric or a latency SLO.
template<std::size_t Workers, unsigned WriteEvery>
void Measurements() {
  constexpr std::size_t samples=512, warmup=32;
  constexpr std::uint64_t salt=0x9e3779b97f4a7c15ULL;
  struct Sample { std::uint64_t acquire_ns=0, cycle_ns=0; bool write=false; };
  std::array<Sample,Workers*samples> timings{};
  std::array<std::uint64_t,Workers> completions{}, worker_ns{};
  std::array<std::uint32_t,Workers> observed_holders{}, observed_waiters{};
  std::array<std::uint64_t,512> calibration{};
  const auto ns=[](auto duration) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
  };
  for(auto& sample:calibration) {
    const auto start=c::ModeLatchClock::now(); sample=ns(c::ModeLatchClock::now()-start);
  }
  std::sort(calibration.begin(),calibration.end());
  Fixture fixture;
  c::ModeLatchClock::time_point cleanup;
  {
    m::MemorySafeRetirement domain(*fixture.resource,Id(10),4*Workers+4,3*Workers+4);
    Check(domain.Initialize()==S::ok,"measurement real mode domain");
    {
      c::ModeLatchOwner owner(domain);
      Check(owner.Initialize(Identity(),{Workers,Workers,Workers},Hazard(30))==S::ok,
            "measurement real retained mode latch");
      // Only granted exclusive writers modify these ordinary, non-atomic words.
      std::uint64_t version=0, checksum=salt;
      std::latch ready(Workers), go(1);
      std::array<std::jthread,Workers> threads;
      try {
        for(std::size_t worker=0;worker<Workers;++worker) {
          threads[worker]=std::jthread([&,worker] {
            const unsigned task=99-static_cast<unsigned>(worker);
            auto operation=Operation(owner,50+worker,task);
            std::uint64_t last_seen=0;
            const auto cycle=[&](std::size_t i, bool measured) {
              const bool write=WriteEvery!=0 && (i+worker)%std::max(1U,WriteEvery)==0;
              const unsigned serial=static_cast<unsigned>(worker*(samples+warmup)+i+(measured?warmup:0));
              const auto request=Request(10000+serial,write?M::exclusive_write:M::shared_read,task);
              auto memory=Memory(200+3*worker,task);
              memory.record=Id(20000+serial); // Distinct from task/request/hazard identities.
              c::ModeLatchGrant grant;
              const auto start=c::ModeLatchClock::now();
              const auto outcome=operation.Acquire(request,memory,grant,start+10s);
              const auto acquired=c::ModeLatchClock::now();
              if(outcome.code!=C::acquired || !grant) Check(false,"measured real mode acquisition");
              if(checksum!=(version^salt) || version<last_seen)
                Check(false,"mode readers see coherent monotonic ordinary payload");
              if(write) { ++version; checksum=version^salt; }
              last_seen=version;
              if(measured && i%64==0) {
                const auto observed=owner.Snapshot().native;
                observed_holders[worker]=std::max(observed_holders[worker],observed.holders);
                observed_waiters[worker]=std::max(observed_waiters[worker],observed.waiters);
              }
              if(grant.Release(request).code!=C::released) Check(false,"measured real mode release");
              if(domain.Collect()!=S::ok) Check(false,"measured real grant record collection");
              if(measured) {
                timings[worker*samples+i]={ns(acquired-start),ns(c::ModeLatchClock::now()-start),write};
                ++completions[worker];
              }
            };
            for(std::size_t i=0;i<warmup;++i) cycle(i,false);
            ready.count_down(); go.wait(); last_seen=0;
            const auto start=c::ModeLatchClock::now();
            for(std::size_t i=0;i<samples;++i) cycle(i,true);
            worker_ns[worker]=ns(c::ModeLatchClock::now()-start);
          });
        }
      } catch(...) {
        // No worker may remain waiting on the measurement barrier after a
        // partial native launch. All created workers run finite work and join.
        go.count_down();
        for(auto& thread:threads) if(thread.joinable()) thread.join();
        Check(false,"measurement native thread creation failed");
      }
      ready.wait();
      constexpr auto warmup_writes=WriteEvery==0?0:Workers*warmup/std::max(1U,WriteEvery);
      Check(version==warmup_writes && checksum==(version^salt),"exact warmup mode payload");
      version=0; checksum=salt; // All workers wait at go; publish before release.
      const auto baseline_bytes=fixture.manager.Snapshot().current_bytes;
      const auto cpu_start=std::clock(); const auto start=c::ModeLatchClock::now();
      go.count_down();
      for(auto& thread:threads) thread.join();
      const auto wall=ns(c::ModeLatchClock::now()-start); const auto cpu_end=std::clock();
      Check(cpu_start!=std::clock_t(-1) && cpu_end!=std::clock_t(-1) && wall>0,"mode measurement clocks");
      constexpr auto expected_writes=WriteEvery==0?0:Workers*samples/std::max(1U,WriteEvery);
      Check(version==expected_writes && checksum==(version^salt),"exact final measured mode payload");
      for(auto count:completions) Check(count==samples,"every mode worker completes exact work");
      const auto state=owner.Snapshot();
      Check(state.operations==0 && state.grants==0 && !state.native.holders &&
            !state.native.waiters && !state.native.calls,"actual mode references and calls joined");
      const auto memory=domain.Snapshot();
      Check(memory.readers==1 && memory.published==1 && !memory.retired && !memory.reclaiming,
            "all private mode grant records reclaimed before owner cleanup");
      const auto profile=WriteEvery==0?"shared_read":WriteEvery==1?"exclusive_write":"mixed_7read_1write";
      for(bool write:{false,true}) {
        std::vector<std::uint64_t> acquire,cycles;
        acquire.reserve(timings.size()); cycles.reserve(timings.size());
        for(const auto& sample:timings) if(sample.write==write) {
          acquire.push_back(sample.acquire_ns); cycles.push_back(sample.cycle_ns);
        }
        if(acquire.empty()) continue;
        std::sort(acquire.begin(),acquire.end()); std::sort(cycles.begin(),cycles.end());
        const auto last=acquire.size()-1;
        std::cout<<"measurement=retained_mode profile="<<profile<<" workers="<<Workers
            <<" mode="<<(write?"write":"read")<<" samples="<<acquire.size()
            <<" acquire_p50_ns="<<acquire[last/2]<<" acquire_p99_ns="<<acquire[last*99/100]
            <<" acquire_max_ns="<<acquire.back()<<" cycle_p50_ns="<<cycles[last/2]
            <<" cycle_p99_ns="<<cycles[last*99/100]<<" cycle_max_ns="<<cycles.back()<<'\n';
      }
      std::cout<<"measurement=retained_mode_run profile="<<profile<<" workers="<<Workers
          <<" wall_ns="<<wall<<" process_cpu_ns="
          <<static_cast<double>(cpu_end-cpu_start)*1000000000.0/CLOCKS_PER_SEC
          <<" cycles_per_second="<<timings.size()*1000000000.0/wall
          <<" slowest_worker_ns="<<*std::max_element(worker_ns.begin(),worker_ns.end())
          <<" sampled_max_holders="<<*std::max_element(observed_holders.begin(),observed_holders.end())
          <<" sampled_max_waiters="<<*std::max_element(observed_waiters.begin(),observed_waiters.end())
          <<" baseline_governed_bytes="<<baseline_bytes
          <<" peak_governed_bytes="<<fixture.manager.Snapshot().peak_bytes
          <<" caller_timing_bytes="<<sizeof(timings)
          <<" clock_pair_p50_ns="<<calibration[(calibration.size()-1)/2]
          <<" clock_pair_p99_ns="<<calibration[(calibration.size()-1)*99/100]<<'\n';
      cleanup=c::ModeLatchClock::now();
      Check(owner.Close() && owner.FenceAdmission()==S::ok,"measured mode close and fence");
      Check(owner.Drain(c::ModeLatchClock::now()+10s).code==C::drained,"measured mode owner drain");
    }
    Check(domain.Collect()==S::ok,"measured mode owner physically collected");
  }
  fixture.Empty();
  std::cout<<"measurement=retained_mode_cleanup workers="<<Workers<<" write_every="<<WriteEvery
           <<" cleanup_ns="<<ns(c::ModeLatchClock::now()-cleanup)<<" final_governed_bytes=0\n";
}
template<unsigned WriteEvery> void MeasurementWorkers() {
  Measurements<1,WriteEvery>(); Measurements<2,WriteEvery>();
  Measurements<4,WriteEvery>(); Measurements<8,WriteEvery>();
}
} // namespace
int main(int argc,char** argv) {
  if(argc==2 && std::string_view(argv[1])=="--measure") {
    std::cout<<"measurement_profile=component_only clock=steady_clock cpu_clock=process"
        " cycle_includes_grant_collection=true observation_every=64"
        " clocks_and_validation_included=true latency_slo_claimed=false";
#if defined(SB_MODE_NATIVE_FAULT_GATE)
    std::cout<<" native_fault_wrappers=enabled";
#else
    std::cout<<" native_fault_wrappers=disabled";
#endif
#if defined(__OPTIMIZE__)
    std::cout<<" compiler_optimization=enabled\n";
#else
    std::cout<<" compiler_optimization=disabled\n";
#endif
    MeasurementWorkers<0>(); MeasurementWorkers<1>(); MeasurementWorkers<8>();
    std::printf("PASS retained mode measurements: %u checks\n",checks.load());
    return 0;
  }
  SharedOrderMatrix(); SharedOrderLifetime(); ClassBinding(); IntentUpgrade();
  Matrix(); Ownership(); Nonrecursive(); Exhaustion(); MultipleRetainedHolders(); RetainedTerminals();
#if defined(SB_MODE_NATIVE_FAULT_GATE)
  ParkedLifetime();
  DomainClosesBeforeProtection();
  PreparationOwnershipRecheck();
  PreparationOrderRecheck();
  IntentUpgradeParking();
  IntentUpgradeDeliveryLifetime();
#endif
  std::printf("PASS retained mode latch: %u checks\n",checks.load());
}
