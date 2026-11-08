// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "filespace_page_zero.hpp"
#include "catalog_page.hpp"
#include "native_index_btree_page.hpp"
#include "physical_mga_cow_store.hpp"
#include "native_catalog_leaf_memory.hpp"
#include "native_btree_tree_memory.hpp"
#include "metric_observation_queue.hpp"
#include "catalog_schema_definition.hpp"
#include "catalog_context_namespace.hpp"
#include "catalog_metric_retention_policy.hpp"
#include "catalog_storage_action_policy.hpp"
#include "transaction_inventory_page.hpp"
#include "database_dirty_manifest.hpp"
#include "native_owned_checkpoint_source.hpp"
#ifdef SB_NATIVE_ROUTE_SOURCE_TESTS
#include "../../src/server/database_ownership.hpp"
#include <fcntl.h>
#include <sys/file.h>
#endif
#include "native_filespace_initialization.hpp"
#include "native_storage_policy_lookup.hpp"
#include "disk_device.hpp"
#include "uuid.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <source_location>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <unistd.h>

namespace {
long allocation_budget=-1;
unsigned long observed_allocations=0;
bool count_allocations=false;
unsigned hash_fault=0,reads=0,read_fault=0;
unsigned allocation_failure_reads=0;
unsigned full_digest_fault=0;
unsigned segmented_context_skip=0,observed_segmented_contexts=0;
bool selected_hash_context=true,count_segmented_contexts=false;
unsigned stage_write_fault=0,stage_sync_fault=0,stage_writes=0,stage_syncs=0;
unsigned stage_write_fault_after=0,stage_sync_fault_after=0;
bool allocation_fault_after_stage_write=false;
unsigned stage_corrupt_read=0;
unsigned initialization_entropy_fault=0,initialization_entropy_calls=0;
bool initialization_repeat_entropy=false;
unsigned char initialization_entropy_tag=5;
std::int64_t initialization_clock_offset_millis=0;
bool initialization_real_clock=false;
bool initialization_clock_failure=false,initialization_clock_allocation_failure=false;
constexpr std::uint64_t initialization_uuid_millis=0x010203040506ULL;
const scratchbird::core::platform::TypedUuid* revoke_on_stage_sync=nullptr;
unsigned observed_full_digests=0;
bool count_full_digests=false;
std::size_t observed_read_bytes=0;
std::uint64_t checkpoint_transferred_bytes=0;
bool track_reads=false;
bool deny_heap_on_next_read=false;
struct FencedHeapProbe {
  std::recursive_mutex* mutex=nullptr;
  std::atomic<unsigned> request{0},completed{0};
  std::atomic<bool> available{false},stop{false};
  unsigned outside_allocations=0;
  void Run() noexcept {
    while(!stop.load()){
      const auto next=request.load();
      if(next==completed.load()){std::this_thread::yield();continue;}
      const bool free=mutex->try_lock();if(free)mutex->unlock();
      available=free;completed=next;
    }
  }
  bool Outside() noexcept {
    const auto next=request.fetch_add(1)+1;
    while(completed.load()!=next)std::this_thread::yield();
    if(!available.load())return false;
    ++outside_allocations;return true;
  }
};
thread_local FencedHeapProbe* fenced_heap_probe=nullptr;
std::atomic<bool> pause_next_tree_read{false},tree_read_paused{false},resume_tree_read{false};
unsigned pause_tree_read_number=0;
const std::vector<unsigned char>* replace_on_second_read=nullptr;
bool extend_on_second_read=false;
struct MemoryDeviceLockProbe {
  std::array<std::recursive_mutex*,2> locks{};
  bool armed=false,available=false;
};
thread_local MemoryDeviceLockProbe allocation_lock_probe,deallocation_lock_probe;
void ObserveMemoryDeviceLocks(MemoryDeviceLockProbe& probe){
  if(!probe.armed)return;
  probe.armed=false;bool available=true;
  std::thread inspect([&]{for(auto* mutex:probe.locks){
    if(mutex->try_lock())mutex->unlock();else available=false;
  }});inspect.join();probe.available=available;
}
}
void* operator new(std::size_t bytes) {
  if(count_allocations) ++observed_allocations;
  if(allocation_budget==0&&!(fenced_heap_probe&&fenced_heap_probe->Outside())) {
    allocation_budget=-1; allocation_failure_reads=reads; throw std::bad_alloc(); }
  if(allocation_budget>0) --allocation_budget;
  if(auto* p=std::malloc(bytes?bytes:1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t bytes,std::align_val_t alignment){
  if(count_allocations)++observed_allocations;
  if(allocation_budget==0&&!(fenced_heap_probe&&fenced_heap_probe->Outside())){
    allocation_budget=-1;allocation_failure_reads=reads;throw std::bad_alloc();}
  if(allocation_budget>0)--allocation_budget;
  ObserveMemoryDeviceLocks(allocation_lock_probe);
  void* p=nullptr;if(posix_memalign(&p,static_cast<std::size_t>(alignment),bytes?bytes:1)==0)return p;
  throw std::bad_alloc();
}
void operator delete(void* p,std::align_val_t)noexcept{ObserveMemoryDeviceLocks(deallocation_lock_probe);std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t)noexcept{ObserveMemoryDeviceLocks(deallocation_lock_probe);std::free(p);}
extern "C" int __real_RAND_bytes(unsigned char*,int);
extern "C" int __wrap_RAND_bytes(unsigned char* out,int count) {
  ++initialization_entropy_calls;
  if(initialization_entropy_fault&&--initialization_entropy_fault==0)return 0;
  if(initialization_repeat_entropy){if(count!=16)return 0;
    const unsigned char collision[16]={1,2,3,4,5,6,0x71,8,0x89,10,11,12,13,14,15,initialization_entropy_tag};
    std::copy_n(collision,16,out);return 1;}
  return __real_RAND_bytes(out,count);
}
extern "C" scratchbird::core::time::ClockSnapshotResult __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
extern "C" scratchbird::core::time::ClockSnapshotResult __wrap__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv() {
  if(initialization_real_clock)return __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
  if(initialization_clock_allocation_failure)throw std::bad_alloc();
  scratchbird::core::time::ClockSnapshotResult result;
  if(initialization_clock_failure){result.status={scratchbird::core::platform::StatusCode::time_source_unavailable,scratchbird::core::platform::Severity::error,scratchbird::core::platform::Subsystem::time};return result;}
  const auto millis=static_cast<std::int64_t>(initialization_uuid_millis)+initialization_clock_offset_millis;
  result.value={{100},{millis/1000,static_cast<std::uint32_t>((millis%1000)*1000000)}};
  return result;
}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* b,size_t n,unsigned char* out,unsigned int* count,const EVP_MD* md,ENGINE* e) {
  if(count_full_digests) ++observed_full_digests;
  if(full_digest_fault && --full_digest_fault==0) return 0;
  return __real_EVP_Digest(b,n,out,count,md,e);
}
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new() {
  if(count_segmented_contexts)++observed_segmented_contexts;
  selected_hash_context=segmented_context_skip==0;
  if(segmented_context_skip)--segmented_context_skip;
  if(selected_hash_context&&hash_fault==1) { hash_fault=0; return nullptr; } return __real_EVP_MD_CTX_new();
}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* m,ENGINE* e) {
  if(selected_hash_context&&hash_fault==2) { hash_fault=0; return 0; } return __real_EVP_DigestInit_ex(c,m,e);
}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* b,size_t n) {
  if(selected_hash_context&&hash_fault==3) { hash_fault=0; return 0; } return __real_EVP_DigestUpdate(c,b,n);
}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* b,unsigned int* n) {
  if(selected_hash_context&&hash_fault==4) { hash_fault=0; return 0; }
  const int result=__real_EVP_DigestFinal_ex(c,b,n);
  if(selected_hash_context&&hash_fault==5) { hash_fault=0; *n=31; } return result;
}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_pwrite(int fd,const void* b,size_t n,off_t offset) {
  ++stage_writes;
  if(stage_write_fault&&(!stage_write_fault_after||--stage_write_fault_after==0)){const auto mode=stage_write_fault;stage_write_fault=0;
    if(mode==2&&n>1){const auto part=__real_pwrite(fd,b,n/2,offset);if(part<0)return part;}
    if(mode==3&&n>64){const auto part=__real_pwrite(fd,b,64,offset);if(part<0)return part;}
    if(mode==4&&n>64){const auto part=__real_pwrite(fd,b,64,offset);if(part<0)return part;stage_write_fault=1;return part;}
    if(allocation_fault_after_stage_write){allocation_fault_after_stage_write=false;allocation_budget=0;}
    errno=EIO;return -1;}
  return __real_pwrite(fd,b,n,offset);
}
extern "C" int __wrap_fsync(int fd) {
  ++stage_syncs;if(stage_sync_fault&&(!stage_sync_fault_after||--stage_sync_fault_after==0)){stage_sync_fault=0;errno=EIO;return -1;}
  const auto result=__real_fsync(fd);
  if(result==0&&revoke_on_stage_sync){const auto id=*revoke_on_stage_sync;revoke_on_stage_sync=nullptr;scratchbird::transaction::mga::RevokePublishedSnapshotVector(id);}
  return result;
}
extern "C" ssize_t __wrap_pread(int fd,void* b,size_t n,off_t offset) {
  if(deny_heap_on_next_read){deny_heap_on_next_read=false;allocation_budget=0;}
  if(pause_next_tree_read.exchange(false)||(pause_tree_read_number&&track_reads&&reads+1==pause_tree_read_number)) {
    tree_read_paused=true;while(!resume_tree_read.load())std::this_thread::yield();}
  if(track_reads) { ++reads; observed_read_bytes+=n; }
  if(read_fault&&reads==read_fault) { read_fault=0; errno=EIO; return -1; }
  if(extend_on_second_read&&reads==2){extend_on_second_read=false;
    const auto end=::lseek(fd,0,SEEK_END);const unsigned char zero=0;
    if(end<0||__real_pwrite(fd,&zero,1,end)!=1){errno=EIO;return -1;}}
  if(replace_on_second_read&&reads==2) {
    const auto* image=replace_on_second_read; replace_on_second_read=nullptr;
    if(::pwrite(fd,image->data(),image->size(),0)!=static_cast<ssize_t>(image->size())||::fsync(fd)!=0) {
      errno=EIO; return -1;
    }
  }
  const auto result=__real_pread(fd,b,n,offset);
  if(track_reads&&result>0)checkpoint_transferred_bytes+=static_cast<std::uint64_t>(result);
  if(stage_corrupt_read&&reads==stage_corrupt_read&&result>0){stage_corrupt_read=0;static_cast<unsigned char*>(b)[0]^=1;}
  return result;
}

namespace {
namespace disk=scratchbird::storage::disk;
using disk::byte; using disk::Uuid; using disk::u64;
using Error=disk::FilespacePageZeroError;
using Bytes=std::vector<byte>;
unsigned checks=0;
void Check(bool ok,std::string_view why,std::source_location at=std::source_location::current()) {
  ++checks; if(!ok) throw std::runtime_error(std::string(why)+" line="+std::to_string(at.line()));
}
Uuid Id(byte tag) { return Uuid{{1,2,3,4,5,6,0x71,8,0x89,10,11,12,13,14,15,tag}}; }
constexpr std::array<unsigned,5> sizes{{8192,16384,32768,65536,131072}};
constexpr std::array<std::array<byte,3>,5> suffixes{{
  {{0,0x81,0x92}},{{1,0x63,0x84}},{{3,0x27,0x68}},{{6,0x55,0x36}},{{0x13,0x10,0x72}}}};
constexpr std::array<unsigned,18> root_types{{0,8,5,3,769,9,10,11,5,768,771,773,775,779,1301,1030,1025,777}};
Uuid Profile(unsigned p) { return Uuid{{0,0,0,0,0,0,0x70,0,0x80,0,0,0,0,suffixes[p][0],suffixes[p][1],suffixes[p][2]}}; }
disk::FilespacePageZero Example(unsigned p=0,unsigned role=1,unsigned state=1,unsigned flags=0) {
  disk::FilespacePageZero v; auto& h=v.bootstrap;
  h.database_uuid=Id(1); h.filespace_uuid=Id(2); h.page_size_profile_uuid=Profile(p);
  h.page_size_bytes=sizes[p]; h.filespace_role=static_cast<disk::u16>(role);
  h.lifecycle_state=static_cast<disk::u16>(state); h.flags=flags;
  h.checksum_profile_uuid=Uuid{{0x01,0xa0,0x8e,0x13,0x41,0x6b,0x73,0xf6,0xb3,0xc5,0xbd,0xde,0x88,0xb1,0xf2,0xd9}};
  if(flags&1) h.encryption_profile_uuid=Id(4);
  v.page_uuid=Id(3); v.creation_operation_uuid=Id(5); v.writer_identity_uuid=Id(6);
  v.page_generation=7; v.root_set_generation=8; v.total_pages=64; v.free_pages=10;
  v.preallocated_pages=11; v.creation_utc_millis=1790000000000ull;
  for(unsigned kind=1;kind<=9;++kind) if(role<=4||kind==3)
    v.roots.push_back({static_cast<disk::u16>(kind),root_types[kind],h.filespace_uuid,
      kind+10,kind+100,h.page_size_profile_uuid,Id(static_cast<byte>(40+kind))});
  return v;
}
void Number(Bytes& b,std::size_t at,unsigned width,u64 value) {
  for(unsigned n=0;n<width;++n) b[at+n]=static_cast<byte>(value>>(8*n));
}
void PutUuid(Bytes& b,std::size_t at,Uuid id) {
  for(unsigned n=0;n<16;++n) b[at+n]=id.bytes[n];
}
void CommonSeal(Bytes& b) {
  u64 fnv=14695981039346656037ull;
  for(unsigned i=0;i<128;++i) { fnv^=(i>=96&&i<104)?0:b[4096+i]; fnv*=1099511628211ull; }
  Number(b,4192,8,fnv);
}
void FullSeal(Bytes& b) {
  std::fill(b.begin()+4448,b.begin()+4480,0); std::array<byte,32> digest{};
  Check(SHA256(b.data(),b.size(),digest.data())!=nullptr,"independent full SHA256");
  std::copy(digest.begin(),digest.end(),b.begin()+4448);
}
Bytes Oracle(const disk::FilespacePageZero& v) {
  const auto& h=v.bootstrap; Bytes b(h.page_size_bytes,0);
  const std::string_view pm="SBFP",cm="SBPGV002",fm="SBFZV001";
  std::copy(pm.begin(),pm.end(),b.begin()); Number(b,4,2,1); Number(b,6,2,4096);
  Number(b,8,4,h.page_size_bytes); Number(b,12,4,h.flags); PutUuid(b,16,h.database_uuid);
  PutUuid(b,32,h.filespace_uuid); PutUuid(b,48,h.page_size_profile_uuid);
  Number(b,64,4,h.durable_format_generation); Number(b,68,2,h.filespace_role); Number(b,70,2,h.lifecycle_state);
  PutUuid(b,72,h.checksum_profile_uuid); PutUuid(b,88,h.encryption_profile_uuid);
  Check(SHA256(b.data(),104,b.data()+104)!=nullptr,"independent preamble SHA256");
  std::copy(cm.begin(),cm.end(),b.begin()+4096); Number(b,4104,4,128);
  Number(b,4108,4,h.page_size_bytes); Number(b,4112,4,h.filespace_role<=4?1:2);
  Number(b,4116,2,1); Number(b,4118,2,1); PutUuid(b,4120,h.database_uuid);
  PutUuid(b,4136,h.filespace_uuid); PutUuid(b,4152,v.page_uuid); Number(b,4176,8,v.page_generation);
  Number(b,4184,8,h.flags&2); PutUuid(b,4200,h.page_size_profile_uuid); Number(b,4216,2,1); CommonSeal(b);
  constexpr unsigned f=4224;
  std::copy(fm.begin(),fm.end(),b.begin()+f); Number(b,f+8,4,256); Number(b,f+12,4,v.roots.size());
  Number(b,f+16,8,256+80*v.roots.size()); Number(b,f+24,8,v.root_set_generation);
  PutUuid(b,f+32,h.database_uuid); PutUuid(b,f+48,h.filespace_uuid); PutUuid(b,f+64,h.page_size_profile_uuid);
  PutUuid(b,f+80,h.checksum_profile_uuid); PutUuid(b,f+96,h.encryption_profile_uuid); PutUuid(b,f+112,v.page_uuid);
  Number(b,f+128,4,h.page_size_bytes); Number(b,f+132,4,h.durable_format_generation);
  Number(b,f+136,2,h.filespace_role); Number(b,f+138,2,h.lifecycle_state); Number(b,f+140,4,h.flags);
  Number(b,f+144,8,v.total_pages); Number(b,f+152,8,v.free_pages); Number(b,f+160,8,v.preallocated_pages);
  Number(b,f+168,8,v.page_generation); PutUuid(b,f+176,v.creation_operation_uuid); PutUuid(b,f+192,v.writer_identity_uuid);
  Number(b,f+208,8,v.creation_utc_millis); Number(b,f+216,4,80);
  unsigned at=4480;
  for(const auto& r:v.roots) {
    Number(b,at,2,r.kind); Number(b,at+4,4,r.page_type); PutUuid(b,at+8,r.filespace_uuid);
    Number(b,at+24,8,r.page_number); Number(b,at+32,8,r.page_generation);
    PutUuid(b,at+40,r.page_size_profile_uuid); PutUuid(b,at+56,r.object_uuid); at+=80;
  }
  FullSeal(b); return b;
}
void Reject(const Bytes& b,Error expected,std::source_location at=std::source_location::current()) {
  const auto r=disk::DecodeFilespacePageZero(b.data(),b.size());
  Check(!r.record&&r.error==expected,"precise atomic page-zero refusal actual="+
      std::to_string(static_cast<unsigned>(r.error))+" expected="+
      std::to_string(static_cast<unsigned>(expected)),at);
}
void Invalid(const disk::FilespacePageZero& v,Error expected) {
  Reject(Oracle(v),expected); const auto e=disk::EncodeFilespacePageZero(v);
  Check(!e.bytes&&e.error==expected,"invalid producer emits no bytes");
}
void Codecs() {
  for(unsigned k=0;k<root_types.size();++k) Check(disk::CanonicalPageZeroRootPageType(static_cast<disk::u16>(k))==root_types[k],"exact root symbol mapping");
  Check(disk::CanonicalPageZeroRootPageType(65535)==0,"unknown root kind");
  for(unsigned p=0;p<5;++p) for(unsigned role=1;role<=14;++role)
    for(unsigned state=1;state<=15;++state) for(unsigned flags=0;flags<4;++flags) {
      const auto v=Example(p,role,state,flags); const auto b=Oracle(v);
      const auto e=disk::EncodeFilespacePageZero(v);
      Check(e.ok()&&*e.bytes==b,"independent complete page-zero bytes");
      const disk::FilespaceBootstrapBinding binding{v.bootstrap.database_uuid,v.bootstrap.filespace_uuid,v.bootstrap.page_size_profile_uuid};
      const auto d=disk::DecodeFilespacePageZero(b.data(),b.size(),&binding);
      Check(d.ok()&&Oracle(*d.record)==b,"complete decoded metadata and bound identity");
    }
  const auto good=Oracle(Example());
  for(unsigned at=0;at<good.size();++at) for(unsigned bit=0;bit<8;++bit) {
    auto b=good; b[at]^=static_cast<byte>(1u<<bit);
    const auto d=disk::DecodeFilespacePageZero(b.data(),b.size()); Check(!d.ok()&&!d.record,"all image bits integrity protected");
  }
  for(unsigned n=0;n<good.size();++n) Check(!disk::DecodeFilespacePageZero(good.data(),n).record,"all image truncations atomic");
  Check(!disk::DecodeFilespacePageZero(nullptr,good.size()).record,"null image");
  Check(!disk::DecodeFilespacePageZero(good.data(),good.size()+1).record,"trailing image");
  for(unsigned offset : {4104u,4108u,4112u,4116u,4118u,4120u,4136u,4168u,4184u,4200u,4216u,4218u}) {
    auto b=good; b[offset]^=1; CommonSeal(b); FullSeal(b); Reject(b,Error::invalid_common_header);
  }
  // A different valid page UUID is valid common-header structure but must fail
  // the family duplicate-identity check, not pretend the UUID itself is invalid.
  { auto b=good; b[4152]^=1; CommonSeal(b); FullSeal(b); Reject(b,Error::invalid_family); }
  for(unsigned offset : {4256u,4272u,4288u,4304u,4320u,4336u,4352u,4356u,4360u,4362u,4364u,4392u,4440u,4444u}) {
    auto b=good; b[offset]^=1; FullSeal(b); Reject(b,Error::invalid_family);
  }
  auto number=[&](unsigned at,unsigned width,u64 value,Error error) { auto b=good; Number(b,at,width,value); FullSeal(b); Reject(b,error); };
  number(4236,4,33,Error::invalid_family); number(4482,2,1,Error::invalid_root_directory);
  number(4552,8,1,Error::invalid_root_directory); number(5200,1,1,Error::invalid_family);
  { auto v=Example(); v.roots.erase(v.roots.begin()); Invalid(v,Error::required_root_missing); }
  { auto v=Example(); v.roots[0].kind=0; Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].page_type=13; Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); std::swap(v.roots[0],v.roots[1]); Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].page_number=0; Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].page_generation=0; Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].page_number=64; Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].page_number=std::numeric_limits<u64>::max(); Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].page_size_profile_uuid=Profile(1); Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].object_uuid={}; Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[2].filespace_uuid=Id(50); Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].filespace_uuid=Id(50); v.roots[1].filespace_uuid=Id(50);
    v.roots[1].page_size_profile_uuid=Profile(1); Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].page_number=v.roots[1].page_number; Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.free_pages=64; Invalid(v,Error::invalid_capacity); }
  { auto v=Example(); v.preallocated_pages=54; Invalid(v,Error::invalid_capacity); }
  { auto v=Example(); v.total_pages=std::numeric_limits<u64>::max(); Invalid(v,Error::invalid_capacity); }
  { auto v=Example(); v.page_generation=0; Check(!disk::EncodeFilespacePageZero(v).bytes,"zero page generation"); }
  { auto v=Example(); v.root_set_generation=0; Invalid(v,Error::invalid_family); }
  { auto v=Example(); v.writer_identity_uuid={}; Invalid(v,Error::invalid_family); }
  { auto v=Example(); v.creation_operation_uuid.bytes[6]=0x41; Invalid(v,Error::invalid_family); }
  for(unsigned kind=10;kind<=17;++kind) {
    auto v=Example(0,1,1,2); v.roots.push_back({static_cast<disk::u16>(kind),root_types[kind],v.bootstrap.filespace_uuid,
      30+kind,1,v.bootstrap.page_size_profile_uuid,Id(static_cast<byte>(60+kind))});
    Check(disk::EncodeFilespacePageZero(v).ok(),"all optional root kinds");
    if(kind>=15) { v.bootstrap.flags=0; Invalid(v,Error::invalid_root_directory); }
  }
  { auto v=Example(); v.roots[7]=v.roots[1]; v.roots[7].kind=8;
    Check(disk::EncodeFilespacePageZero(v).ok(),"identical shared catalog/feature authority alias"); }
  { auto v=Example(0,5,7); v.roots.clear(); v.total_pages=1; v.free_pages=0; v.preallocated_pages=0;
    Check(disk::EncodeFilespacePageZero(v).ok(),"non-serving initialization record, not open success");
    v.bootstrap.lifecycle_state=1; Invalid(v,Error::required_root_missing); }
  for(unsigned fault=1;fault<=5;++fault) {
    auto v=Example(); hash_fault=fault; const auto e=disk::EncodeFilespacePageZero(v);
    Check(!e.bytes&&e.error==Error::hash_provider_failure&&hash_fault==0,"encode real digest provider failure");
    hash_fault=fault; const auto d=disk::DecodeFilespacePageZero(good.data(),good.size());
    Check(!d.record&&d.error==Error::hash_provider_failure&&hash_fault==0,"decode real digest provider failure");
  }
  for(unsigned i=0;i<2;++i) {
    auto v=Example(); allocation_budget=0;
    if(i==0) { const auto e=disk::EncodeFilespacePageZero(v); allocation_budget=-1;
      Check(!e.bytes&&e.error==Error::resource_exhausted,"encode allocation atomicity"); }
    else { const auto d=disk::DecodeFilespacePageZero(good.data(),good.size()); allocation_budget=-1;
      Check(!d.record&&d.error==Error::resource_exhausted,"decode allocation atomicity"); }
  }
}
bool Locked(const disk::IoResult& r) {
  return !r.ok()&&(r.diagnostic.diagnostic_code=="SB-STORAGE-DISK-OWNER-LOCK-HELD"
      ||r.diagnostic.diagnostic_code=="SB-STORAGE-DISK-DATA-OWNER-LOCK-HELD"
      ||r.diagnostic.diagnostic_code=="SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD");
}
struct Fixture {
  std::filesystem::path root;
  Fixture() { std::string s=(std::filesystem::temp_directory_path()/"sb_pagezero.XXXXXX").string();
    std::vector<char> b(s.begin(),s.end()); b.push_back(0); const auto* p=::mkdtemp(b.data()); Check(p,"temp root"); root=p; }
  ~Fixture() { std::error_code e; std::filesystem::remove_all(root,e); }
};
void Exclusive(const std::string& path) {
  disk::FileDevice second; Check(Locked(second.Open(path,disk::FileOpenMode::open_existing)),"local owner exclusion");
  const auto pid=::fork(); Check(pid>=0,"fork ownership probe");
  if(pid==0) { ::execl("/proc/self/exe","pagezero_test","--probe",path.c_str(),nullptr); ::_exit(125); }
  int s=0; Check(::waitpid(pid,&s,0)==pid&&WIFEXITED(s)&&WEXITSTATUS(s)==0,"fresh-exec process excluded");
}
void Files() {
  Fixture fixture;
  for(unsigned p=0;p<5;++p) {
    const auto path=(fixture.root/std::to_string(p)).string(); auto v=Example(p); const auto bytes=Oracle(v);
    disk::FileDevice device; Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"own fixture file");
    Check(device.WriteAt(0,bytes.data(),bytes.size()).ok(),"write actual page zero");
    const byte zero=0; Check(device.WriteAt(v.total_pages*sizes[p]-1,&zero,1).ok()&&device.Sync().ok(),"physical capacity");
    reads=0; observed_read_bytes=0; track_reads=true;
    auto r=disk::ReadFilespacePageZeroFromOpenDevice(device); track_reads=false;
    Check(r.ok()&&Oracle(*r.record)==bytes&&reads==2&&observed_read_bytes==4096+sizes[p],"exact probe then full-page read");
    Exclusive(path);
    for(unsigned fault=1;fault<=2;++fault) {
      reads=0; read_fault=fault; track_reads=true; r=disk::ReadFilespacePageZeroFromOpenDevice(device); track_reads=false;
      Check(!r.record&&r.error==Error::io_failure&&read_fault==0,"actual first/second pread failure");
    }
    auto changed=v; changed.bootstrap.database_uuid=Id(90); const auto alternate=Oracle(changed);
    reads=0; track_reads=true; replace_on_second_read=&alternate;
    r=disk::ReadFilespacePageZeroFromOpenDevice(device); track_reads=false;
    Check(!r.record&&r.error==Error::probe_changed&&!replace_on_second_read,"actual intervening page replacement rejected");
    Check(device.WriteAt(0,bytes.data(),bytes.size()).ok()&&device.Sync().ok(),"restore own race fixture");
    Exclusive(path);
    Check(device.Close().ok()&&device.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"readonly reopen");
    r=disk::ReadFilespacePageZeroFromOpenDevice(device); Check(r.ok()&&device.read_only(),"readonly complete metadata probe");
    const disk::FilespaceBootstrapBinding wrong{Id(77),v.bootstrap.filespace_uuid,v.bootstrap.page_size_profile_uuid};
    r=disk::ReadFilespacePageZeroFromOpenDevice(device,&wrong); Check(!r.record&&r.error==Error::invalid_bootstrap,"wrong node binding");
    Check(device.Close().ok(),"close fixture"); r=disk::ReadFilespacePageZeroFromOpenDevice(device);
    Check(!r.record&&r.error==Error::device_not_open,"closed owned reader");
  }
  const auto path=(fixture.root/"capacity-mismatch").string(); auto v=Example(); const auto bytes=Oracle(v);
  disk::FileDevice d; Check(d.Open(path,disk::FileOpenMode::create_new).ok(),"capacity fixture");
  Check(d.WriteAt(0,bytes.data(),bytes.size()).ok(),"only pagezero persisted");
  auto r=disk::ReadFilespacePageZeroFromOpenDevice(d); Check(!r.record&&r.error==Error::invalid_capacity,"no fabricated remaining pages");
  const byte z=0; Check(d.WriteAt(v.total_pages*sizes[0],&z,1).ok(),"actual unaccounted appended byte");
  r=disk::ReadFilespacePageZeroFromOpenDevice(d); Check(!r.record&&r.error==Error::invalid_capacity,"partial trailing page refused");
  using Relation=disk::FilespaceExtentRelation;
  const auto empty=[](const auto& result){Check(!result.ok()&&!result.record&&!result.declared_bytes&&
    !result.observed_bytes&&!result.complete_pages&&!result.trailing_bytes&&result.relation==Relation::unknown,"failed recovery observation exposes no partial facts");};
  for(unsigned p=0;p<5;++p){const auto expected=Example(p);const auto image=Oracle(expected);const u64 size=sizes[p],declared=64*size;
    const auto recovery_path=(fixture.root/("recovery-"+std::to_string(p))).string();disk::FileDevice file;
    Check(file.Open(recovery_path,disk::FileOpenMode::create_new).ok(),"own recovery observation fixture");
    const disk::FilespaceBootstrapBinding binding{expected.bootstrap.database_uuid,expected.bootstrap.filespace_uuid,expected.bootstrap.page_size_profile_uuid};
    const auto observe=[&](){return disk::ObserveFilespacePageZeroForRecoveryFromOpenDevice(file,binding);};
    const auto verify=[&](const auto& result,u64 physical){Check(result.ok()&&Oracle(*result.record)==image&&
      result.declared_bytes==declared&&result.observed_bytes==physical&&result.complete_pages==physical/size&&
      result.trailing_bytes==physical%size&&result.relation==(physical==declared?Relation::matching:physical<declared?Relation::shorter:Relation::longer),"exact declared versus physical recovery facts without rounding");};
    for(const u64 physical:{size-1,size,declared-size,declared-1,declared,declared+1,declared+size}){
      Check(file.WriteAt(0,image.data(),image.size()).ok(),"restore intact fixture metadata");std::filesystem::resize_file(recovery_path,physical);
      Check(file.Close().ok()&&file.Open(recovery_path,disk::FileOpenMode::open_existing_read_only).ok(),"independent readonly recovery reopen");
      stage_writes=stage_syncs=0;const auto result=observe();
      Check(!stage_writes&&!stage_syncs,"recovery observation never repairs writes or syncs");
      if(physical<size){empty(result);Check(result.error==Error::invalid_capacity,"incomplete page-zero image is not recoverable metadata");}
      else verify(result,physical);
      const auto strict=disk::ReadFilespacePageZeroFromOpenDevice(file,&binding);
      Check(physical==declared?strict.ok():(!strict.record&&strict.error==Error::invalid_capacity),"ordinary admission remains strict for every size discrepancy");
      Check(file.Close().ok()&&file.Open(recovery_path,disk::FileOpenMode::open_existing).ok(),"restore retained writable test handle");
    }
    auto foreign=binding;foreign.database_uuid=Id(77);const auto refused=disk::ObserveFilespacePageZeroForRecoveryFromOpenDevice(file,foreign);
    empty(refused);Check(refused.error==Error::invalid_bootstrap,"recovery cannot bypass binary node binding");
    if(p==0){
      for(unsigned at=1;at<=2;++at){reads=0;read_fault=at;track_reads=true;const auto failed=observe();track_reads=false;
        empty(failed);Check(!read_fault&&failed.error==Error::io_failure,"each native recovery read fault retained");}
      for(unsigned at=1;at<=5;++at){hash_fault=at;const auto failed=observe();empty(failed);
        Check(!hash_fault&&failed.error==Error::hash_provider_failure,"recovery hash provider failure is not capacity evidence");}
      observed_allocations=0;count_allocations=true;const auto measured=observe();count_allocations=false;verify(measured,declared+size);const auto sites=observed_allocations;
      for(unsigned long at=0;at<sites;++at){const auto lost=file.failed_io_latency_observations();allocation_budget=at;const auto result=observe();allocation_budget=-1;
        if(result.ok()){verify(result,declared+size);Check(file.failed_io_latency_observations()==lost+1,"only nonauthoritative telemetry loss permits recovery observation");}
        else{empty(result);Check(result.error==Error::resource_exhausted,"recovery allocation failure remains exact");}}
      reads=0;track_reads=true;extend_on_second_read=true;const auto changed=observe();track_reads=false;
      empty(changed);Check(!extend_on_second_read&&changed.error==Error::probe_changed,"intervening physical extent change withholds recovery facts");
      verify(observe(),declared+size+1);
      auto damaged=image;damaged.back()^=1;Check(file.WriteAt(0,damaged.data(),damaged.size()).ok(),"persist corrupt recovery fixture");
      const auto padding=observe();empty(padding);Check(padding.error==Error::invalid_family,"recovery refuses malformed reserved padding before digest admission");
      damaged=image;damaged[4448]^=1;Check(file.WriteAt(0,damaged.data(),damaged.size()).ok(),"persist corrupt recovery image seal");
      const auto corrupt=observe();empty(corrupt);Check(corrupt.error==Error::integrity_mismatch,"recovery never bypasses full-image integrity");
    }
    Check(file.Close().ok(),"release recovery observation fixture");const auto closed=observe();empty(closed);Check(closed.error==Error::device_not_open,"closed recovery device refused");
  }
}
namespace page=scratchbird::storage::page;
using RootError=page::NativeCatalogRootError;
page::NativeCatalogRoot RootExample(unsigned p=0) {
  page::NativeCatalogRoot r;
  r.header={sizes[p],5,Id(1),Id(2),Id(90),12,102,0,Profile(p)};
  r.object_uuid=Id(42); r.creator_transaction_uuid=Id(91);
  r.creator_local_transaction_id=17; r.catalog_generation=1;
  r.schema_epoch=2; r.security_epoch=3;
  for(unsigned role=1;role<=6;++role)
    r.roots.push_back({static_cast<disk::u16>(role),role%2?6u:512u,
      {Id(2),20+role,7,Profile(p)},Id(static_cast<byte>(100+role))});
  return r;
}
void RootSeal(Bytes& b) {
  std::fill(b.begin()+304,b.begin()+336,0);
  std::array<byte,32> digest{};
  Check(SHA256(b.data(),b.size(),digest.data())!=nullptr,"independent catalog SHA256");
  std::copy(digest.begin(),digest.end(),b.begin()+304);
}
Bytes RootOracle(const page::NativeCatalogRoot& r) {
  Bytes b(r.header.page_size_bytes,0); const auto& h=r.header;
  const std::string_view cm="SBPGV002",fm="SBCROOT1";
  std::copy(cm.begin(),cm.end(),b.begin()); Number(b,8,4,128);
  Number(b,12,4,h.page_size_bytes); Number(b,16,4,h.page_type);
  Number(b,20,2,1); Number(b,22,2,1); PutUuid(b,24,h.database_uuid);
  PutUuid(b,40,h.filespace_uuid); PutUuid(b,56,h.page_uuid);
  Number(b,72,8,h.page_number); Number(b,80,8,h.page_generation);
  Number(b,88,8,h.flags); PutUuid(b,104,h.page_size_profile_uuid); Number(b,120,2,1);
  u64 fnv=14695981039346656037ull;
  for(unsigned i=0;i<128;++i) { fnv^=b[i]; fnv*=1099511628211ull; }
  Number(b,96,8,fnv);
  std::copy(fm.begin(),fm.end(),b.begin()+128); Number(b,136,2,1); Number(b,138,2,256);
  Number(b,140,4,384+80*r.roots.size()); Number(b,144,2,r.root_kind);
  Number(b,146,2,r.roots.size()); Number(b,152,8,r.catalog_generation);
  Number(b,160,8,r.schema_epoch); Number(b,168,8,r.security_epoch);
  Number(b,176,8,r.resource_epoch); Number(b,184,8,r.creator_local_transaction_id);
  PutUuid(b,192,r.object_uuid); PutUuid(b,208,r.creator_transaction_uuid);
  auto put_ref=[&](std::size_t at,const page::NativeCatalogPageReference& ref) {
    PutUuid(b,at,ref.filespace_uuid); Number(b,at+16,8,ref.page_number);
    Number(b,at+24,8,ref.page_generation); PutUuid(b,at+32,ref.page_size_profile_uuid);
  };
  if(r.predecessor) put_ref(224,*r.predecessor);
  std::copy(r.predecessor_sha256.begin(),r.predecessor_sha256.end(),b.begin()+272);
  for(std::size_t i=0;i<r.roots.size();++i) {
    const auto& ref=r.roots[i]; const auto at=384+80*i;
    Number(b,at,2,ref.role); Number(b,at+4,4,ref.page_type); put_ref(at+8,ref.page);
    PutUuid(b,at+56,ref.object_uuid);
  }
  RootSeal(b); return b;
}
page::NativeBtreePage BtreeExample(unsigned p=0,unsigned type=0x200,bool branch=false) {
  page::NativeBtreePage r;r.header={sizes[p],type,Id(1),Id(2),Id(90),12,102,0,Profile(p)};
  r.dependencies={Id(42),3,8,Id(43),Id(44),Id(45),{}};r.dependencies.dependency_map_sha256.fill(19);
  r.creator_transaction_uuid=Id(98);r.creator_local_transaction_id=17;r.maintenance_state=1;r.tree_level=branch?1:0;
  const auto ref=[&](u64 n){return disk::NativePageReference{Id(2),n,7,Profile(p)};};
  if(type!=0x200){r.parent=ref(9);r.left=ref(10);r.right=ref(11);
    r.low_fence=page::NativeBtreeKey{{0},Id(60),Id(61)};r.high_fence=page::NativeBtreeKey{{255,1},Id(60),Id(61)};}
  if(branch)r.first_child=ref(20);
  for(unsigned i=0;i<3;++i){page::NativeBtreeCell c;c.key={{static_cast<byte>(i==2?255:0)},Id(static_cast<byte>(70+i)),Id(static_cast<byte>(80+i))};
    if(i==1)c.key.encoded_key.push_back(0);if(branch)c.child=ref(21+i);
    else{c.deleted=i==0;c.base_page=disk::NativePageReference{Id(7),50+i,4,Profile((p+1)%5)};}r.cells.push_back(c);}
  return r;
}
void BtreeSeal(Bytes& b) {std::fill(b.begin()+512,b.begin()+544,0);std::array<byte,32> digest{};
  Check(SHA256(b.data(),b.size(),digest.data())!=nullptr,"independent full native B-tree hash");std::copy(digest.begin(),digest.end(),b.begin()+512);}
template<class Page> Bytes BtreeOracle(const Page& r) {
  auto common=RootExample();common.header=r.header;auto b=RootOracle(common);std::fill(b.begin()+128,b.end(),0);
  const std::string_view magic="SBBTP001";std::copy(magic.begin(),magic.end(),b.begin()+128);Number(b,136,2,1);Number(b,138,2,512);
  const auto& d=r.dependencies;PutUuid(b,144,d.index_uuid);Number(b,160,8,d.descriptor_generation);Number(b,168,8,d.storage_generation);
  PutUuid(b,176,d.key_profile_uuid);PutUuid(b,192,d.visibility_profile_uuid);PutUuid(b,208,d.dependency_map_uuid);
  std::copy(d.dependency_map_sha256.begin(),d.dependency_map_sha256.end(),b.begin()+224);PutUuid(b,256,r.creator_transaction_uuid);
  Number(b,272,8,r.creator_local_transaction_id);Number(b,280,2,r.maintenance_state);Number(b,282,2,r.tree_level);
  const auto ref=[&](std::size_t at,const auto& p){if(!p)return;PutUuid(b,at,p->filespace_uuid);Number(b,at+16,8,p->page_number);Number(b,at+24,8,p->page_generation);PutUuid(b,at+32,p->page_size_profile_uuid);};
  ref(288,r.parent);ref(336,r.left);ref(384,r.right);ref(432,r.first_child);
  Number(b,496,4,r.cells.size());Number(b,500,4,std::count_if(r.cells.begin(),r.cells.end(),[](const auto& c){return c.deleted;}));Number(b,504,4,640);
  std::size_t at=640+4*r.cells.size();const auto fence=[&](const auto& k,std::size_t slot){if(!k)return;Number(b,slot,4,at);Number(b,slot+4,4,40+k->encoded_key.size());
    Number(b,at,4,k->encoded_key.size());PutUuid(b,at+8,k->row_uuid);PutUuid(b,at+24,k->version_uuid);std::copy(k->encoded_key.begin(),k->encoded_key.end(),b.begin()+at+40);at+=40+k->encoded_key.size();};
  fence(r.low_fence,480);fence(r.high_fence,488);
  for(unsigned i=0;i<r.cells.size();++i){const auto& c=r.cells[i];Number(b,640+4*i,4,at);Number(b,at,4,144+c.key.encoded_key.size());Number(b,at+4,4,c.key.encoded_key.size());Number(b,at+8,4,c.deleted?1:0);
    PutUuid(b,at+16,c.key.row_uuid);PutUuid(b,at+32,c.key.version_uuid);ref(at+48,c.child);ref(at+96,c.base_page);std::copy(c.key.encoded_key.begin(),c.key.encoded_key.end(),b.begin()+at+144);at+=144+c.key.encoded_key.size();}
  Number(b,140,4,at);BtreeSeal(b);return b;
}
struct BorrowedBtreeWorkspace {
  std::vector<page::NativeBtreeCellView> cells;
  std::vector<page::NativeBtreeReferenceSlot> references;
  explicit BorrowedBtreeWorkspace(std::size_t bytes){
    const auto required=page::NativeBtreePageViewBackingRequirements(bytes);
    cells.resize(required.cells);references.resize(required.reference_slots);
  }
  page::NativeBtreeViewWorkspace get(){return {cells,references};}
};
void BtreeBorrowedCheck(const Bytes& image,const page::NativeBtreePageResult& owning) {
  BorrowedBtreeWorkspace scratch(image.size());allocation_budget=0;
  const auto view=page::DecodeNativeBtreePageInto(image,scratch.get());
  const auto remaining=allocation_budget;allocation_budget=-1;
  Check(remaining==0&&view.error==owning.error&&view.ok()==owning.ok(),
    "owning and bounded B-tree readers preserve complete admission without C++ allocation");
  if(!view.ok()){Check(!view.page&&view.bytes.empty(),"borrowed B-tree failure exposes no page prefix");return;}
  Check(view.bytes.data()==image.data()&&view.bytes.size()==image.size()&&BtreeOracle(*view.page)==image,
    "every borrowed B-tree field independently reconstructs the exact original bytes");
  const auto resident=[&](const auto& key){return key.encoded_key.data()>=image.data()&&
    key.encoded_key.data()+key.encoded_key.size()<=image.data()+image.size();};
  if(view.page->low_fence)Check(resident(*view.page->low_fence),"low fence borrows exact immutable image");
  if(view.page->high_fence)Check(resident(*view.page->high_fence),"high fence borrows exact immutable image");
  for(const auto& cell:view.page->cells)Check(resident(cell.key),"every B-tree key borrows exact immutable image");
}
void BtreeBorrowedBounds() {
  using E=page::NativeBtreeError;
  for(unsigned profile=0;profile<5;++profile){
    auto image=BtreeOracle(BtreeExample(profile));const auto original=image;
    BorrowedBtreeWorkspace scratch(image.size());
    const auto refuse=[&](page::NativeBtreeViewWorkspace workspace,E error){
      allocation_budget=0;const auto result=page::DecodeNativeBtreePageInto(image,workspace);
      const auto remaining=allocation_budget;allocation_budget=-1;
      Check(!result.ok()&&!result.page&&result.bytes.empty()&&result.error==error&&remaining==0&&image==original,
        "short or invalid B-tree workspace preserves immutable input and typed failure without heap fallback");
    };
    auto workspace=scratch.get();workspace.cells=workspace.cells.first(2);refuse(workspace,E::resource_exhausted);
    workspace=scratch.get();workspace.references=workspace.references.first(24);refuse(workspace,E::resource_exhausted);
    for(std::size_t offset:{std::size_t(0),std::size_t(512),image.size()-sizeof(page::NativeBtreeCellView)}){
      workspace=scratch.get();workspace.cells={reinterpret_cast<page::NativeBtreeCellView*>(image.data()+offset),1};
      refuse(workspace,E::invalid_workspace);
    }
    for(std::size_t offset:{std::size_t(0),std::size_t(512),image.size()-sizeof(page::NativeBtreeReferenceSlot)}){
      workspace=scratch.get();workspace.references={reinterpret_cast<page::NativeBtreeReferenceSlot*>(image.data()+offset),1};
      refuse(workspace,E::invalid_workspace);
    }
    workspace=scratch.get();workspace.references={reinterpret_cast<page::NativeBtreeReferenceSlot*>(workspace.cells.data()),1};
    refuse(workspace,E::invalid_workspace);
    workspace=scratch.get();workspace.cells={reinterpret_cast<page::NativeBtreeCellView*>(
      reinterpret_cast<byte*>(scratch.cells.data())+1),1};refuse(workspace,E::invalid_workspace);
    workspace=scratch.get();workspace.references={reinterpret_cast<page::NativeBtreeReferenceSlot*>(
      reinterpret_cast<byte*>(scratch.references.data())+1),1};refuse(workspace,E::invalid_workspace);
    // Maximum density reaches the advertised cell/reference bounds. Each
    // reference uses a distinct filespace with binary suffixes beyond word one.
    for(bool branch:{false,true}){
      auto dense=BtreeExample(profile,0x200,branch);dense.cells.clear();
      const auto n=page::NativeBtreePageViewBackingRequirements(image.size()).cells;
      for(std::size_t i=0;i<n;++i){page::NativeBtreeCell cell;cell.key.encoded_key={0};
        cell.key.row_uuid=Id(60);cell.key.row_uuid.bytes[13]=0xa5;
        cell.key.row_uuid.bytes[14]=static_cast<byte>(i>>8);cell.key.row_uuid.bytes[15]=static_cast<byte>(i);
        cell.key.version_uuid=Id(61);auto filespace=cell.key.row_uuid;filespace.bytes[13]=0xb6;
        disk::NativePageReference ref{filespace,50+i,1,Profile(i%5)};
        if(branch)cell.child=ref;else cell.base_page=ref;dense.cells.push_back(cell);
      }
      const auto packed=BtreeOracle(dense);const auto owning=page::DecodeNativeBtreePage(packed);
      Check(owning.ok(),"maximal independent B-tree cell and reference population");
      BtreeBorrowedCheck(packed,owning);
      dense.cells.push_back(dense.cells.back());dense.cells.back().key.encoded_key={1};
      if(branch)++dense.cells.back().child->page_number;
      Check(page::EncodeNativeBtreePage(dense).error==E::resource_exhausted,"first extra B-tree cell cannot fit");
    }
    auto collided=BtreeExample(profile,0x200,true);
    const auto bucket=[](const Uuid& id,u64 number){
      u64 value=14695981039346656037ULL;
      for(auto b:id.bytes){value^=b;value*=1099511628211ULL;}
      for(unsigned i=0;i<8;++i){value^=(number>>(8*i))&255;value*=1099511628211ULL;}
      return value%25;
    };
    const auto target=bucket(collided.first_child->filespace_uuid,0);
    unsigned selected=0;
    for(unsigned suffix=0;selected<3&&suffix<65536;++suffix){auto id=Id(170);
      id.bytes[14]=static_cast<byte>(suffix>>8);id.bytes[15]=static_cast<byte>(suffix);
      if(bucket(id,0)!=target)continue;
      collided.cells[selected].child=disk::NativePageReference{id,70+selected,2,Profile(selected%5)};++selected;
    }
    Check(selected==3,"construct distinct full UUID reference keys with deliberately colliding scratch buckets");
    auto collision_image=BtreeOracle(collided);auto admitted=page::DecodeNativeBtreePage(collision_image);
    Check(admitted.ok(),"colliding profile references remain distinct complete native identities");BtreeBorrowedCheck(collision_image,admitted);
    collided.cells[1].child=collided.first_child;++collided.cells[1].child->page_generation;
    collision_image=BtreeOracle(collided);admitted=page::DecodeNativeBtreePage(collision_image);
    Check(admitted.error==E::invalid_reference,"changed generation does not hide duplicate physical child");
    BtreeBorrowedCheck(collision_image,admitted);
    collided=BtreeExample(profile,0x200,true);collided.cells[1].child->page_size_profile_uuid=Profile((profile+1)%5);
    collision_image=BtreeOracle(collided);admitted=page::DecodeNativeBtreePage(collision_image);
    Check(admitted.error==E::invalid_reference,"same native filespace cannot acquire a second page profile");
    BtreeBorrowedCheck(collision_image,admitted);
    auto large=BtreeExample(profile);large.cells.resize(1);large.cells[0].key.encoded_key.resize(image.size()-640-4-144,0xab);
    auto packed=BtreeOracle(large);auto large_result=page::DecodeNativeBtreePage(packed);
    Check(large_result.ok(),"largest complete inline key is admitted in every profile");BtreeBorrowedCheck(packed,large_result);
    large.cells[0].key.encoded_key.push_back(0);
    Check(page::EncodeNativeBtreePage(large).error==E::resource_exhausted,"one excess key octet is not silently truncated");
    auto empty=BtreeExample(profile);empty.cells.clear();packed=BtreeOracle(empty);
    const auto empty_result=page::DecodeNativeBtreePage(packed);
    Check(empty_result.ok(),"empty allocated root leaf remains valid in every profile");BtreeBorrowedCheck(packed,empty_result);
    for(unsigned fault=1;fault<=5;++fault){hash_fault=fault;allocation_budget=0;
      const auto result=page::DecodeNativeBtreePageInto(image,scratch.get());
      const auto remaining=allocation_budget;allocation_budget=-1;
      Check(!hash_fault&&remaining==0&&!result.ok()&&!result.page&&result.bytes.empty()&&result.error==E::hash_failure,
        "every borrowed B-tree provider fault retains exact typed failure without error-string allocation");}
  }
}
void NativeBtreePages() {
  BtreeBorrowedBounds();
  using E=page::NativeBtreeError;
  const auto empty=[&](const auto& r){Check(!r.ok()&&!r.page&&r.bytes.empty(),"native B-tree refusal returns no image or authority prefix");};
  for(unsigned p=0;p<5;++p)for(unsigned variant=0;variant<4;++variant){const bool branch=variant%2;
    const auto r=BtreeExample(p,variant<2?0x200:variant==2?0x202:0x201,branch);
    const auto bytes=BtreeOracle(r),encoded=page::EncodeNativeBtreePage(r).bytes;
    Check(bytes==encoded,"all profiles and roles independently packed B-tree image");const auto decoded=page::DecodeNativeBtreePage(bytes);
    Check(decoded.ok()&&BtreeOracle(*decoded.page)==bytes,"decode independent B-tree frame without prototype body");
    BtreeBorrowedCheck(bytes,decoded);}
  const auto good=BtreeExample();const auto bytes=BtreeOracle(good);
  for(std::size_t at=0;at<bytes.size();++at){auto bad=bytes;bad[at]^=1;const auto result=page::DecodeNativeBtreePage(bad);
    empty(result);BtreeBorrowedCheck(bad,result);}
  for(unsigned which=0;which<22;++which){auto r=BtreeExample(0,0x201,true);
    if(which==0)r.dependencies.index_uuid={};if(which==1)r.dependencies.descriptor_generation=0;if(which==2)r.dependencies.storage_generation=0;
    if(which==3)r.dependencies.key_profile_uuid={};if(which==4)r.dependencies.visibility_profile_uuid={};if(which==5)r.dependencies.dependency_map_uuid={};
    if(which==6)r.dependencies.dependency_map_sha256.fill(0);if(which==7)r.creator_transaction_uuid={};if(which==8)r.creator_local_transaction_id=0;
    if(which==9)r.maintenance_state=10;if(which==10)r.tree_level=0;if(which==11)r.parent.reset();if(which==12)r.low_fence.reset();
    if(which==13)r.first_child.reset();if(which==14)r.cells[1].child=r.first_child;if(which==15)r.cells[1].child->page_size_profile_uuid=Profile(1);
    if(which==16)r.cells[1].child->page_number=r.header.page_number;if(which==17)r.cells[1].key.row_uuid={};if(which==18)r.cells[1].key=r.cells[0].key;
    if(which==19)r.high_fence=r.cells.back().key;if(which==20)r.cells[1].deleted=true;if(which==21)r.cells[1].base_page=r.parent;
    empty(page::EncodeNativeBtreePage(r));const auto bad=BtreeOracle(r);const auto result=page::DecodeNativeBtreePage(bad);
    empty(result);BtreeBorrowedCheck(bad,result);}
  for(auto offset:{128u,136u,138u,140u,284u,504u,508u,544u,639u,640u,644u,656u,660u,664u,8191u}){
    auto bad=bytes;bad[offset]^=1;BtreeSeal(bad);const auto result=page::DecodeNativeBtreePage(bad);
    empty(result);BtreeBorrowedCheck(bad,result);}
  for(auto size:{0u,127u,639u,8191u,8193u}){auto bad=bytes;bad.resize(size);const auto result=page::DecodeNativeBtreePage(bad);
    empty(result);BtreeBorrowedCheck(bad,result);}
  {auto r=good;r.cells.clear();Check(page::EncodeNativeBtreePage(r).ok()&&page::DecodeNativeBtreePage(BtreeOracle(r)).ok(),"empty allocated root leaf is representable");}
  {auto r=good;r.cells.resize(1);r.cells[0].key.encoded_key.resize(sizes[0],0);const auto e=page::EncodeNativeBtreePage(r);empty(e);Check(e.error==E::resource_exhausted,"oversized inline key cannot be truncated");}
  // Tie-breaking is binary, including bytes after the first machine word.
  {auto a=good.cells[0].key,b=a;b.row_uuid.bytes[15]^=1;Check(page::CompareNativeBtreeKeys(a,b)!=0,"full row UUID participates in tuple order");
    b=a;b.version_uuid.bytes[15]^=1;Check(page::CompareNativeBtreeKeys(a,b)!=0,"full native version UUID participates in tuple order");}
  for(unsigned mode=0;mode<2;++mode)for(unsigned fault=1;fault<=5;++fault){hash_fault=fault;
    const auto result=mode?page::DecodeNativeBtreePage(bytes):page::EncodeNativeBtreePage(good);
    Check(hash_fault==0,"B-tree family digest backend fault consumed");empty(result);Check(result.error==E::hash_failure,"B-tree family digest error preserved");}
  observed_allocations=0;count_allocations=true;auto encoded=page::EncodeNativeBtreePage(good);count_allocations=false;
  const auto allocation_count=observed_allocations;Check(encoded.ok(),"measure B-tree encoder allocations");bool encoder_success=false;
  for(unsigned long budget=0;budget<=allocation_count;++budget){allocation_budget=budget;encoded=page::EncodeNativeBtreePage(good);allocation_budget=-1;
    if(encoded.ok()){encoder_success=true;break;}empty(encoded);Check(encoded.error==E::resource_exhausted,"B-tree encoder allocation refusal classified");}
  Check(encoder_success,"every B-tree encoder allocation fault position");
  for(unsigned p=0;p<5;++p){Fixture fixture;disk::FileDevice device;const auto path=(fixture.root/"native-btree").string();const auto r=BtreeExample(p);
    const auto z=Example(p,p%2?5:6);const auto zero=Oracle(z),image=BtreeOracle(r);const byte padding=0;
    Check(device.Open(path,disk::FileOpenMode::create_new).ok()&&device.WriteAt(0,zero.data(),zero.size()).ok()
      &&device.WriteAt(z.total_pages*sizes[p]-1,&padding,1).ok()&&device.WriteAt(12*sizes[p],image.data(),image.size()).ok()&&device.Sync().ok(),"persist native B-tree in actual index/data filespace");
    const disk::NativePageReference ref{Id(2),12,102,Profile(p)};
    const auto read=[&]{return page::ReadNativeBtreePageFromOpenDevice(device,Id(1),ref,0x200,r.dependencies);};
    auto result=read();Check(result.ok()&&result.bytes==image,"retained-device exact B-tree image/dependency binding");
    for(unsigned which=0;which<7;++which){auto d=r.dependencies;if(which==0)d.index_uuid=Id(200);if(which==1)++d.descriptor_generation;if(which==2)++d.storage_generation;
      if(which==3)d.key_profile_uuid=Id(200);if(which==4)d.visibility_profile_uuid=Id(200);if(which==5)d.dependency_map_uuid=Id(200);if(which==6)d.dependency_map_sha256[31]^=1;
      result=page::ReadNativeBtreePageFromOpenDevice(device,Id(1),ref,0x200,d);empty(result);Check(result.error==E::binding_mismatch,"every exact descriptor dependency binding enforced");}
    auto wrong=ref;++wrong.page_generation;result=page::ReadNativeBtreePageFromOpenDevice(device,Id(1),wrong,0x200,r.dependencies);empty(result);Check(result.error==E::binding_mismatch,"exact page generation required");
    result=page::ReadNativeBtreePageFromOpenDevice(device,Id(1),ref,0x202,r.dependencies);empty(result);Check(result.error==E::binding_mismatch,"exact page role required");
    for(u64 flags:{1u,2u,4u,8u}){auto flagged=r;flagged.header.flags=flags;const auto flag_image=BtreeOracle(flagged);
      if(flags!=1)Check(page::EncodeNativeBtreePage(flagged).ok()&&page::DecodeNativeBtreePage(flag_image).ok(),"non-encrypted flag image inspection is not serving authority");
      Check(device.WriteAt(12*sizes[p],flag_image.data(),flag_image.size()).ok(),"persist actual flagged B-tree image");
      result=read();empty(result);Check(result.error==(flags==1?E::encrypted_requires_crypto_authority:flags==2?E::cluster_requires_authority:E::header_policy_requires_authority),"truthful crypto cluster or header policy requirement");}
    {auto foreign=r;foreign.header.database_uuid=Id(201);foreign.header.flags=2;const auto foreign_image=BtreeOracle(foreign);
      Check(device.WriteAt(12*sizes[p],foreign_image.data(),foreign_image.size()).ok(),"persist foreign cluster-flagged image");
      result=read();empty(result);Check(result.error==E::binding_mismatch,"exact node identity precedes cluster policy classification");}
    Check(device.WriteAt(12*sizes[p],image.data(),image.size()).ok(),"restore ordinary B-tree image");
    for(unsigned fault=1;fault<=5;++fault){hash_fault=fault;result=read();Check(hash_fault==0,"native B-tree hash provider fault consumed");empty(result);}
    reads=0;track_reads=true;result=read();track_reads=false;const auto count=reads;Check(result.ok(),"measure B-tree actual reads");
    for(unsigned fault=1;fault<=count;++fault){reads=0;read_fault=fault;track_reads=true;result=read();track_reads=false;Check(read_fault==0,"native B-tree read fault consumed");empty(result);}
    observed_allocations=0;count_allocations=true;result=read();count_allocations=false;const auto allocations=observed_allocations;Check(result.ok(),"measure B-tree reader allocations");
    bool success=false;for(unsigned long budget=0;budget<=allocations;++budget){allocation_budget=budget;result=read();allocation_budget=-1;
      if(result.ok()){success=true;break;}empty(result);Check(result.error==E::resource_exhausted,"native B-tree allocation refusal classified");}Check(success,"every B-tree reader allocation fault position");
    for(unsigned role=1;role<=14;++role){const auto placed_zero=Oracle(Example(p,role));
      Check(disk::DecodeFilespacePageZero(placed_zero.data(),placed_zero.size()).ok(),"registered placement role has valid canonical page zero");
      Check(device.WriteAt(0,placed_zero.data(),placed_zero.size()).ok(),"persist architect-selected index placement role");
      result=read();Check(result.ok()&&result.bytes==image,"all registered roles admit exact structural index read, not lifecycle authorization");}
    for(unsigned role:{0u,15u}){const auto bad_zero=Oracle(Example(p,role));
      Check(!disk::DecodeFilespacePageZero(bad_zero.data(),bad_zero.size()).ok(),"unregistered placement role is invalid despite coherent checksums");
      Check(device.WriteAt(0,bad_zero.data(),bad_zero.size()).ok(),"persist unknown role negative");result=read();empty(result);
      Check(result.error==E::invalid_filespace,"placement flexibility never admits an unknown filespace role");}
    Check(device.WriteAt(0,zero.data(),zero.size()).ok()&&device.Sync().ok(),"restore owned B-tree filespace");Exclusive(path);
    Check(device.Close().ok(),"close B-tree before independent reopen");const auto child=::fork();Check(child>=0,"fork B-tree persisted reader");
    if(child==0){const auto profile=std::to_string(p);::execl("/proc/self/exe","native-btree-probe","--native-btree-probe",path.c_str(),profile.c_str(),nullptr);::_exit(125);}
    int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable reads actual canonical B-tree image");
  }
}
std::vector<page::NativeBtreePage> BtreeTreeExample(unsigned p) {
  const auto q=(p+1)%5,s=(p+2)%5;const auto base=BtreeExample(p);std::vector<page::NativeBtreePage> pages(7);
  for(unsigned i=0;i<7;++i){auto& r=pages[i];r.header=base.header;r.header.page_type=i==0?0x200:i<3?0x201:0x202;
    r.header.page_uuid=Id(static_cast<byte>(90+i));r.header.page_number=i==0?12:i==1?15:i==2?16:17+i;r.header.page_generation=111+i;
    if(i==1||i==4||i==6){r.header.filespace_uuid=Id(7);r.header.page_size_bytes=sizes[q];r.header.page_size_profile_uuid=Profile(q);}
    r.dependencies=base.dependencies;r.creator_transaction_uuid=base.creator_transaction_uuid;r.creator_local_transaction_id=17;r.maintenance_state=1;r.tree_level=i==0?2:i<3?1:0;}
  const auto ref=[&](unsigned i){const auto& h=pages[i].header;return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};};
  const auto key=[](unsigned n){return page::NativeBtreeKey{{static_cast<byte>(n)},Id(static_cast<byte>(70+n)),Id(static_cast<byte>(120+n))};};
  const auto branch=[&](unsigned i,unsigned first,unsigned second,unsigned pivot){auto& r=pages[i];r.first_child=ref(first);page::NativeBtreeCell c;c.key=key(pivot);c.child=ref(second);r.cells.push_back(c);};
  branch(0,1,2,40);branch(1,3,4,20);branch(2,5,6,60);
  pages[1].parent=ref(0);pages[2].parent=ref(0);pages[1].right=ref(2);pages[2].left=ref(1);
  pages[1].high_fence=key(40);pages[2].low_fence=key(40);
  for(unsigned i=3;i<7;++i){auto& r=pages[i];r.parent=ref(i<5?1:2);
    if(i>3){r.left=ref(i-1);r.low_fence=key((i-3)*20);}if(i<6){r.right=ref(i+1);r.high_fence=key((i-2)*20);}
    for(unsigned n:{i==3?10u:(i-3)*20,i==3?15u:(i-3)*20+10}){page::NativeBtreeCell c;c.key=key(n);c.deleted=n==15;
      c.base_page=disk::NativePageReference{Id(9),50+i,3,Profile(s)};r.cells.push_back(c);}}
  return pages;
}
std::vector<page::NativeBtreePage> BtreeDeepTreeExample(unsigned p) {
  const auto original=BtreeTreeExample(p);std::vector<page::NativeBtreePage> nodes{original[0]};
  nodes.insert(nodes.end(),original.begin(),original.end());nodes.insert(nodes.end(),original.begin(),original.end());
  nodes[0].header.page_number=30;nodes[0].header.page_generation=200;nodes[0].header.page_uuid=Id(150);nodes[0].tree_level=3;
  for(unsigned i=8;i<15;++i){auto& n=nodes[i];n.header.page_number+=20;n.header.page_generation+=30;n.header.page_uuid=Id(static_cast<byte>(160+i-8));
    const auto rebase=[](auto& ref){if(ref){ref->page_number+=20;ref->page_generation+=30;}};
    rebase(n.parent);rebase(n.left);rebase(n.right);rebase(n.first_child);for(auto& c:n.cells){rebase(c.child);c.key.encoded_key[0]+=100;}
    if(n.low_fence)n.low_fence->encoded_key[0]+=100;if(n.high_fence)n.high_fence->encoded_key[0]+=100;}
  const auto ref=[&](unsigned i){const auto& h=nodes[i].header;return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};};
  const page::NativeBtreeKey boundary{{100},Id(170),Id(220)};
  nodes[0].first_child=ref(1);nodes[0].cells[0].child=ref(8);nodes[0].cells[0].key=boundary;
  nodes[1].header.page_type=0x201;nodes[8].header.page_type=0x201;nodes[1].parent=ref(0);nodes[8].parent=ref(0);
  for(const auto& pair:{std::pair{1u,8u},std::pair{3u,9u},std::pair{7u,11u}}){nodes[pair.first].right=ref(pair.second);nodes[pair.second].left=ref(pair.first);
    nodes[pair.first].high_fence=boundary;nodes[pair.second].low_fence=boundary;}
  return nodes;
}
void GovernedBtreeParity(const std::vector<disk::NativeFilespaceDevice>&,
  const disk::NativePageReference&,const page::NativeBtreeDependencies&,u64,const page::NativeBtreeTreeResult&);
void NativeBtreeTrees() {
  using E=page::NativeBtreeError;
  const auto empty=[&](const auto& r){Check(!r.ok()&&r.pages.empty()&&r.leaves.empty()&&r.retained_image_bytes==0,"native tree failure exposes no images leaf order or counters");};
  for(unsigned p=0;p<5;++p){const auto q=(p+1)%5,s=(p+2)%5;Fixture fixture;disk::FileDevice first,second,base;
    const auto path1=(fixture.root/"tree-first").string(),path2=(fixture.root/"tree-second").string(),path3=(fixture.root/"tree-base").string();
    auto z1=Example(p,6),z2=Example(q,5),z3=Example(s,1);z2.bootstrap.filespace_uuid=Id(7);z3.bootstrap.filespace_uuid=Id(9);
    for(auto& r:z2.roots)r.filespace_uuid=Id(7);for(auto& r:z3.roots)r.filespace_uuid=Id(9);
    const auto prepare=[&](auto& device,const auto& path,const auto& zero){const auto image=Oracle(zero);const byte padding=0;
      Check(device.Open(path,disk::FileOpenMode::create_new).ok()&&device.WriteAt(0,image.data(),image.size()).ok()
        &&device.WriteAt(zero.total_pages*zero.bootstrap.page_size_bytes-1,&padding,1).ok(),"prepare actual retained tree filespace");};
    prepare(first,path1,z1);prepare(second,path2,z2);prepare(base,path3,z3);
    const auto original=BtreeTreeExample(p);auto nodes=original;
    const auto reference=[](const auto& node){const auto& h=node.header;return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};};
    const auto persist=[&]{for(const auto& r:nodes){const auto bytes=BtreeOracle(r);auto& device=r.header.filespace_uuid==Id(2)?first:second;
      Check(device.WriteAt(r.header.page_number*r.header.page_size_bytes,bytes.data(),bytes.size()).ok(),"persist independently packed navigation image");}
      Check(first.Sync().ok()&&second.Sync().ok()&&base.Sync().ok(),"sync actual navigation tree");};
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(9),Profile(s),&base},{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};
    const u64 budget=4*sizes[p]+3*sizes[q];const auto root=reference(original[0]);
    const auto read=[&](u64 allowance){return page::ReadNativeBtreeTreeFromOpenDevices(Id(1),devices,root,original[0].dependencies,allowance);};
    persist();auto result=read(budget);Check(result.ok()&&result.pages.size()==7&&result.leaves==std::vector<std::size_t>{2,3,5,6}
      &&result.retained_image_bytes==budget,"complete cross-profile two-level navigation tree and exact retained budget");
    for(unsigned i=0;i<4;++i)Check(result.pages[result.leaves[i]].bytes==BtreeOracle(original[3+i]),"verified leaf order uses actual independently packed images");
    GovernedBtreeParity(devices,root,original[0].dependencies,budget,result);
    result=read(budget-1);empty(result);Check(result.error==E::resource_exhausted,"late subtree one-byte-short budget returns no valid prefix");
    result=read(0);empty(result);Check(result.error==E::invalid_reference,"zero navigation allowance refused");
    auto reversed=devices;std::reverse(reversed.begin(),reversed.end());
    std::atomic<bool> concurrent_ok{true};std::thread t1([&]{for(unsigned i=0;i<3;++i)if(!read(budget).ok())concurrent_ok=false;});
    std::thread t2([&]{for(unsigned i=0;i<3;++i)if(!page::ReadNativeBtreeTreeFromOpenDevices(Id(1),reversed,root,original[0].dependencies,budget).ok())concurrent_ok=false;});
    t1.join();t2.join();Check(concurrent_ok,"opposing caller orders share consistent retained-filespace lock ordering");
    if(p==0){
      const auto mutex_of=[](auto& device){auto guard=device.AcquireOperationGuard();return guard.mutex();};
      const std::array mutexes{mutex_of(first),mutex_of(second),mutex_of(base)};
      tree_read_paused=false;resume_tree_read=false;pause_next_tree_read=true;std::atomic<bool> done{false};page::NativeBtreeTreeResult paused_result;
      std::thread paused_reader([&]{paused_result=read(budget);done=true;});
      while(!tree_read_paused.load()&&!done.load())std::this_thread::yield();
      bool all_held=tree_read_paused.load();for(auto* mutex:mutexes)if(mutex->try_lock()){all_held=false;mutex->unlock();}
      resume_tree_read=true;paused_reader.join();pause_next_tree_read=false;
      Check(all_held&&paused_result.ok(),"all filespace guards held before the first physical read through complete traversal");
      for(unsigned fault=0;fault<14;++fault){nodes=original;E expected=E::tree_reference_mismatch;
        if(fault==0)++nodes[6].parent->page_generation;
        if(fault==1){nodes[2].tree_level=2;expected=E::tree_level_mismatch;}
        if(fault==2){nodes[4].low_fence->encoded_key={19};nodes[3].high_fence=nodes[4].low_fence;expected=E::tree_fence_mismatch;}
        if(fault==3){nodes[4].right=reference(nodes[6]);expected=E::tree_sibling_mismatch;}
        if(fault==4){nodes[2].left.reset();expected=E::tree_sibling_mismatch;}
        if(fault==5){nodes[6].right=reference(nodes[3]);nodes[6].high_fence=page::NativeBtreeKey{{80},Id(150),Id(200)};expected=E::tree_fence_mismatch;}
        if(fault==6)nodes[5].header.page_uuid=nodes[3].header.page_uuid;
        if(fault==7)nodes[2].first_child=reference(nodes[4]);
        if(fault==8){nodes[6].dependencies.dependency_map_sha256[31]^=1;expected=E::binding_mismatch;}
        if(fault==9){for(auto& c:nodes[6].cells)c.base_page->page_size_profile_uuid=Profile(4);expected=E::invalid_filespace;}
        if(fault==10){for(auto& c:nodes[6].cells)c.base_page->page_number=64;expected=E::invalid_filespace;}
        if(fault==11){nodes[1].right.reset();expected=E::tree_sibling_mismatch;}
        if(fault==12||fault==13){auto& key=*nodes[4].low_fence;if(fault==12)--key.row_uuid.bytes[15];else --key.version_uuid.bytes[15];
          nodes[3].high_fence=key;expected=E::tree_fence_mismatch;}
        for(const auto& n:nodes)Check(page::DecodeNativeBtreePage(BtreeOracle(n)).ok(),"negative tree still has individually valid native images");
        persist();result=read(budget);empty(result);Check(result.error==expected,"exact cross-page tree fault classification actual="+std::to_string(static_cast<unsigned>(result.error))+" expected="+std::to_string(static_cast<unsigned>(expected)));
        GovernedBtreeParity(devices,root,original[0].dependencies,budget,result);}
      nodes=original;persist();
      auto duplicate=devices;duplicate.push_back(devices[0]);result=page::ReadNativeBtreeTreeFromOpenDevices(Id(1),duplicate,root,original[0].dependencies,budget);empty(result);Check(result.error==E::invalid_filespace,"duplicate retained filespace refused");
      duplicate=devices;duplicate[0].device=&first;result=page::ReadNativeBtreeTreeFromOpenDevices(Id(1),duplicate,root,original[0].dependencies,budget);empty(result);Check(result.error==E::invalid_filespace,"aliased retained device refused");
      duplicate=devices;duplicate.erase(duplicate.begin());result=page::ReadNativeBtreeTreeFromOpenDevices(Id(1),duplicate,root,original[0].dependencies,budget);empty(result);Check(result.error==E::invalid_filespace,"base locator cannot reference an unbound filespace");
      auto foreign=z3;foreign.bootstrap.database_uuid=Id(201);const auto foreign_bytes=Oracle(foreign);
      Check(base.WriteAt(0,foreign_bytes.data(),foreign_bytes.size()).ok(),"write foreign node binding on unvisited primary device");result=read(budget);empty(result);Check(result.error==E::invalid_filespace,"unvisited foreign filespace not silently ignored");
      const auto original_zero=Oracle(z3);Check(base.WriteAt(0,original_zero.data(),original_zero.size()).ok(),"restore primary node binding");
      auto corrupt=BtreeOracle(nodes.back());corrupt[512]^=1;Check(second.WriteAt(nodes.back().header.page_number*sizes[q],corrupt.data(),corrupt.size()).ok(),"corrupt final leaf self digest");
      result=read(budget);empty(result);Check(result.error==E::invalid_integrity,"corrupted final leaf refuses whole earlier tree");persist();
      reads=0;track_reads=true;result=read(budget);track_reads=false;const auto read_count=reads;Check(result.ok(),"measure complete tree reads");
      for(unsigned fault=1;fault<=read_count;++fault){reads=0;read_fault=fault;track_reads=true;result=read(budget);track_reads=false;Check(read_fault==0,"each actual tree read fault consumed");empty(result);}
      observed_allocations=0;count_allocations=true;result=read(budget);count_allocations=false;const auto allocations=observed_allocations;Check(result.ok(),"measure complete tree allocations");bool success=false;
      for(unsigned long allowance=0;allowance<=allocations;++allowance){allocation_budget=allowance;result=read(budget);allocation_budget=-1;if(result.ok()){success=true;break;}
        empty(result);Check(result.error==E::resource_exhausted,"every tree allocation refusal classified");}Check(success,"every complete tree allocation fault position");
      for(unsigned fault=1;fault<=5;++fault){hash_fault=fault;result=read(budget);Check(hash_fault==0,"tree digest backend fault consumed");empty(result);Check(result.error==E::hash_failure,"tree hash provider error preserved");}
      // This changes only root reachability. Unreachable stale images remain an
      // allocation-map verifier obligation, not fabricated tree members.
      nodes[0].tree_level=0;nodes[0].first_child.reset();nodes[0].cells.clear();persist();result=read(sizes[p]);
      Check(result.ok()&&result.pages.size()==1&&result.leaves==std::vector<std::size_t>{0}&&result.retained_image_bytes==sizes[p],"actual empty allocated root tree");
      GovernedBtreeParity(devices,root,original[0].dependencies,sizes[p],result);
      nodes=BtreeDeepTreeExample(p);persist();const auto deep_root=reference(nodes[0]);const u64 deep_budget=2*budget+sizes[p];
      const auto deep_read=[&]{return page::ReadNativeBtreeTreeFromOpenDevices(Id(1),devices,deep_root,original[0].dependencies,deep_budget);};
      result=deep_read();Check(result.ok()&&result.pages.size()==15&&result.leaves==std::vector<std::size_t>{3,4,6,7,10,11,13,14}
        &&result.retained_image_bytes==deep_budget,"three-level actual tree includes branch siblings across different parents");
      GovernedBtreeParity(devices,deep_root,original[0].dependencies,deep_budget,result);
      const auto right=nodes[3].right;nodes[3].right=reference(nodes[10]);
      Check(page::DecodeNativeBtreePage(BtreeOracle(nodes[3])).ok(),"cross-parent branch sibling fault is locally valid");persist();result=deep_read();empty(result);
      Check(result.error==E::tree_sibling_mismatch,"branch sibling reciprocity checked across parent boundaries");
      GovernedBtreeParity(devices,deep_root,original[0].dependencies,deep_budget,result);nodes[3].right=right;persist();
    }
    Exclusive(path1);Exclusive(path2);Exclusive(path3);Check(first.Close().ok()&&second.Close().ok()&&base.Close().ok(),"close tree owners before fresh executable");
    const auto child=::fork();Check(child>=0,"fork independent native tree reader");
    if(child==0){const auto profile=std::to_string(p);::execl("/proc/self/exe","native-btree-tree-probe",p==0?"--native-btree-deep-probe":"--native-btree-tree-probe",fixture.root.c_str(),profile.c_str(),nullptr);::_exit(125);}
    int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable verifies actual cross-profile navigation tree");
  }
}
void RootReject(const Bytes& b,RootError expected,std::source_location at=std::source_location::current()) {
  const auto r=page::DecodeNativeCatalogRoot(b);
  Check(!r.ok()&&!r.root&&r.bytes.empty()&&r.error==expected,"atomic catalog root refusal actual="+
    std::to_string(static_cast<unsigned>(r.error))+" expected="+
    std::to_string(static_cast<unsigned>(expected)),at);
}
void RootInvalid(const page::NativeCatalogRoot& r,RootError error) {
  RootReject(RootOracle(r),error); const auto e=page::EncodeNativeCatalogRoot(r);
  Check(!e.ok()&&!e.root&&e.bytes.empty()&&e.error==error,"invalid root emits no image");
}
void CatalogRoots() {
  for(unsigned p=0;p<5;++p) for(unsigned kind:{2u,6u,7u,8u}) {
    auto r=RootExample(p); r.root_kind=static_cast<disk::u16>(kind);
    if(kind!=2) {
      const auto target=r.roots[kind==6?4:kind==7?3:5];r.roots={target};
      r.header.page_type=kind==6?10:kind==7?11:5;
    }
    for(unsigned generation:{1u,2u}) {
      r.catalog_generation=generation;
      if(generation==2) {
        r.predecessor=page::NativeCatalogPageReference{Id(2),10,1,Profile(p)};
        r.predecessor_sha256.fill(9);
      }
      const auto expected=RootOracle(r); const auto e=page::EncodeNativeCatalogRoot(r);
      Check(e.ok()&&e.bytes==expected,"all profiles/kinds/generations independent root bytes");
      const auto d=page::DecodeNativeCatalogRoot(expected);
      Check(d.ok()&&RootOracle(*d.root)==expected,"independent root decode");
    }
  }
  const auto good=RootOracle(RootExample());
  for(std::size_t at=0;at<good.size();++at) {
    auto b=good; b[at]^=1; const auto r=page::DecodeNativeCatalogRoot(b);
    Check(!r.ok()&&!r.root&&r.bytes.empty(),"every byte corruption rejected without partial root");
  }
  for(unsigned at:{128u,136u,138u,140u,148u,336u,383u,864u,8191u}) {
    auto b=good; b[at]^=1; RootSeal(b); RootReject(b,RootError::invalid_family);
  }
  for(unsigned at:{386u,387u,456u,463u}) {
    auto b=good; b[at]=1; RootSeal(b); RootReject(b,RootError::invalid_roots);
  }
  for(std::size_t size:{0u,127u,383u,8191u,8193u,131073u}) {
    auto b=good; b.resize(size); RootReject(b,RootError::invalid_header);
  }
  {auto r=RootExample(); r.root_kind=9; Check(!page::EncodeNativeCatalogRoot(r).ok(),"unknown kind producer");}
  {auto r=RootExample(); r.header.page_type=6; RootInvalid(r,RootError::invalid_header);}
  {auto r=RootExample(); r.object_uuid.bytes[6]=0x41; RootInvalid(r,RootError::invalid_family);}
  {auto r=RootExample(); r.creator_transaction_uuid={}; RootInvalid(r,RootError::invalid_family);}
  {auto r=RootExample(); r.creator_local_transaction_id=0; RootInvalid(r,RootError::invalid_family);}
  {auto r=RootExample(); r.catalog_generation=0; RootInvalid(r,RootError::invalid_family);}
  {auto r=RootExample(); r.schema_epoch=0; RootInvalid(r,RootError::invalid_family);}
  {auto r=RootExample(); r.security_epoch=0; RootInvalid(r,RootError::invalid_family);}
  // SB_CATALOG_ROOT_ADVANCE (owner Q24 = 1): a later generation may carry
  // neither predecessor nor digest (the in-place form), never one without the other.
  {auto r=RootExample(); r.catalog_generation=2; const auto e=page::EncodeNativeCatalogRoot(r);
    Check(e.ok()&&page::DecodeNativeCatalogRoot(RootOracle(r)).ok(),"the in-place later generation encodes and decodes");}
  {auto r=RootExample(); r.catalog_generation=2; r.predecessor_sha256[0]=1; RootInvalid(r,RootError::invalid_reference);}
  {auto r=RootExample(); r.predecessor_sha256[0]=1; RootInvalid(r,RootError::invalid_reference);}
  {auto r=RootExample(); r.roots[0].role=2; RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.roots[0].page_type=5; RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.roots[0].object_uuid={}; RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.roots[0].page.page_number=12; RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.roots[0].page.page_generation=0; RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.roots[0].page.page_number=std::numeric_limits<u64>::max(); RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.roots[0].page.page_size_profile_uuid=Profile(1); RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.roots[1].page=r.roots[0].page; RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.roots[1].object_uuid=r.roots[0].object_uuid; RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.catalog_generation=2;
    r.predecessor=page::NativeCatalogPageReference{Id(2),12,1,Profile(0)};
    r.predecessor_sha256[0]=1; RootInvalid(r,RootError::invalid_reference);
    r.predecessor->page_number=21; RootInvalid(r,RootError::invalid_roots);
    r.predecessor->page_number=10; r.predecessor->page_size_profile_uuid=Profile(1);
    RootInvalid(r,RootError::invalid_reference);}
  {auto r=RootExample(); r.roots[0].page.filespace_uuid=Id(88); r.roots[0].page.page_size_profile_uuid=Profile(1);
    Check(page::EncodeNativeCatalogRoot(r).ok(),"cross-filespace profile retained, not opened");
    r.roots[1].page.filespace_uuid=Id(88); RootInvalid(r,RootError::invalid_roots);}
  for(unsigned fault=1;fault<=5;++fault) {
    auto value=RootExample(); hash_fault=fault; const auto e=page::EncodeNativeCatalogRoot(value);
    Check(!e.ok()&&e.bytes.empty()&&!e.root&&e.error==RootError::hash_failure&&!hash_fault,"root encode digest failure");
    hash_fault=fault; RootReject(good,RootError::hash_failure); Check(!hash_fault,"root decode digest fault consumed");
  }
  for(unsigned i=0;i<2;++i) {
    const auto value=RootExample(); allocation_budget=0;
    const auto r=i? page::DecodeNativeCatalogRoot(good):page::EncodeNativeCatalogRoot(value);
    allocation_budget=-1;
    Check(!r.ok()&&!r.root&&r.bytes.empty()&&r.error==RootError::resource_exhausted,"root allocation failure atomicity");
  }
}
void CatalogRootFiles() {
  Fixture fixture;
  for(unsigned p=0;p<5;++p) {
    auto z=Example(p); auto root=RootExample(p); const auto image=RootOracle(root);
    const auto path=(fixture.root/("catalog-"+std::to_string(p))).string();
    disk::FileDevice device; Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"own root file");
    const auto pagezero=Oracle(z); const byte zero=0;
    Check(device.WriteAt(0,pagezero.data(),pagezero.size()).ok()
      &&device.WriteAt(z.total_pages*sizes[p]-1,&zero,1).ok(),"real root filespace capacity");
    const auto& ref=z.roots[1];
    Check(device.WriteAt(ref.page_number*sizes[p],image.data(),image.size()).ok()&&device.Sync().ok(),"real root persisted");
    auto r=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),ref);
    Check(r.ok()&&r.bytes==image,"read exact canonical catalog target, no prototype fallback");
    auto feature=ref; feature.kind=8;
    Check(page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),feature).ok(),"shared feature root selects catalog role6");
    Exclusive(path);
    for(unsigned fault=1;fault<=3;++fault) {
      reads=0; read_fault=fault; track_reads=true;
      r=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),ref); track_reads=false;
      Check(!r.ok()&&!r.root&&r.bytes.empty()&&r.error==RootError::io_failure&&!read_fault,"root actual pread failure");
    }
    for(unsigned field=0;field<4;++field) {
      auto bad=ref;
      if(field==0) ++bad.page_generation;
      if(field==1) bad.object_uuid=Id(99);
      if(field==2) bad.filespace_uuid=Id(99);
      if(field==3) bad.page_size_profile_uuid=Profile((p+1)%5);
      r=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),bad);
      Check(!r.ok()&&!r.root&&r.bytes.empty(),"exact owner/generation/filespace/profile binding");
    }
    Check(!page::ReadNativeCatalogRootFromOpenDevice(device,Id(99),ref).ok(),"other database refused");
    auto changed=root; changed.header.flags=1; auto badimage=RootOracle(changed);
    Check(device.WriteAt(ref.page_number*sizes[p],badimage.data(),badimage.size()).ok(),"encrypted-header fixture");
    r=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),ref);
    Check(r.error==RootError::encrypted_requires_crypto_authority&&!r.root&&r.bytes.empty(),"no plaintext encrypted root admission");
    changed=root; changed.root_kind=8; changed.roots.erase(changed.roots.begin(),changed.roots.end()-1);
    badimage=RootOracle(changed);
    Check(device.WriteAt(ref.page_number*sizes[p],badimage.data(),badimage.size()).ok(),"dedicated feature root persisted");
    Check(page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),feature).ok(),"dedicated feature root read");
    r=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),ref);
    Check(r.error==RootError::binding_mismatch&&!r.root,"dedicated feature root cannot stand in for catalog");
    changed=root; changed.header.database_uuid=Id(99); badimage=RootOracle(changed);
    Check(device.WriteAt(ref.page_number*sizes[p],badimage.data(),badimage.size()).ok(),"other node root image fixture");
    r=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),ref);
    Check(r.error==RootError::binding_mismatch&&!r.root,"actual image node binding checked");
    changed=root; changed.roots[0].page.page_number=z.total_pages; badimage=RootOracle(changed);
    Check(device.WriteAt(ref.page_number*sizes[p],badimage.data(),badimage.size()).ok(),"outside-capacity target fixture");
    r=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),ref);
    Check(r.error==RootError::invalid_reference&&!r.root,"actual local target capacity checked");
    Check(device.WriteAt(ref.page_number*sizes[p],image.data(),image.size()).ok()&&device.Sync().ok(),"restore own root fixture");
    Check(device.Close().ok()&&device.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"reopen canonical root read-only");
    r=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),ref);
    Check(r.ok()&&r.bytes==image&&device.read_only(),"read-only reopen actual root");
    Bytes actual(image.size()); Check(device.ReadAt(ref.page_number*sizes[p],actual.data(),actual.size()).ok()
      &&actual==image,"root reader did not mutate persisted image"); Exclusive(path);
    Check(device.Close().ok(),"close root fixture");
    Check(!page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),ref).ok(),"closed root device refused");
    const auto pid=::fork(); Check(pid>=0,"fork root reopen verifier");
    if(pid==0) { const auto profile=std::to_string(p);
      ::execl("/proc/self/exe","pagezero_test","--catalog-probe",path.c_str(),profile.c_str(),nullptr); ::_exit(125); }
    int status=0;
    Check(::waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh process verifies exact durable root");
  }
}
std::array<byte,32> WholeRootHash(const Bytes& bytes) {
  std::array<byte,32> out{};
  Check(SHA256(bytes.data(),bytes.size(),out.data())!=nullptr,"independent complete root image digest");
  return out;
}
disk::FilespaceRootReference RootRef(const page::NativeCatalogRoot& root) {
  const auto& h=root.header;
  return {root.root_kind,h.page_type,h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid,root.object_uuid};
}
page::NativeCatalogPageReference PageRef(const page::NativeCatalogRoot& root) {
  const auto& h=root.header; return {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};
}
struct RootRangeFixture {
  Fixture files;
  disk::FileDevice first,second;
  std::array<page::NativeCatalogRoot,3> roots;
  std::array<Bytes,3> images;
  std::vector<page::NativeCatalogFilespaceDevice> devices;
  RootRangeFixture() {
    roots[0]=RootExample(); roots[1]=RootExample(1); roots[2]=RootExample();
    roots[1].header.filespace_uuid=Id(7); roots[1].header.page_number=34;
    roots[1].header.page_generation=202; roots[1].header.page_uuid=Id(93);
    for(auto& target:roots[1].roots) target.page.filespace_uuid=Id(7);
    roots[2].header.page_number=35; roots[2].header.page_generation=302; roots[2].header.page_uuid=Id(94);
    for(unsigned i=0;i<3;++i) {
      roots[i].catalog_generation=i+1; roots[i].schema_epoch=2+i;
      roots[i].security_epoch=3+i; roots[i].resource_epoch=i;
      if(i) { roots[i].predecessor=PageRef(roots[i-1]); roots[i].predecessor_sha256=WholeRootHash(images[i-1]); }
      images[i]=RootOracle(roots[i]);
    }
    Check(first.Open((files.root/"first").string(),disk::FileOpenMode::create_new).ok()
      &&second.Open((files.root/"second").string(),disk::FileOpenMode::create_new).ok(),"own two filespaces of one node");
    auto a=Example(),b=Example(1); b.bootstrap.filespace_uuid=Id(7); b.page_uuid=Id(4);
    for(auto& ref:b.roots) ref.filespace_uuid=Id(7);
    a.roots[1]=RootRef(roots[2]); b.roots[1]=RootRef(roots[1]); const byte zero=0;
    const auto ab=Oracle(a),bb=Oracle(b);
    Check(first.WriteAt(0,ab.data(),ab.size()).ok()&&second.WriteAt(0,bb.data(),bb.size()).ok()
      &&first.WriteAt(a.total_pages*sizes[0]-1,&zero,1).ok()
      &&second.WriteAt(b.total_pages*sizes[1]-1,&zero,1).ok(),"persist actual mixed-profile page zeros");
    Restore();
    devices={{Id(7),Profile(1),&second},{Id(2),Profile(0),&first}};
  }
  void Write(unsigned i,const Bytes& bytes) {
    auto& device=i==1?second:first;
    Check(device.WriteAt(roots[i].header.page_number*roots[i].header.page_size_bytes,bytes.data(),bytes.size()).ok(),"write own range root fixture");
  }
  void Restore() { for(unsigned i=0;i<3;++i) Write(i,images[i]);
    Check(first.Sync().ok()&&second.Sync().ok(),"sync actual range fixture"); }
  page::NativeCatalogRootRangeResult Read(u64 budget=32768,unsigned terminal=0) {
    return page::ReadNativeCatalogRootRangeFromOpenDevices(Id(1),devices,RootRef(roots[2]),RootRef(roots[terminal]),budget);
  }
};
void RangeReject(const page::NativeCatalogRootRangeResult& r,RootError error,
                 std::source_location at=std::source_location::current()) {
  Check(!r.ok()&&r.roots.empty()&&!r.retained_image_bytes&&r.error==error,"range fails without prefix actual="+
    std::to_string(static_cast<unsigned>(r.error))+" expected="+std::to_string(static_cast<unsigned>(error)),at);
}
void CatalogRootRanges() {
  RootRangeFixture f;
  auto r=f.Read(); Check(r.ok()&&r.roots.size()==3&&r.retained_image_bytes==32768,"actual mixed-profile root range");
  for(unsigned i=0;i<3;++i) Check(r.roots[i].bytes==f.images[2-i],"exact ordered independent range images");
  auto ordered=f.devices; std::reverse(f.devices.begin(),f.devices.end());
  Check(f.Read().ok(),"input order does not control filespace locking"); f.devices=ordered;
  Check(f.Read(8192,2).ok(),"one-root exact head/terminal range");
  Check(f.Read(24576,1).ok(),"explicit retained terminal stops before older history");
  for(u64 budget:{0ull,8191ull,8192ull,24575ull,24576ull,32767ull}) RangeReject(f.Read(budget),RootError::resource_exhausted);
  for(const auto& [budget,expected_reads]:std::array<std::pair<u64,unsigned>,3>{{{8191,4},{8192,7},{24576,10}}}) {
    reads=0; track_reads=true; r=f.Read(budget); track_reads=false;
    RangeReject(r,RootError::resource_exhausted);
    Check(reads==expected_reads,"byte allowance checked before reading the next root image");
  }
  {auto middle=f.roots[1],head=f.roots[2];
    middle.header.filespace_uuid=Id(2); middle.header.page_size_profile_uuid=Profile(0);
    middle.header.page_size_bytes=sizes[0];
    for(auto& target:middle.roots) { target.page.filespace_uuid=Id(2); target.page.page_size_profile_uuid=Profile(0); }
    const auto image=RootOracle(middle);
    Check(f.first.WriteAt(middle.header.page_number*sizes[0],image.data(),image.size()).ok(),"actual same-filespace predecessor image");
    head.predecessor=PageRef(middle); head.predecessor_sha256=WholeRootHash(image); f.Write(2,RootOracle(head));
    r=f.Read(24576); Check(r.ok()&&r.roots.size()==3&&r.retained_image_bytes==24576
      &&r.roots[1].bytes==image,"actual three-generation same-filespace range"); f.Restore();}
  auto bad_terminal=RootRef(f.roots[0]); ++bad_terminal.page_generation;
  RangeReject(page::ReadNativeCatalogRootRangeFromOpenDevices(Id(1),f.devices,RootRef(f.roots[2]),bad_terminal,65536),RootError::history_mismatch);
  bad_terminal=RootRef(f.roots[0]); bad_terminal.object_uuid=Id(99);
  RangeReject(page::ReadNativeCatalogRootRangeFromOpenDevices(Id(1),f.devices,RootRef(f.roots[2]),bad_terminal,65536),RootError::invalid_reference);
  f.devices.pop_back(); RangeReject(f.Read(),RootError::invalid_filespace); f.devices=ordered;
  f.devices.push_back(f.devices[0]); RangeReject(f.Read(),RootError::invalid_filespace); f.devices=ordered;
  f.devices[1].device=f.devices[0].device; RangeReject(f.Read(),RootError::invalid_filespace); f.devices=ordered;
  f.devices[0].device=nullptr; RangeReject(f.Read(),RootError::invalid_filespace); f.devices=ordered;
  f.devices[0].page_size_profile_uuid=Profile(0); RangeReject(f.Read(),RootError::invalid_filespace); f.devices=ordered;
  RangeReject(page::ReadNativeCatalogRootRangeFromOpenDevices(Id(99),f.devices,RootRef(f.roots[2]),RootRef(f.roots[0]),32768),RootError::invalid_filespace);
  // An unused device cannot smuggle a second node into an owning-node range.
  disk::FileDevice other; Check(other.Open((f.files.root/"other-node").string(),disk::FileOpenMode::create_new).ok(),"other-node fixture");
  auto z=Example(); z.bootstrap.database_uuid=Id(99); z.bootstrap.filespace_uuid=Id(88);
  for(auto& ref:z.roots) ref.filespace_uuid=Id(88);
  const auto zb=Oracle(z); const byte zero=0;
  Check(other.WriteAt(0,zb.data(),zb.size()).ok()&&other.WriteAt(z.total_pages*sizes[0]-1,&zero,1).ok(),"other-node actual pagezero");
  f.devices.push_back({Id(88),Profile(0),&other}); RangeReject(f.Read(),RootError::invalid_filespace); f.devices=ordered;
  reads=0; track_reads=true; r=f.Read(); track_reads=false;
  Check(r.ok()&&reads==13,"two filespace probes and three actual root reads");
  for(unsigned fault=1;fault<=13;++fault) {
    reads=0; read_fault=fault; track_reads=true; r=f.Read(); track_reads=false;
    RangeReject(r,RootError::io_failure); Check(!read_fault,"range actual read fault consumed");
  }
  for(unsigned fault:{1u,2u}) { full_digest_fault=fault; r=f.Read();
    RangeReject(r,RootError::hash_failure); Check(!full_digest_fault,"actual predecessor full-image hash failure"); }
  for(unsigned i:{0u,1u}) {
    auto damaged=f.images[i]; damaged[700]^=1; f.Write(i,damaged);
    Check(!f.Read().ok(),"deep image corruption not accepted as missing history");
    r=f.Read(); Check(r.roots.empty()&&!r.retained_image_bytes,"deep corruption emits no prefix"); f.Restore();
  }
  // Reseal both sides so these tests exercise links/epochs, not just checksums.
  for(unsigned field=0;field<6;++field) {
    auto middle=f.roots[1],head=f.roots[2];
    if(field==0) middle.catalog_generation=4;
    if(field==1) middle.schema_epoch=head.schema_epoch+1;
    if(field==2) middle.security_epoch=head.security_epoch+1;
    if(field==3) middle.resource_epoch=head.resource_epoch+1;
    if(field==4) middle.object_uuid=Id(99);
    if(field==5) middle.predecessor=PageRef(head); // cycle; no digest fixed point is needed to reject it.
    const auto bytes=RootOracle(middle); f.Write(1,bytes); head.predecessor_sha256=WholeRootHash(bytes);
    f.Write(2,RootOracle(head));
    RangeReject(f.Read(),field==4?RootError::binding_mismatch:RootError::history_mismatch); f.Restore();
  }
  {auto head=f.roots[2]; head.predecessor_sha256[0]^=1; f.Write(2,RootOracle(head));
    RangeReject(f.Read(),RootError::invalid_integrity); f.Restore();}
  // Retained ranges do not claim authority over images older than their terminal.
  {auto damaged=f.images[0]; damaged[500]^=1; f.Write(0,damaged);
    Check(f.Read(24576,1).ok(),"unrequested older image is outside the proven range"); f.Restore();}
  bool saw_failure=false,saw_success=false;
  const auto head=RootRef(f.roots[2]),terminal=RootRef(f.roots[0]);
  observed_allocations=0; count_allocations=true;
  r=page::ReadNativeCatalogRootRangeFromOpenDevices(Id(1),f.devices,head,terminal,32768);
  count_allocations=false;
  const auto allocation_count=observed_allocations;
  Check(r.ok()&&allocation_count>0,"measure actual range allocation positions");
  for(unsigned long budget=0;budget<=allocation_count;++budget) {
    allocation_budget=budget;
    r=page::ReadNativeCatalogRootRangeFromOpenDevices(Id(1),f.devices,head,terminal,32768);
    allocation_budget=-1;
    if(r.ok()) { saw_success=true; break; }
    RangeReject(r,RootError::resource_exhausted); saw_failure=true;
  }
  Check(saw_failure&&saw_success,"all allocation failure positions through successful complete range");
  std::atomic<bool> concurrent_ok=true;
  auto reverse=f.devices; std::reverse(reverse.begin(),reverse.end());
  auto reader=[&](const auto& devices) { for(unsigned i=0;i<16;++i) {
    const auto result=page::ReadNativeCatalogRootRangeFromOpenDevices(Id(1),devices,head,terminal,32768);
    if(!result.ok()||result.roots.size()!=3||result.retained_image_bytes!=32768) concurrent_ok=false;
  }};
  std::thread a([&]{reader(f.devices);}),b([&]{reader(reverse);}); a.join(); b.join();
  Check(concurrent_ok,"concurrent reversed-order range readers complete");
  for(unsigned i=0;i<3;++i) {
    Bytes bytes(f.images[i].size()); auto& device=i==1?f.second:f.first;
    Check(device.ReadAt(f.roots[i].header.page_number*f.roots[i].header.page_size_bytes,bytes.data(),bytes.size()).ok()
      &&bytes==f.images[i],"range reads/failures did not mutate durable roots");
  }
  Exclusive(f.first.path()); Exclusive(f.second.path());
  Check(f.first.Close().ok()&&f.second.Close().ok(),"close node filespaces before fresh process range read");
  const auto first=(f.files.root/"first").string(),second=(f.files.root/"second").string();
  const auto pid=::fork(); Check(pid>=0,"fork range reopen verifier");
  if(pid==0) { ::execl("/proc/self/exe","pagezero_test","--range-probe",first.c_str(),second.c_str(),nullptr); ::_exit(125); }
  int status=0; Check(::waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh process validates mixed-profile retained range");
}
namespace db=scratchbird::storage::database;
namespace catalog=scratchbird::core::catalog;
namespace platform=scratchbird::core::platform;
db::NativeCatalogLeafPage LeafExample(unsigned p=0) {
  db::NativeCatalogLeafPage leaf; leaf.header=RootExample(p).header;
  leaf.header.page_type=6; leaf.header.page_number=21; leaf.header.page_generation=7; leaf.header.page_uuid=Id(150);
  auto& b=leaf.body; b.relation_uuid={platform::UuidKind::object,Id(101)};
  b.segment_id=1; b.segment_generation=2; b.compaction_generation=3;
  b.page_number=21; b.page_generation=7;
  for(unsigned i=0;i<2;++i) {
    page::RowDataRecord row;
    row.storage_generation = 1; row.row_uuid={platform::UuidKind::row,Id(static_cast<byte>(160+i))};
    row.version_uuid=Id(static_cast<byte>(170+i)); row.transaction_uuid={platform::UuidKind::transaction,Id(162)};
    row.local_transaction_id=13; row.internal_row_ordinal=i+1; row.stable_slot_id=i+1;
    catalog::CatalogMetadataVersion metadata;
    metadata.record.header.kind=catalog::CatalogRecordKind::sql_object;
    metadata.record.header.row_uuid=row.row_uuid;
    metadata.record.header.object_uuid={platform::UuidKind::object,Id(static_cast<byte>(180+i))};
    metadata.record.header.parent_uuid={platform::UuidKind::schema,Id(164)};
    metadata.owning_schema_uuid=metadata.record.header.parent_uuid;
    metadata.owner_uuid={platform::UuidKind::principal,Id(165)}; metadata.audit_uuid={platform::UuidKind::object,Id(166)};
    metadata.creator_transaction_uuid=row.transaction_uuid; metadata.creator_local_transaction_id=13;
    metadata.definition_version=metadata.schema_epoch=metadata.security_epoch=metadata.catalog_generation=1;
    metadata.dependency_generation=metadata.invalidation_generation=1;
    metadata.lifecycle=catalog::CatalogObjectLifecycle::active; metadata.status=catalog::CatalogObjectStatus::active;
    metadata.trace_search_key="LEAF-STORAGE-ORACLE"; metadata.object_subtype="application"; metadata.retention_class="catalog_history";
    // Opaque common-envelope storage fixture, not an admitted SQL definition.
    metadata.record.payload="opaque-leaf-family-oracle";
    const auto encoded=catalog::EncodeCatalogMetadataVersion(metadata); Check(encoded.ok(),"leaf metadata fixture");
    page::RowDataCell cell; cell.column_ordinal=1;
    cell.value.type_id=scratchbird::core::datatypes::CanonicalTypeId::binary; cell.value.payload=encoded.bytes;
    row.cells.push_back(cell); b.rows.push_back(row);
  }
  return leaf;
}
u64 BodyFnv(const byte* b,std::size_t n) {
  u64 hash=1469598103934665603ull; for(std::size_t i=0;i<n;++i) {hash^=b[i];hash*=1099511628211ull;} return hash;
}
void LeafSeal(Bytes& b) {
  std::fill(b.end()-32,b.end(),0); const auto digest=WholeRootHash(b);
  std::copy(digest.begin(),digest.end(),b.end()-32);
}
Bytes LeafOracle(const db::NativeCatalogLeafPage& leaf) {
  const auto& h=leaf.header; const auto& body=leaf.body; Bytes b(h.page_size_bytes,0);
  const std::string_view cm="SBPGV002",rm="SBROW004",vm="SBDVAL01";
  std::copy(cm.begin(),cm.end(),b.begin()); Number(b,8,4,128); Number(b,12,4,h.page_size_bytes);
  Number(b,16,4,h.page_type); Number(b,20,2,1); Number(b,22,2,1);
  PutUuid(b,24,h.database_uuid); PutUuid(b,40,h.filespace_uuid); PutUuid(b,56,h.page_uuid);
  Number(b,72,8,h.page_number); Number(b,80,8,h.page_generation); Number(b,88,8,h.flags);
  PutUuid(b,104,h.page_size_profile_uuid); Number(b,120,2,1);
  u64 common=14695981039346656037ull; for(unsigned i=0;i<128;++i) {common^=b[i];common*=1099511628211ull;} Number(b,96,8,common);
  std::copy(rm.begin(),rm.end(),b.begin()+128); Number(b,136,4,96); Number(b,140,4,body.rows.size());
  Number(b,152,8,body.next_page_number); PutUuid(b,168,body.relation_uuid.value);
  Number(b,184,8,body.page_generation); Number(b,192,8,body.segment_id);
  Number(b,200,8,body.segment_generation); Number(b,208,8,body.compaction_generation);
  struct Slot {unsigned stable,offset,size;u64 hash;bool deleted;}; std::vector<Slot> slots;
  unsigned at=224;
  for(unsigned i=0;i<body.rows.size();++i) {
    const auto& row=body.rows[i]; const auto start=at;
    PutUuid(b,at,row.row_uuid.value); PutUuid(b,at+16,row.transaction_uuid.value);
    Number(b,at+32,8,row.local_transaction_id); Number(b,at+40,8,row.row_version);
    Number(b,at+48,2,row.deleted?1:0); Number(b,at+50,2,row.cells.size()); Number(b,at+52,4,i+1);
    Number(b,at+60,4,row.stable_slot_id); PutUuid(b,at+72,row.version_uuid);
    Number(b,at+88,8,row.previous_row_version); Number(b,at+96,8,row.next_row_version);
    PutUuid(b,at+104,row.previous_version_uuid); PutUuid(b,at+120,row.next_version_uuid);
    Number(b,at+136,8,row.storage_generation); at+=144;
    for(const auto& cell:row.cells) {
      const auto& payload=cell.value.payload; const unsigned value=at+16;
      Number(b,at,2,cell.column_ordinal); Number(b,at+4,4,32+payload.size());
      std::copy(vm.begin(),vm.end(),b.begin()+value); Number(b,value+8,4,static_cast<unsigned>(cell.value.type_id));
      Number(b,value+12,2,(cell.value.is_null?1:0)|(cell.value.payload_is_toast_reference?2:0));
      Number(b,value+14,2,32); Number(b,value+16,4,payload.size());
      Number(b,value+24,8,BodyFnv(payload.data(),payload.size()));
      std::copy(payload.begin(),payload.end(),b.begin()+value+32);
      Number(b,at+8,8,BodyFnv(b.data()+value,32+payload.size())); at+=48+payload.size();
    }
    Number(b,start+56,4,at-start); const auto hash=BodyFnv(b.data()+start,at-start); Number(b,start+64,8,hash);
    slots.push_back({row.stable_slot_id,start-128,at-start,hash,row.deleted});
  }
  Number(b,216,4,at-128);
  for(const auto& slot:slots) {
    Number(b,at,4,slot.stable); Number(b,at+4,4,slot.offset); Number(b,at+8,4,slot.size);
    Number(b,at+12,4,slot.deleted?1:0); Number(b,at+16,8,slot.hash); at+=24;
  }
  Number(b,144,4,at-128); Number(b,220,4,b.size()-32-at);
  Number(b,160,8,BodyFnv(b.data()+128,b.size()-160)); LeafSeal(b); return b;
}
struct CatalogBindingFixtureImages {
  std::vector<page::NativeBtreePage> nodes;
  std::vector<db::NativeCatalogLeafPage> leaves;
};
CatalogBindingFixtureImages CatalogBindingImages(unsigned p) {
  const auto q=(p+1)%5,s=(p+2)%5;CatalogBindingFixtureImages images;images.nodes=BtreeTreeExample(p);
  // Fixture row/version identities are disjoint from each other and page,
  // object and transaction identities, just as separately issued UUIDs are.
  const auto key_identity=[](auto& key){key.row_uuid.bytes[14]=0x41;key.version_uuid.bytes[14]=0x42;};
  for(auto& node:images.nodes){if(node.low_fence)key_identity(*node.low_fence);if(node.high_fence)key_identity(*node.high_fence);
    for(auto& cell:node.cells)key_identity(cell.key);}
  for(unsigned j=0;j<4;++j){auto leaf=LeafExample(j==3?q:s);auto& h=leaf.header;
    h.filespace_uuid=j==3?Id(7):Id(9);h.page_number=53+j;h.page_generation=3;h.page_uuid=Id(static_cast<byte>(150+j));
    leaf.body.page_number=h.page_number;leaf.body.page_generation=h.page_generation;
    for(unsigned i=0;i<2;++i){auto& row=leaf.body.rows[i];auto& cell=images.nodes[3+j].cells[i];
      row.row_uuid.value=cell.key.row_uuid;row.version_uuid=cell.key.version_uuid;
      auto metadata=catalog::DecodeCatalogMetadataVersion(row.cells[0].value.payload);Check(metadata.ok(),"existing common metadata fixture decoded");
      metadata.record.record.header.row_uuid=row.row_uuid;metadata.record.record.header.object_uuid.value=Id(static_cast<byte>(180+2*j+i));
      const auto encoded=catalog::EncodeCatalogMetadataVersion(metadata.record);Check(encoded.ok(),"bound common metadata fixture produced");row.cells[0].value.payload=encoded.bytes;
      cell.base_page=disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
    images.leaves.push_back(std::move(leaf));}
  auto extra=images.nodes[3].cells[0];extra.key.encoded_key={12};images.nodes[3].cells.insert(images.nodes[3].cells.begin()+1,extra);
  return images;
}
void NativeCatalogRelationBindings() {
  using E=db::NativeCatalogRelationError;using LE=db::NativeCatalogLeafError;
  const auto empty=[&](const auto& r){Check(!r.ok()&&!r.index&&r.catalogs.empty()&&r.bindings.empty()&&r.retained_image_bytes==0,"catalog join refusal exposes no tree pages metadata bindings or counters");};
  for(unsigned p=0;p<5;++p){const auto q=(p+1)%5,s=(p+2)%5;Fixture fixture;disk::FileDevice first,second,base;
    const auto path1=(fixture.root/"catalog-index").string(),path2=(fixture.root/"catalog-data").string(),path3=(fixture.root/"catalog-primary").string();
    auto z1=Example(p,6),z2=Example(q,5),z3=Example(s,1);z2.bootstrap.filespace_uuid=Id(7);z3.bootstrap.filespace_uuid=Id(9);
    for(auto& r:z2.roots)r.filespace_uuid=Id(7);for(auto& r:z3.roots)r.filespace_uuid=Id(9);
    const auto prepare=[&](auto& device,const auto& path,const auto& zero){const auto image=Oracle(zero);const byte padding=0;
      Check(device.Open(path,disk::FileOpenMode::create_new).ok()&&device.WriteAt(0,image.data(),image.size()).ok()
        &&device.WriteAt(zero.total_pages*zero.bootstrap.page_size_bytes-1,&padding,1).ok(),"prepare real catalog relation filespace");};
    prepare(first,path1,z1);prepare(second,path2,z2);prepare(base,path3,z3);
    const auto original=CatalogBindingImages(p);auto images=original;
    const auto reference=[](const auto& h){return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};};
    const auto persist=[&]{for(const auto& n:images.nodes){const auto b=BtreeOracle(n);auto& device=n.header.filespace_uuid==Id(2)?first:second;
      Check(device.WriteAt(n.header.page_number*n.header.page_size_bytes,b.data(),b.size()).ok(),"persist catalog navigation image");}
      for(const auto& leaf:images.leaves){const auto b=LeafOracle(leaf);auto& device=leaf.header.filespace_uuid==Id(9)?base:second;
        Check(device.WriteAt(leaf.header.page_number*leaf.header.page_size_bytes,b.data(),b.size()).ok(),"persist actual native catalog leaf");}
      Check(first.Sync().ok()&&second.Sync().ok()&&base.Sync().ok(),"sync native catalog index and base images");};
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(9),Profile(s),&base},{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};
    const page::NativeCatalogRootReference indexed{1,0x200,reference(original.nodes[0].header),original.nodes[0].dependencies.index_uuid};
    const db::NativeCatalogRelationBinding binding{Id(101),original.nodes[0].dependencies};
    const page::NativeCatalogRootReference direct{1,6,reference(original.leaves[0].header),Id(101)};
    const u64 budget=4*sizes[p]+4*sizes[q]+3*sizes[s];
    const auto read=[&](u64 allowance){return db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),devices,indexed,binding,allowance);};
    persist();auto result=read(budget);Check(result.ok()&&result.index&&result.index->pages.size()==7&&result.catalogs.size()==4&&result.bindings.size()==9
      &&result.retained_image_bytes==budget,"actual catalog tree to row/version join with distinct-image budget");
    for(const auto& b:result.bindings){Check(b.index_entry.has_value(),"indexed binding retains its source entry");
      const auto& cell=result.index->pages[b.index_entry->page_index].page->cells[b.index_entry->cell_index];
      const auto& leaf=result.catalogs[b.catalog_page_index];const auto& row=leaf.page->body.rows[b.catalog_row_index];
      Check(row.row_uuid.value==cell.key.row_uuid&&row.version_uuid==cell.key.version_uuid
        &&leaf.metadata.at(row.version_uuid).record.header.row_uuid.value==row.row_uuid.value,"bindings refer to actual native rows and validated metadata");}
    Check(result.bindings[0].catalog_page_index==result.bindings[1].catalog_page_index&&result.bindings[0].catalog_row_index==result.bindings[1].catalog_row_index,
      "multiple keys retain separate bindings without duplicate base images");
    const auto deleted=result.bindings[2].index_entry;Check(deleted&&result.index->pages[deleted->page_index].page->cells[deleted->cell_index].deleted,
      "deleted index entry remains bound for owning MGA interpretation");
    result=read(budget-1);empty(result);Check(result.error==E::resource_exhausted,"one-byte-short last base image refuses whole join");
    result=db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),devices,direct,{Id(101),{}},sizes[s]);
    Check(result.ok()&&!result.index&&result.catalogs.size()==1&&result.bindings.size()==2&&result.retained_image_bytes==sizes[s]
      &&!result.bindings[0].index_entry,"direct leaf route enumerates actual retained rows");
    result=db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),devices,direct,{Id(101),{}},sizes[s]-1);empty(result);Check(result.error==E::resource_exhausted,"direct leaf exact budget boundary");
    if(p==0){
      for(unsigned fault=0;fault<9;++fault){images=original;E expected=E::invalid_locator;
        if(fault==0){images.nodes[6].cells[1].key.row_uuid=Id(240);expected=E::binding_mismatch;}
        if(fault==1)images.nodes[6].cells[1].key.version_uuid=Id(241);
        if(fault==2)images.nodes[6].cells[1].base_page=images.nodes[3].cells[0].base_page;
        if(fault==3)images.nodes[6].cells[1].base_page.reset();
        if(fault==4){++images.nodes[3].cells[1].base_page->page_generation;expected=E::binding_mismatch;}
        if(fault==5){images.leaves[3].header.page_uuid=images.leaves[0].header.page_uuid;expected=E::duplicate_identity;}
        if(fault==6){images.leaves[3].body.rows[0].version_uuid=images.leaves[0].body.rows[1].version_uuid;expected=E::duplicate_identity;}
        if(fault==7)images.nodes[3].cells[0].base_page=reference(images.nodes[0].header);
        if(fault==8){auto row=images.leaves[3].body.rows[0];row.row_uuid.value=Id(240);row.version_uuid=Id(241);row.internal_row_ordinal=3;row.stable_slot_id=3;
          images.leaves[3].body.rows.push_back(row);expected=E::leaf_failure;}
        persist();result=read(budget);empty(result);Check(result.error==expected,"exact catalog binding fault="+std::to_string(fault)+" actual="+std::to_string(static_cast<unsigned>(result.error))+" expected="+std::to_string(static_cast<unsigned>(expected)));
        if(fault==8)Check(result.leaf_error==LE::invalid_metadata,"unindexed malformed retained row is not skipped");}
      images=original;persist();
      auto wrong=binding;wrong.relation_uuid=Id(240);result=db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),devices,indexed,wrong,budget);empty(result);
      Check(result.error==E::leaf_failure&&result.leaf_error==LE::binding_mismatch,"actual relation owner binding required");
      auto bad_head=indexed;bad_head.object_uuid=Id(240);result=db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),devices,bad_head,binding,budget);empty(result);Check(result.error==E::binding_mismatch,"head index UUID must match exact dependency tuple");
      result=db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),devices,direct,binding,budget);empty(result);Check(result.error==E::binding_mismatch,"direct head cannot inherit an unrelated index dependency tuple");
      for(u64 flags:{1u,2u,4u,8u}){images=original;images.leaves[3].header.flags=flags;persist();result=read(budget);empty(result);
        Check(result.error==E::leaf_failure&&result.leaf_error==(flags==1?LE::encrypted_requires_crypto_authority:flags==2?LE::cluster_requires_authority:LE::header_policy_requires_authority),"late base image retains truthful crypto cluster header policy failure");}
      images=original;persist();auto corrupt=LeafOracle(images.leaves[3]);corrupt.back()^=1;
      Check(second.WriteAt(56*sizes[q],corrupt.data(),corrupt.size()).ok(),"corrupt actual final catalog leaf digest");result=read(budget);empty(result);Check(result.error==E::leaf_failure&&result.leaf_error==LE::invalid_integrity,"late corrupt catalog page exposes no earlier metadata");persist();
      const auto mutex_of=[](auto& device){auto guard=device.AcquireOperationGuard();return guard.mutex();};const std::array mutexes{mutex_of(first),mutex_of(second),mutex_of(base)};
      tree_read_paused=false;resume_tree_read=false;pause_next_tree_read=true;std::atomic<bool> done{false};db::NativeCatalogRelationImageResult paused_result;
      std::thread reader([&]{paused_result=read(budget);done=true;});while(!tree_read_paused.load()&&!done.load())std::this_thread::yield();
      bool all_held=tree_read_paused.load();for(auto* mutex:mutexes)if(mutex->try_lock()){all_held=false;mutex->unlock();}resume_tree_read=true;reader.join();pause_next_tree_read=false;
      Check(all_held&&paused_result.ok(),"all catalog join filespace guards held before the first physical read");
      reads=0;track_reads=true;result=read(budget);track_reads=false;const auto read_count=reads;Check(result.ok(),"measure complete catalog join reads");
      for(unsigned fault=1;fault<=read_count;++fault){reads=0;read_fault=fault;track_reads=true;result=read(budget);track_reads=false;Check(read_fault==0,"each actual catalog join read fault consumed");empty(result);}
      observed_allocations=0;count_allocations=true;result=read(budget);count_allocations=false;const auto allocations=observed_allocations;Check(result.ok(),"measure catalog join allocations");bool success=false;
      for(unsigned long allowance=0;allowance<=allocations;++allowance){allocation_budget=allowance;result=read(budget);allocation_budget=-1;if(result.ok()){success=true;break;}
        empty(result);Check(result.error==E::resource_exhausted||(result.error==E::tree_failure&&result.tree_error==page::NativeBtreeError::resource_exhausted)
          ||(result.error==E::leaf_failure&&result.leaf_error==LE::resource_exhausted),"catalog join allocation refusal classified");}Check(success,"every catalog join allocation fault position");
      for(unsigned fault=1;fault<=5;++fault){hash_fault=fault;result=read(budget);Check(hash_fault==0,"catalog join digest backend fault consumed");empty(result);Check(result.error==E::hash_failure,"catalog join digest failure preserved");}
      images.leaves[0].body.rows.clear();persist();result=db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),devices,direct,{Id(101),{}},sizes[s]);
      Check(result.ok()&&result.catalogs.size()==1&&result.bindings.empty(),"empty allocated direct catalog leaf, not invented existence");
      images=original;images.nodes[0].tree_level=0;images.nodes[0].first_child.reset();images.nodes[0].cells.clear();persist();result=read(sizes[p]);
      Check(result.ok()&&result.index&&result.index->pages.size()==1&&result.catalogs.empty()&&result.bindings.empty(),"empty allocated catalog index is represented without invented base rows");images=original;persist();
    }
    Exclusive(path1);Exclusive(path2);Exclusive(path3);Check(first.Close().ok()&&second.Close().ok()&&base.Close().ok(),"close catalog join files before independent process");
    const auto child=::fork();Check(child>=0,"fork actual catalog relation reader");if(child==0){const auto profile=std::to_string(p);
      ::execl("/proc/self/exe","catalog-relation-probe","--catalog-relation-probe",fixture.root.c_str(),profile.c_str(),nullptr);::_exit(125);}
    int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable binds persisted index entries to catalog rows");
  }
}
void LeafReject(const db::NativeCatalogLeafResult& r,db::NativeCatalogLeafError error,
                std::source_location at=std::source_location::current()) {
  Check(!r.ok()&&!r.page&&r.metadata.empty()&&r.bytes.empty()&&r.error==error,"leaf rejects without partial page/metadata actual="+
    std::to_string(static_cast<unsigned>(r.error))+" expected="+std::to_string(static_cast<unsigned>(error)),at);
}

bool SameBinaryDiagnostic(const scratchbird::core::datatypes::DatatypeBinaryDiagnosticView& a,
                          const scratchbird::core::datatypes::DatatypeBinaryDiagnosticView& b) {
  if(a.abi_version!=b.abi_version || a.status.code!=b.status.code ||
     a.status.severity!=b.status.severity || a.status.subsystem!=b.status.subsystem ||
     a.diagnostic_code!=b.diagnostic_code || a.message_key!=b.message_key ||
     a.origin!=b.origin || a.argument_count!=b.argument_count)return false;
  for(std::size_t i=0;i<a.arguments.size();++i){const auto& x=a.arguments[i];const auto& y=b.arguments[i];
    if(x.key!=y.key || x.kind!=y.kind || x.text!=y.text || x.unsigned_integer!=y.unsigned_integer ||
       x.uuid!=y.uuid || x.generation!=y.generation)return false;
  }
  return true;
}

struct BorrowedRowWorkspace {
  std::vector<page::RowDataRecordView> rows;
  std::vector<page::RowDataCellView> cells;
  std::vector<page::RowDataSlot> slots;
  std::vector<platform::u32> versions,sequences;
  explicit BorrowedRowWorkspace(std::size_t bytes) {
    const auto size=page::RowDataPageViewBackingRequirements(bytes);
    rows.resize(size.rows);cells.resize(size.cells);slots.resize(size.rows);
    versions.resize(size.index_slots);sequences.resize(size.index_slots);
  }
  page::RowDataPageViewWorkspace get() { return {rows,cells,slots,versions,sequences}; }
};
struct LeafMemoryFixture {
  scratchbird::core::memory::MemoryManager manager;
  scratchbird::core::memory::HierarchicalMemoryBudgetLedger ledger{3,5};
  db::NativeStorageMemoryBinding binding{Id(1),Id(220),Id(221),Id(222)};
  db::NativeStorageMemory memory;
  static auto Policy(u64 bytes){auto p=scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();
    p.hard_limit_bytes=p.per_context_limit_bytes=4*bytes;p.soft_limit_bytes=0;return p;}
  explicit LeafMemoryFixture(u64 bytes):manager(Policy(bytes)){
    namespace mem=scratchbird::core::memory;
    mem::ReservationBackedMemoryResourceRequest r;r.memory_manager=&manager;r.reservation_ledger=&ledger;
    r.consumer_kind=mem::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.category=mem::MemoryCategory::page_buffer;r.requested_bytes=bytes;r.memory_class="page_buffer";
    r.route_label="storage leaf conformance";r.purpose="actual image and decoded metadata";
    r.binary_operation_uuid=binding.operation_uuid.bytes;
    r.binary_ownership[mem::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
    r.binary_ownership[mem::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
    r.binary_ownership[mem::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
    r.scope_chain={{mem::HierarchicalMemoryScopeKind::process,{},Id(223).bytes},
      {mem::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
    r.provenance.source=mem::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label="actual storage memory conformance";
    for(const auto& scope:r.scope_chain){mem::HierarchicalMemoryBudget b;b.scope=scope;b.hard_limit_bytes=bytes;b.provenance=r.provenance;
      Check(ledger.SetBudget(b).ok(),"real leaf parent budget");}
    auto grant=mem::AcquireReservationBackedMemoryResource(r);Check(grant.ok(),"real leaf memory grant");
    auto adopted=db::AdoptNativeStorageMemory(binding,grant.resource);
    Check(adopted.ok()&&!grant.resource,"exclusive binary-bound leaf grant adoption");memory=std::move(adopted.memory);
  }
  void Empty(){const auto s=manager.Snapshot();Check(!s.current_bytes&&!s.reserved_capacity_bytes&&
    !s.active_capacity_reservation_count&&!ledger.Snapshot().current_bytes,"leaf backing and grant completely released");}
};
void GovernedBtreeParity(const std::vector<disk::NativeFilespaceDevice>& files,
    const disk::NativePageReference& root,const page::NativeBtreeDependencies& dependencies,
    u64 bytes,const page::NativeBtreeTreeResult& expected) {
  const db::NativeBtreeTreeMemoryLimits limits{31,static_cast<std::size_t>(bytes)};
  const auto amount=db::NativeBtreeTreeWorkspaceBytes(files,limits);LeafMemoryFixture grant(amount);
  auto result=db::ReadNativeBtreeTreeWithMemoryFromOpenDevices(Id(1),files,root,dependencies,limits,grant.memory,grant.binding);
  Check(result.ok()==expected.ok()&&result.tree_error==expected.error,"grant-backed and owning complete-tree classification parity");
  if(expected.ok()){
    Check(result.pages.size()==expected.pages.size()&&result.retained_image_bytes==expected.retained_image_bytes&&
      std::equal(result.leaves.begin(),result.leaves.end(),expected.leaves.begin(),expected.leaves.end()),"identical complete retained topology and leaf order");
    for(std::size_t i=0;i<result.pages.size();++i)Check(BtreeOracle(*result.pages[i].page)==expected.pages[i].bytes,
      "grant-backed complete decoded tree independently reconstructs owning wire bytes");
  }else Check(!result.arena&&result.pages.empty()&&result.leaves.empty()&&!result.retained_image_bytes,
      "invalid later tree publishes no grant-backed prefix");
  result={};grant.memory={};grant.Empty();
}
void NativeBtreeTreeMemory() {
  using E=db::NativeBtreeTreeMemoryError;using T=page::NativeBtreeError;
  const auto empty=[](const auto& r){Check(!r.ok()&&!r.arena&&r.pages.empty()&&r.leaves.empty()&&!r.retained_image_bytes,
    "failed governed tree returns no retained image or topology prefix");};
  for(unsigned p=0;p<5;++p)for(unsigned q=0;q<5;++q){const auto s=(p+2)%5;
    Fixture fixture;disk::FileDevice first,second,base;
    const auto path1=(fixture.root/"memory-tree-first").string(),path2=(fixture.root/"memory-tree-second").string(),path3=(fixture.root/"memory-tree-base").string();
    auto z1=Example(p,6),z2=Example(q,5),z3=Example(s,1);z2.bootstrap.filespace_uuid=Id(7);z3.bootstrap.filespace_uuid=Id(9);
    for(auto& r:z2.roots)r.filespace_uuid=Id(7);for(auto& r:z3.roots)r.filespace_uuid=Id(9);
    const auto prepare=[&](auto& device,const auto& path,const auto& zero){const auto image=Oracle(zero);const byte padding=0;
      Check(device.Open(path,disk::FileOpenMode::create_new).ok()&&device.WriteAt(0,image.data(),image.size()).ok()&&
        device.WriteAt(zero.total_pages*zero.bootstrap.page_size_bytes-1,&padding,1).ok(),"prepare actual governed tree filespace");};
    prepare(first,path1,z1);prepare(second,path2,z2);prepare(base,path3,z3);
    auto nodes=BtreeTreeExample(p);
    for(auto& n:nodes){if(n.header.filespace_uuid==Id(7)){n.header.page_size_bytes=sizes[q];n.header.page_size_profile_uuid=Profile(q);}
      const auto reprofile=[&](auto& r){if(r&&r->filespace_uuid==Id(7))r->page_size_profile_uuid=Profile(q);};
      reprofile(n.parent);reprofile(n.left);reprofile(n.right);reprofile(n.first_child);
      for(auto& c:n.cells){reprofile(c.child);reprofile(c.base_page);}}
    const auto persist=[&]{for(const auto& n:nodes){const auto bytes=BtreeOracle(n);auto& device=n.header.filespace_uuid==Id(2)?first:second;
      Check(device.WriteAt(n.header.page_number*n.header.page_size_bytes,bytes.data(),bytes.size()).ok(),"persist independent governed tree image");}
      Check(first.Sync().ok()&&second.Sync().ok()&&base.Sync().ok(),"sync actual governed tree");};persist();
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(9),Profile(s),&base},{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};
    const auto& h=nodes[0].header;const disk::NativePageReference root{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};
    const db::NativeBtreeTreeMemoryLimits limits{7,static_cast<std::size_t>(4*sizes[p]+3*sizes[q])};
    const auto amount=db::NativeBtreeTreeWorkspaceBytes(devices,limits);Check(amount>limits.maximum_retained_image_bytes,"whole-tree grant covers images and all decoded traversal backing");
    LeafMemoryFixture grant(amount);
    const auto read=[&]{return db::ReadNativeBtreeTreeWithMemoryFromOpenDevices(Id(1),devices,root,nodes[0].dependencies,limits,grant.memory,grant.binding);};
    const auto oracle=[&](const auto& r){
      const std::array<std::size_t,7> preorder{0,1,3,4,2,5,6};const std::array<std::size_t,4> leaves{2,3,5,6};
      Check(r.ok()&&r.pages.size()==7&&r.retained_image_bytes==limits.maximum_retained_image_bytes&&
        std::equal(r.leaves.begin(),r.leaves.end(),leaves.begin(),leaves.end()),"exact mixed-profile governed tree and leaf order");
      for(std::size_t i=0;i<7;++i){const auto bytes=BtreeOracle(nodes[preorder[i]]);
        Check(BtreeOracle(*r.pages[i].page)==bytes&&std::equal(r.pages[i].bytes.begin(),r.pages[i].bytes.end(),bytes.begin(),bytes.end()),
          "complete granted tree matches independent images including every key and native reference");}
      Check(grant.manager.Snapshot().current_bytes==amount&&grant.memory.Snapshot().allocated_bytes==amount,"all retained tree backing remains actually charged");};
    reads=0;track_reads=true;auto result=read();track_reads=false;const auto read_count=reads;oracle(result);
    Check(read_count==13,"six bootstrap/pagezero reads plus seven tree pages");result={};
    auto held=first.AcquireOperationGuard();auto* mutex=held.mutex();held.unlock();
    FencedHeapProbe heap_probe;heap_probe.mutex=mutex;std::thread observer([&]{heap_probe.Run();});
    fenced_heap_probe=&heap_probe;deny_heap_on_next_read=true;result=read();const auto remaining=allocation_budget;
    allocation_budget=-1;fenced_heap_probe=nullptr;heap_probe.stop=true;observer.join();
    Check(!deny_heap_on_next_read&&remaining==0&&heap_probe.outside_allocations>0,"complete tree has no C++ allocation under cohort fences");oracle(result);result={};
    for(auto short_limit:{db::NativeBtreeTreeMemoryLimits{6,limits.maximum_retained_image_bytes},
        db::NativeBtreeTreeMemoryLimits{7,limits.maximum_retained_image_bytes-1}}){
      auto refused=db::ReadNativeBtreeTreeWithMemoryFromOpenDevices(Id(1),devices,root,nodes[0].dependencies,short_limit,grant.memory,grant.binding);
      empty(refused);Check(refused.error==E::resource_exhausted&&!grant.manager.Snapshot().current_bytes,"exact page and byte ceilings refuse without a prefix");}
    for(unsigned fault=1;fault<=read_count;++fault){reads=0;track_reads=true;read_fault=fault;auto refused=read();track_reads=false;
      empty(refused);Check(!read_fault&&refused.tree_error==T::io_failure&&!grant.manager.Snapshot().current_bytes,"every actual governed tree read failure releases backing");}
    for(unsigned fault=1;fault<=5;++fault){hash_fault=fault;auto refused=read();empty(refused);
      Check(!hash_fault&&refused.tree_error==T::hash_failure&&!grant.manager.Snapshot().current_bytes,"every digest provider failure remains typed and releases backing");}
    if(p==0&&q==0){
      for(unsigned field=0;field<4;++field){auto wrong=grant.binding;
        std::array<platform::Uuid*,4> ids{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};*ids[field]=Id(230);
        reads=0;track_reads=true;auto refused=db::ReadNativeBtreeTreeWithMemoryFromOpenDevices(Id(1),devices,root,nodes[0].dependencies,limits,grant.memory,wrong);track_reads=false;
        empty(refused);Check(refused.error==E::memory_binding_failure&&!reads,"all four binary grant identities required before I/O");}
      for(unsigned variant=0;variant<3;++variant){auto wrong=devices;if(variant==0)wrong.push_back(wrong[0]);
        if(variant==1)wrong[0].device=&first;if(variant==2)wrong[1].filespace_uuid=Uuid{};
        LeafMemoryFixture extra(db::NativeBtreeTreeWorkspaceBytes(wrong,limits));reads=0;track_reads=true;
        auto refused=db::ReadNativeBtreeTreeWithMemoryFromOpenDevices(Id(1),wrong,root,nodes[0].dependencies,limits,extra.memory,extra.binding);track_reads=false;
        empty(refused);Check(refused.tree_error==T::invalid_filespace&&!reads,"duplicate identities aliased handles and nil filespaces refused before I/O");}
      const auto original=Oracle(z2);
      for(unsigned role=1;role<=14;++role) {
        auto placed=Example(q,role);placed.bootstrap.filespace_uuid=z2.bootstrap.filespace_uuid;placed.page_uuid=z2.page_uuid;
        for(auto& ref:placed.roots)ref.filespace_uuid=placed.bootstrap.filespace_uuid;
        const auto bytes=Oracle(placed);
        Check(disk::DecodeFilespacePageZero(bytes.data(),bytes.size()).ok(),"governed tree placement uses a registered complete filespace bootstrap");
        Check(second.WriteAt(0,bytes.data(),bytes.size()).ok(),"persist actual governed index placement role");
        {auto accepted=read();Check(accepted.ok()&&accepted.pages.size()==7,
            "actual memory-backed tree reads exact navigation in every registered placement role");}
        Check(!grant.manager.Snapshot().current_bytes,"placement read releases actual physical backing");
      }
      Check(second.WriteAt(0,original.data(),original.size()).ok(),"restore actual placement bootstrap");
      for(unsigned flag:{1u,2u}){auto bad=z2;bad.bootstrap.flags=flag;
        if(flag==1)bad.bootstrap.encryption_profile_uuid=Id(4);const auto bytes=Oracle(bad);
        Check(disk::DecodeFilespacePageZero(bytes.data(),bytes.size()).ok(),"authority requirement fixture is a valid page zero");
        Check(second.WriteAt(0,bytes.data(),bytes.size()).ok(),"persist actual encrypted or cluster filespace requirement");
        auto refused=read();empty(refused);Check(refused.tree_error==(flag==1?T::encrypted_requires_crypto_authority:T::cluster_requires_authority),"actual filespace flags cannot be bypassed by valid page bytes");}
      Check(second.WriteAt(0,original.data(),original.size()).ok(),"restore actual local plaintext filespace");
      for(unsigned flag:{1u,2u,4u,8u}){auto bad=nodes.back();bad.header.flags=flag;const auto bytes=BtreeOracle(bad);
        Check(second.WriteAt(bad.header.page_number*sizes[q],bytes.data(),bytes.size()).ok(),"persist header authority requirements");
        auto refused=read();empty(refused);Check(refused.tree_error==(flag==1?T::encrypted_requires_crypto_authority:flag==2?T::cluster_requires_authority:T::header_policy_requires_authority),"complete tree respects actual page authority requirements");}persist();
      auto reversed=devices;std::reverse(reversed.begin(),reversed.end());LeafMemoryFixture other(amount);
      std::atomic<bool> success=true;std::thread a([&]{for(unsigned i=0;i<3;++i)if(!read().ok())success=false;});
      std::thread b([&]{for(unsigned i=0;i<3;++i)if(!db::ReadNativeBtreeTreeWithMemoryFromOpenDevices(Id(1),reversed,root,nodes[0].dependencies,limits,other.memory,other.binding).ok())success=false;});
      a.join();b.join();Check(success,"governed trees with opposite caller order use the same cohort lock order");
      const auto mutex_of=[](auto& d){auto lock=d.AcquireOperationGuard();return lock.mutex();};
      const std::array mutexes{mutex_of(first),mutex_of(second),mutex_of(base)};
      tree_read_paused=false;resume_tree_read=false;pause_next_tree_read=true;std::atomic<bool> done=false;db::NativeBtreeTreeMemoryResult paused;
      std::thread reader([&]{paused=read();done=true;});while(!tree_read_paused.load()&&!done.load())std::this_thread::yield();
      bool all_held=tree_read_paused.load();for(auto* m:mutexes)if(m->try_lock()){all_held=false;m->unlock();}
      resume_tree_read=true;reader.join();pause_next_tree_read=false;Check(all_held,"all filespace fences precede first governed read");oracle(paused);paused={};
      auto warm=read();Check(warm.ok(),"warm actual whole-tree allocator");warm={};
      observed_allocations=0;count_allocations=true;warm=read();count_allocations=false;
      const auto allocation_count=observed_allocations;Check(warm.ok(),"measure complete whole-tree allocations");warm={};
      for(unsigned long fault=0;fault<=allocation_count;++fault){LeafMemoryFixture isolated(amount);
        const auto attempt=[&]{return db::ReadNativeBtreeTreeWithMemoryFromOpenDevices(Id(1),devices,root,nodes[0].dependencies,limits,isolated.memory,isolated.binding);};
        auto primed=attempt();Check(primed.ok(),"warm independent fault grant");primed={};
        observed_allocations=0;count_allocations=true;primed=attempt();count_allocations=false;
        Check(primed.ok()&&observed_allocations==allocation_count,"identical warmed tree allocation sequence");primed={};
        const auto losses=first.failed_io_latency_observations()+second.failed_io_latency_observations()+base.failed_io_latency_observations();
        const auto truncation=isolated.manager.Snapshot().telemetry_truncation_count;
        allocation_budget=fault;auto injected=attempt();const auto budget=allocation_budget;allocation_budget=-1;
        Check(fault==allocation_count?budget==0:budget==-1,"each measured tree allocation fault consumed");
        if(!injected.ok()){empty(injected);Check(!isolated.manager.Snapshot().current_bytes,"failed tree allocation releases complete backing");}
        else {Check(injected.pages.size()==7&&injected.retained_image_bytes==limits.maximum_retained_image_bytes&&
            isolated.manager.Snapshot().current_bytes==amount,"optional observation failure retains fully charged complete tree");
          for(std::size_t i=0;i<7;++i){const std::array<std::size_t,7> order{0,1,3,4,2,5,6};
            Check(BtreeOracle(*injected.pages[i].page)==BtreeOracle(nodes[order[i]]),"successful fault path retains exact independently reconstructed tree");}
          if(fault<allocation_count)Check(isolated.manager.Snapshot().telemetry_truncation_count>truncation||
            first.failed_io_latency_observations()+second.failed_io_latency_observations()+base.failed_io_latency_observations()>losses,
            "only explicit optional telemetry loss may keep a tree successful after allocation failure");}
        injected={};isolated.memory={};isolated.Empty();
      }
      Check(!db::NativeBtreeTreeWorkspaceBytes(devices,{std::numeric_limits<std::size_t>::max(),8192})&&
        !db::NativeBtreeTreeWorkspaceBytes(devices,{7,std::numeric_limits<std::size_t>::max()}),"overflowing memory limits never wrap into a small reservation");
    }
    LeafMemoryFixture short_grant(amount-1);reads=0;track_reads=true;
    auto refused=db::ReadNativeBtreeTreeWithMemoryFromOpenDevices(Id(1),devices,root,nodes[0].dependencies,limits,short_grant.memory,short_grant.binding);track_reads=false;
    empty(refused);Check(refused.error==E::memory_allocation_failure&&!reads&&!short_grant.manager.Snapshot().current_bytes,"one-byte-short actual grant refuses before device access");
    result=read();oracle(result);const auto revoked=grant.ledger.CleanupOwner(grant.binding.owner_uuid.bytes);
    Check(revoked.retained_bytes==amount,"revocation retains all live tree backing");empty(read());
    Check(first.Close().ok()&&second.Close().ok()&&base.Close().ok(),"close actual devices with immutable tree still retained");oracle(result);grant.memory={};
    allocation_budget=0;result={};const auto release_budget=allocation_budget;allocation_budget=-1;
    Check(release_budget==0,"last whole-tree owner releases actual backing without heap allocation");grant.Empty();
    Check(first.Open(path1,disk::FileOpenMode::open_existing_read_only).ok()&&second.Open(path2,disk::FileOpenMode::open_existing_read_only).ok()&&base.Open(path3,disk::FileOpenMode::open_existing_read_only).ok(),"fresh read-only tree devices");
    LeafMemoryFixture reopened(amount);auto fresh=db::ReadNativeBtreeTreeWithMemoryFromOpenDevices(Id(1),devices,root,nodes[0].dependencies,limits,reopened.memory,reopened.binding);
    Check(fresh.ok()&&fresh.pages.size()==7&&fresh.retained_image_bytes==limits.maximum_retained_image_bytes,"new grant and reopened files reproduce complete tree");
  }
}
bool SameBorrowedRowBody(const page::RowDataPageView& a,const page::RowDataPageBody& b) {
  const auto same_typed=[](const auto& x,const auto& y){return x.kind==y.kind&&x.value==y.value;};
  if(!same_typed(a.relation_uuid,b.relation_uuid)||a.segment_id!=b.segment_id||
     a.segment_generation!=b.segment_generation||a.page_number!=b.page_number||
     a.page_generation!=b.page_generation||a.compaction_generation!=b.compaction_generation||
     a.next_page_number!=b.next_page_number||a.free_space_offset!=b.free_space_offset||
     a.free_space_bytes!=b.free_space_bytes||a.rows.size()!=b.rows.size()||a.slots.size()!=b.slots.size())return false;
  for(std::size_t i=0;i<a.rows.size();++i){const auto& x=a.rows[i];const auto& y=b.rows[i];
    if(!same_typed(x.row_uuid,y.row_uuid)||!same_typed(x.transaction_uuid,y.transaction_uuid)||
       x.version_uuid!=y.version_uuid||x.previous_version_uuid!=y.previous_version_uuid||
       x.next_version_uuid!=y.next_version_uuid||x.local_transaction_id!=y.local_transaction_id||
       x.internal_row_ordinal!=y.internal_row_ordinal||x.stable_slot_id!=y.stable_slot_id||
       x.row_version!=y.row_version||x.storage_generation!=y.storage_generation||
       x.previous_row_version!=y.previous_row_version||x.next_row_version!=y.next_row_version||
       x.deleted!=y.deleted||x.cells.size()!=y.cells.size())return false;
    for(std::size_t j=0;j<x.cells.size();++j){const auto& c=x.cells[j];const auto& d=y.cells[j];
      if(c.column_ordinal!=d.column_ordinal||c.value.type_id!=d.value.type_id||
         c.value.is_null!=d.value.is_null||c.value.payload_is_toast_reference!=d.value.payload_is_toast_reference||
         c.value.payload_bytes!=d.value.payload.size())return false;
      if(c.value.payload_bytes&&!std::equal(d.value.payload.begin(),d.value.payload.end(),c.value.payload_data))return false;
    }
  }
  for(std::size_t i=0;i<a.slots.size();++i){const auto& x=a.slots[i];const auto& y=b.slots[i];
    if(x.stable_slot_id!=y.stable_slot_id||x.row_offset!=y.row_offset||x.row_bytes!=y.row_bytes||
       x.row_checksum!=y.row_checksum||x.deleted!=y.deleted)return false;
  }
  return true;
}

void CatalogBinaryCells() {
  namespace dt=scratchbird::core::datatypes;
  constexpr dt::DatatypeBinaryDiagnosticContextV1 context{
    {{0x2d,0x01,0,0,0x62,0x69,0x7e,0x61,0xb2,0x79,0,0,0,0,0,0}},1};
  using K=dt::DatatypeBinaryDiagnosticArgumentKind;
  for(unsigned p=0;p<5;++p)for(bool empty_payload:{false,true}){
    auto leaf=LeafExample(p);auto row=leaf.body.rows.front();leaf.body.rows.clear();leaf.body.slots.clear();
    if(empty_payload)row.cells[0].value.payload.clear();
    const auto count=(sizes[p]-256)/(216+row.cells[0].value.payload.size());
    Check(count>1,"dense profile has multiple real binary row frames");
    for(unsigned i=0;i<count;++i){row.row_version=i+1;row.stable_slot_id=i+1;
      row.previous_row_version=i;row.previous_version_uuid=i?leaf.body.rows.back().version_uuid:Uuid{};
      row.next_row_version=0;row.next_version_uuid={};row.version_uuid=Id(0);
      // Reverse version UUID order forces metadata sorting to preserve the
      // original row ordinal; row sequence order and lineage stay increasing.
      row.version_uuid.bytes[13]=0xab;row.version_uuid.bytes[14]=(count-i)>>8;
      row.version_uuid.bytes[15]=(count-i)&255;leaf.body.rows.push_back(row);
    }
    const auto image=LeafOracle(leaf);const std::span<const byte> body{image.data()+128,image.size()-160};
    const auto expected=page::ParseRowDataPageRowsWithCanonicalBinaryCells(body,leaf.header.page_number,context);
    Check(expected.ok()&&expected.body.rows.size()==count,"independent maximally dense row frame");
    BorrowedRowWorkspace scratch(body.size());allocation_budget=0;
    const auto view=page::ParseRowDataPageWithCanonicalBinaryCellsInto(body,leaf.header.page_number,scratch.get(),context);
    const auto remaining=allocation_budget;allocation_budget=-1;
    Check(remaining==0&&view.ok()&&SameBorrowedRowBody(view.body,expected.body),
      "maximal per-profile rows and lineage retain every field without heap");
    if(!empty_payload){std::vector<db::NativeCatalogLeafRecordView> metadata(scratch.rows.size());
      allocation_budget=0;const auto decoded=db::DecodeNativeCatalogLeafInto(image,scratch.get(),metadata);
      const auto remaining=allocation_budget;allocation_budget=-1;
      Check(remaining==0&&decoded.ok()&&decoded.page->metadata.size()==count,
        "maximally dense metadata leaf admits without hidden allocation");
      for(unsigned i=0;i<count;++i)Check(decoded.page->metadata[i].row_index==count-i-1&&
        decoded.page->metadata[i].version_uuid==leaf.body.rows[count-i-1].version_uuid,
        "dense sorted metadata preserves exact binary identity and physical row ordinal");
    }
    row.row_version=count+1;row.stable_slot_id=count+1;row.previous_row_version=count;
    row.previous_version_uuid=leaf.body.rows.back().version_uuid;row.version_uuid.bytes[13]=0xac;
    leaf.body.rows.push_back(row);const auto overflow=page::BuildRowDataPageBody(leaf.body,sizes[p]-32);
    Check(!overflow.ok()&&overflow.diagnostic.diagnostic_code=="SB-ROW-DATA-PAGE-BODY-TOO-LARGE",
      "one additional complete row cannot cross the profile capacity boundary");
  }
  // Independent oracle alters/reseals the containing row, slot, body and page.
  // No production encode/checksum routine constructs these malformed frames.
  const auto reseal=[](Bytes& bytes,std::size_t row,std::size_t cell,unsigned index){
    const auto extent=platform::LoadLittle32(bytes.data()+cell+4);
    Number(bytes,cell+8,8,BodyFnv(bytes.data()+cell+16,extent));
    Number(bytes,row+64,8,0);
    const auto checksum=BodyFnv(bytes.data()+row,platform::LoadLittle32(bytes.data()+row+56));
    Number(bytes,row+64,8,checksum);
    const auto directory=128+platform::LoadLittle32(bytes.data()+216);
    Number(bytes,directory+24*index+16,8,checksum);
    Number(bytes,160,8,0);Number(bytes,160,8,BodyFnv(bytes.data()+128,bytes.size()-160));LeafSeal(bytes);
  };
  for(unsigned profile=0;profile<5;++profile)for(unsigned index=0;index<2;++index){
    for(unsigned fault=0;fault<14;++fault){
      auto leaf=LeafExample(profile);auto& value=leaf.body.rows[index].cells[0].value;
      if(fault==0){value.is_null=true;value.payload.clear();}
      if(fault==1)value.is_null=true;
      if(fault==2)value.payload_is_toast_reference=true;
      if(fault==3)value.type_id=dt::CanonicalTypeId::character;
      if(fault==11){value.is_null=true;value.payload.clear();value.payload_is_toast_reference=true;}
      auto bytes=LeafOracle(leaf);
      const std::size_t row=index==0?224:224+platform::LoadLittle32(bytes.data()+280);
      const std::size_t cell=row+144,encoded=cell+16;
      if(fault==4)bytes[encoded]^=1;
      if(fault==5)Number(bytes,encoded+14,2,31);
      if(fault==6)Number(bytes,encoded+16,4,value.payload.size()+1);
      if(fault==7)bytes[encoded+12]|=4;
      if(fault==8)bytes[encoded+20]=1;
      if(fault==9)bytes[encoded+24]^=1;
      if(fault==10){Number(bytes,encoded+16,4,16777217);bytes[encoded+24]^=1;}
      if(fault==12)Number(bytes,cell+4,4,31);
      if(fault==13)Number(bytes,cell+4,4,0);
      reseal(bytes,row,cell,index);
      const std::string_view code=fault==0?"DATATYPE.NULL_NOT_ADMITTED":
          fault==1||fault==11?"DATATYPE.NULL_STATE.INVALID":
          fault==9?"CTB.BINARY.INTEGRITY_FAILED":"CTB.BINARY.FRAME_INVALID";
      const auto provider=dt::DecodeCanonicalBinaryValueViewNoAlloc(bytes.data()+encoded,
          platform::LoadLittle32(bytes.data()+cell+4),context);
      Check(!provider.ok()&&provider.diagnostic.diagnostic_code==code,"independent binary error class");
      Bytes body(bytes.begin()+128,bytes.end()-32);
      const auto parsed=page::ParseRowDataPageBodyWithCanonicalBinaryCells(body,leaf.header.page_number,context);
      Check(!parsed.ok()&&parsed.body.rows.empty()&&parsed.body.slots.empty()&&parsed.serialized.empty()&&
          parsed.binary_diagnostic&&SameBinaryDiagnostic(*parsed.binary_diagnostic,provider.diagnostic),
          "row forwards complete native datatype vector without valid prefix");
      const auto span_parsed=page::ParseRowDataPageRowsWithCanonicalBinaryCells(
          {bytes.data()+128,bytes.size()-160},leaf.header.page_number,context);
      Check(!span_parsed.ok()&&span_parsed.body.rows.empty()&&span_parsed.body.slots.empty()&&
          span_parsed.serialized.empty()&&span_parsed.binary_diagnostic&&
          SameBinaryDiagnostic(*span_parsed.binary_diagnostic,provider.diagnostic),
          "input span row reader preserves full binary refusal without partial output");
BorrowedRowWorkspace scratch(body.size());
      allocation_budget=0;
      const auto borrowed=page::ParseRowDataPageWithCanonicalBinaryCellsInto(
          body,leaf.header.page_number,scratch.get(),context);
      const auto unused_budget=allocation_budget;allocation_budget=-1;
      Check(unused_budget==0&&!borrowed.ok()&&borrowed.body.rows.empty()&&borrowed.body.slots.empty()&&
          borrowed.binary_diagnostic&&SameBinaryDiagnostic(*borrowed.binary_diagnostic,provider.diagnostic),
          "caller-backed row refuses malformed binary with full diagnostic and no heap allocation");
      std::vector<db::NativeCatalogLeafRecordView> metadata(scratch.rows.size());
      allocation_budget=0;
      const auto leaf_view=db::DecodeNativeCatalogLeafInto(bytes,scratch.get(),metadata);
      const auto remaining_leaf=allocation_budget;allocation_budget=-1;
      Check(remaining_leaf==0&&!leaf_view.ok()&&!leaf_view.page&&leaf_view.binary_diagnostic&&
          SameBinaryDiagnostic(*leaf_view.binary_diagnostic,provider.diagnostic),
          "caller-backed leaf retains complete native binary failure without C++ allocation");

      observed_allocations=0;count_allocations=true;
      auto result=db::DecodeNativeCatalogLeaf(bytes);
      count_allocations=false;const auto allocations=observed_allocations;
      LeafReject(result,db::NativeCatalogLeafError::invalid_body);
      Check(result.binary_diagnostic&&SameBinaryDiagnostic(*result.binary_diagnostic,provider.diagnostic),
          "leaf forwards exact ABI status message origin and all native arguments");
      if(fault==0||fault==1||fault==11){const auto& descriptor=result.binary_diagnostic->arguments[0];
        Check(descriptor.kind==K::descriptor_reference&&descriptor.uuid==context.descriptor_uuid&&
            descriptor.generation==1&&descriptor.text.empty(),"descriptor error has binary UUID and native generation");
      }
      if(fault==1)Check(result.binary_diagnostic->arguments[2].kind==K::unsigned_integer&&
          result.binary_diagnostic->arguments[2].unsigned_integer==value.payload.size(),"NULL length is native integer");
      // Fail every actual allocation before the producer, then deny any further
      // allocation exactly at the typed refusal. No bad_alloc may erase the
      // producer's diagnostic once it has been reached.
      if(profile==0)for(unsigned long budget=0;budget<=allocations;++budget){
        allocation_budget=budget;const auto injected=db::DecodeNativeCatalogLeaf(bytes);allocation_budget=-1;
        if(budget<allocations){LeafReject(injected,db::NativeCatalogLeafError::resource_exhausted);
          Check(!injected.binary_diagnostic,"pre-producer allocation failure invents no datatype refusal");
        }else Check(injected.binary_diagnostic&&SameBinaryDiagnostic(*injected.binary_diagnostic,provider.diagnostic),
            "complete binary refusal requires no trailing diagnostic allocation");
      }
      allocation_budget=0;const auto saved=*result.binary_diagnostic;allocation_budget=-1;
      std::fill(bytes.begin(),bytes.end(),0);body.clear();
      Check(SameBinaryDiagnostic(saved,provider.diagnostic),"fixed diagnostic survives source mutation and result copy");
      if(fault==0){const auto null_bytes=LeafOracle(leaf);Bytes null_body(null_bytes.begin()+128,null_bytes.end()-32);
        const auto generic=page::ParseRowDataPageBody(null_body,leaf.header.page_number);
        Check(generic.ok()&&generic.body.rows[index].cells[0].value.is_null,
            "general row reader still supports clean typed NULL");
      }
    }
    auto empty=LeafExample(profile);empty.body.rows[index].cells[0].value.payload.clear();
    const auto empty_bytes=LeafOracle(empty);Bytes empty_body(empty_bytes.begin()+128,empty_bytes.end()-32);
    const auto present=page::ParseRowDataPageBodyWithCanonicalBinaryCells(empty_body,empty.header.page_number,context);
    Check(present.ok()&&!present.binary_diagnostic&&!present.body.rows[index].cells[0].value.is_null&&
        present.body.rows[index].cells[0].value.payload.empty(),"empty PRESENT binary remains distinct from NULL");
    const auto metadata_refusal=db::DecodeNativeCatalogLeaf(empty_bytes);
    LeafReject(metadata_refusal,db::NativeCatalogLeafError::invalid_metadata);
    Check(!metadata_refusal.binary_diagnostic,"empty metadata is not falsely reported as malformed binary");
    auto text=LeafExample(profile);text.body.rows[index].cells[0].value.type_id=dt::CanonicalTypeId::character;
    text.body.rows[index].cells[0].value.payload={'4','2'};
    const auto text_bytes=LeafOracle(text);Bytes text_body(text_bytes.begin()+128,text_bytes.end()-32);
    const auto generic_text=page::ParseRowDataPageBody(text_body,text.header.page_number);
    Check(generic_text.ok()&&generic_text.body.rows[index].cells[0].value.payload==Bytes({'4','2'}),
          "canonical catalog policy does not remove general character row support");
  }
  // Exercise the owning provider's typed allocation failure in ordinary binary
  // row pages as well as catalog pages. It need not escape as bad_alloc: every
  // containing admission must retain resource classification, never corruption.
  for(unsigned profile=0;profile<5;++profile){
    const auto catalog=LeafExample(profile);const auto image=LeafOracle(catalog);
    Bytes input(image.begin()+128,image.end()-32);
    observed_allocations=0;count_allocations=true;
    const auto copied=page::ParseRowDataPageBodyWithCanonicalBinaryCells(input,catalog.header.page_number,context);
    count_allocations=false;const auto copy_count=observed_allocations;
    observed_allocations=0;count_allocations=true;
    const auto rows=page::ParseRowDataPageRowsWithCanonicalBinaryCells(input,catalog.header.page_number,context);
    count_allocations=false;const auto row_count=observed_allocations;
    Check(copied.ok()&&rows.ok()&&rows.serialized.empty()&&copy_count==row_count+1,
        "rows-only canonical admission removes exactly one full body allocation");
    Check(page::BuildRowDataPageBody(rows.body,catalog.header.page_size_bytes-32).serialized==input,
        "rows-only admission preserves all canonical fields and cell bytes");
BorrowedRowWorkspace scratch(input.size());
    allocation_budget=0;
    const auto borrowed=page::ParseRowDataPageWithCanonicalBinaryCellsInto(
        input,catalog.header.page_number,scratch.get(),context);
    const auto unused_budget=allocation_budget;allocation_budget=-1;
    Check(unused_budget==0&&borrowed.ok()&&SameBorrowedRowBody(borrowed.body,rows.body),
        "caller-backed row preserves every native field and payload without allocation");
    for(const auto& row:borrowed.body.rows)for(const auto& cell:row.cells)
      Check(cell.value.payload_data>=input.data()&&
          cell.value.payload_data+cell.value.payload_bytes<=input.data()+input.size(),
          "borrowed payload points into retained image, not an uncharged copy");
    for(unsigned shortage=0;shortage<5;++shortage){
      auto short_space=scratch.get();
      if(shortage==0)short_space.rows=short_space.rows.first(1);
      if(shortage==1)short_space.cells=short_space.cells.first(1);
      if(shortage==2)short_space.slots=short_space.slots.first(1);
      if(shortage==3)short_space.version_index={};
      if(shortage==4)short_space.sequence_index={};
      allocation_budget=0;
      const auto refused=page::ParseRowDataPageWithCanonicalBinaryCellsInto(
          input,catalog.header.page_number,short_space,context);
      const auto remaining=allocation_budget;allocation_budget=-1;
      Check(remaining==0&&!refused.ok()&&refused.error==page::RowDataPageViewError::insufficient_workspace&&
          refused.body.rows.empty()&&refused.body.slots.empty()&&!refused.binary_diagnostic,
          "every insufficient backing dimension refuses without prefix or invented datatype diagnostic");
    }
    auto aliased=scratch.get();aliased.sequence_index=aliased.version_index;
    const auto saved_input=input;
    allocation_budget=0;
    const auto overlap=page::ParseRowDataPageWithCanonicalBinaryCellsInto(
        input,catalog.header.page_number,aliased,context);
    allocation_budget=-1;
    Check(!overlap.ok()&&overlap.error==page::RowDataPageViewError::invalid_workspace&&
        overlap.status.code==platform::StatusCode::memory_invalid_request&&overlap.body.rows.empty()&&input==saved_input,
        "overlapping identity scratch rejected before input mutation");

    std::fill(input.begin(),input.end(),0);input.clear();input.shrink_to_fit();
    Check(page::BuildRowDataPageBody(rows.body,catalog.header.page_size_bytes-32).serialized==copied.serialized,
        "rows-only output owns final values independently of destroyed input");
    // Shared structural diagnostics retain native Status and every owning
    // diagnostic argument. Damage each complete body/row header and its slots.
    const auto same_error=[](const auto& a,const auto& b){
      if(a.status.code!=b.status.code||a.status.severity!=b.status.severity||
          a.status.subsystem!=b.status.subsystem||a.diagnostic.diagnostic_code!=b.diagnostic.diagnostic_code||
          a.diagnostic.message_key!=b.diagnostic.message_key||a.diagnostic.trace_id!=b.diagnostic.trace_id||
          a.diagnostic.source_component!=b.diagnostic.source_component||
          a.diagnostic.remediation_hint!=b.diagnostic.remediation_hint||
          a.diagnostic.arguments.size()!=b.diagnostic.arguments.size())return false;
      for(std::size_t i=0;i<a.diagnostic.arguments.size();++i)
        if(a.diagnostic.arguments[i].key!=b.diagnostic.arguments[i].key||
           a.diagnostic.arguments[i].value!=b.diagnostic.arguments[i].value)return false;
      return true;
    };
    for(unsigned offset=0;offset<240;++offset){
      auto bad=copied.serialized;bad[offset]^=0xff;
      if(offset<32||offset>=40){Number(bad,32,8,0);Number(bad,32,8,BodyFnv(bad.data(),bad.size()));}
      const auto a=page::ParseRowDataPageBodyWithCanonicalBinaryCells(bad,catalog.header.page_number,context);
      const auto b=page::ParseRowDataPageRowsWithCanonicalBinaryCells(bad,catalog.header.page_number,context);
      allocation_budget=0;
      const auto v=page::ParseRowDataPageWithCanonicalBinaryCellsInto(
          bad,catalog.header.page_number,scratch.get(),context);
      const auto remaining=allocation_budget;allocation_budget=-1;
      Check(remaining==0&&a.ok()==v.ok()&&a.status.code==v.status.code&&a.status.severity==v.status.severity&&
          a.status.subsystem==v.status.subsystem&&a.diagnostic.diagnostic_code==v.diagnostic.diagnostic_code&&
          a.diagnostic.message_key==v.diagnostic.message_key&&
          a.diagnostic.source_component==v.diagnostic.source_component,
          "same parser gives exact fixed row refusal under complete heap denial");
      if(a.ok())Check(SameBorrowedRowBody(v.body,a.body),"borrowed successful header mutation retains all fields");
      else {
        Check(v.body.rows.empty()&&v.body.slots.empty(),"borrowed malformed framing exposes no prefix");
        if(v.diagnostic.detail)Check(a.diagnostic.arguments.size()==1&&
            a.diagnostic.arguments[0].key=="detail"&&
            std::holds_alternative<std::string>(a.diagnostic.arguments[0].value)&&
            std::get<std::string>(a.diagnostic.arguments[0].value)==std::to_string(*v.diagnostic.detail),
            "fixed native numeric detail matches legacy boundary rendering");
      }

      Check(a.ok()==b.ok()&&same_error(a,b),"span and owning row framing retain exact diagnostics");
      if(!b.ok())Check(b.body.rows.empty()&&b.body.slots.empty()&&b.serialized.empty(),
          "malformed span row publishes no prefix");
    }
    for(unsigned variation=0;variation<9;++variation){
      auto shape=catalog;auto& first=shape.body.rows[0];auto& last=shape.body.rows[1];
      if(variation==0)last.version_uuid=first.version_uuid;
      if(variation==1)last.row_uuid=first.row_uuid;
      if(variation>=2&&variation<=5){
        last.row_version=2;last.previous_row_version=1;last.previous_version_uuid=first.version_uuid;
        if(variation!=2)last.row_uuid=first.row_uuid;
        if(variation==4)last.previous_version_uuid=Id(239);
        if(variation==5){first.next_row_version=3;first.next_version_uuid=Id(239);}
      }
      if(variation==6){last.version_uuid=Id(172);last.row_uuid.value=Id(162);}
      if(variation==7)for(auto& row:shape.body.rows)row.cells.clear();
      if(variation==8)shape.body.rows.clear();
      const auto wire=LeafOracle(shape);const std::span<const byte> source{wire.data()+128,wire.size()-160};
      const auto owning=page::ParseRowDataPageRowsWithCanonicalBinaryCells(source,shape.header.page_number,context);
      auto space=scratch.get();
      // Two slots deliberately fill both native identity indexes; collisions
      // must compare complete UUID/sequence identities, never treat hashes as IDs.
      if(variation==6){space.version_index=space.version_index.first(2);space.sequence_index=space.sequence_index.first(2);}
      allocation_budget=0;
      const auto fixed=page::ParseRowDataPageWithCanonicalBinaryCellsInto(source,shape.header.page_number,space,context);
      const auto remaining=allocation_budget;allocation_budget=-1;
      const bool expected=variation==3||variation>=5;
      Check(remaining==0&&owning.ok()==expected&&fixed.ok()==expected,
          "exact duplicate lineage collision empty-row and empty-page contracts");
      if(expected)Check(SameBorrowedRowBody(fixed.body,owning.body),"complete bounded row shape parity");
      else Check(fixed.body.rows.empty()&&fixed.body.slots.empty()&&
          owning.diagnostic.message_key==fixed.diagnostic.message_key,
          "bounded identity or lineage refusal preserves exact classification without prefix");
    }

    for(unsigned kind=0;kind<2;++kind){
      auto general=catalog;
      auto& cell=general.body.rows[0].cells[0].value;
      if(kind==0){cell.is_null=true;cell.payload.clear();}
      else {cell.type_id=dt::CanonicalTypeId::character;cell.payload={'4','2'};}
      const auto wire=LeafOracle(general);
      const auto decoded=page::ParseRowDataPageRows({wire.data()+128,wire.size()-160},general.header.page_number);
      Check(decoded.ok()&&decoded.serialized.empty()&&decoded.body.rows[0].cells[0].value.is_null==(kind==0)&&
          decoded.body.rows[0].cells[0].value.payload==cell.payload,"span general row preserves NULL and character values");
    }
    auto fixture=LeafExample(profile);fixture.header.page_type=0x0100;
    const page::NativeRowDataPage row{fixture.header,fixture.body};
    const auto expected=LeafOracle(fixture);
    for(unsigned mode=0;mode<2;++mode){
      observed_allocations=0;count_allocations=true;
      const auto baseline=mode?page::DecodeNativeRowDataPage(expected):page::EncodeNativeRowDataPage(row);
      count_allocations=false;const auto count=observed_allocations;
      Check(baseline.ok()&&baseline.bytes==expected,"independent ordinary binary row image");
      for(unsigned long budget=0;budget<=count;++budget){
        allocation_budget=budget;
        const auto result=mode?page::DecodeNativeRowDataPage(expected):page::EncodeNativeRowDataPage(row);
        allocation_budget=-1;
        if(budget<count)Check(result.error==page::NativeRowDataError::resource_exhausted&&
            !result.page&&result.bytes.empty(),"every binary row allocation failure remains a resource failure");
        else Check(result.ok()&&result.bytes==expected,"uninjected binary row terminal success");
      }
    }
  }
  // Streaming checksum matches the old logical-zero algorithm at every short
  // extent, including a partial checksum field, under complete heap denial.
  for(unsigned n=0;n<300;++n){Bytes input(n);for(unsigned i=0;i<n;++i)input[i]=(i*37+19)&255;
    auto normalized=input;if(normalized.size()>=40)std::fill(normalized.begin()+32,normalized.begin()+40,byte{0});
    const auto expected=BodyFnv(normalized.data(),normalized.size());
    allocation_budget=0;const auto actual=page::ComputeRowDataPageChecksum(input);allocation_budget=-1;
    Check(actual==expected,"streamed body checksum exact at every small boundary without allocation");
  }
}

void CatalogLeaves() {
  CatalogBinaryCells();
  using Error=db::NativeCatalogLeafError;
  for(unsigned p=0;p<5;++p) {
    auto leaf=LeafExample(p); const auto expected=LeafOracle(leaf);
    const auto e=db::EncodeNativeCatalogLeaf(leaf);
    Check(e.ok()&&e.bytes==expected&&e.metadata.size()==2,"all profiles exact independent catalog leaf bytes");
    const auto d=db::DecodeNativeCatalogLeaf(expected);
    Check(d.ok()&&LeafOracle(*d.page)==expected&&d.metadata.size()==2,"independent canonical leaf decode");
    BorrowedRowWorkspace scratch(expected.size()-160);
    std::vector<db::NativeCatalogLeafRecordView> metadata(scratch.rows.size());
    allocation_budget=0;const auto v=db::DecodeNativeCatalogLeafInto(expected,scratch.get(),metadata);
    const auto remaining=allocation_budget;allocation_budget=-1;
    Check(remaining==0&&v.ok()&&SameBorrowedRowBody(v.page->body,d.page->body)&&v.page->metadata.size()==2,
        "complete caller-backed leaf rows and metadata admitted without C++ allocation");
    for(const auto& record:v.page->metadata){const auto encoded=catalog::EncodeCatalogMetadataVersion(
        catalog::MaterializeCatalogMetadataVersion(record.metadata));
      Check(encoded.ok()&&encoded.bytes==d.page->body.rows[record.row_index].cells[0].value.payload,
          "every borrowed metadata field matches independently persisted cell");}
    for(unsigned fault=0;fault<3;++fault){auto backing=std::span(metadata);
      if(fault==0)backing=backing.first(1);
      if(fault==1)backing={reinterpret_cast<db::NativeCatalogLeafRecordView*>(
        const_cast<byte*>(expected.data())),1};
      if(fault==2)backing={reinterpret_cast<db::NativeCatalogLeafRecordView*>(scratch.rows.data()),1};
      allocation_budget=0;const auto invalid=db::DecodeNativeCatalogLeafInto(expected,scratch.get(),backing);
      const auto remaining=allocation_budget;allocation_budget=-1;
      Check(remaining==0&&!invalid.ok()&&!invalid.page&&
        invalid.error==(fault?Error::invalid_workspace:Error::resource_exhausted)&&
        expected==LeafOracle(leaf),"short or overlapping metadata backing refuses without heap or input modification");
    }
    for(const auto& row:leaf.body.rows) Check(d.metadata.at(row.version_uuid).record.header.row_uuid.value==row.row_uuid.value,
      "metadata keyed by actual native version UUID");
    leaf.body.rows.clear(); const auto empty=db::EncodeNativeCatalogLeaf(leaf);
    Check(empty.ok()&&empty.bytes==LeafOracle(leaf)&&empty.metadata.empty(),"empty allocated leaf, not absent table success");
  }
  const auto leaf=LeafExample(); const auto good=LeafOracle(leaf);
  BorrowedRowWorkspace leaf_scratch(good.size()-160);
  std::vector<db::NativeCatalogLeafRecordView> leaf_metadata(leaf_scratch.rows.size());
  for(platform::u64 generation:{platform::u64{0},leaf.body.page_generation+1,
      std::numeric_limits<platform::u64>::max()}) {
    auto bad=leaf;bad.body.rows.back().storage_generation=generation;
    LeafReject(db::EncodeNativeCatalogLeaf(bad),Error::invalid_body);
    // Independent oracle refreshes all row, body and page integrity fields.
    LeafReject(db::DecodeNativeCatalogLeaf(LeafOracle(bad)),Error::invalid_body);
  }
  for(std::size_t at=0;at<good.size();++at) {
    auto b=good; b[at]^=1; const auto r=db::DecodeNativeCatalogLeaf(b);
    Check(!r.ok()&&!r.page&&r.metadata.empty()&&r.bytes.empty(),"every leaf byte corruption fails without prefix");
    allocation_budget=0;const auto v=db::DecodeNativeCatalogLeafInto(b,leaf_scratch.get(),leaf_metadata);
    const auto remaining=allocation_budget;allocation_budget=-1;
    Check(remaining==0&&!v.ok()&&!v.page&&v.error==r.error,"every borrowed leaf byte corruption preserves exact failure without heap");
  }
  for(std::size_t size:{0u,127u,8191u,8193u}) {auto b=good;b.resize(size);LeafReject(db::DecodeNativeCatalogLeaf(b),Error::invalid_header);}
  for(unsigned mode=0;mode<9;++mode) {
    auto bad=leaf;
    if(mode==0) bad.header.page_type=5;
    if(mode==1) bad.body.page_generation=8;
    if(mode==2) bad.body.next_page_number=99;
    if(mode==3) bad.body.compaction_generation=0;
    if(mode==4) bad.body.rows[1].stable_slot_id=0;
    if(mode==5) bad.body.rows[1].transaction_uuid.value=Id(99);
    if(mode==6) ++bad.body.rows[1].local_transaction_id;
    if(mode==7) bad.body.rows[1].cells[0].column_ordinal=2;
    if(mode==8) bad.body.rows[1].deleted=true;
    const auto expected=mode==0?Error::invalid_header:mode<5?Error::invalid_body:Error::invalid_metadata;
    LeafReject(db::EncodeNativeCatalogLeaf(bad),expected);
    LeafReject(db::DecodeNativeCatalogLeaf(LeafOracle(bad)),expected);
    const auto bytes=LeafOracle(bad);allocation_budget=0;
    const auto borrowed=db::DecodeNativeCatalogLeafInto(bytes,leaf_scratch.get(),leaf_metadata);
    const auto remaining=allocation_budget;allocation_budget=-1;
    Check(remaining==0&&!borrowed.ok()&&!borrowed.page&&borrowed.error==expected,
        "borrowed leaf retains every metadata binding refusal without C++ heap");
  }
  {auto bad=leaf;++bad.body.page_number;LeafReject(db::EncodeNativeCatalogLeaf(bad),Error::invalid_body);}
  {auto b=good; b[b.size()-33]=1; Number(b,160,8,0); Number(b,160,8,BodyFnv(b.data()+128,b.size()-160)); LeafSeal(b);
    LeafReject(db::DecodeNativeCatalogLeaf(b),Error::invalid_body);
    allocation_budget=0;const auto borrowed=db::DecodeNativeCatalogLeafInto(b,leaf_scratch.get(),leaf_metadata);
    const auto remaining=allocation_budget;allocation_budget=-1;
    Check(remaining==0&&!borrowed.ok()&&!borrowed.page&&borrowed.error==Error::invalid_body,
      "recomputed checksums do not admit noncanonical unused tail through borrowed leaf");}
  for(unsigned i=0;i<2;++i) {
    allocation_budget=0; const auto r=i?db::DecodeNativeCatalogLeaf(good):db::EncodeNativeCatalogLeaf(leaf);
    allocation_budget=-1; LeafReject(r,Error::resource_exhausted);
  }
  for(unsigned fault=1;fault<=5;++fault) {
    hash_fault=fault; const auto r=db::DecodeNativeCatalogLeaf(good);
    LeafReject(r,Error::hash_failure); Check(!hash_fault,"leaf digest provider fault consumed");
    hash_fault=fault;allocation_budget=0;
    const auto borrowed=db::DecodeNativeCatalogLeafInto(good,leaf_scratch.get(),leaf_metadata);
    const auto remaining=allocation_budget;allocation_budget=-1;
    Check(remaining==0&&!hash_fault&&!borrowed.ok()&&!borrowed.page&&borrowed.error==Error::hash_failure,
        "borrowed leaf hash-provider refusal needs no diagnostic allocation");
  }
  for(unsigned mode=0;mode<2;++mode) {
    observed_segmented_contexts=0;count_segmented_contexts=true;
    auto r=mode?db::DecodeNativeCatalogLeaf(good):db::EncodeNativeCatalogLeaf(leaf);
    count_segmented_contexts=false;const auto contexts=observed_segmented_contexts;
    Check(r.ok()&&contexts>1,"measure actual leaf and nested metadata hash contexts");
    for(unsigned context=0;context<contexts;++context)for(unsigned fault=1;fault<=5;++fault) {
      segmented_context_skip=context;hash_fault=fault;
      r=mode?db::DecodeNativeCatalogLeaf(good):db::EncodeNativeCatalogLeaf(leaf);
      const bool reached=segmented_context_skip==0;segmented_context_skip=0;
      LeafReject(r,Error::hash_failure);
      Check(reached&&!hash_fault,"every actual nested metadata hash stage failure propagated");
    }
    observed_allocations=0;count_allocations=true;
    r=mode?db::DecodeNativeCatalogLeaf(good):db::EncodeNativeCatalogLeaf(leaf);
    count_allocations=false;const auto count=observed_allocations;Check(r.ok(),"leaf allocation baseline");
    bool succeeded=false;
    for(unsigned long position=0;position<=count;++position) {
      allocation_budget=position;r=mode?db::DecodeNativeCatalogLeaf(good):db::EncodeNativeCatalogLeaf(leaf);allocation_budget=-1;
      if(r.ok()) {succeeded=true;break;} LeafReject(r,Error::resource_exhausted);
    }
    Check(succeeded,"every leaf allocation position through complete page");
  }
}
void BorrowedLeafHashFaults() {
  const auto bytes=LeafOracle(LeafExample());BorrowedRowWorkspace scratch(bytes.size()-160);
  std::vector<db::NativeCatalogLeafRecordView> records(scratch.rows.size());
  observed_segmented_contexts=0;count_segmented_contexts=true;
  const auto baseline=db::DecodeNativeCatalogLeafInto(bytes,scratch.get(),records);
  count_segmented_contexts=false;const auto contexts=observed_segmented_contexts;
  Check(baseline.ok()&&contexts>1,"measure borrowed leaf and nested metadata provider contexts");
  for(unsigned context=0;context<contexts;++context)for(unsigned fault=1;fault<=5;++fault){
    segmented_context_skip=context;hash_fault=fault;allocation_budget=0;
    const auto failed=db::DecodeNativeCatalogLeafInto(bytes,scratch.get(),records);
    const auto remaining=allocation_budget;allocation_budget=-1;
    const bool reached=segmented_context_skip==0;segmented_context_skip=0;
    Check(remaining==0&&reached&&!hash_fault&&!failed.ok()&&!failed.page&&
      failed.error==db::NativeCatalogLeafError::hash_failure,"every borrowed nested hash failure is exact and heap-free");
  }
}
void CatalogLeafWorkspaceAliasing() {
  for(unsigned profile=0;profile<5;++profile){
    auto bytes=LeafOracle(LeafExample(profile));const auto original=bytes;
    BorrowedRowWorkspace scratch(bytes.size()-160);
    std::vector<db::NativeCatalogLeafRecordView> records(scratch.rows.size());
    for(unsigned region=0;region<8;++region){auto workspace=scratch.get();
      auto* target=bytes.data()+(region<5?0:bytes.size()-32);
      switch(region){
        case 0:workspace.rows={reinterpret_cast<page::RowDataRecordView*>(target),1};break;
        case 1:workspace.cells={reinterpret_cast<page::RowDataCellView*>(target),2};break;
        case 2:case 5:workspace.slots={reinterpret_cast<decltype(workspace.slots)::element_type*>(target),1};break;
        case 3:case 6:workspace.version_index={reinterpret_cast<scratchbird::core::platform::u32*>(target),4};break;
        case 4:case 7:workspace.sequence_index={reinterpret_cast<scratchbird::core::platform::u32*>(target),4};break;
      }
      allocation_budget=0;const auto result=db::DecodeNativeCatalogLeafInto(bytes,workspace,records);
      const auto remaining=allocation_budget;allocation_budget=-1;
      Check(!result.ok()&&!result.page&&result.error==db::NativeCatalogLeafError::invalid_workspace&&
        remaining==0&&bytes==original,"every scratch region excludes complete header and checksum image before writes");
    }
  }
}
void BufferedReadTelemetry() {
  namespace m=scratchbird::core::metrics;
  auto& registry=m::DefaultMetricRegistry();
  Check(registry.Descriptors().empty(),"storage fixture must not manufacture metric activation");
  auto made=m::MetricObservationQueue::Create({Id(1),Id(3),{}},{128,4*1024*1024});
  Check(made.ok(),"allocate actual local metric queue");
  std::shared_ptr<m::MetricObservationQueue> queue=std::move(made.queue);
  Check(registry.BindObservationQueue(queue).ok,"retain actual local metric queue");
  std::array<m::MetricDescriptor,2> definitions;
  std::array<m::MetricSeriesIdentity,3> series;
  const std::array<m::MetricLabelSet,3> labels{{
    {{"component","storage.disk"},{"operation","read_at"},{"result","ok"},{"device_class","file"}},
    {{"component","storage.filespace"},{"database_uuid",Id(1)},{"filespace_uuid",Id(2)},
      {"node_uuid",Id(3)},{"filespace_role","primary_system"},{"device_class","first_device_class_long_label"}},
    {{"component","storage.filespace"},{"database_uuid",Id(1)},{"filespace_uuid",Id(7)},
      {"node_uuid",Id(3)},{"filespace_role","secondary_data"},{"device_class","second_device_class_long_label"}}
  }};
  for(unsigned i=0;i<2;++i){auto& d=definitions[i];
    d.family=i?"sb_filespace_device_read_latency_microseconds":"sb_storage_device_read_latency_microseconds";
    d.namespace_path="sys.metrics.storage";d.producer_owner="storage_disk";
    d.type=m::MetricType::histogram;d.unit=m::MetricUnit::microseconds;
    d.value_type=m::MetricScalarType::float64;d.histogram_buckets={1.,1000.,1000000.};
    d.readiness=m::MetricReadiness::implemented;d.metric_uuid=Id(220+i);d.descriptor_generation=1;
    d.label_schema_uuid=Id(230+i);d.label_schema_generation=1;
    d.retention_policy_uuid=Id(235);d.retention_policy_generation=1;
    d.visibility_policy_uuid=Id(236);d.visibility_policy_generation=1;
    for(const auto& label:labels[i])d.labels.push_back({label.key,true,false,
      std::holds_alternative<Uuid>(label.value)?m::MetricLabelType::system_uuid:m::MetricLabelType::text});
    Check(registry.RegisterDescriptor(d).ok,"register explicitly bound histogram fixture");
  }
  m::MetricRetentionPolicy policy;policy.policy_uuid=Id(235);policy.generation=1;policy.policy_name="component capture policy";
  for(unsigned i=0;i<3;++i){const auto& d=definitions[i?1:0];m::MetricHistoryBinding binding;
    static_cast<m::MetricDescriptorBinding&>(binding)=d;binding.database_uuid=Id(1);binding.node_uuid=Id(3);
    auto value=m::MakeMetricSeriesIdentity(d,labels[i],policy,binding,Id(240+i),1);
    Check(value.ok()&&registry.RegisterSeries(*value.record,policy).ok,"register exact raw16 sample series");series[i]=std::move(*value.record);
  }
  Fixture fixture;disk::FileDevice device;const auto path=(fixture.root/"buffered-telemetry").string();const byte expected=42;
  Check(device.Open(path,disk::FileOpenMode::create_new).ok()&&device.WriteAt(0,&expected,1).ok(),"real telemetry read fixture");
  device.SetMetricContext(Id(1),Id(2),Id(3),"primary_system","first_device_class_long_label");
  const auto before_rejected=device.rejected_io_latency_observations();
  {
    disk::FileDevice::ReadLatencyBatch batch(device);auto fence=device.AcquireOperationGuard();byte actual=0;
    Check(batch.ReadAt(0,&actual,1).ok()&&actual==expected,"first retained context physical read");
    device.SetMetricContext(Id(1),Id(7),Id(3),"secondary_data","second_device_class_long_label");
    Check(batch.ReadAt(0,&actual,1).ok()&&actual==expected,"replacement context physical read");
    Check(queue->Stats().admitted==0&&registry.SnapshotCurrent().empty(),"capture is not registry admission");
    Check(device.Close().ok(),"close before buffered observation publication");
  }
  Check(device.rejected_io_latency_observations()==before_rejected&&queue->Stats().admitted==4,
    "captured observations retain both scopes after context replacement and device close");
  const auto drain=[&](std::array<unsigned,3> expected_samples){std::array<unsigned,3> samples{};
    while(queue->Stats().queued){auto held=queue->TryAcquire();Check(held.ok(),"acquire actual encoded histogram sample");
      const auto found=std::find_if(series.begin(),series.end(),[&](const auto& value){return value.series_uuid==held.lease.observation->series_uuid;});
      Check(found!=series.end(),"publication has an exact prebound series identity");
      const unsigned ordinal=static_cast<unsigned>(found-series.begin());
      const auto decoded=m::DecodeMetricRawSample(definitions[ordinal?1:0],*found,held.lease.observation->bytes);
      Check(decoded.ok()&&decoded.record->database_uuid==Id(1)&&decoded.record->node_uuid==Id(3)&&
        decoded.record->series_uuid==found->series_uuid&&decoded.record->value.count>0,
        "real queue bytes retain native metric scope and histogram observation");
      Check(queue->TryRemove(held.lease)==m::MetricQueueError::none,"retire component observation without durability claim");++samples[ordinal];
    }Check(samples==expected_samples,"exact per-series buffered telemetry sample counts");};
  drain({2,1,1});
  Check(device.Open(path,disk::FileOpenMode::open_existing).ok(),"reopen physical telemetry source");
  const auto admitted=queue->Stats().admitted;
  {disk::FileDevice::ReadLatencyBatch batch(device);auto fence=device.AcquireOperationGuard();byte actual=0;
    for(unsigned i=0;i<17;++i){allocation_budget=0;const auto io=batch.ReadAt(0,&actual,1);
      const auto remaining=allocation_budget;allocation_budget=-1;
      Check(remaining==0&&io.ok()&&actual==expected,"bounded capture and overflow perform no heap allocation");}
    Check(queue->Stats().admitted==admitted&&device.rejected_io_latency_observations()==before_rejected+1,
      "overflow is counted without fabricating publication or losing physical read success");}
  drain({16,0,16});
  const auto failed=device.failed_io_latency_observations();
  {disk::FileDevice::ReadLatencyBatch batch(device);{auto fence=device.AcquireOperationGuard();byte actual=0;
      Check(batch.ReadAt(0,&actual,1).ok()&&actual==expected,"actual bytes precede failed telemetry publication");}
    allocation_budget=0;}
  const auto consumed=allocation_budget;allocation_budget=-1;
  Check(consumed==-1&&device.failed_io_latency_observations()==failed+1&&!queue->Stats().queued,
    "publication allocation loss is explicit and does not invent a sample");
  Check(device.Close().ok(),"close telemetry fixture");
}

void CatalogLeafFiles() {
  BorrowedLeafHashFaults();
  CatalogLeafWorkspaceAliasing();
  using Error=db::NativeCatalogLeafError; Fixture fixture;
  for(unsigned p=0;p<5;++p) {
    auto leaf=LeafExample(p); const auto image=LeafOracle(leaf); auto z=Example(p,p==0?5:1);
    const auto ref=RootExample(p).roots[0]; const auto pagezero=Oracle(z);
    const auto path=(fixture.root/("leaf-"+std::to_string(p))).string(); disk::FileDevice device;
    Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"own canonical leaf file"); const byte zero=0;
    Check(device.WriteAt(0,pagezero.data(),pagezero.size()).ok()&&device.WriteAt(z.total_pages*sizes[p]-1,&zero,1).ok()
      &&device.WriteAt(21*sizes[p],image.data(),image.size()).ok()&&device.Sync().ok(),"actual canonical leaf persisted");
    auto r=db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),ref);
    Check(r.ok()&&r.bytes==image&&r.metadata.size()==2,"read actual bound leaf in primary/secondary filespace");
    {
      const auto amount=db::NativeCatalogLeafWorkspaceBytes(Profile(p));LeafMemoryFixture grant(amount);
      using ME=db::NativeCatalogLeafMemoryError;
      const auto read=[&]{return db::ReadNativeCatalogLeafWithMemoryFromOpenDevice(device,Id(1),ref,grant.memory,grant.binding);};
      const auto no_page=[](const auto& value,std::source_location at=std::source_location::current()){
        Check(!value.ok()&&!value.arena&&value.image.empty()&&!value.leaf.page,
          "grant-backed leaf refusal retains no usable image or metadata prefix error="+
          std::to_string(static_cast<unsigned>(value.error)),at);};
      auto guard=device.AcquireOperationGuard();auto* mutex=guard.mutex();guard.unlock();
      allocation_lock_probe={{mutex,mutex},true,false};
      reads=stage_writes=stage_syncs=0;track_reads=true;auto owned=read();track_reads=false;
      Check(owned.ok()&&SameBorrowedRowBody(owned.leaf.page->body,r.page->body)&&
          std::equal(owned.image.begin(),owned.image.end(),image.begin(),image.end())&&
          grant.manager.Snapshot().current_bytes==amount&&grant.memory.Snapshot().allocated_bytes==amount&&
          allocation_lock_probe.available&&!stage_writes&&!stage_syncs,
          "actual leaf image and every decoded allocation charged before device fence without durable effects");
      const auto read_count=reads;Check(read_count==3,"actual bootstrap pagezero and leaf reads");
      owned={};Check(!grant.manager.Snapshot().current_bytes,"retiring leaf returns physical backing");
      FencedHeapProbe heap_probe;heap_probe.mutex=mutex;
      std::thread heap_observer([&]{heap_probe.Run();});
      // Negative control: this observer really refuses allocations while the
      // other thread's device fence is held, even if telemetry swallows failure.
      const auto prior_loss=device.failed_io_latency_observations();
      bool denied=false,legacy_ok=false;byte probe_byte=0;
      {auto held=device.AcquireOperationGuard();fenced_heap_probe=&heap_probe;allocation_budget=0;
        const auto legacy=device.ReadAt(0,&probe_byte,1);denied=allocation_budget==-1;legacy_ok=legacy.ok();
        allocation_budget=-1;fenced_heap_probe=nullptr;}
      const bool negative_control=denied&&legacy_ok&&device.failed_io_latency_observations()==prior_loss+1;
      fenced_heap_probe=&heap_probe;deny_heap_on_next_read=true;owned=read();
      const auto fenced_budget=allocation_budget;allocation_budget=-1;fenced_heap_probe=nullptr;
      heap_probe.stop=true;heap_observer.join();
      Check(negative_control,"heap-fence negative control catches legacy telemetry allocation under actual lock");
      Check(!deny_heap_on_next_read&&fenced_budget==0&&heap_probe.outside_allocations>0&&owned.ok()&&
          SameBorrowedRowBody(owned.leaf.page->body,r.page->body)&&grant.manager.Snapshot().current_bytes==amount,
          "no C++ heap allocation under device fence; complete borrowed result and synchronous post-unlock telemetry");owned={};
      tree_read_paused=false;resume_tree_read=false;pause_next_tree_read=true;
      std::atomic<bool> done=false;db::NativeCatalogLeafMemoryResult concurrent;
      std::thread reader([&]{concurrent=read();done=true;});
      while(!tree_read_paused.load()&&!done.load())std::this_thread::yield();
      bool held=tree_read_paused.load();if(mutex->try_lock()){held=false;mutex->unlock();}
      resume_tree_read=true;reader.join();pause_next_tree_read=false;
      Check(held&&concurrent.ok(),"actual retained device fence covers grant-backed leaf I/O");concurrent={};
      for(unsigned fault=1;fault<=read_count;++fault){reads=0;read_fault=fault;track_reads=true;
        deallocation_lock_probe={{mutex,mutex},true,false};auto refused=read();track_reads=false;
        no_page(refused);Check(!read_fault&&refused.error==ME::io_failure&&deallocation_lock_probe.available&&
            !grant.manager.Snapshot().current_bytes,"every real read failure releases charged backing after device unlock");}
      for(unsigned field=0;field<4;++field){auto wrong=grant.binding;
        std::array<platform::Uuid*,4> ids{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};*ids[field]=Id(229);
        reads=0;track_reads=true;auto refused=db::ReadNativeCatalogLeafWithMemoryFromOpenDevice(device,Id(1),ref,grant.memory,wrong);track_reads=false;
        no_page(refused);Check(refused.error==ME::memory_binding_failure&&!reads&&!grant.manager.Snapshot().current_bytes,
            "each binary grant ownership dimension checked before effects");}
      for(unsigned role:{0u,7u}){auto wrong=ref;wrong.role=role;reads=0;track_reads=true;
        const auto invalid=db::ReadNativeCatalogLeafWithMemoryFromOpenDevice(device,Id(1),wrong,grant.memory,grant.binding);
        track_reads=false;no_page(invalid);Check(invalid.error==ME::invalid_request&&!reads&&
          !grant.manager.Snapshot().current_bytes,"catalog root role refused before admission and I/O");}
      observed_allocations=0;count_allocations=true;owned=read();count_allocations=false;
      const auto allocations=observed_allocations;Check(owned.ok(),"measure actual grant-backed leaf allocations");owned={};
      for(unsigned long n=0;n<=allocations;++n){
        // Prior failed allocations can enqueue best-effort telemetry. Start
        // every fault at the same fresh warmed allocator state, so n targets
        // the measured operation rather than a previous failure's reporting.
        LeafMemoryFixture isolated(amount);
        const auto attempt=[&]{return db::ReadNativeCatalogLeafWithMemoryFromOpenDevice(
            device,Id(1),ref,isolated.memory,isolated.binding);};
        auto primed=attempt();Check(primed.ok(),"warm isolated actual grant before fault");primed={};
        observed_allocations=0;count_allocations=true;primed=attempt();count_allocations=false;
        const auto exact_count=observed_allocations;Check(primed.ok()&&exact_count==allocations,
            "fresh isolated allocation sequence matches measured leaf operation");primed={};
        const auto before_telemetry=isolated.manager.Snapshot().telemetry_truncation_count;
        const auto before_io_telemetry=device.failed_io_latency_observations();
        allocation_budget=n;auto injected=attempt();const auto remaining=allocation_budget;allocation_budget=-1;
        Check(n==exact_count?remaining==0:remaining==-1,"every measured allocation fault is actually reached");
        if(!injected.ok()){no_page(injected);Check(n<exact_count&&!isolated.manager.Snapshot().current_bytes,
            "failed allocation releases actual leaf bytes");}
        else {
          Check(SameBorrowedRowBody(injected.leaf.page->body,r.page->body)&&
              std::equal(injected.image.begin(),injected.image.end(),image.begin(),image.end())&&
              isolated.manager.Snapshot().current_bytes==amount&&isolated.memory.Snapshot().allocated_bytes==amount,
              "successful allocation or optional telemetry failure retains complete correct charged leaf");
          if(n<exact_count)Check(isolated.manager.Snapshot().telemetry_truncation_count>before_telemetry||
              device.failed_io_latency_observations()>before_io_telemetry,
              "only explicitly recorded optional telemetry failure may preserve complete success");
        }
        injected={};isolated.memory={};isolated.Empty();
      }
      owned=read();Check(owned.ok(),"retain actual leaf before revocation");
      const auto revoked=grant.ledger.CleanupOwner(grant.binding.owner_uuid.bytes);
      Check(revoked.retained_bytes==amount,"revocation retains live leaf's full charge");
      no_page(read());grant.memory={};Check(grant.manager.Snapshot().current_bytes==amount,
          "leaf owner outlives original grant handle");
      Check(device.Close().ok(),"close physical file while immutable leaf remains owned");
      Check(SameBorrowedRowBody(owned.leaf.page->body,r.page->body),"retained leaf remains valid after device close");
      allocation_budget=0;owned={};const auto remaining=allocation_budget;allocation_budget=-1;
      Check(remaining==0,"final leaf backing cleanup needs no allocation");grant.Empty();
      Check(device.Open(path,disk::FileOpenMode::open_existing).ok(),"reopen leaf for existing storage regressions");
      LeafMemoryFixture short_grant(amount-1);reads=0;track_reads=true;
      const auto short_read=db::ReadNativeCatalogLeafWithMemoryFromOpenDevice(device,Id(1),ref,short_grant.memory,short_grant.binding);track_reads=false;
      no_page(short_read);Check(short_read.error==ME::memory_allocation_failure&&!reads&&!short_grant.manager.Snapshot().current_bytes,
          "one-byte-short actual grant refuses before device access");short_grant.memory={};short_grant.Empty();
    }
    {
      auto invalid=LeafExample(p);invalid.body.rows.back().cells[0].value.is_null=true;
      invalid.body.rows.back().cells[0].value.payload.clear();const auto damaged=LeafOracle(invalid);
      const auto expected=db::DecodeNativeCatalogLeaf(damaged);
      Check(device.WriteAt(21*sizes[p],damaged.data(),damaged.size()).ok()&&device.Sync().ok(),
          "persist resealed invalid later binary cell");
      const auto refused=db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),ref);
      LeafReject(refused,db::NativeCatalogLeafError::invalid_body);
      Check(expected.binary_diagnostic&&refused.binary_diagnostic&&
          SameBinaryDiagnostic(*expected.binary_diagnostic,*refused.binary_diagnostic),
          "retained device read preserves full typed binary refusal without valid prefix");
      Check(device.WriteAt(21*sizes[p],image.data(),image.size()).ok()&&device.Sync().ok(),
          "restore valid independent image for existing device regressions");
    }
    if(p!=0) {
      const auto root_image=RootOracle(RootExample(p));
      Check(device.WriteAt(12*sizes[p],root_image.data(),root_image.size()).ok()&&device.Sync().ok(),"persist owning canonical catalog root");
      const auto actual_root=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),z.roots[1]);
      Check(actual_root.ok(),"resolve actual root before leaf traversal");
      r=db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),actual_root.root->roots[0]);
      Check(r.ok()&&r.bytes==image&&r.metadata.size()==2,"actual pagezero to root to leaf to bound metadata");
    }
    for(unsigned field=0;field<4;++field) {
      auto bad=ref;
      if(field==0) ++bad.page.page_generation;
      if(field==1) bad.object_uuid=Id(99);
      if(field==2) bad.page.page_number=22;
      if(field==3) bad.page.filespace_uuid=Id(99);
      r=db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),bad);
      Check(!r.ok()&&!r.page&&r.metadata.empty()&&r.bytes.empty(),"wrong actual leaf binding no partial metadata");
    }
    for(unsigned fault=1;fault<=3;++fault) {
      reads=0;read_fault=fault;track_reads=true;r=db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),ref);track_reads=false;
      LeafReject(r,Error::io_failure);Check(!read_fault,"actual leaf pread fault consumed");
    }
    auto encrypted=leaf;encrypted.header.flags=1;const auto encrypted_image=LeafOracle(encrypted);
    Check(device.WriteAt(21*sizes[p],encrypted_image.data(),encrypted_image.size()).ok(),"encrypted leaf fixture");
    LeafReject(db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),ref),Error::encrypted_requires_crypto_authority);
    Check(device.WriteAt(21*sizes[p],image.data(),image.size()).ok()&&device.Sync().ok(),"restore own leaf fixture");
    Exclusive(path); Check(device.Close().ok()&&device.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"readonly leaf reopen");
    r=db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),ref);
    Check(r.ok()&&r.bytes==image&&device.read_only(),"actual reopened leaf metadata");
    Bytes actual(image.size());Check(device.ReadAt(21*sizes[p],actual.data(),actual.size()).ok()&&actual==image,"leaf reads/faults never mutate image");
    Check(device.Close().ok(),"close leaf file");LeafReject(db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),ref),Error::invalid_filespace);
    const auto pid=::fork();Check(pid>=0,"fork leaf reopen verifier");
    if(pid==0) {const auto profile=std::to_string(p);::execl("/proc/self/exe","pagezero_test","--leaf-probe",path.c_str(),profile.c_str(),nullptr);::_exit(125);}
    int status=0;Check(::waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh process validates exact leaf bytes and metadata");
  }
  BufferedReadTelemetry();
}
}  // namespace
namespace mga=scratchbird::transaction::mga;
page::NativeTransactionInventoryPage InventoryExample(unsigned profile=0) {
  page::NativeTransactionInventoryPage p;
  p.header={sizes[profile],0x0301,Id(1),Id(2),Id(92),14,104,0,Profile(profile)};
  p.object_uuid=Id(44); p.inventory_generation=19;
  p.inventory.next_local_transaction_id=3; p.inventory.next_commit_sequence=1;
  mga::TransactionInventoryEntry e; e.identity.local_id=mga::MakeLocalTransactionId(1);
  e.identity.transaction_uuid={scratchbird::core::platform::UuidKind::transaction,Id(93)};
  e.identity.scope=mga::TransactionScope::local_node; e.state=mga::TransactionState::active;
  e.begin_unix_epoch_millis=1790000000123ull;
  p.inventory.entries.push_back(e); return p;
}
disk::NativePageReference InventoryRef(const page::NativeTransactionInventoryPage& p) {
  return {p.header.filespace_uuid,p.header.page_number,p.header.page_generation,p.header.page_size_profile_uuid};
}
void InventorySeal(Bytes& b) {
  std::fill(b.begin()+320,b.begin()+352,0); std::array<byte,32> digest{};
  Check(SHA256(b.data(),b.size(),digest.data())!=nullptr,"independent inventory SHA256");
  std::copy(digest.begin(),digest.end(),b.begin()+320);
}
// Independent framing/number packing. The fixture deliberately controls the
// expected summaries; this oracle never calls production inventory/horizon code.
Bytes InventoryOracle(const page::NativeTransactionInventoryPage& p,u64 oit,u64 oat,u64 ost) {
  auto common=RootExample(); common.header=p.header;
  auto b=RootOracle(common); std::fill(b.begin()+128,b.end(),0);
  const std::string_view magic="SBTINV01"; std::copy(magic.begin(),magic.end(),b.begin()+128);
  Number(b,136,2,1); Number(b,138,2,256); Number(b,140,4,384+72*p.inventory.entries.size());
  PutUuid(b,144,p.object_uuid); Number(b,160,8,p.inventory_generation);
  Number(b,168,8,p.inventory.next_local_transaction_id); Number(b,176,8,p.inventory.next_commit_sequence);
  Number(b,184,4,p.inventory.entries.size());
  const auto ref=[&](std::size_t at,const disk::NativePageReference& r) {
    PutUuid(b,at,r.filespace_uuid); Number(b,at+16,8,r.page_number);
    Number(b,at+24,8,r.page_generation); PutUuid(b,at+32,r.page_size_profile_uuid);
  };
  if(p.previous) ref(192,*p.previous); if(p.next) ref(240,*p.next);
  Number(b,288,8,oit); Number(b,296,8,oat); Number(b,304,8,ost);
  for(std::size_t i=0;i<p.inventory.entries.size();++i) {
    const auto& e=p.inventory.entries[i]; const auto at=384+72*i;
    Number(b,at,8,e.identity.local_id.value); PutUuid(b,at+8,e.identity.transaction_uuid.value);
    Number(b,at+24,2,static_cast<unsigned>(e.identity.scope)); Number(b,at+26,2,static_cast<unsigned>(e.state));
    const unsigned origin=e.archived_from_state==mga::TransactionState::committed?1:
      e.archived_from_state==mga::TransactionState::rolled_back?2:e.archived_from_state==mga::TransactionState::failed_terminal?3:0;
    Number(b,at+28,4,(e.evidence_record_required?1:0)|(e.evidence_record_written?2:0)|(e.rollback_only?4:0)|(origin<<3));
    Number(b,at+32,8,e.begin_unix_epoch_millis); Number(b,at+40,8,e.final_unix_epoch_millis);
    Number(b,at+48,8,e.begin_visible_through_local_transaction_id);
    Number(b,at+56,8,e.begin_visible_through_commit_sequence); Number(b,at+64,8,e.commit_sequence);
  }
  InventorySeal(b); return b;
}
// Spec-table oracle for fixtures that vary lifecycle/archived outcomes. Kept
// separate from the explicit-summary encoder used for malformed-summary tests.
Bytes InventoryStateOracle(const page::NativeTransactionInventoryPage& p) {
  auto oit=p.inventory.next_local_transaction_id,oat=oit;
  for(const auto& e:p.inventory.entries){
    const auto state=e.state==mga::TransactionState::archived?e.archived_from_state:e.state;
    const bool active=e.state==mga::TransactionState::active||e.state==mga::TransactionState::read_only_active;
    if(active)oat=std::min(oat,e.identity.local_id.value);
    for(auto hold:{mga::TransactionState::created,mga::TransactionState::active,mga::TransactionState::read_only_active,
        mga::TransactionState::preparing,mga::TransactionState::prepared,mga::TransactionState::committing,
        mga::TransactionState::rolling_back,mga::TransactionState::limbo,mga::TransactionState::recovering,mga::TransactionState::failed_terminal})
      if(state==hold)oit=std::min(oit,e.identity.local_id.value);
  }
  return InventoryOracle(p,oit,oat,oat);
}
void InventoryReject(const page::NativeTransactionInventoryPageResult& r) {
  Check(!r.ok()&&!r.page&&r.bytes.empty(),"invalid inventory returns no page prefix");
}
void CanonicalInventoryImages() {
  using E=page::NativeInventoryError;
  for(unsigned profile=0;profile<5;++profile) {
    auto p=InventoryExample(profile); const auto oracle=InventoryOracle(p,1,1,1);
    const auto encoded=page::EncodeNativeTransactionInventoryPage(p);
    if(!encoded.ok()) std::cerr<<"inventory_encode_error="<<static_cast<unsigned>(encoded.error)<<'\n';
    else if(encoded.bytes!=oracle) {
      const auto mismatch=std::mismatch(encoded.bytes.begin(),encoded.bytes.end(),oracle.begin());
      std::cerr<<"inventory_oracle_offset="<<(mismatch.first-encoded.bytes.begin())
        <<" actual="<<static_cast<unsigned>(*mismatch.first)<<" expected="<<static_cast<unsigned>(*mismatch.second)<<'\n';
    }
    Check(encoded.ok()&&encoded.bytes==oracle,"independent canonical inventory bytes all profiles");
    const auto decoded=page::DecodeNativeTransactionInventoryPage(oracle);
    Check(decoded.ok()&&decoded.page->inventory.entries.size()==1
      &&decoded.page->inventory.entries[0].identity.transaction_uuid.value==Id(93)
      &&!decoded.page->inventory.publication_base,"decode binary identity without invented publication base");
    p.inventory.entries.clear(); const auto empty=InventoryOracle(p,3,3,3);
    Check(page::EncodeNativeTransactionInventoryPage(p).bytes==empty
      &&page::DecodeNativeTransactionInventoryPage(empty).ok(),"empty allocated native inventory slice");
    const auto capacity=(sizes[profile]-384)/72;
    for(unsigned i=0;i<capacity;++i) {
      auto e=InventoryExample().inventory.entries[0]; e.identity.local_id=mga::MakeLocalTransactionId(i+1);
      e.identity.transaction_uuid.value=Id(93); e.identity.transaction_uuid.value.bytes[14]=static_cast<byte>(i>>8);
      e.identity.transaction_uuid.value.bytes[15]=static_cast<byte>(i);
      p.inventory.entries.push_back(e);
    }
    p.inventory.next_local_transaction_id=capacity+1;
    const auto full=page::EncodeNativeTransactionInventoryPage(p);
    Check(full.ok()&&full.bytes==InventoryOracle(p,1,1,1)
      &&page::DecodeNativeTransactionInventoryPage(full.bytes).page->inventory.entries.size()==capacity,"exact full inventory capacity");
    p.inventory.entries.push_back(p.inventory.entries.back()); InventoryReject(page::EncodeNativeTransactionInventoryPage(p));
  }
  auto p=InventoryExample(); const auto good=InventoryOracle(p,1,1,1);
  for(std::size_t i=0;i<good.size();++i) { auto b=good;b[i]^=1;InventoryReject(page::DecodeNativeTransactionInventoryPage(b)); }
  for(std::size_t at:{128u,136u,138u,140u,176u,184u,188u,192u,240u,288u,296u,304u,312u,352u,456u}) {
    auto b=good; b[at]^=1; InventorySeal(b); InventoryReject(page::DecodeNativeTransactionInventoryPage(b));
  }
  for(std::size_t at:{144u,160u,168u}) {
    auto b=good;std::fill_n(b.begin()+at,at==144?16:8,0);InventorySeal(b);InventoryReject(page::DecodeNativeTransactionInventoryPage(b));
  }
  for(unsigned state:{0u,14u,65535u}) { auto b=good;Number(b,410,2,state);InventorySeal(b);InventoryReject(page::DecodeNativeTransactionInventoryPage(b)); }
  for(unsigned scope:{2u,65535u}) { auto b=good;Number(b,408,2,scope);InventorySeal(b);InventoryReject(page::DecodeNativeTransactionInventoryPage(b)); }
  for(std::size_t at:{398u,400u,412u,448u}) {
    auto b=good; b[at]=255; InventorySeal(b); InventoryReject(page::DecodeNativeTransactionInventoryPage(b));
  }
  for(std::size_t size:{0u,127u,8191u,8193u}) {auto b=good;b.resize(size);InventoryReject(page::DecodeNativeTransactionInventoryPage(b));}
  for(auto origin:{mga::TransactionState::committed,mga::TransactionState::rolled_back,mga::TransactionState::failed_terminal}) {
    auto archived=p;auto& e=archived.inventory.entries[0];e.state=mga::TransactionState::archived;e.archived_from_state=origin;
    if(origin==mga::TransactionState::committed) {e.commit_sequence=1;archived.inventory.next_commit_sequence=2;}
    const auto bytes=InventoryOracle(archived,origin==mga::TransactionState::failed_terminal?1:3,3,3);const auto r=page::DecodeNativeTransactionInventoryPage(bytes);
    Check(r.ok()&&r.page->inventory.entries[0].archived_from_state==origin
      &&page::EncodeNativeTransactionInventoryPage(archived).bytes==bytes,"archive terminal origin survives canonical inventory");
  }
  for(unsigned fault=1;fault<=4;++fault) {
    hash_fault=fault;const auto r=page::DecodeNativeTransactionInventoryPage(good);
    Check(!r.ok()&&r.error==E::hash_failure&&hash_fault==0&&!r.page&&r.bytes.empty(),"inventory digest backend failure");
  }
  for(unsigned mode=0;mode<2;++mode) {
    bool success=false;
    for(long budget=0;budget<100;++budget) {
      allocation_budget=budget;const auto r=mode?page::DecodeNativeTransactionInventoryPage(good):page::EncodeNativeTransactionInventoryPage(p);allocation_budget=-1;
      if(r.ok()){success=true;break;} Check(r.error==E::resource_exhausted&&!r.page&&r.bytes.empty(),"inventory allocation fails atomically");
    }
    Check(success,"inventory all allocation positions complete");
  }
}
void CanonicalInventoryChains() {
  using E=page::NativeInventoryError; Fixture fixture; disk::FileDevice first,second;
  auto head=InventoryExample(),tail=InventoryExample(1);
  tail.header.filespace_uuid=Id(7);tail.header.page_number=17;tail.header.page_uuid=Id(94);
  tail.inventory.entries[0].identity.local_id=mga::MakeLocalTransactionId(2);
  tail.inventory.entries[0].identity.transaction_uuid.value=Id(95);tail.inventory.entries[0].state=mga::TransactionState::rolled_back;
  head.next=InventoryRef(tail);tail.previous=InventoryRef(head);
  auto z1=Example(),z2=Example(1);z2.bootstrap.filespace_uuid=Id(7);
  for(auto& root:z2.roots)root.filespace_uuid=Id(7);
  const auto path1=(fixture.root/"inventory-primary").string(),path2=(fixture.root/"inventory-secondary-primary").string();
  Check(first.Open(path1,disk::FileOpenMode::create_new).ok()&&second.Open(path2,disk::FileOpenMode::create_new).ok(),"own both inventory filespaces");
  auto persist=[&](disk::FileDevice& d,const auto& zero,const auto& p,u64 horizon) {
    const auto z=Oracle(zero),b=InventoryOracle(p,horizon,horizon,horizon);const byte v=0;
    Check(d.WriteAt(0,z.data(),z.size()).ok()&&d.WriteAt(zero.total_pages*zero.bootstrap.page_size_bytes-1,&v,1).ok()
      &&d.WriteAt(p.header.page_number*p.header.page_size_bytes,b.data(),b.size()).ok()&&d.Sync().ok(),"persist actual canonical inventory images");
  };
  persist(first,z1,head,1);persist(second,z2,tail,3);
  const std::vector<disk::NativeFilespaceDevice> devices{{Id(7),Profile(1),&second},{Id(2),Profile(0),&first}};
  const auto root=z1.roots[3];
  const auto read=[&](u64 budget=24576) {return page::ReadNativeTransactionInventoryChainFromOpenDevices(Id(1),devices,root,budget);};
  const auto check_empty=[&](const auto& r) {Check(!r.ok()&&r.pages.empty()&&r.inventory.entries.empty()&&!r.inventory.publication_base&&!r.horizons.valid&&r.retained_image_bytes==0,"chain failure retains no prefix or authority");};
  auto r=read();Check(r.ok()&&r.pages.size()==2&&r.inventory.entries.size()==2&&r.retained_image_bytes==24576
    &&r.horizons.oldest_active_transaction.value==1&&!r.inventory.publication_base,"full mixed-profile binary chain and horizons");
  r=read(24575);check_empty(r);Check(r.error==E::resource_exhausted,"exact retained image budget");
  for(unsigned field=0;field<7;++field) {
    auto bad=tail;
    if(field==0)bad.previous.reset();if(field==1)++bad.previous->page_generation;
    if(field==2)++bad.inventory_generation;if(field==3)++bad.inventory.next_local_transaction_id;
    if(field==4)bad.header.page_uuid=head.header.page_uuid;
    if(field==5)bad.inventory.entries[0].identity.transaction_uuid=head.inventory.entries[0].identity.transaction_uuid;
    if(field==6)bad.inventory.entries[0].identity.local_id=mga::MakeLocalTransactionId(1);
    persist(second,z2,bad,bad.inventory.next_local_transaction_id);check_empty(read());
  }
  persist(second,z2,tail,3);
  auto duplicate=devices;duplicate.push_back(devices[0]);
  check_empty(page::ReadNativeTransactionInventoryChainFromOpenDevices(Id(1),duplicate,root,24576));
  for(unsigned field=0;field<4;++field){auto bad=root;if(field==0)++bad.page_generation;if(field==1)bad.object_uuid=Id(98);if(field==2)bad.kind=2;if(field==3)bad.filespace_uuid=Id(98);check_empty(page::ReadNativeTransactionInventoryChainFromOpenDevices(Id(1),devices,bad,24576));}
  for(unsigned fault=1;fault<=4;++fault){hash_fault=fault;r=read();Check(hash_fault==0,"chain hash fault consumed");check_empty(r);Check(r.error==E::hash_failure,"chain hash backend refusal");}
  reads=0;track_reads=true;r=read();track_reads=false;const auto read_count=reads;
  Check(r.ok()&&read_count>3,"measure complete actual chain reads");
  for(unsigned fault=1;fault<=read_count;++fault){reads=0;read_fault=fault;track_reads=true;r=read();track_reads=false;Check(read_fault==0,"chain actual read fault consumed");check_empty(r);}
  observed_allocations=0;count_allocations=true;r=read();count_allocations=false;
  const auto allocation_count=observed_allocations;Check(r.ok(),"measure complete chain allocations");
  bool success=false;
  for(unsigned long budget=0;budget<=allocation_count;++budget){allocation_budget=static_cast<long>(budget);r=read();allocation_budget=-1;if(r.ok()){success=true;break;}check_empty(r);Check(r.error==E::resource_exhausted,"chain allocation diagnostic");}
  Check(success,"complete chain under all allocation positions");
  auto bad_tail=InventoryOracle(tail,3,3,3);bad_tail[420]^=1;
  Check(second.WriteAt(tail.header.page_number*tail.header.page_size_bytes,bad_tail.data(),bad_tail.size()).ok()&&second.Sync().ok(),"persist late-page corruption");
  r=read();check_empty(r);Check(r.error==E::invalid_integrity,"late-page corruption rejects earlier inventory prefix");
  persist(second,z2,tail,3);
  auto committed_head=head,committed_tail=tail;
  for(auto* p:{&committed_head,&committed_tail}){p->inventory.entries[0].state=mga::TransactionState::committed;p->inventory.entries[0].commit_sequence=1;p->inventory.next_commit_sequence=2;}
  persist(first,z1,committed_head,3);persist(second,z2,committed_tail,3);
  r=read();check_empty(r);Check(r.error==E::invalid_inventory,"duplicate commit order across individually valid pages refused");
  committed_tail.inventory.entries[0].commit_sequence=2;committed_head.inventory.next_commit_sequence=committed_tail.inventory.next_commit_sequence=3;
  persist(first,z1,committed_head,3);persist(second,z2,committed_tail,3);
  r=read();Check(r.ok()&&r.inventory.entries[1].commit_sequence==2,"distinct global committed order admitted");
  persist(first,z1,head,1);persist(second,z2,tail,3);
  auto encrypted=tail;encrypted.header.flags=1;persist(second,z2,encrypted,3);
  r=read();check_empty(r);Check(r.error==E::encrypted_requires_crypto_authority,"raw encrypted inventory never interpreted as plaintext");
  persist(second,z2,tail,3);
  auto missing=devices;missing.erase(missing.begin());check_empty(page::ReadNativeTransactionInventoryChainFromOpenDevices(Id(1),missing,root,24576));
  auto mismatched=devices;mismatched[0].page_size_profile_uuid=Profile(0);check_empty(page::ReadNativeTransactionInventoryChainFromOpenDevices(Id(1),mismatched,root,24576));
  std::atomic<unsigned> completed=0;
  auto reverse=devices;std::reverse(reverse.begin(),reverse.end());
  const auto concurrent=[&](const auto& owners){for(unsigned n=0;n<16;++n){const auto result=page::ReadNativeTransactionInventoryChainFromOpenDevices(Id(1),owners,root,24576);if(result.ok()&&result.inventory.entries.size()==2)++completed;}};
  std::thread one([&]{concurrent(devices);}),two([&]{concurrent(reverse);});one.join();two.join();
  Check(completed==32,"opposite supplied device orders converge on one binary lock order");
  Exclusive(path1);Exclusive(path2);
  Check(first.Close().ok()&&second.Close().ok(),"close canonical inventory filespaces");
  Check(first.Open(path1,disk::FileOpenMode::open_existing_read_only).ok()&&second.Open(path2,disk::FileOpenMode::open_existing_read_only).ok(),"reopen canonical inventory filespaces");
  r=read();Check(r.ok()&&r.inventory.entries[1].state==mga::TransactionState::rolled_back,"reopened canonical inventory exact outcomes");
  Check(first.Close().ok()&&second.Close().ok(),"release before fresh process inventory reader");
  const auto child=::fork();Check(child>=0,"fork independent inventory reader");
  if(child==0){::execl("/proc/self/exe","inventory-chain-probe","--inventory-chain-probe",fixture.root.c_str(),nullptr);::_exit(125);}
  int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable reads actual canonical inventory chain");
}


Bytes AllocationOracle(const page::NativeAllocationMap& map) {
  auto common=RootExample();common.header=map.header;
  auto b=RootOracle(common);std::fill(b.begin()+128,b.end(),0);
  unsigned version=map.creator_operation_uuid.is_nil()?1:2;
  for(const auto& r:map.records)if(!r.creator_operation_uuid.is_nil())version=2;
  std::copy_n(version==1?"SBABM001":"SBABM002",8,b.begin()+128);Number(b,136,2,version);Number(b,138,2,256);
  const auto bitmap=(map.states.size()+1)/2,at=(384+bitmap+7)&~std::size_t(7);
  Number(b,140,4,at+128*map.records.size());PutUuid(b,144,map.object_uuid);
  Number(b,160,8,map.map_generation);Number(b,168,8,map.capacity_generation);
  Number(b,176,8,map.total_pages);Number(b,184,8,map.first_page);Number(b,192,8,map.states.size());
  PutUuid(b,200,map.creator_transaction_uuid);Number(b,216,8,map.creator_local_transaction_id);
  if(map.next){PutUuid(b,224,map.next->filespace_uuid);Number(b,240,8,map.next->page_number);
    Number(b,248,8,map.next->page_generation);PutUuid(b,256,map.next->page_size_profile_uuid);}
  std::copy(map.next_sha256.begin(),map.next_sha256.end(),b.begin()+272);
  Number(b,304,4,bitmap);Number(b,308,4,map.records.size());
  PutUuid(b,344,map.creator_operation_uuid);
  for(std::size_t i=0;i<map.states.size();++i)b[384+i/2]|=static_cast<byte>(map.states[i])<<(4*(i%2));
  for(std::size_t i=0;i<map.records.size();++i){const auto& r=map.records[i];const auto pos=at+128*i;
    Number(b,pos,8,r.page_number);PutUuid(b,pos+8,r.allocation_uuid);PutUuid(b,pos+24,r.page_uuid);
    PutUuid(b,pos+40,r.owner_uuid);PutUuid(b,pos+56,r.creator_transaction_uuid);
    Number(b,pos+72,8,r.creator_local_transaction_id);Number(b,pos+80,8,r.page_generation);
    Number(b,pos+88,8,r.reuse_horizon);Number(b,pos+96,4,r.page_type);PutUuid(b,pos+100,r.creator_operation_uuid);}
  Check(SHA256(b.data(),b.size(),b.data()+312)!=nullptr,"independent allocation image seal");return b;
}
constexpr std::array<u64,5> initialization_coverage{{60,124,252,507,1017}};
db::NativeFilespaceInitializationRequest InitializationRequest(unsigned p=0,unsigned role=1,u64 total=5,unsigned flags=0){
  db::NativeFilespaceInitializationRequest r;r.bootstrap=Example(p,role,7,flags).bootstrap;
  r.operation_uuid=Id(5);r.writer_uuid=Id(6);r.policy_snapshot_uuid=Id(10);r.creator={{1},{scratchbird::core::platform::UuidKind::transaction,Id(90)},mga::TransactionScope::local_node};
  r.creation_utc_millis=1790000000000ULL;r.total_pages=total;return r;
}
void VerifyInitializedFilespace(disk::FileDevice& device,const db::NativeFilespaceInitializationRequest& request,
    unsigned p,const db::NativeFilespaceInitializationReceipt* receipt=nullptr){
  const u64 size=sizes[p],coverage=initialization_coverage[p];const auto total=request.total_pages;
  const u64 count=(total+coverage-1)/coverage,budget=(count+2)*size;
  const auto z=disk::ReadFilespacePageZeroFromOpenDevice(device);Check(z.ok(),"actual initialized page zero");
  const auto& zero=*z.record;const auto& b=request.bootstrap;
  Check(device.Size().size_bytes==total*size&&zero.bootstrap.database_uuid==b.database_uuid&&zero.bootstrap.filespace_uuid==b.filespace_uuid&&
    zero.bootstrap.page_size_profile_uuid==b.page_size_profile_uuid&&zero.bootstrap.page_size_bytes==size&&zero.bootstrap.filespace_role==b.filespace_role&&
    zero.bootstrap.flags==b.flags&&zero.bootstrap.encryption_profile_uuid==b.encryption_profile_uuid&&zero.bootstrap.checksum_profile_uuid==b.checksum_profile_uuid&&
    zero.bootstrap.lifecycle_state==7&&zero.page_generation==1&&zero.root_set_generation==1&&zero.total_pages==total&&zero.free_pages==total-count-1&&
    zero.preallocated_pages==0&&zero.creation_utc_millis==request.creation_utc_millis&&zero.creation_operation_uuid==request.operation_uuid&&
    zero.writer_identity_uuid==request.writer_uuid&&zero.roots.size()==1,"exact non-serving bootstrap, capacity and creation lineage");
  const auto& root=zero.roots.front();Check(root.kind==3&&root.page_type==3&&root.filespace_uuid==b.filespace_uuid&&root.page_number==1&&
    root.page_generation==1&&root.page_size_profile_uuid==b.page_size_profile_uuid,"sole initial allocation root");
  const auto chain=page::ReadNativeAllocationChainFromOpenDevice(device,{b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid},budget);
  Check(chain.ok()&&chain.pages.size()==count&&chain.state_counts[0]==total-count-1&&chain.state_counts[2]==count+1,"actual complete initial allocation chain");
  std::set<Uuid> ids{b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid,b.checksum_profile_uuid,b.encryption_profile_uuid,
    request.operation_uuid,request.writer_uuid,request.creator.transaction_uuid.value,request.policy_snapshot_uuid};
  const auto fresh=[&](const Uuid& id){u64 timestamp=0;for(unsigned i=0;i<6;++i)timestamp=(timestamp<<8)|id.bytes[i];
    Check((id.bytes[6]>>4)==7&&(id.bytes[8]&0xc0)==0x80&&timestamp==initialization_uuid_millis&&timestamp!=request.creation_utc_millis&&ids.insert(id).second,"fresh distinct binary UUIDv7 identity from accepted clock not lineage timestamp");};
  fresh(zero.page_uuid);fresh(root.object_uuid);
  for(const auto& image:chain.pages)fresh(image.map->header.page_uuid);
  Bytes actual(size);Check(device.ReadAt(0,actual.data(),actual.size()).ok()&&actual==Oracle(zero),"independent exact initialized page-zero image");
  for(u64 i=0;i<count;++i){const auto& map=*chain.pages[i].map;const auto& h=map.header;const auto first=i*coverage,covered=std::min(coverage,total-first);
    Check(h.page_size_bytes==size&&h.page_type==3&&h.database_uuid==b.database_uuid&&h.filespace_uuid==b.filespace_uuid&&h.page_number==i+1&&
      h.page_generation==1&&h.flags==0&&h.page_size_profile_uuid==b.page_size_profile_uuid&&map.object_uuid==root.object_uuid&&map.map_generation==1&&
      map.capacity_generation==1&&map.total_pages==total&&map.first_page==first&&map.states.size()==covered&&map.creator_transaction_uuid==request.creator.transaction_uuid.value&&
      map.creator_local_transaction_id==request.creator.local_id.value,"exact initialized map identity, creator and independently specified coverage");
    Check(map.next.has_value()==(i+1<count),"complete map chain terminality");
    if(map.next){const auto& next=*map.next;Check(next.filespace_uuid==b.filespace_uuid&&next.page_number==i+2&&next.page_generation==1&&
      next.page_size_profile_uuid==b.page_size_profile_uuid&&map.next_sha256==WholeRootHash(chain.pages[i+1].bytes),"exact actual map successor reference and whole-image hash");}
    else Check(map.next_sha256==std::array<byte,32>{},"terminal map has no hidden link digest");
    std::size_t records=0;for(u64 n=first;n<first+covered;++n){Check(map.states[n-first]==(n<=count?page::NativeAllocationState::allocated:page::NativeAllocationState::free),"only actual control pages allocated");
      if(n>count)continue;Check(records<map.records.size(),"every control page has allocation identity");const auto& r=map.records[records++];fresh(r.allocation_uuid);
      Check(r.page_number==n&&r.page_uuid==(n?chain.pages[n-1].map->header.page_uuid:zero.page_uuid)&&r.owner_uuid==(n?root.object_uuid:b.filespace_uuid)&&
        r.creator_transaction_uuid==request.creator.transaction_uuid.value&&r.creator_local_transaction_id==request.creator.local_id.value&&r.page_generation==1&&
        r.reuse_horizon==0&&r.page_type==(n?3u:b.filespace_role<=4?1u:2u),"exact physical control-page allocation and pending creation lineage");}
    Check(records==map.records.size(),"free pages carry no fabricated allocations");
    Check(device.ReadAt((i+1)*size,actual.data(),actual.size()).ok()&&actual==AllocationOracle(map)&&actual==chain.pages[i].bytes,"independent complete initialized map image");
  }
  for(u64 n=count+1;n<total;++n)Check(device.ReadAt(n*size,actual.data(),actual.size()).ok()&&std::all_of(actual.begin(),actual.end(),[](byte v){return !v;}),"every actual free physical page is zero");
  if(receipt)Check(receipt->database_uuid==b.database_uuid&&receipt->filespace_uuid==b.filespace_uuid&&receipt->page_zero_uuid==zero.page_uuid&&
    receipt->map_uuid==root.object_uuid&&receipt->operation_uuid==request.operation_uuid&&receipt->writer_uuid==request.writer_uuid&&
    receipt->creator.transaction_uuid.value==request.creator.transaction_uuid.value&&receipt->creator.local_id.value==request.creator.local_id.value&&
    receipt->creator.scope==request.creator.scope&&receipt->allocation_root.kind==3&&receipt->allocation_root.page_type==3&&
    receipt->allocation_root.filespace_uuid==b.filespace_uuid&&receipt->allocation_root.page_number==1&&receipt->allocation_root.page_generation==1&&
    receipt->allocation_root.page_size_profile_uuid==b.page_size_profile_uuid&&receipt->allocation_root.object_uuid==root.object_uuid&&
    receipt->page_zero_sha256==WholeRootHash(Oracle(zero))&&receipt->allocation_head_sha256==WholeRootHash(chain.pages.front().bytes)&&
    receipt->total_pages==total&&receipt->free_pages==total-count-1&&receipt->map_pages==count,"receipt derives from actual complete initialized storage");
}
void CanonicalFilespaceInitialization(){using E=db::NativeFilespaceInitializationError;
  for(unsigned p=0;p<5;++p)for(unsigned role=1;role<=15;++role){Fixture fixture;disk::FileDevice device;
    const auto path=(fixture.root/"initializing").string();auto request=InitializationRequest(p,role==15?1:role,role==1?initialization_coverage[p]+1:5,role==15?1:0);
    auto issuer=std::make_unique<scratchbird::core::uuid::StandaloneUuidV7Issuer>(scratchbird::core::uuid::StandaloneUuidV7Binding{request.bootstrap.database_uuid,request.policy_snapshot_uuid},scratchbird::core::uuid::StandaloneUuidV7Policy{{},0,1000});
    const u64 count=(request.total_pages+initialization_coverage[p]-1)/initialization_coverage[p],budget=(count+2)*sizes[p];
    Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"exclusively own empty initialization device");
    auto result=db::InitializeNativeFilespaceOnOpenDevice(device,request,budget-1,*issuer);
    Check(!result.ok()&&!result.receipt&&result.error==E::resource_exhausted&&device.Size().size_bytes==0,"one-byte image-budget deficit cannot write");
    result=db::InitializeNativeFilespaceOnOpenDevice(device,request,budget,*issuer);Check(result.ok(),"real initializing filespace writer");
    VerifyInitializedFilespace(device,request,p,&*result.receipt);
    const auto prior=disk::ReadFilespacePageZeroFromOpenDevice(device);result=db::InitializeNativeFilespaceOnOpenDevice(device,request,budget,*issuer);
    Check(!result.ok()&&!result.receipt&&result.error==E::device_not_empty&&Oracle(*disk::ReadFilespacePageZeroFromOpenDevice(device).record)==Oracle(*prior.record),"nonempty initializing device cannot be reinterpreted or retried");
    if(role==1){Exclusive(path);Check(device.Close().ok(),"release initialization owner before fresh reader");const auto child=::fork();Check(child>=0,"fork initialized filespace reader");
      if(child==0){const auto profile=std::to_string(p),total=std::to_string(request.total_pages);::execl("/proc/self/exe","filespace-initialization-probe","--filespace-initialization-probe",path.c_str(),profile.c_str(),total.c_str(),nullptr);::_exit(125);}
      int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh process verifies real initialization without creator caches");}
  }
  Fixture fixture;disk::FileDevice device;unsigned attempt=0;auto request=InitializationRequest();const u64 budget=3*sizes[0];std::string path;
  std::unique_ptr<scratchbird::core::uuid::StandaloneUuidV7Issuer> issuer;
  const auto reset=[&](){issuer=std::make_unique<scratchbird::core::uuid::StandaloneUuidV7Issuer>(scratchbird::core::uuid::StandaloneUuidV7Binding{request.bootstrap.database_uuid,request.policy_snapshot_uuid},scratchbird::core::uuid::StandaloneUuidV7Policy{{},0,1000});if(device.is_open())Check(device.Close().ok(),"close owned initialization fault fixture");path=(fixture.root/std::to_string(attempt++)).string();
    Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"new empty initialization fault fixture");};
  const auto initialize=[&](){return db::InitializeNativeFilespaceOnOpenDevice(device,request,budget,*issuer);};
  const auto empty=[&](const auto& r,std::source_location at=std::source_location::current()){
    Check(!r.ok()&&!r.receipt,"initialization failure never returns a receipt",at);};
  const auto image=[&](){const auto size=device.Size();Check(size.ok(),"read actual interrupted extent");Bytes bytes(size.size_bytes);if(!bytes.empty())Check(device.ReadAt(0,bytes.data(),bytes.size()).ok(),"read actual interrupted bytes");return bytes;};
  const auto preserved=[&](){const auto before=image();if(!before.empty()){const auto again=initialize();empty(again);Check(again.error==E::device_not_empty&&image()==before,"interrupted owned bytes preserved without implicit repair");}};
  reset();initialization_entropy_calls=stage_writes=stage_syncs=reads=observed_full_digests=observed_allocations=0;
  track_reads=count_full_digests=count_allocations=true;auto result=initialize();track_reads=count_full_digests=count_allocations=false;
  Check(result.ok(),"cold initialization succeeds");const auto entropy_count=initialization_entropy_calls,write_count=stage_writes,read_count=reads,digest_count=observed_full_digests;
  const auto allocation_count=observed_allocations;Check(entropy_count==1&&write_count==7&&stage_syncs==1,"all identities issued before exact physical writes and sync");
  for(bool allocation:{false,true}){reset();initialization_clock_failure=!allocation;initialization_clock_allocation_failure=allocation;
    result=initialize();initialization_clock_failure=initialization_clock_allocation_failure=false;empty(result);
    Check(result.error==(allocation?E::resource_exhausted:E::identity_failure)&&!device.Size().size_bytes,"clock or allocation source failure preserves empty filespace and exact error");
  }
  for(unsigned fault=1;fault<=entropy_count;++fault){reset();initialization_entropy_fault=fault;result=initialize();Check(!initialization_entropy_fault,"each entropy failure consumed");empty(result);
    Check(result.error==E::identity_failure&&device.Size().size_bytes==0,"no generated substitute after randomness failure");}
  reset();initialization_repeat_entropy=true;result=initialize();initialization_repeat_entropy=false;empty(result);Check(result.error==E::identity_failure&&device.Size().size_bytes==0,"duplicate generated UUIDs refuse before writes");
  for(unsigned fault=1;fault<=write_count;++fault)for(unsigned mode:{1u,3u}){reset();stage_write_fault=mode;stage_write_fault_after=fault;result=initialize();
    Check(!stage_write_fault&&!stage_write_fault_after,"every early and late physical write fault consumed");empty(result);Check(result.error==E::io_failure,"write failure classified");preserved();}
  reset();stage_sync_fault=1;result=initialize();Check(!stage_sync_fault,"initialization sync fault consumed");empty(result);preserved();
  for(unsigned fault=1;fault<=read_count;++fault){reset();reads=0;read_fault=fault;track_reads=true;result=initialize();track_reads=false;Check(!read_fault,"every initialization read failure consumed");empty(result);preserved();}
  for(unsigned fault=1;fault<=read_count;++fault){reset();reads=0;stage_corrupt_read=fault;track_reads=true;result=initialize();track_reads=false;Check(!stage_corrupt_read,"every final verification read corruption consumed");empty(result);preserved();}
  for(unsigned fault=1;fault<=digest_count;++fault){reset();full_digest_fault=fault;result=initialize();Check(!full_digest_fault,"every initialization full hash failure consumed");empty(result);preserved();}
  for(unsigned fault=1;fault<=5;++fault){reset();hash_fault=fault;result=initialize();Check(!hash_fault,"every multipart hash failure consumed");empty(result);preserved();}
  bool allocation_end=false;unsigned long consumed=0,observation_loss_sites=0;
  for(unsigned long fault=0;fault<=allocation_count;++fault){reset();const auto losses=device.failed_io_latency_observations();
    allocation_budget=static_cast<long>(fault);result=initialize();const bool injected=allocation_budget<0;allocation_budget=-1;
    if(!injected){Check(result.ok(),"uninjected allocation sweep termination succeeds");VerifyInitializedFilespace(device,request,0,&*result.receipt);allocation_end=true;break;}
    ++consumed;
    if(result.ok()){
      // Latency observation is not physical I/O authority. The actual disk
      // observer must account for this precise swallowed failure, and all
      // durable bytes and receipt fields still pass the independent oracle.
      Check(device.failed_io_latency_observations()==losses+1,"initialization success after allocation fault requires recorded observation loss");
      ++observation_loss_sites;VerifyInitializedFilespace(device,request,0,&*result.receipt);
    }else {empty(result);preserved();}}
  Check(allocation_end,"all reachable allocation failure positions exercised");
  std::cout<<"initialization fault sites: allocations="<<consumed<<" observation_loss="<<observation_loss_sites<<" entropy="<<entropy_count<<" writes="<<write_count<<" reads="<<read_count<<" full_hashes="<<digest_count<<std::endl;
  for(unsigned fault=0;fault<22;++fault){reset();auto changed=request;
    if(fault==0)changed.bootstrap.lifecycle_state=1;if(fault==1)changed.bootstrap.page_size_bytes=4096;
    if(fault==2)changed.bootstrap.database_uuid={};if(fault==3)changed.bootstrap.filespace_uuid={};if(fault==4)changed.bootstrap.page_size_profile_uuid=Id(91);
    if(fault==5)changed.operation_uuid={};if(fault==6)changed.writer_uuid.bytes[6]=0x41;if(fault==7)changed.creator.transaction_uuid.value={};
    if(fault==8)changed.creator.transaction_uuid.kind=scratchbird::core::platform::UuidKind::object;if(fault==9)changed.creator.local_id.value=0;
    if(fault==10)changed.creator.scope=mga::TransactionScope::cluster_global;if(fault==11)changed.creation_utc_millis=0;if(fault==12)changed.creation_utc_millis=0x1000000000000ULL;
    if(fault==13)changed.total_pages=0;if(fault==14)changed.total_pages=1;if(fault==15)changed.total_pages=std::numeric_limits<u64>::max();
    if(fault==16)changed.bootstrap.flags|=disk::FilespaceBootstrapFlag::cluster_authority_required;
    if(fault==17)changed.policy_snapshot_uuid={};if(fault==18)changed.policy_snapshot_uuid.bytes[6]=0x40;
    if(fault==19)changed.policy_snapshot_uuid=Id(91);if(fault==20)changed.bootstrap.database_uuid=Id(91);if(fault==21)changed.policy_snapshot_uuid=changed.writer_uuid;
    result=db::InitializeNativeFilespaceOnOpenDevice(device,changed,budget,*issuer);empty(result);Check(device.Size().size_bytes==0,"invalid input preserves empty owned device");
    if(fault==16)Check(result.error==E::cluster_requires_authority,"cluster initialization stays at provider boundary");}
  reset();Check(device.Close().ok()&&device.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"empty read-only fixture");result=initialize();empty(result);Check(result.error==E::invalid_device,"read-only initialization refused");
  Check(device.Close().ok(),"closed initialization fixture");result=initialize();empty(result);Check(result.error==E::invalid_device,"closed initialization refused");
  reset();const byte unknown=0xa5;Check(device.WriteAt(0,&unknown,1).ok(),"one unknown existing byte");result=initialize();empty(result);Check(result.error==E::device_not_empty&&image()==Bytes{unknown},"unknown bytes are never truncated or converted");
  reset();pause_next_tree_read=true;tree_read_paused=false;resume_tree_read=false;std::atomic<bool> second_started=false,second_finished=false;
  db::NativeFilespaceInitializationResult first,second;
  std::thread a([&](){first=initialize();});while(!tree_read_paused.load())std::this_thread::yield();
  std::thread b([&](){second_started=true;second=initialize();second_finished=true;});while(!second_started.load())std::this_thread::yield();
  const bool was_blocked=!second_finished.load();resume_tree_read=true;a.join();b.join();
  Check(was_blocked&&first.ok()&&!second.ok()&&!second.receipt&&second.error==E::device_not_empty,"retained guard excludes concurrent initialization through final readback");VerifyInitializedFilespace(device,request,0,&*first.receipt);
  reset();request.total_pages=2;result=initialize();Check(result.ok()&&result.receipt->free_pages==0,"minimum complete physical capacity initializes without free pages");VerifyInitializedFilespace(device,request,0,&*result.receipt);
  request.total_pages=initialization_coverage[0]+1;
  for(unsigned fault:{1u,61u,62u,63u,64u}){reset();stage_write_fault=3;stage_write_fault_after=fault;
    result=db::InitializeNativeFilespaceOnOpenDevice(device,request,4*sizes[0],*issuer);empty(result);
    Check(!stage_write_fault&&!stage_write_fault_after,"multi-map zeroing, first and second map, and final page-zero write faults consumed");preserved();}
  reset();request.total_pages=initialization_coverage[0]*initialization_coverage[0]+1;
  result=db::InitializeNativeFilespaceOnOpenDevice(device,request,63*sizes[0],*issuer);Check(result.ok()&&result.receipt->map_pages==61,"control pages can span allocation coverage ranges");VerifyInitializedFilespace(device,request,0,&*result.receipt);
}


Bytes DirectoryOracle(const page::NativeFilespaceDirectory& value) {
  auto common=RootExample();common.header=value.header;auto b=RootOracle(common);std::fill(b.begin()+128,b.end(),0);
  const auto ref=[&](std::size_t at,const disk::NativePageReference& r){PutUuid(b,at,r.filespace_uuid);Number(b,at+16,8,r.page_number);Number(b,at+24,8,r.page_generation);PutUuid(b,at+32,r.page_size_profile_uuid);};
  std::copy_n("SBFDIR01",8,b.begin()+128);Number(b,136,2,1);Number(b,138,2,256);Number(b,140,4,384+192*value.records.size());
  PutUuid(b,144,value.object_uuid);Number(b,160,8,value.directory_generation);PutUuid(b,168,value.creator_transaction_uuid);Number(b,184,8,value.creator_local_transaction_id);
  Number(b,192,8,value.total_records);Number(b,200,8,value.first_record);Number(b,208,4,value.records.size());if(value.next)ref(216,*value.next);
  std::copy(value.next_sha256.begin(),value.next_sha256.end(),b.begin()+264);
  for(std::size_t i=0;i<value.records.size();++i){const auto at=384+i*192;const auto& r=value.records[i];const auto& a=r.bootstrap;
    PutUuid(b,at,a.filespace_uuid);PutUuid(b,at+16,a.page_size_profile_uuid);PutUuid(b,at+32,a.checksum_profile_uuid);PutUuid(b,at+48,a.encryption_profile_uuid);
    PutUuid(b,at+64,r.locator_uuid);PutUuid(b,at+80,r.page_zero_uuid);Number(b,at+96,8,r.page_zero_generation);Number(b,at+104,8,r.root_set_generation);
    Number(b,at+112,8,r.total_pages);Number(b,at+120,8,r.verification_epoch);Number(b,at+128,2,a.filespace_role);Number(b,at+130,2,a.lifecycle_state);
    Number(b,at+132,4,a.flags);Number(b,at+136,4,a.page_size_bytes);Number(b,at+140,4,a.durable_format_generation);if(r.operation)ref(at+144,*r.operation);
  }
  const auto digest=WholeRootHash(b);std::copy(digest.begin(),digest.end(),b.begin()+296);return b;
}








struct CatalogTestPin {
  mga::SnapshotVectorResult published;
  mga::PublishedSnapshotPin pin;
  CatalogTestPin(const mga::LocalTransactionInventory& inventory,u64 reader) {
    published=mga::PublishStatementStableSnapshotVector(inventory,mga::MakeLocalTransactionId(reader),1790000001000ull);
    Check(published.ok(),"publish actual inventory snapshot");pin=mga::RetainPublishedSnapshotVector(published.descriptor.snapshot_uuid);Check(pin.valid(),"retain actual engine snapshot pin");
  }
  ~CatalogTestPin(){mga::RevokePublishedSnapshotVector(published.descriptor.snapshot_uuid);mga::ReleasePublishedSnapshotVector(published.descriptor.snapshot_uuid);}
};

#ifdef SB_NATIVE_ROUTE_SOURCE_TESTS
#endif

// Shared byte/fault fixtures may be included by focused component executables.
// Do not compile an unused external copy of this complete dispatcher into them:
// it otherwise retains and optimizes every unrelated test path at -O3.
#ifndef SB_NATIVE_STORAGE_FIXTURE_WITHOUT_MAIN
int main(int argc,char** argv) {
  if(argc==2&&std::string_view(argv[1])=="--catalog-binary-cells-only"){
    try{CatalogBinaryCells();CatalogLeafFiles();std::cout<<"catalog binary cell checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==2&&std::string_view(argv[1])=="--filespace-initialization-only"){
    try{CanonicalFilespaceInitialization();std::cout<<"filespace initialization checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==5&&std::string_view(argv[1])=="--filespace-initialization-probe"){
    try{const auto p=static_cast<unsigned>(std::stoul(argv[3]));if(p>=5)return 2;disk::FileDevice device;
      if(!device.Open(argv[2],disk::FileOpenMode::open_existing_read_only).ok())return 3;
      VerifyInitializedFilespace(device,InitializationRequest(p,1,std::stoull(argv[4])),p);return 0;
    }catch(...){return 4;}}
  if(argc==4&&std::string_view(argv[1])=="--catalog-relation-probe") {
    const auto p=static_cast<unsigned>(std::stoul(argv[3])),q=(p+1)%5,s=(p+2)%5;const std::filesystem::path dir=argv[2];disk::FileDevice first,second,base;
    if(!first.Open((dir/"catalog-index").string(),disk::FileOpenMode::open_existing_read_only).ok()||!second.Open((dir/"catalog-data").string(),disk::FileOpenMode::open_existing_read_only).ok()
      ||!base.Open((dir/"catalog-primary").string(),disk::FileOpenMode::open_existing_read_only).ok())return 2;
    const auto expected=CatalogBindingImages(p);const auto& h=expected.nodes[0].header;
    const auto result=db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),{{Id(9),Profile(s),&base},{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}},
      {1,0x200,{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid},expected.nodes[0].dependencies.index_uuid},
      {Id(101),expected.nodes[0].dependencies},4*sizes[p]+4*sizes[q]+3*sizes[s]);
    if(!result.ok()||!result.index||result.bindings.size()!=9||result.catalogs.size()!=4)return 3;
    for(unsigned i=0;i<4;++i)if(result.catalogs[i].bytes!=LeafOracle(expected.leaves[i]))return 4;return 0;
  }
  if(argc==4&&(std::string_view(argv[1])=="--native-btree-tree-probe"||std::string_view(argv[1])=="--native-btree-deep-probe")) {
    const auto p=static_cast<unsigned>(std::stoul(argv[3])),q=(p+1)%5,s=(p+2)%5;const std::filesystem::path dir=argv[2];
    disk::FileDevice first,second,base;if(!first.Open((dir/"tree-first").string(),disk::FileOpenMode::open_existing_read_only).ok()
      ||!second.Open((dir/"tree-second").string(),disk::FileOpenMode::open_existing_read_only).ok()||!base.Open((dir/"tree-base").string(),disk::FileOpenMode::open_existing_read_only).ok())return 2;
    const bool deep=std::string_view(argv[1])=="--native-btree-deep-probe";
    const auto expected=deep?BtreeDeepTreeExample(p):BtreeTreeExample(p);const auto& h=expected[0].header;
    const auto result=page::ReadNativeBtreeTreeFromOpenDevices(Id(1),{{Id(9),Profile(s),&base},{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}},
      {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid},expected[0].dependencies,(deep?2:1)*(4*sizes[p]+3*sizes[q])+(deep?sizes[p]:0));
    const std::vector<std::size_t> leaf_order=deep?std::vector<std::size_t>{3,4,6,7,10,11,13,14}:std::vector<std::size_t>{2,3,5,6};
    if(!result.ok()||result.pages.size()!=expected.size()||result.leaves!=leaf_order)return 3;
    GovernedBtreeParity({{Id(9),Profile(s),&base},{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}},
      {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid},expected[0].dependencies,
      (deep?2:1)*(4*sizes[p]+3*sizes[q])+(deep?sizes[p]:0),result);
    for(unsigned i=0;i<leaf_order.size();++i){const auto target=deep?(i<4?4+i:7+i):3+i;
      if(result.pages[result.leaves[i]].bytes!=BtreeOracle(expected[target]))return 4;}return 0;
  }
  if(argc==4&&std::string_view(argv[1])=="--native-btree-probe") {
    const auto p=static_cast<unsigned>(std::stoul(argv[3]));const auto expected=BtreeExample(p);disk::FileDevice device;
    if(!device.Open(argv[2],disk::FileOpenMode::open_existing_read_only).ok())return 2;
    const auto result=page::ReadNativeBtreePageFromOpenDevice(device,Id(1),{Id(2),12,102,Profile(p)},0x200,expected.dependencies);
    return result.ok()&&result.bytes==BtreeOracle(expected)?0:3;
  }
  if(argc==3&&std::string_view(argv[1])=="--inventory-chain-probe") {
    disk::FileDevice first,second;const std::filesystem::path root=argv[2];
    if(!first.Open((root/"inventory-primary").string(),disk::FileOpenMode::open_existing_read_only).ok()
      ||!second.Open((root/"inventory-secondary-primary").string(),disk::FileOpenMode::open_existing_read_only).ok())return 2;
    const auto r=page::ReadNativeTransactionInventoryChainFromOpenDevices(Id(1),{{Id(2),Profile(0),&first},{Id(7),Profile(1),&second}},Example().roots[3],24576);
    return r.ok()&&r.pages.size()==2&&r.inventory.entries.size()==2&&!r.inventory.publication_base
      &&r.inventory.entries[0].state==mga::TransactionState::active&&r.inventory.entries[1].state==mga::TransactionState::rolled_back?0:3;
  }
  if(argc==4&&std::string_view(argv[1])=="--leaf-probe") {
    if(std::string_view(argv[3]).size()!=1||argv[3][0]<'0'||argv[3][0]>'4') return 2;
    const auto p=static_cast<unsigned>(argv[3][0]-'0');disk::FileDevice d;
    if(!d.Open(argv[2],disk::FileOpenMode::open_existing_read_only).ok()) return 3;
    const auto r=db::ReadNativeCatalogLeafFromOpenDevice(d,Id(1),RootExample(p).roots[0]);
    return r.ok()&&r.metadata.size()==2&&r.bytes==LeafOracle(LeafExample(p))?0:4;
  }
  if(argc==4&&std::string_view(argv[1])=="--range-probe") {
    disk::FileDevice first,second;
    if(!first.Open(argv[2],disk::FileOpenMode::open_existing_read_only).ok()
        ||!second.Open(argv[3],disk::FileOpenMode::open_existing_read_only).ok()) return 3;
    const std::vector<page::NativeCatalogFilespaceDevice> devices{{Id(7),Profile(1),&second},{Id(2),Profile(0),&first}};
    auto initial=RootExample(),head=RootExample(); head.header.page_number=35; head.header.page_generation=302;
    const auto r=page::ReadNativeCatalogRootRangeFromOpenDevices(Id(1),devices,RootRef(head),RootRef(initial),32768);
    return r.ok()&&r.roots.size()==3&&r.retained_image_bytes==32768
      &&r.roots[0].root->catalog_generation==3&&r.roots[1].root->catalog_generation==2
      &&r.roots[2].root->catalog_generation==1?0:4;
  }
  if(argc==3&&std::string_view(argv[1])=="--probe") { disk::FileDevice d; return Locked(d.Open(argv[2],disk::FileOpenMode::open_existing))?0:1; }
  if(argc==4&&std::string_view(argv[1])=="--catalog-probe") {
    if(std::string_view(argv[3]).size()!=1||argv[3][0]<'0'||argv[3][0]>'4') return 2;
    const auto p=static_cast<unsigned>(argv[3][0]-'0'); disk::FileDevice d;
    if(!d.Open(argv[2],disk::FileOpenMode::open_existing_read_only).ok()) return 3;
    const auto r=page::ReadNativeCatalogRootFromOpenDevice(d,Id(1),Example(p).roots[1]);
    return r.ok()&&r.bytes==RootOracle(RootExample(p))?0:4;
  }
  if(argc==2&&std::string_view(argv[1])=="--native-btree-memory-only"){
    try{NativeBtreeTreeMemory();NativeBtreeTrees();std::cout<<"PASS governed_tree_checks="<<checks<<'\n';return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==2&&std::string_view(argv[1])=="--index-placement-only"){
    try{NativeBtreePages();NativeBtreeTreeMemory();NativeBtreeTrees();
      std::cout<<"PASS index_placement_checks="<<checks<<'\n';return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc!=1){std::cerr<<"FAIL unknown test mode or invalid argument count\n";return 2;}
  try { NativeCatalogRelationBindings(); NativeBtreeTreeMemory(); NativeBtreeTrees(); NativeBtreePages(); CanonicalInventoryImages(); CanonicalInventoryChains(); Codecs(); Files(); CatalogRoots(); CatalogRootFiles(); CatalogRootRanges(); CatalogLeaves(); CatalogLeafFiles();
    std::cout<<"PASS checks="<<checks<<" canonical_page_image_and_chain_only=true\n"; return 0; }
  catch(const std::exception& e) { allocation_budget=-1; std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n'; return 1; }
}
#endif
