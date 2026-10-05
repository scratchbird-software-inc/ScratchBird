// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_checkpoint_selection.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <source_location>
#include <stdexcept>
#if defined(SB_NATIVE_SELECTOR_MEMORY_TESTS)
#include "native_checkpoint_selection_memory.hpp"
#include <cerrno>
#include <filesystem>
#include <thread>
#include <unistd.h>
#endif
namespace {thread_local long allocation_budget=-1;thread_local bool counting=false;thread_local unsigned long allocations=0;thread_local unsigned hash_fault=0;}
#if defined(SB_NATIVE_SELECTOR_MEMORY_TESTS)
namespace {thread_local std::recursive_mutex* metric_publication_mutex=nullptr;thread_local bool metric_publication_unlocked=false;
void ProbeMetricPublication(){if(auto* mutex=metric_publication_mutex){metric_publication_mutex=nullptr;
  bool unlocked=false;std::thread probe([&]{unlocked=mutex->try_lock();if(unlocked)mutex->unlock();});probe.join();metric_publication_unlocked=unlocked;}}}
#endif
void* operator new(std::size_t n){
#if defined(SB_NATIVE_SELECTOR_MEMORY_TESTS)
  ProbeMetricPublication();
#endif
  if(counting)++allocations;if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
#if defined(SB_NATIVE_SELECTOR_MEMORY_TESTS)
namespace {thread_local std::recursive_mutex* allocation_device_mutex=nullptr;
thread_local std::recursive_mutex* deallocation_device_mutex=nullptr;
thread_local bool allocation_lock_free=false,deallocation_lock_free=false;
void CheckDeallocationLock(){if(deallocation_device_mutex){auto* mutex=deallocation_device_mutex;deallocation_device_mutex=nullptr;
  bool available=false;std::thread inspect([&]{available=mutex->try_lock();if(available)mutex->unlock();});inspect.join();deallocation_lock_free=available;}}}
void* operator new(std::size_t n,std::align_val_t a){if(counting)++allocations;
  if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;
  if(allocation_device_mutex){auto* mutex=allocation_device_mutex;allocation_device_mutex=nullptr;
    bool available=false;std::thread inspect([&]{available=mutex->try_lock();if(available)mutex->unlock();});inspect.join();allocation_lock_free=available;}
  void* p=nullptr;if(posix_memalign(&p,static_cast<std::size_t>(a),n?n:1)==0)return p;throw std::bad_alloc();}
void operator delete(void* p,std::align_val_t)noexcept{CheckDeallocationLock();std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t)noexcept{CheckDeallocationLock();std::free(p);}
namespace {thread_local unsigned reads=0,fail_read=0,short_read=0,eof_read=0;thread_local void* last_read_buffer=nullptr;}
namespace {thread_local unsigned writes=0,fail_write=0,partial_write=0,zero_write=0,syncs=0,fail_sync=0;}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* data,size_t n,off_t offset){++reads;last_read_buffer=data;
  if(fail_read&&reads==fail_read){errno=EIO;return -1;}
  if(eof_read&&reads==eof_read)return 0;
  return __real_pread(fd,data,short_read&&reads==short_read?n-1:n,offset);}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t n,off_t offset){++writes;
  if(fail_write&&writes==fail_write){errno=EIO;return -1;}
  if(zero_write&&writes==zero_write)return 0;
  return __real_pwrite(fd,data,partial_write&&writes==partial_write?n/2:n,offset);}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){++syncs;if(fail_sync&&syncs==fail_sync){errno=EIO;return -1;}return __real_fsync(fd);}
#endif
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){if(hash_fault==1){hash_fault=0;return nullptr;}return __real_EVP_MD_CTX_new();}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* m,ENGINE* e){if(hash_fault==2){hash_fault=0;return 0;}return __real_EVP_DigestInit_ex(c,m,e);}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* b,size_t n){if(hash_fault==3){hash_fault=0;return 0;}return __real_EVP_DigestUpdate(c,b,n);}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* b,unsigned int* n){if(hash_fault==4){hash_fault=0;return 0;}const int r=__real_EVP_DigestFinal_ex(c,b,n);if(hash_fault==5){hash_fault=0;*n=31;}return r;}
namespace {
namespace db=scratchbird::storage::database;namespace d=scratchbird::storage::disk;using namespace scratchbird::core::platform;
using Bytes=std::vector<byte>;using E=db::NativeCheckpointSelectionError;unsigned checks=0;
void Check(bool ok,const char* why,std::source_location at=std::source_location::current()){++checks;if(!ok)throw std::runtime_error(std::string(why)+" line="+std::to_string(at.line()));}
template<class F> auto DenyCodecAllocation(F&& call){
  const auto saved=allocation_budget;allocation_budget=0;
  auto result=call();const bool unchanged=allocation_budget==0;allocation_budget=saved;
  Check(unchanged,"native codec provider refusal must not allocate diagnostic text");
  return result;
}
Uuid Id(byte n){Uuid id;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=n;return id;}
void Num(Bytes& b,std::size_t at,unsigned n,u64 v){for(unsigned i=0;i<n;++i)b[at+i]=static_cast<byte>(v>>(8*i));}
void Put(Bytes& b,std::size_t at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}
void Ref(Bytes& b,std::size_t at,const d::NativePageReference& r){Put(b,at,r.filespace_uuid);Num(b,at+16,8,r.page_number);Num(b,at+24,8,r.page_generation);Put(b,at+32,r.page_size_profile_uuid);}
void Seal(Bytes& b){std::fill(b.begin()+432,b.begin()+464,0);std::array<byte,32> sha{};Check(SHA256(b.data(),b.size(),sha.data())!=nullptr,"independent complete selector digest");std::copy(sha.begin(),sha.end(),b.begin()+432);}
Bytes Oracle(const db::NativeCheckpointSelection& s){const auto& h=s.header;Bytes b(h.page_size_bytes,0);std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,h.page_type);Num(b,20,2,1);Num(b,22,2,1);
  Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Num(b,88,8,h.flags);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);
  u64 fnv=14695981039346656037ull;for(unsigned i=0;i<128;++i){fnv^=b[i];fnv*=1099511628211ull;}Num(b,96,8,fnv);
  std::copy_n("SBDCP001",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,384);Num(b,140,4,512);Put(b,144,s.object_uuid);Put(b,160,s.bootstrap_uuid);Num(b,176,8,s.selection_generation);Put(b,184,s.publication_uuid);
  Ref(b,200,s.checkpoint);Put(b,248,s.checkpoint_object_uuid);std::copy(s.checkpoint_sha256.begin(),s.checkpoint_sha256.end(),b.begin()+264);Num(b,296,8,s.checkpoint_generation);Num(b,304,8,s.root_set_generation);Put(b,312,s.timeline_uuid);
  Num(b,328,8,s.previous_selection_generation);if(s.previous_checkpoint)Ref(b,336,*s.previous_checkpoint);Put(b,384,s.previous_checkpoint_object_uuid);std::copy(s.previous_checkpoint_sha256.begin(),s.previous_checkpoint_sha256.end(),b.begin()+400);Seal(b);return b;}
db::NativeCheckpointSelection Example(unsigned p=0){const auto& profile=d::kCanonicalFilespacePageProfiles[p];db::NativeCheckpointSelection s;
  s.header={profile.page_size_bytes,0x30e,Id(1),Id(2),Id(3),21,7,0,profile.uuid};s.object_uuid=Id(4);s.bootstrap_uuid=Id(5);s.publication_uuid=Id(6);s.selection_generation=1;
  s.checkpoint={Id(2),19,109,profile.uuid};s.checkpoint_object_uuid=Id(49);s.checkpoint_sha256.fill(7);s.checkpoint_generation=5;s.root_set_generation=8;s.timeline_uuid=Id(8);return s;}
auto Other(db::NativeCheckpointSelection s){s.header.page_uuid=Id(9);s.header.page_number=22;return s;}
auto Next(db::NativeCheckpointSelection s){s.previous_selection_generation=s.selection_generation++;s.previous_checkpoint=s.checkpoint;s.previous_checkpoint_object_uuid=s.checkpoint_object_uuid;s.previous_checkpoint_sha256=s.checkpoint_sha256;
  s.publication_uuid=Id(10);s.checkpoint.page_number=23;s.checkpoint.page_generation++;s.checkpoint_sha256.fill(11);s.checkpoint_generation++;s.root_set_generation++;return s;}
void Empty(const db::NativeCheckpointSelectionImage& r){Check(!r.ok()&&!r.selection&&r.bytes.empty(),"image failure returns no prefix");}
void Empty(const db::NativeCheckpointSelectionViewImage& r){Check(!r.ok()&&!r.selection&&r.bytes.empty(),"bounded image failure returns no prefix");}
void Empty(const db::NativeCheckpointSelectionPair& r){Check(!r.ok()&&!r.selection,"pair failure returns no usable selection");}
void InvalidImage(const Bytes& b){const auto owned=db::DecodeNativeCheckpointSelection(b);Empty(owned);
  const auto value=DenyCodecAllocation([&]{return db::DecodeNativeCheckpointSelectionValue(b);});
  Check(!value.ok()&&!value.selection&&value.error==owned.error,"value and owning decoder preserve exact refusal without prefix");}
void Invalid(const db::NativeCheckpointSelection& s){
  const auto expected=db::EncodeNativeCheckpointSelection(s);Empty(expected);
  Bytes output(d::kCanonicalFilespacePageProfiles.back().page_size_bytes,0xa5);
  const auto actual=DenyCodecAllocation([&]{return db::EncodeNativeCheckpointSelectionInto(s,output);});
  Empty(actual);Check(actual.error==expected.error&&std::all_of(output.begin(),output.end(),[](byte v){return v==0xa5;}),"bounded invalid input refuses with exact error before write");InvalidImage(Oracle(s));
}
void EncodingTest(const db::NativeCheckpointSelection& s){
  const auto expected=Oracle(s);Bytes output(expected.size()+2,0xa5);
  const auto span=std::span(output).subspan(1,expected.size());
  const auto parity=[&](const auto& r){Check(r.ok()&&r.bytes.data()==span.data()&&r.bytes.size()==expected.size()&&
    std::equal(r.bytes.begin(),r.bytes.end(),expected.begin())&&Oracle(*r.selection)==expected,"exact independent bounded selector bytes and fields");};
  parity(DenyCodecAllocation([&]{return db::EncodeNativeCheckpointSelectionInto(s,span);}));
  Check(output.front()==0xa5&&output.back()==0xa5,"unaligned output exact prefix only");
  parity(DenyCodecAllocation([&]{return db::EncodeNativeCheckpointSelectionInto(s,std::span(output).subspan(1));}));
  Check(output.back()==0xa5,"oversized backing suffix untouched");
  for(const auto size:{std::size_t(0),std::size_t(1),expected.size()-1}){
    std::fill(output.begin(),output.end(),0xa5);
    const auto r=DenyCodecAllocation([&]{return db::EncodeNativeCheckpointSelectionInto(s,span.first(size));});Empty(r);
    Check(r.error==E::resource_exhausted&&std::all_of(output.begin(),output.end(),[](byte v){return v==0xa5;}),"short output refuses without modification");
  }
  auto alias=s;std::array<byte,sizeof(alias)> before{};std::memcpy(before.data(),&alias,sizeof(alias));
  for(const auto offset:{std::size_t(0),sizeof(alias)-1}){
    const auto r=DenyCodecAllocation([&]{return db::EncodeNativeCheckpointSelectionInto(alias,{reinterpret_cast<byte*>(&alias)+offset,sizeof(alias)-offset});});Empty(r);
    Check(r.error==E::invalid_backing&&std::memcmp(before.data(),&alias,sizeof(alias))==0,"complete input excluded including predecessor and last byte");
  }
  constexpr auto suffix_offset=d::kCanonicalFilespacePageProfiles.back().page_size_bytes;
  std::vector<std::max_align_t> shared((suffix_offset+sizeof(s)+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t));
  auto* bytes=reinterpret_cast<byte*>(shared.data());auto* suffix=std::construct_at(reinterpret_cast<db::NativeCheckpointSelection*>(bytes+suffix_offset),s);
  const Bytes original(bytes,bytes+shared.size()*sizeof(std::max_align_t));
  const auto overlap=DenyCodecAllocation([&]{return db::EncodeNativeCheckpointSelectionInto(*suffix,{bytes,original.size()});});Empty(overlap);
  Check(overlap.error==E::invalid_backing&&std::equal(original.begin(),original.end(),bytes),"suffix input alias refused before any write");std::destroy_at(suffix);
  for(unsigned mode=1;mode<=5;++mode){hash_fault=mode;
    const auto r=DenyCodecAllocation([&]{return db::EncodeNativeCheckpointSelectionInto(s,span);});Empty(r);
    Check(!hash_fault&&r.error==E::hash_failure,"each seal provider fault leaves no bounded success prefix");
    parity(DenyCodecAllocation([&]{return db::EncodeNativeCheckpointSelectionInto(s,span);}));
  }
  auto overflow=s;overflow.checkpoint.page_number=u64(std::numeric_limits<std::streamoff>::max())/s.header.page_size_bytes;Invalid(overflow);
}
#if defined(SB_NATIVE_SELECTOR_MEMORY_TESTS)
namespace m=scratchbird::core::memory;
using ME=db::NativeCheckpointSelectionMemoryError;
struct MemoryFixture {
  m::MemoryManager manager;
  m::HierarchicalMemoryBudgetLedger ledger{3,5};
  db::NativeStorageMemoryBinding binding{Id(1),Id(61),Id(62),Id(63)};
  db::NativeStorageMemory memory;
  static auto Policy(){auto p=m::DefaultLocalEngineMemoryPolicy();p.hard_limit_bytes=1048576;p.per_context_limit_bytes=1048576;return p;}
  explicit MemoryFixture(u64 bytes):manager(Policy()){
    m::ReservationBackedMemoryResourceRequest r;r.memory_manager=&manager;r.reservation_ledger=&ledger;
    r.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.category=m::MemoryCategory::page_buffer;r.requested_bytes=bytes;r.memory_class="page_buffer";
    r.route_label="storage.selector.conformance";r.purpose="actual selector page";
    r.binary_operation_uuid=binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
    r.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(64).bytes},
      {m::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
    r.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label="selector resource conformance";
    for(const auto& scope:r.scope_chain){m::HierarchicalMemoryBudget b;
      b.scope=scope;b.hard_limit_bytes=bytes;b.provenance=r.provenance;Check(ledger.SetBudget(b).ok(),"actual parent budget");}
    auto grant=m::AcquireReservationBackedMemoryResource(r);Check(grant.ok(),"actual node-issued selector grant");
    auto adopted=db::AdoptNativeStorageMemory(binding,grant.resource);Check(adopted.ok()&&!grant.resource,"exclusive native grant adoption");
    memory=std::move(adopted.memory);
  }
  void Empty(){const auto s=manager.Snapshot();Check(!s.current_bytes&&!s.reserved_capacity_bytes&&
    !s.active_capacity_reservation_count&&!ledger.Snapshot().current_bytes,"all real selector charges released");}
};
struct SelectorFile {
  std::filesystem::path directory,path;
  d::FileDevice device;
  db::NativeCheckpointSelection value;
  Bytes bytes;
  d::NativeCommonPageHeaderBinding expected;
  explicit SelectorFile(unsigned p):value(Example(p)),bytes(Oracle(value)){
    char name[]="/tmp/sb-selector-memory-XXXXXX";const auto* made=mkdtemp(name);if(!made)throw std::runtime_error("mkdtemp");
    directory=made;path=directory/"native.bin";
    Check(device.Open(path.string(),d::FileOpenMode::create_new).ok(),"actual selector device");
    expected={{value.header.database_uuid,value.header.filespace_uuid,value.header.page_size_profile_uuid},
      value.header.page_number,value.header.page_generation,0x30e,value.header.page_uuid};
    Bootstrap();Store(bytes);Check(device.Sync().ok()&&device.Close().ok()&&
      device.Open(path.string(),d::FileOpenMode::open_existing).ok(),"cold reopen independently packed selector");
  }
  ~SelectorFile(){(void)device.Close();std::error_code ec;std::filesystem::remove_all(directory,ec);}
  void Bootstrap(u32 flags=0){
    Bytes b(4096,0);std::copy_n("SBFP",4,b.begin());Num(b,4,2,1);Num(b,6,2,4096);
    Num(b,8,4,value.header.page_size_bytes);Num(b,12,4,flags);Put(b,16,value.header.database_uuid);
    Put(b,32,value.header.filespace_uuid);Put(b,48,value.header.page_size_profile_uuid);
    Num(b,64,4,1);Num(b,68,2,1);Num(b,70,2,1);Put(b,72,d::kNativeBootstrapIntegrityProfile);
    if(flags&1)Put(b,88,Id(71));std::array<byte,32> digest{};
    Check(SHA256(b.data(),104,digest.data())!=nullptr,"independent bootstrap digest");
    std::copy(digest.begin(),digest.end(),b.begin()+104);
    Check(device.WriteAt(0,b.data(),b.size()).ok()&&device.Sync().ok(),"actual independently packed bootstrap");
  }
  void Store(const Bytes& image){Check(device.WriteAt(value.header.page_number*u64(value.header.page_size_bytes),image.data(),image.size()).ok()&&device.Sync().ok(),"actual selector bytes");}
  auto Read(MemoryFixture& f){reads=0;return db::ReadNativeCheckpointSelectionWithMemoryFromOpenDevice(
    device,expected,value.object_uuid,f.memory,f.binding);}
};
void NoImage(const db::NativeCheckpointSelectionMemoryResult& r){Check(!r.ok()&&!r.selection&&!r.image,"refusal exposes neither payload nor decoded prefix");}
void MemoryTest(){
  for(unsigned p=0;p<5;++p){SelectorFile file(p);const auto size=file.bytes.size();MemoryFixture f(size);
    allocation_device_mutex=file.device.AcquireOperationGuard().mutex();allocation_lock_free=false;
    auto result=file.Read(f);
    Check(!allocation_device_mutex&&allocation_lock_free,"allocate through node manager before holding device operation guard");
    Check(result.ok()&&reads==2&&result.page_bytes_read==size&&result.image.data()==last_read_buffer,
      "actual file read returns same physical governed allocation");
    Check(std::equal(file.bytes.begin(),file.bytes.end(),result.image.data())&&Oracle(*result.selection)==file.bytes,
      "all-profile independently packed bytes and fields preserved");
    Check(f.manager.Snapshot().current_bytes==size&&f.memory.Snapshot().allocated_bytes==size,
      "returned image stays exactly charged");
    {
      MemoryFixture destination(size);
      allocation_device_mutex=file.device.AcquireOperationGuard().mutex();allocation_lock_free=false;
      auto payload=destination.memory.AllocatePage(file.value.header.page_size_profile_uuid);
      Check(payload.ok()&&!allocation_device_mutex&&allocation_lock_free,"actual selector encoding backing before device fence");
      {
        d::FileDevice::WriteLatencyBatch batch(file.device);
        Check(&batch.device()==&file.device,"write observation batch retains exact device");
        {
          const auto guard=file.device.AcquireOperationGuard();
          const auto encoded=DenyCodecAllocation([&]{return db::EncodeNativeCheckpointSelectionInto(*result.selection,{payload.buffer.data(),payload.buffer.size()});});
          Check(encoded.ok()&&std::equal(file.bytes.begin(),file.bytes.end(),encoded.bytes.begin()),"actual retained source and admitted encoding under guard");
          const auto rejected=file.device.rejected_io_latency_observations(),failed=file.device.failed_io_latency_observations();
          for(unsigned i=0;i<15;++i){const auto write=DenyCodecAllocation([&]{return batch.WriteAt(file.value.header.page_number*u64(size),encoded.bytes.data(),encoded.bytes.size());});
            Check(write.ok()&&write.bytes_transferred==size,"actual staged bounded selector write");}
          const auto sync=DenyCodecAllocation([&]{return batch.Sync();});
          Check(sync.ok()&&file.device.rejected_io_latency_observations()==rejected&&file.device.failed_io_latency_observations()==failed,"sixteen captures including sync without telemetry callback");
          const auto overflow=DenyCodecAllocation([&]{return batch.WriteAt(file.value.header.page_number*u64(size),encoded.bytes.data(),encoded.bytes.size());});
          Check(overflow.ok()&&overflow.bytes_transferred==size&&file.device.rejected_io_latency_observations()==rejected+1&&file.device.failed_io_latency_observations()==failed,"seventeenth observation rejected without cancelling physical write");
        }
        metric_publication_mutex=file.device.AcquireOperationGuard().mutex();metric_publication_unlocked=false;
      }
      Check(!metric_publication_mutex&&metric_publication_unlocked,"write and sync observations publish only after compound guard releases");
      {
        d::FileDevice::WriteLatencyBatch batch(file.device);const auto guard=file.device.AcquireOperationGuard();
        const auto offset=file.value.header.page_number*u64(size);
        writes=0;partial_write=1;fail_write=2;const auto partial=batch.WriteAt(offset,payload.buffer.data(),size);partial_write=fail_write=0;
        Check(!partial.ok()&&partial.bytes_transferred==size/2,"batched error retains actual partial physical prefix");
        writes=0;zero_write=1;const auto zero=batch.WriteAt(offset,payload.buffer.data(),size);zero_write=0;
        Check(!zero.ok()&&!zero.bytes_transferred,"zero native progress remains a failed write");
        syncs=0;fail_sync=1;const auto failed_sync=batch.Sync();fail_sync=0;
        Check(!failed_sync.ok(),"batched native synchronization failure is preserved");
        const auto retry=batch.WriteAt(offset,payload.buffer.data(),size);
        Check(retry.ok()&&retry.bytes_transferred==size&&batch.Sync().ok(),"original staged bytes can be exactly retried and synchronized");
        Check(!batch.WriteAt(offset,nullptr,1).ok()&&!batch.WriteAt(UINT64_MAX,payload.buffer.data(),size).ok(),"batched invalid buffer and overflowing range refuse");
      }
      Check(destination.manager.Snapshot().current_bytes==size&&destination.memory.Snapshot().allocated_bytes==size&&destination.ledger.Snapshot().current_bytes==size,"bounded selector image remains exactly charged");
      Check(file.device.Close().ok()&&file.device.Open(file.path.string(),d::FileOpenMode::open_existing).ok(),"reopen staged bounded selector");
      const auto read=file.device.ReadAt(file.value.header.page_number*u64(size),payload.buffer.data(),size);
      Check(read.ok()&&read.bytes_transferred==size&&std::equal(file.bytes.begin(),file.bytes.end(),payload.buffer.data()),"independent full physical bytes after reopen");
      Check(file.device.Close().ok()&&file.device.Open(file.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"open read-only batch control");
      {
        d::FileDevice::WriteLatencyBatch batch(file.device);const auto guard=file.device.AcquireOperationGuard();
        Check(!batch.WriteAt(file.value.header.page_number*u64(size),payload.buffer.data(),size).ok(),"batch preserves read-only write refusal");
        const auto sync=DenyCodecAllocation([&]{return batch.Sync();});Check(sync.ok(),"read-only sync keeps existing no-op semantics");
      }
      Check(file.device.Close().ok()&&file.device.Open(file.path.string(),d::FileOpenMode::open_existing).ok(),"restore original writable fixture");
      deallocation_device_mutex=file.device.AcquireOperationGuard().mutex();deallocation_lock_free=false;
      Check(payload.buffer.Reset().ok()&&deallocation_lock_free,"actual encoded payload released after device guard");
      destination.memory={};destination.Empty();
    }
    auto refused=file.Read(f);NoImage(refused);Check(refused.error==ME::memory_allocation_failure&&
      refused.memory_error==db::NativeStorageMemoryError::resource_exhausted&&!reads&&!refused.page_bytes_read,
      "retained image prevents cumulative overcommit before payload read");
    Check(f.manager.Snapshot().current_bytes==size,"failed second read preserves first payload charge");
    const auto revoked=f.ledger.CleanupOwner(f.binding.owner_uuid.bytes);
    Check(revoked.retained_bytes==size,"revocation retains page owner");
    refused=file.Read(f);NoImage(refused);Check(refused.error==ME::memory_binding_failure&&!reads,
      "revoked grant refused before any source read");
    f.memory={};Check(f.manager.Snapshot().current_bytes==size&&result.image.data()[0]==file.bytes[0],
      "retained reader survives workspace exit and revocation");
    bool cleaned=false;std::thread final_owner([image=std::move(result.image),&cleaned]() mutable {
      allocation_budget=0;cleaned=image.Reset().ok()&&allocation_budget==0;allocation_budget=-1;});final_owner.join();
    Check(cleaned,"worker-final page cleanup is allocation-free");f.Empty();
    MemoryFixture short_grant(size-1);refused=file.Read(short_grant);NoImage(refused);
    Check(refused.error==ME::memory_allocation_failure&&!reads&&!short_grant.manager.Snapshot().current_bytes,
      "one-byte-short grant never reads selector payload");short_grant.memory={};short_grant.Empty();
    MemoryFixture valid(size);
    deallocation_device_mutex=file.device.AcquireOperationGuard().mutex();deallocation_lock_free=false;
    const auto wrong_object=db::ReadNativeCheckpointSelectionWithMemoryFromOpenDevice(
      file.device,file.expected,Id(99),valid.memory,valid.binding);NoImage(wrong_object);
    Check(wrong_object.error==ME::object_mismatch&&!deallocation_device_mutex&&deallocation_lock_free,
      "failure releases device guard before node allocator cleanup");
    for(unsigned dimension=0;dimension<4;++dimension){auto wrong=valid.binding;
      std::array<Uuid*,4> fields{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};*fields[dimension]=Id(91);
      reads=0;const auto r=db::ReadNativeCheckpointSelectionWithMemoryFromOpenDevice(file.device,file.expected,file.value.object_uuid,valid.memory,wrong);
      NoImage(r);Check(r.error==ME::memory_binding_failure&&!reads&&!valid.manager.Snapshot().current_bytes,
        "each binary ownership dimension refuses before locks/reads/payload");}
    for(unsigned field=0;field<9;++field){auto expected=file.expected;auto object=file.value.object_uuid;
      if(field==0)expected.filespace.database_uuid=Id(99);
      if(field==1)expected.filespace.filespace_uuid={};if(field==2)expected.filespace.page_size_profile_uuid={};
      if(field==3)expected.page_number=0;if(field==4)expected.page_number=std::numeric_limits<u64>::max();
      if(field==5)expected.page_generation=0;if(field==6)expected.page_type=3;
      if(field==7)expected.page_uuid=Uuid{};if(field==8)object={};reads=0;
      const auto r=db::ReadNativeCheckpointSelectionWithMemoryFromOpenDevice(file.device,expected,object,valid.memory,valid.binding);
      NoImage(r);Check(!reads&&!valid.manager.Snapshot().current_bytes,"malformed target refuses without source reads or charge");}
    for(unsigned mode=0;mode<5;++mode){auto expected=file.expected;auto object=file.value.object_uuid;
      // The wrong-profile case must reach real bootstrap validation, not be
      // masked by the deliberately earlier page grant admission.
      MemoryFixture mismatch(d::kCanonicalFilespacePageProfiles.back().page_size_bytes);
      if(mode==0)expected.filespace.filespace_uuid=Id(99);if(mode==1)expected.page_generation++;
      if(mode==2)expected.page_uuid=Id(99);if(mode==3)object=Id(99);
      if(mode==4)expected.filespace.page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[(p+1)%5].uuid;
      const auto r=db::ReadNativeCheckpointSelectionWithMemoryFromOpenDevice(file.device,expected,object,mismatch.memory,mismatch.binding);
      NoImage(r);Check(r.error==(mode==0||mode==4?ME::bootstrap_failure:mode==3?ME::object_mismatch:ME::header_failure)&&
        !mismatch.manager.Snapshot().current_bytes,"actual file and header/object binding mismatch retains exact refusal");
      mismatch.memory={};mismatch.Empty();}
    for(unsigned fault=1;fault<=2;++fault)for(bool short_io:{false,true}){
      if(short_io){short_read=fault;eof_read=fault+1;}else fail_read=fault;const auto r=file.Read(valid);fail_read=short_read=eof_read=0;
      NoImage(r);Check(r.error==(fault==1?ME::bootstrap_failure:ME::io_failure)&&!valid.manager.Snapshot().current_bytes,
        "every physical read fails/shortens without leaked payload or prefix");
      if(fault==2)Check(r.page_bytes_read==(short_io?size-1:0),"exact partial page read count retained");}
    for(unsigned fault=1;fault<=2;++fault){short_read=fault;auto r=file.Read(valid);short_read=0;
      Check(r.ok()&&reads==3&&r.page_bytes_read==size,"device completes legal partial read without false truncation");}
    auto damaged=file.bytes;damaged.back()^=1;file.Store(damaged);const auto corrupt=file.Read(valid);NoImage(corrupt);
    Check(corrupt.error==ME::selection_failure&&corrupt.selection_error==E::invalid_integrity&&!valid.manager.Snapshot().current_bytes,"corrupt payload has no retained charge");file.Store(file.bytes);
    file.Bootstrap(1);const auto encrypted=file.Read(valid);NoImage(encrypted);
    Check(encrypted.error==ME::encrypted_requires_authority&&reads==1&&!valid.manager.Snapshot().current_bytes,"encrypted payload never falls back to plaintext");file.Bootstrap();
    for(unsigned mode=1;mode<=5;++mode){hash_fault=mode;const auto failed=file.Read(valid);NoImage(failed);
      Check(!hash_fault&&failed.error==ME::selection_failure&&failed.selection_error==E::hash_failure&&
        !valid.manager.Snapshot().current_bytes,"provider failures preserve typed failure and release actual image");}
    Check(file.device.Close().ok(),"close borrowed device explicitly");const auto closed=file.Read(valid);NoImage(closed);
    Check(closed.error==ME::bootstrap_failure&&closed.bootstrap_error==d::FilespaceBootstrapError::device_not_open,"reader never reopens closed source");
    valid.memory={};valid.Empty();
  }
  SelectorFile file(0);unsigned injected=0;bool terminal=false;
  for(long point=0;point<1024;++point){MemoryFixture f(file.bytes.size());allocation_budget=point;
    auto r=file.Read(f);const bool hit=allocation_budget<0;allocation_budget=-1;
    if(hit)++injected;
    if(!r.ok())NoImage(r);else Check(std::equal(file.bytes.begin(),file.bytes.end(),r.image.data()),"successful fault-sweep read retains bytes");
    r={};Check(!f.manager.Snapshot().current_bytes,"allocation-failure path releases every actual payload");
    auto retry=file.Read(f);Check(retry.ok(),"same owner remains retryable after each allocation failure");retry={};
    f.memory={};f.Empty();if(!hit){terminal=true;break;}
  }
  Check(terminal&&injected,"every measured allocation including aligned page payload faulted through terminal success");
  std::cout<<"governed selector profiles=5 read_fault_positions=2 allocation_faults="<<injected<<'\n';
}
#endif
void Test(){
  for(unsigned p=0;p<5;++p)for(bool successor:{false,true}){
    auto s=Example(p);if(successor)s=Next(s);const auto first=Oracle(s),second=Oracle(Other(s));const auto e=db::EncodeNativeCheckpointSelection(s);
    EncodingTest(s);
    Check(e.ok()&&e.bytes==first,"exact independently packed selector image");const auto decoded=db::DecodeNativeCheckpointSelection(first);Check(decoded.ok()&&Oracle(*decoded.selection)==first,"all selector fields preserved");
    // No global/new payload allocation is permitted, including at 128KiB.
    // The provider's own SHA context is a separate existing resource boundary.
    allocation_budget=0;const auto value=db::DecodeNativeCheckpointSelectionValue(first);
    const auto unchanged_budget=allocation_budget;allocation_budget=-1;
    Check(value.ok()&&unchanged_budget==0&&Oracle(*value.selection)==first,"fixed-value decoding does not copy caller image");
    Bytes unaligned(first.size()+2,0xa5);std::copy(first.begin(),first.end(),unaligned.begin()+1);
    const auto view=std::span<const byte>(unaligned).subspan(1,first.size());
    const auto from_view=db::DecodeNativeCheckpointSelectionValue(view);
    Check(from_view.ok()&&Oracle(*from_view.selection)==first&&unaligned.front()==0xa5&&unaligned.back()==0xa5,
      "unaligned bounded image view decoded without writing caller bytes");
    std::fill(unaligned.begin(),unaligned.end(),0);
    Check(Oracle(*from_view.selection)==first,"returned fixed values are independent of caller image lifetime");
    for(bool reverse:{false,true}){const auto pair=db::ClassifyNativeCheckpointSelectionPair(reverse?second:first,reverse?first:second);Check(pair.ok()&&pair.selection->selection_generation==s.selection_generation,"stable pair is independent of slot order");}
    auto torn=second;torn.back()=1;auto pair=db::ClassifyNativeCheckpointSelectionPair(first,torn);Empty(pair);Check(pair.error==E::repair_required,"one damaged slot cannot grant current-root selection");
    pair=db::ClassifyNativeCheckpointSelectionPair(first,Bytes(second.size(),0));Empty(pair);Check(pair.error==E::repair_required,"one missing slot cannot grant current-root selection");
    auto mixed=Other(Next(Example(p)));pair=db::ClassifyNativeCheckpointSelectionPair(Oracle(Example(p)),Oracle(mixed));Empty(pair);Check(pair.error==E::repair_required,"interrupted consecutive publication needs recovery");
    mixed.previous_checkpoint_sha256[0]^=1;pair=db::ClassifyNativeCheckpointSelectionPair(Oracle(Example(p)),Oracle(mixed));Empty(pair);Check(pair.error==E::invalid_pair,"contradictory predecessor is not repairable by choosing higher generation");
  }
  const auto first=Oracle(Example()),second=Oracle(Other(Example()));
  for(std::size_t i=0;i<first.size();++i){auto bad=first;bad[i]^=1;InvalidImage(bad);}
  for(unsigned at:{128u,136u,138u,140u,464u,511u,8191u}){auto b=first;b[at]^=1;Seal(b);InvalidImage(b);}
  for(std::size_t n:{0u,127u,511u,8191u,8193u}){auto b=first;b.resize(n);InvalidImage(b);}
  for(unsigned n=0;n<23;++n){auto s=Example();switch(n){
    case 0:s.header.page_type=8;break;case 1:s.header.flags=2;break;case 2:s.object_uuid={};break;case 3:s.bootstrap_uuid={};break;case 4:s.publication_uuid={};break;
    case 5:s.selection_generation=0;break;case 6:s.checkpoint_generation=0;break;case 7:s.root_set_generation=0;break;case 8:s.timeline_uuid={};break;case 9:s.checkpoint_object_uuid={};break;case 10:s.checkpoint_sha256.fill(0);break;
    case 11:s.checkpoint.page_number=0;break;case 12:s.checkpoint.page_number=s.header.page_number;break;case 13:s.checkpoint.page_generation=0;break;case 14:s.checkpoint.page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;break;
    case 15:s.previous_selection_generation=1;break;case 16:s.previous_checkpoint=s.checkpoint;break;case 17:s.previous_checkpoint_object_uuid=Id(20);break;case 18:s.previous_checkpoint_sha256.fill(1);break;
    case 19:s=Next(s);s.previous_selection_generation=0;break;case 20:s.checkpoint.page_number=std::numeric_limits<u64>::max();break;case 21:s.object_uuid=s.bootstrap_uuid;break;case 22:s.header.page_uuid=s.bootstrap_uuid;break;}Invalid(s);}
  for(unsigned n=0;n<13;++n){auto s=Other(Example());switch(n){
    case 0:s.publication_uuid=Id(24);break;case 1:s.checkpoint_sha256[0]^=1;break;case 2:s.checkpoint_generation++;break;case 3:s.root_set_generation++;break;case 4:s.timeline_uuid=Id(24);break;
    case 5:s.header.page_uuid=Id(3);break;case 6:s.header.page_number=21;break;case 7:s.header.database_uuid=Id(24);break;case 8:s.object_uuid=Id(24);break;
    case 9:s.bootstrap_uuid=Id(24);break;case 10:s.checkpoint.page_number=21;break;case 11:s=Next(s);s.selection_generation=3;s.previous_selection_generation=2;break;case 12:s.checkpoint_object_uuid=Id(24);break;}
    const auto image=Oracle(s);Check(db::DecodeNativeCheckpointSelection(image).ok(),"pair mismatch is individually valid image");const auto pair=db::ClassifyNativeCheckpointSelectionPair(first,image);Empty(pair);Check(pair.error==E::invalid_pair,"same-generation ambiguity and physical/lineage alias refused");}
  allocations=0;counting=true;const auto measured=db::ClassifyNativeCheckpointSelectionPair(first,second);counting=false;Check(measured.ok(),"measure successful pair");const auto count=allocations;
  Check(count==0,"pair classification must not allocate hidden copies of caller-owned page images");
  for(unsigned long n=0;n<=count;++n){allocation_budget=n;const auto pair=db::ClassifyNativeCheckpointSelectionPair(first,second);allocation_budget=-1;if(!pair.ok())Empty(pair);if(n==count)Check(pair.ok(),"allocation sweep terminal success");}
  for(unsigned mode=1;mode<=5;++mode){hash_fault=mode;const auto pair=DenyCodecAllocation([&]{return db::ClassifyNativeCheckpointSelectionPair(first,second);});Check(!hash_fault,"multipart failure consumed");Empty(pair);Check(pair.error==E::hash_failure,"provider failure is not classified as damaged durable data");}
  allocation_budget=0;const auto owned_refusal=db::DecodeNativeCheckpointSelection(first);allocation_budget=-1;
  Empty(owned_refusal);Check(owned_refusal.error==E::resource_exhausted,"owning compatibility decoder still reports image copy allocation failure");
  std::cout<<"pair allocations="<<count<<" checks="<<checks<<" image_only=true\n";
}
}
int main(){try{Test();
#if defined(SB_NATIVE_SELECTOR_MEMORY_TESTS)
MemoryTest();std::cout<<"governed selector checks="<<checks<<'\n';
#endif
return 0;}catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
