// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_retention_memory.hpp"
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
using Entry=pg::NativeRetentionPin;
using Bytes=std::vector<byte>;using E=pg::NativeRetentionError;
void Check(bool ok,const char* why,std::source_location at=std::source_location::current()){
  ++checks;if(!ok){std::cerr<<at.line()<<": "<<why<<'\n';throw why;}}
Uuid Id(unsigned n){Uuid id;id.bytes[6]=0x70;id.bytes[8]=0x80;for(unsigned i=0;i<4;++i)id.bytes[15-i]=byte(n>>(8*i));return id;}
void Num(Bytes& b,usize at,unsigned n,u64 v){for(unsigned i=0;i<n;++i)b[at+i]=byte(v>>(8*i));}
void Put(Bytes& b,usize at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}
void Ref(Bytes& b,usize at,const d::NativePageReference& p){Put(b,at,p.filespace_uuid);Num(b,at+16,8,p.page_number);Num(b,at+24,8,p.page_generation);Put(b,at+32,p.page_size_profile_uuid);}
auto Hash(const Bytes& b){std::array<byte,32> h{};Check(SHA256(b.data(),b.size(),h.data())!=nullptr,"independent SHA256");return h;}


void Seal(Bytes& b){std::fill(b.begin()+328,b.begin()+360,0);const auto h=Hash(b);std::copy(h.begin(),h.end(),b.begin()+328);}
void RecordSeal(Bytes& b,usize at){std::fill(b.begin()+at+112,b.begin()+at+144,0);std::array<byte,32> h{};
  Check(SHA256(b.data()+at,160,h.data())!=nullptr,"independent record SHA");std::copy(h.begin(),h.end(),b.begin()+at+112);}
template<class Page> Bytes Oracle(const Page& v){const auto& h=v.header;Bytes b(h.page_size_bytes,0);
  std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,h.page_type);Num(b,20,2,1);Num(b,22,2,1);
  Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Num(b,88,8,h.flags);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);
  u64 fnv=14695981039346656037ull;for(unsigned i=0;i<128;++i){fnv^=b[i];fnv*=1099511628211ull;}Num(b,96,8,fnv);
  std::copy_n(h.page_type==0x303?"SBPIN001":"SBPINL01",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,256);Num(b,140,4,384+160*v.records.size());
  Put(b,144,v.object_uuid);Num(b,160,8,v.epoch);Put(b,168,v.creator_transaction_uuid);Num(b,184,8,v.creator_local_transaction_id);
  Num(b,192,8,v.flags);Num(b,200,8,v.total_pins);Num(b,208,8,v.first_record);Num(b,216,4,v.records.size());
  if(v.next)Ref(b,224,*v.next);std::copy(v.next_sha256.begin(),v.next_sha256.end(),b.begin()+272);
  Num(b,304,8,v.legal_hold_pins);Num(b,312,8,v.lowest_start);Num(b,320,8,v.highest_end);
  for(usize i=0;i<v.records.size();++i){const auto& r=v.records[i];const auto n=384+160*i;
    Put(b,n,r.pin_uuid);Put(b,n+16,r.owner_uuid);Num(b,n+32,2,unsigned(r.kind));Num(b,n+34,2,unsigned(r.access));
    Num(b,n+36,4,r.flags);Num(b,n+40,8,r.start_local);Num(b,n+48,8,r.end_local);
    Put(b,n+56,r.timeline_uuid);Put(b,n+72,r.filespace_uuid);Num(b,n+88,8,r.retain_until_local);Num(b,n+96,8,r.retain_until_unix_ns);
    Num(b,n+104,8,r.blocked_operations);RecordSeal(b,n);
  }Seal(b);return b;
}
pg::NativeRetentionPage Example(unsigned profile=0,unsigned member=0,unsigned count=2,unsigned kind=1,unsigned access=2,unsigned flags=1,unsigned cluster=0){
  const auto& own=d::kCanonicalFilespacePageProfiles[profile];const auto& target=d::kCanonicalFilespacePageProfiles[member];
  pg::NativeRetentionPage p;p.header={own.page_size_bytes,count?0x304u:0x303u,Id(1),Id(2),Id(10),19,109,0,own.uuid};
  p.object_uuid=Id(20);p.epoch=7;p.creator_transaction_uuid=Id(21);p.creator_local_transaction_id=42;p.flags=cluster;
  if(count){p.total_pins=count+1;p.next=d::NativePageReference{Id(3),6,11,target.uuid};p.next_sha256.fill(0x5c);}
  for(unsigned i=0;i<count;++i){Entry r;r.pin_uuid=Id(100+i);r.owner_uuid=Id(200+i);r.kind=pg::NativeRetentionKind(kind);r.access=pg::NativeRetentionAccess(access);
    r.flags=flags;r.start_local=100+i;r.end_local=200+i;r.timeline_uuid=Id(30);r.filespace_uuid=Id(31);
    r.retain_until_local=1;r.retain_until_unix_ns=1;r.blocked_operations=31;p.records.push_back(r);}
  return p;
}
bool ValidRecord(unsigned kind,unsigned access,unsigned flags,unsigned cluster,unsigned blocked=31){
  if(!blocked||(blocked&~31u))return false;
  if((flags&1)&&!(blocked&1))return false;
  if((flags&4)&&(!(flags&1)||(flags&2)))return false;
  if((kind==3||access==4)&&(flags&2))return false;
  if((access==1||access==7)&&(flags&8))return false;
  return kind!=4||cluster;
}
struct Scratch {
  std::vector<Entry> records;
  explicit Scratch(usize n):records(n){}
  auto Decode(std::span<const byte> b){return pg::DecodeNativeRetentionPageInto(b,records);}
};
namespace m=scratchbird::core::memory;
using ME=db::NativeRetentionMemoryError;
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
    r.route_label="storage.retention.conformance";r.purpose="actual retention image and metadata";
    r.binary_operation_uuid=binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
    r.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(64).bytes},
      {m::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
    r.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label="retention resource conformance";
    for(const auto& scope:r.scope_chain){m::HierarchicalMemoryBudget b;b.scope=scope;b.hard_limit_bytes=bytes;
      b.provenance=r.provenance;Check(ledger.SetBudget(b).ok(),"actual parent budget");}
    auto grant=m::AcquireReservationBackedMemoryResource(r);Check(grant.ok(),"actual node-issued metadata grant");
    auto adopted=db::AdoptNativeStorageMemory(binding,grant.resource);Check(adopted.ok()&&!grant.resource,"exclusive native adoption");
    memory=std::move(adopted.memory);
  }
  void Empty(){const auto s=manager.Snapshot();Check(!s.current_bytes&&!s.reserved_capacity_bytes&&
    !s.active_capacity_reservation_count&&!ledger.Snapshot().current_bytes,"all real retention charges released");}
};
struct MemoryFile {
  std::filesystem::path directory,path;
  d::FileDevice device;
  pg::NativeRetentionPage value;
  Bytes bytes;
  d::NativeCommonPageHeaderBinding expected;
  explicit MemoryFile(unsigned profile,bool root=false):value(Example(profile,0,root?0:2)),bytes(Oracle(value)){
    char name[]="/tmp/sb-retention-memory-XXXXXX";const auto* made=mkdtemp(name);if(!made)throw std::runtime_error("mkdtemp");
    directory=made;path=directory/"native.bin";
    Check(device.Open(path.string(),d::FileOpenMode::create_new).ok(),"actual retention source");
    expected={{value.header.database_uuid,value.header.filespace_uuid,value.header.page_size_profile_uuid},
      value.header.page_number,value.header.page_generation,value.header.page_type,value.header.page_uuid};
    Bootstrap();Store(bytes);Check(device.Sync().ok()&&device.Close().ok()&&
      device.Open(path.string(),d::FileOpenMode::open_existing).ok(),"cold reopen independent retention image");
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
  void Store(const Bytes& image){Check(device.WriteAt(value.header.page_number*u64(value.header.page_size_bytes),image.data(),image.size()).ok()&&device.Sync().ok(),"actual independent retention");}
  auto Read(MemoryFixture& f){reads=hashes=0;return db::ReadNativeRetentionWithMemoryFromOpenDevice(device,expected,value.object_uuid,f.memory,f.binding);}
};
void NoPage(const db::NativeRetentionMemoryResult& r){Check(!r.ok()&&!r.page&&r.image.empty()&&!r.arena,"refusal exposes no image metadata or owner prefix");}
void MemoryTests(){
  for(unsigned profile=0;profile<5;++profile)for(bool root:{false,true}){MemoryFile file(profile,root);
    const auto capacity=db::NativeRetentionWorkspaceBytes(file.value.header.page_size_profile_uuid,file.value.header.page_type);
    Check(capacity==file.bytes.size()+(root?0:((file.bytes.size()-384)/160)*sizeof(Entry)+(alignof(std::max_align_t)-1)),"independent native backing size formula");
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
    {auto r=file.Read(valid);Check(r.ok()&&fenced_reads==2&&!observation_unlocked,"same actual fence held over bootstrap and retention reads");}
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
      reads=0;auto denied=db::ReadNativeRetentionWithMemoryFromOpenDevice(file.device,file.expected,file.value.object_uuid,valid.memory,wrong);
      NoPage(denied);Check(denied.error==ME::memory_binding_failure&&!reads&&!valid.manager.Snapshot().current_bytes,"exact binary memory identity before allocation or I/O");}
    MemoryFixture small(capacity-1);auto short_grant=file.Read(small);NoPage(short_grant);
    Check(short_grant.error==ME::memory_allocation_failure&&!reads&&!small.manager.Snapshot().current_bytes,"one byte short cannot obtain image or metadata");small.memory={};small.Empty();
    file.expected.page_generation++;{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::header_failure,"stale generation binds actual source");}file.expected.page_generation--;
    auto object=db::ReadNativeRetentionWithMemoryFromOpenDevice(file.device,file.expected,Id(99),valid.memory,valid.binding);NoPage(object);Check(object.error==ME::object_mismatch,"exact retention object identity");
    for(unsigned n=1;n<=2;++n){fail_read=n;auto r=file.Read(valid);fail_read=0;NoPage(r);
      Check(reads==n&&!valid.manager.Snapshot().current_bytes,"every real read failure releases admitted payload");}
    short_read=2;{auto r=file.Read(valid);Check(r.ok()&&reads==3,"legal short physical read completes");}short_read=0;
    short_read=2;eof_read=3;{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::io_failure&&r.page_bytes_read==file.bytes.size()-1,"partial then EOF retains actual read progress only");}short_read=eof_read=0;
    bootstrap_fault=true;{auto r=file.Read(valid);NoPage(r);Check(!bootstrap_fault&&r.error==ME::bootstrap_failure,"real bootstrap digest failure reached");}
    for(unsigned phase=1;phase<=1+file.value.records.size();++phase)for(unsigned method=1;method<=4;++method){fault_context=phase;fault=method;
      auto r=file.Read(valid);NoPage(r);Check(fault==0&&r.retention_error==E::hash_failure,"each method in each actual retention digest reached");}
    for(unsigned phase=1;phase<=1+file.value.records.size();++phase){hash_at=phase;auto r=file.Read(valid);hash_at=0;NoPage(r);
      Check(r.retention_error==E::hash_failure&&hashes==phase,"each actual retention digest context failure typed");}
    Check(writes==no_writes&&syncs==no_syncs,"reader and refusal paths never write or sync source");
    auto corrupt=file.bytes;corrupt.back()^=1;file.Store(corrupt);
    {auto guard=file.device.AcquireOperationGuard();deallocation_device_mutex=guard.mutex();}deallocation_lock_free=false;
    {auto r=file.Read(valid);NoPage(r);Check(r.error==ME::retention_failure&&deallocation_lock_free,"failed image cleanup occurs after releasing device guard");}file.Store(file.bytes);
    file.Bootstrap(0,Id(99));{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::bootstrap_failure,"actual bootstrap identity mismatch");}file.Bootstrap(1);
    {auto r=file.Read(valid);NoPage(r);Check(r.error==ME::encrypted_requires_authority&&reads==1,"encrypted filespace refuses before retention payload I/O");}file.Bootstrap();
    {auto encrypted=file.value;encrypted.header.flags=1;file.Store(Oracle(encrypted));auto r=file.Read(valid);NoPage(r);
      Check(r.error==ME::encrypted_requires_authority,"encrypted common header never parsed as plaintext");}file.Store(file.bytes);
    if(!root){
      for(unsigned member=0;member<5;++member)for(unsigned kind=1;kind<=8;++kind)for(unsigned access=1;access<=7;++access)
        for(unsigned flags=0;flags<32;++flags)for(unsigned cluster=0;cluster<2;++cluster){
          auto v=Example(profile,member,2,kind,access,flags,cluster);const auto image=Oracle(v);file.Store(image);auto r=file.Read(valid);
          const bool expected=ValidRecord(kind,access,flags,cluster);Check(r.ok()==expected,"actual kind access flags cluster truth table");
          if(expected)Check(Oracle(*r.page)==image,"actual binary pins preserve expiry and all native values");
          else {NoPage(r);Check(r.retention_error==E::invalid_record,"invalid record combination refuses");}
        }
      auto maximum=Example(profile,profile,(file.bytes.size()-384)/160);auto image=Oracle(maximum);file.Store(image);auto r=file.Read(valid);
      Check(r.ok()&&r.page->records.size()==maximum.records.size()&&Oracle(*r.page)==image,"actual maximum native record backing");
    }else{
      for(unsigned member=0;member<5;++member)for(unsigned legal=0;legal<=2;++legal)for(unsigned end:{0u,200u})for(unsigned cluster:{0u,1u}){
        auto v=Example(profile,member,0);v.total_pins=2;v.legal_hold_pins=legal;v.lowest_start=100;v.highest_end=end;v.flags=cluster;
        v.next=d::NativePageReference{Id(3),6,11,d::kCanonicalFilespacePageProfiles[member].uuid};v.next_sha256.fill(0x5c);
        const auto image=Oracle(v);file.Store(image);auto r=file.Read(valid);
        Check(r.ok()&&r.page->records.empty()&&Oracle(*r.page)==image,"actual empty-record root retains complete declared summaries and next reference");
      }
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
      Check(sites>0,"allocation sweep executed");std::cout<<"governed retention metadata faults="<<sites<<'\n';
    }
    Check(file.device.Close().ok()&&file.device.Open(file.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"read-only reopen");
    {auto r=file.Read(valid);Check(r.ok()&&file.device.read_only()&&Oracle(*r.page)==file.bytes,"read-only retention inspection");}
    Check(file.device.Close().ok(),"close source");auto closed=file.Read(valid);NoPage(closed);
    Check(closed.error==ME::bootstrap_failure&&closed.bootstrap_error==d::FilespaceBootstrapError::device_not_open,"reader never reopens closed source");
    valid.memory={};valid.Empty();
  }
}

void Codecs(){
  for(unsigned profile=0;profile<5;++profile){Scratch scratch((d::kCanonicalFilespacePageProfiles[profile].page_size_bytes-384)/160);
    for(unsigned member=0;member<5;++member)for(unsigned kind=1;kind<=8;++kind)for(unsigned access=1;access<=7;++access)
      for(unsigned flags=0;flags<32;++flags)for(unsigned cluster=0;cluster<2;++cluster){
        auto v=Example(profile,member,2,kind,access,flags,cluster);auto b=Oracle(v);
        allocations=0;measuring=true;budget=0;auto r=scratch.Decode(b);const auto remaining=budget;budget=-1;measuring=false;
        const bool expected=ValidRecord(kind,access,flags,cluster);auto owned=pg::DecodeNativeRetentionPage(b);
        Check(r.ok()==expected&&owned.ok()==expected&&r.error==owned.error&&!allocations&&remaining==0,"independent record truth table and owning parity without hidden allocations");
        if(expected)Check(r.page->records.data()==scratch.records.data()&&Oracle(*r.page)==b&&owned.bytes==b,"exact caller-backed records and independent owned image");
        else Check(!r.page&&!owned.page&&owned.bytes.empty(),"invalid flags produce no usable prefix");
      }
    for(unsigned n:{0u,1u,unsigned(scratch.records.size()-1),unsigned(scratch.records.size())}){
      auto v=Example(profile,profile,n);auto b=Oracle(v);budget=0;auto r=scratch.Decode(b);auto remaining=budget;budget=-1;
      Check(r.ok()&&remaining==0&&r.page->records.size()==n&&Oracle(*r.page)==b,"empty root single and maximum leaves use caller backing");
      if(n){auto shortfall=pg::DecodeNativeRetentionPageInto(b,std::span(scratch.records).first(n-1));
        Check(!shortfall.page&&shortfall.error==E::resource_exhausted,"one native record short");}
    }
  }
  Scratch scratch(128);auto p=Example();const auto good=Oracle(p);
  // Factorized blocked-operation coverage: every record rule crosses every mask
  // at one physical profile; all profile pairs above cover the same field rules.
  for(unsigned kind=1;kind<=8;++kind)for(unsigned access=1;access<=7;++access)for(unsigned flags=0;flags<32;++flags)
    for(unsigned cluster=0;cluster<2;++cluster)for(unsigned blocked=0;blocked<32;++blocked){
      auto v=Example(0,0,1,kind,access,flags,cluster);v.records[0].blocked_operations=blocked;
      auto r=scratch.Decode(Oracle(v));Check(r.ok()==ValidRecord(kind,access,flags,cluster,blocked),"complete blocked-operation mask truth table");
    }
  for(unsigned count:{0u,2u}){auto v=Example(0,0,count);auto b=Oracle(v);
    for(usize i=0;i<b.size();++i){auto bad=b;bad[i]^=1;Check(!scratch.Decode(bad).page,"every-byte root and leaf integrity failure");}}
  auto overlap=pg::DecodeNativeRetentionPageInto(good,{reinterpret_cast<Entry*>(const_cast<byte*>(good.data())),2});
  Check(!overlap.page&&overlap.error==E::invalid_backing,"input and record alias refuses before writes");
  auto overflow=pg::DecodeNativeRetentionPageInto(good,{scratch.records.data(),std::numeric_limits<usize>::max()/sizeof(Entry)+1});
  Check(!overflow.page&&overflow.error==E::invalid_backing,"native backing multiplication overflow refuses");
  Bytes unaligned(1);unaligned.insert(unaligned.end(),good.begin(),good.end());auto r=scratch.Decode(std::span<const byte>(unaligned).subspan(1));
  Check(r.ok(),"unaligned encoded input");std::fill(unaligned.begin(),unaligned.end(),0);
  Check(Oracle(*r.page)==good,"native records retain values independently of encoded image");
  for(unsigned variant=0;variant<29;++variant){auto v=p;auto& pin=v.records[0];
    switch(variant){
      case 0:v.object_uuid={};break;case 1:v.creator_transaction_uuid={};break;case 2:v.epoch=0;break;case 3:v.creator_local_transaction_id=0;break;
      case 4:v.flags=2;break;case 5:v.total_pins=0;break;case 6:v.first_record=v.total_pins;break;
      case 7:v.legal_hold_pins=1;break;case 8:v.lowest_start=1;break;case 9:v.highest_end=1;break;
      case 10:v.next.reset();v.next_sha256={};break;case 11:v.next_sha256={};break;case 12:v.next->page_number=0;break;
      case 13:v.next=d::NativePageReference{Id(2),19,109,v.header.page_size_profile_uuid};break;
      case 14:v.next->filespace_uuid=Id(2);v.next->page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;break;
      case 15:pin.pin_uuid={};break;case 16:pin.owner_uuid={};break;case 17:pin.timeline_uuid={};break;
      case 18:pin.filespace_uuid.bytes[6]=0;break;case 19:pin.kind=pg::NativeRetentionKind(0);break;case 20:pin.kind=pg::NativeRetentionKind(9);break;
      case 21:pin.access=pg::NativeRetentionAccess(0);break;case 22:pin.access=pg::NativeRetentionAccess(8);break;
      case 23:pin.start_local=0;break;case 24:pin.end_local=pin.start_local;break;case 25:pin.flags|=32;break;
      case 26:pin.blocked_operations=32;break;case 27:pin.kind=pg::NativeRetentionKind(5);pin.filespace_uuid={};break;
      case 28:v.records[1].pin_uuid=pin.pin_uuid;break;
    }
    auto b=Oracle(v);auto r=scratch.Decode(b);auto owned=pg::DecodeNativeRetentionPage(b);
    Check(!r.page&&!owned.page&&r.error==owned.error,"resealed semantic leaf failure exact owning parity");
  }
  for(unsigned variant=0;variant<7;++variant){auto v=Example(0,0,0);
    if(variant==0)v.first_record=1;if(variant==1)v.legal_hold_pins=1;if(variant==2)v.lowest_start=1;
    if(variant==3)v.highest_end=1;if(variant==4)v.records=p.records;
    if(variant==5){v.total_pins=1;v.lowest_start=10;}
    if(variant==6){v.next=p.next;v.next_sha256=p.next_sha256;}
    Check(!scratch.Decode(Oracle(v)).page,"invalid empty and nonempty root summaries refuse");
  }
  for(unsigned at:{128u,136u,138u,140u,220u,360u,383u,8191u}){auto b=good;b[at]^=0x80;Seal(b);Check(!scratch.Decode(b).page,"resealed framing reserved and tail failure");}
  auto bad_hash=good;bad_hash[384+112]^=1;Seal(bad_hash);auto failed_hash=scratch.Decode(bad_hash);
  Check(!failed_hash.page&&failed_hash.error==E::invalid_integrity,"per-record hash independently checked despite valid whole-page hash");
  auto reserved=good;reserved[384+144]=1;RecordSeal(reserved,384);Seal(reserved);
  Check(!scratch.Decode(reserved).page,"record reserved bytes checked after both valid hashes");
  for(unsigned phase=1;phase<=3;++phase){hashes=0;hash_at=phase;auto r=scratch.Decode(good);hash_at=0;
    Check(!r.page&&r.error==E::hash_failure,"every page and record hash context failure");
    for(unsigned method=1;method<=4;++method){hashes=0;fault_context=phase;fault=method;auto failed=scratch.Decode(good);
      Check(!fault&&!failed.page&&failed.error==E::hash_failure,"every phase of every page and record digest");}}
  for(unsigned size:{0u,127u,383u,8191u,8193u}){auto b=good;b.resize(size);Check(!scratch.Decode(b).page,"invalid image length");}
  for(unsigned field=0;field<4;++field)for(unsigned at=0;at<16;++at){auto v=Example(0,0,1);
    const std::array<Uuid*,4> ids{&v.records[0].pin_uuid,&v.records[0].owner_uuid,&v.records[0].timeline_uuid,&v.records[0].filespace_uuid};
    ids[field]->bytes[at]^=1;const auto b=Oracle(v);auto r=scratch.Decode(b);Check(r.ok()&&Oracle(*r.page)==b,"every binary record identity byte retained");}
  auto open=p;open.records[0].end_local=0;open.records[0].filespace_uuid={};open.records[0].retain_until_local=open.records[0].retain_until_unix_ns=std::numeric_limits<u64>::max();
  Check(scratch.Decode(Oracle(open)).ok(),"open-ended node-wide pin and maximum expiry remain data");
  auto terminal=p;terminal.total_pins=2;terminal.next.reset();terminal.next_sha256={};
  Check(scratch.Decode(Oracle(terminal)).ok(),"terminal leaf has no invented continuation");
  terminal.first_record=9;terminal.total_pins=11;Check(scratch.Decode(Oracle(terminal)).ok(),"nonzero leaf ordinal");
  auto max=Example(0,0,1);max.total_pins=std::numeric_limits<u64>::max();max.first_record=max.total_pins-2;max.epoch=max.creator_local_transaction_id=max.total_pins;
  max.records[0].start_local=max.total_pins-1;max.records[0].end_local=max.total_pins;
  Check(scratch.Decode(Oracle(max)).ok(),"maximum counters and exclusive range endpoint");
}
}
int main(){try{Codecs();MemoryTests();std::cout<<"PASS governed retention checks="<<checks<<" not_SQL_E2E=true\n";return 0;}
  catch(...){budget=-1;std::cerr<<"FAIL checks="<<checks<<'\n';return 1;}}
