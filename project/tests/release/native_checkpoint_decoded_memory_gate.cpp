// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "database_dirty_manifest.hpp"
#include "native_checkpoint_root_memory.hpp"
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
  ++reads;last_read_buffer=p;if(fail_read==reads){errno=EIO;return -1;}if(eof_read&&eof_read==reads)return 0;
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
using Bytes=std::vector<byte>;using E=db::NativeCheckpointError;
void Check(bool ok,const char* why,std::source_location at=std::source_location::current()){
  ++checks;if(!ok){std::cerr<<at.line()<<": "<<why<<'\n';throw why;}}
template<class F> auto DenyCodecAllocation(F&& call){
  const auto saved=budget;budget=0;
  auto result=call();const bool unchanged=budget==0;budget=saved;
  Check(unchanged,"native codec provider refusal must not allocate diagnostic text");
  return result;
}
Uuid Id(byte n){Uuid id;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=n;return id;}
void Num(Bytes& b,usize at,unsigned n,u64 v){for(unsigned i=0;i<n;++i)b[at+i]=byte(v>>(8*i));}
void Put(Bytes& b,usize at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}
void Ref(Bytes& b,usize at,const d::NativePageReference& p){Put(b,at,p.filespace_uuid);Num(b,at+16,8,p.page_number);Num(b,at+24,8,p.page_generation);Put(b,at+32,p.page_size_profile_uuid);}
auto Hash(const Bytes& b){std::array<byte,32> h{};Check(SHA256(b.data(),b.size(),h.data())!=nullptr,"independent SHA256");return h;}
void Seal(Bytes& b,bool roots=true){
  if(roots){u32 used=0;for(unsigned i=0;i<4;++i)used|=u32(b[140+i])<<(8*i);
    Bytes material{'S','B','C','P','S','E','T',byte(b[136]==2?'2':'1')};
    material.insert(material.end(),b.begin()+160,b.begin()+256);
    if(b[136]==2)material.insert(material.end(),b.begin()+408,b.begin()+424);
    material.insert(material.end(),b.begin()+512,b.begin()+used);const auto h=Hash(material);std::copy(h.begin(),h.end(),b.begin()+336);}
  std::fill(b.begin()+368,b.begin()+400,0);const auto h=Hash(b);std::copy(h.begin(),h.end(),b.begin()+368);
}
template<class Root> Bytes Oracle(const Root& r){const auto& h=r.header;Bytes b(h.page_size_bytes,0);
  std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,h.page_type);Num(b,20,2,1);Num(b,22,2,1);
  Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Num(b,88,8,h.flags);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);
  u64 fnv=14695981039346656037ull;for(unsigned i=0;i<128;++i){fnv^=b[i];fnv*=1099511628211ull;}Num(b,96,8,fnv);
  const bool operation=!r.creator_operation_uuid.is_nil();std::copy_n(operation?"SBCPNT02":"SBCPNT01",8,b.begin()+128);
  Num(b,136,2,operation?2:1);Num(b,138,2,384);Num(b,140,4,512+112*r.roots.size());Put(b,144,r.object_uuid);
  Num(b,160,8,r.checkpoint_generation);Num(b,168,8,r.root_set_generation);Num(b,176,8,r.selected_local_transaction_id);
  Num(b,184,8,r.stable_local_transaction_id);Num(b,192,8,r.local_durable_transaction_id);Num(b,200,8,r.cluster_quorum_transaction_id);
  Put(b,208,r.timeline_uuid);Put(b,224,r.creator_transaction_uuid);Num(b,240,8,r.creator_local_transaction_id);Num(b,248,8,r.flags);
  if(r.predecessor)Ref(b,256,*r.predecessor);std::copy(r.predecessor_sha256.begin(),r.predecessor_sha256.end(),b.begin()+304);Num(b,400,8,r.completed?1:0);Put(b,408,r.creator_operation_uuid);
  for(usize i=0;i<r.roots.size();++i){const auto& root=r.roots[i];const auto at=512+112*i;
    Num(b,at,2,root.role);Num(b,at+4,4,root.page_type);Ref(b,at+8,root.page);Put(b,at+56,root.object_uuid);std::copy(root.sha256.begin(),root.sha256.end(),b.begin()+at+72);}
  Seal(b);return b;
}
db::NativeCheckpointRoot Example(unsigned profile=0,unsigned member=0,unsigned count=10){
  const auto& own=d::kCanonicalFilespacePageProfiles[profile];const auto& target=d::kCanonicalFilespacePageProfiles[member];
  db::NativeCheckpointRoot r;r.header={own.page_size_bytes,0x300,Id(1),Id(2),Id(10),19,109,0,own.uuid};
  r.object_uuid=Id(20);r.checkpoint_generation=1;r.root_set_generation=8;r.selected_local_transaction_id=17;
  r.stable_local_transaction_id=12;r.local_durable_transaction_id=16;r.timeline_uuid=Id(21);r.creator_transaction_uuid=Id(22);r.creator_local_transaction_id=17;
  constexpr unsigned types[]={0,769,770,9,3,5,10,11,8,5,771,773,775,776,777,779,1280};
  for(unsigned role=1;role<=count;++role){db::NativeCheckpointRootReference ref;
    ref.role=role;ref.page_type=types[role];ref.page={Id(3),30+role,80+role,target.uuid};ref.object_uuid=Id(100+role);ref.sha256.fill(byte(role));r.roots.push_back(ref);}
  if(count>=14){r.flags=4;r.cluster_quorum_transaction_id=15;}return r;
}
namespace m=scratchbird::core::memory;
using ME=db::NativeCheckpointRootMemoryError;
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
    r.route_label="storage.checkpoint.conformance";r.purpose="actual checkpoint image and metadata";
    r.binary_operation_uuid=binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
    r.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(64).bytes},
      {m::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
    r.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label="checkpoint resource conformance";
    for(const auto& scope:r.scope_chain){m::HierarchicalMemoryBudget b;b.scope=scope;b.hard_limit_bytes=bytes;
      b.provenance=r.provenance;Check(ledger.SetBudget(b).ok(),"actual parent budget");}
    auto grant=m::AcquireReservationBackedMemoryResource(r);Check(grant.ok(),"actual node-issued metadata grant");
    auto adopted=db::AdoptNativeStorageMemory(binding,grant.resource);Check(adopted.ok()&&!grant.resource,"exclusive native adoption");
    memory=std::move(adopted.memory);
  }
  void Empty(){const auto s=manager.Snapshot();Check(!s.current_bytes&&!s.reserved_capacity_bytes&&
    !s.active_capacity_reservation_count&&!ledger.Snapshot().current_bytes,"all real checkpoint charges released");}
};
struct MemoryFile {
  std::filesystem::path directory,path;
  d::FileDevice device;
  db::NativeCheckpointRoot value;
  Bytes bytes;
  d::NativeCommonPageHeaderBinding expected;
  explicit MemoryFile(unsigned profile):value(Example(profile)),bytes(Oracle(value)){
    char name[]="/tmp/sb-checkpoint-memory-XXXXXX";const auto* made=mkdtemp(name);if(!made)throw std::runtime_error("mkdtemp");
    directory=made;path=directory/"native.bin";
    Check(device.Open(path.string(),d::FileOpenMode::create_new).ok(),"actual checkpoint source");
    expected={{value.header.database_uuid,value.header.filespace_uuid,value.header.page_size_profile_uuid},
      value.header.page_number,value.header.page_generation,0x300,value.header.page_uuid};
    Bootstrap();Store(bytes);Check(device.Sync().ok()&&device.Close().ok()&&
      device.Open(path.string(),d::FileOpenMode::open_existing).ok(),"cold reopen independent checkpoint image");
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
  void Store(const Bytes& image){Check(device.WriteAt(value.header.page_number*u64(value.header.page_size_bytes),image.data(),image.size()).ok()&&device.Sync().ok(),"actual independent checkpoint");}
  auto Read(MemoryFixture& f){reads=hashes=0;return db::ReadNativeCheckpointRootWithMemoryFromOpenDevice(device,expected,value.object_uuid,f.memory,f.binding);}
};
void NoRoot(const db::NativeCheckpointRootMemoryResult& r){Check(!r.ok()&&!r.root&&r.image.empty()&&!r.arena,"refusal exposes no image metadata or owner prefix");}
void MemoryTests(){
  for(unsigned profile=0;profile<5;++profile){MemoryFile file(profile);
    const auto capacity=db::NativeCheckpointRootWorkspaceBytes(file.value.header.page_size_profile_uuid);
    Check(capacity==file.bytes.size()+16*sizeof(db::NativeCheckpointRootReference)+(alignof(std::max_align_t)-1),"independent native backing size formula");
    {
      MemoryFixture f(capacity);
      {auto guard=file.device.AcquireOperationGuard();allocation_device_mutex=guard.mutex();}
      allocation_lock_free=false;auto read=file.Read(f);
      Check(read.ok()&&allocation_lock_free,"real metadata backing allocated outside device guard");
      Check(last_read_buffer==read.image.data()&&read.image.size()==file.bytes.size()&&
        std::equal(read.image.begin(),read.image.end(),file.bytes.begin(),file.bytes.end()),"actual read destination is returned charged image");
      const auto begin=reinterpret_cast<std::uintptr_t>(read.image.data());
      const auto inside=[&](const void* ptr,usize bytes){auto n=reinterpret_cast<std::uintptr_t>(ptr);return n>=begin&&n-begin<=capacity&&bytes<=capacity-(n-begin);};
      Check(inside(read.root->roots.data(),read.root->roots.size_bytes())&&
        Oracle(*read.root)==file.bytes,"actual metadata resides in same charged block with exact binary records");
      Check(f.manager.Snapshot().current_bytes==capacity&&f.memory.Snapshot().allocated_bytes==capacity&&
        f.ledger.Snapshot().current_bytes==capacity&&read.arena.Snapshot().retained_bytes==capacity,"actual physical parent and arena charges agree");
      auto full=file.Read(f);NoRoot(full);Check(full.error==ME::memory_allocation_failure&&reads==0,"simultaneous live image prevents uncharged second reader");
      const auto revoked=f.ledger.CleanupOwner(f.binding.owner_uuid.bytes);Check(revoked.retained_bytes==capacity,"revocation retains actual image and metadata");
      auto denied=file.Read(f);NoRoot(denied);Check(denied.error==ME::memory_binding_failure&&reads==0,"revoked grant prevents new source reads");
      f.memory={};Check(f.manager.Snapshot().current_bytes==capacity,"returned metadata owner survives caller workspace");
      std::thread worker([retained=std::move(read)]()mutable{budget=0;retained={};if(budget!=0)std::abort();budget=-1;});worker.join();f.Empty();
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
      auto r=file.Read(valid);NoRoot(r);Check(r.error==ME::invalid_request&&reads==0&&
        !valid.memory.Snapshot().allocation_count,"invalid typed page binding refuses before physical admission");
      file.expected=original_binding;
    }
    for(unsigned field=0;field<4;++field){auto wrong=valid.binding;
      const std::array<Uuid*,4> fields{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};*fields[field]=Id(99);
      reads=0;auto denied=db::ReadNativeCheckpointRootWithMemoryFromOpenDevice(file.device,file.expected,file.value.object_uuid,valid.memory,wrong);
      NoRoot(denied);Check(denied.error==ME::memory_binding_failure&&!reads&&!valid.manager.Snapshot().current_bytes,"exact binary memory identity before allocation or I/O");}
    MemoryFixture small(capacity-1);auto short_grant=file.Read(small);NoRoot(short_grant);
    Check(short_grant.error==ME::memory_allocation_failure&&!reads&&!small.manager.Snapshot().current_bytes,"one byte short cannot obtain image or metadata");small.memory={};small.Empty();
    file.expected.page_generation++;{auto r=file.Read(valid);NoRoot(r);Check(r.error==ME::header_failure,"stale generation binds actual source");}file.expected.page_generation--;
    auto object=db::ReadNativeCheckpointRootWithMemoryFromOpenDevice(file.device,file.expected,Id(99),valid.memory,valid.binding);NoRoot(object);Check(object.error==ME::object_mismatch,"exact checkpoint object identity");
    for(unsigned n=1;n<=2;++n){fail_read=n;auto r=file.Read(valid);fail_read=0;NoRoot(r);
      Check(reads==n&&!valid.manager.Snapshot().current_bytes,"every real read failure releases admitted payload");}
    short_read=2;{auto r=file.Read(valid);Check(r.ok()&&reads==3,"legal short physical read completes");}short_read=0;
    short_read=2;eof_read=3;{auto r=file.Read(valid);NoRoot(r);Check(r.error==ME::io_failure&&r.page_bytes_read==file.bytes.size()-1,"partial then EOF retains actual read progress only");}short_read=eof_read=0;
    bootstrap_fault=true;{auto r=file.Read(valid);NoRoot(r);Check(!bootstrap_fault&&r.error==ME::bootstrap_failure,"real bootstrap digest failure reached");}
    for(unsigned phase=1;phase<=2;++phase)for(unsigned method=1;method<=4;++method){fault_context=phase;fault=method;
      auto r=file.Read(valid);NoRoot(r);Check(fault==0&&r.checkpoint_error==E::hash_failure,"each method in each actual checkpoint digest reached");}
    for(unsigned phase=1;phase<=2;++phase){hash_at=phase;auto r=file.Read(valid);hash_at=0;NoRoot(r);
      Check(r.checkpoint_error==E::hash_failure&&hashes==phase,"each actual checkpoint digest context failure typed");}
    Check(writes==no_writes&&syncs==no_syncs,"reader and refusal paths never write or sync source");
    auto corrupt=file.bytes;corrupt.back()^=1;file.Store(corrupt);
    {auto guard=file.device.AcquireOperationGuard();deallocation_device_mutex=guard.mutex();}deallocation_lock_free=false;
    {auto r=file.Read(valid);NoRoot(r);Check(r.error==ME::checkpoint_failure&&deallocation_lock_free,"failed image cleanup occurs after releasing device guard");}file.Store(file.bytes);
    file.Bootstrap(0,Id(99));{auto r=file.Read(valid);NoRoot(r);Check(r.error==ME::bootstrap_failure,"actual bootstrap identity mismatch");}file.Bootstrap(1);
    {auto r=file.Read(valid);NoRoot(r);Check(r.error==ME::encrypted_requires_authority&&reads==1,"encrypted filespace refuses before checkpoint payload I/O");}file.Bootstrap();
    {auto encrypted=file.value;encrypted.header.flags=1;file.Store(Oracle(encrypted));auto r=file.Read(valid);NoRoot(r);
      Check(r.error==ME::encrypted_requires_authority,"encrypted common header never parsed as plaintext");}file.Store(file.bytes);
    for(unsigned member=0;member<5;++member)for(unsigned mask=0;mask<64;++mask)for(bool operation:{false,true})for(bool completed:{false,true}){
      auto variant=Example(profile,member,16);variant.completed=completed;
      std::erase_if(variant.roots,[&](const auto& root){return root.role>10&&!(mask&(1u<<(root.role-11)));});
      variant.flags=(mask&(1u<<3))?4:0;variant.cluster_quorum_transaction_id=variant.flags?15:0;
      if(operation){variant.creator_transaction_uuid={};variant.creator_local_transaction_id=0;variant.creator_operation_uuid=Id(98);}
      const auto encoded=Oracle(variant);file.Store(encoded);auto r=file.Read(valid);
      Check(r.ok()&&Oracle(*r.root)==encoded,"actual managed checkpoint retains all 64 optional role subsets across mixed profiles creators and completion without upgrading authority");
    }
    file.Store(file.bytes);
    if(!profile){unsigned long sites=0;
      {allocations=0;measuring=true;auto r=file.Read(valid);measuring=false;sites=allocations;Check(r.ok(),"measure full admitted reader allocation sites");}
      for(unsigned long n=0;n<sites;++n){budget=n;auto r=file.Read(valid);budget=-1;
        if(r.ok())Check(Oracle(*r.root)==file.bytes,"optional telemetry loss cannot change decoded result");
        else NoRoot(r);
        r={};Check(!valid.manager.Snapshot().current_bytes&&!valid.memory.Snapshot().allocated_bytes,"every allocation fault retains zero physical payload after cleanup");
        {auto retry=file.Read(valid);Check(retry.ok(),"same-owner retry after each allocation failure");}
      }
      Check(sites>0,"allocation sweep executed");std::cout<<"governed checkpoint metadata faults="<<sites<<'\n';
    }
    Check(file.device.Close().ok(),"close source");auto closed=file.Read(valid);NoRoot(closed);
    Check(closed.error==ME::bootstrap_failure&&closed.bootstrap_error==d::FilespaceBootstrapError::device_not_open,"reader never reopens closed source");
    valid.memory={};valid.Empty();
  }
}
void Codecs(){
  for(unsigned profile=0;profile<5;++profile)for(unsigned member=0;member<5;++member)
    for(unsigned count=10;count<=16;++count)for(bool operation:{false,true})for(bool complete:{false,true}){
      auto value=Example(profile,member,count);value.completed=complete;
      if(operation){value.creator_transaction_uuid={};value.creator_local_transaction_id=0;value.creator_operation_uuid=Id(23);}
      auto bytes=Oracle(value);std::array<db::NativeCheckpointRootReference,16> roots;
      allocations=0;measuring=true;budget=0;auto r=db::DecodeNativeCheckpointRootInto(bytes,roots);
      const auto remaining=budget;budget=-1;measuring=false;
      Check(r.ok()&&!allocations&&remaining==0,"caller-backed checkpoint has no hidden allocations");
      Check(r.root->roots.data()==roots.data()&&r.root->roots.size()==count&&Oracle(*r.root)==bytes,"full independent image and exact-length native root span");
      auto short_result=db::DecodeNativeCheckpointRootInto(bytes,std::span(roots).first(count-1));
      Check(!short_result.root&&short_result.error==E::resource_exhausted,"one-short roots refuse without partial image");
      auto alias=db::DecodeNativeCheckpointRootInto(bytes,{reinterpret_cast<db::NativeCheckpointRootReference*>(bytes.data()),count});
      Check(!alias.root&&alias.error==E::invalid_backing&&bytes==Oracle(value),"overlapping storage refuses before writes");
      Bytes unaligned(1);unaligned.insert(unaligned.end(),bytes.begin(),bytes.end());
      Check(db::DecodeNativeCheckpointRootInto(std::span<const byte>(unaligned).subspan(1),roots).ok(),"unaligned image is decoded bytewise");
      std::fill(bytes.begin(),bytes.end(),0);Check(Oracle(*r.root)==Oracle(value),"decoded fields do not borrow encoded image lifetime");
    }
  auto value=Example();auto good=Oracle(value);std::array<db::NativeCheckpointRootReference,16> roots;
  for(usize at=0;at<good.size();++at){auto bad=good;bad[at]^=1;auto r=db::DecodeNativeCheckpointRootInto(bad,roots);Check(!r.ok()&&!r.root,"every-byte corruption returns no root view");}
  for(unsigned at:{128u,136u,138u,140u,160u,184u,208u,248u,400u,408u,511u,512u,514u,516u,520u,584u,616u,8191u}){
    auto bad=good;bad[at]^=0x80;Seal(bad,false);const auto owned=db::DecodeNativeCheckpointRoot(bad);const auto r=db::DecodeNativeCheckpointRootInto(bad,roots);
    Check(!r.root&&r.error==owned.error,"resealed malformed checkpoint preserves exact refusal");}
  for(unsigned n=0;n<5;++n){auto bad=value;
    if(n==0){bad.roots[1].page=bad.roots[0].page;}
    if(n==1)bad.roots[1].page.page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;
    if(n==2)bad.roots[1].role=1;
    if(n==3)bad.roots.front().sha256={};
    if(n==4)bad.roots.front().object_uuid={};
    const auto r=db::DecodeNativeCheckpointRootInto(Oracle(bad),roots);Check(!r.root&&r.error==E::invalid_roots,"fully resealed invalid root semantics refused");}
  for(unsigned which=1;which<=2;++which){hashes=0;hash_at=which;const auto r=DenyCodecAllocation([&]{return db::DecodeNativeCheckpointRootInto(good,roots);});hash_at=0;
    Check(!r.root&&r.error==E::hash_failure&&hashes==which,"both real digest contexts fail independently");}
  for(unsigned context=1;context<=2;++context)for(unsigned n=1;n<=4;++n){hashes=0;fault_context=context;fault=n;
    const auto r=DenyCodecAllocation([&]{return db::DecodeNativeCheckpointRootInto(good,roots);});Check(!fault&&!r.root&&r.error==E::hash_failure,"both full-page and root-set provider failures remain typed");}
  for(unsigned size:{0u,127u,511u,8191u,8193u}){auto bad=good;bad.resize(size);const auto r=db::DecodeNativeCheckpointRootInto(bad,roots);Check(!r.root&&r.error==E::invalid_header,"invalid image lengths refuse");}
  value.checkpoint_generation=2;value.predecessor=d::NativePageReference{Id(4),7,9,d::kCanonicalFilespacePageProfiles[4].uuid};value.predecessor_sha256.fill(0x91);
  const auto linked=Oracle(value);const auto r=db::DecodeNativeCheckpointRootInto(linked,roots);Check(r.ok()&&Oracle(*r.root)==linked,"full predecessor linkage preserved without granting authority");
  auto shared=Example();shared.roots[8]=shared.roots[4];shared.roots[8].role=9;
  const auto shared_image=Oracle(shared);const auto shared_result=db::DecodeNativeCheckpointRootInto(shared_image,roots);
  Check(shared_result.ok()&&Oracle(*shared_result.root)==shared_image,"exact catalog-feature root alias remains valid");
  for(unsigned field=0;field<4;++field){auto bad=shared;
    if(field==0)++bad.roots[8].page.page_generation;
    if(field==1)bad.roots[8].object_uuid=Id(199);
    if(field==2)bad.roots[8].sha256[0]^=1;
    if(field==3)bad.roots[8].page.page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;
    const auto result=db::DecodeNativeCheckpointRootInto(Oracle(bad),roots);
    Check(!result.root&&result.error==E::invalid_roots,"conflicting shared root metadata cannot be interpreted as a valid alias");}
}
}
int main(){try{Codecs();MemoryTests();std::cout<<"PASS governed checkpoint checks="<<checks<<" not_SQL_E2E=true\n";return 0;}
  catch(...){budget=-1;std::cerr<<"FAIL checks="<<checks<<'\n';return 1;}}
