// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_horizon_memory.hpp"
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
using Entry=pg::NativeHorizonRecord;
using Bytes=std::vector<byte>;using E=pg::NativeHorizonError;
void Check(bool ok,const char* why,std::source_location at=std::source_location::current()){
  ++checks;if(!ok){std::cerr<<at.line()<<": "<<why<<'\n';throw why;}}
template<class F> auto DenyCodecAllocation(F&& call){
  const auto saved=budget;budget=0;
  auto result=call();const bool unchanged=budget==0;budget=saved;
  Check(unchanged,"native codec provider refusal must not allocate diagnostic text");
  return result;
}
Uuid Id(unsigned n){Uuid id;id.bytes[6]=0x70;id.bytes[8]=0x80;for(unsigned i=0;i<4;++i)id.bytes[15-i]=byte(n>>(8*i));return id;}
void Num(Bytes& b,usize at,unsigned n,u64 v){for(unsigned i=0;i<n;++i)b[at+i]=byte(v>>(8*i));}
void Put(Bytes& b,usize at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}
void Ref(Bytes& b,usize at,const d::NativePageReference& p){Put(b,at,p.filespace_uuid);Num(b,at+16,8,p.page_number);Num(b,at+24,8,p.page_generation);Put(b,at+32,p.page_size_profile_uuid);}
auto Hash(const Bytes& b){std::array<byte,32> h{};Check(SHA256(b.data(),b.size(),h.data())!=nullptr,"independent SHA256");return h;}

void Seal(Bytes& b){std::fill(b.begin()+408,b.begin()+440,0);const auto h=Hash(b);std::copy(h.begin(),h.end(),b.begin()+408);}
template<class Page> Bytes Oracle(const Page& v){
  const auto& h=v.header;Bytes b(h.page_size_bytes,0);
  std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,h.page_type);Num(b,20,2,1);Num(b,22,2,1);
  Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);
  Num(b,88,8,h.flags);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);
  u64 fnv=14695981039346656037ull;for(unsigned i=0;i<128;++i){fnv^=b[i];fnv*=1099511628211ull;}Num(b,96,8,fnv);
  std::copy_n("SBHOR002",8,b.begin()+128);Num(b,136,2,2);Num(b,138,2,384);Num(b,140,4,512+192*v.records.size());
  Put(b,144,v.object_uuid);Num(b,160,8,v.epoch);Put(b,168,v.creator_transaction_uuid);Num(b,184,8,v.creator_local_transaction_id);
  Num(b,192,8,v.flags);Num(b,200,8,v.total_records);Num(b,208,8,v.first_record);Num(b,216,4,v.records.size());
  Ref(b,224,v.retention);Put(b,272,v.retention_object_uuid);std::copy(v.retention_sha256.begin(),v.retention_sha256.end(),b.begin()+288);
  if(v.next)Ref(b,320,*v.next);std::copy(v.next_sha256.begin(),v.next_sha256.end(),b.begin()+368);Num(b,400,8,v.minimum_blocker);
  for(usize i=0;i<v.records.size();++i){const auto& r=v.records[i];const auto n=512+192*i;
    Num(b,n,2,unsigned(r.kind));Num(b,n+2,2,unsigned(r.owner_kind));Num(b,n+4,4,r.flags);Num(b,n+8,8,r.local_boundary);
    Put(b,n+16,r.owner_uuid);Put(b,n+32,r.pin_uuid);Put(b,n+48,r.checkpoint_object_uuid);Num(b,n+64,8,r.checkpoint_generation);
    Put(b,n+72,r.diagnostic_uuid);if(r.checkpoint)Ref(b,n+88,*r.checkpoint);Put(b,n+136,r.horizon_uuid);Put(b,n+152,r.timeline_uuid);
  }
  Seal(b);return b;
}
pg::NativeHorizonRoot Example(unsigned profile=0,unsigned member=0,unsigned count=2,unsigned kind=1,unsigned owner=1,unsigned flags=1,unsigned cluster=0){
  const auto& own=d::kCanonicalFilespacePageProfiles[profile];const auto& target=d::kCanonicalFilespacePageProfiles[member];
  pg::NativeHorizonRoot p;p.header={own.page_size_bytes,0x302,Id(1),Id(2),Id(10),19,109,0,own.uuid};
  p.object_uuid=Id(20);p.epoch=7;p.creator_transaction_uuid=Id(21);p.creator_local_transaction_id=42;p.flags=cluster;
  p.retention={Id(3),6,11,target.uuid};p.retention_object_uuid=Id(22);p.retention_sha256.fill(0x39);
  if(count){p.total_records=count+1;p.next=d::NativePageReference{Id(3),7,12,target.uuid};p.next_sha256.fill(0x5c);}
  for(unsigned i=0;i<count;++i){Entry r;r.kind=pg::NativeHorizonKind(kind);r.owner_kind=pg::NativeHorizonOwner(owner);r.flags=flags;
    r.local_boundary=100+i;r.owner_uuid=Id(200+i);r.pin_uuid=Id(400+i);r.checkpoint_object_uuid=Id(23);r.checkpoint_generation=18;
    r.checkpoint=d::NativePageReference{Id(3),8,13,target.uuid};if(flags)r.diagnostic_uuid=Id(24);
    r.horizon_uuid=Id(1000+i);r.timeline_uuid=Id(30);p.records.push_back(r);}
  if(count&&(flags&1))p.minimum_blocker=100;
  return p;
}
// Independent tabular admissibility: hard-stop masks require blocking;
// provider-owned rows require configured cluster membership.
bool ValidRecord(unsigned kind,unsigned owner,unsigned flags,unsigned cluster){
  constexpr std::array<bool,16> allowed{true,true,true,true,false,true,false,true,true,true,true,true,false,true,false,true};
  if(!kind||kind>18||!owner||owner>8||flags>=allowed.size()||!allowed[flags])return false;
  if(kind>=11)return owner==5&&cluster==1;
  return owner!=5||cluster==1;
}
using Identity=pg::NativeHorizonIdentityScratch;
using Reference=pg::NativeHorizonReferenceScratch;
struct Scratch {
  std::vector<Entry> records;
  std::vector<Identity> identities;
  std::vector<Reference> references;
  explicit Scratch(usize n):records(n),identities(n),references(n+3){}
  auto Decode(std::span<const byte> b){return pg::DecodeNativeHorizonRootInto(b,records,identities,references);}
};
namespace m=scratchbird::core::memory;
using ME=db::NativeHorizonMemoryError;
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
    r.route_label="storage.horizon.conformance";r.purpose="actual horizon image and metadata";
    r.binary_operation_uuid=binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
    r.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(64).bytes},
      {m::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
    r.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label="horizon resource conformance";
    for(const auto& scope:r.scope_chain){m::HierarchicalMemoryBudget b;b.scope=scope;b.hard_limit_bytes=bytes;
      b.provenance=r.provenance;Check(ledger.SetBudget(b).ok(),"actual parent budget");}
    auto grant=m::AcquireReservationBackedMemoryResource(r);Check(grant.ok(),"actual node-issued metadata grant");
    auto adopted=db::AdoptNativeStorageMemory(binding,grant.resource);Check(adopted.ok()&&!grant.resource,"exclusive native adoption");
    memory=std::move(adopted.memory);
  }
  void Empty(){const auto s=manager.Snapshot();Check(!s.current_bytes&&!s.reserved_capacity_bytes&&
    !s.active_capacity_reservation_count&&!ledger.Snapshot().current_bytes,"all real horizon charges released");}
};
struct MemoryFile {
  std::filesystem::path directory,path;
  d::FileDevice device;
  pg::NativeHorizonRoot value;
  Bytes bytes;
  d::NativeCommonPageHeaderBinding expected;
  explicit MemoryFile(unsigned profile,bool root=false):value(Example(profile,0,root?0:2)),bytes(Oracle(value)){
    char name[]="/tmp/sb-horizon-memory-XXXXXX";const auto* made=mkdtemp(name);if(!made)throw std::runtime_error("mkdtemp");
    directory=made;path=directory/"native.bin";
    Check(device.Open(path.string(),d::FileOpenMode::create_new).ok(),"actual horizon source");
    expected={{value.header.database_uuid,value.header.filespace_uuid,value.header.page_size_profile_uuid},
      value.header.page_number,value.header.page_generation,value.header.page_type,value.header.page_uuid};
    Bootstrap();Store(bytes);Check(device.Sync().ok()&&device.Close().ok()&&
      device.Open(path.string(),d::FileOpenMode::open_existing).ok(),"cold reopen independent horizon image");
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
  void Store(const Bytes& image){Check(device.WriteAt(value.header.page_number*u64(value.header.page_size_bytes),image.data(),image.size()).ok()&&device.Sync().ok(),"actual independent horizon");}
  auto Read(MemoryFixture& f){reads=hashes=0;return db::ReadNativeHorizonWithMemoryFromOpenDevice(device,expected,value.object_uuid,f.memory,f.binding);}
};
void NoPage(const db::NativeHorizonMemoryResult& r){Check(!r.ok()&&!r.page&&r.image.empty()&&!r.arena,"refusal exposes no image metadata or owner prefix");}
void MemoryTests(){
  for(unsigned profile=0;profile<5;++profile)for(bool root:{false,true}){MemoryFile file(profile,root);
    const auto capacity=db::NativeHorizonWorkspaceBytes(file.value.header.page_size_profile_uuid);
    const auto n=(file.bytes.size()-512)/192;
    Check(capacity==file.bytes.size()+n*(sizeof(Entry)+sizeof(Identity))+(n+3)*sizeof(Reference)+3*(alignof(std::max_align_t)-1),"independent native backing size formula");
    {
      MemoryFixture f(capacity);
      {auto guard=file.device.AcquireOperationGuard();allocation_device_mutex=guard.mutex();}
      allocation_lock_free=false;auto read=file.Read(f);
      Check(read.ok()&&allocation_lock_free,"real metadata backing allocated outside device guard");
      Check(last_read_buffer==read.image.data()&&read.image.size()==file.bytes.size()&&
        std::equal(read.image.begin(),read.image.end(),file.bytes.begin(),file.bytes.end()),"actual read destination is returned charged image");
      const auto begin=reinterpret_cast<std::uintptr_t>(read.image.data());
      const auto inside=[&](const void* ptr,usize bytes){auto n=reinterpret_cast<std::uintptr_t>(ptr);return n>=begin&&n-begin<=capacity&&bytes<=capacity-(n-begin);};
      Check((root||inside(read.page->records.data(),read.page->records.size_bytes()))&&
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
    {auto r=file.Read(valid);Check(r.ok()&&fenced_reads==2&&!observation_unlocked,"same actual fence held over bootstrap and horizon reads");}
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
      reads=0;auto denied=db::ReadNativeHorizonWithMemoryFromOpenDevice(file.device,file.expected,file.value.object_uuid,valid.memory,wrong);
      NoPage(denied);Check(denied.error==ME::memory_binding_failure&&!reads&&!valid.manager.Snapshot().current_bytes,"exact binary memory identity before allocation or I/O");}
    MemoryFixture small(capacity-1);auto short_grant=file.Read(small);NoPage(short_grant);
    Check(short_grant.error==ME::memory_allocation_failure&&!reads&&!small.manager.Snapshot().current_bytes,"one byte short cannot obtain image or metadata");small.memory={};small.Empty();
    file.expected.page_generation++;{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::header_failure,"stale generation binds actual source");}file.expected.page_generation--;
    auto object=db::ReadNativeHorizonWithMemoryFromOpenDevice(file.device,file.expected,Id(99),valid.memory,valid.binding);NoPage(object);Check(object.error==ME::object_mismatch,"exact horizon object identity");
    for(unsigned n=1;n<=2;++n){fail_read=n;auto r=file.Read(valid);fail_read=0;NoPage(r);
      Check(reads==n&&!valid.manager.Snapshot().current_bytes,"every real read failure releases admitted payload");}
    short_read=2;{auto r=file.Read(valid);Check(r.ok()&&reads==3,"legal short physical read completes");}short_read=0;
    short_read=2;eof_read=3;{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::io_failure&&r.page_bytes_read==file.bytes.size()-1,"partial then EOF retains actual read progress only");}short_read=eof_read=0;
    bootstrap_fault=true;{auto r=file.Read(valid);NoPage(r);Check(!bootstrap_fault&&r.error==ME::bootstrap_failure,"real bootstrap digest failure reached");}
    for(unsigned phase=1;phase<=1;++phase)for(unsigned method=1;method<=4;++method){fault_context=phase;fault=method;
      auto r=file.Read(valid);NoPage(r);Check(fault==0&&r.horizon_error==E::hash_failure,"each method in each actual horizon digest reached");}
    for(unsigned phase=1;phase<=1;++phase){hash_at=phase;auto r=file.Read(valid);hash_at=0;NoPage(r);
      Check(r.horizon_error==E::hash_failure&&hashes==phase,"each actual horizon digest context failure typed");}
    Check(writes==no_writes&&syncs==no_syncs,"reader and refusal paths never write or sync source");
    auto corrupt=file.bytes;corrupt.back()^=1;file.Store(corrupt);
    {auto guard=file.device.AcquireOperationGuard();deallocation_device_mutex=guard.mutex();}deallocation_lock_free=false;
    {auto r=file.Read(valid);NoPage(r);Check(r.error==ME::horizon_failure&&deallocation_lock_free,"failed image cleanup occurs after releasing device guard");}file.Store(file.bytes);
    file.Bootstrap(0,Id(99));{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::bootstrap_failure,"actual bootstrap identity mismatch");}file.Bootstrap(1);
    {auto r=file.Read(valid);NoPage(r);Check(r.error==ME::encrypted_requires_authority&&reads==1,"encrypted filespace refuses before horizon payload I/O");}file.Bootstrap();
    {auto encrypted=file.value;encrypted.header.flags=1;file.Store(Oracle(encrypted));auto r=file.Read(valid);NoPage(r);
      Check(r.error==ME::encrypted_requires_authority,"encrypted common header never parsed as plaintext");}file.Store(file.bytes);
    if(!root){
      for(unsigned member=0;member<5;++member)for(unsigned kind=1;kind<=18;++kind)for(unsigned owner=1;owner<=8;++owner)
        for(unsigned flags=0;flags<16;++flags)for(unsigned cluster=0;cluster<2;++cluster){
          auto v=Example(profile,member,2,kind,owner,flags,cluster);const auto image=Oracle(v);file.Store(image);auto r=file.Read(valid);
          const bool expected=ValidRecord(kind,owner,flags,cluster);Check(r.ok()==expected,"actual horizon record truth table");
          if(expected)Check(Oracle(*r.page)==image,"actual binary records and references retained");
          else {NoPage(r);Check(r.horizon_error==E::invalid_record,"invalid combination refuses");}
        }
      auto maximum=Example(profile,profile,(file.bytes.size()-512)/192);auto image=Oracle(maximum);file.Store(image);auto r=file.Read(valid);
      Check(r.ok()&&r.page->records.size()==maximum.records.size()&&Oracle(*r.page)==image,"actual maximum native records and scratch");
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
      Check(sites>0,"allocation sweep executed");std::cout<<"governed horizon metadata faults="<<sites<<'\n';
    }
    Check(file.device.Close().ok()&&file.device.Open(file.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"read-only reopen");
    {auto r=file.Read(valid);Check(r.ok()&&file.device.read_only()&&Oracle(*r.page)==file.bytes,"read-only horizon inspection");}
    Check(file.device.Close().ok(),"close source");auto closed=file.Read(valid);NoPage(closed);
    Check(closed.error==ME::bootstrap_failure&&closed.bootstrap_error==d::FilespaceBootstrapError::device_not_open,"reader never reopens closed source");
    valid.memory={};valid.Empty();
  }
}

void Codecs(){
  for(unsigned profile=0;profile<5;++profile){Scratch scratch((d::kCanonicalFilespacePageProfiles[profile].page_size_bytes-512)/192);
    for(unsigned member=0;member<5;++member)for(unsigned kind=1;kind<=18;++kind)for(unsigned owner=1;owner<=8;++owner)
      for(unsigned flags=0;flags<16;++flags)for(unsigned cluster=0;cluster<2;++cluster){
        auto v=Example(profile,member,2,kind,owner,flags,cluster);auto b=Oracle(v);
        allocations=0;measuring=true;budget=0;auto r=scratch.Decode(b);const auto remaining=budget;budget=-1;measuring=false;
        const bool expected=ValidRecord(kind,owner,flags,cluster);auto owned=pg::DecodeNativeHorizonRoot(b);
        Check(r.ok()==expected&&owned.ok()==expected&&r.error==owned.error&&!allocations&&remaining==0,"independent truth table and owning parity without hidden allocation");
        if(expected)Check(r.root->records.data()==scratch.records.data()&&Oracle(*r.root)==b&&owned.bytes==b,"exact caller-backed record values");
        else Check(!r.root&&!owned.root&&owned.bytes.empty(),"invalid flags expose no prefix");
      }
    for(unsigned n:{0u,1u,unsigned(scratch.records.size()-1),unsigned(scratch.records.size())}){
      auto v=Example(profile,profile,n);auto b=Oracle(v);budget=0;auto r=scratch.Decode(b);auto remaining=budget;budget=-1;
      Check(r.ok()&&remaining==0&&r.root->records.size()==n&&Oracle(*r.root)==b,"empty single and maximum pages use caller backing");
      if(n)for(unsigned field=0;field<3;++field){
        auto shortfall=pg::DecodeNativeHorizonRootInto(b,std::span(scratch.records).first(field==0?n-1:n),
          std::span(scratch.identities).first(field==1?n-1:n),std::span(scratch.references).first(field==2?n+2:n+3));
        Check(!shortfall.root&&shortfall.error==E::resource_exhausted,"each native scratch region one entry short");
      }
    }
  }
  Scratch scratch(128);auto p=Example();const auto good=Oracle(p);
  for(unsigned count:{0u,2u}){auto b=Oracle(Example(0,0,count));
    for(usize i=0;i<b.size();++i){auto bad=b;bad[i]^=1;Check(!scratch.Decode(bad).root,"every image byte integrity failure");}}
  // Every region pair is rejected before touching any caller memory.
  auto invalid=[&](std::span<const byte> b,std::span<Entry> r,std::span<Identity> i,std::span<Reference> refs){
    const auto out=pg::DecodeNativeHorizonRootInto(b,r,i,refs);
    Check(!out.root&&out.error==E::invalid_backing,"alias or arithmetic overflow rejected");};
  auto* encoded=const_cast<byte*>(good.data());
  invalid(good,{reinterpret_cast<Entry*>(encoded),2},scratch.identities,scratch.references);
  invalid(good,scratch.records,{reinterpret_cast<Identity*>(encoded),2},scratch.references);
  invalid(good,scratch.records,scratch.identities,{reinterpret_cast<Reference*>(encoded),5});
  invalid(good,scratch.records,{reinterpret_cast<Identity*>(scratch.records.data()),2},scratch.references);
  invalid(good,scratch.records,scratch.identities,{reinterpret_cast<Reference*>(scratch.records.data()),5});
  invalid(good,scratch.records,scratch.identities,{reinterpret_cast<Reference*>(scratch.identities.data()),5});
  invalid(good,{scratch.records.data(),std::numeric_limits<usize>::max()/sizeof(Entry)+1},scratch.identities,scratch.references);
  invalid(good,scratch.records,{scratch.identities.data(),std::numeric_limits<usize>::max()/sizeof(Identity)+1},scratch.references);
  invalid(good,scratch.records,scratch.identities,{scratch.references.data(),std::numeric_limits<usize>::max()/sizeof(Reference)+1});
  invalid(good,{reinterpret_cast<Entry*>(std::numeric_limits<std::uintptr_t>::max()-7),2},scratch.identities,scratch.references);
  Check(Oracle(p)==good,"alias refusals did not change input");
  Bytes unaligned(1);unaligned.insert(unaligned.end(),good.begin(),good.end());
  auto r=scratch.Decode(std::span<const byte>(unaligned).subspan(1));Check(r.ok(),"unaligned encoded input");
  std::fill(unaligned.begin(),unaligned.end(),0);Check(Oracle(*r.root)==good,"native values independent from image lifetime");
  // Independent expected error categories, including the validation precedence.
  for(unsigned variant=0;variant<39;++variant){auto v=p;auto& rec=v.records[0];E expected=E::invalid_record;
    switch(variant){
      case 0:v.object_uuid={};expected=E::invalid_family;break;
      case 1:v.creator_transaction_uuid={};expected=E::invalid_family;break;
      case 2:v.epoch=0;expected=E::invalid_family;break;
      case 3:v.creator_local_transaction_id=0;expected=E::invalid_family;break;
      case 4:v.flags=2;expected=E::invalid_family;break;
      case 5:v.total_records=0;expected=E::invalid_family;break;
      case 6:v.first_record=v.total_records+1;expected=E::invalid_family;break;
      case 7:v.retention_object_uuid={};expected=E::invalid_family;break;
      case 8:v.retention_sha256={};expected=E::invalid_family;break;
      case 9:v.next.reset();v.next_sha256={};expected=E::invalid_reference;break;
      case 10:v.next_sha256={};expected=E::invalid_reference;break;
      case 11:v.next->page_number=0;expected=E::invalid_reference;break;
      case 12:v.next=d::NativePageReference{Id(2),19,109,v.header.page_size_profile_uuid};expected=E::invalid_reference;break;
      case 13:v.next->filespace_uuid=Id(2);v.next->page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;expected=E::invalid_reference;break;
      case 14:rec.owner_uuid={};break;case 15:rec.horizon_uuid={};break;case 16:rec.timeline_uuid={};break;
      case 17:rec.pin_uuid.bytes[6]=0;break;case 18:rec.kind=pg::NativeHorizonKind(0);break;case 19:rec.kind=pg::NativeHorizonKind(19);break;
      case 20:rec.owner_kind=pg::NativeHorizonOwner(0);break;case 21:rec.owner_kind=pg::NativeHorizonOwner(9);break;
      case 22:rec.local_boundary=0;break;case 23:rec.flags=16;break;case 24:rec.diagnostic_uuid={};break;
      case 25:rec.flags=0;break;case 26:v.minimum_blocker++;break;
      case 27:rec.checkpoint_object_uuid={};expected=E::invalid_reference;break;
      case 28:rec.checkpoint_generation=0;expected=E::invalid_reference;break;
      case 29:rec.checkpoint.reset();expected=E::invalid_reference;break;
      case 30:v.records[1].horizon_uuid=rec.horizon_uuid;break;
      case 31:v.records[1].owner_uuid=rec.owner_uuid;break;
      case 32:v.records[1].checkpoint_generation++;expected=E::invalid_reference;break;
      case 33:v.records[1].checkpoint_object_uuid=Id(88);expected=E::invalid_reference;break;
      case 34:v.records[1].checkpoint->page_generation++;expected=E::invalid_reference;break;
      case 35:v.records[1].checkpoint->page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;expected=E::invalid_reference;break;
      case 36:rec.checkpoint=v.retention;expected=E::invalid_reference;break;
      case 37:v.retention=*v.next;expected=E::invalid_reference;break;
      case 38:rec.checkpoint->page_number=std::numeric_limits<u64>::max();expected=E::invalid_reference;break;
    }
    const auto b=Oracle(v);const auto borrowed=scratch.Decode(b);const auto owned=pg::DecodeNativeHorizonRoot(b);
    Check(!borrowed.root&&!owned.root&&borrowed.error==expected&&owned.error==expected,"resealed rule failure exact independent diagnostic");
    // Same malformed bytes exercise actual read into governed backing too.
    MemoryFile file(0);file.Store(b);MemoryFixture f(db::NativeHorizonWorkspaceBytes(v.header.page_size_profile_uuid));
    auto actual=file.Read(f);NoPage(actual);Check(actual.error==ME::horizon_failure&&actual.horizon_error==expected,"actual file semantic refusal matches independent oracle");
  }
  for(unsigned a=0;a<8;++a)for(unsigned b=0;b<8;++b)if(a!=b){
    auto v=Example(0,0,8);v.records[b].horizon_uuid=v.records[a].horizon_uuid;
    auto r=scratch.Decode(Oracle(v));Check(!r.root&&r.error==E::invalid_record,"all duplicate identity ordinal pairs");
  }
  // A prior checkpoint error wins over a later duplicate, just as in owning validation.
  auto order=p;order.records[0].checkpoint_generation=0;order.records[1].horizon_uuid=order.records[0].horizon_uuid;
  Check(scratch.Decode(Oracle(order)).error==E::invalid_reference,"original per-record error ordering");
  for(unsigned at:{128u,136u,138u,140u,220u,440u,511u,680u,8191u}){
    auto b=good;b[at]^=0x80;Seal(b);Check(!scratch.Decode(b).root,"resealed family record reserved and tail failure");}
  for(unsigned method=1;method<=4;++method){hashes=0;fault_context=1;fault=method;auto failed=DenyCodecAllocation([&]{return scratch.Decode(good);});
    Check(!fault&&!failed.root&&failed.error==E::hash_failure,"every digest phase failure");}
  hashes=0;hash_at=1;auto failed=DenyCodecAllocation([&]{return scratch.Decode(good);});hash_at=0;
  Check(!failed.root&&failed.error==E::hash_failure,"digest context failure");
  for(unsigned size:{0u,127u,511u,8191u,8193u}){auto b=good;b.resize(size);Check(!scratch.Decode(b).root,"invalid image length");}
  for(unsigned field=0;field<6;++field)for(unsigned at=0;at<16;++at){auto v=Example(0,0,1);auto& r=v.records[0];
    const std::array<Uuid*,6> ids{&r.pin_uuid,&r.owner_uuid,&r.timeline_uuid,&r.horizon_uuid,&r.checkpoint_object_uuid,&r.diagnostic_uuid};
    ids[field]->bytes[at]^=1;const auto b=Oracle(v);auto decoded=scratch.Decode(b);
    Check(decoded.ok()&&Oracle(*decoded.root)==b,"every byte of native identities retained");}
  for(unsigned at=0;at<16;++at){auto v=p;v.records[1].horizon_uuid=v.records[0].horizon_uuid;v.records[1].horizon_uuid.bytes[at]^=1;
    Check(scratch.Decode(Oracle(v)).ok(),"uniqueness compares all sixteen identity bytes");}
  auto terminal=p;terminal.total_records=2;terminal.next.reset();terminal.next_sha256={};
  Check(scratch.Decode(Oracle(terminal)).ok(),"terminal page no continuation");
  terminal.first_record=9;terminal.total_records=11;Check(scratch.Decode(Oracle(terminal)).ok(),"nonzero ordinal");
  auto absent=p;for(auto& r:absent.records){r.checkpoint.reset();r.checkpoint_object_uuid={};r.checkpoint_generation=0;r.pin_uuid={};}
  Check(scratch.Decode(Oracle(absent)).ok(),"absent optional checkpoint and pin remain absent");
  auto maximum=Example(0,0,1);maximum.total_records=std::numeric_limits<u64>::max();maximum.first_record=maximum.total_records-2;
  maximum.epoch=maximum.creator_local_transaction_id=maximum.total_records;maximum.records[0].local_boundary=maximum.minimum_blocker=maximum.total_records;
  Check(scratch.Decode(Oracle(maximum)).ok(),"maximum native counters preserve uint64 boundaries");
}
}
int main(){try{Codecs();MemoryTests();std::cout<<"PASS governed horizon checks="<<checks<<" not_SQL_E2E=true\n";return 0;}
  catch(...){budget=-1;std::cerr<<"FAIL checks="<<checks<<'\n';return 1;}}
