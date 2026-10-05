// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_allocation_map.hpp"
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
#include "native_allocation_map_memory.hpp"
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
namespace {
namespace p = scratchbird::storage::page;
namespace d = scratchbird::storage::disk;
using namespace scratchbird::core::platform;
using S = p::NativeAllocationState;
using E = p::NativeAllocationError;
using Bytes = std::vector<byte>;
std::size_t checks = 0;
void Check(bool pass, const char* message, std::source_location where = std::source_location::current()) {
  ++checks;
  if (!pass) { std::cerr << where.line() << ": " << message << '\n'; throw std::runtime_error("native allocation conformance failed"); }
}
template<class F> auto DenyCodecAllocation(F&& call){
  const auto saved=allocation_budget;allocation_budget=0;
  auto result=call();const bool unchanged=allocation_budget==0;allocation_budget=saved;
  Check(unchanged,"native codec provider refusal must not allocate diagnostic text");
  return result;
}
Uuid Id(byte n) { return Uuid{{1,2,3,4,5,6,0x71,8,0x89,10,11,12,13,14,15,n}}; }
void Num(Bytes& b, std::size_t at, unsigned size, u64 n) {
  for (unsigned i = 0; i < size; ++i) b[at+i] = static_cast<byte>(n >> (8*i));
}
void Put(Bytes& b, std::size_t at, const Uuid& id) { std::copy(id.bytes.begin(), id.bytes.end(), b.begin()+at); }
std::array<byte,32> Hash(const Bytes& b) {
  std::array<byte,32> out{}; Check(SHA256(b.data(), b.size(), out.data()) != nullptr, "independent SHA256"); return out;
}
void Seal(Bytes& b) {
  std::fill(b.begin()+312, b.begin()+344, 0); const auto hash = Hash(b);
  std::copy(hash.begin(), hash.end(), b.begin()+312);
}
// Independent bytes, including the common header and its FNV checksum.
Bytes Oracle(const p::NativeAllocationMap& m) {
  Bytes b(m.header.page_size_bytes, 0); const auto& h = m.header;
  std::copy_n("SBPGV002",8,b.begin()); Num(b,8,4,128); Num(b,12,4,h.page_size_bytes);
  Num(b,16,4,h.page_type); Num(b,20,2,1); Num(b,22,2,1); Put(b,24,h.database_uuid);
  Put(b,40,h.filespace_uuid); Put(b,56,h.page_uuid); Num(b,72,8,h.page_number);
  Num(b,80,8,h.page_generation); Num(b,88,8,h.flags); Put(b,104,h.page_size_profile_uuid); Num(b,120,2,1);
  u64 fnv = 14695981039346656037ull;
  for (unsigned i=0;i<128;++i) { fnv ^= b[i]; fnv *= 1099511628211ull; } Num(b,96,8,fnv);
  const auto bitmap = (m.states.size()+1)/2, at = (384+bitmap+7)&~std::size_t(7);
  unsigned version=m.creator_operation_uuid.is_nil()?1:2;
  for(const auto& r:m.records)if(!r.creator_operation_uuid.is_nil())version=2;
  std::copy_n(version==1?"SBABM001":"SBABM002",8,b.begin()+128); Num(b,136,2,version); Num(b,138,2,256);
  Num(b,140,4,at+128*m.records.size()); Put(b,144,m.object_uuid); Num(b,160,8,m.map_generation);
  Num(b,168,8,m.capacity_generation); Num(b,176,8,m.total_pages); Num(b,184,8,m.first_page);
  Num(b,192,8,m.states.size()); Put(b,200,m.creator_transaction_uuid); Num(b,216,8,m.creator_local_transaction_id);
  if (m.next) { Put(b,224,m.next->filespace_uuid); Num(b,240,8,m.next->page_number);
    Num(b,248,8,m.next->page_generation); Put(b,256,m.next->page_size_profile_uuid); }
  std::copy(m.next_sha256.begin(),m.next_sha256.end(),b.begin()+272);
  Num(b,304,4,bitmap); Num(b,308,4,m.records.size());
  Put(b,344,m.creator_operation_uuid);
  for (std::size_t i=0;i<m.states.size();++i) b[384+i/2] |= static_cast<byte>(m.states[i]) << (4*(i%2));
  for (std::size_t i=0;i<m.records.size();++i) { const auto& r=m.records[i]; const auto pos=at+128*i;
    Num(b,pos,8,r.page_number); Put(b,pos+8,r.allocation_uuid); Put(b,pos+24,r.page_uuid);
    Put(b,pos+40,r.owner_uuid); Put(b,pos+56,r.creator_transaction_uuid); Num(b,pos+72,8,r.creator_local_transaction_id);
    Num(b,pos+80,8,r.page_generation); Num(b,pos+88,8,r.reuse_horizon); Num(b,pos+96,4,r.page_type);
    Put(b,pos+100,r.creator_operation_uuid); }
  Seal(b); return b;
}
p::NativeAllocationMap Example(unsigned profile=0) {
  const auto& size = d::kCanonicalFilespacePageProfiles[profile];
  p::NativeAllocationMap m;
  m.header = {size.page_size_bytes,3,Id(1),Id(2),Id(10),1,4,0,size.uuid};
  m.object_uuid=Id(20); m.map_generation=5; m.capacity_generation=6; m.total_pages=11;
  m.creator_transaction_uuid=Id(31); m.creator_local_transaction_id=10;
  m.states={S::allocated,S::allocated,S::free,S::reserved,S::allocated,S::reusable_pending_mga,
            S::reusable_free,S::compacting,S::quarantined,S::preallocated,S::quarantined};
  for (unsigned i=0;i<m.states.size();++i) {
    if (m.states[i]==S::free || i==8) continue;
    p::NativeAllocationRecord r{i,Id(40+i),Id(60+i),Id(80+i),Id(30),8,3,0,6};
    if (i==0) { r.page_uuid=Id(3); r.page_generation=7; r.page_type=2; r.owner_uuid=Id(2); }
    if (i==1) { r.page_uuid=Id(10); r.page_generation=4; r.page_type=3; r.owner_uuid=Id(20); }
    if (i==3 || i==9) { r.page_uuid={}; r.page_generation=0; }
    if (i==5 || i==6) r.reuse_horizon=9;
    m.records.push_back(r);
  }
  return m;
}
void Empty(const p::NativeAllocationMapResult& r) { Check(!r.ok() && !r.map && r.bytes.empty(), "no partial map failure"); }
void Empty(const p::NativeAllocationChainResult& r) {
  Check(!r.ok() && r.pages.empty() && r.retained_image_bytes==0 &&
        std::all_of(r.state_counts.begin(),r.state_counts.end(),[](u64 n){return n==0;}), "no partial chain failure");
}
template<class Map> p::NativeAllocationMapConstView ConstMap(const Map& m){
  return {m.header,m.object_uuid,m.map_generation,m.capacity_generation,m.total_pages,m.first_page,
    m.creator_transaction_uuid,m.creator_local_transaction_id,m.next,m.next_sha256,m.states,m.records,m.creator_operation_uuid};
}
void BoundedEncode(const p::NativeAllocationMap& m,const Bytes& expected,E error=E::none){
  const auto view=ConstMap(m);Bytes backing(expected.size()+3,0xa5);
  const auto result=DenyCodecAllocation([&]{return p::EncodeNativeAllocationMapInto(view,std::span(backing).subspan(1));});
  Check(result.error==error,"bounded encoder shares exact owning validation");
  if(error==E::none)Check(result.ok()&&result.bytes.data()==backing.data()+1&&result.bytes.size()==expected.size()&&
    std::equal(result.bytes.begin(),result.bytes.end(),expected.begin(),expected.end())&&
    backing.front()==0xa5&&backing[backing.size()-2]==0xa5&&backing.back()==0xa5,
    "unaligned bounded encoding equals independent full image and preserves output suffix");
  else Check(!result.ok()&&result.bytes.empty()&&std::all_of(backing.begin(),backing.end(),[](byte b){return b==0xa5;}),
    "invalid input neither exposes an image nor touches backing");
}
void Codecs() {
  for (unsigned profile=0;profile<5;++profile) {
    const auto m=Example(profile); const auto expected=Oracle(m); const auto encoded=p::EncodeNativeAllocationMap(m);
    Check(encoded.ok() && encoded.bytes==expected,"complete independent allocation image");
    BoundedEncode(m,expected);
    const auto decoded=p::DecodeNativeAllocationMap(expected);
    Check(decoded.ok() && decoded.map->states==m.states && decoded.map->records==m.records,
          "all allocation states and exact binary ownership records");
    Check(p::EncodeNativeAllocationMap(*decoded.map).bytes==expected,"decoded metadata exact re-encoding");
    for(const u64 number:{u64{1},m.creator_local_transaction_id,m.creator_local_transaction_id+1,std::numeric_limits<u64>::max()}){
      auto overlapping=m;overlapping.records.front().creator_local_transaction_id=number;
      const auto image=Oracle(overlapping);const auto encoded_overlap=p::EncodeNativeAllocationMap(overlapping);
      const auto decoded_overlap=p::DecodeNativeAllocationMap(image);
      Check(encoded_overlap.ok()&&encoded_overlap.bytes==image&&decoded_overlap.ok()&&
        decoded_overlap.map->records==overlapping.records,"creator start numbers do not impose publication order");
    }
  }
  const auto good=Example(); const auto bytes=Oracle(good);
  for (unsigned mutation=0;mutation<25;++mutation) {
    auto m=good;
    if(mutation==0)m.object_uuid={}; if(mutation==1)m.creator_transaction_uuid.bytes[6]=0x41;
    if(mutation==2)m.map_generation=0; if(mutation==3)m.capacity_generation=0;
    if(mutation==4)m.first_page=std::numeric_limits<u64>::max(); if(mutation==5)m.total_pages=0;
    if(mutation==6)m.states[2]=static_cast<S>(8); if(mutation==7)m.records[0].allocation_uuid={};
    if(mutation==8)m.records[0].owner_uuid={}; if(mutation==9)m.records[0].creator_transaction_uuid={};
    if(mutation==10)m.records[0].page_uuid={}; if(mutation==11)m.records[0].page_generation=0;
    if(mutation==12)m.records[0].page_type=0; if(mutation==13)m.records[0].page_type=0xdead;
    if(mutation==14)m.records[0].page_number=1; if(mutation==15)m.records[0].page_number=11;
    if(mutation==16)m.records.erase(m.records.begin()); if(mutation==17)m.states[0]=S::free;
    if(mutation==18)m.records[4].reuse_horizon=0; if(mutation==19)m.records[0].reuse_horizon=1;
    if(mutation==20)m.next=d::NativePageReference{Id(2),2,1,good.header.page_size_profile_uuid};
    if(mutation==21)m.next_sha256[0]=1; if(mutation==22)m.total_pages=12;
    if(mutation==23)m.records[0].page_uuid.bytes[8]=0; if(mutation==24)m.records[0].creator_local_transaction_id=0;
    const auto encoded=p::EncodeNativeAllocationMap(m);Empty(encoded);BoundedEncode(m,bytes,encoded.error);
    Empty(p::DecodeNativeAllocationMap(Oracle(m)));
  }
  for (std::size_t at : {128u,136u,138u,140u,304u,308u,344u,383u,389u,390u,391u,492u,8191u}) {
    auto bad=bytes; bad[at]^=0x80; Seal(bad); Empty(p::DecodeNativeAllocationMap(bad));
  }
  for (std::size_t n : {0u,127u,383u,8191u,8193u}) { auto bad=bytes;bad.resize(n);Empty(p::DecodeNativeAllocationMap(bad)); }
  for (std::size_t at=0;at<bytes.size();++at) { auto bad=bytes;bad[at]^=1;Empty(p::DecodeNativeAllocationMap(bad)); }
  for (unsigned mode=0;mode<2;++mode) {
    fail_hash=true;const auto r=mode?p::DecodeNativeAllocationMap(bytes):p::EncodeNativeAllocationMap(good);
    Check(!fail_hash && r.error==E::hash_failure,"hash failure surfaced");Empty(r);
    bool succeeded=false;
    for(long budget=0;budget<100;++budget) {
      allocation_budget=budget;const auto result=mode?p::DecodeNativeAllocationMap(bytes):p::EncodeNativeAllocationMap(good);allocation_budget=-1;
      if(result.ok()){succeeded=true;break;} Empty(result); Check(result.error==E::resource_exhausted,"allocation failure classified");
    }
    Check(succeeded,"all allocation failure positions passed");
  }
}
template<class T> void OperationOwned(T& value, byte id=100) {
  value.creator_transaction_uuid={};value.creator_local_transaction_id=0;value.creator_operation_uuid=Id(id);
}
void OperationCodecs() {
  for(unsigned profile=0;profile<5;++profile) {
    // Every combination of original record owners, independently of the map
    // creator. This includes reserved/uninitialized and retained/reuse states.
    const auto original=Example(profile);
    for(unsigned map_owner=0;map_owner<2;++map_owner)
      for(unsigned mask=0;mask<(1u<<original.records.size());++mask) {
        auto m=original;if(map_owner)OperationOwned(m);
        for(unsigned i=0;i<m.records.size();++i)if(mask&(1u<<i))OperationOwned(m.records[i],110+i);
        const auto expected=Oracle(m);const auto encoded=p::EncodeNativeAllocationMap(m);
        Check(encoded.ok()&&encoded.bytes==expected,"independent mixed-owner image");
        BoundedEncode(m,expected);
        Check(expected[135]==((mask||map_owner)?'2':'1')&&expected[136]==((mask||map_owner)?2:1),
              "canonical version selected by actual owner presence");
        const auto decoded=p::DecodeNativeAllocationMap(expected);
        Check(decoded.ok()&&decoded.map->records==m.records&&
              decoded.map->creator_transaction_uuid==m.creator_transaction_uuid&&
              decoded.map->creator_local_transaction_id==m.creator_local_transaction_id&&
              decoded.map->creator_operation_uuid==m.creator_operation_uuid,"exclusive binary owners preserved");
        Check(p::EncodeNativeAllocationMap(*decoded.map).bytes==expected,"operation lineage exact re-encoding");
      }
    Uuid nil{},v7=Id(101),v4=v7,bad_variant=v7;v4.bytes[6]=0x41;bad_variant.bytes[8]=0x09;
    const std::array<Uuid,4> ids{nil,v7,v4,bad_variant};
    const std::array<u64,3> numbers{0,1,std::numeric_limits<u64>::max()};
    for(unsigned level=0;level<2;++level)for(unsigned tx=0;tx<4;++tx)
      for(unsigned op=0;op<4;++op)for(auto number:numbers) {
        auto m=original;OperationOwned(m);for(auto& r:m.records)OperationOwned(r);
        if(level==0){m.creator_transaction_uuid=ids[tx];m.creator_operation_uuid=ids[op];m.creator_local_transaction_id=number;}
        else {auto& r=m.records[0];r.creator_transaction_uuid=ids[tx];r.creator_operation_uuid=ids[op];r.creator_local_transaction_id=number;}
        const bool valid=(tx==1&&op==0&&number!=0)||(tx==0&&op==1&&number==0);
        const auto encoded=p::EncodeNativeAllocationMap(m),decoded=p::DecodeNativeAllocationMap(Oracle(m));
        Check(encoded.ok()==valid&&decoded.ok()==valid,"complete owner identity and number truth table");
        BoundedEncode(m,Oracle(m),encoded.error);
        if(!valid){Empty(encoded);Empty(decoded);}
      }
    auto m=original;OperationOwned(m);for(auto& r:m.records)OperationOwned(r);
    const auto bytes=Oracle(m);
    const auto records_at=(384+(m.states.size()+1)/2+7)&~std::size_t(7);
    for(std::size_t at=360;at<384;++at){auto bad=bytes;bad[at]=1;Seal(bad);Empty(p::DecodeNativeAllocationMap(bad));}
    for(unsigned i=0;i<m.records.size();++i)for(unsigned offset=116;offset<128;++offset){
      auto bad=bytes;bad[records_at+128*i+offset]=1;Seal(bad);Empty(p::DecodeNativeAllocationMap(bad));}
    for(unsigned mutation=0;mutation<5;++mutation){auto bad=bytes;
      if(mutation==0)bad[135]='1';if(mutation==1)Num(bad,136,2,1);
      if(mutation==2){bad[135]='1';Num(bad,136,2,1);}
      if(mutation==3)Num(bad,136,2,3);
      if(mutation==4){bad=Oracle(original);bad[135]='2';Num(bad,136,2,2);}
      Seal(bad);Empty(p::DecodeNativeAllocationMap(bad));
    }
    // Mixed operation/transaction lineage does not invent commit ordering.
    auto overlapping=original;OperationOwned(overlapping.records.back());
    for(const u64 number:{u64{1},overlapping.creator_local_transaction_id,overlapping.creator_local_transaction_id+1,std::numeric_limits<u64>::max()}){
      overlapping.records.front().creator_local_transaction_id=number;
      const auto image=Oracle(overlapping);const auto encoded_overlap=p::EncodeNativeAllocationMap(overlapping);
      const auto decoded_overlap=p::DecodeNativeAllocationMap(image);
      Check(encoded_overlap.ok()&&encoded_overlap.bytes==image&&decoded_overlap.ok()&&
        decoded_overlap.map->records==overlapping.records,"mixed lineage retains exact independent creator numbers");
    }
    for(unsigned mode=0;mode<2;++mode){
      fail_hash=true;const auto r=mode?p::DecodeNativeAllocationMap(bytes):p::EncodeNativeAllocationMap(m);
      Check(!fail_hash&&r.error==E::hash_failure,"operation image hash failure");Empty(r);
      bool complete=false;
      for(long budget=0;budget<100;++budget){allocation_budget=budget;
        const auto result=mode?p::DecodeNativeAllocationMap(bytes):p::EncodeNativeAllocationMap(m);allocation_budget=-1;
        if(result.ok()){complete=true;break;}Empty(result);Check(result.error==E::resource_exhausted,"operation image allocation failure");}
      Check(complete,"operation image allocation sweep complete");
    }
  }
}
void BorrowedCodecs() {
  for(unsigned profile=0;profile<5;++profile)for(unsigned lineage=0;lineage<3;++lineage){
    auto expected=Example(profile);
    if(lineage)OperationOwned(expected);
    if(lineage==2)for(auto& r:expected.records)OperationOwned(r);
    auto bytes=Oracle(expected);
    std::vector<S> states(expected.states.size());
    std::vector<p::NativeAllocationRecord> records(expected.records.size());
    observed_allocations=0;count_allocations=true;allocation_budget=0;
    const auto value=p::DecodeNativeAllocationMapInto(bytes,states,records);
    const auto remaining=allocation_budget;allocation_budget=-1;count_allocations=false;
    Check(value.ok()&&remaining==0&&observed_allocations==0,"caller-backed decoder has no hidden image/metadata allocation");
    const auto& m=*value.map;
    auto input=ConstMap(m);Bytes encoded(bytes.size());
    auto output=DenyCodecAllocation([&]{return p::EncodeNativeAllocationMapInto(input,encoded);});
    Check(output.ok()&&encoded==bytes,"grant-shaped borrowed decode-to-encode retains exact original bytes");
    for(std::size_t n:{std::size_t{0},std::size_t{1},bytes.size()-1}){
      std::fill(encoded.begin(),encoded.end(),0xa5);
      output=DenyCodecAllocation([&]{return p::EncodeNativeAllocationMapInto(input,std::span(encoded).first(n));});
      Check(output.error==E::resource_exhausted&&output.bytes.empty()&&
        std::all_of(encoded.begin(),encoded.end(),[](byte b){return b==0xa5;}),"short output cannot produce a partial image");
    }
    for(auto alias:{std::span<byte>{reinterpret_cast<byte*>(&input),sizeof(input)},
        std::span<byte>{reinterpret_cast<byte*>(states.data()),states.size()*sizeof(S)},
        std::span<byte>{reinterpret_cast<byte*>(records.data()),records.size()*sizeof(p::NativeAllocationRecord)}}){
      output=DenyCodecAllocation([&]{return p::EncodeNativeAllocationMapInto(input,alias);});
      Check(output.error==E::invalid_workspace&&output.bytes.empty(),"descriptor and both input arrays cannot alias output");
    }
    fail_hash=true;output=DenyCodecAllocation([&]{return p::EncodeNativeAllocationMapInto(input,encoded);});
    Check(!fail_hash&&output.error==E::hash_failure&&output.bytes.empty(),"bounded encoder hash allocation failure has no success prefix");
#ifdef SB_NATIVE_ALLOCATION_MEMORY_TESTS
    for(unsigned mode=2;mode<=5;++mode){method_fault=mode;
      output=DenyCodecAllocation([&]{return p::EncodeNativeAllocationMapInto(input,encoded);});
      Check(!method_fault&&output.error==E::hash_failure&&output.bytes.empty(),"all encoder digest failures have typed allocation-free refusal");}
#endif
    Check(p::EncodeNativeAllocationMapInto(input,encoded).ok()&&encoded==bytes,"exact encoding retry after all failures");
    Check(m.states.data()==states.data()&&m.records.data()==records.data()&&
      std::equal(m.states.begin(),m.states.end(),expected.states.begin(),expected.states.end())&&
      std::equal(m.records.begin(),m.records.end(),expected.records.begin(),expected.records.end()),"exact independent state and record arrays in supplied backing");
    Check(m.object_uuid==expected.object_uuid&&m.map_generation==expected.map_generation&&
      m.capacity_generation==expected.capacity_generation&&m.total_pages==expected.total_pages&&m.first_page==expected.first_page&&
      m.creator_transaction_uuid==expected.creator_transaction_uuid&&m.creator_operation_uuid==expected.creator_operation_uuid&&
      m.creator_local_transaction_id==expected.creator_local_transaction_id&&m.next==expected.next&&m.next_sha256==expected.next_sha256&&
      m.header.database_uuid==expected.header.database_uuid&&m.header.filespace_uuid==expected.header.filespace_uuid&&
      m.header.page_uuid==expected.header.page_uuid&&m.header.page_number==expected.header.page_number&&
      m.header.page_generation==expected.header.page_generation&&m.header.page_size_profile_uuid==expected.header.page_size_profile_uuid&&
      m.header.page_type==expected.header.page_type&&m.header.page_size_bytes==expected.header.page_size_bytes&&m.header.flags==expected.header.flags,
      "all fixed metadata retains binary identity and exact header fields");
    for(unsigned short_kind=0;short_kind<2;++short_kind){
      const auto r=p::DecodeNativeAllocationMapInto(bytes,std::span(states).first(states.size()-(short_kind==0)),
        std::span(records).first(records.size()-(short_kind==1)));
      Check(r.error==E::resource_exhausted&&!r.map,"one-element-short backing returns no usable prefix");
    }
    Bytes unaligned(1,0);unaligned.insert(unaligned.end(),bytes.begin(),bytes.end());
    Check(p::DecodeNativeAllocationMapInto(std::span<const byte>(unaligned).subspan(1),states,records).ok(),"unaligned input uses bytewise scalar decoding");
    auto alias=p::DecodeNativeAllocationMapInto(bytes,
      {reinterpret_cast<S*>(bytes.data()),expected.states.size()},records);
    Check(alias.error==E::invalid_range&&!alias.map&&bytes==Oracle(expected),"overlapping input/state buffer rejected before writing");
    alias=p::DecodeNativeAllocationMapInto(bytes,{reinterpret_cast<S*>(records.data()),1},records);
    Check(alias.error==E::invalid_range&&!alias.map,"overlapping metadata regions rejected before writing");
    fail_hash=true;auto hash_failure=DenyCodecAllocation([&]{return p::DecodeNativeAllocationMapInto(bytes,states,records);});
    Check(!fail_hash&&hash_failure.error==E::hash_failure&&!hash_failure.map,"borrowed decoder preserves actual hash failure");
#ifdef SB_NATIVE_ALLOCATION_MEMORY_TESTS
    for(unsigned mode=2;mode<=5;++mode){method_fault=mode;
      const auto failed=DenyCodecAllocation([&]{return p::DecodeNativeAllocationMapInto(bytes,states,records);});
      Check(!method_fault&&!failed.map&&failed.error==E::hash_failure,"all provider phases and short digest fail without diagnostic allocation");
    }
#endif
    for(std::size_t at:{128u,136u,138u,140u,304u,308u,383u}){
      auto bad=bytes;bad[at]^=0x80;Seal(bad);
      const auto owned=p::DecodeNativeAllocationMap(bad);
      const auto borrowed=p::DecodeNativeAllocationMapInto(bad,states,records);
      Check(!borrowed.map&&borrowed.error==owned.error,"malformed resealed fields share exact decoder refusal");
    }
    Check(p::DecodeNativeAllocationMapInto(bytes,states,records).ok(),"legitimate retry after all failures");
    std::fill(bytes.begin(),bytes.end(),0);
    Check(std::equal(m.records.begin(),m.records.end(),expected.records.begin(),expected.records.end()),"decoded records do not borrow input image bytes");
  }
  for(unsigned profile=0;profile<5;++profile){auto expected=Example(profile);
    expected.records.clear();expected.states.assign(2*(expected.header.page_size_bytes-384),S::free);expected.total_pages=expected.states.size();
    const auto bytes=Oracle(expected);std::vector<S> states(expected.states.size());
    const auto result=p::DecodeNativeAllocationMapInto(bytes,states,{});
    Check(result.ok()&&result.map->states.size()==expected.states.size()&&result.map->records.empty()&&
      std::all_of(states.begin(),states.end(),[](S s){return s==S::free;}),"maximum packed bitmap has exact decoded backing and no fabricated records");
    BoundedEncode(expected,bytes);
    expected.states.push_back(S::free);++expected.total_pages;
    BoundedEncode(expected,bytes,E::invalid_range);
  }
  const auto expected=Example();const auto bytes=Oracle(expected);
  std::vector<S> states(expected.states.size());std::vector<p::NativeAllocationRecord> records(expected.records.size());
  for(std::size_t at=0;at<bytes.size();++at){auto corrupt=bytes;corrupt[at]^=1;
    const auto r=p::DecodeNativeAllocationMapInto(corrupt,states,records);
    Check(!r.ok()&&!r.map,"every corrupted byte rejected without decoded prefix");
  }
  for(std::size_t size:{0u,127u,383u,8191u,8193u}){auto bad=bytes;bad.resize(size);
    const auto r=p::DecodeNativeAllocationMapInto(bad,states,records);
    Check(!r.map&&r.error==p::DecodeNativeAllocationMap(bad).error,"all malformed image sizes share owning errors");
  }
}
#ifdef SB_NATIVE_ALLOCATION_MEMORY_TESTS
namespace db=scratchbird::storage::database;
namespace m=scratchbird::core::memory;
using ME=db::NativeAllocationMapMemoryError;
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
struct MemoryFile {
  std::filesystem::path directory,path;
  d::FileDevice device;
  p::NativeAllocationMap value;
  Bytes bytes;
  d::NativeCommonPageHeaderBinding expected;
  explicit MemoryFile(unsigned profile):value(Example(profile)),bytes(Oracle(value)){
    char name[]="/tmp/sb-allocation-memory-XXXXXX";const auto* made=mkdtemp(name);if(!made)throw std::runtime_error("mkdtemp");
    directory=made;path=directory/"native.bin";
    Check(device.Open(path.string(),d::FileOpenMode::create_new).ok(),"actual allocation source");
    expected={{value.header.database_uuid,value.header.filespace_uuid,value.header.page_size_profile_uuid},
      value.header.page_number,value.header.page_generation,3,value.header.page_uuid};
    Bootstrap();Store(bytes);Check(device.Sync().ok()&&device.Close().ok()&&
      device.Open(path.string(),d::FileOpenMode::open_existing).ok(),"cold reopen independent allocation image");
  }
  ~MemoryFile(){(void)device.Close();std::error_code ec;std::filesystem::remove_all(directory,ec);}
  void Bootstrap(u32 flags=0,Uuid database=Id(1)){
    Bytes b(4096,0);std::copy_n("SBFP",4,b.begin());Num(b,4,2,1);Num(b,6,2,4096);
    Num(b,8,4,value.header.page_size_bytes);Num(b,12,4,flags);Put(b,16,database);
    Put(b,32,value.header.filespace_uuid);Put(b,48,value.header.page_size_profile_uuid);
    Num(b,64,4,1);Num(b,68,2,1);Num(b,70,2,1);Put(b,72,d::kNativeBootstrapIntegrityProfile);
    if(flags&1)Put(b,88,Id(71));std::array<byte,32> digest{};
    Check(SHA256(b.data(),104,digest.data())!=nullptr,"independent bootstrap digest");
    std::copy(digest.begin(),digest.end(),b.begin()+104);
    Check(device.WriteAt(0,b.data(),b.size()).ok()&&device.Sync().ok(),"actual independent bootstrap");
  }
  void Store(const Bytes& image){Check(device.WriteAt(value.header.page_number*u64(value.header.page_size_bytes),image.data(),image.size()).ok()&&device.Sync().ok(),"actual independent map");}
  auto Read(MemoryFixture& f){reads=hashes=0;return db::ReadNativeAllocationMapWithMemoryFromOpenDevice(device,expected,value.object_uuid,f.memory,f.binding);}
};
void NoMap(const db::NativeAllocationMapMemoryResult& r){Check(!r.ok()&&!r.map&&r.image.empty()&&!r.arena,"refusal exposes no image metadata or owner prefix");}
void MemoryEncodingTests(){
  for(unsigned profile=0;profile<5;++profile){
    MemoryFile file(profile);
    const auto capacity=db::NativeAllocationMapWorkspaceBytes(file.value.header.page_size_profile_uuid);
    MemoryFixture source(capacity),output(file.value.header.page_size_bytes);
    auto read=file.Read(source);Check(read.ok(),"actual admitted source map for encoding");
    auto granted=output.memory.AllocatePage(file.value.header.page_size_profile_uuid);
    Check(granted.ok(),"actual admitted destination page before device guard");
    // A staged map can have operation-owned metadata and transaction-owned
    // reservations. This fixture proves bytes, not selected allocation authority.
    auto expected=file.value;OperationOwned(expected);
    auto reservation=std::find_if(expected.records.begin(),expected.records.end(),[](const auto& r){return r.page_number==3;});
    Check(reservation!=expected.records.end(),"existing reservation test slot");
    reservation->page_uuid=Id(200);reservation->page_generation=1;
    OperationOwned(*read.map);
    for(auto& r:read.map->records)if(r.page_number==3){r.page_uuid=Id(200);r.page_generation=1;}
    const auto oracle=Oracle(expected);const auto input=ConstMap(*read.map);
    {auto guard=file.device.AcquireOperationGuard();
      observation_device_mutex=guard.mutex();memory_probes=locked_memory_probes=0;
      allocation_budget=0;const auto encoded=p::EncodeNativeAllocationMapInto(input,{granted.buffer.data(),granted.buffer.size()});
      const auto remaining=allocation_budget;allocation_budget=-1;observation_device_mutex=nullptr;
      Check(encoded.ok()&&remaining==0&&!memory_probes&&!locked_memory_probes&&
        std::equal(encoded.bytes.begin(),encoded.bytes.end(),oracle.begin(),oracle.end()),
        "canonical map encoding beneath actual guard uses only retained admitted bytes");
      const auto write=file.device.WriteAt(expected.header.page_number*u64{expected.header.page_size_bytes},encoded.bytes.data(),encoded.bytes.size());
      Check(write.ok()&&write.bytes_transferred==encoded.bytes.size()&&file.device.Sync().ok(),"persist exact staged allocation image");
    }
    read={};Check(granted.buffer.Reset().ok(),"release destination after guard");
    output.memory={};output.Empty();
    Check(file.device.Close().ok()&&file.device.Open(file.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"reopen persisted staged map read-only");
    auto reopened=file.Read(source);Check(reopened.ok()&&reopened.image.size()==oracle.size()&&
      std::equal(reopened.image.begin(),reopened.image.end(),oracle.begin(),oracle.end())&&
      std::equal(reopened.map->records.begin(),reopened.map->records.end(),expected.records.begin(),expected.records.end())&&
      reopened.map->creator_operation_uuid==expected.creator_operation_uuid,
      "exact binary transaction reservation and operation map lineage survive physical reopen");
    reopened={};source.memory={};source.Empty();
  }
}
void MemoryTests(){
  for(unsigned profile=0;profile<5;++profile){MemoryFile file(profile);
    const auto capacity=db::NativeAllocationMapWorkspaceBytes(file.value.header.page_size_profile_uuid);
    Check(capacity==file.bytes.size()+2*(file.bytes.size()-384)*sizeof(S)+
      ((file.bytes.size()-384)/128)*sizeof(p::NativeAllocationRecord)+2*(alignof(std::max_align_t)-1),"independent native backing size formula");
    {
      MemoryFixture f(capacity);
      {auto guard=file.device.AcquireOperationGuard();allocation_device_mutex=guard.mutex();}
      allocation_lock_free=false;auto read=file.Read(f);
      Check(read.ok()&&allocation_lock_free,"real metadata backing allocated outside device guard");
      Check(last_read_buffer==read.image.data()&&read.image.size()==file.bytes.size()&&
        std::equal(read.image.begin(),read.image.end(),file.bytes.begin(),file.bytes.end()),"actual read destination is returned charged image");
      const auto begin=reinterpret_cast<std::uintptr_t>(read.image.data());
      const auto inside=[&](const void* ptr,usize bytes){auto n=reinterpret_cast<std::uintptr_t>(ptr);return n>=begin&&n-begin<=capacity&&bytes<=capacity-(n-begin);};
      Check(inside(read.map->states.data(),read.map->states.size_bytes())&&inside(read.map->records.data(),read.map->records.size_bytes())&&
        std::equal(read.map->states.begin(),read.map->states.end(),file.value.states.begin(),file.value.states.end())&&
        std::equal(read.map->records.begin(),read.map->records.end(),file.value.records.begin(),file.value.records.end()),"actual metadata resides in same charged block with exact binary records");
      Check(f.manager.Snapshot().current_bytes==capacity&&f.memory.Snapshot().allocated_bytes==capacity&&
        f.ledger.Snapshot().current_bytes==capacity&&read.arena.Snapshot().retained_bytes==capacity,"actual physical parent and arena charges agree");
      auto full=file.Read(f);NoMap(full);Check(full.error==ME::memory_allocation_failure&&reads==0,"simultaneous live image prevents uncharged second reader");
      const auto revoked=f.ledger.CleanupOwner(f.binding.owner_uuid.bytes);Check(revoked.retained_bytes==capacity,"revocation retains actual image and metadata");
      auto denied=file.Read(f);NoMap(denied);Check(denied.error==ME::memory_binding_failure&&reads==0,"revoked grant prevents new source reads");
      f.memory={};Check(f.manager.Snapshot().current_bytes==capacity,"returned metadata owner survives caller workspace");
      std::thread worker([retained=std::move(read)]()mutable{allocation_budget=0;retained={};if(allocation_budget!=0)std::abort();allocation_budget=-1;});worker.join();f.Empty();
    }
    MemoryFixture valid(capacity);const auto no_writes=writes,no_syncs=syncs;
    const auto original_binding=file.expected;
    for(unsigned invalid=0;invalid<7;++invalid){
      if(invalid==0)file.expected.page_number=0;
      if(invalid==1)file.expected.page_number=std::numeric_limits<u64>::max();
      if(invalid==2)file.expected.page_generation=0;
      if(invalid==3)file.expected.page_type=0x30e;
      if(invalid==4)file.expected.filespace.filespace_uuid={};
      if(invalid==5)file.expected.filespace.page_size_profile_uuid=Id(99);
      if(invalid==6)file.expected.page_uuid=Uuid{};
      auto r=file.Read(valid);NoMap(r);Check(r.error==ME::invalid_request&&reads==0&&
        !valid.memory.Snapshot().allocation_count,"invalid typed page binding refuses before physical admission");
      file.expected=original_binding;
    }
    for(unsigned field=0;field<4;++field){auto wrong=valid.binding;
      const std::array<Uuid*,4> fields{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};*fields[field]=Id(99);
      reads=0;auto denied=db::ReadNativeAllocationMapWithMemoryFromOpenDevice(file.device,file.expected,file.value.object_uuid,valid.memory,wrong);
      NoMap(denied);Check(denied.error==ME::memory_binding_failure&&!reads&&!valid.manager.Snapshot().current_bytes,"exact binary memory identity before allocation or I/O");}
    MemoryFixture small(capacity-1);auto short_grant=file.Read(small);NoMap(short_grant);
    Check(short_grant.error==ME::memory_allocation_failure&&!reads&&!small.manager.Snapshot().current_bytes,"one byte short cannot obtain image or metadata");small.memory={};small.Empty();
    file.expected.page_generation++;{auto r=file.Read(valid);NoMap(r);Check(r.error==ME::header_failure,"stale generation binds actual source");}file.expected.page_generation--;
    auto object=db::ReadNativeAllocationMapWithMemoryFromOpenDevice(file.device,file.expected,Id(99),valid.memory,valid.binding);NoMap(object);Check(object.error==ME::object_mismatch,"exact map object identity");
    for(unsigned n=1;n<=2;++n){fail_read=n;auto r=file.Read(valid);fail_read=0;NoMap(r);
      Check(reads==n&&!valid.manager.Snapshot().current_bytes,"every real read failure releases admitted payload");}
    short_read=2;{auto r=file.Read(valid);Check(r.ok()&&reads==3,"legal short physical read completes");}short_read=0;
    short_read=2;eof_read=3;{auto r=file.Read(valid);NoMap(r);Check(r.error==ME::io_failure&&r.page_bytes_read==file.bytes.size()-1,"partial then EOF retains actual read progress only");}short_read=eof_read=0;
    for(unsigned fault=1;fault<=5;++fault){method_fault=fault;auto r=file.Read(valid);NoMap(r);Check(method_fault==0,"each real digest-provider failure reached");}
    fail_hash=true;{auto r=file.Read(valid);NoMap(r);Check(!fail_hash&&r.map_error==E::hash_failure,"actual digest context failure typed");}
    Check(writes==no_writes&&syncs==no_syncs,"reader and refusal paths never write or sync source");
    auto corrupt=file.bytes;corrupt.back()^=1;file.Store(corrupt);
    {auto guard=file.device.AcquireOperationGuard();deallocation_device_mutex=guard.mutex();}deallocation_lock_free=false;
    {auto r=file.Read(valid);NoMap(r);Check(r.error==ME::map_failure&&deallocation_lock_free,"failed image cleanup occurs after releasing device guard");}file.Store(file.bytes);
    file.Bootstrap(0,Id(99));{auto r=file.Read(valid);NoMap(r);Check(r.error==ME::bootstrap_failure,"actual bootstrap identity mismatch");}file.Bootstrap(1);
    {auto r=file.Read(valid);Check(r.ok(),"allocation metadata remains cleartext in encrypted filespace");}file.Bootstrap();
    for(unsigned version=0;version<3;++version){auto variant=file.value;
      if(version==0)OperationOwned(variant);
      if(version==1){OperationOwned(variant);for(auto& r:variant.records)OperationOwned(r);}
      if(version==2){variant.records.clear();variant.states.assign(2*(file.bytes.size()-384),S::free);variant.total_pages=variant.states.size();}
      file.Store(Oracle(variant));
      {auto r=file.Read(valid);Check(r.ok()&&std::equal(r.map->states.begin(),r.map->states.end(),variant.states.begin(),variant.states.end())&&
        std::equal(r.map->records.begin(),r.map->records.end(),variant.records.begin(),variant.records.end()),"actual governed metadata supports mixed operation lineage and maximum bitmap");}
    }
    file.Store(file.bytes);
    if(!profile){unsigned long sites=0;
      {observed_allocations=0;count_allocations=true;auto r=file.Read(valid);count_allocations=false;sites=observed_allocations;Check(r.ok(),"measure full admitted reader allocation sites");}
      for(unsigned long n=0;n<sites;++n){allocation_budget=n;auto r=file.Read(valid);allocation_budget=-1;
        if(r.ok())Check(std::equal(r.map->records.begin(),r.map->records.end(),file.value.records.begin(),file.value.records.end()),"optional telemetry loss cannot change decoded result");
        else NoMap(r);
        r={};Check(!valid.manager.Snapshot().current_bytes&&!valid.memory.Snapshot().allocated_bytes,"every allocation fault retains zero physical payload after cleanup");
        {auto retry=file.Read(valid);Check(retry.ok(),"same-owner retry after each allocation failure");}
      }
      Check(sites>0,"allocation sweep executed");std::cout<<"governed allocation metadata faults="<<sites<<'\n';
    }
    Check(file.device.Close().ok(),"close source");auto closed=file.Read(valid);NoMap(closed);
    Check(closed.error==ME::bootstrap_failure&&closed.bootstrap_error==d::FilespaceBootstrapError::device_not_open,"reader never reopens closed source");
    valid.memory={};valid.Empty();
  }
}
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
void ChainMemoryChecks(d::FileDevice& device,const d::FilespaceBootstrapBinding& binding,
    const d::FilespaceRootReference& root,const Bytes& zero,const Bytes& head,bool deep){
  using C=p::NativeAllocationChainReadContext;
  const auto digest=Hash(head);const std::size_t capacity=6*head.size()+65536;
  const auto old_writes=writes,old_syncs=syncs;
  unsigned long fixture_writes=0,fixture_syncs=0;
  const auto source_bytes=[&]{const auto extent=device.Size();Check(extent.ok(),"observe whole chain fixture");
    Bytes bytes(extent.size_bytes);const auto io=device.ReadAt(0,bytes.data(),bytes.size());
    Check(io.ok()&&io.bytes_transferred==bytes.size(),"independent whole-file no-effect oracle");return bytes;};
  const auto original=source_bytes();
  for(const auto context:{C::bootstrap,C::selected,C::historical}){
    const auto* selected=context==C::bootstrap?nullptr:&root;
    const auto* sha=context==C::historical?&digest:nullptr;
    const std::span<const byte> historical=context==C::historical?std::span<const byte>(zero):std::span<const byte>{};
    const u64 allowance=2*head.size()+(context==C::historical?zero.size()+4096:0);
    const auto expected=context==C::bootstrap?p::ReadNativeAllocationChainFromOpenDevice(device,binding,allowance):
      context==C::selected?p::ReadNativeAllocationChainAtRootFromOpenDevice(device,binding,root,allowance):
      p::ReadNativeAllocationChainAtHistoricalRootFromOpenDevice(device,binding,root,digest,zero,allowance);
    Bytes backing(capacity);d::FileDevice::ReadLatencyBatch batch(device);
    const auto direct=[&](std::span<byte> region,u64 limit,bool deny=true){
      if(deny)allocation_budget=0;
      auto r=p::ReadNativeAllocationChainInto(device,binding,limit,context,selected,sha,historical,batch,region);
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
        composed=p::detail::ReadNativeAllocationChainBacked(device,binding,allowance,
          context,selected,sha,historical,batch,resource);
      }
      const bool untouched=allocation_budget==0;allocation_budget=-1;
      Check(untouched&&resource.used()==used,"complete resource composition uses same bounded payload under enclosing fence");
      SameChain(expected,composed.chain,backing);
      auto refused=p::detail::ReadNativeAllocationChainBacked(device,binding,allowance,
        context,selected,sha,historical,batch,*std::pmr::null_memory_resource());
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
      alias(binding);alias(device);alias(batch);if(selected)alias(root);if(sha)alias(digest);
      if(!historical.empty()){const auto r=direct({const_cast<byte*>(zero.data()),zero.size()},allowance);
        Check(r.chain.error==E::invalid_workspace&&!r.physical_bytes_read,"historical source bytes cannot be overwritten");Empty(r.chain);}
      d::FileDevice foreign;d::FileDevice::ReadLatencyBatch wrong(foreign);
      auto mismatch=p::ReadNativeAllocationChainInto(device,binding,allowance,context,selected,sha,historical,wrong,backing);
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
      return db::ReadNativeAllocationChainWithMemoryFromOpenDevice(device,binding,limits,f.memory,owner,
        context,selected,sha,historical);
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

void HistoricalChain(d::FileDevice& device, const d::FilespacePageZero& zero,
                     const Bytes& zero_bytes, const Bytes& head, const Bytes& tail) {
  const auto& b = zero.bootstrap;
  const d::FilespaceBootstrapBinding binding{b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid};
  const auto root = zero.roots.front();
  const auto digest = Hash(head);
  const u64 size = b.page_size_bytes, original_length = zero.total_pages * size;
  const u64 limit = 3 * size + 4096;
  const auto read = [&](u64 budget) {
    return p::ReadNativeAllocationChainAtHistoricalRootFromOpenDevice(
        device,binding,root,digest,zero_bytes,budget);
  };
  const auto image = [&] {
    const auto extent=device.Size();Check(extent.ok(),"observe fixture extent");
    Bytes bytes(extent.size_bytes);
    const auto io=device.ReadAt(0,bytes.data(),bytes.size());
    Check(io.ok()&&io.bytes_transferred==bytes.size(),"read whole fixture for no-effect oracle");
    return bytes;
  };
  const auto original = image();
  const auto verify = [&](const p::NativeAllocationChainResult& result) {
    Check(result.ok()&&result.pages.size()==2&&result.retained_image_bytes==limit&&
          result.pages[0].bytes==head&&result.pages[1].bytes==tail&&result.state_counts[4]==1&&
          result.state_counts[7]==1,"historical image binds exact independent chain and allowance");
  };
  const auto initial_writes=writes,initial_syncs=syncs;
  reads=hashes=stats=0;observed_allocations=0;count_allocations=true;
  const auto good=read(limit);count_allocations=false;
  const auto read_count=reads,hash_count=hashes;
  const auto stat_count=stats;
#ifdef NATIVE_HISTORICAL_IO_FAULTS
  Check(stat_count>=2,"both actual size observations reached kernel");
#endif
  const auto allocation_count=observed_allocations;
  verify(good);
  for (const u64 budget : {u64{0},size-1,size+4095,limit-1}) {
    const auto r=read(budget);Empty(r);Check(r.error==E::resource_exhausted,"historical image ceiling exact refusal");
  }
  for (unsigned at=1;at<=read_count;++at) {
    reads=0;fail_read=at;const auto r=read(limit);fail_read=0;
    Empty(r);Check(r.error==E::io_failure,"historical read error retained");
  }
  for (unsigned at=1;at<=hash_count;++at) {
    hashes=0;fail_hash_at=at;const auto r=read(limit);fail_hash_at=0;
    Empty(r);Check(r.error==E::hash_failure,"historical hash-provider error retained");
  }
  for (unsigned at=1;at<=stat_count;++at) {
    stats=0;fail_stat=at;const auto r=read(limit);fail_stat=0;
    Empty(r);Check(r.error==E::io_failure,"historical size observation error retained");
  }
  for (unsigned long at=0;at<allocation_count;++at) {
    const auto lost=device.failed_io_latency_observations();
    allocation_budget=static_cast<long>(at);const auto r=read(limit);allocation_budget=-1;
    if(r.ok()) {
      // Device latency observation is explicitly optional; all chain-owned
      // allocations must fail closed. Do not silently accept arbitrary success.
      Check(device.failed_io_latency_observations()>lost,"only measured optional telemetry can lose an allocation");
      verify(r);
    } else {Empty(r);Check(r.error==E::resource_exhausted,"historical allocation error retained");}
  }
  Check(image()==original&&writes==initial_writes&&syncs==initial_syncs,
        "historical success and fault sweeps preserve every file byte without writes or syncs");
  for (unsigned mutation=0;mutation<8;++mutation) {
    auto old=zero;auto expected=digest;auto reference=root;auto expected_binding=binding;
    if(mutation==0)expected[0]^=1;
    if(mutation==1)expected={};
    if(mutation==2)reference.page_generation++;
    if(mutation==3)old.page_generation++;
    if(mutation==4)old.page_uuid=Id(232);
    if(mutation==5)expected_binding.database_uuid=Id(230);
    if(mutation==6)reference.object_uuid=Id(231);
    if(mutation==7)old.bootstrap.flags|=d::FilespaceBootstrapFlag::cluster_authority_required;
    const auto encoded=d::EncodeFilespacePageZero(old);Check(encoded.ok(),"individually valid historical negative fixture");
    const auto r=p::ReadNativeAllocationChainAtHistoricalRootFromOpenDevice(
        device,expected_binding,reference,expected,*encoded.bytes,limit);
    Empty(r);
    if(mutation==0)Check(r.error==E::invalid_integrity,"historical root whole-image digest mismatch");
    if(mutation==7)Check(r.error==E::cluster_requires_authority,"historical cluster flag preserves routing refusal");
  }
  // A retained page-zero image may predate selected allocations within the
  // same capacity. Like the current explicit-root overload, count the actual
  // hash-bound map, not the bootstrap's initial free/preallocation counters.
  auto stale_counters=zero;stale_counters.free_pages++;stale_counters.preallocated_pages=0;
  const auto stale_image=d::EncodeFilespacePageZero(stale_counters);
  Check(stale_image.ok(),"valid initial counters before selected allocations");
  verify(p::ReadNativeAllocationChainAtHistoricalRootFromOpenDevice(
      device,binding,root,digest,*stale_image.bytes,limit));
  for (u64 extra : {u64{1},size,2*size+1}) {
    std::filesystem::resize_file(device.path(),original_length+extra);
    verify(read(limit));
    Empty(p::ReadNativeAllocationChainFromOpenDevice(device,binding,2*size));
    Empty(p::ReadNativeAllocationChainAtRootFromOpenDevice(device,binding,root,2*size));
  }
  auto current=zero;current.total_pages+=2;current.page_generation++;
  current.root_set_generation++;current.free_pages+=2;
  const auto new_zero=d::EncodeFilespacePageZero(current);Check(new_zero.ok(),"new current capacity image");
  std::filesystem::resize_file(device.path(),current.total_pages*size);
  Check(device.WriteAt(0,new_zero.bytes->data(),new_zero.bytes->size()).ok()&&device.Sync().ok(),"persist changed page zero");
  Check(d::ReadFilespacePageZeroFromOpenDevice(device,&binding).ok(),"new page zero independently valid");
  verify(read(limit));
  Empty(p::ReadNativeAllocationChainAtRootFromOpenDevice(device,binding,root,2*size));
  current.bootstrap.flags|=d::FilespaceBootstrapFlag::cluster_authority_required;
  const auto cluster=d::EncodeFilespacePageZero(current);Check(cluster.ok(),"current cluster fixture");
  Check(device.WriteAt(0,cluster.bytes->data(),cluster.bytes->size()).ok(),"write actual cluster bootstrap");
  const auto routed=read(limit);Empty(routed);
  Check(routed.error==E::cluster_requires_authority,"actual cluster flag cannot be hidden by historical input");
  Check(device.WriteAt(0,zero_bytes.data(),zero_bytes.size()).ok(),"restore page zero");
  std::filesystem::resize_file(device.path(),original_length-1);
  const auto short_file=read(limit);Empty(short_file);
  Check(short_file.error==E::invalid_range,"complete historical extent must remain present");
  std::filesystem::resize_file(device.path(),original_length);
  reads=0;resize_at_read=2;resize_to=original_length+1;resized_during_read=false;
  const auto raced=read(limit);resize_at_read=0;
  Empty(raced);Check(resized_during_read&&raced.error==E::physical_extent_changed,"actual extent change during chain read rejected");
  std::filesystem::resize_file(device.path(),original_length);
  Check(image()==original,"historical tests restore exact real fixture");
  const auto path=device.path();
  {
    std::lock_guard lock(read_pause_mutex);pause_read=true;read_entered=false;release_read=false;
  }
  auto reading=std::async(std::launch::async,[&]{return read(limit);});
  {
    std::unique_lock lock(read_pause_mutex);
    if(!read_pause_cv.wait_for(lock,std::chrono::seconds(5),[]{return read_entered;})) {
      release_read=true;read_pause_cv.notify_all();
      Check(false,"historical reader reached retained device read");
    }
  }
  std::atomic<bool> close_returned=false;
  bool held=false;
  std::thread controller([&]{
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    held=!close_returned.load();
    std::lock_guard lock(read_pause_mutex);release_read=true;read_pause_cv.notify_all();
  });
  const auto closed=device.Close();close_returned=true;
  controller.join();const auto finished=reading.get();
  Check(held&&closed.ok(),"concurrent close waits for complete historical verification");verify(finished);
  Check(device.Open(path,d::FileOpenMode::open_existing).ok(),"reopen after retained-guard test");
  std::cout<<"historical profile="<<size<<" reads="<<read_count<<" hashes="<<hash_count<<" stats="<<stat_count<<" allocations="<<allocation_count<<'\n';
}
void RetainedChain() {
  for (unsigned profile=0;profile<5;++profile) for(unsigned ownership=0;ownership<4;++ownership) {
    Fixture fixture;auto full=Example(profile); full.states[2]=S::allocated;
    full.records.insert(full.records.begin()+2,{2,Id(42),Id(11),Id(20),Id(30),8,5,0,3});
    if(ownership==1||ownership==3)OperationOwned(full);
    if(ownership>=2)for(auto& r:full.records)if(ownership==3||r.page_number>=5)OperationOwned(r,110+r.page_number);
    auto head=full,tail=full; head.states.resize(5);
    head.records.erase(std::remove_if(head.records.begin(),head.records.end(),[](const auto& r){return r.page_number>=5;}),head.records.end());
    tail.first_page=5;tail.states.erase(tail.states.begin(),tail.states.begin()+5);
    tail.records.erase(std::remove_if(tail.records.begin(),tail.records.end(),[](const auto& r){return r.page_number<5;}),tail.records.end());
    tail.header.page_number=2;tail.header.page_generation=5;tail.header.page_uuid=Id(11);
    const auto tail_bytes=Oracle(tail);head.next=d::NativePageReference{Id(2),2,5,head.header.page_size_profile_uuid};
    head.next_sha256=Hash(tail_bytes); const auto head_bytes=Oracle(head);
    d::FilespacePageZero zero; auto& b=zero.bootstrap;
    b.database_uuid=Id(1);b.filespace_uuid=Id(2);b.page_size_profile_uuid=head.header.page_size_profile_uuid;
    b.page_size_bytes=head.header.page_size_bytes;b.checksum_profile_uuid=d::kNativeBootstrapIntegrityProfile;
    b.filespace_role=5;b.lifecycle_state=1;zero.page_uuid=Id(3);zero.creation_operation_uuid=Id(4);zero.writer_identity_uuid=Id(5);
    zero.page_generation=7;zero.root_set_generation=8;zero.total_pages=11;zero.free_pages=1;zero.preallocated_pages=1;
    zero.roots.push_back({3,3,Id(2),1,4,b.page_size_profile_uuid,Id(20)});
    const auto zero_bytes=d::EncodeFilespacePageZero(zero);Check(zero_bytes.ok(),"actual filespace metadata fixture");
    d::FileDevice device;const auto path=(fixture.root/"node").string();
    Check(device.Open(path,d::FileOpenMode::create_new).ok(),"own filespace fixture");
    const byte padding=0;
    const auto write=[&](u64 number,const Bytes& image){const auto io=device.WriteAt(number*b.page_size_bytes,image.data(),image.size());
      Check(io.ok()&&io.bytes_transferred==image.size()&&device.Sync().ok(),"persist actual fixture image");};
    Check(device.WriteAt(zero.total_pages*b.page_size_bytes-1,&padding,1).ok(),"allocate actual fixture length");
    write(0,*zero_bytes.bytes);write(1,head_bytes);write(2,tail_bytes);
    const d::FilespaceBootstrapBinding binding{Id(1),Id(2),b.page_size_profile_uuid};
    const auto read=[&](u64 budget){return p::ReadNativeAllocationChainFromOpenDevice(device,binding,budget);};
    const u64 limit=2*b.page_size_bytes;
    reads=0;observed_allocations=0;count_allocations=true;auto result=read(limit);count_allocations=false;
    const auto read_count=reads;const auto allocation_count=observed_allocations;
    Check(result.ok()&&result.pages.size()==2&&result.retained_image_bytes==limit&&result.state_counts[4]==1&&
          result.state_counts[7]==1&&result.pages[0].bytes==head_bytes&&result.pages[1].bytes==tail_bytes,
          "actual complete multi-page allocation chain");
    ChainMemoryChecks(device,binding,zero.roots.front(),*zero_bytes.bytes,head_bytes,profile==0&&ownership==0);
    Empty(read(limit-1));
    for(unsigned fault=1;fault<=read_count;++fault){reads=0;fail_read=fault;result=read(limit);fail_read=0;Empty(result);}
    if(profile==0){bool success=false;
      for(unsigned long budget=0;budget<=allocation_count;++budget){allocation_budget=static_cast<long>(budget);result=read(limit);allocation_budget=-1;
        if(result.ok()){success=true;break;}Empty(result);Check(result.error==E::resource_exhausted,"retained allocation failure classified");}
      Check(success,"all retained-chain allocation failure positions");
      std::cout << "retained allocation fault positions=" << allocation_count << '\n';
    }
    for(unsigned mutation=0;mutation<10;++mutation){auto bad_head=head,bad_tail=tail;
      if(mutation==0)bad_tail.map_generation++;
      if(mutation==1)bad_tail.header.page_uuid=Id(10);
      if(mutation==2){if(bad_tail.creator_operation_uuid.is_nil())bad_tail.creator_transaction_uuid=Id(90);
        else bad_tail.creator_operation_uuid=Id(90);}
      if(mutation==3)bad_head.records[1].page_uuid=Id(90);
      if(mutation==4)bad_head.records[0].page_generation++;
      if(mutation==5)bad_head.records[2].owner_uuid=Id(90);
      if(mutation==6)bad_head.next->page_generation++;
      if(mutation==7)bad_tail.object_uuid=Id(90);
      if(mutation==8){bad_head.header.page_uuid=zero.page_uuid;bad_head.records[1].page_uuid=zero.page_uuid;}
      if(mutation==9)OperationOwned(bad_tail,101);
      const auto changed_tail=Oracle(bad_tail);bad_head.next_sha256=Hash(changed_tail);
      if(mutation==2||mutation==9)Check(p::DecodeNativeAllocationMap(changed_tail).ok()&&
        p::DecodeNativeAllocationMap(Oracle(bad_head)).ok(),"lineage mismatch preserves individually valid images");
      write(1,Oracle(bad_head));write(2,changed_tail);const auto rejected=read(limit);Empty(rejected);
      if(mutation==2||mutation==9)Check(rejected.error==E::chain_mismatch,"actual chain binds complete creator tuple");
    }
    write(1,head_bytes);write(2,tail_bytes);
    auto changed=tail_bytes;changed[392+8+15]^=1;Seal(changed);write(2,changed);Empty(read(limit));write(2,tail_bytes);
    { // Individually valid nonterminal images must not navigate back into the root.
      auto cycle_head=head,cycle_tail=full;
      cycle_head.states.resize(4);
      cycle_head.records.erase(std::remove_if(cycle_head.records.begin(),cycle_head.records.end(),
          [](const auto& r){return r.page_number>=4;}),cycle_head.records.end());
      cycle_tail.header=tail.header;cycle_tail.first_page=4;
      cycle_tail.states={S::allocated,S::reusable_pending_mga,S::reusable_free,S::compacting};
      cycle_tail.records.erase(std::remove_if(cycle_tail.records.begin(),cycle_tail.records.end(),
          [](const auto& r){return r.page_number<4||r.page_number>=8;}),cycle_tail.records.end());
      cycle_tail.next=d::NativePageReference{Id(2),1,4,b.page_size_profile_uuid};cycle_tail.next_sha256[0]=1;
      const auto cycle_bytes=Oracle(cycle_tail);cycle_head.next_sha256=Hash(cycle_bytes);
      Check(p::DecodeNativeAllocationMap(cycle_bytes).ok()&&p::EncodeNativeAllocationMap(cycle_head).ok(),
            "cycle fixture images individually valid");
      write(1,Oracle(cycle_head));write(2,cycle_bytes);const auto cycle=read(limit);
      Empty(cycle);Check(cycle.error==E::chain_mismatch,"physical root cycle rejected before reread");
      write(1,head_bytes);write(2,tail_bytes);
    }
    auto wrong_zero=zero;wrong_zero.free_pages=2;auto wrong=d::EncodeFilespacePageZero(wrong_zero);Check(wrong.ok(),"counter mismatch fixture");
    write(0,*wrong.bytes);Empty(read(limit));write(0,*zero_bytes.bytes);
    fail_hash=true;result=read(limit);Check(!fail_hash,"retained hash failure reached provider");Empty(result);
    HistoricalChain(device,zero,*zero_bytes.bytes,head_bytes,tail_bytes);
    Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing_read_only).ok(),"reopen read-only actual filespace");
    Check(read(limit).ok()&&device.read_only(),"retained reader preserves read-only ownership");
    const auto retained=p::ReadNativeAllocationChainAtHistoricalRootFromOpenDevice(
        device,binding,zero.roots.front(),Hash(head_bytes),*zero_bytes.bytes,limit+b.page_size_bytes+4096);
    Check(retained.ok()&&device.read_only()&&retained.pages[0].bytes==head_bytes&&retained.pages[1].bytes==tail_bytes,
          "historical reader preserves actual read-only reopened ownership");
    Check(device.Close().ok(),"close fixture");Empty(read(limit));
  }
}
}  // namespace
int main(){
  try { Codecs();OperationCodecs();BorrowedCodecs();RetainedChain();
#ifdef SB_NATIVE_ALLOCATION_MEMORY_TESTS
    MemoryEncodingTests();MemoryTests();
#endif
    std::cout<<"native allocation checks="<<checks<<" failures=0\n"; }
  catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
