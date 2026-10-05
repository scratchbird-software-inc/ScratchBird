// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_storage_memory.hpp"
#include "disk_device.hpp"
#include <algorithm>
#include <array>
#include <barrier>
#include <condition_variable>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
thread_local long fail_after=-1;
thread_local bool failure_hit=false;
thread_local unsigned long allocations=0;
unsigned checks=0;
struct BlockPhysicalAllocation {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered=false,release=false,returned=false;
};
thread_local BlockPhysicalAllocation* block_physical=nullptr;
namespace m=scratchbird::core::memory;
namespace db=scratchbird::storage::database;
namespace d=scratchbird::storage::disk;
using namespace scratchbird::core::platform;
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
    request.memory_class="page_buffer";request.route_label="storage.arena.conformance";
    request.purpose="storage arena conformance";request.binary_operation_uuid=binding.operation_uuid.bytes;
    request.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
    request.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
    request.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
    request.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(5).bytes},
      {m::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
    request.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    request.provenance.source_label="actual arena fixture";
    for(const auto& scope:request.scope_chain){m::HierarchicalMemoryBudget b;
      b.scope=scope;b.hard_limit_bytes=bytes;b.provenance=request.provenance;
      Check(ledger.SetBudget(b).ok(),"actual parent budget");}
  }
  auto Grant(){auto r=m::AcquireReservationBackedMemoryResource(request);Check(r.ok(),"real grant");return std::move(r.resource);}
  auto Memory(){auto grant=Grant();auto r=db::AdoptNativeStorageMemory(binding,grant);Check(r.ok()&&!grant,"real adoption");return std::move(r.memory);}
  void Empty(){Check(!ledger.Snapshot().current_bytes,"parent charge leak");const auto s=manager.Snapshot();
    Check(!s.current_bytes&&!s.reserved_capacity_bytes&&!s.active_capacity_reservation_count,"physical charge leak");}
};
struct File {
  std::filesystem::path directory,path;
  File(){char name[]="/tmp/sb-native-arena-XXXXXX";const auto* p=mkdtemp(name);if(!p)throw std::runtime_error("mkdtemp");directory=p;path=directory/"payload.bin";}
  ~File(){std::error_code ec;std::filesystem::remove_all(directory,ec);}
};
m::MemoryPolicyBinding ActivatePolicy(Fixture& f,u64 expected,u64 limit){
  m::MemoryLimitReduction request;
  request.policy_uuid=Id(static_cast<byte>(70+expected)).bytes;
  request.expected_generation=expected;
  request.hard_limit_bytes=request.soft_limit_bytes=request.per_context_limit_bytes=
      request.page_buffer_pool_limit_bytes=limit;
  request.existing_grants=m::MemoryExistingGrantRule::reject_change;
  const auto result=f.manager.allocator()->ReduceLimits(request);
  Check(result.ok()&&result.generation==expected+1,"actual generation activation");
  return f.manager.allocator()->PolicyBinding();
}
void Backends(){
  for(bool shared:{false,true}){
    Fixture f(8192);std::shared_ptr<m::ReservationBackedMemoryResource> resource;
    const auto original=ActivatePolicy(f,0,2097152);
    if(shared)resource=f.Grant();
    m::MemoryTag tag;tag.category=m::MemoryCategory::page_buffer;tag.purpose="legacy arena conformance";
    auto arena=shared?m::ArenaAllocator(resource):f.manager.CreateArena(tag);
    Check(arena.ReserveBacking(256,64).ok(),"exact first backing");
    const auto current=ActivatePolicy(f,1,1048576);
    auto cap=arena.CapacitySnapshot();Check(cap.retained_bytes==256&&!cap.consumed_bytes&&cap.chunk_count==1,"preallocation consumes no payload");
    auto first=arena.AllocateWithinCapacity(1,64,0);Check(first.ok()&&reinterpret_cast<std::uintptr_t>(first.pointer)%64==0,"first aligned byte");
    Check(first.policy_binding==original,"old backing keeps its exact original policy generation");
    *static_cast<byte*>(first.pointer)=0xab;
    const auto before=allocations;
    auto second=arena.AllocateWithinCapacity(128,128,0);
    Check(second.ok()&&reinterpret_cast<std::uintptr_t>(second.pointer)%128==0,"padding alignment within backing");
    Check(allocations==before,"no control allocation on existing-backing path");
    Check(second.policy_binding==original,"zero-growth receipt does not consult current governor generation");
    const auto used=arena.CapacitySnapshot().consumed_bytes;
    Check(used==192||used==256,"padding is charged from actual address");
    auto denied=arena.AllocateWithinCapacity(256,0,0);
    Check(!denied.ok()&&denied.status.code==StatusCode::memory_limit_exceeded&&arena.CapacitySnapshot().consumed_bytes==used,"no-fit preserves cursor");
    for(usize alignment:{usize(3),usize(7)}){
      Check(!arena.ReserveBacking(128,alignment).ok(),"invalid backing alignment");
      Check(!arena.AllocateWithinCapacity(1,alignment,0).ok(),"invalid bump alignment");
    }
    Check(!arena.ReserveBacking(0).ok()&&!arena.AllocateWithinCapacity(0,0,0).ok(),"zero request refused");
    Check(!arena.ReserveBacking(std::numeric_limits<usize>::max()).ok(),"backing sum overflow refused");
    Check(arena.CapacitySnapshot().retained_bytes==256&&arena.CapacitySnapshot().consumed_bytes==used,"invalid requests preserve chunks/cursor");
    Check(arena.ReserveBacking(256,256).ok()&&arena.CapacitySnapshot().retained_bytes==512,"exact second chunk");
    auto third=arena.AllocateWithinCapacity(256,256,0);Check(third.ok(),"full second chunk consumed");
    const auto expected=shared?original:current;
    Check(third.policy_binding==expected,"new chunk inherits actual admission or original capacity grant");
    Check(f.manager.Snapshot().current_bytes==512,"actual physical retained bytes");
    if(shared)Check(resource->Snapshot().allocated_bytes==512&&f.ledger.Snapshot().current_bytes==8192,"actual full parent grant retained");
    m::ArenaAllocator moved(std::move(arena));
    Check(!arena.CapacitySnapshot().retained_bytes&&moved.CapacitySnapshot().retained_bytes==512&&*static_cast<byte*>(first.pointer)==0xab,"move preserves real data");
    arena=std::move(moved);Check(!moved.CapacitySnapshot().retained_bytes,"move assignment sole owner");
    auto grown=arena.AllocateWithinCapacity(512,64,512);
    Check(grown.ok()&&grown.policy_binding==expected,"growth return propagates backing receipt after move");
    fail_after=0;auto reset=arena.ResetNoAlloc();const auto remaining=fail_after;fail_after=-1;
    Check(reset.ok()&&remaining==0&&!arena.CapacitySnapshot().retained_bytes&&!f.manager.Snapshot().current_bytes,"allocation-free actual reset");
    Check(arena.Reset().ok(),"idempotent reset");
    if(shared){Check(resource->Snapshot().allocated_bytes==0,"reset leaves grant but no live payload");
      Check(resource->ReleaseNoAlloc().ok(),"release exhausted-use grant");}
    f.Empty();
  }
  m::ArenaAllocator null(std::shared_ptr<m::ReservationBackedMemoryResource>{});
  Check(!null.ReserveBacking(1).ok()&&!null.Allocate(1).ok()&&null.ResetNoAlloc().ok(),"unbound arena refuses without fake backing");
}
void GrantAndLifetime(){
  Fixture f(512);std::shared_ptr<m::ReservationBackedMemoryResource> resource=f.Grant();
  {
    m::ArenaAllocator arena(resource),other(resource);
    Check(arena.ReserveBacking(256).ok()&&other.ReserveBacking(256).ok(),"two consumers share actual grant");
    auto denied=arena.ReserveBacking(1);Check(!denied.ok()&&!denied.diagnostic.diagnostic_code.empty(),"short cumulative grant retains actual diagnostic");
    Check(arena.CapacitySnapshot().retained_bytes==256&&resource->Snapshot().allocated_bytes==512,"refusal keeps prior owned chunks");
    auto revoked=f.ledger.CleanupOwner(f.binding.owner_uuid.bytes);Check(revoked.retained_bytes==512,"revocation retains admitted payload");
    Check(!arena.ReserveBacking(1).ok(),"revoked grant refuses new backing");
    fail_after=0;auto retained=arena.AllocateWithinCapacity(256,0,0);const auto remaining=fail_after;fail_after=-1;
    Check(retained.ok()&&remaining==0,"existing admitted bytes survive revocation without governor calls");
    *static_cast<byte*>(retained.pointer)=0x7f;
    Check(other.ResetNoAlloc().ok()&&resource->Snapshot().allocated_bytes==256,"one arena cannot release sibling backing");
    resource.reset();Check(f.manager.Snapshot().current_bytes==256,"arena retains real shared provider after caller exit");
    std::thread worker([arena=std::move(arena)]()mutable{
      fail_after=0;{auto final=std::move(arena);}if(fail_after!=0)std::abort();fail_after=-1;
    });worker.join();
  }
  f.Empty();
}
void NoGovernorReentry(){
  Fixture f(8192);std::shared_ptr<m::ReservationBackedMemoryResource> resource=f.Grant();
  {
    m::ArenaAllocator arena(resource);Check(arena.ReserveBacking(4096).ok(),"pre-admit arena before competing physical allocation");
    const auto original=f.manager.allocator()->PolicyBinding();
    BlockPhysicalAllocation block;m::AllocationResult competing;
    std::thread worker([&]{block_physical=&block;competing=resource->Allocate({4096,4096,{}});block_physical=nullptr;});
    {
      std::unique_lock lock(block.mutex);
      const bool entered=block.changed.wait_for(lock,std::chrono::seconds(10),[&]{return block.entered;});
      if(!entered){block.release=true;block.changed.notify_all();lock.unlock();worker.join();Check(false,"competitor reached actual allocator hook");}
    }
    // The competitor currently owns BOTH the actual resource and physical
    // allocator mutexes. A callback to either would block until the watchdog.
    fail_after=0;auto value=arena.AllocateWithinCapacity(128,64,0);const auto remaining=fail_after;fail_after=-1;
    bool before_return;
    {std::lock_guard lock(block.mutex);before_return=!block.returned;block.release=true;block.changed.notify_all();}
    worker.join();
    Check(value.ok()&&remaining==0&&before_return&&competing.ok(),"zero-growth allocation does not reenter either real governor lock");
    Check(value.policy_binding==original,"fenced receipt preserves backing binding without governor reentry");
    Check(resource->DeallocateNoAlloc(competing.pointer,competing.bytes,competing.alignment).ok(),"release competing physical payload");
    auto replacement=m::ArenaAllocator(resource);Check(replacement.ReserveBacking(256).ok(),"populated move destination");
    fail_after=0;replacement=std::move(arena);const auto move_remaining=fail_after;fail_after=-1;
    Check(move_remaining==0&&!arena.CapacitySnapshot().retained_bytes&&replacement.CapacitySnapshot().retained_bytes==4096,
      "populated move assignment retires only old backing with no allocation");
    Check(resource->Snapshot().allocated_bytes==4096,"move preserves exact retained grant charge");
  }
  resource.reset();f.Empty();
}
void NativeFiles(){
  for(const auto& profile:d::kCanonicalFilespacePageProfiles){
    Fixture f(profile.page_size_bytes);const auto original=ActivatePolicy(f,0,2097152);auto memory=f.Memory();
    auto result=memory.CreateArena(f.binding,profile.page_size_bytes,profile.page_size_bytes);
    Check(result.ok()&&result.backing.bytes==profile.page_size_bytes,"native exact page backing");
    Check(result.arena.binding().operation_uuid==f.binding.operation_uuid,"binary operation retained");
    (void)ActivatePolicy(f,1,1048576);
    File file;d::FileDevice device;Check(device.Open(file.path.string(),d::FileOpenMode::create_new).ok(),"open real file");
    m::AllocationResult payload;
    {
      auto guard=device.AcquireOperationGuard();fail_after=0;
      payload=result.arena.Allocate(profile.page_size_bytes,profile.page_size_bytes);
      const auto remaining=fail_after;fail_after=-1;
      Check(payload.ok()&&remaining==0,"fenced consumption uses only preadmitted shared backing");
      Check(payload.policy_binding==original,"real-file page profile retains original backing generation");
      auto* data=static_cast<byte*>(payload.pointer);
      for(usize n=0;n<payload.bytes;++n)data[n]=byte((n*17+23)%251);
      const auto io=device.WriteAt(0,data,payload.bytes);Check(io.ok()&&io.bytes_transferred==payload.bytes,"write actual arena payload");
    }
    Check(device.Sync().ok()&&device.Close().ok()&&device.Open(file.path.string(),d::FileOpenMode::open_existing).ok(),"cold reopen");
    std::memset(payload.pointer,0,payload.bytes);
    auto io=device.ReadAt(0,payload.pointer,payload.bytes);Check(io.ok()&&io.bytes_transferred==payload.bytes,"read actual arena buffer");
    for(usize n=0;n<payload.bytes;++n)if(static_cast<byte*>(payload.pointer)[n]!=byte((n*17+23)%251))throw std::runtime_error("independent file oracle");
    Check(!result.arena.Allocate(1).ok(),"native consumer cannot grow past admitted backing");
    const auto revoked=f.ledger.CleanupOwner(f.binding.owner_uuid.bytes);Check(revoked.retained_bytes==profile.page_size_bytes,"live native backing retained on revoke");
    Check(!memory.CreateArena(f.binding,1).ok(),"new native operation cannot use revoked credit");
    memory={};Check(f.manager.Snapshot().current_bytes==profile.page_size_bytes,"native arena outlives workspace");
    fail_after=0;result.arena={};const auto remaining=fail_after;fail_after=-1;
    Check(remaining==0,"native final cleanup has no allocation");f.Empty();Check(device.Close().ok(),"close fixture");
  }
}
void NativeRefusals(){
  Fixture f(256);auto memory=f.Memory();
  for(unsigned field=0;field<4;++field){auto wrong=f.binding;
    std::array<Uuid*,4> fields{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};*fields[field]=Id(90);
    auto refused=memory.CreateArena(wrong,256);Check(!refused.ok()&&refused.error==db::NativeStorageMemoryError::invalid_binding,"all binary dimensions before admission");
    Check(!memory.Snapshot().allocation_count&&!f.manager.Snapshot().current_bytes,"wrong owner cannot allocate");
  }
  auto short_grant=memory.CreateArena(f.binding,257);Check(!short_grant.ok()&&!short_grant.arena,"short grant publishes no usable owner");
  Check(!memory.CreateArena(f.binding,0).ok()&&!memory.CreateArena(f.binding,1,3).ok(),"native invalid inputs refused");
  auto correct=memory.CreateArena(f.binding,256);Check(correct.ok(),"same-owner retry after refusal");
  correct.arena={};memory={};f.Empty();
}
void Faults(){
  unsigned fault_count=0;
  for(unsigned mode=0;mode<2;++mode){bool ended=false;
    for(long point=0;point<512;++point){Fixture f(8192);auto memory=f.Memory();
      std::shared_ptr<m::ReservationBackedMemoryResource> resource;
      std::unique_ptr<m::ArenaAllocator> arena;
      if(mode){memory={};resource=f.Grant();arena=std::make_unique<m::ArenaAllocator>(resource);Check(arena->ReserveBacking(128).ok(),"existing chunk for failed append");}
      failure_hit=false;fail_after=point;
      if(!mode){auto result=memory.CreateArena(f.binding,4096,4096);fail_after=-1;
        Check(result.ok()||result.error==db::NativeStorageMemoryError::resource_exhausted,"native every allocation failure typed");
      }else{auto result=arena->ReserveBacking(4096,4096);fail_after=-1;
        Check(result.ok()||result.status.code==StatusCode::memory_allocation_failed,"shared append every allocation failure typed");
        Check(arena->CapacitySnapshot().retained_bytes==(result.ok()?4224:128),"failed append never removes prior chunk or leaks new backing");}
      fail_after=-1;const bool hit=failure_hit;fault_count+=hit;
      arena.reset();resource.reset();memory={};f.Empty();if(!hit){ended=true;break;}
    }Check(ended,"sweep reached unfaulted completion");
  }
  Check(fault_count>0,"measured failure points actually executed");
  std::cout<<"arena allocation faults="<<fault_count<<'\n';
}
void ConcurrentArenas(){
  Fixture f(4096);auto memory=f.Memory();std::barrier start(9),held(9),release(9);
  std::array<bool,8> admitted{};std::array<std::thread,8> workers;
  for(unsigned n=0;n<8;++n)workers[n]=std::thread([&,n]{start.arrive_and_wait();auto r=memory.CreateArena(f.binding,4096);
    admitted[n]=r.ok();held.arrive_and_wait();release.arrive_and_wait();});
  start.arrive_and_wait();held.arrive_and_wait();const auto count=std::count(admitted.begin(),admitted.end(),true);
  const auto actual=f.manager.Snapshot().current_bytes;release.arrive_and_wait();for(auto& worker:workers)worker.join();
  Check(count==1&&actual==4096&&!f.manager.Snapshot().current_bytes,"competing arenas share one true grant");memory={};f.Empty();
}
void AllocationPoint(){++allocations;if(fail_after>=0&&fail_after--==0){failure_hit=true;fail_after=-1;throw std::bad_alloc();}}
}
void* operator new(std::size_t n){AllocationPoint();if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void* operator new(std::size_t n,std::align_val_t alignment){
  AllocationPoint();
  if(block_physical){std::unique_lock lock(block_physical->mutex);block_physical->entered=true;block_physical->changed.notify_all();
    block_physical->changed.wait_for(lock,std::chrono::seconds(10),[&]{return block_physical->release;});block_physical->returned=true;}
  void* p=nullptr;if(posix_memalign(&p,static_cast<std::size_t>(alignment),n?n:1)==0)return p;throw std::bad_alloc();}
void operator delete(void* p,std::align_val_t)noexcept{std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t)noexcept{std::free(p);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
int main(){try{Backends();GrantAndLifetime();NoGovernorReentry();NativeFiles();NativeRefusals();Faults();ConcurrentArenas();
  std::cout<<"PASS native shared-grant arenas checks="<<checks<<" profiles=5\n";return 0;
}catch(const std::exception& e){fail_after=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
