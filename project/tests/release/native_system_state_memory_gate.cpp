// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_system_state_memory.hpp"
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
using Bytes=std::vector<byte>;using E=db::NativeSystemStateError;
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
Bytes Oracle(const db::NativeSystemState& s){const auto& h=s.header;Bytes b(h.page_size_bytes,0);
  std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,h.page_type);Num(b,20,2,1);Num(b,22,2,1);
  Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Num(b,88,8,h.flags);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);
  u64 fnv=14695981039346656037ull;for(unsigned i=0;i<128;++i){fnv^=b[i];fnv*=1099511628211ull;}Num(b,96,8,fnv);
  std::copy_n("SBSYS001",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,384);Num(b,140,4,512);
  Put(b,144,s.object_uuid);Num(b,160,8,s.state_generation);Num(b,168,8,s.restart_generation);Num(b,176,8,s.startup_counter);
  Put(b,184,s.creator_transaction_uuid);Num(b,200,8,s.creator_local_transaction_id);
  Num(b,208,2,unsigned(s.lifecycle));Num(b,210,2,unsigned(s.recovery));Num(b,212,4,s.flags);
  Num(b,216,8,s.checkpoint_generation);if(s.checkpoint)Ref(b,224,*s.checkpoint);Put(b,272,s.checkpoint_object_uuid);
  Put(b,288,s.clean_transaction_uuid);Num(b,304,8,s.clean_local_transaction_id);Put(b,312,s.transition_operation_uuid);
  if(s.predecessor)Ref(b,328,*s.predecessor);std::copy(s.predecessor_sha256.begin(),s.predecessor_sha256.end(),b.begin()+376);
  Seal(b);return b;
}
db::NativeSystemState Example(unsigned profile=0,unsigned member=0,unsigned state=4,unsigned recovery=1,unsigned flags=6){
  const auto& own=d::kCanonicalFilespacePageProfiles[profile];const auto& target=d::kCanonicalFilespacePageProfiles[member];
  db::NativeSystemState s;s.header={own.page_size_bytes,8,Id(1),Id(2),Id(10),19,109,0,own.uuid};
  s.object_uuid=Id(20);s.state_generation=2;s.restart_generation=3;s.startup_counter=4;
  s.creator_transaction_uuid=Id(21);s.creator_local_transaction_id=11;
  s.lifecycle=db::NativeSystemLifecycle(state);s.recovery=db::NativeSystemRecovery(recovery);s.flags=flags;
  s.checkpoint_generation=7;s.checkpoint=d::NativePageReference{Id(3),5,10,target.uuid};s.checkpoint_object_uuid=Id(22);
  s.clean_transaction_uuid=Id(23);s.clean_local_transaction_id=9;s.transition_operation_uuid=Id(24);
  s.predecessor=d::NativePageReference{Id(3),6,11,target.uuid};s.predecessor_sha256.fill(0x4c);
  return s;
}
// Independent truth table from NATIVE-SYSTEM-STATE-IMAGE-001; includes invalid
// combinations, so a blanket accept/reject implementation cannot satisfy it.
bool ValidCombination(unsigned state,unsigned recovery,unsigned flags){
  const bool clean=flags&1,dirty=flags&2,fenced=flags&4;
  return clean!=dirty&&clean==(state==2)&&(fenced||(state==4&&recovery<4));
}
namespace m=scratchbird::core::memory;
using ME=db::NativeSystemStateMemoryError;
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
    r.route_label="storage.system-state.conformance";r.purpose="actual system-state image and metadata";
    r.binary_operation_uuid=binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
    r.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(64).bytes},
      {m::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
    r.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label="system-state resource conformance";
    for(const auto& scope:r.scope_chain){m::HierarchicalMemoryBudget b;b.scope=scope;b.hard_limit_bytes=bytes;
      b.provenance=r.provenance;Check(ledger.SetBudget(b).ok(),"actual parent budget");}
    auto grant=m::AcquireReservationBackedMemoryResource(r);Check(grant.ok(),"actual node-issued metadata grant");
    auto adopted=db::AdoptNativeStorageMemory(binding,grant.resource);Check(adopted.ok()&&!grant.resource,"exclusive native adoption");
    memory=std::move(adopted.memory);
  }
  void Empty(){const auto s=manager.Snapshot();Check(!s.current_bytes&&!s.reserved_capacity_bytes&&
    !s.active_capacity_reservation_count&&!ledger.Snapshot().current_bytes,"all real system-state charges released");}
};
struct MemoryFile {
  std::filesystem::path directory,path;
  d::FileDevice device;
  db::NativeSystemState value;
  Bytes bytes;
  d::NativeCommonPageHeaderBinding expected;
  explicit MemoryFile(unsigned profile):value(Example(profile)),bytes(Oracle(value)){
    char name[]="/tmp/sb-system-state-memory-XXXXXX";const auto* made=mkdtemp(name);if(!made)throw std::runtime_error("mkdtemp");
    directory=made;path=directory/"native.bin";
    Check(device.Open(path.string(),d::FileOpenMode::create_new).ok(),"actual system-state source");
    expected={{value.header.database_uuid,value.header.filespace_uuid,value.header.page_size_profile_uuid},
      value.header.page_number,value.header.page_generation,8,value.header.page_uuid};
    Bootstrap();Store(bytes);Check(device.Sync().ok()&&device.Close().ok()&&
      device.Open(path.string(),d::FileOpenMode::open_existing).ok(),"cold reopen independent system-state image");
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
  void Store(const Bytes& image){Check(device.WriteAt(value.header.page_number*u64(value.header.page_size_bytes),image.data(),image.size()).ok()&&device.Sync().ok(),"actual independent system-state");}
  auto Read(MemoryFixture& f){reads=hashes=0;return db::ReadNativeSystemStateWithMemoryFromOpenDevice(device,expected,value.object_uuid,f.memory,f.binding);}
};
void NoPage(const db::NativeSystemStateMemoryResult& r){Check(!r.ok()&&!r.state&&!r.image,"refusal exposes no state or image owner prefix");}
void MemoryTests(){
  for(unsigned profile=0;profile<5;++profile){MemoryFile file(profile);
    const auto capacity=file.bytes.size();
    {
      MemoryFixture f(capacity);
      {auto guard=file.device.AcquireOperationGuard();allocation_device_mutex=guard.mutex();}
      allocation_lock_free=false;auto read=file.Read(f);
      Check(read.ok()&&allocation_lock_free,"real metadata backing allocated outside device guard");
      Check(last_read_buffer==read.image.data()&&read.image.size()==file.bytes.size()&&
        std::equal(read.image.data(),read.image.data()+read.image.size(),file.bytes.begin(),file.bytes.end()),"actual read destination is returned charged image");
      Check(Oracle(*read.state)==file.bytes,"exact fixed native values with no hidden variable storage");
      Check(f.manager.Snapshot().current_bytes==capacity&&f.memory.Snapshot().allocated_bytes==capacity&&
        f.ledger.Snapshot().current_bytes==capacity,"actual physical parent and buffer charges agree");
      auto full=file.Read(f);NoPage(full);Check(full.error==ME::memory_allocation_failure&&reads==0,"simultaneous live image prevents uncharged second reader");
      const auto revoked=f.ledger.CleanupOwner(f.binding.owner_uuid.bytes);Check(revoked.retained_bytes==capacity,"revocation retains actual image and metadata");
      auto denied=file.Read(f);NoPage(denied);Check(denied.error==ME::memory_binding_failure&&reads==0,"revoked grant prevents new source reads");
      f.memory={};Check(f.manager.Snapshot().current_bytes==capacity,"returned metadata owner survives caller workspace");
      std::thread worker([retained=std::move(read)]()mutable{budget=0;retained={};if(budget!=0)std::abort();budget=-1;});worker.join();f.Empty();
    }
    MemoryFixture valid(capacity);const auto no_writes=writes,no_syncs=syncs;
    {auto guard=file.device.AcquireOperationGuard();observation_mutex=guard.mutex();}
    fenced_reads=0;observation_unlocked=false;
    {auto r=file.Read(valid);Check(r.ok()&&fenced_reads==2&&!observation_unlocked,"same actual fence held over bootstrap and system-state reads");}
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
      reads=0;auto denied=db::ReadNativeSystemStateWithMemoryFromOpenDevice(file.device,file.expected,file.value.object_uuid,valid.memory,wrong);
      NoPage(denied);Check(denied.error==ME::memory_binding_failure&&!reads&&!valid.manager.Snapshot().current_bytes,"exact binary memory identity before allocation or I/O");}
    MemoryFixture small(capacity-1);auto short_grant=file.Read(small);NoPage(short_grant);
    Check(short_grant.error==ME::memory_allocation_failure&&!reads&&!small.manager.Snapshot().current_bytes,"one byte short cannot obtain image or metadata");small.memory={};small.Empty();
    file.expected.page_generation++;{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::header_failure,"stale generation binds actual source");}file.expected.page_generation--;
    file.expected.page_uuid=Id(99);{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::header_failure,"known binary page identity is exact");}file.expected=original_binding;
    file.expected.filespace.filespace_uuid=Id(99);{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::bootstrap_failure,"foreign filespace refuses at actual bootstrap");}file.expected=original_binding;
    file.expected.filespace.page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[(profile+1)%5].uuid;
    {MemoryFixture profile_grant(131072);auto r=file.Read(profile_grant);NoPage(r);
      Check(r.error==ME::bootstrap_failure,"foreign canonical profile with sufficient backing refuses at bootstrap");
      profile_grant.memory={};profile_grant.Empty();}file.expected=original_binding;
    file.expected.filespace.database_uuid=Id(99);{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::memory_binding_failure&&!reads,"foreign database disagrees with actual grant");}file.expected=original_binding;
    auto object=db::ReadNativeSystemStateWithMemoryFromOpenDevice(file.device,file.expected,Id(99),valid.memory,valid.binding);NoPage(object);Check(object.error==ME::object_mismatch,"exact system-state object identity");
    for(unsigned n=1;n<=2;++n){fail_read=n;auto r=file.Read(valid);fail_read=0;NoPage(r);
      Check(reads==n&&!valid.manager.Snapshot().current_bytes,"every real read failure releases admitted payload");}
    short_read=2;{auto r=file.Read(valid);Check(r.ok()&&reads==3,"legal short physical read completes");}short_read=0;
    short_read=2;eof_read=3;{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::io_failure&&r.page_bytes_read==file.bytes.size()-1,"partial then EOF retains actual read progress only");}short_read=eof_read=0;
    bootstrap_fault=true;{auto r=file.Read(valid);NoPage(r);Check(!bootstrap_fault&&r.error==ME::bootstrap_failure,"real bootstrap digest failure reached");}
    for(unsigned phase=1;phase<=1;++phase)for(unsigned method=1;method<=4;++method){fault_context=phase;fault=method;
      auto r=file.Read(valid);NoPage(r);Check(fault==0&&r.state_error==E::hash_failure,"each method in each actual system-state digest reached");}
    for(unsigned phase=1;phase<=1;++phase){hash_at=phase;auto r=file.Read(valid);hash_at=0;NoPage(r);
      Check(r.state_error==E::hash_failure&&hashes==phase,"each actual system-state digest context failure typed");}
    Check(writes==no_writes&&syncs==no_syncs,"reader and refusal paths never write or sync source");
    auto corrupt=file.bytes;corrupt.back()^=1;file.Store(corrupt);
    {auto guard=file.device.AcquireOperationGuard();deallocation_device_mutex=guard.mutex();}deallocation_lock_free=false;
    {auto r=file.Read(valid);NoPage(r);Check(r.error==ME::state_failure&&deallocation_lock_free,"failed image cleanup occurs after releasing device guard");}file.Store(file.bytes);
    file.Bootstrap(0,Id(99));{auto r=file.Read(valid);NoPage(r);Check(r.error==ME::bootstrap_failure,"actual bootstrap identity mismatch");}file.Bootstrap(1);
    {auto r=file.Read(valid);NoPage(r);Check(r.error==ME::encrypted_requires_authority&&reads==1,"encrypted filespace refuses before system-state payload I/O");}file.Bootstrap();
    {auto encrypted=file.value;encrypted.header.flags=1;file.Store(Oracle(encrypted));auto r=file.Read(valid);NoPage(r);
      Check(r.error==ME::encrypted_requires_authority,"encrypted common header never parsed as plaintext");}file.Store(file.bytes);
    for(unsigned member=0;member<5;++member)for(unsigned state=1;state<=10;++state)
      for(unsigned recovery=1;recovery<=7;++recovery)for(unsigned flags=0;flags<16;++flags){
        auto variant=Example(profile,member,state,recovery,flags);const auto encoded=Oracle(variant);file.Store(encoded);
        auto r=file.Read(valid);const bool expected=ValidCombination(state,recovery,flags);
        Check(r.ok()==expected,"actual file lifecycle recovery fence cluster truth table");
        if(expected)Check(Oracle(*r.state)==encoded,"all native fixed values survive real file decoding");
        else {NoPage(r);Check(r.state_error==E::invalid_state,"invalid flag combination retains typed state refusal");}
      }
    file.Store(file.bytes);
    if(!profile){unsigned long sites=0;
      {allocations=0;measuring=true;auto r=file.Read(valid);measuring=false;sites=allocations;Check(r.ok(),"measure full admitted reader allocation sites");}
      for(unsigned long n=0;n<sites;++n){budget=n;auto r=file.Read(valid);budget=-1;
        if(r.ok())Check(Oracle(*r.state)==file.bytes,"optional telemetry loss cannot change decoded result");
        else NoPage(r);
        r={};Check(!valid.manager.Snapshot().current_bytes&&!valid.memory.Snapshot().allocated_bytes,"every allocation fault retains zero physical payload after cleanup");
        {auto retry=file.Read(valid);Check(retry.ok(),"same-owner retry after each allocation failure");}
      }
      Check(sites>0,"allocation sweep executed");std::cout<<"governed system-state metadata faults="<<sites<<'\n';
    }
    Check(file.device.Close().ok()&&file.device.Open(file.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"actual read-only reopen");
    {const auto before_writes=writes,before_syncs=syncs;auto r=file.Read(valid);
      Check(r.ok()&&file.device.read_only()&&Oracle(*r.state)==file.bytes&&writes==before_writes&&syncs==before_syncs,"read-only ownership and bytes preserved");}
    Check(file.device.Close().ok(),"close source");auto closed=file.Read(valid);NoPage(closed);
    Check(closed.error==ME::bootstrap_failure&&closed.bootstrap_error==d::FilespaceBootstrapError::device_not_open,"reader never reopens closed source");
    valid.memory={};valid.Empty();
  }
}

void Codecs(){
  for(unsigned profile=0;profile<5;++profile)for(unsigned member=0;member<5;++member)
    for(unsigned state=1;state<=10;++state)for(unsigned recovery=1;recovery<=7;++recovery)for(unsigned flags=0;flags<16;++flags){
      auto v=Example(profile,member,state,recovery,flags);auto b=Oracle(v);
      allocations=0;measuring=true;budget=0;auto r=db::DecodeNativeSystemStateValue(b);const auto remaining=budget;budget=-1;measuring=false;
      const bool expected=ValidCombination(state,recovery,flags);
      Check(r.ok()==expected&&!allocations&&remaining==0,"independent lifecycle recovery and flags truth table without C++ heap allocation");
      auto owned=db::DecodeNativeSystemState(b);
      Check(owned.error==r.error&&owned.ok()==expected,"owning route preserves exact value validation");
      if(expected){Check(Oracle(*r.state)==b&&owned.bytes==b,"complete fixed native values and owning independent image");
        std::fill(b.begin(),b.end(),0);Check(Oracle(*r.state)==Oracle(v),"value does not borrow input");}
      else Check(!r.state&&!owned.state&&owned.bytes.empty(),"invalid state exposes no prefix");
    }
  auto p=Example();const auto good=Oracle(p);
  for(unsigned field=0;field<9;++field)for(unsigned at=0;at<16;++at){auto v=p;
    const std::array<Uuid*,9> ids{&v.header.database_uuid,&v.header.filespace_uuid,&v.header.page_uuid,
      &v.object_uuid,&v.creator_transaction_uuid,&v.checkpoint_object_uuid,&v.clean_transaction_uuid,
      &v.transition_operation_uuid,&v.checkpoint->filespace_uuid};
    ids[field]->bytes[at]^=1;const auto b=Oracle(v);auto r=db::DecodeNativeSystemStateValue(b);
    Check(r.ok()&&Oracle(*r.state)==b,"every binary identity byte preserved without text or truncation");}
  for(usize i=0;i<good.size();++i){auto bad=good;bad[i]^=1;Check(!db::DecodeNativeSystemStateValue(bad).state,"every-byte integrity refusal");}
  for(unsigned variant=0;variant<31;++variant){auto bad=p;
    switch(variant){
      case 0:bad.object_uuid={};break;case 1:bad.creator_transaction_uuid={};break;case 2:bad.transition_operation_uuid={};break;
      case 3:bad.state_generation=0;break;case 4:bad.restart_generation=0;break;case 5:bad.startup_counter=2;break;
      case 6:bad.creator_local_transaction_id=0;break;case 7:bad.lifecycle=db::NativeSystemLifecycle(0);break;
      case 8:bad.lifecycle=db::NativeSystemLifecycle(11);break;case 9:bad.recovery=db::NativeSystemRecovery(0);break;
      case 10:bad.recovery=db::NativeSystemRecovery(8);break;case 11:bad.flags|=16;break;
      case 12:bad.clean_transaction_uuid={};break;case 13:bad.clean_local_transaction_id=0;break;
      case 14:bad.checkpoint.reset();break;case 15:bad.checkpoint_object_uuid={};break;case 16:bad.checkpoint_generation=0;break;
      case 17:bad.lifecycle=db::NativeSystemLifecycle(2);bad.flags=5;bad.clean_transaction_uuid={};bad.clean_local_transaction_id=0;break;
      case 18:bad.lifecycle=db::NativeSystemLifecycle(2);bad.flags=5;bad.checkpoint_generation=0;bad.checkpoint.reset();bad.checkpoint_object_uuid={};break;
      case 19:bad.state_generation=1;break;case 20:bad.predecessor.reset();bad.predecessor_sha256={};break;
      case 21:bad.predecessor_sha256={};break;case 22:bad.checkpoint->page_number=0;break;
      case 23:bad.predecessor->page_generation=0;break;case 24:bad.checkpoint->filespace_uuid={};break;
      case 25:bad.predecessor->page_size_profile_uuid=Id(99);break;
      case 26:bad.checkpoint->filespace_uuid=Id(2);bad.checkpoint->page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;break;
      case 27:bad.checkpoint=d::NativePageReference{Id(2),19,109,bad.header.page_size_profile_uuid};break;
      case 28:bad.predecessor=bad.checkpoint;break;case 29:bad.predecessor->page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;break;
      case 30:bad.checkpoint->page_number=std::numeric_limits<u64>::max();break;
    }
    auto b=Oracle(bad);auto r=db::DecodeNativeSystemStateValue(b);auto owned=db::DecodeNativeSystemState(b);
    Check(!r.state&&!owned.state&&owned.bytes.empty()&&r.error==owned.error,"resealed semantic violation rejected by shared validation");
  }
  for(unsigned at:{128u,136u,138u,140u,440u,511u,8191u}){auto b=good;b[at]^=0x80;Seal(b);Check(!db::DecodeNativeSystemStateValue(b).state,"resealed family and padding refuse");}
  for(unsigned mask=0;mask<8;++mask){auto v=p;
    if(!(mask&1)){v.state_generation=1;v.predecessor.reset();v.predecessor_sha256={};}
    if(!(mask&2)){v.checkpoint_generation=0;v.checkpoint.reset();v.checkpoint_object_uuid={};}
    if(!(mask&4)){v.clean_transaction_uuid={};v.clean_local_transaction_id=0;}
    Check(db::DecodeNativeSystemStateValue(Oracle(v)).ok(),"all optional predecessor checkpoint clean-observation combinations");
  }
  auto maximum=p;maximum.state_generation=maximum.restart_generation=maximum.startup_counter=maximum.creator_local_transaction_id=
    maximum.checkpoint_generation=maximum.clean_local_transaction_id=std::numeric_limits<u64>::max();
  Check(db::DecodeNativeSystemStateValue(Oracle(maximum)).ok(),"uint64 fields preserve maximum boundary");
  Bytes unaligned(1);unaligned.insert(unaligned.end(),good.begin(),good.end());
  Check(db::DecodeNativeSystemStateValue(std::span<const byte>(unaligned).subspan(1)).ok(),"unaligned immutable image");
  for(unsigned size:{0u,127u,511u,8191u,8193u}){auto b=good;b.resize(size);Check(!db::DecodeNativeSystemStateValue(b).state,"invalid image length");}
  hashes=0;hash_at=1;auto context=DenyCodecAllocation([&]{return db::DecodeNativeSystemStateValue(good);});hash_at=0;Check(!context.state&&context.error==E::hash_failure,"context failure");
  for(unsigned n=1;n<=4;++n){hashes=0;fault_context=1;fault=n;auto r=DenyCodecAllocation([&]{return db::DecodeNativeSystemStateValue(good);});
    Check(!fault&&!r.state&&r.error==E::hash_failure,"each digest phase failure");}
}
}
int main(){try{Codecs();MemoryTests();std::cout<<"PASS governed system-state checks="<<checks<<" not_SQL_E2E=true\n";return 0;}
  catch(...){budget=-1;std::cerr<<"FAIL checks="<<checks<<'\n';return 1;}}
