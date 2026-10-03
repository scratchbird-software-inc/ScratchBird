// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_inventory_memory.hpp"
#include "transaction_inventory_validation.hpp"
#include "transaction_horizon_projection.hpp"
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
namespace pg=scratchbird::storage::page;namespace mga=scratchbird::transaction::mga;
using Entry=mga::TransactionInventoryEntry;
using Bytes=std::vector<byte>;using E=pg::NativeInventoryError;
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

void Seal(Bytes& b){std::fill(b.begin()+320,b.begin()+352,0);const auto h=Hash(b);std::copy(h.begin(),h.end(),b.begin()+320);}
// Independent declarative horizon oracle: classify by wire state numbers,
// archive origin and begin commit boundaries, not production projection helpers.
template<class Inventory> std::array<u64,3> Horizons(const Inventory& inv){
  u64 interesting=inv.next_local_transaction_id,active=interesting,oldest=interesting,boundary=inv.next_commit_sequence;
  for(const auto& e:inv.entries){const unsigned state=unsigned(e.state),effective=state==12?unsigned(e.archived_from_state):state;
    if(effective!=6&&effective!=8)interesting=std::min(interesting,e.identity.local_id.value);
    if(state==2||state==13){active=std::min(active,e.identity.local_id.value);if(e.stable_snapshot)boundary=std::min(boundary,e.begin_visible_through_commit_sequence);}}
  oldest=active;if(boundary!=inv.next_commit_sequence){oldest=std::min(oldest,interesting);
    for(const auto& e:inv.entries)if((unsigned(e.state)==6||(unsigned(e.state)==12&&unsigned(e.archived_from_state)==6))&&e.commit_sequence>boundary)
      oldest=std::min(oldest,e.identity.local_id.value);}
  return {interesting,active,oldest};
}
template<class Page> Bytes Oracle(const Page& r){const auto& h=r.header;Bytes b(h.page_size_bytes,0);
  std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,h.page_type);Num(b,20,2,1);Num(b,22,2,1);
  Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Num(b,88,8,h.flags);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);
  u64 fnv=14695981039346656037ull;for(unsigned i=0;i<128;++i){fnv^=b[i];fnv*=1099511628211ull;}Num(b,96,8,fnv);
  std::copy_n("SBTINV01",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,256);Num(b,140,4,384+72*r.inventory.entries.size());
  Put(b,144,r.object_uuid);Num(b,160,8,r.inventory_generation);Num(b,168,8,r.inventory.next_local_transaction_id);Num(b,176,8,r.inventory.next_commit_sequence);
  Num(b,184,4,r.inventory.entries.size());if(r.previous)Ref(b,192,*r.previous);if(r.next)Ref(b,240,*r.next);
  const auto horizons=Horizons(r.inventory);for(unsigned i=0;i<3;++i)Num(b,288+8*i,8,horizons[i]);
  for(usize i=0;i<r.inventory.entries.size();++i){const auto& e=r.inventory.entries[i];const usize n=384+72*i;
    unsigned origin=unsigned(e.archived_from_state)==6?1:unsigned(e.archived_from_state)==8?2:unsigned(e.archived_from_state)==11?3:0;
    const u32 flags=(e.evidence_record_required?1:0)|(e.evidence_record_written?2:0)|(e.rollback_only?4:0)|(origin<<3)|(e.stable_snapshot?32:0);
    Num(b,n,8,e.identity.local_id.value);Put(b,n+8,e.identity.transaction_uuid.value);Num(b,n+24,2,unsigned(e.identity.scope));Num(b,n+26,2,unsigned(e.state));Num(b,n+28,4,flags);
    Num(b,n+32,8,e.begin_unix_epoch_millis);Num(b,n+40,8,e.final_unix_epoch_millis);Num(b,n+48,8,e.begin_visible_through_local_transaction_id);
    Num(b,n+56,8,e.begin_visible_through_commit_sequence);Num(b,n+64,8,e.commit_sequence);
  }Seal(b);return b;
}
pg::NativeTransactionInventoryPage Example(unsigned profile=0,unsigned member=0,unsigned count=8,unsigned state=0,unsigned flags=9){
  const auto& own=d::kCanonicalFilespacePageProfiles[profile];const auto& target=d::kCanonicalFilespacePageProfiles[member];
  pg::NativeTransactionInventoryPage p;p.header={own.page_size_bytes,0x301,Id(1),Id(2),Id(10),19,109,0,own.uuid};
  p.object_uuid=Id(20);p.inventory_generation=7;p.inventory.next_local_transaction_id=count+1;p.inventory.next_commit_sequence=count+10;
  p.previous=d::NativePageReference{Id(3),5,10,target.uuid};p.next=d::NativePageReference{Id(3),6,11,target.uuid};
  for(unsigned i=0;i<count;++i){Entry e;e.identity={{i+1},{UuidKind::transaction,Id(100+i)},mga::TransactionScope(i%2)};
    e.state=mga::TransactionState(state?state:1+i%13);const unsigned origin=6+(i%3==0?0:i%3==1?2:5);
    if(unsigned(e.state)==12)e.archived_from_state=mga::TransactionState(origin);
    e.begin_unix_epoch_millis=100+i;e.final_unix_epoch_millis=200+i;e.begin_visible_through_local_transaction_id=i;
    e.begin_visible_through_commit_sequence=0;e.commit_sequence=(unsigned(e.state)==6||(unsigned(e.state)==12&&origin==6))?i+1:0;
    e.evidence_record_required=flags&1;e.evidence_record_written=flags&2;e.rollback_only=flags&4;e.stable_snapshot=flags&8;p.inventory.entries.push_back(e);}
  return p;
}
struct Scratch {
  std::vector<Entry> entries;std::vector<usize> indices;Bytes markers;
  explicit Scratch(usize n):entries(n),indices(n),markers(n){}
  auto Decode(std::span<const byte> b){return pg::DecodeNativeTransactionInventoryPageInto(b,entries,indices,markers);}
};
namespace m=scratchbird::core::memory;
using ME=db::NativeInventoryMemoryError;
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
    r.route_label="storage.inventory.conformance";r.purpose="actual inventory image and metadata";
    r.binary_operation_uuid=binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
    r.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(64).bytes},
      {m::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
    r.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label="inventory resource conformance";
    for(const auto& scope:r.scope_chain){m::HierarchicalMemoryBudget b;b.scope=scope;b.hard_limit_bytes=bytes;
      b.provenance=r.provenance;Check(ledger.SetBudget(b).ok(),"actual parent budget");}
    auto grant=m::AcquireReservationBackedMemoryResource(r);Check(grant.ok(),"actual node-issued metadata grant");
    auto adopted=db::AdoptNativeStorageMemory(binding,grant.resource);Check(adopted.ok()&&!grant.resource,"exclusive native adoption");
    memory=std::move(adopted.memory);
  }
  void Empty(){const auto s=manager.Snapshot();Check(!s.current_bytes&&!s.reserved_capacity_bytes&&
    !s.active_capacity_reservation_count&&!ledger.Snapshot().current_bytes,"all real inventory charges released");}
};
struct MemoryFile {
  std::filesystem::path directory,path;
  d::FileDevice device;
  pg::NativeTransactionInventoryPage value;
  Bytes bytes;
  d::NativeCommonPageHeaderBinding expected;
  explicit MemoryFile(unsigned profile):value(Example(profile)),bytes(Oracle(value)){
    char name[]="/tmp/sb-inventory-memory-XXXXXX";const auto* made=mkdtemp(name);if(!made)throw std::runtime_error("mkdtemp");
    directory=made;path=directory/"native.bin";
    Check(device.Open(path.string(),d::FileOpenMode::create_new).ok(),"actual inventory source");
    expected={{value.header.database_uuid,value.header.filespace_uuid,value.header.page_size_profile_uuid},
      value.header.page_number,value.header.page_generation,0x301,value.header.page_uuid};
    Bootstrap();Store(bytes);Check(device.Sync().ok()&&device.Close().ok()&&
      device.Open(path.string(),d::FileOpenMode::open_existing).ok(),"cold reopen independent inventory image");
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
  void Store(const Bytes& image){Check(device.WriteAt(value.header.page_number*u64(value.header.page_size_bytes),image.data(),image.size()).ok()&&device.Sync().ok(),"actual independent inventory");}
  auto Read(MemoryFixture& f){reads=hashes=0;return db::ReadNativeInventoryWithMemoryFromOpenDevice(device,expected,value.object_uuid,f.memory,f.binding);}
};
void NoPage(const db::NativeInventoryMemoryResult& r){Check(!r.ok()&&!r.page&&r.image.empty()&&!r.arena,"refusal exposes no image metadata or owner prefix");}
void MemoryTests(){
  for(unsigned profile=0;profile<5;++profile){MemoryFile file(profile);
    const auto capacity=db::NativeInventoryWorkspaceBytes(file.value.header.page_size_profile_uuid);
    Check(capacity==file.bytes.size()+((file.bytes.size()-384)/72)*(sizeof(Entry)+sizeof(usize)+1)+3*(alignof(std::max_align_t)-1),"independent native backing size formula");
    {
      MemoryFixture f(capacity);
      {auto guard=file.device.AcquireOperationGuard();allocation_device_mutex=guard.mutex();}
      allocation_lock_free=false;auto read=file.Read(f);
      Check(read.ok()&&allocation_lock_free,"real metadata backing allocated outside device guard");
      Check(last_read_buffer==read.image.data()&&read.image.size()==file.bytes.size()&&
        std::equal(read.image.begin(),read.image.end(),file.bytes.begin(),file.bytes.end()),"actual read destination is returned charged image");
      const auto begin=reinterpret_cast<std::uintptr_t>(read.image.data());
      const auto inside=[&](const void* ptr,usize bytes){auto n=reinterpret_cast<std::uintptr_t>(ptr);return n>=begin&&n-begin<=capacity&&bytes<=capacity-(n-begin);};
      Check(inside(read.page->inventory.entries.data(),read.page->inventory.entries.size_bytes())&&
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
    {auto r=file.Read(valid);Check(r.ok()&&fenced_reads==2&&!observation_unlocked,"same actual fence held over bootstrap and inventory reads");}
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
      reads=0;auto denied=db::ReadNativeInventoryWithMemoryFromOpenDevice(file.device,file.expected,file.value.object_uuid,valid.memory,wrong);
      NoPage(denied);Check(denied.error==ME::memory_binding_failure&&!reads&&!valid.manager.Snapshot().current_bytes,"exact binary memory identity before allocation or I/O");}
    MemoryFixture small(capacity-1);auto short_grant=file.Read(small);NoPage(short_grant);
    Check(short_grant.error==ME::memory_allocation_failure&&!reads&&!small.manager.Snapshot().current_bytes,"one byte short cannot obtain image or metadata");small.memory={};small.Empty();
    file.expected.page_generation++;{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::header_failure,"stale generation binds actual source");}file.expected.page_generation--;
    auto object=db::ReadNativeInventoryWithMemoryFromOpenDevice(file.device,file.expected,Id(99),valid.memory,valid.binding);NoPage(object);Check(object.error==ME::object_mismatch,"exact inventory object identity");
    for(unsigned n=1;n<=2;++n){fail_read=n;auto r=file.Read(valid);fail_read=0;NoPage(r);
      Check(reads==n&&!valid.manager.Snapshot().current_bytes,"every real read failure releases admitted payload");}
    short_read=2;{auto r=file.Read(valid);Check(r.ok()&&reads==3,"legal short physical read completes");}short_read=0;
    short_read=2;eof_read=3;{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::io_failure&&r.page_bytes_read==file.bytes.size()-1,"partial then EOF retains actual read progress only");}short_read=eof_read=0;
    bootstrap_fault=true;{auto r=file.Read(valid);NoPage(r);Check(!bootstrap_fault&&r.error==ME::bootstrap_failure,"real bootstrap digest failure reached");}
    for(unsigned phase=1;phase<=1;++phase)for(unsigned method=1;method<=4;++method){fault_context=phase;fault=method;
      auto r=file.Read(valid);NoPage(r);Check(fault==0&&r.inventory_error==E::hash_failure,"each method in each actual inventory digest reached");}
    for(unsigned phase=1;phase<=1;++phase){hash_at=phase;auto r=file.Read(valid);hash_at=0;NoPage(r);
      Check(r.inventory_error==E::hash_failure&&hashes==phase,"each actual inventory digest context failure typed");}
    Check(writes==no_writes&&syncs==no_syncs,"reader and refusal paths never write or sync source");
    auto corrupt=file.bytes;corrupt.back()^=1;file.Store(corrupt);
    {auto guard=file.device.AcquireOperationGuard();deallocation_device_mutex=guard.mutex();}deallocation_lock_free=false;
    {auto r=file.Read(valid);NoPage(r);Check(r.error==ME::inventory_failure&&deallocation_lock_free,"failed image cleanup occurs after releasing device guard");}file.Store(file.bytes);
    file.Bootstrap(0,Id(99));{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::bootstrap_failure,"actual bootstrap identity mismatch");}file.Bootstrap(1);
    {auto r=file.Read(valid);NoPage(r);Check(r.error==ME::encrypted_requires_authority&&reads==1,"encrypted filespace refuses before inventory payload I/O");}file.Bootstrap();
    {auto encrypted=file.value;encrypted.header.flags=1;file.Store(Oracle(encrypted));auto r=file.Read(valid);NoPage(r);
      Check(r.error==ME::encrypted_requires_authority,"encrypted common header never parsed as plaintext");}file.Store(file.bytes);
    for(unsigned member=0;member<5;++member)for(unsigned state=1;state<=13;++state)for(unsigned flags=0;flags<16;++flags){
      auto variant=Example(profile,member,8,state,flags);
      const auto encoded=Oracle(variant);file.Store(encoded);auto r=file.Read(valid);
      Check(r.ok()&&Oracle(*r.page)==encoded,"actual managed entries retain every state flag combination and mixed reference profile");
    }
    for(unsigned count:{0u,unsigned((file.bytes.size()-384)/72)}){auto variant=Example(profile,profile,count);auto encoded=Oracle(variant);
      file.Store(encoded);auto r=file.Read(valid);Check(r.ok()&&r.page->inventory.entries.size()==count&&Oracle(*r.page)==encoded,"empty and maximum actual native entry payload");}
    file.Store(file.bytes);
    if(!profile){unsigned long sites=0;
      {allocations=0;measuring=true;auto r=file.Read(valid);measuring=false;sites=allocations;Check(r.ok(),"measure full admitted reader allocation sites");}
      for(unsigned long n=0;n<sites;++n){budget=n;auto r=file.Read(valid);budget=-1;
        if(r.ok())Check(Oracle(*r.page)==file.bytes,"optional telemetry loss cannot change decoded result");
        else NoPage(r);
        r={};Check(!valid.manager.Snapshot().current_bytes&&!valid.memory.Snapshot().allocated_bytes,"every allocation fault retains zero physical payload after cleanup");
        {auto retry=file.Read(valid);Check(retry.ok(),"same-owner retry after each allocation failure");}
      }
      Check(sites>0,"allocation sweep executed");std::cout<<"governed inventory metadata faults="<<sites<<'\n';
    }
    Check(file.device.Close().ok(),"close source");auto closed=file.Read(valid);NoPage(closed);
    Check(closed.error==ME::bootstrap_failure&&closed.bootstrap_error==d::FilespaceBootstrapError::device_not_open,"reader never reopens closed source");
    valid.memory={};valid.Empty();
  }
}

void CompleteInventoryValidation(){
  const auto compare=[&](mga::LocalTransactionInventory& inventory){
    const std::string_view expected=mga::ValidateLocalTransactionInventoryStructure(inventory);
    const pg::NativeTransactionInventoryView view{inventory.next_local_transaction_id,inventory.next_commit_sequence,inventory.entries};
    std::vector<usize> indices(inventory.entries.size());Bytes markers(inventory.entries.size());
    const auto result=DenyCodecAllocation([&]{return pg::ValidateNativeTransactionInventoryView(view,indices,markers);});
    Check(result.ok()==expected.empty()&&std::string_view(result.detail)==expected,"complete structural first-error parity");
    return result.error;
  };
  for(unsigned count:{0u,1u,8u,2048u})for(unsigned state=1;state<=13;++state)for(unsigned flags=0;flags<16;++flags){
    auto value=Example(0,0,count,state,flags).inventory;
    Check(compare(value)==E::none,"complete multi-page-sized inventory state scope and flag combinations");
    std::reverse(value.entries.begin(),value.entries.end());
    Check(compare(value)==E::none,"structure validation does not invent page-chain ordering authority");
  }
  for(unsigned first=0;first<8;++first)for(unsigned second=0;second<8;++second)if(first!=second){
    for(unsigned field=0;field<3;++field){auto value=Example(0,0,8,6).inventory;
      if(field==0)value.entries[second].identity.local_id=value.entries[first].identity.local_id;
      if(field==1)value.entries[second].identity.transaction_uuid=value.entries[first].identity.transaction_uuid;
      if(field==2)value.entries[second].commit_sequence=value.entries[first].commit_sequence;
      Check(compare(value)==E::invalid_inventory,"duplicates across original record positions");
    }
  }
  for(unsigned fault=0;fault<13;++fault){auto value=Example(0,0,8,6).inventory;auto& e=value.entries[0];
    switch(fault){
      case 0:value.next_local_transaction_id=0;break;
      case 1:value.next_commit_sequence=0;break;
      case 2:e.begin_visible_through_commit_sequence=value.next_commit_sequence;break;
      case 3:e.state=mga::TransactionState::archived;e.archived_from_state=mga::TransactionState::none;break;
      case 4:e.archived_from_state=mga::TransactionState::committed;break;
      case 5:e.commit_sequence=0;break;
      case 6:e.state=mga::TransactionState::active;break;
      case 7:e.identity.transaction_uuid.value={};break;
      case 8:e.identity.scope=mga::TransactionScope(99);break;
      case 9:e.identity.local_id.value=value.next_local_transaction_id;break;
      case 10:e.state=mga::TransactionState::none;e.commit_sequence=0;break;
      case 11:e.commit_sequence=value.next_commit_sequence;break;
      case 12:e.begin_visible_through_commit_sequence=e.commit_sequence;break;
    }
    value.entries[2].identity.transaction_uuid=value.entries[1].identity.transaction_uuid;
    Check(compare(value)==E::invalid_inventory,"earlier structural error wins over later duplicate");
  }
  auto value=Example(0,0,8,6).inventory;
  pg::NativeTransactionInventoryView view{value.next_local_transaction_id,value.next_commit_sequence,value.entries};
  std::vector<usize> indices(8);Bytes markers(8);
  for(bool short_indices:{false,true}){
    const auto result=DenyCodecAllocation([&]{return pg::ValidateNativeTransactionInventoryView(view,
      std::span(indices).first(short_indices?7:8),std::span(markers).first(short_indices?8:7));});
    Check(result.error==E::resource_exhausted&&std::string_view(result.detail)=="insufficient_backing","exact one-short structural scratch");
  }
  const auto saved_bytes=std::as_bytes(std::span(value.entries));
  const std::vector<std::byte> saved(saved_bytes.begin(),saved_bytes.end());
  for(unsigned field=0;field<4;++field){
    auto ids=std::span(indices);auto bits=std::span(markers);
    if(field==0)ids={reinterpret_cast<usize*>(value.entries.data()),8};
    if(field==1)bits={reinterpret_cast<byte*>(value.entries.data()),8};
    if(field==2)bits={reinterpret_cast<byte*>(indices.data()),8};
    if(field==3)bits={reinterpret_cast<byte*>(&view),8};
    const auto result=DenyCodecAllocation([&]{return pg::ValidateNativeTransactionInventoryView(view,ids,bits);});
    Check(result.error==E::invalid_backing,"record scratch and descriptor alias refused before writes");
    Check(std::equal(saved.begin(),saved.end(),std::as_bytes(std::span(value.entries)).begin()),"structural validation never changes input entries");
  }
  alignas(Entry) std::array<byte,sizeof(Entry)*9> raw{};
  auto bad=view;bad.entries={reinterpret_cast<Entry*>(raw.data()+1),8};
  Check(DenyCodecAllocation([&]{return pg::ValidateNativeTransactionInventoryView(bad,indices,markers);}).error==E::invalid_backing,"misaligned entry backing refused");
  const auto image=Oracle(Example());
  const auto result=DenyCodecAllocation([&]{return pg::DecodeNativeTransactionInventoryPageInto(image,bad.entries,indices,markers);});
  Check(!result.page&&result.error==E::invalid_backing,"decoder refuses misaligned entries before any write");
  auto misaligned=std::span<usize>(reinterpret_cast<usize*>(raw.data()+1),8);
  Check(DenyCodecAllocation([&]{return pg::ValidateNativeTransactionInventoryView(view,misaligned,markers);}).error==E::invalid_backing,"misaligned index scratch refused");
}
void Codecs(){
  for(unsigned profile=0;profile<5;++profile){const usize maximum=(d::kCanonicalFilespacePageProfiles[profile].page_size_bytes-384)/72;
    Scratch scratch(maximum);
    for(unsigned member=0;member<5;++member)for(unsigned state=1;state<=13;++state)for(unsigned flags=0;flags<16;++flags){
      auto p=Example(profile,member,8,state,flags);auto b=Oracle(p);
      allocations=0;measuring=true;budget=0;auto r=scratch.Decode(b);auto remaining=budget;budget=-1;measuring=false;
      Check(r.ok()&&!allocations&&remaining==0,"all lifecycle scope archive flag and 25 link profile pairs decode without heap");
      Check(r.page->inventory.entries.data()==scratch.entries.data()&&r.page->inventory.entries.size()==8&&Oracle(*r.page)==b,"exact native entry and counter decoding");
      const auto owning=pg::DecodeNativeTransactionInventoryPage(b);Check(owning.ok()&&owning.bytes==b&&!owning.page->inventory.publication_base,"legacy owning route retains independent image and no publication base");
    }
    for(unsigned n:{0u,1u,unsigned(maximum-1),unsigned(maximum)}){
      auto p=Example(profile,profile,n);const auto bytes=Oracle(p);allocations=0;measuring=true;budget=0;auto r=scratch.Decode(bytes);auto remaining=budget;budget=-1;measuring=false;
      Check(r.ok()&&r.page->inventory.entries.size()==n&&!allocations&&remaining==0&&Oracle(*r.page)==bytes,"zero single and maximum native entries have no hidden trees or image copy");
      if(n)for(unsigned field=0;field<3;++field){auto entries=std::span(scratch.entries).first(n);auto indices=std::span(scratch.indices).first(n);auto markers=std::span(scratch.markers).first(n);
        if(field==0)entries=entries.first(n-1);if(field==1)indices=indices.first(n-1);if(field==2)markers=markers.first(n-1);
        auto short_result=pg::DecodeNativeTransactionInventoryPageInto(bytes,entries,indices,markers);
        Check(!short_result.page&&short_result.error==E::resource_exhausted,"each individual native or scratch shortfall refuses");}
    }
  }
  Scratch scratch(128);auto p=Example();auto good=Oracle(p);
  for(usize i=0;i<good.size();++i){auto bad=good;bad[i]^=1;auto r=scratch.Decode(bad);Check(!r.page,"every-byte corruption refuses without metadata");}
  for(unsigned field=0;field<3;++field){auto entries=std::span(scratch.entries);auto indices=std::span(scratch.indices);auto markers=std::span(scratch.markers);
    if(field==0)entries={reinterpret_cast<Entry*>(good.data()),8};if(field==1)indices={reinterpret_cast<usize*>(good.data()),8};if(field==2)markers={good.data(),8};
    const auto r=pg::DecodeNativeTransactionInventoryPageInto(good,entries,indices,markers);Check(!r.page&&r.error==E::invalid_backing&&good==Oracle(p),"input alias refuses before writes");}
  auto overlap=pg::DecodeNativeTransactionInventoryPageInto(good,scratch.entries,{reinterpret_cast<usize*>(scratch.entries.data()),8},scratch.markers);
  Check(!overlap.page&&overlap.error==E::invalid_backing,"output scratch alias refuses");
  auto wrapped=pg::DecodeNativeTransactionInventoryPageInto(good,{scratch.entries.data(),std::numeric_limits<usize>::max()/sizeof(Entry)+1},scratch.indices,scratch.markers);
  Check(!wrapped.page&&wrapped.error==E::invalid_backing,"native-size multiplication overflow refuses");
  Bytes unaligned(1);unaligned.insert(unaligned.end(),good.begin(),good.end());auto r=scratch.Decode(std::span<const byte>(unaligned).subspan(1));Check(r.ok(),"unaligned image decodes");
  unaligned.clear();std::fill(good.begin(),good.end(),0);std::fill(scratch.markers.begin(),scratch.markers.end(),0xff);scratch.indices.clear();
  Check(Oracle(*r.page)==Oracle(p),"native entry lifetime independent of image and uniqueness scratch");scratch.indices.resize(128);good=Oracle(p);
  for(unsigned variant=0;variant<19;++variant){auto bad=p;auto& e=bad.inventory.entries[1];
    switch(variant){
      case 0:bad.inventory.next_local_transaction_id=0;break;case 1:bad.inventory.next_commit_sequence=0;break;
      case 2:e.identity.local_id.value=bad.inventory.next_local_transaction_id;break;case 3:e.identity.local_id.value=1;break;
      case 4:e.identity.transaction_uuid.value=bad.inventory.entries[0].identity.transaction_uuid.value;break;
      case 5:e.identity.transaction_uuid.value={};break;case 6:e.identity.scope=mga::TransactionScope(7);break;
      case 7:e.state=mga::TransactionState::none;break;case 8:e.archived_from_state=mga::TransactionState::committed;break;
      case 9:e.state=mga::TransactionState::archived;break;case 10:e.commit_sequence=1;break;
      case 11:e.begin_visible_through_commit_sequence=bad.inventory.next_commit_sequence;break;
      case 12:e.state=mga::TransactionState::committed;e.commit_sequence=bad.inventory.next_commit_sequence;break;
      case 13:e.state=mga::TransactionState::committed;e.commit_sequence=6;break;
      case 14:bad.object_uuid={};break;case 15:bad.inventory_generation=0;break;
      case 16:bad.previous=d::NativePageReference{Id(2),19,109,bad.header.page_size_profile_uuid};break;
      case 17:bad.next=bad.previous;break;case 18:bad.next->page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;break;
    }
    const auto b=Oracle(bad);auto owned=pg::DecodeNativeTransactionInventoryPage(b);auto borrowed=scratch.Decode(b);
    Check(!owned.page&&!borrowed.page&&owned.error==borrowed.error,"re-sealed invalid structure and links preserve exact owning refusal");
  }
  for(unsigned at:{128u,136u,138u,140u,188u,288u,296u,304u,312u,352u,412u,8191u}){auto b=good;b[at]^=0x80;Seal(b);auto r=scratch.Decode(b);Check(!r.page,"re-sealed framing flags horizon and padding refuse");}
  auto stable=Example(0,0,2,6);stable.inventory.entries[1].state=mga::TransactionState::active;stable.inventory.entries[1].commit_sequence=0;
  stable.inventory.entries[1].stable_snapshot=true;const auto stable_image=Oracle(stable);
  auto stable_result=scratch.Decode(stable_image);Check(stable_result.ok()&&Horizons(stable_result.page->inventory)==std::array<u64,3>{2,2,1},"stable reader retains earlier local transaction committed after begin");
  for(unsigned at=0;at<16;++at){auto distinct=Example(0,0,2,2);distinct.inventory.entries[1].identity.transaction_uuid=distinct.inventory.entries[0].identity.transaction_uuid;
    distinct.inventory.entries[1].identity.transaction_uuid.value.bytes[at]^=1;
    Check(scratch.Decode(Oracle(distinct)).ok(),"full binary UUID key distinguishes every byte without text or truncation");}
  for(unsigned first=0;first<8;++first)for(unsigned second=0;second<8;++second)if(first!=second){auto duplicate=Example(0,0,8,6);
    duplicate.inventory.entries[second].identity.transaction_uuid=duplicate.inventory.entries[first].identity.transaction_uuid;
    auto r=scratch.Decode(Oracle(duplicate));Check(!r.page&&r.error==E::invalid_inventory,"duplicate binary identity detected at every original-order position pair");
    duplicate=Example(0,0,8,6);duplicate.inventory.entries[second].commit_sequence=duplicate.inventory.entries[first].commit_sequence;
    r=scratch.Decode(Oracle(duplicate));Check(!r.page&&r.error==E::invalid_inventory,"duplicate commit order detected at every position pair");}
  for(bool archived:{false,true})for(unsigned active_state:{2u,13u})for(unsigned boundary:{0u,1u}){
    auto v=stable;v.inventory.entries[0].state=archived?mga::TransactionState::archived:mga::TransactionState::committed;
    v.inventory.entries[0].archived_from_state=archived?mga::TransactionState::committed:mga::TransactionState::none;
    v.inventory.entries[1].state=mga::TransactionState(active_state);v.inventory.entries[1].begin_visible_through_commit_sequence=boundary;
    auto r=scratch.Decode(Oracle(v));Check(r.ok()&&Horizons(r.page->inventory)==std::array<u64,3>{2,2,boundary?2u:1u},"both active reader states and archived outcomes preserve exact begin-commit visibility boundary");
  }
  for(unsigned links=0;links<4;++links){auto v=p;if(!(links&1))v.previous.reset();if(!(links&2))v.next.reset();
    Check(scratch.Decode(Oracle(v)).ok(),"all predecessor successor presence combinations retain their inspection meaning");}
  auto maximum=Example(0,0,1,6);maximum.inventory.next_local_transaction_id=maximum.inventory.next_commit_sequence=std::numeric_limits<u64>::max();
  maximum.inventory.entries[0].identity.local_id.value=maximum.inventory.entries[0].commit_sequence=std::numeric_limits<u64>::max()-1;
  Check(scratch.Decode(Oracle(maximum)).ok(),"maximum counters and final number remain exact uint64");
  auto multi=Example(0,0,3,2);multi.inventory.entries[0].state=mga::TransactionState::committed;multi.inventory.entries[0].commit_sequence=2;
  multi.inventory.entries[1].stable_snapshot=multi.inventory.entries[2].stable_snapshot=true;
  multi.inventory.entries[1].begin_visible_through_commit_sequence=2;multi.inventory.entries[2].begin_visible_through_commit_sequence=1;
  auto multiple=scratch.Decode(Oracle(multi));Check(multiple.ok()&&Horizons(multiple.page->inventory)==std::array<u64,3>{2,2,1},"minimum of multiple stable begin boundaries retains earlier writer");
  for(unsigned phase=1;phase<=1;++phase){hashes=0;hash_at=phase;auto fail=DenyCodecAllocation([&]{return scratch.Decode(good);});hash_at=0;Check(!fail.page&&fail.error==E::hash_failure,"actual digest context failure");}
  for(unsigned n=1;n<=4;++n){hashes=0;fault_context=1;fault=n;auto fail=DenyCodecAllocation([&]{return scratch.Decode(good);});Check(!fault&&!fail.page&&fail.error==E::hash_failure,"each digest provider phase fails closed");}
  for(unsigned size:{0u,127u,383u,8191u,8193u}){auto b=good;b.resize(size);Check(!scratch.Decode(b).page,"invalid image lengths refuse");}
  mga::LocalTransactionHorizonRequest request;request.inventory=stable.inventory;request.active_snapshot_horizons={mga::LocalTransactionId{0}};
  auto invalid=mga::ComputeLocalTransactionHorizons(request);Check(!invalid.ok()&&!invalid.horizons.valid&&invalid.diagnostic.message_key=="transaction.horizon.invalid_snapshot_horizon","shared projection preserves invalid snapshot vector");
  request.active_snapshot_horizons={{100}};auto future=mga::ComputeLocalTransactionHorizons(request);
  Check(!future.ok()&&!future.horizons.valid&&future.diagnostic.message_key=="transaction.horizon.future_snapshot_horizon","shared projection preserves future snapshot vector");
}
}
int main(){try{CompleteInventoryValidation();Codecs();MemoryTests();std::cout<<"PASS governed inventory checks="<<checks<<" not_SQL_E2E=true\n";return 0;}
  catch(...){budget=-1;std::cerr<<"FAIL checks="<<checks<<'\n';return 1;}}
