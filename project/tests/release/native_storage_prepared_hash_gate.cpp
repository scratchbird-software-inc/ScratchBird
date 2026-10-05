// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_storage_memory.hpp"
#include "filespace_page_zero.hpp"
#include "disk_device.hpp"
#include "crypto_memory_adapter.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <unistd.h>

namespace d=scratchbird::storage::disk;
namespace db=scratchbird::storage::database;
namespace h=scratchbird::core::hash;
namespace m=scratchbird::core::memory;
using scratchbird::core::platform::Uuid;
using E=h::CryptoMemoryError;
thread_local bool deny=false, reject_fresh=false;
unsigned checks=0, heap_attempts=0, fresh_attempts=0, hash_number=0, fault_hash=0, fault=0;
void Check(bool value,const char* why){++checks;if(!value){deny=false;std::cerr<<"FAIL "<<why<<'\n';std::abort();}}
void* operator new(std::size_t n){if(deny){++heap_attempts;throw std::bad_alloc();}if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void* operator new(std::size_t n,std::align_val_t a){if(deny){++heap_attempts;throw std::bad_alloc();}void* p=nullptr;
  if(!posix_memalign(&p,static_cast<std::size_t>(a),n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
void operator delete(void* p,std::align_val_t)noexcept{std::free(p);}void operator delete[](void* p,std::align_val_t)noexcept{std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t,std::align_val_t)noexcept{std::free(p);}
extern "C" {
int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
int __wrap_EVP_Digest(const void* b,size_t n,unsigned char* out,unsigned int* size,const EVP_MD* md,ENGINE* e){
  if(reject_fresh){++fresh_attempts;return 0;}return __real_EVP_Digest(b,n,out,size,md,e);}
EVP_MD_CTX* __real_EVP_MD_CTX_new();
EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){if(reject_fresh){++fresh_attempts;return nullptr;}return __real_EVP_MD_CTX_new();}
int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* md,ENGINE* e){++hash_number;
  if(fault==1&&hash_number==fault_hash){fault=0;return 0;}return __real_EVP_DigestInit_ex(c,md,e);}
int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* b,size_t n){if(fault==2&&hash_number==fault_hash){fault=0;return 0;}
  return __real_EVP_DigestUpdate(c,b,n);}
int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* out,unsigned int* n){
  if(fault==3&&hash_number==fault_hash){fault=0;std::fill_n(out,32,0xff);return 0;}
  const auto rc=__real_EVP_DigestFinal_ex(c,out,n);
  if(fault==4&&hash_number==fault_hash){fault=0;*n=31;}return rc;}
}
Uuid Id(unsigned n){Uuid id{};id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=n;return id;}
h::CryptoMemoryBinding CryptoBinding(const db::NativeStorageMemoryBinding& b){return {b.database_uuid,b.operation_uuid,b.owner_uuid,b.context_uuid};}
struct Resources {
  static constexpr std::size_t capacity=16*1024*1024;
  static auto Policy(){auto p=m::DefaultLocalEngineMemoryPolicy();p.hard_limit_bytes=capacity;p.per_context_limit_bytes=capacity;return p;}
  m::MemoryManager manager;
  m::HierarchicalMemoryBudgetLedger ledger{3,5};
  Resources():manager(Policy()){}
  db::NativeStorageMemory Make(const db::NativeStorageMemoryBinding& binding,std::size_t bytes){
    m::ReservationBackedMemoryResourceRequest r;r.memory_manager=&manager;r.reservation_ledger=&ledger;
    r.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.category=m::MemoryCategory::page_buffer;r.requested_bytes=bytes;r.memory_class="page_buffer";
    r.route_label="storage.prepared-hash.conformance";r.purpose="actual storage and provider backing";
    r.binary_operation_uuid=binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
    r.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(90).bytes},
      {m::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
    r.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;r.provenance.source_label="storage provider conformance";
    for(const auto& scope:r.scope_chain){m::HierarchicalMemoryBudget b;b.scope=scope;b.hard_limit_bytes=capacity;b.provenance=r.provenance;
      Check(ledger.SetBudget(b).ok(),"actual ancestor budget");}
    auto grant=m::AcquireReservationBackedMemoryResource(r);Check(grant.ok(),"actual binary grant");
    auto adopted=db::AdoptNativeStorageMemory(binding,grant.resource);Check(adopted.ok()&&!grant.resource,"storage adopts exact grant");
    return std::move(adopted.memory);
  }
};
d::FilespacePageZero Example(unsigned own,unsigned member){
  const auto& p=d::kCanonicalFilespacePageProfiles[own];const auto& q=d::kCanonicalFilespacePageProfiles[member];
  d::FilespacePageZero r;r.bootstrap={Id(1),Id(2),p.uuid,d::kNativeBootstrapIntegrityProfile,{},p.page_size_bytes,1,0,1,1};
  r.page_uuid=Id(10);r.creation_operation_uuid=Id(11);r.writer_identity_uuid=Id(12);r.page_generation=3;r.root_set_generation=8;
  r.total_pages=64;r.free_pages=10;r.preallocated_pages=3;r.creation_utc_millis=123456;
  constexpr unsigned types[]={0,8,5,3,769,9,10,11,5,768,771,773,775,779,1301,1030,1025,777,782,782,1280,1280};
  for(unsigned kind=1;kind<=21;++kind){if(kind>=15&&kind<=17)continue;const bool local=kind==3||kind>=18;
    r.roots.push_back({static_cast<d::u16>(kind),types[kind],local?Id(2):Id(3),kind,50+kind,
      local?p.uuid:q.uuid,kind>=20?Id(81):kind>=18?Id(80):Id(100+kind)});}
  return r;
}
struct Fixture {
  std::string root,path;d::FileDevice device;
  Fixture(){char pattern[]="/tmp/sb-01e-prepared-storage-XXXXXX";const auto* p=mkdtemp(pattern);Check(p,"temporary actual-file fixture");
    root=p;path=root+"/data";Check(device.Open(path,d::FileOpenMode::create_new).ok(),"create native file");}
  ~Fixture(){if(device.is_open())Check(device.Close().ok(),"close native file");std::error_code ec;std::filesystem::remove_all(root,ec);}
};
struct GuardedDenial {GuardedDenial(){deny=reject_fresh=true;}~GuardedDenial(){deny=reject_fresh=false;}};
void Test(h::CryptoMemoryPool& pool,const h::CryptoMemoryBinding& binding,db::NativeStorageMemory& memory,
          h::CryptoMemoryPool& process){
  h::PreparedSha256 session;Check(session.Prepare(pool,binding)==E::none,"prepare provider before guards");
  for(unsigned own=0;own<5;++own){Fixture f;
    auto payload=memory.AllocatePage(d::kCanonicalFilespacePageProfiles[own].uuid);Check(payload.ok(),"actual image backing");
    for(unsigned member=0;member<5;++member){const auto example=Example(own,member);
      const auto legacy=d::EncodeFilespacePageZero(example);Check(legacy.ok(),"existing canonical encoder");
      const auto bootstrap=d::EncodeFilespaceBootstrap(example.bootstrap);Check(bootstrap.ok(),"existing canonical bootstrap");
      // Independent provider entry verifies the embedded digest and zero-slot
      // framing, rather than accepting encode/decode agreement alone.
      auto independent=*legacy.bytes;std::array<unsigned char,32> digest{};
      std::fill(independent.begin()+4448,independent.begin()+4480,0);
      Check(SHA256(independent.data(),independent.size(),digest.data())&&
        std::equal(digest.begin(),digest.end(),legacy.bytes->begin()+4448),"independent complete page-zero digest");
      Check(SHA256(bootstrap.bytes->data(),104,digest.data())&&
        std::equal(digest.begin(),digest.end(),bootstrap.bytes->begin()+104),"independent bootstrap digest");
      {h::PreparedSha256Scope route(session);reject_fresh=true;
        const auto encoded=d::EncodeFilespacePageZero(example);reject_fresh=false;
        Check(encoded.ok()&&encoded.bytes==legacy.bytes,"owning encoding consumes retained provider with identical canonical image");}
      Check(f.device.WriteAt(0,legacy.bytes->data(),legacy.bytes->size()).ok()&&f.device.Sync().ok(),"stage exact real bytes");
      const auto root_before=process.Snapshot();const auto op_before=pool.Snapshot();const auto charge=memory.Snapshot().allocated_bytes;
      std::array<d::FilespaceRootReference,32> roots{};
      {
        d::FileDevice::BoundedIoBatch observations(f.device);h::PreparedSha256Scope route(session);
        const auto guard=f.device.AcquireOperationGuard();GuardedDenial denied;
        Check(observations.ReadAt(0,payload.buffer.data(),payload.buffer.size()).ok(),"bounded actual image read");
        const auto decoded=d::DecodeFilespacePageZeroInto({payload.buffer.data(),payload.buffer.size()},roots);
        Check(decoded.ok()&&decoded.record->roots.size()==example.roots.size()&&decoded.record->page_uuid==example.page_uuid,
          "all25 ordered profile pairs decode with prepared provider");
        for(std::size_t n=0;n<example.roots.size();++n){const auto& x=roots[n];const auto& y=example.roots[n];
          Check(x.kind==y.kind&&x.page_type==y.page_type&&x.filespace_uuid==y.filespace_uuid&&x.page_number==y.page_number&&
            x.page_generation==y.page_generation&&x.page_size_profile_uuid==y.page_size_profile_uuid&&x.object_uuid==y.object_uuid,
            "every binary root field preserved");}
        const auto encoded=d::EncodeFilespaceBootstrap(example.bootstrap);
        Check(encoded.ok()&&encoded.bytes==bootstrap.bytes,"prepared bootstrap emits identical canonical bytes");
        Check(process.Snapshot().allocations==root_before.allocations,"guarded hashing never falls back to process pool");
      }
      Check(pool.Snapshot().allocations>op_before.allocations&&pool.Snapshot().live_bytes==op_before.live_bytes&&
        pool.Snapshot().backing_bytes==op_before.backing_bytes&&memory.Snapshot().allocated_bytes==charge,"actual retained provider backing and charge unchanged");
      for(unsigned phase=1;phase<=2;++phase)for(unsigned mode=1;mode<=4;++mode){
        Check(session.Prepare(pool,binding)==E::none,"explicit reprepare outside guard");
        hash_number=0;fault_hash=phase;fault=mode;
        {h::PreparedSha256Scope route(session);const auto guard=f.device.AcquireOperationGuard();GuardedDenial denied;
          const auto failed=d::DecodeFilespacePageZeroInto({payload.buffer.data(),payload.buffer.size()},roots);
          Check(!fault&&!failed.record&&failed.error==d::FilespacePageZeroError::hash_provider_failure,"each bootstrap/full-image provider stage refuses without prefix");
          const auto retry=d::DecodeFilespacePageZeroInto({payload.buffer.data(),payload.buffer.size()},roots);
          Check(!retry.record&&retry.error==d::FilespacePageZeroError::hash_provider_failure,"poisoned provider never falls back");
        }
      }
      Check(session.Prepare(pool,binding)==E::none,"recover prepared provider after failures");
    }
    Check(f.device.Close().ok()&&f.device.Open(f.path,d::FileOpenMode::open_existing_read_only).ok(),"cold reopen final exact image");
    {d::FileDevice::BoundedIoBatch observations(f.device);h::PreparedSha256Scope route(session);
      const auto guard=f.device.AcquireOperationGuard();GuardedDenial denied;std::array<d::FilespaceRootReference,32> roots{};
      Check(observations.ReadAt(0,payload.buffer.data(),payload.buffer.size()).ok()&&
        d::DecodeFilespacePageZeroInto({payload.buffer.data(),payload.buffer.size()},roots).ok(),"reopened native image validates under actual bounded provider");}
    Check(payload.buffer.Reset().ok(),"release admitted image outside fence");
  }
  Check(session.Close()==E::none,"close prepared context outside device guards");
}
int main(){
  Resources resources;constexpr std::size_t pool_bytes=4*1024*1024;
  const db::NativeStorageMemoryBinding root_binding{Id(1),Id(30),Id(31),Id(32)},op_binding{Id(1),Id(40),Id(41),Id(42)};
  auto root_memory=resources.Make(root_binding,pool_bytes),op_memory=resources.Make(op_binding,pool_bytes+262144);
  auto root_arena=root_memory.CreateArena(root_binding,pool_bytes,alignof(std::max_align_t));
  auto op_arena=op_memory.CreateArena(op_binding,pool_bytes,alignof(std::max_align_t));
  Check(root_arena.ok()&&op_arena.ok(),"actual pre-admitted provider arenas");
  const auto root_block=root_arena.arena.Allocate(pool_bytes,alignof(std::max_align_t));
  const auto op_block=op_arena.arena.Allocate(pool_bytes,alignof(std::max_align_t));
  Check(root_block.ok()&&op_block.ok(),"consume fixed backing before any storage guard");
  h::CryptoMemoryPool process,operation;
  Check(process.Open(CryptoBinding(root_binding),root_block.pointer,root_block.bytes)==E::none&&
    operation.Open(CryptoBinding(op_binding),op_block.pointer,op_block.bytes)==E::none,"exact binary-bound provider pools");
  Check(h::InstallCryptoMemoryAdapter(process)==E::none,"explicit bootstrap before crypto");
  Test(operation,CryptoBinding(op_binding),op_memory,process);
  Check(!heap_attempts&&!fresh_attempts,"no fresh provider or C++ heap fallback beneath guards");
  Check(resources.manager.Snapshot().current_bytes==2*pool_bytes,"both real provider backings retained after image cleanup");
  OPENSSL_thread_stop();OPENSSL_cleanup();
  Check(h::StopCryptoMemoryAdapter()==E::none&&operation.Close()==E::none&&process.Close()==E::none,"actual provider drain before backing release");
  op_arena.arena={};root_arena.arena={};op_memory={};root_memory={};
  const auto memory=resources.manager.Snapshot();Check(!memory.current_bytes&&!memory.reserved_capacity_bytes&&
    !memory.active_capacity_reservation_count&&!resources.ledger.Snapshot().current_bytes,"complete actual memory cleanup");
  std::cout<<"PASS native prepared storage hashes "<<checks<<" checks profiles=25\n";
}
