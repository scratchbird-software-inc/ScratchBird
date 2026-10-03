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
  bool pause_recheck = false;
  int pause_result = -1;
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
int PauseRecheck(int result,pthread_mutex_t* mutex) {
  if (park && park->pause_recheck && (park->pause_result<0 || result==park->pause_result)) {
    park->pause_recheck=false;
    if (__real_pthread_mutex_unlock(mutex)!=0) std::abort();
    park->committed.release(); park->resume.acquire();
    if (__real_pthread_mutex_lock(mutex)!=0) std::abort();
  }
  return result;
}
extern "C" int __wrap_pthread_cond_wait(pthread_cond_t* condition, pthread_mutex_t* mutex) {
  if (std::exchange(fail_wait, false)) return EINVAL;
  ObservePark();
  const auto result=__real_pthread_cond_wait(condition, mutex);
  if (!result && park && park->pause_delivery) pause_unlock=mutex;
  return PauseRecheck(result,mutex);
}
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* condition, pthread_mutex_t* mutex,
                                           const timespec* deadline) {
  if (std::exchange(fail_wait, false)) return EINVAL;
  ObservePark();
  const auto result=__real_pthread_cond_timedwait(condition, mutex, deadline);
  if (!result && park && park->pause_delivery) pause_unlock=mutex;
  return PauseRecheck(result,mutex);
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
  auto d=c::MutexLatchDescriptor{Id(20), Id(22), c::MutexOwnerScope::database, 7, 11, Id(23)};
  d.latch_class=c::MutexLatchClass::database_state;
  return d;
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
void LatchOrderPairs() {
  using L=c::MutexLatchClass;
  // Independent canonical name order, not an oracle derived from enum numbers.
  constexpr std::array classes{L::engine_lifecycle,L::database_state,L::transaction_inventory,
      L::catalog_metadata,L::relation_index_descriptor,L::filespace_descriptor,L::allocation_map,
      L::page_cache_bucket,L::page,L::record_lineage,L::archive_descriptor,
      L::temporary_storage,L::metrics_evidence};
  for (unsigned held=1;held<=13;++held) for (unsigned requested=1;requested<=13;++requested) {
    Fixture f;
    {
      m::MemorySafeRetirement domain(*f.resource,Id(10),2,16);
      Check(domain.Initialize()==S::ok,"order domain initialized");
      {
        c::MutexLatchOwner first(domain),second(domain);
        auto a=Descriptor(),b=Descriptor(); b.primitive_id=Id(21);
        a.latch_class=classes[held-1];
        b.latch_class=classes[requested-1];
        Check(first.Initialize(a,{2,4},Hazard(30))==S::ok &&
              second.Initialize(b,{2,4},Hazard(31))==S::ok,"actual ordered latch pair");
        auto left=Operation(first,32);
        c::MutexLatchOperation right;
        Check(second.AcquireOperation(Id(21),7,Hazard(33),right)==S::ok,"ordered target operation");
        auto ra=Request(100),rb=Request(101); rb.primitive_id=Id(21);
        c::MutexLatchGrant ga,gb;
        Check(left.Acquire(ra,Hazard(34),ga,{}).code==C::acquired,"first class really held");
        const auto before=domain.Snapshot().readers;
        const auto result=right.Acquire(rb,Hazard(35),gb,{});
        Check(result.code==(requested<held ? C::order_violation : C::acquired),
              "every lower class refused before grant and every equal or higher class admitted");
        if (requested<held) {
          Check(result.order_conflict && result.order_conflict->held_primitive==a.primitive_id &&
                result.order_conflict->held_generation==a.generation &&
                result.order_conflict->held_class==a.latch_class,
                "order refusal identifies actual held binary primitive generation and class");
          Check(!gb && !second.Snapshot().native.held && second.Snapshot().native.waiters==0 &&
                domain.Snapshot().readers==before,
                "order refusal has no grant wait registration or extra hazard");
        } else Check(gb.Release(rb).code==C::released,"ordered grant actually released");
        Check(ga.Release(ra).code==C::released,"prior holder survives order decision");
        Check(right.Acquire(rb,Hazard(35),gb,{}).code==C::acquired,
              "release removes order obstruction without sticky refusal");
        Check(gb.Release(rb).code==C::released,"unobstructed grant release");
        left.Reset(); right.Reset(); Finish(first); Finish(second);
      }
      Check(domain.Collect()==S::ok,"ordered payloads collected");
    }
    f.Empty();
  }
}
void LatchOrderBeforePark() {
  Fixture f;
  {
    m::MemorySafeRetirement domain(*f.resource,Id(10),2,16);
    Check(domain.Initialize()==S::ok,"order park domain initialized");
    {
      c::MutexLatchOwner lower(domain),higher(domain);
      auto lo=Descriptor(),hi=Descriptor(); hi.primitive_id=Id(21);
      lo.latch_class=c::MutexLatchClass::engine_lifecycle;
      hi.latch_class=c::MutexLatchClass::metrics_evidence;
      Check(lower.Initialize(lo,{2,4},Hazard(30))==S::ok &&
            higher.Initialize(hi,{2,4},Hazard(31))==S::ok,"order park real objects");
      auto waiting=Operation(lower,32);
      std::binary_semaphore held{0},release{0};
      std::thread holder([&] {
        auto op=Operation(lower,33,98); auto request=Request(101,98);
        c::MutexLatchGrant grant;
        Check(op.Acquire(request,Hazard(34,98),grant,{}).code==C::acquired,"contended lower physically held");
        held.release(); release.acquire();
        Check(grant.Release(request).code==C::released,"contended lower owner releases");
      });
      held.acquire();
      c::MutexLatchOperation own;
      Check(higher.AcquireOperation(Id(21),7,Hazard(35),own)==S::ok,"higher park operation");
      auto high_request=Request(102); high_request.primitive_id=Id(21);
      c::MutexLatchGrant high_grant;
      Check(own.Acquire(high_request,Hazard(36),high_grant,{}).code==C::acquired,"higher latch really held at refusal");
      for (const bool immediate:{false,true}) {
        c::MutexLatchGrant refused;
#if defined(SB_MUTEX_NATIVE_FAULT_GATE)
        Park probe; park=&probe;
#endif
        Check(waiting.Acquire(Request(),Hazard(37),refused,c::MutexClock::now()+50ms,{},immediate).code==C::order_violation,
              "contended lower order refuses before native blocking or try grant");
#if defined(SB_MUTEX_NATIVE_FAULT_GATE)
        park=nullptr; Check(!probe.observed,"order violation never reaches native park");
#endif
        Check(!refused && lower.Snapshot().native.waiters==0,"order refusal never joins actual FIFO queue");
      }
      Check(high_grant.Release(high_request).code==C::released,"higher owner preserved across refusal");
      release.release(); holder.join(); own.Reset(); waiting.Reset();
      Finish(lower); Finish(higher);
    }
    Check(domain.Collect()==S::ok,"order park objects reclaimed");
  }
  f.Empty();
}
void LatchOrderOwnership() {
  Fixture f;
  {
    m::MemorySafeRetirement domain(*f.resource,Id(10),4,32);
    Check(domain.Initialize()==S::ok,"order ownership domain initialized");
    {
      std::array<std::unique_ptr<c::MutexLatchOwner>,4> owners;
      std::array<c::MutexLatchOperation,4> operations;
      std::array<c::MutexLatchRequest,4> requests;
      std::array<c::MutexLatchGrant,4> grants;
      for (unsigned i=0;i<4;++i) {
        owners[i]=std::make_unique<c::MutexLatchOwner>(domain);
        auto d=Descriptor(); d.primitive_id=Id(20+i);
        d.latch_class=static_cast<c::MutexLatchClass>(i==3 ? 1 : 3+i*3);
        Check(owners[i]->Initialize(d,{4,8},Hazard(30+i))==S::ok,"governed order ownership payload");
        Check(owners[i]->AcquireOperation(Id(20+i),7,Hazard(40+i),operations[i])==S::ok,
              "real retained order operation");
        requests[i]=Request(100+i); requests[i].primitive_id=Id(20+i);
      }
      const auto acquire=[&](unsigned i) {
        return operations[i].Acquire(requests[i],Hazard(50+i),grants[i],{}).code;
      };
      const auto release=[&](unsigned i) {
        Check(grants[i].Release(requests[i]).code==C::released,"actual ordered ownership release");
      };
      Check(acquire(0)==C::acquired && acquire(1)==C::acquired && acquire(2)==C::acquired,
            "three actual held order nodes");
      // Nodes belong to retained payloads, not the addresses of movable wrappers.
      c::MutexLatchGrant moved(std::move(grants[2]));
      release(1); // Remove the middle, not necessarily a LIFO release.
      Check(acquire(1)==C::order_violation && acquire(3)==C::order_violation,
            "middle removal and grant move retain highest obstruction");
      auto wrong=requests[2]; wrong.request_id=Id(999);
      Check(moved.Release(wrong).code==C::wrong_owner && acquire(3)==C::order_violation,
            "wrong release cannot erase actual held order node");
      // Same binary task may execute elsewhere. Native held order belongs to
      // this execution, not to an unproved task-wide exclusivity assertion.
      std::thread other([&] {
        Check(moved.Release(requests[2]).code==C::wrong_owner,
              "wrong native owner cannot mutate another thread held stack");
        Check(acquire(3)==C::acquired,"held stack does not leak into another native execution");
        release(3);
      });
      other.join();
      Check(acquire(3)==C::order_violation,"wrong native release preserves original order obstruction");
#if defined(SB_MUTEX_NATIVE_FAULT_GATE)
      for (unsigned failure=1;failure<=2;++failure) {
        fail_lock=failure;
        Check(moved.Release(requests[2]).code==C::synchronization_failed,
              "real release lock fault preserves grant");
        Check(acquire(3)==C::order_violation,"failed native release preserves held order node");
      }
#endif
      Check(owners[2]->Close(Id(70)) && owners[2]->FenceAdmission()==S::ok,
            "actual highest owner retired with committed grant");
      Check(acquire(3)==C::order_violation,"close and retirement do not erase holder order");
      grants[2]=std::move(moved); release(2);
      Check(acquire(1)==C::acquired,"highest release reveals remaining held class");
      // Move assignment releases its prior live destination before adopting the
      // source, but cannot move the source's physical node or lose its class.
      grants[0]=std::move(grants[1]);
      Check(acquire(3)==C::order_violation,"move assignment preserves adopted higher order");
      Check(grants[0].Release(requests[1]).code==C::released,"adopted grant releases exact request");
      Check(acquire(3)==C::acquired,"all actual held nodes removed after non-LIFO releases");
      release(3);
      // Failed acquisition must never leave a phantom holder node.
      Check(acquire(0)==C::acquired,"lower holder for acquisition failure check");
      std::stop_source stop; stop.request_stop();
      Check(operations[1].Acquire(requests[1],Hazard(51),grants[1],{},stop.get_token()).code==C::cancelled,
            "cancelled higher acquisition has no grant");
      release(0);
      Check(acquire(3)==C::acquired,"failed acquisition leaves no phantom high order");
      release(3);
      for (auto& op:operations) op.Reset();
      for (auto& owner:owners) Finish(*owner);
    }
    Check(domain.Collect()==S::ok && domain.Snapshot().retired==0,"order nodes reclaimed only after release");
  }
  f.Empty();
}
void Descriptors() {
  Fixture f;
  {
    m::MemorySafeRetirement domain(*f.resource,Id(10),2,8);
    Check(domain.Initialize()==S::ok,"descriptor domain initialized");
    for (unsigned test=0;test<14;++test) {
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
        case 12:d.latch_class=c::MutexLatchClass::unspecified;break;
        case 13:d.latch_class=static_cast<c::MutexLatchClass>(255);break;
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
void CycleTopology(unsigned count,const std::array<unsigned,8>& targets,unsigned expected,unsigned terminal=0,bool alias=false,bool rebound=false,bool permutations=false) {
  using N=scratchbird::core::platform::CheckedFifoMutex;
  Fixture fixture;
  {
    m::MemorySafeRetirement domain(*fixture.resource,Id(10),count,64);
    Check(domain.Initialize()==S::ok,"topology real memory domain");
    std::array<std::optional<c::MutexLatchOwner>,8> owners;
    std::array<c::MutexLatchOperation,8> observers;
    std::array<c::MutexLatchOperation*,8> operations;
    for (unsigned i=0;i<count;++i) {
      owners[i].emplace(domain); auto d=Descriptor(); d.primitive_id=Id(700+i); d.generation=7+i;
      Check(owners[i]->Initialize(d,{8,8},Hazard(800+i))==S::ok,"topology actual backing initialized");
      Check(owners[i]->AcquireOperation(d.primitive_id,d.generation,Hazard(900+i,97),observers[i])==S::ok,
            "topology actual observation guard"); operations[i]=&observers[i];
    }
    std::array<Park,8> probes;
    if (terminal) { probes[0].pause_recheck=true; probes[0].pause_result=terminal==3?ETIMEDOUT:-1; }
    std::array<std::stop_source,8> stops;
    std::array<std::thread,8> threads;
    std::counting_semaphore<8> ready(0),proceed(0),idle(0),release_holders(0);
    for (unsigned i=0;i<count;++i) threads[i]=std::thread([&,i] {
      const auto task=1000+(alias && i==2?0:i);
      c::MutexLatchOperation op,waiting_op; c::MutexLatchGrant grant,waiting_grant;
      c::MutexLatchRequest request{Id(700+i),7+i,Id(task),Id(1500+i)};
      Check(owners[i]->AcquireOperation(request.primitive_id,request.generation,Hazard(1100+i,task),op)==S::ok &&
            op.Acquire(request,Hazard(1300+i,task),grant,{}, {},true).code==C::acquired,"topology actual holder");
      ready.release(); proceed.acquire();
      if (targets[i]<count) {
        const auto target=targets[i];
        const auto waiting_task=task+(rebound && i==0?20:0);
        c::MutexLatchRequest wait{Id(700+target),7+target,Id(waiting_task),Id(1600+i)};
        Check(owners[target]->AcquireOperation(wait.primitive_id,wait.generation,Hazard(1200+i,waiting_task),waiting_op)==S::ok,
              "topology retained blocking request"); park=&probes[i];
        const auto deadline=(terminal==3 && i==0)?std::optional(c::MutexClock::now()+1s):std::nullopt;
        const auto code=waiting_op.Acquire(wait,Hazard(1400+i,waiting_task),waiting_grant,deadline,stops[i].get_token()).code;
        Check(code==(i==0 && terminal==1?C::closed:i==0 && terminal==3?C::timed_out:C::cancelled),
              "topology terminal result preserves actual ownership");
      } else idle.acquire();
      release_holders.acquire();
      Check(grant.Release(request).code==C::released,"topology holder performs own release");
    });
    for (unsigned i=0;i<count;++i) Check(ready.try_acquire_for(5s),"topology holders ready");
    proceed.release(count);
    for (unsigned i=0;i<count;++i) if (targets[i]<count)
      Check(probes[i].entered.try_acquire_for(5s),"topology waits actually parked");
    std::array<N::WaitSetEntry,8> frames;
    std::array<c::MutexLatchWaitSetObservation,8> observations;
    std::array<N::WaiterObservation,8> waits;
    std::array<c::MutexLatchCycleEdge,8> edges;
    std::array<std::size_t,8> cycle;
    if (terminal) {
      if (terminal==1) Check(owners[targets[0]]->Close(Id(1900)),"close actual cycle edge");
      if (terminal==2) stops[0].request_stop();
      Check(probes[0].committed.try_acquire_for(5s),"terminal waiter paused before actual unlink");
    }
    std::array<unsigned,8> permutation{};
    for (unsigned i=0;i<count;++i) permutation[i]=i;
    unsigned captures=0;
    do {
      for (unsigned i=0;i<count;++i) operations[i]=&observers[permutation[i]];
      const auto found=c::MutexLatchOperation::CaptureWaitCycle(std::span(operations).first(count),frames,observations,waits,edges,cycle,8,8);
      if (alias) Check(found.status==c::MutexLatchCycleStatus::no_cycle_in_set &&
          observations[0].wait.owner==observations[2].wait.owner &&
          observations[0].wait.holder!=observations[2].wait.holder,
          "concurrent executions sharing task UUID do not manufacture a thread cycle");
      if (rebound) Check(found.status==c::MutexLatchCycleStatus::execution_binding_required,
          "different binary task bindings on one native thread are not spliced");
      if (terminal) Check(found.status==(expected?c::MutexLatchCycleStatus::cycle:c::MutexLatchCycleStatus::no_cycle_in_set),
          "terminal wait cannot contribute a continuing cycle before unlink");
      Check(found.status==(rebound?c::MutexLatchCycleStatus::execution_binding_required:
            expected?c::MutexLatchCycleStatus::cycle:c::MutexLatchCycleStatus::no_cycle_in_set) &&
            found.cycle_size==expected,"actual topology yields exact cycle length independent of input permutation");
      if (terminal) {
        unsigned registered=0; for (unsigned i=0;i<count;++i) registered+=observations[i].wait.state.waiters;
        Check(registered==count,"terminal filtering precedes actual edge unlink");
      }
      for (unsigned i=0;i<expected;++i) {
        const auto& held=observations[edges[cycle[i]].latch_index].wait;
        const auto& next=waits[edges[cycle[(i+1)%expected]].waiter_index];
        Check(held.holder==next.thread && held.owner==next.owner,"independent topology witness closes every execution edge");
        if (terminal) Check(waits[edges[cycle[i]].waiter_index].owner!=Id(1000),
            "surviving cycle never reuses the terminated execution edge");
      }
      ++captures;
      if (!permutations) {
        if (captures==2) break;
        std::reverse(permutation.begin(),permutation.begin()+count);
      }
    } while (!permutations || std::next_permutation(permutation.begin(),permutation.begin()+count));
    Check(captures==(permutations?24u:2u),"every requested capture ordering exercised");
    // The selected terminal edge already has its close/stop/deadline outcome.
    // Do not inject cancellation ahead of an expired deadline during teardown.
    for (unsigned i=0;i<count;++i) if (!terminal || i!=0) stops[i].request_stop();
    if (terminal) probes[0].resume.release();
    idle.release(count);
    release_holders.release(count);
    for (unsigned i=0;i<count;++i) threads[i].join();
    for (unsigned i=0;i<count;++i) { observers[i].Reset(); Finish(*owners[i]); owners[i].reset(); }
    Check(domain.Collect()==S::ok && domain.Snapshot().retired==0,"topology backing reclaimed by real memory owner");
  }
  fixture.Empty();
}
void NativeBoundaries() {
  // All 27 nonrecursive three-execution wait topologies, including missing
  // edges, chains, merging paths, cycles and tails into cycles. Oracle uses
  // the small graph's independent two-pair/three-ring characterization.
  for (unsigned a=0;a<3;++a) for (unsigned b=0;b<3;++b) for (unsigned d=0;d<3;++d) {
    const std::array<unsigned,3> choices{a,b,d}; std::array<unsigned,8> targets{};
    bool all=true; unsigned expected=0;
    for (unsigned i=0;i<3;++i) { targets[i]=choices[i]==2?3:(i+1+choices[i])%3; all&=targets[i]<3; }
    for (unsigned i=0;i<3;++i) for (unsigned j=i+1;j<3;++j)
      if (targets[i]==j && targets[j]==i) expected=2;
    if (!expected && all) expected=3;
    CycleTopology(3,targets,expected);
  }
  // All256 nonrecursive four-execution graphs, each captured in all24 input
  // permutations. Independent transitive closure, rather than the production
  // functional-graph traversal, determines strongly connected cycle size.
  // Two distinct cycles can coexist here; both then have exactly two members.
  std::array<unsigned,5> sizes{};
  for (unsigned encoded=0;encoded<256;++encoded) {
    unsigned value=encoded; std::array<unsigned,8> targets{};
    bool reaches[4][4]{};
    for (unsigned i=0;i<4;++i) {
      const auto choice=value%4; value/=4;
      targets[i]=choice==3?4:(i+1+choice)%4;
      if (targets[i]<4) reaches[i][targets[i]]=true;
    }
    for (unsigned via=0;via<4;++via) for (unsigned from=0;from<4;++from)
      for (unsigned to=0;to<4;++to) reaches[from][to]|=reaches[from][via] && reaches[via][to];
    unsigned expected=0;
    for (unsigned i=0;i<4;++i) if (reaches[i][i]) {
      unsigned members=0;
      for (unsigned j=0;j<4;++j) members+=reaches[i][j] && reaches[j][i];
      Check(!expected || expected==members,"independent four-node cycle cardinalities agree");
      expected=members;
    }
    ++sizes[expected];
    CycleTopology(4,targets,expected,0,false,false,true);
  }
  // Acyclic:5^3 forests rooted at the absent-edge sink. Two-cycles:
  // six chosen pairs * sixteen remaining choices minus three double-counted
  // disjoint pairs. Three-cycles:four triples * two directions * four choices
  // for the remaining node. Four-cycles:3! directed rings.
  Check(sizes[0]==125 && sizes[2]==93 && sizes[3]==32 && sizes[4]==6,
        "exhaustive graph population matches independent labeled-tree and cycle counts");
  CycleTopology(8,{1,2,3,4,5,6,7,0},8);
  CycleTopology(3,{1,2,3},0,0,true);
  CycleTopology(2,{1,0},0,0,false,true);
  for (unsigned terminal:{1U,2U,3U}) {
    CycleTopology(2,{1,0},0,terminal);
    CycleTopology(4,{1,0,3,2},2,terminal);
    CycleTopology(5,{1,0,3,4,2},3,terminal);
  }
  Run([](auto& left,auto& domain) {
    c::MutexLatchOwner right(domain); auto d=Descriptor(); d.primitive_id=Id(21); d.generation=8;
    Check(right.Initialize(d,{4,8},Hazard(40))==S::ok,"second real retained latch published");
    auto observe_left=Operation(left,41,97); c::MutexLatchOperation observe_right;
    Check(right.AcquireOperation(Id(21),8,Hazard(42,97),observe_right)==S::ok,"second actual observation guard");
    std::counting_semaphore<2> ready(0), proceed(0); Park a,b; std::stop_source stop;
    auto run=[&](bool reversed) {
      const unsigned task=reversed?98:99, base=reversed?50:60;
      auto first_request=Request(base,task), second_request=Request(base+1,task);
      auto& first=reversed?right:left; auto& second=reversed?left:right;
      if (reversed) { first_request.primitive_id=Id(21); first_request.generation=8; }
      else { second_request.primitive_id=Id(21); second_request.generation=8; }
      c::MutexLatchOperation first_op,second_op;
      Check(first.AcquireOperation(first_request.primitive_id,first_request.generation,Hazard(base+2,task),first_op)==S::ok &&
            second.AcquireOperation(second_request.primitive_id,second_request.generation,Hazard(base+3,task),second_op)==S::ok,
            "cycle operations retain both actual backing objects");
      c::MutexLatchGrant holder,waiting;
      Check(first_op.Acquire(first_request,Hazard(base+4,task),holder,{}, {},true).code==C::acquired,"retained cycle actual grant");
      ready.release(); proceed.acquire(); park=reversed?&b:&a;
      Check(second_op.Acquire(second_request,Hazard(base+5,task),waiting,{},stop.get_token()).code==C::cancelled,
            "retained cycle wait ends through real cooperative cancellation");
      Check(!waiting && holder.Release(first_request).code==C::released,"retained cycle owner releases only its own grant");
    };
    std::thread one([&]{run(false);}),two([&]{run(true);});
    Check(ready.try_acquire_for(5s) && ready.try_acquire_for(5s),"both retained holders ready"); proceed.release(2);
    Check(a.entered.try_acquire_for(5s) && b.entered.try_acquire_for(5s),"actual retained cycle parked");
    using N=scratchbird::core::platform::CheckedFifoMutex; using R=N::WaitSetResult;
    std::array<c::MutexLatchOperation*,2> operations{&observe_right,&observe_left};
    std::array<N::WaitSetEntry,2> frames;
    std::array<c::MutexLatchWaitSetObservation,2> observations;
    std::array<N::WaiterObservation,2> waits;
    const auto readers=domain.Snapshot().readers;
    std::array<c::MutexLatchCycleEdge,2> edges;
    std::array<std::size_t,2> cycle;
    const auto detected=c::MutexLatchOperation::CaptureWaitCycle(operations,frames,observations,waits,edges,cycle,2,2);
    Check(detected.status==c::MutexLatchCycleStatus::cycle && detected.cycle_size==2,
          "detect a cycle from actual retained waits not caller asserted edges");
    for (unsigned failure:{1U,2U}) {
      fail_lock=failure;
      const auto failed=c::MutexLatchOperation::CaptureWaitCycle(operations,frames,observations,waits,edges,cycle,2,2);
      Check(failed.status==c::MutexLatchCycleStatus::capture_failed && failed.capture==R::synchronization_failed &&
            !failed.cycle_size,"capture failure never becomes a no-cycle receipt");
    }
    Check(c::MutexLatchOperation::CaptureWaitCycle(operations,frames,observations,waits,edges,cycle,2,0).status==
          c::MutexLatchCycleStatus::capture_failed,"zero waiter bound fails before graph analysis");
    const auto limited=c::MutexLatchOperation::CaptureWaitCycle(operations,frames,observations,waits,edges,cycle,2,1);
    Check(limited.status==c::MutexLatchCycleStatus::capture_failed && limited.capture==R::insufficient_capacity,
          "waiter admission bound cannot produce a truncated graph");
    Check(c::MutexLatchOperation::CaptureWaitCycle(operations,frames,observations,waits,std::span(edges).first(1),cycle,2,2).status==
          c::MutexLatchCycleStatus::exhausted,"edge scratch exhaustion cannot become no-cycle evidence");
    Check(c::MutexLatchOperation::CaptureWaitCycle(operations,frames,observations,waits,edges,cycle,2,2).status==
          c::MutexLatchCycleStatus::cycle,"fresh capture rebuilds graph after bounded failures");
    for (unsigned i=0;i<2;++i) {
      const auto& edge=edges[cycle[i]];
      const auto& successor=edges[cycle[(i+1)%2]];
      const auto& held=observations[edge.latch_index].wait;
      const auto& next_wait=waits[successor.waiter_index];
      Check(held.holder==next_wait.thread && held.owner==next_wait.owner,
            "cycle witness links exact native thread and binary execution owner");
    }
    Check(c::MutexLatchOperation::CaptureWaitCycle(operations,frames,observations,waits,edges,std::span(cycle).first(1),2,2).status==
          c::MutexLatchCycleStatus::exhausted,"cycle scratch capacity cannot silently hide a real cycle");
    Check(c::MutexLatchOperation::CaptureWaitCycle(std::span(operations).first(1),frames,observations,waits,edges,cycle,2,2).status==
          c::MutexLatchCycleStatus::no_cycle_in_set,"incomplete supplied latch set never manufactures missing edges");
    Check(c::MutexLatchOperation::SnapshotWaitSet(operations,frames,observations,waits,2)==R::captured,
          "capture actual retained two-latch set");
    Check(observations[0].primitive_id==Id(21) && observations[0].generation==8 &&
          observations[1].primitive_id==Id(20) && observations[1].generation==7 &&
          observations[0].wait.owner==Id(98) && observations[1].wait.owner==Id(99) &&
          waits[observations[0].offset].owner==Id(99) && waits[observations[1].offset].owner==Id(98),
          "retained capture binds exact binary primitive generations to real cycle edges");
    Check(!frames[0].mutex && !frames[1].mutex && !frames[0].lock.owns_lock() &&
          !frames[1].lock.owns_lock() && domain.Snapshot().readers==readers,
          "capture clears borrowed native pointers and preserves real reader count");
    waits[0].owner=Id(88);
    Check(c::MutexLatchOperation::SnapshotWaitSet(operations,frames,observations,std::span(waits).first(1),2)==R::insufficient_capacity &&
          !observations[0].wait.complete && !observations[1].wait.complete && waits[0].owner==Id(88),
          "retained capacity refusal preserves full identity association without partial edges");
    operations[1]=operations[0];
    Check(c::MutexLatchOperation::SnapshotWaitSet(operations,frames,observations,waits,2)==R::invalid,
          "duplicate retained identity rejected before capture");
    operations[1]=nullptr;
    Check(c::MutexLatchOperation::SnapshotWaitSet(operations,frames,observations,waits,2)==R::invalid,
          "absent retained operation rejected before capture");
    operations[1]=&observe_left;
    Check(c::MutexLatchOperation::SnapshotWaitSet(operations,frames,std::span(observations).first(1),waits,2)==R::exhausted,
          "retained output metadata must cover the admitted set");
    stop.request_stop(); one.join(); two.join();
    Check(left.Close(Id(24)) && right.Close(Id(25)) && left.FenceAdmission()==S::ok && right.FenceAdmission()==S::ok,
          "both observed latches fenced through real retirement");
    Check(domain.Collect()==S::ok && domain.Snapshot().reclamation_blocked_objects==2,
          "real observation guards retain both retired objects");
    Check(c::MutexLatchOperation::SnapshotWaitSet(operations,frames,observations,{},2)==R::captured &&
          observations[0].wait.state.closed && observations[1].wait.state.closed,
          "retained capture stays valid after fence without permitting new grants");
    Check(c::MutexLatchOperation::CaptureWaitCycle(operations,frames,observations,waits,edges,cycle,2,2).status==
          c::MutexLatchCycleStatus::no_cycle_in_set,"resolved cycle is not replayed from stale scratch");
    Check(right.Drain(c::MutexClock::now()).code==C::timed_out,"observation guard is not a drained runtime");
    observe_left.Reset(); observe_right.Reset(); Finish(right);
  });
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
      Check(result.wait.registered,"retained terminal preserves native registered-call evidence");
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
      const auto failed=waiting.Acquire(Request(101,98),Hazard(34,98),rejected,c::MutexClock::now()+5s);
      Check(failed.code==C::synchronization_failed,
            "native parking error cannot become grant");
      Check(failed.wait.registered,"retained native failure preserves registered-call evidence");
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
      Check(result.wait.registered,"retained delivery preserves actual native wait sample");
      Check(grant.Release(Request(101,98)).code==C::released,"late delivered grant has real release path after fence");
    });
    Check(probe.entered.try_acquire_for(5s),"late delivery waiter actually parked");
    Check(holder.Release(Request()).code==C::released,"release to pending retained task");
    op.Reset();
    Check(probe.committed.try_acquire_for(5s),"pause after native grant before callback join and retained delivery");
    Check(owner.Close(Id(24)) && owner.FenceAdmission()==S::ok,"close and fence delivery gap");
    stop.request_stop();
    const auto snapshot=owner.Snapshot();
    Check(snapshot.native.registered_waits==1,"retained snapshot counts registration before result delivery");
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
  LatchOrderPairs(); LatchOrderBeforePark(); LatchOrderOwnership(); Descriptors(); IdentityAndModes(); LifetimeAndExhaustion(); RequestValidationAndTerminals(); Publication();
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
