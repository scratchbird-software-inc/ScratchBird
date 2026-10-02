// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_catalog_root_memory.hpp"
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
void* last_read_buffer=nullptr;
bool bootstrap_fault=false;
std::recursive_mutex* allocation_device_mutex=nullptr;
std::recursive_mutex* deallocation_device_mutex=nullptr;
bool allocation_lock_free=false,deallocation_lock_free=false;
std::recursive_mutex* observation_mutex=nullptr;
unsigned fenced_reads=0;
bool observation_unlocked=false;
void ProbeObservation(){if(observation_mutex){bool available=false;
  std::thread worker([&]{available=observation_mutex->try_lock();if(available)observation_mutex->unlock();});worker.join();
  ++fenced_reads;observation_unlocked|=available;}}
void ProbeCleanup(){if(deallocation_device_mutex){auto* mutex=deallocation_device_mutex;deallocation_device_mutex=nullptr;
  bool available=false;std::thread worker([&]{available=mutex->try_lock();if(available)mutex->unlock();});worker.join();deallocation_lock_free=available;}}
}
void* operator new(std::size_t n){if(measuring)++allocations;if(budget==0){budget=-1;throw std::bad_alloc();}
  if(budget>0)--budget;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
void* operator new(std::size_t n,std::align_val_t a){
  if(measuring)++allocations;if(budget==0){budget=-1;throw std::bad_alloc();}if(budget>0)--budget;
  if(allocation_device_mutex){auto* mutex=allocation_device_mutex;allocation_device_mutex=nullptr;
    bool available=false;std::thread worker([&]{available=mutex->try_lock();if(available)mutex->unlock();});worker.join();allocation_lock_free=available;}
  void* p=nullptr;if(posix_memalign(&p,static_cast<std::size_t>(a),n?n:1)==0)return p;throw std::bad_alloc();
}
void operator delete(void* p,std::align_val_t)noexcept{ProbeCleanup();std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t)noexcept{ProbeCleanup();std::free(p);}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* p,size_t n,off_t at){
  ++reads;last_read_buffer=p;ProbeObservation();if(fail_read==reads){errno=EIO;return -1;}if(eof_read&&eof_read==reads)return 0;
  return __real_pread(fd,p,short_read&&short_read==reads?n-1:n,at);
}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t at){++writes;return __real_pwrite(fd,p,n,at);}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){++syncs;return __real_fsync(fd);}
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* p,size_t n,unsigned char* out,unsigned int* count,const EVP_MD* md,ENGINE* engine){
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
namespace pg=scratchbird::storage::page;
using Entry=pg::NativeCatalogRootReference;
using Bytes=std::vector<byte>;using E=pg::NativeCatalogRootError;
void Check(bool ok,const char* why,std::source_location at=std::source_location::current()){
  ++checks;if(!ok){std::cerr<<at.line()<<": "<<why<<'\n';throw why;}}
Uuid Id(unsigned n){Uuid id;id.bytes[6]=0x70;id.bytes[8]=0x80;for(unsigned i=0;i<4;++i)id.bytes[15-i]=byte(n>>(8*i));return id;}
void Num(Bytes& b,usize at,unsigned n,u64 v){for(unsigned i=0;i<n;++i)b[at+i]=byte(v>>(8*i));}
void Put(Bytes& b,usize at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}
void Ref(Bytes& b,usize at,const d::NativePageReference& p){Put(b,at,p.filespace_uuid);Num(b,at+16,8,p.page_number);Num(b,at+24,8,p.page_generation);Put(b,at+32,p.page_size_profile_uuid);}
auto Hash(const Bytes& b){std::array<byte,32> h{};Check(SHA256(b.data(),b.size(),h.data())!=nullptr,"independent SHA256");return h;}

void Seal(Bytes& b){std::fill(b.begin()+304,b.begin()+336,0);const auto h=Hash(b);std::copy(h.begin(),h.end(),b.begin()+304);}
template<class Root> Bytes Oracle(const Root& v){
  const auto& h=v.header;Bytes b(h.page_size_bytes,0);
  std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,h.page_type);Num(b,20,2,1);Num(b,22,2,1);
  Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);
  Num(b,88,8,h.flags);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);
  u64 fnv=14695981039346656037ull;for(unsigned i=0;i<128;++i){fnv^=b[i];fnv*=1099511628211ull;}Num(b,96,8,fnv);
  std::copy_n("SBCROOT1",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,256);Num(b,140,4,384+80*v.roots.size());
  Num(b,144,2,v.root_kind);Num(b,146,2,v.roots.size());Num(b,152,8,v.catalog_generation);Num(b,160,8,v.schema_epoch);
  Num(b,168,8,v.security_epoch);Num(b,176,8,v.resource_epoch);Num(b,184,8,v.creator_local_transaction_id);
  Put(b,192,v.object_uuid);Put(b,208,v.creator_transaction_uuid);if(v.predecessor)Ref(b,224,*v.predecessor);
  std::copy(v.predecessor_sha256.begin(),v.predecessor_sha256.end(),b.begin()+272);
  for(usize i=0;i<v.roots.size();++i){const auto& r=v.roots[i];const auto n=384+80*i;
    Num(b,n,2,r.role);Num(b,n+4,4,r.page_type);Ref(b,n+8,r.page);Put(b,n+56,r.object_uuid);}
  Seal(b);return b;
}
pg::NativeCatalogRoot Example(unsigned profile=0,unsigned member=0,unsigned kind=2,unsigned generation=1,unsigned types=0,unsigned flags=0){
  const auto& own=d::kCanonicalFilespacePageProfiles[profile];const auto& target=d::kCanonicalFilespacePageProfiles[member];
  pg::NativeCatalogRoot p;p.header={own.page_size_bytes,kind==6?10u:kind==7?11u:5u,Id(1),Id(2),Id(10),19,109,flags,own.uuid};
  p.root_kind=kind;p.object_uuid=Id(20);p.creator_transaction_uuid=Id(21);p.creator_local_transaction_id=42;
  p.catalog_generation=generation;p.schema_epoch=9;p.security_epoch=10;p.resource_epoch=0;
  if(generation>1){p.predecessor=d::NativePageReference{Id(3),30,12,target.uuid};p.predecessor_sha256.fill(0x5c);}
  for(unsigned i=0;i<(kind==2?6u:1u);++i){Entry r;r.role=kind==2?i+1:kind==6?5:kind==7?4:6;
    r.page_type=(types&(1u<<i))?0x200:6;r.page={Id(3),40+i,13+i,target.uuid};r.object_uuid=Id(100+i);p.roots.push_back(r);}
  return p;
}
struct Scratch {
  std::vector<Entry> roots;
  explicit Scratch(usize n=6):roots(n){}
  auto Decode(std::span<const byte> b){return pg::DecodeNativeCatalogRootInto(b,roots);}
};
namespace m=scratchbird::core::memory;
using ME=db::NativeCatalogRootMemoryError;
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
    r.route_label="storage.catalog_root.conformance";r.purpose="actual catalog_root image and metadata";
    r.binary_operation_uuid=binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
    r.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(64).bytes},
      {m::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
    r.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label="catalog_root resource conformance";
    for(const auto& scope:r.scope_chain){m::HierarchicalMemoryBudget b;b.scope=scope;b.hard_limit_bytes=bytes;
      b.provenance=r.provenance;Check(ledger.SetBudget(b).ok(),"actual parent budget");}
    auto grant=m::AcquireReservationBackedMemoryResource(r);Check(grant.ok(),"actual node-issued metadata grant");
    auto adopted=db::AdoptNativeStorageMemory(binding,grant.resource);Check(adopted.ok()&&!grant.resource,"exclusive native adoption");
    memory=std::move(adopted.memory);
  }
  void Empty(){const auto s=manager.Snapshot();Check(!s.current_bytes&&!s.reserved_capacity_bytes&&
    !s.active_capacity_reservation_count&&!ledger.Snapshot().current_bytes,"all real catalog_root charges released");}
};
struct MemoryFile {
  std::filesystem::path directory,path;
  d::FileDevice device;
  pg::NativeCatalogRoot value;
  Bytes bytes;
  d::NativeCommonPageHeaderBinding expected;
  explicit MemoryFile(unsigned profile,bool root=false):value(Example(profile,0,root?6:2)),bytes(Oracle(value)){
    char name[]="/tmp/sb-catalog_root-memory-XXXXXX";const auto* made=mkdtemp(name);if(!made)throw std::runtime_error("mkdtemp");
    directory=made;path=directory/"native.bin";
    Check(device.Open(path.string(),d::FileOpenMode::create_new).ok(),"actual catalog_root source");
    expected={{value.header.database_uuid,value.header.filespace_uuid,value.header.page_size_profile_uuid},
      value.header.page_number,value.header.page_generation,value.header.page_type,value.header.page_uuid};
    Bootstrap();Store(bytes);Check(device.Sync().ok()&&device.Close().ok()&&
      device.Open(path.string(),d::FileOpenMode::open_existing).ok(),"cold reopen independent catalog_root image");
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
  void Store(const Bytes& image){Check(device.WriteAt(value.header.page_number*u64(value.header.page_size_bytes),image.data(),image.size()).ok()&&device.Sync().ok(),"actual independent catalog_root");}
  auto Read(MemoryFixture& f){reads=hashes=0;return db::ReadNativeCatalogRootWithMemoryFromOpenDevice(device,expected,value.object_uuid,f.memory,f.binding);}
};
void NoPage(const db::NativeCatalogRootMemoryResult& r){Check(!r.ok()&&!r.page&&r.image.empty()&&!r.arena,"refusal exposes no image metadata or owner prefix");}
void MemoryTests(){
  for(unsigned profile=0;profile<5;++profile)for(bool root:{false,true}){MemoryFile file(profile,root);
    const auto capacity=db::NativeCatalogRootWorkspaceBytes(file.value.header.page_size_profile_uuid);
    Check(capacity==file.bytes.size()+6*sizeof(Entry)+(alignof(std::max_align_t)-1),"independent native backing size formula");
    {
      MemoryFixture f(capacity);
      {auto guard=file.device.AcquireOperationGuard();allocation_device_mutex=guard.mutex();}
      allocation_lock_free=false;auto read=file.Read(f);
      Check(read.ok()&&allocation_lock_free,"real metadata backing allocated outside device guard");
      Check(last_read_buffer==read.image.data()&&read.image.size()==file.bytes.size()&&
        std::equal(read.image.begin(),read.image.end(),file.bytes.begin(),file.bytes.end()),"actual read destination is returned charged image");
      const auto begin=reinterpret_cast<std::uintptr_t>(read.image.data());
      const auto inside=[&](const void* ptr,usize bytes){auto n=reinterpret_cast<std::uintptr_t>(ptr);return n>=begin&&n-begin<=capacity&&bytes<=capacity-(n-begin);};
      Check(inside(read.page->roots.data(),read.page->roots.size_bytes())&&
        Oracle(*read.page)==file.bytes,"actual metadata resides in same charged block with exact binary records");
      Check(f.manager.Snapshot().current_bytes==capacity&&f.memory.Snapshot().allocated_bytes==capacity&&
        f.ledger.Snapshot().current_bytes==capacity&&read.arena.Snapshot().retained_bytes==capacity,"actual physical parent and arena charges agree");
      auto full=file.Read(f);NoPage(full);Check(full.error==ME::memory_allocation_failure&&reads==0,"simultaneous live image prevents uncharged second reader");
      const auto revoked=f.ledger.CleanupOwner(f.binding.owner_uuid.bytes);Check(revoked.retained_bytes==capacity,"revocation retains actual image and metadata");
      auto denied=file.Read(f);NoPage(denied);Check(denied.error==ME::memory_binding_failure&&reads==0,"revoked grant prevents new source reads");
      f.memory={};Check(f.manager.Snapshot().current_bytes==capacity,"returned metadata owner survives caller workspace");
      std::thread worker([retained=std::move(read)]()mutable{budget=0;retained={};if(budget!=0)std::abort();budget=-1;});worker.join();f.Empty();
    }
    MemoryFixture valid(capacity);const auto no_writes=writes,no_syncs=syncs;
    {auto guard=file.device.AcquireOperationGuard();observation_mutex=guard.mutex();}
    fenced_reads=0;observation_unlocked=false;
    {auto r=file.Read(valid);Check(r.ok()&&fenced_reads==2&&!observation_unlocked,"same actual fence held over bootstrap and catalog_root reads");}
    observation_mutex=nullptr;
    const auto original_binding=file.expected;
    for(unsigned invalid=0;invalid<7;++invalid){
      if(invalid==0)file.expected.page_number=0;
      if(invalid==1)file.expected.page_number=std::numeric_limits<u64>::max();
      if(invalid==2)file.expected.page_generation=0;
      if(invalid==3)file.expected.page_type=0x30e;
      if(invalid==4)file.expected.filespace.filespace_uuid={};
      if(invalid==5)file.expected.filespace.page_size_profile_uuid=Id(99);
      if(invalid==6)file.expected.page_uuid=Uuid{};
      auto r=file.Read(valid);NoPage(r);Check(r.error==ME::invalid_request&&reads==0&&
        !valid.memory.Snapshot().allocation_count,"invalid typed page binding refuses before physical admission");
      file.expected=original_binding;
    }
    for(unsigned field=0;field<4;++field){auto wrong=valid.binding;
      const std::array<Uuid*,4> fields{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};*fields[field]=Id(99);
      reads=0;auto denied=db::ReadNativeCatalogRootWithMemoryFromOpenDevice(file.device,file.expected,file.value.object_uuid,valid.memory,wrong);
      NoPage(denied);Check(denied.error==ME::memory_binding_failure&&!reads&&!valid.manager.Snapshot().current_bytes,"exact binary memory identity before allocation or I/O");}
    MemoryFixture small(capacity-1);auto short_grant=file.Read(small);NoPage(short_grant);
    Check(short_grant.error==ME::memory_allocation_failure&&!reads&&!small.manager.Snapshot().current_bytes,"one byte short cannot obtain image or metadata");small.memory={};small.Empty();
    file.expected.page_generation++;{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::header_failure,"stale generation binds actual source");}file.expected.page_generation--;
    auto object=db::ReadNativeCatalogRootWithMemoryFromOpenDevice(file.device,file.expected,Id(99),valid.memory,valid.binding);NoPage(object);Check(object.error==ME::object_mismatch,"exact catalog_root object identity");
    for(unsigned n=1;n<=2;++n){fail_read=n;auto r=file.Read(valid);fail_read=0;NoPage(r);
      Check(reads==n&&!valid.manager.Snapshot().current_bytes,"every real read failure releases admitted payload");}
    short_read=2;{auto r=file.Read(valid);Check(r.ok()&&reads==3,"legal short physical read completes");}short_read=0;
    short_read=2;eof_read=3;{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::io_failure&&r.page_bytes_read==file.bytes.size()-1,"partial then EOF retains actual read progress only");}short_read=eof_read=0;
    bootstrap_fault=true;{auto r=file.Read(valid);NoPage(r);Check(!bootstrap_fault&&r.error==ME::bootstrap_failure,"real bootstrap digest failure reached");}
    for(unsigned phase=1;phase<=1;++phase)for(unsigned method=1;method<=4;++method){fault_context=phase;fault=method;
      auto r=file.Read(valid);NoPage(r);Check(fault==0&&r.catalog_root_error==E::hash_failure,"each method in each actual catalog_root digest reached");}
    for(unsigned phase=1;phase<=1;++phase){hash_at=phase;auto r=file.Read(valid);hash_at=0;NoPage(r);
      Check(r.catalog_root_error==E::hash_failure&&hashes==phase,"each actual catalog_root digest context failure typed");}
    Check(writes==no_writes&&syncs==no_syncs,"reader and refusal paths never write or sync source");
    auto corrupt=file.bytes;corrupt.back()^=1;file.Store(corrupt);
    {auto guard=file.device.AcquireOperationGuard();deallocation_device_mutex=guard.mutex();}deallocation_lock_free=false;
    {auto r=file.Read(valid);NoPage(r);Check(r.error==ME::catalog_root_failure&&deallocation_lock_free,"failed image cleanup occurs after releasing device guard");}file.Store(file.bytes);
    file.Bootstrap(0,Id(99));{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::bootstrap_failure,"actual bootstrap identity mismatch");}file.Bootstrap(1);
    {auto r=file.Read(valid);NoPage(r);Check(r.error==ME::encrypted_requires_authority&&reads==1,"encrypted filespace refuses before catalog_root payload I/O");}file.Bootstrap();
    {auto encrypted=file.value;encrypted.header.flags=1;file.Store(Oracle(encrypted));auto r=file.Read(valid);NoPage(r);
      Check(r.error==ME::encrypted_requires_authority,"encrypted common header never parsed as plaintext");}file.Store(file.bytes);
    if(!root){
      for(unsigned member=0;member<5;++member)for(unsigned kind:{2u,6u,7u,8u})for(unsigned generation:{1u,2u})
        for(unsigned types=0;types<(kind==2?64u:2u);++types)for(unsigned flags=0;flags<16;++flags){
          auto v=Example(profile,member,kind,generation,types,flags);const auto image=Oracle(v);
          file.expected.page_type=v.header.page_type;file.Store(image);auto r=file.Read(valid);
          if(flags&1){NoPage(r);Check(r.error==ME::encrypted_requires_authority,"physical encrypted bytes require owning crypto route");}
          else Check(r.ok()&&Oracle(*r.page)==image,"actual kinds generations target roles and native flags preserved");
        }
      file.expected=original_binding;
    }
    file.Store(file.bytes);
    if(!profile){unsigned long sites=0;
      {allocations=0;measuring=true;auto r=file.Read(valid);measuring=false;sites=allocations;Check(r.ok(),"measure full admitted reader allocation sites");}
      for(unsigned long n=0;n<sites;++n){budget=n;auto r=file.Read(valid);budget=-1;
        if(r.ok())Check(Oracle(*r.page)==file.bytes,"optional telemetry loss cannot change decoded result");
        else NoPage(r);
        r={};Check(!valid.manager.Snapshot().current_bytes&&!valid.memory.Snapshot().allocated_bytes,"every allocation fault retains zero physical payload after cleanup");
        {auto retry=file.Read(valid);Check(retry.ok(),"same-owner retry after each allocation failure");}
      }
      Check(sites>0,"allocation sweep executed");std::cout<<"governed catalog_root metadata faults="<<sites<<'\n';
    }
    Check(file.device.Close().ok()&&file.device.Open(file.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"read-only reopen");
    {auto r=file.Read(valid);Check(r.ok()&&file.device.read_only()&&Oracle(*r.page)==file.bytes,"read-only catalog_root inspection");}
    Check(file.device.Close().ok(),"close source");auto closed=file.Read(valid);NoPage(closed);
    Check(closed.error==ME::bootstrap_failure&&closed.bootstrap_error==d::FilespaceBootstrapError::device_not_open,"reader never reopens closed source");
    valid.memory={};valid.Empty();
  }
}

void Codecs(){
  Scratch scratch;
  for(unsigned profile=0;profile<5;++profile)for(unsigned member=0;member<5;++member)
    for(unsigned kind:{2u,6u,7u,8u})for(unsigned generation:{1u,2u})
      for(unsigned types=0;types<(kind==2?64u:2u);++types)for(unsigned flags=0;flags<16;++flags){
        const auto v=Example(profile,member,kind,generation,types,flags);const auto b=Oracle(v);
        allocations=0;measuring=true;budget=0;auto r=scratch.Decode(b);const auto remaining=budget;budget=-1;measuring=false;
        Check(r.ok()&&!allocations&&remaining==0&&r.root->roots.data()==scratch.roots.data(),"all logical root variants use caller backing");
        auto owned=pg::DecodeNativeCatalogRoot(b);auto encoded=pg::EncodeNativeCatalogRoot(v);
        Check(Oracle(*r.root)==b&&owned.ok()&&owned.bytes==b&&encoded.ok()&&encoded.bytes==b,"independent byte oracle versus both codecs");
      }
  const auto p=Example();const auto good=Oracle(p);
  for(unsigned kind:{2u,6u,7u,8u}){
    const auto b=Oracle(Example(0,0,kind));const auto count=kind==2?6:1;
    auto shortfall=pg::DecodeNativeCatalogRootInto(b,std::span(scratch.roots).first(count-1));
    Check(!shortfall.root&&shortfall.error==E::resource_exhausted,"one root-reference slot short");
    for(usize i=0;i<b.size();++i){auto bad=b;bad[i]^=1;Check(!scratch.Decode(bad).root,"every byte of every root family sealed");}
  }
  auto alias=pg::DecodeNativeCatalogRootInto(good,{reinterpret_cast<Entry*>(const_cast<byte*>(good.data())),6});
  Check(!alias.root&&alias.error==E::invalid_backing,"image reference alias before writes");
  auto overflow=pg::DecodeNativeCatalogRootInto(good,{scratch.roots.data(),std::numeric_limits<usize>::max()/sizeof(Entry)+1});
  Check(!overflow.root&&overflow.error==E::invalid_backing,"reference multiplication overflow");
  auto wrap=pg::DecodeNativeCatalogRootInto(good,{reinterpret_cast<Entry*>(std::numeric_limits<std::uintptr_t>::max()-7),6});
  Check(!wrap.root&&wrap.error==E::invalid_backing,"reference address overflow");
  Check(Oracle(p)==good,"invalid backing leaves input unchanged");
  Bytes unaligned(1);unaligned.insert(unaligned.end(),good.begin(),good.end());
  auto retained=scratch.Decode(std::span<const byte>(unaligned).subspan(1));Check(retained.ok(),"unaligned encoded image");
  std::fill(unaligned.begin(),unaligned.end(),0);Check(Oracle(*retained.root)==good,"decoded values outlive encoded input");
  for(unsigned variant=0;variant<34;++variant){auto v=p;E expected=E::invalid_roots;
    switch(variant){
      case 0:v.header.flags=16;expected=E::invalid_header;break;
      case 1:v.object_uuid={};expected=E::invalid_family;break;
      case 2:v.creator_transaction_uuid={};expected=E::invalid_family;break;
      case 3:v.creator_local_transaction_id=0;expected=E::invalid_family;break;
      case 4:v.catalog_generation=0;expected=E::invalid_family;break;
      case 5:v.schema_epoch=0;expected=E::invalid_family;break;
      case 6:v.security_epoch=0;expected=E::invalid_family;break;
      case 7:v.root_kind=0;break;
      case 8:v.header.page_type=10;expected=E::invalid_header;break;
      case 9:v.predecessor=v.roots[0].page;expected=E::invalid_reference;break;
      case 10:v.predecessor_sha256.fill(1);expected=E::invalid_reference;break;
      case 11:v.catalog_generation=2;expected=E::invalid_reference;break;
      case 12:v=Example(0,0,2,2);v.predecessor_sha256={};expected=E::invalid_reference;break;
      case 13:v=Example(0,0,2,2);v.predecessor->filespace_uuid={};expected=E::invalid_reference;break;
      case 14:v=Example(0,0,2,2);v.predecessor=d::NativePageReference{Id(2),19,109,v.header.page_size_profile_uuid};expected=E::invalid_reference;break;
      case 15:v=Example(0,0,2,2);v.predecessor->filespace_uuid=Id(2);v.predecessor->page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;expected=E::invalid_reference;break;
      case 16:v.roots.pop_back();break;
      case 17:v.roots.push_back(v.roots.back());break;
      case 18:v.roots[0].role=2;break;
      case 19:v.roots[0].page_type=7;break;
      case 20:v.roots[0].object_uuid={};break;
      case 21:v.roots[0].page.filespace_uuid={};break;
      case 22:v.roots[0].page.page_number=0;break;
      case 23:v.roots[0].page.page_generation=0;break;
      case 24:v.roots[0].page.page_size_profile_uuid=Id(99);break;
      case 25:v.roots[0].page={Id(2),19,109,v.header.page_size_profile_uuid};break;
      case 26:v.roots[0].page.filespace_uuid=Id(2);v.roots[0].page.page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;break;
      case 27:v=Example(0,0,2,2);v.roots[0].page=*v.predecessor;break;
      case 28:v.roots[1].page=v.roots[0].page;break;
      case 29:v.roots[1].object_uuid=v.roots[0].object_uuid;break;
      case 30:v.roots[1].page.page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;break;
      case 31:v.roots[0].page.page_number=std::numeric_limits<u64>::max();break;
      case 32:v.header.page_number=0;expected=E::invalid_header;break;
      case 33:v.roots.clear();break;
    }
    const auto b=Oracle(v);auto r=scratch.Decode(b);auto owned=pg::DecodeNativeCatalogRoot(b);
    if(r.error!=expected||owned.error!=expected)std::cerr<<"variant="<<variant<<" borrowed="<<unsigned(r.error)<<" owned="<<unsigned(owned.error)<<" expected="<<unsigned(expected)<<'\n';
    Check(!r.root&&!owned.root&&r.error==expected&&owned.error==expected,"independent expected semantic error without prefix");
    MemoryFile file(0);file.Store(b);MemoryFixture f(db::NativeCatalogRootWorkspaceBytes(v.header.page_size_profile_uuid));
    auto actual=file.Read(f);NoPage(actual);
    if(variant==0||variant==8||variant==32)Check(actual.error==ME::header_failure,"actual common expected header refuses before root");
    else Check(actual.error==ME::catalog_root_failure&&actual.catalog_root_error==expected,"actual resealed root refusal preserves nested reason");
  }
  for(unsigned kind:{6u,7u,8u}){auto v=Example(0,0,kind);v.roots[0].role=1;
    Check(scratch.Decode(Oracle(v)).error==E::invalid_roots,"each dedicated kind requires its exact role");}
  for(unsigned i=0;i<6;++i)for(unsigned j=0;j<6;++j)if(i!=j)for(bool object:{false,true}){
    auto v=p;if(object)v.roots[j].object_uuid=v.roots[i].object_uuid;else v.roots[j].page=v.roots[i].page;
    Check(scratch.Decode(Oracle(v)).error==E::invalid_roots,"all ordered duplicate object and physical-slot pairs");
  }
  for(unsigned at:{128u,136u,138u,140u,146u,148u,336u,383u,386u,456u,8191u}){
    auto b=good;b[at]^=0x80;Seal(b);Check(!scratch.Decode(b).root,"resealed family entry reserved and tail failures");}
  hashes=0;hash_at=1;auto failed=scratch.Decode(good);hash_at=0;
  Check(!failed.root&&failed.error==E::hash_failure,"provider context failure");
  for(unsigned method=1;method<=4;++method){hashes=0;fault_context=1;fault=method;auto r=scratch.Decode(good);
    Check(!fault&&!r.root&&r.error==E::hash_failure,"each provider digest phase failure");}
  for(unsigned size:{0u,127u,383u,8191u,8193u}){auto b=good;b.resize(size);Check(!scratch.Decode(b).root,"invalid page length");}
  for(unsigned at=0;at<16;++at)for(unsigned field=0;field<5;++field){
    auto v=p;v.roots[0].object_uuid=Id(10000); // Keep byte mutations distinct from the five other target objects.
    std::array<Uuid*,5> ids{&v.object_uuid,&v.creator_transaction_uuid,&v.roots[0].object_uuid,&v.roots[0].page.filespace_uuid,&v.header.page_uuid};
    ids[field]->bytes[at]^=1;const auto b=Oracle(v);auto r=scratch.Decode(b);
    Check(r.ok()&&Oracle(*r.root)==b,"all sixteen bytes of native identities retained");
  }
  auto max=Example(0,0,2,2);max.catalog_generation=max.schema_epoch=max.security_epoch=max.resource_epoch=max.creator_local_transaction_id=std::numeric_limits<u64>::max();
  Check(scratch.Decode(Oracle(max)).ok(),"maximum uint64 epochs and transaction number");
}
}
int main(){try{Codecs();MemoryTests();std::cout<<"PASS governed catalog-root checks="<<checks<<" not_SQL_E2E=true\n";return 0;}
  catch(...){budget=-1;std::cerr<<"FAIL checks="<<checks<<'\n';return 1;}}
