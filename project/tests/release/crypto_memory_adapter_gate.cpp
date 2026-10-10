// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "crypto_memory_adapter.hpp"
#include "openpgp_seipd.hpp"
#include "argon2_kdf.hpp"
#include "reservation_backed_memory_resource.hpp"
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <pthread.h>
#include <thread>
#include <vector>

namespace h=scratchbird::core::hash;
namespace m=scratchbird::core::memory;
using E=h::CryptoMemoryError;
using scratchbird::core::platform::Uuid;
thread_local bool deny_cpp=false;
thread_local int fault=0;
thread_local bool fail_next_lock=false;
std::atomic<bool> block_update{false},update_entered{false},release_update{false};
void* operator new(std::size_t n){if(deny_cpp)throw std::bad_alloc();if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
extern "C" {
int __real_pthread_mutex_lock(pthread_mutex_t*);
int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex){if(fail_next_lock){fail_next_lock=false;return EAGAIN;}return __real_pthread_mutex_lock(mutex);}
EVP_MD_CTX* __real_EVP_MD_CTX_new();
int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,std::size_t);
int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){if(fault==5)return nullptr;return __real_EVP_MD_CTX_new();}
int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* m,ENGINE* e){if(fault==1)return 0;return __real_EVP_DigestInit_ex(c,m,e);}
int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* p,std::size_t n){
  if(block_update.load()){update_entered=true;while(!release_update.load())std::this_thread::yield();}
  if(fault==2)return 0;
  if(fault==6){auto* allocation=OPENSSL_malloc(16*1024*1024);if(allocation){OPENSSL_free(allocation);std::abort();}return 0;}
  return __real_EVP_DigestUpdate(c,p,n);
}
int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* p,unsigned int* n){
  if(fault==3){std::memset(p,0xcc,32);return 0;}
  const int rc=__real_EVP_DigestFinal_ex(c,p,n);if(fault==4)*n=31;return rc;
}
}
unsigned checks=0;
void Check(bool condition,const char* name){++checks;if(!condition){deny_cpp=false;std::cerr<<"FAIL "<<name<<'\n';std::exit(1);}}
Uuid Id(unsigned n){Uuid id{};id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=n;return id;}
h::CryptoMemoryBinding Binding(unsigned n){return {Id(1),Id(n),Id(n+1),Id(n+2)};}
bool Zero(const h::Digest256& digest){return digest==h::Digest256{};}
h::Digest256 Hex(const char* hex){h::Digest256 out{};auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};
  for(unsigned i=0;i<32;++i)out[i]=nibble(hex[2*i])*16+nibble(hex[2*i+1]);return out;}
void* CustomMalloc(std::size_t n,const char*,int){return std::malloc(n);}
void* CustomRealloc(void* p,std::size_t n,const char*,int){return std::realloc(p,n);}
void CustomFree(void* p,const char*,int){std::free(p);}

struct Grant {
  m::MemoryManager manager;
  m::HierarchicalMemoryBudgetLedger ledger{3,5};
  h::CryptoMemoryBinding binding=Binding(20);
  std::unique_ptr<m::ReservationBackedMemoryResource> resource;
  void* pointer=nullptr;
  std::size_t bytes=8*1024*1024;
  static auto Policy(std::size_t bytes){auto p=m::DefaultLocalEngineMemoryPolicy();p.hard_limit_bytes=2*bytes;p.per_context_limit_bytes=2*bytes;return p;}
  explicit Grant(std::size_t admitted_bytes=8*1024*1024):manager(Policy(admitted_bytes)),bytes(admitted_bytes){
    m::ReservationBackedMemoryResourceRequest r;r.memory_manager=&manager;r.reservation_ledger=&ledger;
    r.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.category=m::MemoryCategory::core_runtime;r.requested_bytes=bytes;r.memory_class="crypto_provider";
    r.route_label="storage.crypto.conformance";r.purpose="actual provider backing";
    r.binary_operation_uuid=binding.operation.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context.bytes;
    r.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(90).bytes},
      {m::HierarchicalMemoryScopeKind::database,{},binding.database.bytes}};
    r.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label="crypto resource conformance";
    for(const auto& scope:r.scope_chain){m::HierarchicalMemoryBudget b;b.scope=scope;b.hard_limit_bytes=bytes;
      b.provenance=r.provenance;Check(ledger.SetBudget(b).ok(),"parent budget");}
    auto grant=m::AcquireReservationBackedMemoryResource(r);Check(grant.ok(),"actual binary memory grant");resource=std::move(grant.resource);
    auto allocated=resource->Allocate({bytes,alignof(std::max_align_t),"actual crypto backing"});
    Check(allocated.ok(),"physical backing from exact grant");pointer=allocated.pointer;
    Check(resource->Snapshot().allocated_bytes==bytes&&manager.Snapshot().current_bytes==bytes,"full physical backing charged");
  }
  void Release(){
    Check(resource->DeallocateNoAlloc(pointer,bytes,alignof(std::max_align_t)).ok(),"actual backing release");pointer=nullptr;
    Check(resource->ReleaseNoAlloc().ok(),"actual grant release");resource.reset();
    Check(!manager.Snapshot().current_bytes&&!manager.Snapshot().reserved_capacity_bytes&&!ledger.Snapshot().current_bytes,"zero actual retained charge after drain");
  }
};

void AllocatorChecks(h::CryptoMemoryPool& process){
  alignas(std::max_align_t) std::array<unsigned char,8192> a{},b{};
  h::CryptoMemoryPool first,second;auto ab=Binding(30),bb=Binding(40);
  Check(first.Open({},a.data(),a.size())==E::invalid_binding,"binary v7 identity required");
  Check(first.Open(ab,a.data()+1,a.size()-1)==E::invalid_backing,"unaligned backing");
  Check(first.Open(ab,a.data(),1)==E::invalid_backing,"short backing");
  Check(first.Open(ab,a.data(),std::numeric_limits<std::size_t>::max())==E::invalid_backing,"backing address overflow");
  Check(first.Open(ab,&first,sizeof(first))==E::invalid_backing,"control/backing overlap");
  Check(first.Open(ab,a.data(),a.size())==E::none,"first pool");
  Check(second.Open(bb,a.data(),a.size())==E::invalid_backing,"reject duplicate backing");
  Check(second.Open(bb,a.data()+16,a.size()-16)==E::invalid_backing,"reject intersecting backing suffix");
  Check(second.Open(bb,b.data(),b.size())==E::none,"two independent binary pools");
  Check(first.Open(ab,a.data(),a.size())==E::busy,"cannot overwrite live pool");
  void* saved;
  {
    h::CryptoMemoryScope active(first,ab);Check(active.ok(),"scope admission");
    Check(first.Close()==E::busy&&h::StopCryptoMemoryAdapter()==E::busy,"scope prevents drain");
    saved=OPENSSL_malloc(63);Check(saved,"real hooked allocation");std::memset(saved,0x5a,63);
    Check(reinterpret_cast<std::uintptr_t>(saved)%alignof(std::max_align_t)==0,"max alignment");
    for(unsigned dimension=0;dimension<4;++dimension){auto wrong=ab;
      std::array<Uuid*,4> fields{&wrong.database,&wrong.operation,&wrong.owner,&wrong.context};*fields[dimension]=Id(89);
      h::CryptoMemoryScope invalid(first,wrong);Check(invalid.error()==E::invalid_binding&&!OPENSSL_malloc(1),"each binary binding dimension enforced without fallback");}
    {h::CryptoMemoryScope deny(first,ab,true);Check(deny.ok()&&!OPENSSL_malloc(1)&&!OPENSSL_realloc(saved,65),"sealed scope denies malloc and growth");
      h::CryptoMemoryScope child(second,bb);Check(!OPENSSL_malloc(1),"nested scope cannot lift enclosing denial");}
    Check(first.Snapshot().live_bytes==63,"denial preserves old block");
    Check(!OPENSSL_realloc(saved,std::numeric_limits<std::size_t>::max()),"overflow resize retains old bytes");
    for(unsigned i=0;i<63;++i)Check(static_cast<unsigned char*>(saved)[i]==0x5a,"original payload retained");
    {h::CryptoMemoryScope nested(second,bb);saved=OPENSSL_realloc(saved,511);Check(saved,"resize succeeds in original pool");
      Check(first.Snapshot().live_bytes==511&&!second.Snapshot().live_blocks,"resize never transfers identity");}
    saved=OPENSSL_realloc(saved,17);Check(saved&&first.Snapshot().live_bytes==17,"shrink updates exact requested bytes");
    for(unsigned i=17;i<63;++i)Check(static_cast<unsigned char*>(saved)[i]==0,"shrink clears discarded bytes");
  }
  Check(first.Close()==E::busy,"live pointer prevents backing release");
  std::thread release([&]{OPENSSL_free(saved);});release.join();
  Check(!first.Snapshot().live_bytes&&!first.Snapshot().occupied_bytes,"cross-thread release reconciled");
  {
    h::CryptoMemoryScope active(first,ab);
    std::array<void*,256> blocks{};unsigned count=0;
    while(count<blocks.size()&&(blocks[count]=OPENSSL_malloc(23)))++count;
    Check(count>2&&count<blocks.size(),"bounded exhaustion no heap fallback");
    for(unsigned i=0;i<count;i+=2)OPENSSL_free(blocks[i]);
    for(unsigned i=1;i<count;i+=2)OPENSSL_free(blocks[i]);
    auto* all=OPENSSL_malloc(8000);Check(all,"coalescing recovers contiguous capacity");
    for(unsigned i=0;i<8000;++i)Check(static_cast<unsigned char*>(all)[i]==0,"free clears full reused backing");
    Check(OPENSSL_realloc(all,0)==nullptr,"zero realloc releases");
    auto* retained=OPENSSL_malloc(64);Check(retained,"retained revocation block");
    Check(first.Revoke()==E::none,"explicit revocation result");Check(!OPENSSL_malloc(1)&&!OPENSSL_realloc(retained,128),"revocation reaches already entered scope");
    Check(first.Close()==E::busy,"revocation does not erase retained bytes");OPENSSL_free(retained);
  }
  {h::CryptoMemoryScope revoked(first,ab);Check(revoked.error()==E::revoked&&!OPENSSL_malloc(1),"revoked nested scope denies");}
  Check(first.Close()==E::none&&second.Close()==E::none,"empty pools drain");
  {h::CryptoMemoryScope closed(second,bb);Check(closed.error()==E::closed&&!OPENSSL_malloc(1),"closed scope refuses without fallback");}
  Check(process.Close()==E::busy,"installed process backing retained");
  fail_next_lock=true;Check(process.Snapshot().observation_error==E::busy,"snapshot lock failure is not zero-accounting proof");
  fail_next_lock=true;Check(h::SnapshotCryptoMemoryAdapter().observation_error==E::busy,"aggregate snapshot failure explicit");
  fail_next_lock=true;Check(process.Revoke()==E::busy,"failed revocation not reported successful");
  Check(!process.Snapshot().revoked&&process.Snapshot().observation_error==E::none,"failed revocation leaves live process pool unchanged");
}

void DigestChecks(h::CryptoMemoryPool& pool,const h::CryptoMemoryBinding& binding){
  const auto abc=Hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const auto empty=Hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  const unsigned char input[]="abc";const h::HashDigestSegment segments[]={{input,1},{nullptr,0},{input+1,2}};
  h::PreparedSha256 session;Check(session.Prepare(pool,binding)==E::none,"prepare retained configured provider");
  const auto before=pool.Snapshot();
  for(unsigned i=0;i<100;++i){
    deny_cpp=true;const auto digest=session.Compute(segments,3);deny_cpp=false;
    Check(digest.ok()&&digest.digest==abc,"prepared digest exact independent SHA256 vector");
  }
  const auto after=pool.Snapshot();Check(after.allocations>=before.allocations&&after.live_bytes==before.live_bytes&&
    after.occupied_bytes==before.occupied_bytes&&after.backing_bytes==before.backing_bytes,"provider reset reuses charged backing without growth");
  Check(session.Compute(nullptr,0).digest==empty,"empty vector");
  const unsigned char boundary[]="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  const auto boundary_hash=Hex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  for(unsigned split=0;split<=56;++split){const h::HashDigestSegment parts[]={{boundary,split},{boundary+split,56-split}};
    Check(session.Compute(parts,2).digest==boundary_hash,"SHA256 padding boundary and every segment split");}
  std::vector<unsigned char> million(1000000,'a');const h::HashDigestSegment long_part{million.data(),million.size()};
  Check(session.Compute(&long_part,1).digest==Hex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),"million byte independent vector");
  Check(session.Compute(nullptr,1).error==h::Sha256PartsError::segments_missing,"missing segments");
  const h::HashDigestSegment invalid{nullptr,1};Check(session.Compute(&invalid,1).error==h::Sha256PartsError::segment_extent_invalid,"invalid extent");
  const h::HashDigestSegment overflow{input,std::numeric_limits<std::size_t>::max()};
  Check(session.Compute(&overflow,1).error==h::Sha256PartsError::segment_extent_invalid,"bit length overflow");
  {
    h::PreparedSha256Scope route(session);deny_cpp=true;
    const auto a=h::ComputeSha256DigestNative(input,3),b=h::ComputeSha256DigestNative(input,0);
    const auto c=h::ComputeSha256DigestPartsNative(segments,3);deny_cpp=false;
    Check(a.ok()&&a.digest==abc&&b.ok()&&b.digest==empty&&c.ok()&&c.digest==abc,"existing native calls use prepared provider");
  }
  for(int mode:{1,2,3,4,6}){
    fault=mode;deny_cpp=true;const auto failed=session.Compute(segments,3);deny_cpp=false;fault=0;
    Check(!failed.ok()&&Zero(failed.digest),"provider failure returns no prefix");
    Check(!session.Compute(segments,3).ok(),"poisoned provider requires explicit reprepare");
    Check(session.Prepare(pool,binding)==E::none&&session.Compute(segments,3).digest==abc,"outside-guard retry preserves selected hash");
  }
  for(int mode:{1,5}){fault=mode;const auto error=session.Prepare(pool,binding);fault=0;
    Check(error==E::provider_failure&&!session.Compute(segments,3).ok(),"prepare provider failure fixed refusal");
    Check(session.Prepare(pool,binding)==E::none,"prepare retry");}
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  Check(EVP_set_default_properties(nullptr,"provider=sb_missing_test_provider")==1,"configured provider refusal fixture");
  Check(session.Compute(segments,3).digest==abc,"retained provider is not silently replaced");
  Check(session.Prepare(pool,binding)==E::provider_failure,"new preparation respects unavailable configured provider");
  Check(EVP_set_default_properties(nullptr,nullptr)==1,"restore provider configuration fixture");
  Check(session.Prepare(pool,binding)==E::none,"configured provider retry");
#endif
  block_update=true;update_entered=false;release_update=false;std::atomic<bool> worker_ok{false};
  std::thread worker([&]{worker_ok=session.Compute(segments,3).digest==abc;OPENSSL_thread_stop();});
  while(!update_entered)std::this_thread::yield();
  Check(!session.Compute(segments,3).ok()&&session.Close()==E::busy,"same context concurrent call refused");
  release_update=true;worker.join();block_update=false;Check(worker_ok,"original in-flight digest retained");
  h::PreparedSha256 other;Check(other.Prepare(pool,binding)==E::none,"second independent context");
  std::atomic<bool> concurrent_ok{true};
  auto run=[&](h::PreparedSha256& context){deny_cpp=true;for(unsigned i=0;i<100;++i)if(context.Compute(segments,3).digest!=abc)concurrent_ok=false;
    deny_cpp=false;OPENSSL_thread_stop();};
  std::thread one(run,std::ref(session)),two(run,std::ref(other));one.join();two.join();Check(concurrent_ok,"independent contexts concurrently correct");
  Check(other.Close()==E::none&&session.Close()==E::none,"context resources closed");
}

void ProviderCapacityChecks(){
  alignas(std::max_align_t) std::array<unsigned char,2048> backing{};
  bool zero_allocation_failure=false,private_state_failure=false,success=false;
  for(unsigned size=64;size<=backing.size();size+=16){
    h::CryptoMemoryPool pool;const auto binding=Binding(70);
    Check(pool.Open(binding,backing.data(),size)==E::none,"bounded capacity fixture");
    h::PreparedSha256 session;const auto prepared=session.Prepare(pool,binding);const auto snapshot=pool.Snapshot();
    if(prepared==E::none)success=true;
    else{Check(prepared==E::provider_failure,"actual allocation failure typed");
      if(!snapshot.allocations)zero_allocation_failure=true;else private_state_failure=true;}
    Check(session.Close()==E::none,"partial context cleanup");
    // OpenSSL's real error queue may retain admitted operation bytes; retain the
    // pool through actual thread cleanup rather than manufacturing zero usage.
    OPENSSL_thread_stop();Check(pool.Close()==E::none,"real error-state cleanup before backing release");
  }
  Check(zero_allocation_failure&&private_state_failure&&success,"actual EVP context/private-state capacity failures and successful admission");
}

void OpenPgpChecks(Grant& grant,h::CryptoMemoryPool& process,h::CryptoMemoryPool& operation){
  namespace pgp=scratchbird::core::crypto;
  using Code=pgp::PgpCode;
  const auto binding=grant.binding;
  const auto half=grant.bytes/2;
  auto* backing=static_cast<unsigned char*>(grant.pointer)+half;
  std::array<unsigned char,32> key{},salt{};
  std::array<unsigned char,129> plaintext{},recovered{};
  plaintext.fill(0x73);key.fill(0x19);salt.fill(0x37);
  const auto zero=[](const auto& bytes){for(auto b:bytes)if(b)return false;return true;};
  // Process-owned provider caches must not pin operation backing. Warm every
  // selected cipher and HKDF in an explicit process scope, not an operation.
  {
    h::CryptoMemoryScope warm(process,binding);Check(warm.ok(),"PGP process cache scope");
    for(unsigned cipher=7;cipher<=9;++cipher)for(unsigned aead:{2u,3u}){
      pgp::SeipdProfile profile{static_cast<unsigned char>(cipher),static_cast<unsigned char>(aead),0};
      std::vector<unsigned char> body(pgp::SeipdEncryptedSize(profile,plaintext.size()).bytes);
      Check(pgp::EncryptSeipdV2(profile,{key.data(),16+(cipher-7)*8},{salt.data(),salt.size()},
        {plaintext.data(),plaintext.size()},{body.data(),body.size()})==Code::ok,"PGP process cache warmup");
      Check(pgp::DecryptSeipdV2({key.data(),16+(cipher-7)*8},{body.data(),body.size()},
        {recovered.data(),recovered.size()})==Code::ok&&recovered==plaintext,"PGP receive cache warmup");
      std::vector<unsigned char> session_body(pgp::IteratedSkeskV6Size(cipher,aead,16+(cipher-7)*8).bytes);
      std::array<unsigned char,32> session_key{};
      Check(pgp::EncryptIteratedSkeskV6(cipher,aead,0,{plaintext.data(),plaintext.size()},{salt.data(),8},
        {salt.data(),aead==2?15u:12u},{key.data(),16+(cipher-7)*8},{session_body.data(),session_body.size()})==Code::ok,"SKESK process cache warmup");
      Check(pgp::DecryptIteratedSkeskV6({plaintext.data(),plaintext.size()},{session_body.data(),session_body.size()},
        {session_key.data(),16+(cipher-7)*8})==Code::ok&&!std::memcmp(session_key.data(),key.data(),16+(cipher-7)*8),"SKESK receive cache warmup");
    }
    OPENSSL_thread_stop();
  }
  for(unsigned cipher=7;cipher<=9;++cipher)for(unsigned aead:{2u,3u})for(bool session:{false,true}){
    const pgp::SeipdProfile profile{static_cast<unsigned char>(cipher),static_cast<unsigned char>(aead),0};
    const std::size_t key_size=16+(cipher-7)*8;
    std::vector<unsigned char> body(session?pgp::IteratedSkeskV6Size(cipher,aead,key_size).bytes:pgp::SeipdEncryptedSize(profile,plaintext.size()).bytes);
    std::vector<unsigned char> decoded(session?key_size:plaintext.size(),0xa5);
    const auto encrypt=[&]{return session?pgp::EncryptIteratedSkeskV6(cipher,aead,0,{plaintext.data(),plaintext.size()},
      {salt.data(),8},{salt.data(),aead==2?15u:12u},{key.data(),key_size},{body.data(),body.size()}):
      pgp::EncryptSeipdV2(profile,{key.data(),key_size},{salt.data(),salt.size()},{plaintext.data(),plaintext.size()},{body.data(),body.size()});};
    const auto decrypt=[&]{return session?pgp::DecryptIteratedSkeskV6({plaintext.data(),plaintext.size()},
      {body.data(),body.size()},{decoded.data(),decoded.size()}):pgp::DecryptSeipdV2({key.data(),key_size},
      {body.data(),body.size()},{decoded.data(),decoded.size()});};
    const auto exact=[&]{return !std::memcmp(decoded.data(),session?key.data():plaintext.data(),decoded.size());};
    {
      h::CryptoMemoryScope active(operation,binding);Check(active.ok(),"PGP admitted binary operation scope");
      const auto before=operation.Snapshot();deny_cpp=true;
      const auto encrypted=encrypt();
      const auto decrypted=decrypt();deny_cpp=false;
      Check(encrypted==Code::ok&&decrypted==Code::ok&&exact(),"PGP executes without C++ heap fallback");
      const auto after=operation.Snapshot();
      Check(after.observation_error==E::none&&after.allocations>before.allocations,"PGP actual provider allocations charged to binary operation");
      body.back()^=1;std::fill(decoded.begin(),decoded.end(),0xa5);deny_cpp=true;
      const auto corrupted=decrypt();deny_cpp=false;body.back()^=1;
      Check(corrupted==Code::authentication_failed&&zero(decoded),"accounted PGP authentication failure erases staging");
      OPENSSL_thread_stop();
    }
    Check(!operation.Snapshot().live_blocks,"PGP actual operation allocation drain after thread cleanup");
    Check(operation.Close()==E::none,"PGP operation backing release only after drain");
    bool refused_before_allocation=false,refused_partial=false,success=false;
    Check(operation.Open(binding,backing,half)==E::none,"PGP sealed grant fixture");
    {
      h::CryptoMemoryScope sealed(operation,binding,true);Check(sealed.ok(),"PGP no-allocation admission");
      std::fill(decoded.begin(),decoded.end(),0xa5);
      Check(decrypt()==Code::provider_failure&&zero(decoded),"PGP refuses sealed provider allocation");
      const auto snapshot=operation.Snapshot();
      refused_before_allocation=snapshot.observation_error==E::none&&!snapshot.allocations&&snapshot.refusals>0;
      OPENSSL_thread_stop();
    }
    Check(operation.Close()==E::none,"PGP sealed operation drains");
    for(std::size_t size=64;size<=32768;size+=128){
      Check(operation.Open(binding,backing,size)==E::none,"PGP bounded actual-grant partition");
      {
        h::CryptoMemoryScope active(operation,binding);Check(active.ok(),"PGP bounded operation scope");
        std::fill(decoded.begin(),decoded.end(),0xa5);deny_cpp=true;
        const auto result=decrypt();deny_cpp=false;
        const auto snapshot=operation.Snapshot();Check(snapshot.observation_error==E::none,"PGP capacity observation authoritative");
        if(result==Code::ok){success=true;Check(exact(),"PGP capacity success exact bytes");}
        else{
          Check(result==Code::provider_failure&&zero(decoded)&&snapshot.refusals>0,"PGP actual capacity refusal clears private output");
          if(snapshot.allocations)refused_partial=true;
        }
        OPENSSL_thread_stop();
      }
      Check(!operation.Snapshot().live_blocks&&operation.Close()==E::none,"PGP partial provider state and errors actually drain");
    }
    if(!(refused_before_allocation&&refused_partial&&success))
      std::cerr<<"PGP capacity cipher="<<cipher<<" aead="<<aead<<" first="<<refused_before_allocation
               <<" partial="<<refused_partial<<" success="<<success<<'\n';
    Check(refused_before_allocation&&refused_partial&&success,"PGP actual allocation-boundary refusal, partial unwind and success");
    Check(operation.Open(binding,backing,half)==E::none,"restore actual operation grant partition");
  }
  Check(grant.resource->Snapshot().allocated_bytes==grant.bytes&&grant.manager.Snapshot().current_bytes==grant.bytes,
    "PGP backing remains fully charged through provider effects and cleanup");
}

void ArgonOpenPgpChecks(Grant& provider,h::CryptoMemoryPool& operation){
  namespace pgp=scratchbird::core::crypto;
  using Code=pgp::PgpCode;
  const auto work_size=pgp::Argon2S2kWorkspaceSize({}).bytes;
  // Real physical backing for workspace, input, output and conservative scratch
  // charge. Provider backing has its own existing actual grant. No claim here
  // that this fixture selects or admits a production statement's CPU policy.
  Grant native(work_size+pgp::Argon2idFixedScratchBytes()+1024);
  auto* base=static_cast<unsigned char*>(native.pointer);
  const pgp::PgpOutput work{base,work_size};
  auto* tail=base+work_size;
  pgp::PgpInput password{tail,8},salt{tail+16,16},nonce{tail+32,15},key{tail+48,32};
  pgp::PgpOutput body{tail+80,pgp::Argon2SkeskV6Size(9,2,32).bytes},decoded{tail+192,32};
  std::memset(tail,0x61,256);
  const auto zero=[](pgp::PgpOutput bytes){return std::all_of(bytes.data,bytes.data+bytes.size,[](auto v){return !v;});};
  const auto charge=[&]{return native.resource->Snapshot().allocated_bytes==native.bytes&&
    native.manager.Snapshot().current_bytes==native.bytes&&native.ledger.Snapshot().current_bytes==native.bytes;};
  {
    h::CryptoMemoryScope active(operation,provider.binding);Check(active.ok(),"Argon provider admission");
    const auto before=operation.Snapshot();deny_cpp=true;
    const auto encrypted=pgp::EncryptArgon2SkeskV6(9,2,{},password,salt,nonce,key,work,body);
    const auto decrypted=pgp::DecryptArgon2SkeskV6(password,{body.data,body.size},work,decoded);deny_cpp=false;
    Check(encrypted==Code::ok&&decrypted==Code::ok&&!std::memcmp(key.data,decoded.data,32)&&zero(work),"approved default Argon packet uses actual backing without C++ fallback");
    Check(operation.Snapshot().allocations>before.allocations&&charge(),"actual Argon and provider memory remain charged");
    OPENSSL_thread_stop();
  }
  Check(!operation.Snapshot().live_blocks,"Argon provider temporaries drain");
  {
    h::CryptoMemoryScope sealed(operation,provider.binding,true);Check(sealed.ok(),"Argon sealed provider scope");
    std::memset(decoded.data,0xa5,decoded.size);deny_cpp=true;
    const auto code=pgp::DecryptArgon2SkeskV6(password,{body.data,body.size},work,decoded);deny_cpp=false;
    Check(code==Code::provider_failure&&zero(decoded)&&zero(work)&&charge(),"Argon provider refusal after real KDF clears outputs without downgrade");
    OPENSSL_thread_stop();
  }
  // Revocation of the actual provider owner is observed during native filling,
  // before any HKDF work. Cleanup capacity remains owned until quiescence.
  struct Revocation {
    h::CryptoMemoryPool* pool;unsigned polls=0;
    static bool Poll(void* raw){auto& self=*static_cast<Revocation*>(raw);
      if(++self.polls==20)Check(self.pool->Revoke()==E::none,"Argon operation revocation");
      return self.pool->Snapshot().revoked;
    }
  } cancellation{&operation};
  {
    h::CryptoMemoryScope active(operation,provider.binding);Check(active.ok(),"Argon revocable operation");
    std::memset(decoded.data,0xa5,decoded.size);deny_cpp=true;
    const auto code=pgp::DecryptArgon2SkeskV6(password,{body.data,body.size},work,decoded,{Revocation::Poll,&cancellation});deny_cpp=false;
    Check(code==Code::cancelled&&cancellation.polls==20&&zero(decoded)&&zero(work)&&charge(),"live revocation cancels filling while retaining cleanup grant");
    OPENSSL_thread_stop();
  }
  Check(operation.Close()==E::none,"revoked Argon provider drains");
  Check(operation.Open(provider.binding,static_cast<unsigned char*>(provider.pointer)+provider.bytes/2,provider.bytes/2)==E::none,"fresh provider owner for subsequent operations");
  // Capacity exhaustion is a real allocator refusal, not a smaller-workspace
  // fallback. The original grant and defaults remain unchanged.
  auto refused=native.resource->Allocate({work_size,8,"second Argon workspace exceeds grant"});
  Check(!refused.ok()&&charge()&&pgp::Argon2S2kWorkspaceSize({}).bytes==work_size,"insufficient physical grant never weakens Argon default");
  OPENSSL_cleanse(tail,native.bytes-work_size);native.Release();
}

int main(int argc,char** argv){
  const char* mode=argc>1?argv[1]:"bounded";
  if(!std::strcmp(mode,"late")||!std::strcmp(mode,"custom")){
    alignas(std::max_align_t) std::array<unsigned char,4096> backing{};h::CryptoMemoryPool pool;
    Check(pool.Open(Binding(10),backing.data(),backing.size())==E::none,"open test pool");
    if(!std::strcmp(mode,"late")){void* old=OPENSSL_malloc(8);Check(old,"preexisting allocation");
      Check(h::InstallCryptoMemoryAdapter(pool)==E::late_installation,"late install refuses unchanged");OPENSSL_free(old);
    }else{Check(CRYPTO_set_mem_functions(CustomMalloc,CustomRealloc,CustomFree)==1,"custom hook fixture");
      Check(h::InstallCryptoMemoryAdapter(pool)==E::custom_allocator,"do not replace custom allocator");
      void* p=OPENSSL_malloc(8);Check(p,"custom allocator still usable");OPENSSL_free(p);}
    Check(pool.Close()==E::none,"uninstalled pool closes");std::cout<<"PASS "<<mode<<" checks="<<checks<<'\n';return 0;
  }
  Grant grant;h::CryptoMemoryPool process,operation;
  Check(h::StopCryptoMemoryAdapter()==E::not_installed,"stop before installation");
  Check(h::InstallCryptoMemoryAdapter(process)==E::closed,"unopened process pool refused");
  {h::CryptoMemoryScope unavailable(process,grant.binding);Check(unavailable.error()==E::not_installed,"scope before adapter installation");}
  const auto half=grant.bytes/2;auto op_binding=grant.binding;
  // These two nonoverlapping regions are one actual charged resource. The
  // operation binding uses a separately admitted grant in production; this
  // fixture keeps the issuer's four identities unchanged for both regions.
  Check(process.Open(grant.binding,grant.pointer,half)==E::none,"actual process backing");
  Check(operation.Open(op_binding,static_cast<unsigned char*>(grant.pointer)+half,half)==E::none,"actual operation backing");
  fail_next_lock=true;Check(h::InstallCryptoMemoryAdapter(process)==E::busy,"installation lock failure is explicit");
  Check(!h::SnapshotCryptoMemoryAdapter().installed,"failed installation has no effects");
  Check(h::InstallCryptoMemoryAdapter(process)==E::none,"early explicit adapter install");
  Check(h::InstallCryptoMemoryAdapter(process)==E::already_installed,"idempotent install does not replace hooks");
  AllocatorChecks(process);DigestChecks(operation,op_binding);ProviderCapacityChecks();OpenPgpChecks(grant,process,operation);
  ArgonOpenPgpChecks(grant,operation);
  Check(grant.resource->Snapshot().allocated_bytes==grant.bytes&&grant.manager.Snapshot().current_bytes==grant.bytes,"provider work preserves actual backing charge");
  Check(h::StopCryptoMemoryAdapter()==E::busy,"provider caches prevent false drain");
  OPENSSL_thread_stop();OPENSSL_cleanup();
  Check(!process.Snapshot().live_blocks&&!operation.Snapshot().live_blocks,"real provider cleanup frees all backing allocations");
  fail_next_lock=true;Check(h::StopCryptoMemoryAdapter()==E::busy,"failed terminal observation retains adapter");
  Check(!h::SnapshotCryptoMemoryAdapter().stopped,"failed stop does not fabricate completion");
  Check(h::StopCryptoMemoryAdapter()==E::none,"terminal drain after cleanup");
  const auto stopped=h::SnapshotCryptoMemoryAdapter();Check(stopped.observation_error==E::none&&stopped.installed&&stopped.stopped&&!stopped.live_blocks&&!stopped.active_scopes&&
    stopped.process_binding.operation==grant.binding.operation,"fixed binary shutdown observation");
  Check(!OPENSSL_malloc(1),"late allocation after terminal stop refused");
  Check(operation.Close()==E::none&&process.Close()==E::none,"both backing pools close after drain");grant.Release();
  std::cout<<"PASS bounded crypto adapter checks="<<checks<<" actual_grant=true\n";
}
