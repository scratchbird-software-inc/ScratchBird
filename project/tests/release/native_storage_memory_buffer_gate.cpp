// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_storage_memory.hpp"
#include "disk_device.hpp"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
thread_local long fail_after=-1;
thread_local bool failure_hit=false;
unsigned checks=0;
namespace m=scratchbird::core::memory;
namespace db=scratchbird::storage::database;
namespace d=scratchbird::storage::disk;
using namespace scratchbird::core::platform;
using E=db::NativeStorageMemoryError;
void Check(bool good,const char* why){++checks;if(!good)throw std::runtime_error(why);}
Uuid Id(byte n){Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=n;return id;}
struct Fixture {
  m::MemoryManager manager;
  m::HierarchicalMemoryBudgetLedger ledger{3,5};
  m::ReservationBackedMemoryResourceRequest request;
  db::NativeStorageMemoryBinding binding{Id(4),Id(1),Id(3),Id(2)};
  static auto Policy(){auto p=m::DefaultLocalEngineMemoryPolicy();p.hard_limit_bytes=2097152;p.per_context_limit_bytes=2097152;return p;}
  explicit Fixture(u64 bytes):manager(Policy()){
    request.memory_manager=&manager;request.reservation_ledger=&ledger;
    request.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
    request.category=m::MemoryCategory::page_buffer;request.requested_bytes=bytes;
    request.memory_class="page_buffer";request.route_label="storage.buffer.conformance";
    request.purpose="storage buffer conformance";request.binary_operation_uuid=binding.operation_uuid.bytes;
    request.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
    request.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
    request.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
    request.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(5).bytes},
      {m::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
    request.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    request.provenance.source_label="actual buffer fixture";
    for(const auto& scope:request.scope_chain){m::HierarchicalMemoryBudget b;
      b.scope=scope;b.hard_limit_bytes=bytes;b.provenance=request.provenance;
      Check(ledger.SetBudget(b).ok(),"actual node parent budget");}
  }
  auto Grant(){auto r=m::AcquireReservationBackedMemoryResource(request);Check(r.ok(),"real grant");return std::move(r.resource);}
  auto Memory(){auto grant=Grant();auto r=db::AdoptNativeStorageMemory(binding,grant);Check(r.ok()&&!grant,"actual grant adoption transfers ownership");return std::move(r.memory);}
  void Empty(){Check(!ledger.Snapshot().current_bytes,"hierarchical charge leak");const auto s=manager.Snapshot();
    Check(!s.current_bytes&&!s.reserved_capacity_bytes&&!s.active_capacity_reservation_count,"physical charge leak");}
};
struct File {
  std::filesystem::path directory,path;
  File(){char name[]="/tmp/sb-native-memory-XXXXXX";const auto* created=mkdtemp(name);if(!created)throw std::runtime_error("mkdtemp");directory=created;path=directory/"payload.bin";}
  ~File(){std::error_code ec;std::filesystem::remove_all(directory,ec);}
};
void ProfilesAndLifetime(){
  for(const auto& a:d::kCanonicalFilespacePageProfiles)for(const auto& b:d::kCanonicalFilespacePageProfiles){
    const u64 bytes=u64(a.page_size_bytes)+b.page_size_bytes;Fixture f(bytes);
    auto memory=f.Memory();auto first=memory.AllocatePage(a.uuid),second=memory.AllocatePage(b.uuid);
    Check(first.ok()&&second.ok(),"all ordered page pairs allocate real payloads");
    Check(first.buffer.size()==a.page_size_bytes&&second.buffer.size()==b.page_size_bytes,"exact profile sizes");
    Check(reinterpret_cast<std::uintptr_t>(first.buffer.data())%a.page_size_bytes==0&&
      reinterpret_cast<std::uintptr_t>(second.buffer.data())%b.page_size_bytes==0,"profile-aligned physical buffers");
    Check(std::all_of(first.buffer.data(),first.buffer.data()+first.buffer.size(),[](byte n){return n==0;}),"new payload zero initialized");
    Check(f.manager.Snapshot().current_bytes==bytes&&memory.Snapshot().allocated_bytes==bytes,"actual simultaneous bytes charged");
    const auto full=memory.AllocatePage(a.uuid);Check(!full.ok()&&full.error==E::resource_exhausted,"real cumulative grant prevents overrun");
    for(usize i=0;i<first.buffer.size();++i)first.buffer.data()[i]=static_cast<byte>((i*13+7)%251);
    File file;d::FileDevice device;Check(device.Open(file.path.string(),d::FileOpenMode::create_new).ok(),"open actual owned fixture");
    auto io=device.WriteAt(0,first.buffer.data(),first.buffer.size());Check(io.ok()&&io.bytes_transferred==first.buffer.size(),"actual governed buffer write");
    Check(device.Sync().ok()&&device.Close().ok()&&device.Open(file.path.string(),d::FileOpenMode::open_existing).ok(),"sync and cold device reopen");
    std::memset(first.buffer.data(),0,first.buffer.size());io=device.ReadAt(0,first.buffer.data(),first.buffer.size());
    Check(io.ok()&&io.bytes_transferred==first.buffer.size(),"actual governed readback");
    for(usize i=0;i<first.buffer.size();++i)if(first.buffer.data()[i]!=static_cast<byte>((i*13+7)%251))throw std::runtime_error("independent byte oracle");
    const auto revoked=f.ledger.CleanupOwner(f.binding.owner_uuid.bytes);
    Check(!revoked.ok()&&revoked.retained_bytes==bytes,"revoked parent retains full grant");
    Check(!memory.AllocatePage(a.uuid).ok(),"no allocation after revocation");
    memory={};Check(f.manager.Snapshot().current_bytes==bytes,"buffers retain ownership after workspace exit");
    db::NativeStorageBuffer moved(std::move(first.buffer));Check(!first.buffer&&moved.size()==a.page_size_bytes,"move transfers sole payload ownership");
    moved=std::move(second.buffer);Check(!second.buffer&&moved.size()==b.page_size_bytes&&f.manager.Snapshot().current_bytes==b.page_size_bytes,"move assignment retires old payload exactly once");
    fail_after=0;const auto released=moved.Reset();const auto allocations_left=fail_after;fail_after=-1;
    Check(released.ok()&&!moved&&allocations_left==0&&moved.Reset().ok(),"explicit idempotent cleanup does not allocate");
    f.Empty();Check(device.Close().ok(),"close owned file");
  }
}
void Refusals(){
  const auto& p=d::kCanonicalFilespacePageProfiles[0];
  for(unsigned mode=0;mode<10;++mode){Fixture f(p.page_size_bytes);auto binding=f.binding;
    if(mode==0)binding.database_uuid=Id(90);if(mode==1)binding.operation_uuid=Id(90);
    if(mode==2)binding.owner_uuid=Id(90);if(mode==3)binding.context_uuid=Id(90);
    if(mode==4)binding.database_uuid={};if(mode==5)binding.operation_uuid.bytes[6]=0x40;
    if(mode==6)f.request.category=m::MemoryCategory::core_runtime;
    if(mode==7)f.request.consumer_kind=m::ReservationBackedMemoryConsumerKind::executor_operator;
    auto grant=f.Grant();
    if(mode==8){const auto r=f.ledger.Cancel(grant->reservation_token());Check(r.retained,"revoked unused grant retains owner");}
    void* payload=nullptr;
    if(mode==9){const auto allocated=grant->Allocate({1,0,"preexisting allocation"});Check(allocated.ok(),"preused grant fixture");payload=allocated.pointer;*static_cast<byte*>(payload)=0x5a;}
    const auto refused=db::AdoptNativeStorageMemory(binding,grant);
    Check(!refused.ok()&&refused.error==(mode==4||mode==5?E::invalid_binding:E::invalid_grant),"exact binary/scope/type/live/unused grant admission");
    Check(grant&&f.ledger.Snapshot().current_bytes==p.page_size_bytes,"failed adoption leaves original caller grant charged and owned");
    if(payload)Check(f.manager.Snapshot().current_bytes==1&&*static_cast<byte*>(payload)==0x5a,"rejected preused grant never frees a caller's live payload");
    grant.reset();f.Empty();
  }
  Fixture f(p.page_size_bytes-1);auto memory=f.Memory();
  Check(memory.Matches(f.binding)&&memory.CheckBinding(f.binding)==E::none,"exact full native memory binding");
  for(unsigned field=0;field<4;++field)for(unsigned variant=0;variant<3;++variant){
    auto wrong=f.binding;
    const std::array<Uuid*,4> fields{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};
    if(variant==0)*fields[field]=Id(99);
    if(variant==1)*fields[field]={};
    if(variant==2)fields[field]->bytes[6]=0x40;
    const auto physical=f.manager.Snapshot();const auto grant=memory.Snapshot();
    fail_after=0;const auto result=memory.CheckBinding(wrong);const auto remaining=fail_after;fail_after=-1;
    Check(result==E::invalid_binding&&remaining==0&&!memory.Matches(wrong),"all four mismatched or invalid native identities refuse without allocation");
    Check(memory.Matches(f.binding)&&memory.Snapshot().allocated_bytes==grant.allocated_bytes&&
      memory.Snapshot().reserved_bytes==grant.reserved_bytes&&
      f.manager.Snapshot().allocation_count==physical.allocation_count,"foreign check leaves original resource owner unchanged");
  }
  auto bad=memory.AllocatePage(Id(99));Check(!bad.ok()&&bad.error==E::invalid_profile&&!f.manager.Snapshot().current_bytes,"unknown profile no payload");
  bad=memory.AllocatePage(p.uuid);Check(!bad.ok()&&bad.error==E::resource_exhausted&&!f.manager.Snapshot().current_bytes,"one byte short cannot allocate");
  const auto revoked=f.ledger.CleanupOwner(f.binding.owner_uuid.bytes);
  Check(revoked.retained_bytes==p.page_size_bytes-1&&memory.CheckBinding(f.binding)==E::invalid_grant&&
    !memory.Matches(f.binding),"matching full binding cannot revive a revoked grant");
  memory={};f.Empty();
  Check(memory.CheckBinding(f.binding)==E::invalid_grant&&!memory.Matches(f.binding),"released workspace has no binding authority");
  std::unique_ptr<m::ReservationBackedMemoryResource> missing;
  auto none=db::AdoptNativeStorageMemory(f.binding,missing);Check(!none.ok(),"no fabricated grant");
}
void Faults(){
  const auto& p=d::kCanonicalFilespacePageProfiles[0];
  for(unsigned phase=0;phase<2;++phase){bool end=false;unsigned faults=0;
    for(long point=0;point<1024;++point){Fixture f(p.page_size_bytes);auto grant=f.Grant();
      db::NativeStorageMemory memory;if(phase){auto r=db::AdoptNativeStorageMemory(f.binding,grant);Check(r.ok()&&!grant,"fault fixture adoption");memory=std::move(r.memory);}
      failure_hit=false;fail_after=point;
      if(!phase){auto r=db::AdoptNativeStorageMemory(f.binding,grant);fail_after=-1;
        if(r.ok()){Check(!grant,"successful adoption consumes caller grant");memory=std::move(r.memory);}
        else Check(r.error==E::resource_exhausted&&grant&&f.ledger.Snapshot().current_bytes==p.page_size_bytes,"adoption allocation failure leaves original owner intact");}
      else{auto r=memory.AllocatePage(p.uuid);fail_after=-1;Check(r.ok()||r.error==E::resource_exhausted,"buffer allocation failure typed");}
      fail_after=-1;const bool hit=failure_hit;faults+=hit;memory={};grant.reset();f.Empty();
      if(!hit){end=true;break;}
    }Check(end&&faults,"every measured allocation site faulted");
  }
}
void ConcurrentBuffers(){
  const auto& p=d::kCanonicalFilespacePageProfiles[0];Fixture f(p.page_size_bytes);auto memory=f.Memory();
  std::barrier start(9),held(9),release(9);std::array<bool,8> admitted{};std::array<std::thread,8> threads;
  for(unsigned i=0;i<8;++i)threads[i]=std::thread([&,i]{start.arrive_and_wait();auto r=memory.AllocatePage(p.uuid);
    admitted[i]=r.ok();held.arrive_and_wait();release.arrive_and_wait();});
  start.arrive_and_wait();held.arrive_and_wait();const auto count=std::count(admitted.begin(),admitted.end(),true);
  const auto actual=f.manager.Snapshot().current_bytes;release.arrive_and_wait();for(auto& t:threads)t.join();
  Check(count==1&&actual==p.page_size_bytes&&!f.manager.Snapshot().current_bytes,"concurrent payloads share one actual grant");memory={};f.Empty();
}
}
void* operator new(std::size_t n){if(fail_after>=0&&fail_after--==0){failure_hit=true;fail_after=-1;throw std::bad_alloc();}if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void* operator new(std::size_t n,std::align_val_t alignment){
  if(fail_after>=0&&fail_after--==0){failure_hit=true;fail_after=-1;throw std::bad_alloc();}
  void* p=nullptr;if(posix_memalign(&p,static_cast<std::size_t>(alignment),n?n:1)==0)return p;throw std::bad_alloc();}
void operator delete(void* p,std::align_val_t)noexcept{std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t)noexcept{std::free(p);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
int main(){try{ProfilesAndLifetime();Refusals();Faults();ConcurrentBuffers();std::cout<<"PASS native storage buffers checks="<<checks<<" profile_pairs=25\n";return 0;}
catch(const std::exception& e){fail_after=-1;std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
