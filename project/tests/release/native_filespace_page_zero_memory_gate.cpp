// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_filespace_page_zero_memory.hpp"
#include <sys/stat.h>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>
#include <span>
#include <source_location>

namespace {
thread_local long budget=-1;
thread_local bool measuring=false;
thread_local unsigned allocations=0;
unsigned fault=0,fault_context=1,hashes=0,hash_at=0,checks=0;
unsigned reads=0,fail_read=0,short_read=0,eof_read=0;
unsigned long writes=0,syncs=0;
unsigned stats=0,fail_stat=0,resize_stat=0;
off_t resize_bytes=0;
const std::vector<unsigned char>* changed_prefix=nullptr;
void* last_read_buffer=nullptr;
bool bootstrap_fault=false;
unsigned bootstrap_hashes=0,bootstrap_fail_at=0;
std::recursive_mutex* allocation_device_mutex=nullptr;
std::recursive_mutex* deallocation_device_mutex=nullptr;
bool allocation_lock_free=false,deallocation_lock_free=false;
std::recursive_mutex* observation_mutex=nullptr;
unsigned guarded_observations=0;
bool observation_unlocked=false;
std::recursive_mutex* backend_failure_probe=nullptr;
std::recursive_mutex* next_allocation_probe=nullptr;
bool failure_allocation_unlocked=false,deny_failure_observation=false;
void ProbeFailureAllocation(){if(auto* mutex=next_allocation_probe){
  next_allocation_probe=nullptr;bool available=false;
  std::thread worker([&]{available=mutex->try_lock();if(available)mutex->unlock();});worker.join();
  failure_allocation_unlocked=available;
  if(deny_failure_observation)throw std::bad_alloc();
}}
void ProbeObservation(){if(observation_mutex){bool available=false;
  std::thread worker([&]{available=observation_mutex->try_lock();if(available)observation_mutex->unlock();});worker.join();
  ++guarded_observations;observation_unlocked|=available;}}
void ProbeCleanup(){if(deallocation_device_mutex){auto* mutex=deallocation_device_mutex;deallocation_device_mutex=nullptr;
  bool available=false;std::thread worker([&]{available=mutex->try_lock();if(available)mutex->unlock();});worker.join();deallocation_lock_free=available;}}
}
void* operator new(std::size_t n){ProbeFailureAllocation();if(measuring)++allocations;if(budget==0){budget=-1;throw std::bad_alloc();}
  if(budget>0)--budget;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
void* operator new(std::size_t n,std::align_val_t a){
  ProbeFailureAllocation();
  if(measuring)++allocations;if(budget==0){budget=-1;throw std::bad_alloc();}if(budget>0)--budget;
  if(allocation_device_mutex){auto* mutex=allocation_device_mutex;allocation_device_mutex=nullptr;
    bool available=false;std::thread worker([&]{available=mutex->try_lock();if(available)mutex->unlock();});worker.join();allocation_lock_free=available;}
  void* p=nullptr;if(posix_memalign(&p,static_cast<std::size_t>(a),n?n:1)==0)return p;throw std::bad_alloc();
}
void operator delete(void* p,std::align_val_t)noexcept{ProbeCleanup();std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t)noexcept{ProbeCleanup();std::free(p);}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* p,size_t n,off_t at){
  ++reads;last_read_buffer=p;ProbeObservation();
  if(changed_prefix&&reads==2){if(__real_pwrite(fd,changed_prefix->data(),changed_prefix->size(),0)!=ssize_t(changed_prefix->size()))std::abort();}
if(fail_read==reads){next_allocation_probe=backend_failure_probe;errno=EIO;return -1;}
  if(eof_read&&eof_read==reads){next_allocation_probe=backend_failure_probe;return 0;}
  return __real_pread(fd,p,short_read&&short_read==reads?n-1:n,at);
}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t at){++writes;return __real_pwrite(fd,p,n,at);}
extern "C" int __real_fstat(int,struct stat*);
extern "C" int __wrap_fstat(int fd,struct stat* s){++stats;ProbeObservation();if(fail_stat==stats){next_allocation_probe=backend_failure_probe;errno=EIO;return -1;}
  if(resize_stat==stats&&ftruncate(fd,resize_bytes))std::abort();return __real_fstat(fd,s);}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){++syncs;return __real_fsync(fd);}
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* p,size_t n,unsigned char* out,unsigned int* count,const EVP_MD* md,ENGINE* engine){
  ++bootstrap_hashes;if(bootstrap_fail_at==bootstrap_hashes)return 0;
  if(bootstrap_fault){bootstrap_fault=false;return 0;}return __real_EVP_Digest(p,n,out,count,md,engine);
}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){++hashes;if(hash_at&&hashes==hash_at)return nullptr;return __real_EVP_MD_CTX_new();}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* m,ENGINE* e){if(fault==1&&hashes==fault_context){fault=0;return 0;}return __real_EVP_DigestInit_ex(c,m,e);}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* p,size_t n){if(fault==2&&hashes==fault_context){fault=0;return 0;}return __real_EVP_DigestUpdate(c,p,n);}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* p,unsigned int* n){
  if(fault==3&&hashes==fault_context){fault=0;return 0;}const auto result=__real_EVP_DigestFinal_ex(c,p,n);if(fault==4&&hashes==fault_context){fault=0;*n=31;}return result;}
namespace {
namespace db=scratchbird::storage::database;namespace d=scratchbird::storage::disk;
using namespace scratchbird::core::platform;
using Bytes=std::vector<byte>;using E=d::FilespacePageZeroError;
void Check(bool ok,const char* why,std::source_location at=std::source_location::current()){
  ++checks;if(!ok){std::cerr<<at.line()<<": "<<why<<'\n';throw why;}}
Uuid Id(byte n){Uuid id;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=n;return id;}
void Num(Bytes& b,usize at,unsigned n,u64 v){for(unsigned i=0;i<n;++i)b[at+i]=byte(v>>(8*i));}
void Put(Bytes& b,usize at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}

auto Hash(std::span<const byte> b){std::array<byte,32> h{};Check(SHA256(b.data(),b.size(),h.data())!=nullptr,"independent SHA256");return h;}
void Seal(Bytes& b){std::fill(b.begin()+4448,b.begin()+4480,0);auto h=Hash(b);std::copy(h.begin(),h.end(),b.begin()+4448);}
template<class Zero> Bytes Oracle(const Zero& r){const auto& h=r.bootstrap;Bytes b(h.page_size_bytes,0);
  std::copy_n("SBFP",4,b.begin());Num(b,4,2,1);Num(b,6,2,4096);Num(b,8,4,h.page_size_bytes);Num(b,12,4,h.flags);
  Put(b,16,h.database_uuid);Put(b,32,h.filespace_uuid);Put(b,48,h.page_size_profile_uuid);Num(b,64,4,h.durable_format_generation);
  Num(b,68,2,h.filespace_role);Num(b,70,2,h.lifecycle_state);Put(b,72,h.checksum_profile_uuid);Put(b,88,h.encryption_profile_uuid);
  auto digest=Hash(std::span<const byte>(b).first(104));std::copy(digest.begin(),digest.end(),b.begin()+104);
  constexpr usize c=4096,f=4224;std::copy_n("SBPGV002",8,b.begin()+c);
  Num(b,c+8,4,128);Num(b,c+12,4,h.page_size_bytes);Num(b,c+16,4,h.filespace_role<=4?1:2);Num(b,c+20,2,1);Num(b,c+22,2,1);
  Put(b,c+24,h.database_uuid);Put(b,c+40,h.filespace_uuid);Put(b,c+56,r.page_uuid);
  Num(b,c+80,8,r.page_generation);Num(b,c+88,8,h.flags&2);Put(b,c+104,h.page_size_profile_uuid);Num(b,c+120,2,1);
  u64 fnv=14695981039346656037ull;for(usize i=c;i<c+128;++i){fnv^=b[i];fnv*=1099511628211ull;}Num(b,c+96,8,fnv);
  std::copy_n("SBFZV001",8,b.begin()+f);Num(b,f+8,4,256);Num(b,f+12,4,r.roots.size());Num(b,f+16,8,256+80*r.roots.size());
  Num(b,f+24,8,r.root_set_generation);Put(b,f+32,h.database_uuid);Put(b,f+48,h.filespace_uuid);Put(b,f+64,h.page_size_profile_uuid);
  Put(b,f+80,h.checksum_profile_uuid);Put(b,f+96,h.encryption_profile_uuid);Put(b,f+112,r.page_uuid);Num(b,f+128,4,h.page_size_bytes);
  Num(b,f+132,4,h.durable_format_generation);Num(b,f+136,2,h.filespace_role);Num(b,f+138,2,h.lifecycle_state);Num(b,f+140,4,h.flags);
  Num(b,f+144,8,r.total_pages);Num(b,f+152,8,r.free_pages);Num(b,f+160,8,r.preallocated_pages);Num(b,f+168,8,r.page_generation);
  Put(b,f+176,r.creation_operation_uuid);Put(b,f+192,r.writer_identity_uuid);Num(b,f+208,8,r.creation_utc_millis);Num(b,f+216,4,80);
  for(usize i=0;i<r.roots.size();++i){const auto& a=r.roots[i];const auto n=4480+80*i;Num(b,n,2,a.kind);Num(b,n+4,4,a.page_type);
    Put(b,n+8,a.filespace_uuid);Num(b,n+24,8,a.page_number);Num(b,n+32,8,a.page_generation);Put(b,n+40,a.page_size_profile_uuid);Put(b,n+56,a.object_uuid);}
  Seal(b);return b;
}
d::FilespacePageZero Example(unsigned profile=0,unsigned member=0,unsigned role=1,unsigned state=1,unsigned flags=0){
  const auto& own=d::kCanonicalFilespacePageProfiles[profile];const auto& target=d::kCanonicalFilespacePageProfiles[member];
  d::FilespacePageZero r;r.bootstrap={Id(1),Id(2),own.uuid,d::kNativeBootstrapIntegrityProfile,flags&1?Id(71):Uuid{},own.page_size_bytes,1,flags,u16(role),u16(state)};
  r.page_uuid=Id(10);r.creation_operation_uuid=Id(11);r.writer_identity_uuid=Id(12);r.page_generation=3;r.root_set_generation=8;
  r.total_pages=64;r.free_pages=10;r.preallocated_pages=3;r.creation_utc_millis=123456;
  constexpr unsigned types[]={0,8,5,3,769,9,10,11,5,768,771,773,775,779,1301,1030,1025,777,782,782,1280,1280};
  for(unsigned kind=1;kind<=21;++kind){if(role>4&&kind!=3)continue;if(kind>=15&&kind<=17&&!(flags&2))continue;
    const bool local=kind==3||kind>=18;d::FilespaceRootReference a;
    a.kind=kind;a.page_type=types[kind];a.filespace_uuid=local?Id(2):Id(3);a.page_number=kind;a.page_generation=50+kind;
    a.page_size_profile_uuid=local?own.uuid:target.uuid;a.object_uuid=kind>=20?Id(81):kind>=18?Id(80):Id(100+kind);r.roots.push_back(a);}
  return r;
}
namespace m=scratchbird::core::memory;
using ME=db::NativeFilespacePageZeroMemoryError;
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
    r.route_label="storage.page-zero.conformance";r.purpose="actual page-zero image and metadata";
    r.binary_operation_uuid=binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
    r.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(64).bytes},
      {m::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
    r.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label="page-zero resource conformance";
    for(const auto& scope:r.scope_chain){m::HierarchicalMemoryBudget b;b.scope=scope;b.hard_limit_bytes=bytes;
      b.provenance=r.provenance;Check(ledger.SetBudget(b).ok(),"actual parent budget");}
    auto grant=m::AcquireReservationBackedMemoryResource(r);Check(grant.ok(),"actual node-issued metadata grant");
    auto adopted=db::AdoptNativeStorageMemory(binding,grant.resource);Check(adopted.ok()&&!grant.resource,"exclusive native adoption");
    memory=std::move(adopted.memory);
  }
  void Empty(){const auto s=manager.Snapshot();Check(!s.current_bytes&&!s.reserved_capacity_bytes&&
    !s.active_capacity_reservation_count&&!ledger.Snapshot().current_bytes,"all real page-zero charges released");}
};

struct MemoryFile {
  std::filesystem::path directory,path;d::FileDevice device;d::FilespacePageZero value;Bytes bytes;d::FilespaceBootstrapBinding expected;
  explicit MemoryFile(unsigned profile):value(Example(profile)),bytes(Oracle(value)){
    char name[]="/tmp/sb-pagezero-memory-XXXXXX";const auto* made=mkdtemp(name);if(!made)throw std::runtime_error("mkdtemp");
    directory=made;path=directory/"native.bin";
    Check(device.Open(path.string(),d::FileOpenMode::create_new).ok(),"actual page-zero source");
    expected={value.bootstrap.database_uuid,value.bootstrap.filespace_uuid,value.bootstrap.page_size_profile_uuid};
    Store(bytes);Resize(value.total_pages*bytes.size());
    Check(device.Sync().ok()&&device.Close().ok()&&device.Open(path.string(),d::FileOpenMode::open_existing).ok(),"cold reopen independent page zero");
  }
  ~MemoryFile(){(void)device.Close();std::error_code ec;std::filesystem::remove_all(directory,ec);}
  void Store(const Bytes& b){Check(device.WriteAt(0,b.data(),b.size()).ok()&&device.Sync().ok(),"actual independent page-zero image");}
  void Resize(u64 size){Check(truncate(path.c_str(),off_t(size))==0,"actual external extent change");}
  auto Read(MemoryFixture& f){reads=hashes=stats=bootstrap_hashes=0;return db::ReadNativeFilespacePageZeroWithMemoryFromOpenDevice(device,expected,f.memory,f.binding);}
};
void NoRecord(const db::NativeFilespacePageZeroMemoryResult& r){Check(!r.ok()&&!r.record&&r.image.empty()&&!r.arena,"refusal exposes no image metadata or owner prefix");}
void Codecs(){
  std::array<d::FilespaceRootReference,32> roots;
  for(unsigned profile=0;profile<5;++profile)for(unsigned member=0;member<5;++member)
    for(unsigned role=1;role<=14;++role)for(unsigned state=1;state<=15;++state)for(unsigned flags=0;flags<4;++flags){
      auto value=Example(profile,member,role,state,flags);const auto bytes=Oracle(value);
      allocations=0;measuring=true;budget=0;auto r=d::DecodeFilespacePageZeroInto(bytes,roots);const auto remaining=budget;budget=-1;measuring=false;
      Check(r.ok()&&!allocations&&remaining==0,"all roles states flags and 25 mixed profile pairs decode without hidden allocation");
      Check(r.record->roots.data()==roots.data()&&r.record->roots.size()==value.roots.size()&&Oracle(*r.record)==bytes,"exact-length caller-owned native binary roots");
      Bytes encoded(bytes.size()+2,0xa5);
      const auto& v=*r.record;
      const d::FilespacePageZeroConstView readonly{v.bootstrap,v.page_uuid,v.creation_operation_uuid,v.writer_identity_uuid,
        v.page_generation,v.root_set_generation,v.total_pages,v.free_pages,v.preallocated_pages,v.creation_utc_millis,v.roots};
      allocations=0;measuring=true;budget=0;
      const auto owned=d::EncodeFilespacePageZeroInto(value,std::span(encoded).subspan(1,bytes.size()));
      const auto borrowed=d::EncodeFilespacePageZeroInto(*r.record,std::span(encoded).subspan(1,bytes.size()));
      const auto immutable=d::EncodeFilespacePageZeroInto(readonly,std::span(encoded).subspan(1,bytes.size()));
      const auto encode_remaining=budget;budget=-1;measuring=false;
      Check(owned.ok()&&borrowed.ok()&&immutable.ok()&&!allocations&&encode_remaining==0&&
        owned.bytes.data()==encoded.data()+1&&borrowed.bytes.size()==bytes.size()&&
        encoded.front()==0xa5&&encoded.back()==0xa5&&std::equal(bytes.begin(),bytes.end(),owned.bytes.begin()),
        "all bounded encoders preserve every independent byte for all roles states flags and25pairs");
    }
  auto value=Example(0,0,1,1,2);auto good=Oracle(value);
  Bytes encoded(good.size()+16,0xa5);
  budget=0;const auto oversized=d::EncodeFilespacePageZeroInto(value,encoded);const auto oversized_remaining=budget;budget=-1;
  Check(oversized.ok()&&oversized.bytes.size()==good.size()&&oversized_remaining==0&&
    std::equal(good.begin(),good.end(),encoded.begin())&&std::all_of(encoded.begin()+good.size(),encoded.end(),[](byte b){return b==0xa5;}),
    "oversized output returns exact page and leaves whole suffix untouched");
  std::fill(encoded.begin(),encoded.end(),0xa5);
  budget=0;const auto null_output=d::EncodeFilespacePageZeroInto(value,{static_cast<byte*>(nullptr),1});const auto null_remaining=budget;budget=-1;
  Check(null_output.error==E::invalid_backing&&null_output.bytes.empty()&&null_remaining==0,"null nonempty output refused before access");
  for(const auto length:{std::size_t(0),std::size_t(1),good.size()-1}){
    budget=0;const auto short_output=d::EncodeFilespacePageZeroInto(value,std::span(encoded).first(length));
    const auto remaining=budget;budget=-1;
    Check(short_output.error==E::resource_exhausted&&short_output.bytes.empty()&&remaining==0&&
      std::all_of(encoded.begin(),encoded.end(),[](byte b){return b==0xa5;}),"short output refuses before mutation or diagnostic allocation");
  }
  for(const bool roots_alias:{false,true}){
    auto* alias=roots_alias?reinterpret_cast<byte*>(value.roots.data()):reinterpret_cast<byte*>(&value);
    const auto size=roots_alias?value.roots.size()*sizeof(value.roots[0]):sizeof(value);
    budget=0;const auto refused=d::EncodeFilespacePageZeroInto(value,{alias,size});const auto remaining=budget;budget=-1;
    Check(refused.error==E::invalid_backing&&refused.bytes.empty()&&remaining==0&&Oracle(value)==good,"value and nested root aliases refuse before write");
  }
  budget=0;const auto wrapped_output=d::EncodeFilespacePageZeroInto(value,{encoded.data(),std::numeric_limits<usize>::max()});
  const auto wrap_remaining=budget;budget=-1;
  Check(wrapped_output.error==E::invalid_backing&&wrapped_output.bytes.empty()&&wrap_remaining==0,"wrapped whole output refuses without access");
  for(bool root_overflow:{false,true}){auto invalid=value;
    if(root_overflow)invalid.roots.front().page_number=static_cast<u64>(std::numeric_limits<std::streamoff>::max())/8192;
    else invalid.total_pages=static_cast<u64>(std::numeric_limits<std::streamoff>::max())/8192+1;
    budget=0;const auto refused=d::EncodeFilespacePageZeroInto(invalid,encoded);const auto remaining=budget;budget=-1;
    Check(refused.error==(root_overflow?E::invalid_root_directory:E::invalid_capacity)&&refused.bytes.empty()&&remaining==0,
      "signed physical extent failures retain exact structural errors without rendering");
  }
  for(unsigned count=0;count<value.roots.size();++count){auto r=d::DecodeFilespacePageZeroInto(good,std::span(roots).first(count));Check(!r.record&&r.error==E::resource_exhausted,"every undersized output refuses");}
  auto overlap=d::DecodeFilespacePageZeroInto(good,{reinterpret_cast<d::FilespaceRootReference*>(good.data()),32});
  Check(!overlap.record&&overlap.error==E::invalid_backing&&good==Oracle(value),"overlap refuses before writing");
  auto wrapped=d::DecodeFilespacePageZeroInto(good,{roots.data(),std::numeric_limits<usize>::max()/sizeof(roots[0])+1});
  Check(!wrapped.record&&wrapped.error==E::invalid_backing,"overflow output range refuses");
  Bytes unaligned(1);unaligned.insert(unaligned.end(),good.begin(),good.end());auto r=d::DecodeFilespacePageZeroInto(std::span<const byte>(unaligned).subspan(1),roots);
  Check(r.ok(),"unaligned encoded input accepted");unaligned.clear();std::fill(good.begin(),good.end(),0);
  Check(Oracle(*r.record)==Oracle(value),"metadata independent of input lifetime");good=Oracle(value);
  for(usize at=0;at<good.size();++at){auto bad=good;bad[at]^=1;auto broken=d::DecodeFilespacePageZeroInto(bad,roots);Check(!broken.ok()&&!broken.record,"each byte corruption refuses");}
  for(unsigned variant=0;variant<24;++variant){auto bad=value;
    switch(variant){
      case 0:bad.total_pages=0;break;case 1:bad.total_pages=std::numeric_limits<u64>::max();break;
      case 2:bad.free_pages=bad.total_pages;break;case 3:bad.preallocated_pages=bad.total_pages-bad.free_pages;break;
      case 4:bad.page_uuid={};break;case 5:bad.creation_operation_uuid={};break;case 6:bad.writer_identity_uuid={};break;
      case 7:bad.page_generation=0;break;case 8:bad.root_set_generation=0;break;
      case 9:bad.roots[1].kind=1;break;case 10:bad.roots[1].page_type=3;break;
      case 11:bad.roots[1].filespace_uuid={};break;case 12:bad.roots[1].page_number=0;break;
      case 13:bad.roots[1].page_generation=0;break;case 14:bad.roots[1].object_uuid={};break;
      case 15:bad.roots[1].page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;break;
      case 16:bad.roots[2].filespace_uuid=Id(3);break;case 17:bad.roots[2].page_number=64;break;
      case 18:bad.roots.erase(bad.roots.begin()+17);break;case 19:bad.roots.erase(bad.roots.begin()+19);break;
      case 20:bad.roots[18].object_uuid=Id(200);break;case 21:bad.roots[20].page_number=20;break;
      case 22:bad.roots[19].object_uuid=bad.roots[20].object_uuid=Id(80);break;
      case 23:bad.roots[19].page_number=3;break;
    }
    auto image=Oracle(bad);auto owned=d::DecodeFilespacePageZero(image.data(),image.size());auto borrowed=d::DecodeFilespacePageZeroInto(image,roots);
    Check(!borrowed.record&&!owned.record&&borrowed.error==owned.error,"shared full semantic validation rejects independently sealed invalid page");
    const auto expected_encode=d::EncodeFilespacePageZero(bad);
    std::fill(encoded.begin(),encoded.end(),0xa5);budget=0;
    const auto refused=d::EncodeFilespacePageZeroInto(bad,encoded);const auto remaining=budget;budget=-1;
    Check(!refused.ok()&&refused.bytes.empty()&&refused.error==expected_encode.error&&remaining==0&&
      std::all_of(encoded.begin(),encoded.end(),[](byte b){return b==0xa5;}),"every invalid structural value refuses encoding before mutation with fixed error");
  }
  for(unsigned at:{4232u,4236u,4240u,4256u,4352u,4356u,4358u,4364u,4440u,4444u,4482u,4552u,8191u}){
    auto bad=good;bad[at]^=0x80;Seal(bad);auto owned=d::DecodeFilespacePageZero(bad.data(),bad.size());auto borrowed=d::DecodeFilespacePageZeroInto(bad,roots);
    Check(!borrowed.record&&!owned.record&&borrowed.error==owned.error,"resealed framing duplicate and reserved fields refuse");
  }
  auto empty=Example(0,0,1,7);empty.roots.clear();auto empty_bytes=Oracle(empty);auto no_roots=d::DecodeFilespacePageZeroInto(empty_bytes,{});
  Check(no_roots.ok()&&no_roots.record->roots.empty(),"initializing empty structural directory remains non-serving inspection");
  auto missing=value;missing.roots.erase(missing.roots.begin());Check(d::DecodeFilespacePageZeroInto(Oracle(missing),roots).error==E::required_root_missing,"online required roots not waived");
  auto alias=value;alias.roots[7]=alias.roots[1];alias.roots[7].kind=8;
  Check(d::DecodeFilespacePageZeroInto(Oracle(alias),roots).ok(),"consistent catalog-feature physical alias");
  alias.roots[7].page_generation++;Check(d::DecodeFilespacePageZeroInto(Oracle(alias),roots).error==E::invalid_root_directory,"inconsistent physical alias refuses");
  for(unsigned size:{0u,4095u,4096u,8191u,8193u}){auto b=good;b.resize(size);Check(!d::DecodeFilespacePageZeroInto(b,roots).record,"incorrect image extent refuses");}
}
void MemoryTests(){
  for(unsigned profile=0;profile<5;++profile){MemoryFile file(profile);const auto capacity=db::NativeFilespacePageZeroWorkspaceBytes(file.expected.page_size_profile_uuid);
    Check(capacity==file.bytes.size()+32*sizeof(d::FilespaceRootReference)+(alignof(std::max_align_t)-1),"independent native workspace formula");
    {
      MemoryFixture f(capacity);{auto guard=file.device.AcquireOperationGuard();allocation_device_mutex=guard.mutex();}
      allocation_lock_free=false;auto r=file.Read(f);Check(r.ok()&&allocation_lock_free,"real backing allocated outside device guard");
      Check(last_read_buffer==r.image.data()&&Oracle(*r.record)==file.bytes&&r.size_before==file.value.total_pages*file.bytes.size()&&r.size_after==r.size_before,"actual returned charged image and complete exact extent");
      auto first=reinterpret_cast<std::uintptr_t>(r.image.data()),root=reinterpret_cast<std::uintptr_t>(r.record->roots.data());
      Check(root>=first&&root-first<=capacity&&r.record->roots.size_bytes()<=capacity-(root-first),"native roots in actual shared charged block");
      Check(f.manager.Snapshot().current_bytes==capacity&&f.ledger.Snapshot().current_bytes==capacity&&f.memory.Snapshot().allocated_bytes==capacity&&r.arena.Snapshot().retained_bytes==capacity,"all actual ledgers agree");
      auto full=file.Read(f);NoRecord(full);Check(full.error==ME::memory_allocation_failure&&!reads,"live backing prevents second uncharged reader");
      Check(f.ledger.CleanupOwner(f.binding.owner_uuid.bytes).retained_bytes==capacity,"revocation retains actual backing");
      auto denied=file.Read(f);NoRecord(denied);Check(denied.error==ME::memory_binding_failure&&!reads,"revoked grant prevents source I/O");
      f.memory={};Check(f.manager.Snapshot().current_bytes==capacity,"returned owner survives caller workspace");
      std::thread worker([owned=std::move(r)]()mutable{budget=0;owned={};if(budget!=0)std::abort();budget=-1;});worker.join();f.Empty();
    }
    MemoryFixture valid(capacity);MemoryFixture small(capacity-1);auto denied=file.Read(small);NoRecord(denied);
    Check(denied.error==ME::memory_allocation_failure&&!reads&&!small.manager.Snapshot().current_bytes,"one byte short refuses");small.memory={};small.Empty();
    {auto guard=file.device.AcquireOperationGuard();observation_mutex=guard.mutex();}
    guarded_observations=0;observation_unlocked=false;
    {auto r=file.Read(valid);Check(r.ok()&&guarded_observations==4&&!observation_unlocked,"one actual fence held at both reads and both size observations");}
    observation_mutex=nullptr;
    for(unsigned field=0;field<4;++field){auto wrong=valid.binding;const std::array<Uuid*,4> fields{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};*fields[field]=Id(99);
      reads=0;auto r=db::ReadNativeFilespacePageZeroWithMemoryFromOpenDevice(file.device,file.expected,valid.memory,wrong);NoRecord(r);
      Check(r.error==ME::memory_binding_failure&&!reads&&!valid.manager.Snapshot().current_bytes,"full binary binding refuses before allocation I/O");}
    const auto original=file.expected;
    for(unsigned field=0;field<3;++field){if(field==0)file.expected.database_uuid=Id(99);if(field==1)file.expected.filespace_uuid={};if(field==2)file.expected.page_size_profile_uuid=Id(99);
      auto r=file.Read(valid);NoRecord(r);Check(!reads&&!valid.memory.Snapshot().allocation_count,"invalid expected binding refuses before backing");file.expected=original;}
    const auto no_writes=writes,no_syncs=syncs;
    for(unsigned n=1;n<=2;++n){fail_read=n;auto r=file.Read(valid);fail_read=0;NoRecord(r);Check(r.error==ME::io_failure&&reads==n&&!valid.manager.Snapshot().current_bytes,"each actual read failure releases memory");}
    for(unsigned n=1;n<=2;++n){fail_stat=n;auto r=file.Read(valid);fail_stat=0;NoRecord(r);Check(r.error==ME::io_failure&&stats==n,"both physical size failures remain typed");}
    for(unsigned route=0;route<3;++route)for(unsigned n=1;n<=2;++n)for(bool deny:{false,true}){
      backend_failure_probe=file.device.AcquireOperationGuard().mutex();
      failure_allocation_unlocked=false;deny_failure_observation=deny;
      if(route==0)fail_read=n;
      else if(route==1)fail_stat=n;
      else {short_read=n;eof_read=n+1;}
      const auto r=file.Read(valid);
      fail_read=fail_stat=short_read=eof_read=0;backend_failure_probe=nullptr;deny_failure_observation=false;
      Check(!next_allocation_probe&&failure_allocation_unlocked,
        "first allocation after native error or EOF is outside the complete device fence");
      NoRecord(r);Check(r.error==ME::io_failure&&!r.io_status.ok()&&!valid.manager.Snapshot().current_bytes,
        "diagnostic/observation allocation failure never erases backend cause or retains unreported backing");
      Check(r.io_receipt.operation==(route==1?d::BoundedIoOperation::size:d::BoundedIoOperation::read)&&
        r.io_receipt.error==(route==2?d::BoundedIoError::short_transfer:d::BoundedIoError::native_failure)&&
        r.io_receipt.native_attempted&&r.io_receipt.native_error==(route==2?0u:unsigned(EIO)),
        "fixed receipt distinguishes both size failures, both read failures and EOF");
      if(route!=1){
        Check(r.bootstrap_bytes_read==(n==1?(route==2?4095u:0u):4096u),"exact failed bootstrap progress");
        Check(r.page_bytes_read==(n==2&&route==2?file.bytes.size()-1:0),"exact failed payload progress");
      }else Check(r.size_before==(n==2?std::optional<u64>(file.value.total_pages*file.bytes.size()):std::nullopt)&&
        !r.size_after,"failed size is absent, not a zero-size observation");
      const auto rendered=d::RenderBoundedIoResult(r.io_receipt,file.path.string());
      Check(!rendered.ok()&&rendered.bytes_transferred==r.io_receipt.bytes_transferred,
        "outside-guard diagnostic retains exact physical progress");
    }
    short_read=2;{auto r=file.Read(valid);Check(r.ok()&&reads==3,"short read completes");}short_read=0;
    short_read=2;eof_read=3;{auto r=file.Read(valid);NoRecord(r);Check(r.error==ME::io_failure&&r.page_bytes_read==file.bytes.size()-1,"partial image progress retained");}short_read=eof_read=0;
    bootstrap_fault=true;{auto r=file.Read(valid);NoRecord(r);Check(!bootstrap_fault&&r.error==ME::bootstrap_failure,"actual bootstrap hash failure");}
    bootstrap_fail_at=2;{auto r=file.Read(valid);NoRecord(r);Check(bootstrap_hashes==2&&r.page_zero_error==E::hash_provider_failure,"bootstrap revalidation provider failure remains typed");}bootstrap_fail_at=0;
    for(unsigned method=1;method<=4;++method){fault_context=1;fault=method;auto r=file.Read(valid);NoRecord(r);Check(!fault&&r.page_zero_error==E::hash_provider_failure,"each actual full-page hash method fails");}
    hash_at=1;{auto r=file.Read(valid);NoRecord(r);Check(r.page_zero_error==E::hash_provider_failure,"real digest context failure");}hash_at=0;
    Check(writes==no_writes&&syncs==no_syncs,"reader never mutates source");
    const auto extent=file.value.total_pages*file.bytes.size();
    for(u64 size:{u64(file.bytes.size()-1),extent-file.bytes.size(),extent+file.bytes.size(),extent+1}){
      file.Resize(size);auto r=file.Read(valid);NoRecord(r);Check(r.page_zero_error==E::invalid_capacity,"short excess and unaligned actual extents refuse");file.Store(file.bytes);file.Resize(extent);
    }
    resize_stat=2;resize_bytes=extent+file.bytes.size();{auto r=file.Read(valid);NoRecord(r);Check(r.page_zero_error==E::invalid_capacity&&r.size_before==extent&&r.size_after==u64(resize_bytes),"actual external extent changes between observations");}resize_stat=0;file.Resize(extent);
    auto changed=file.value;changed.bootstrap.lifecycle_state=7;auto changed_image=Oracle(changed);changed_prefix=&changed_image;
    {auto r=file.Read(valid);NoRecord(r);Check(r.page_zero_error==E::probe_changed,"real changed bootstrap between two reads refuses");}changed_prefix=nullptr;file.Store(file.bytes);
    auto corrupt=file.bytes;corrupt[4480+56]^=1;file.Store(corrupt);{auto guard=file.device.AcquireOperationGuard();deallocation_device_mutex=guard.mutex();}
    deallocation_lock_free=false;{auto r=file.Read(valid);NoRecord(r);Check(r.page_zero_error==E::integrity_mismatch&&deallocation_lock_free,"failed read backing cleanup releases device guard first");}file.Store(file.bytes);
    for(unsigned role=1;role<=14;++role)for(unsigned state=1;state<=15;++state)for(unsigned flags=0;flags<4;++flags){
      auto v=Example(profile,profile,role,state,flags);const auto b=Oracle(v);file.Store(b);auto r=file.Read(valid);
      Check(r.ok()&&Oracle(*r.record)==b,"real files preserve all roles states clear encrypted and cluster declarations without granting authority");
    }
    for(unsigned member=0;member<5;++member){auto b=Oracle(Example(profile,member,1,1,3));file.Store(b);auto r=file.Read(valid);Check(r.ok()&&Oracle(*r.record)==b,"all 25 physical reference profile pairs");}
    file.Store(file.bytes);
    if(!profile){unsigned sites=0;{allocations=0;measuring=true;auto r=file.Read(valid);measuring=false;sites=allocations;Check(r.ok(),"measure actual reader allocation sites");}
      for(unsigned n=0;n<sites;++n){budget=n;auto r=file.Read(valid);budget=-1;if(r.ok())Check(Oracle(*r.record)==file.bytes,"optional telemetry loss preserves exact result");else NoRecord(r);
        r={};Check(!valid.manager.Snapshot().current_bytes&&!valid.memory.Snapshot().allocated_bytes,"all fault cleanup physical charges released");{auto retry=file.Read(valid);Check(retry.ok(),"same owner retries every allocation failure");}}
      Check(sites>0,"allocation fault sweep reached");std::cout<<"governed page-zero metadata faults="<<sites<<'\n';
    }
    Check(file.device.Close().ok(),"close source");auto closed=file.Read(valid);NoRecord(closed);Check(closed.error==ME::bootstrap_failure&&closed.bootstrap_error==d::FilespaceBootstrapError::device_not_open&&
      closed.io_receipt.error==d::BoundedIoError::not_open&&!closed.io_receipt.native_attempted&&!closed.io_status.ok(),"closed device never reopened or reported as native I/O");
    valid.memory={};valid.Empty();
  }
}
}
int main(){try{Codecs();MemoryTests();std::cout<<"PASS governed page-zero checks="<<checks<<" not_SQL_E2E=true\n";return 0;}
  catch(...){budget=-1;std::cerr<<"FAIL checks="<<checks<<'\n';return 1;}}
