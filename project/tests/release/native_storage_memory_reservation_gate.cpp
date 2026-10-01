// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "reservation_backed_memory_resource.hpp"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
thread_local long fail_after=-1;
thread_local bool failure_hit=false;
unsigned checks=0;
namespace m=scratchbird::core::memory;
void Check(bool good,const char* why){++checks;if(!good)throw std::runtime_error(why);}
m::MemoryBinaryUuid Id(unsigned n){m::MemoryBinaryUuid id{};id[0]=1;id[6]=0x70;id[8]=0x80;id[15]=n;return id;}
struct Fixture {
  m::MemoryManager manager;
  m::HierarchicalMemoryBudgetLedger ledger{3,5};
  m::ReservationBackedMemoryResourceRequest request;
  static auto Policy(){auto p=m::DefaultLocalEngineMemoryPolicy();p.hard_limit_bytes=8192;p.per_context_limit_bytes=8192;return p;}
  Fixture():manager(Policy()){
    request.memory_manager=&manager;request.reservation_ledger=&ledger;
    request.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
    request.category=m::MemoryCategory::page_buffer;request.requested_bytes=4096;
    request.memory_class="page_buffer";request.route_label="storage.memory.conformance";
    request.purpose="storage work buffers";request.binary_operation_uuid=Id(1);
    request.binary_ownership[m::MemoryBinaryScopeKind::context]=Id(2);
    request.binary_ownership[m::MemoryBinaryScopeKind::owner]=Id(3);
    request.binary_ownership[m::MemoryBinaryScopeKind::database]=Id(4);
    request.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(5)},
      {m::HierarchicalMemoryScopeKind::database,{},Id(4)}};
    request.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    request.provenance.source_label="storage memory component fixture";
    for(const auto& scope:request.scope_chain){m::HierarchicalMemoryBudget b;
      b.scope=scope;b.hard_limit_bytes=4096;b.provenance=request.provenance;
      Check(ledger.SetBudget(b).ok(),"configure actual shared parent limit");}
  }
  void Empty(){Check(ledger.Snapshot().current_bytes==0,"parent charge leaked");
    const auto p=manager.Snapshot();Check(!p.current_bytes&&!p.reserved_capacity_bytes&&!p.active_capacity_reservation_count,"physical charge leaked");}
};
void PositiveAndRevocation(){
  for(unsigned mode=0;mode<3;++mode){Fixture f;auto got=m::AcquireReservationBackedMemoryResource(f.request);
    Check(got.ok()&&got.binary_operation_uuid==Id(1),"actual native grant and binary receipt");
    Check(got.resource->request().operation_id.empty()&&got.resource->request().binary_operation_uuid==Id(1),"native operation never stored as text");
    auto snapshot=got.resource->Snapshot();Check(snapshot.operation_id.empty()&&snapshot.binary_operation_uuid==Id(1)&&snapshot.reserved_bytes==4096,"exact native snapshot");
    auto second=f.request;second.requested_bytes=1;second.binary_operation_uuid=Id(6);
    const auto denied=m::AcquireReservationBackedMemoryResource(second);
    Check(!denied.ok()&&denied.binary_operation_uuid==Id(6)&&f.ledger.Snapshot().current_bytes==4096,"shared parent blocks overcommit and preserves refused identity");
    const auto block=got.resource->Allocate({256,64,"real page staging"});Check(block.ok(),"allocate actual governed payload");
    std::memset(block.pointer,0x5a,256);
    if(mode){
      if(mode==1){const auto revoked=f.ledger.Cancel(got.resource->reservation_token());
        Check(!revoked.ok()&&revoked.retained&&revoked.newly_revoked&&revoked.retained_bytes==4096,"cancel reports actual retained owner, not completed cleanup");}
      else{const auto revoked=f.ledger.CleanupOwner(Id(3));
        Check(!revoked.ok()&&revoked.revoked_reservation_count==1&&revoked.retained_bytes==4096&&!revoked.cleaned_bytes,"owner cleanup retains in-use grant");}
      Check(!got.resource->Allocate({1,0,"after revocation"}).ok(),"revoked grant cannot allocate");
      Check(f.ledger.Snapshot().current_bytes==4096&&f.manager.Snapshot().current_bytes==256&&static_cast<unsigned char*>(block.pointer)[255]==0x5a,"revocation preserves live payload and accounting");}
    Check(got.resource->DeallocateNoAlloc(block.pointer,256,64).ok(),"release actual allocation");
    const auto released=got.resource->Release();Check(released.ok()&&released.snapshot.binary_operation_uuid==Id(1)&&released.snapshot.operation_id.empty(),"release retains binary operation");f.Empty();
  }
}
void InvalidIdentities(){
  Fixture f;
  for(unsigned mode=0;mode<5;++mode){auto request=f.request;
    if(mode==0)request.binary_operation_uuid={};
    if(mode==1)request.binary_operation_uuid[6]=0x40;
    if(mode==2)request.binary_operation_uuid[8]=0;
    if(mode==3)request.operation_id="forbidden mixed operation";
    if(mode==4){request.binary_ownership={};request.owner_id="legacy owner";}
    const auto expected=request.binary_operation_uuid;auto result=m::AcquireReservationBackedMemoryResource(request);
    Check(!result.ok()&&result.binary_operation_uuid==expected&&result.diagnostic.diagnostic_code=="SB_CEIC_012_MEMORY_RESOURCE.IDENTITY_REQUIRED","typed identity refusal before effects");f.Empty();
  }
  m::HierarchicalMemoryBudgetLedger unconfigured(3,5);
  for(unsigned mode=0;mode<3;++mode){auto request=f.request;
    if(mode==0)request.reservation_ledger=nullptr;
    if(mode==1)request.memory_manager=nullptr;
    if(mode==2){request.reservation_ledger=&unconfigured;request.requested_bytes=8193;}
    const auto result=m::AcquireReservationBackedMemoryResource(request);
    Check(!result.ok()&&result.binary_operation_uuid==Id(1),"caller trust flags cannot replace actual services or bypass physical capacity");f.Empty();
    Check(!unconfigured.Snapshot().current_bytes,"unconfigured parent has no invented capacity");
  }
}
void AllocationFailures(){
  bool ended=false;unsigned injected=0;
  for(long point=0;point<4096;++point){Fixture f;auto request=f.request;
    failure_hit=false;fail_after=point;
    auto result=m::AcquireReservationBackedMemoryResource(std::move(request));
    fail_after=-1;const bool hit=failure_hit;injected+=hit;
    Check(result.binary_operation_uuid==Id(1),"failure retains binary operation receipt");
    if(result.ok())Check(result.resource->ReleaseNoAlloc().ok(),"allocation-free grant cleanup");
    f.Empty();if(!hit){ended=true;break;}
  }
  Check(ended&&injected,"all measured acquisition allocation failures reached and recovered");
}
void ConcurrentAdmission(){
  Fixture f;std::barrier start(9),held(9),release(9);std::array<bool,8> admitted{};
  std::array<std::thread,8> threads;
  for(unsigned i=0;i<8;++i)threads[i]=std::thread([&,i]{auto request=f.request;request.binary_operation_uuid=Id(10+i);
    start.arrive_and_wait();auto result=m::AcquireReservationBackedMemoryResource(request);admitted[i]=result.ok();
    held.arrive_and_wait();release.arrive_and_wait();});
  start.arrive_and_wait();held.arrive_and_wait();
  const auto count=std::count(admitted.begin(),admitted.end(),true);const auto bytes=f.ledger.Snapshot().current_bytes;
  release.arrive_and_wait();for(auto& thread:threads)thread.join();
  Check(count==1&&bytes==4096,"concurrent binary requests cannot bypass shared capacity");f.Empty();
}
}
void* operator new(std::size_t n){if(fail_after>=0&&fail_after--==0){failure_hit=true;fail_after=-1;throw std::bad_alloc();}if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
int main(){try{PositiveAndRevocation();InvalidIdentities();AllocationFailures();ConcurrentAdmission();std::cout<<"PASS native storage memory reservation checks="<<checks<<" not_storage_authorization=true\n";return 0;}
catch(const std::exception& e){fail_after=-1;std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
