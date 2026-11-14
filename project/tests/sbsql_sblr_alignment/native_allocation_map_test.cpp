// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_allocation_map.hpp"
#include "allocation_map_inplace_page.hpp"
#include "native_filespace_initialization.hpp"
#include "uuid.hpp"
#include "native_allocation_chain_backing.hpp"
#include "native_metadata_memory.hpp"
#include "disk_device.hpp"
#include "filespace_page_zero.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <atomic>
#include <thread>
#include <type_traits>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <new>
#include <mutex>
#include <source_location>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#ifdef SB_NATIVE_ALLOCATION_MEMORY_TESTS
// (W4d-3b: the single sealed map memory reader was removed with that family.)
#include "native_allocation_chain_memory.hpp"
#include <thread>
#endif

namespace {
long allocation_budget = -1;
bool count_allocations = false;
unsigned long observed_allocations = 0;
unsigned reads = 0, fail_read = 0;
std::uint64_t observed_chain_bytes=0;
unsigned hashes = 0, fail_hash_at = 0, resize_at_read = 0;
off_t resize_to = 0;
bool resized_during_read = false;
unsigned stats = 0, fail_stat = 0;
unsigned long writes = 0, syncs = 0;
bool fail_hash = false;
std::mutex read_pause_mutex;
std::condition_variable read_pause_cv;
bool pause_read = false, read_entered = false, release_read = false;
void* last_read_buffer=nullptr;
unsigned short_read=0,eof_read=0;
#ifdef SB_NATIVE_ALLOCATION_MEMORY_TESTS
std::recursive_mutex* observation_device_mutex=nullptr;
unsigned memory_probes=0,locked_memory_probes=0;
void ProbeObservation(){if(auto* mutex=observation_device_mutex){
  observation_device_mutex=nullptr;bool available=false;
  std::thread worker([&]{available=mutex->try_lock();if(available)mutex->unlock();});worker.join();
  ++memory_probes;if(!available)++locked_memory_probes;observation_device_mutex=mutex;
}}
#endif
}
void* operator new(std::size_t n) {
#ifdef SB_NATIVE_ALLOCATION_MEMORY_TESTS
  ProbeObservation();
#endif
  if (count_allocations) ++observed_allocations;
  if (allocation_budget == 0) { allocation_budget = -1; throw std::bad_alloc(); }
  if (allocation_budget > 0) --allocation_budget;
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept {
#ifdef SB_NATIVE_ALLOCATION_MEMORY_TESTS
  ProbeObservation();
#endif
  std::free(p);
}
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#ifdef SB_NATIVE_ALLOCATION_MEMORY_TESTS
namespace {
std::recursive_mutex* allocation_device_mutex=nullptr;
std::recursive_mutex* deallocation_device_mutex=nullptr;
bool allocation_lock_free=false,deallocation_lock_free=false;
void ProbeCleanup(){if(deallocation_device_mutex){auto* mutex=deallocation_device_mutex;deallocation_device_mutex=nullptr;
  bool available=false;std::thread worker([&]{available=mutex->try_lock();if(available)mutex->unlock();});worker.join();deallocation_lock_free=available;}}
}
void* operator new(std::size_t n,std::align_val_t a){
  ProbeObservation();
  if(count_allocations)++observed_allocations;
  if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;
  if(allocation_device_mutex){auto* mutex=allocation_device_mutex;allocation_device_mutex=nullptr;
    bool available=false;std::thread worker([&]{available=mutex->try_lock();if(available)mutex->unlock();});worker.join();allocation_lock_free=available;}
  void* p=nullptr;if(posix_memalign(&p,static_cast<std::size_t>(a),n?n:1)==0)return p;throw std::bad_alloc();
}
void operator delete(void* p,std::align_val_t)noexcept{ProbeObservation();ProbeCleanup();std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t)noexcept{ProbeObservation();ProbeCleanup();std::free(p);}
#endif
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* out, size_t n, off_t at) {
  ++reads;last_read_buffer=out;
  if (fail_read && reads == fail_read) { errno = EIO; return -1; }
  if(eof_read&&reads==eof_read)return 0;
  if (resize_at_read && reads == resize_at_read) {
    resized_during_read = ::ftruncate(fd, resize_to) == 0;
  }
  {
    std::unique_lock lock(read_pause_mutex);
    if (pause_read) {
      pause_read=false;read_entered=true;read_pause_cv.notify_all();
      read_pause_cv.wait(lock,[]{return release_read;});
    }
  }
  const auto result=__real_pread(fd,out,short_read&&reads==short_read?n-1:n,at);
  if(result>0)observed_chain_bytes+=result;return result;
}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
#ifdef NATIVE_HISTORICAL_IO_FAULTS
extern "C" int __real_fstat(int, struct stat*);
extern "C" int __wrap_fstat(int fd, struct stat* out) {
  ++stats;
  if (fail_stat && stats == fail_stat) { errno=EIO;return -1; }
  return __real_fstat(fd,out);
}
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* bytes, size_t size, off_t at) {
  ++writes;return __real_pwrite(fd,bytes,size,at);
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) { ++syncs;return __real_fsync(fd); }
#endif
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new() {
  ++hashes;
  if (fail_hash_at && hashes == fail_hash_at) return nullptr;
  if (fail_hash) { fail_hash = false; return nullptr; }
  return __real_EVP_MD_CTX_new();
}
#ifdef SB_NATIVE_ALLOCATION_MEMORY_TESTS
namespace {unsigned method_fault=0;}
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* p,size_t s,unsigned char* b,unsigned int* n,const EVP_MD* m,ENGINE* e){
  if(method_fault==1){method_fault=0;return 0;}return __real_EVP_Digest(p,s,b,n,m,e);}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* m,ENGINE* e){
  if(method_fault==2){method_fault=0;return 0;}return __real_EVP_DigestInit_ex(c,m,e);}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* b,size_t n){
  if(method_fault==3){method_fault=0;return 0;}return __real_EVP_DigestUpdate(c,b,n);}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* b,unsigned int* n){
  if(method_fault==4){method_fault=0;return 0;}const auto r=__real_EVP_DigestFinal_ex(c,b,n);
  if(method_fault==5){method_fault=0;*n=31;}return r;}
#endif
// W4d-3b (owner Q28 = 1, 2026-11-08): the sealed allocation map family, its
// codec, its single-map memory reader and the selected and historical chain
// reads were removed, and with them this test's hand-built sealed fixtures and
// their sections (codecs, borrowed codecs, governed single-map memory,
// historical chains). What stays is the shared chain reader the fixed-root
// lease reads through (the bootstrap read: owned, backed and borrowed forms,
// and the governed chain memory), now over a filespace written by the
// production in-place writer for a stand-alone filespace (Q19): two map
// pages, nothing hand-built. The in-place codec is tested in
// native_allocation_map_inplace_test, with the chain forgeries (another
// family, another creator, a link cycle) on production-created databases.
namespace {
namespace p = scratchbird::storage::page;
namespace d = scratchbird::storage::disk;
namespace db = scratchbird::storage::database;
using namespace scratchbird::core::platform;
using E = p::NativeAllocationError;
using Bytes = std::vector<byte>;
std::size_t checks = 0;
void Check(bool pass, const char* message, std::source_location where = std::source_location::current()) {
  ++checks;
  if (!pass) { std::cerr << where.line() << ": " << message << '\n'; throw std::runtime_error("native allocation conformance failed"); }
}
Uuid Id(byte n) { return Uuid{{1,2,3,4,5,6,0x71,8,0x89,10,11,12,13,14,15,n}}; }
void Empty(const p::NativeAllocationChainResult& r) {
  Check(!r.ok() && r.pages.empty() && r.retained_image_bytes==0 &&
        std::all_of(r.state_counts.begin(),r.state_counts.end(),[](u64 n){return n==0;}), "no partial chain failure");
}
#ifdef SB_NATIVE_ALLOCATION_MEMORY_TESTS
namespace m=scratchbird::core::memory;
struct MemoryFixture {
  m::MemoryManager manager;
  m::HierarchicalMemoryBudgetLedger ledger{3,5};
  db::NativeStorageMemoryBinding binding{Id(1),Id(61),Id(62),Id(63)};
  db::NativeStorageMemory memory;
  static auto Policy(){auto p=m::DefaultLocalEngineMemoryPolicy();p.hard_limit_bytes=2097152;p.per_context_limit_bytes=2097152;return p;}
  explicit MemoryFixture(u64 bytes):manager(Policy()){
    m::ReservationBackedMemoryResourceRequest r;r.memory_manager=&manager;r.reservation_ledger=&ledger;
    r.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.category=m::MemoryCategory::page_buffer;r.requested_bytes=bytes;r.memory_class="page_buffer";
    r.route_label="storage.allocation.conformance";r.purpose="actual allocation image and metadata";
    r.binary_operation_uuid=binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
    r.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(64).bytes},
      {m::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
    r.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label="allocation resource conformance";
    for(const auto& scope:r.scope_chain){m::HierarchicalMemoryBudget b;b.scope=scope;b.hard_limit_bytes=bytes;
      b.provenance=r.provenance;Check(ledger.SetBudget(b).ok(),"actual parent budget");}
    auto grant=m::AcquireReservationBackedMemoryResource(r);Check(grant.ok(),"actual node-issued metadata grant");
    auto adopted=db::AdoptNativeStorageMemory(binding,grant.resource);Check(adopted.ok()&&!grant.resource,"exclusive native adoption");
    memory=std::move(adopted.memory);
  }
  void Empty(){const auto s=manager.Snapshot();Check(!s.current_bytes&&!s.reserved_capacity_bytes&&
    !s.active_capacity_reservation_count&&!ledger.Snapshot().current_bytes,"all real allocation-map charges released");}
};
#endif
struct Fixture {
  std::filesystem::path root;
  Fixture() { std::string path=(std::filesystem::temp_directory_path()/"sb-native-allocation.XXXXXX").string();
    auto* result=::mkdtemp(path.data());Check(result,"create isolated fixture");root=result; }
  ~Fixture(){std::error_code e;std::filesystem::remove_all(root,e);}
};

void Empty(const p::NativeAllocationChainView& r){
  Check(!r.ok()&&r.pages.empty()&&!r.retained_image_bytes&&!r.backing_bytes_used&&
    std::all_of(r.state_counts.begin(),r.state_counts.end(),[](u64 n){return n==0;}),
    "complete borrowed chain refusal has no usable prefix");
}
void SameChain(const p::NativeAllocationChainResult& expected,const p::NativeAllocationChainView& actual,
    std::span<const byte> backing={}){
  Check(expected.ok()&&actual.ok()&&expected.pages.size()==actual.pages.size()&&
    expected.state_counts==actual.state_counts&&expected.retained_image_bytes==actual.retained_image_bytes,
    "complete chain counts and image accounting agree");
  const auto inside=[&](const void* pointer,std::size_t n){
    if(backing.empty())return true;
    const auto b=reinterpret_cast<std::uintptr_t>(backing.data()),p=reinterpret_cast<std::uintptr_t>(pointer);
    return p>=b&&p-b<=backing.size()&&n<=backing.size()-(p-b);
  };
  Check(inside(actual.pages.data(),actual.pages.size_bytes()),"retained chain descriptors reside in backing");
  for(std::size_t i=0;i<actual.pages.size();++i){
    const auto& a=*expected.pages[i].map;const auto& b=actual.pages[i].map;
    Check(std::equal(expected.pages[i].bytes.begin(),expected.pages[i].bytes.end(),
      actual.pages[i].image.begin(),actual.pages[i].image.end()),"exact independently encoded complete map image");
    const auto ah=d::EncodeNativeCommonPageHeader(a.header),bh=d::EncodeNativeCommonPageHeader(b.header);
    Check(ah.ok()&&bh.ok()&&ah.bytes==bh.bytes&&a.object_uuid==b.object_uuid&&
      a.map_generation==b.map_generation&&a.capacity_generation==b.capacity_generation&&
      a.total_pages==b.total_pages&&a.first_page==b.first_page&&
      a.creator_transaction_uuid==b.creator_transaction_uuid&&
      a.creator_operation_uuid==b.creator_operation_uuid&&a.creator_local_transaction_id==b.creator_local_transaction_id&&
      a.next==b.next&&a.next_sha256==b.next_sha256&&
      std::equal(a.states.begin(),a.states.end(),b.states.begin(),b.states.end())&&
      std::equal(a.records.begin(),a.records.end(),b.records.begin(),b.records.end()),
      "every full binary allocation map and record field agrees");
    Check(inside(actual.pages[i].image.data(),actual.pages[i].image.size())&&
      inside(b.states.data(),b.states.size_bytes())&&inside(b.records.data(),b.records.size_bytes()),
      "all complete chain images and decoded arrays reside in backing");
  }
}

// Independent of the codec: slots per in-place map page, the largest N with
// roundup128(512 + ceil(N / 2)) + 128 N <= P (allocation_map_inplace_page.hpp).
u64 InplaceSlots(u32 page_size){
  u64 best=0;
  for(u64 n=1;n<page_size;++n){const u64 at=(512+(n+1)/2+127)/128*128;if(at+128*n<=page_size)best=n;}
  return best;
}
void ChainMemoryChecks(d::FileDevice& device,const d::FilespaceBootstrapBinding& binding,const Bytes& head,bool deep){
  const std::size_t capacity=6*head.size()+65536;
  const auto old_writes=writes,old_syncs=syncs;
  unsigned long fixture_writes=0,fixture_syncs=0;
  const auto source_bytes=[&]{const auto extent=device.Size();Check(extent.ok(),"observe whole chain fixture");
    Bytes bytes(extent.size_bytes);const auto io=device.ReadAt(0,bytes.data(),bytes.size());
    Check(io.ok()&&io.bytes_transferred==bytes.size(),"independent whole-file no-effect oracle");return bytes;};
  const auto original=source_bytes();
  {
    const u64 allowance=2*head.size();
    const auto expected=p::ReadNativeAllocationChainFromOpenDevice(device,binding,allowance);
    Bytes backing(capacity);d::FileDevice::ReadLatencyBatch batch(device);
    const auto direct=[&](std::span<byte> region,u64 limit,bool deny=true){
      if(deny)allocation_budget=0;
      auto r=p::ReadNativeAllocationChainInto(device,binding,limit,batch,region);
      const bool untouched=allocation_budget==0;if(deny)allocation_budget=-1;
      if(deny)Check(untouched,"full actual-file chain uses no ordinary or aligned payload heap fallback");
      return r;
    };
    reads=stats=hashes=0;observed_chain_bytes=0;
    const auto full=direct(backing,allowance);SameChain(expected,full.chain,backing);
    Check(full.io_status.ok()&&full.physical_bytes_read==observed_chain_bytes&&full.physical_bytes_read,
      "chain receipt counts actual syscall bytes");
    const auto read_count=reads,stat_count=stats,hash_count=hashes;
    const auto used=full.chain.backing_bytes_used;
    Check(used&&used<=backing.size(),"complete chain records actual backing use");
    {
      d::detail::NativeMetadataMemory resource(backing);
      p::NativeAllocationChainDeviceRead composed;
      allocation_budget=0;
      {
        const auto outer=device.AcquireOperationGuard();
        composed=p::detail::ReadNativeAllocationChainBacked(device,binding,allowance,batch,resource);
      }
      const bool untouched=allocation_budget==0;allocation_budget=-1;
      Check(untouched&&resource.used()==used,"complete resource composition uses same bounded payload under enclosing fence");
      SameChain(expected,composed.chain,backing);
      auto refused=p::detail::ReadNativeAllocationChainBacked(device,binding,allowance,batch,*std::pmr::null_memory_resource());
      Empty(refused.chain);Check(refused.chain.error==E::resource_exhausted&&!refused.physical_bytes_read,
        "composite caller with no backing refuses before actual source I/O");
    }
    SameChain(expected,direct(std::span(backing).first(used),allowance).chain,backing);
    auto short_backing=direct(std::span(backing).first(used-1),allowance);
    Empty(short_backing.chain);Check(short_backing.chain.error==E::resource_exhausted,"one byte short actual backing refuses whole chain");
    auto short_images=direct(backing,allowance-1);Empty(short_images.chain);
    Check(short_images.chain.error==E::resource_exhausted,"verification ceiling remains separate from physical memory");
    if(deep){
      const auto alias=[&](auto& input){
        auto* bytes=reinterpret_cast<byte*>(const_cast<std::remove_const_t<std::remove_reference_t<decltype(input)>>*>(&input));
        const auto r=direct({bytes,sizeof(input)},allowance);
        Check(r.chain.error==E::invalid_workspace&&!r.physical_bytes_read,"whole descriptor alias rejected before reads or writes");Empty(r.chain);
      };
      alias(binding);alias(device);alias(batch);
      d::FileDevice foreign;d::FileDevice::ReadLatencyBatch wrong(foreign);
      auto mismatch=p::ReadNativeAllocationChainInto(device,binding,allowance,wrong,backing);
      Check(!mismatch.ok()&&!mismatch.physical_bytes_read,"foreign batch cannot supply another device");Empty(mismatch.chain);
      for(unsigned n=1;n<=read_count;++n){
        reads=0;observed_chain_bytes=0;fail_read=n;const auto r=direct(backing,allowance,false);fail_read=0;
        Empty(r.chain);Check(r.chain.error==E::io_failure&&r.physical_bytes_read==observed_chain_bytes&&
          !r.io_status.ok()&&r.io_diagnostic.diagnostic_code=="SB-STORAGE-DISK-READ-SHORT",
          "every failed syscall retains exact physical bytes and typed diagnostic");
      }
      for(unsigned n=1;n<=stat_count;++n){
        stats=0;observed_chain_bytes=0;fail_stat=n;const auto r=direct(backing,allowance,false);fail_stat=0;
        Empty(r.chain);Check(r.chain.error==E::io_failure&&r.physical_bytes_read==observed_chain_bytes&&
          r.io_diagnostic.diagnostic_code=="SB-STORAGE-DISK-SIZE-FAILED","all actual extent failures retain diagnostics and bytes");
      }
      for(unsigned n=1;n<=hash_count;++n){
        hashes=0;fail_hash_at=n;const auto r=direct(backing,allowance);fail_hash_at=0;
        Empty(r.chain);Check(r.chain.error==E::hash_failure,"every hash context failure stays typed without heap diagnostics");
      }
      short_read=read_count;eof_read=read_count+1;reads=0;observed_chain_bytes=0;
      auto partial=direct(backing,allowance,false);short_read=eof_read=0;
      Empty(partial.chain);Check(partial.chain.error==E::io_failure&&partial.physical_bytes_read==observed_chain_bytes,
        "partial actual read then EOF retains effects without chain prefix");
      SameChain(expected,direct(backing,allowance).chain,backing);
    }

#ifdef SB_NATIVE_ALLOCATION_MEMORY_TESTS
    using M=db::NativeAllocationChainMemoryError;
    const db::NativeAllocationChainMemoryLimits limits{allowance,capacity};
    MemoryFixture grant(capacity);
    const auto call=[&](MemoryFixture& f,const db::NativeStorageMemoryBinding& owner){
      reads=hashes=stats=0;observed_chain_bytes=0;
      return db::ReadNativeAllocationChainWithMemoryFromOpenDevice(device,binding,limits,f.memory,owner);
    };
    {auto guard=device.AcquireOperationGuard();allocation_device_mutex=guard.mutex();}
    allocation_lock_free=false;auto retained=call(grant,grant.binding);
    Check(retained.ok()&&allocation_lock_free,"whole chain actual grant is admitted outside source fence");
    SameChain(expected,retained.chain);
    Check(grant.manager.Snapshot().current_bytes==capacity&&grant.memory.Snapshot().allocated_bytes==capacity&&
      grant.ledger.Snapshot().current_bytes==capacity&&retained.arena.Snapshot().retained_bytes==capacity,
      "actual manager parent resource and retained arena charges agree");
    const auto failed=[&](const auto& r){Check(!r.ok()&&!r.arena,"no failed chain owner");Empty(r.chain);};
    auto exhausted=call(grant,grant.binding);failed(exhausted);
    Check(exhausted.error==M::memory_allocation_failure&&!reads,"live chain prevents uncharged concurrent allocation");
    for(unsigned field=0;field<4;++field){
      auto wrong=grant.binding;std::array<Uuid*,4> ids{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};
      *ids[field]=Id(99);const auto r=call(grant,wrong);failed(r);
      Check(r.error==M::memory_binding_failure&&!reads,"all four binary ownership bindings enforced before I/O");
    }
    const auto path=device.path();const bool readonly=device.read_only();
    const auto before_close_writes=writes,before_close_syncs=syncs;
    Check(device.Close().ok(),"close source while full chain remains retained");SameChain(expected,retained.chain);
    Check(device.Open(path,readonly?d::FileOpenMode::open_existing_read_only:d::FileOpenMode::open_existing).ok(),"reopen exact source");
    // Explicit fixture reopening updates the process-ownership sidecar. Those
    // syscalls are not reader effects; exclude only this measured fixture step.
    fixture_writes+=writes-before_close_writes;fixture_syncs+=syncs-before_close_syncs;
    const auto revoked=grant.ledger.CleanupOwner(grant.binding.owner_uuid.bytes);
    Check(revoked.retained_bytes==capacity,"revocation preserves retained physical chain accounting");
    const auto denied=call(grant,grant.binding);failed(denied);
    Check(denied.error==M::memory_binding_failure&&!reads,"revoked memory cannot initiate new source reads");
    grant.memory={};SameChain(expected,retained.chain);
    Check(grant.manager.Snapshot().current_bytes==capacity,"whole chain outlives allocating workspace");
    retained={};grant.Empty();
    MemoryFixture small(capacity-1);
    auto shortage=call(small,small.binding);failed(shortage);
    Check(shortage.error==M::memory_allocation_failure&&!reads,"actual backing one-byte-short grant refuses");
    small.memory={};small.Empty();
    if(deep){
      MemoryFixture valid(capacity);const auto held=valid.ledger.Snapshot().current_bytes;
      const auto clean=[&]{const auto s=valid.memory.Snapshot();
        Check(!valid.manager.Snapshot().current_bytes&&!s.allocated_bytes&&s.allocation_count==s.release_count&&
          valid.ledger.Snapshot().current_bytes==held,"all fault payloads released while reusable grant remains");};
      {auto guard=device.AcquireOperationGuard();observation_device_mutex=guard.mutex();}
      memory_probes=locked_memory_probes=0;
      {auto r=call(valid,valid.binding);Check(r.ok(),"actual chain memory and observation fence probe");}
      observation_device_mutex=nullptr;
      Check(memory_probes&&!locked_memory_probes,"backing observations and cleanup acquire no heap storage under source fences");clean();
      for(unsigned n=1;n<=read_count;++n){
        fail_read=n;{auto guard=device.AcquireOperationGuard();deallocation_device_mutex=guard.mutex();}
        deallocation_lock_free=false;auto r=call(valid,valid.binding);fail_read=0;failed(r);
        Check(r.chain_error==E::io_failure&&deallocation_lock_free&&r.physical_bytes_read==observed_chain_bytes,
          "every failed actual chain read cleans up outside device fence with exact receipt");clean();
      }
      for(unsigned n=1;n<=stat_count;++n){
        fail_stat=n;auto r=call(valid,valid.binding);fail_stat=0;failed(r);
        Check(r.chain_error==E::io_failure&&r.physical_bytes_read==observed_chain_bytes&&
          r.io_diagnostic.diagnostic_code=="SB-STORAGE-DISK-SIZE-FAILED","governed extent failure retains full receipt");clean();
      }
      for(unsigned n=1;n<=hash_count;++n){
        fail_hash_at=n;auto r=call(valid,valid.binding);fail_hash_at=0;failed(r);
        Check(r.chain_error==E::hash_failure,"governed complete chain hash failure");clean();
      }
      for(unsigned phase=1;phase<=5;++phase){method_fault=phase;auto r=call(valid,valid.binding);failed(r);
        Check(!method_fault&&r.chain_error==E::hash_failure,"all provider phases have exact typed chain failures");clean();}
      unsigned long sites=0;
      {observed_allocations=0;count_allocations=true;auto r=call(valid,valid.binding);count_allocations=false;
        sites=observed_allocations;Check(r.ok(),"measure all admitted chain allocation sites");}
      for(unsigned long n=0;n<sites;++n){
        allocation_budget=n;auto r=call(valid,valid.binding);allocation_budget=-1;
        if(r.ok())SameChain(expected,r.chain);else failed(r);r={};clean();
        auto retry=call(valid,valid.binding);Check(retry.ok(),"same owner retries after every allocation refusal");
      }
      {
        std::lock_guard lock(read_pause_mutex);pause_read=true;read_entered=false;release_read=false;
      }
      auto reading=std::async(std::launch::async,[&]{return call(valid,valid.binding);});
      bool entered=false;
      {std::unique_lock lock(read_pause_mutex);entered=read_pause_cv.wait_for(lock,std::chrono::seconds(5),[]{return read_entered;});}
      std::atomic<bool> close_returned=false;bool blocked=false;
      std::promise<void> close_requested;auto requested=close_requested.get_future();
      std::thread release([&]{requested.wait();std::this_thread::sleep_for(std::chrono::milliseconds(20));
        blocked=!close_returned.load();std::lock_guard lock(read_pause_mutex);release_read=true;read_pause_cv.notify_all();});
      const auto fixture_before_writes=writes,fixture_before_syncs=syncs;
      close_requested.set_value();const auto closed=device.Close();close_returned=true;
      release.join();auto complete=reading.get();
      Check(entered&&blocked&&closed.ok()&&complete.ok(),"opening-thread Close waits for whole actual-grant chain verification");
      SameChain(expected,complete.chain);
      Check(device.Open(path,readonly?d::FileOpenMode::open_existing_read_only:d::FileOpenMode::open_existing).ok(),"restore exact source after concurrent Close");
      fixture_writes+=writes-fixture_before_writes;fixture_syncs+=syncs-fixture_before_syncs;
      complete={};clean();
      valid.memory={};valid.Empty();
    }
#endif
    Check(source_bytes()==original&&writes==old_writes+fixture_writes&&syncs==old_syncs+fixture_syncs,
      "all chain inspection and fault paths preserve every source byte without writes or syncs");
  }
}

// The fixture: a stand-alone filespace (role 5, a secondary data filespace)
// written by the production in-place writer (InitializeNativeFilespaceOnOpenDevice),
// with two map pages: the first covers a full page of slots, the second 5.
struct InitializedFilespace {
  db::NativeFilespaceInitializationRequest request;
  std::optional<db::NativeFilespaceInitializationReceipt> receipt;
  u64 slots=0;
  InitializedFilespace(d::FileDevice& device,unsigned profile){
    const auto& size=d::kCanonicalFilespacePageProfiles[profile];
    slots=InplaceSlots(size.page_size_bytes);
    request.bootstrap={Id(1),Id(2),size.uuid,d::kNativeBootstrapIntegrityProfile,{},size.page_size_bytes,1,0,5,7};
    request.operation_uuid=Id(4);request.writer_uuid=Id(5);request.policy_snapshot_uuid=Id(6);
    request.creator.transaction_uuid={UuidKind::transaction,Id(30)};
    request.creator.local_id=scratchbird::transaction::mga::MakeLocalTransactionId(8);
    request.creator.scope=scratchbird::transaction::mga::TransactionScope::local_node;
    request.creation_utc_millis=1789357072000ULL;request.total_pages=slots+5;
    scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({Id(1),Id(6)},{{},0,1000});
    const auto created=db::InitializeNativeFilespaceOnOpenDevice(device,request,4*size.page_size_bytes,issuer);
    Check(created.ok()&&created.receipt->map_pages==2,"production in-place writer: a filespace with two map pages");
    receipt=created.receipt;
  }
};
Bytes ReadPage(d::FileDevice& device,u64 number,u32 size){
  Bytes bytes(size);const auto io=device.ReadAt(number*size,bytes.data(),bytes.size());
  Check(io.ok()&&io.bytes_transferred==bytes.size(),"read a page of the fixture");return bytes;
}
void WritePage(d::FileDevice& device,u64 number,const Bytes& bytes){
  const auto io=device.WriteAt(number*bytes.size(),bytes.data(),bytes.size());
  Check(io.ok()&&io.bytes_transferred==bytes.size()&&device.Sync().ok(),"write a forged or restored page");
}
void RetainedChain() {
  for (unsigned profile=0;profile<5;++profile) {
    Fixture fixture;d::FileDevice device;const auto path=(fixture.root/"node").string();
    Check(device.Open(path,d::FileOpenMode::create_new,d::DatabaseFileOwnership::native_data_file).ok(),"own filespace fixture");
    const InitializedFilespace made(device,profile);
    const u32 size=made.request.bootstrap.page_size_bytes;const u64 total=made.request.total_pages;
    const auto head_bytes=ReadPage(device,1,size),tail_bytes=ReadPage(device,2,size);
    Check(p::IsAllocationMapInplaceImage(head_bytes)&&p::IsAllocationMapInplaceImage(tail_bytes),"both map pages are in place");
    const d::FilespaceBootstrapBinding binding{Id(1),Id(2),made.request.bootstrap.page_size_profile_uuid};
    const auto read=[&](u64 budget){return p::ReadNativeAllocationChainFromOpenDevice(device,binding,budget);};
    const u64 limit=2*u64{size};
    reads=0;observed_allocations=0;count_allocations=true;auto result=read(limit);count_allocations=false;
    const auto read_count=reads;const auto allocation_count=observed_allocations;
    Check(result.ok()&&result.pages.size()==2&&result.retained_image_bytes==limit&&result.state_counts[2]==3&&
          result.state_counts[0]==total-3&&result.pages[0].bytes==head_bytes&&result.pages[1].bytes==tail_bytes&&
          result.pages[0].map->states.size()==made.slots&&result.pages[1].map->states.size()==5,
          "actual complete multi-page allocation chain");
    ChainMemoryChecks(device,binding,head_bytes,profile==0);
    Empty(read(limit-1));
    for(unsigned fault=1;fault<=read_count;++fault){reads=0;fail_read=fault;result=read(limit);fail_read=0;Empty(result);}
    if(profile==0){bool success=false;
      for(unsigned long budget=0;budget<=allocation_count;++budget){allocation_budget=static_cast<long>(budget);result=read(limit);allocation_budget=-1;
        if(result.ok()){success=true;break;}Empty(result);Check(result.error==E::resource_exhausted,"retained allocation failure classified");}
      Check(success,"all retained-chain allocation failure positions");
      std::cout << "retained allocation fault positions=" << allocation_count << '\n';
    }
    // The control pages' own records bind the chain: the head map page's
    // record (slot 1 of the first map page) naming another page identity,
    // generation or owner is refused. Offsets from the documented layout:
    // records from roundup128(512 + ceil(slots / 2)), 128 bytes each; page UUID
    // at +24, owner at +40, page generation at +80.
    const u64 records=(512+(made.slots+1)/2+127)/128*128,record=records+128;
    // A changed page UUID or owner is a record the page does not match; a
    // generation flipped from 1 to 0 makes the record itself invalid (a page
    // UUID without a generation).
    for(const auto& [field,expected]:{std::pair{24u,E::physical_owner_mismatch},std::pair{40u,E::physical_owner_mismatch},
                                      std::pair{80u,E::invalid_record}}){
      auto forged=head_bytes;forged[record+field]^=1;WritePage(device,1,forged);
      const auto rejected=read(limit);Empty(rejected);
      Check(rejected.error==expected,"a control page's record naming another identity, owner or generation is refused");
      WritePage(device,1,head_bytes);
    }
    Check(read(limit).ok(),"restored");
    // The chain must cover every page page zero counts: the head map page's
    // next link (offset 256: page, then generation) cleared ends the chain one
    // map page early. (The sealed family's page-zero free-counter cross-check
    // has no in-place counterpart, owner Q22 = 1; coverage is what remains.)
    {
      auto forged=head_bytes;std::fill(forged.begin()+256,forged.begin()+272,byte{0});WritePage(device,1,forged);
      const auto rejected=read(limit);Empty(rejected);
      Check(rejected.error==E::counter_mismatch,"a chain that stops short of page zero's total is refused");
      WritePage(device,1,head_bytes);Check(read(limit).ok(),"restored");
    }
    fail_hash=true;result=read(limit);Check(!fail_hash,"retained hash failure reached provider");Empty(result);
    Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing_read_only).ok(),"reopen read-only actual filespace");
    Check(read(limit).ok()&&device.read_only(),"retained reader preserves read-only ownership");
    Check(device.Close().ok(),"close fixture");Empty(read(limit));
  }
}
}  // namespace
int main(){
  try { RetainedChain();
    std::cout<<"native allocation checks="<<checks<<" failures=0\n"; }
  catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
