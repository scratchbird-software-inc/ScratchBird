// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "crypto_memory_adapter.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>

namespace scratchbird::core::hash {
namespace {
struct Adapter {
  std::mutex mutex;
  CryptoMemoryPool* process=nullptr;
  CryptoMemoryPool* pools=nullptr;
  CryptoMemoryBinding process_binding{};
  std::size_t blocks=0, scopes=0;
  bool installed=false, stopped=false;
};
Adapter& State(){static Adapter state;return state;}
thread_local CryptoMemoryScope* scope=nullptr;
thread_local PreparedSha256* prepared=nullptr;
constexpr auto alignment=alignof(std::max_align_t);
void Increment(u64& value){if(value!=std::numeric_limits<u64>::max())++value;}
bool Same(const CryptoMemoryBinding& a,const CryptoMemoryBinding& b){
  return a==b;
}
void Wipe(void* p,std::size_t n){volatile auto* bytes=static_cast<volatile unsigned char*>(p);while(n--)*bytes++=0;}
struct BusyRelease{std::atomic_flag& flag;~BusyRelease(){flag.clear(std::memory_order_release);}};
}
struct alignas(std::max_align_t) CryptoMemoryPool::Block {
  CryptoMemoryPool* pool;
  Block* previous;
  Block* next;
  std::size_t capacity,requested;
};
struct CryptoMemoryInternals {
  using Block=CryptoMemoryPool::Block;
  static void* Allocate(CryptoMemoryPool& p,std::size_t bytes){
    auto& s=p.state_;
    if(!s.open||s.revoked||!bytes||bytes>std::numeric_limits<std::size_t>::max()-(alignment-1)){
      Increment(s.refusals);return nullptr;
    }
    const auto rounded=(bytes+alignment-1)/alignment*alignment;
    for(auto* b=p.first_;b;b=b->next){
      if(b->requested||b->capacity<rounded)continue;
      if(b->capacity-rounded>=sizeof(Block)+alignment){
        auto* next=new(static_cast<unsigned char*>(static_cast<void*>(b+1))+rounded)
          Block{&p,b,b->next,b->capacity-rounded-sizeof(Block),0};
        if(next->next)next->next->previous=next;
        b->next=next;b->capacity=rounded;
      }
      b->requested=bytes;s.live_bytes+=bytes;s.occupied_bytes+=sizeof(Block)+b->capacity;
      ++s.live_blocks;++State().blocks;Increment(s.allocations);return b+1;
    }
    Increment(s.refusals);return nullptr;
  }
  static void Merge(Block* b){
    if(b->next&&!b->next->requested){auto* next=b->next;
      b->capacity+=sizeof(Block)+next->capacity;b->next=next->next;
      if(b->next)b->next->previous=b;
      Wipe(next,sizeof(Block));
    }
  }
  static void Release(void* pointer){
    auto* b=static_cast<Block*>(pointer)-1;auto& s=b->pool->state_;
    s.live_bytes-=b->requested;s.occupied_bytes-=sizeof(Block)+b->capacity;
    --s.live_blocks;--State().blocks;Wipe(b+1,b->capacity);b->requested=0;
    Merge(b);if(b->previous&&!b->previous->requested)Merge(b->previous);
  }
  static CryptoMemoryPool* Selected(){
    if(!State().installed||State().stopped)return nullptr;
    // An inner scope cannot lift an enclosing guard's allocation prohibition.
    for(auto* current=scope;current;current=current->previous_)
      if(!current->ok()||current->reject_||!current->pool_->state_.open||current->pool_->state_.revoked){
        if(scope->pool_)Increment(scope->pool_->state_.refusals);return nullptr;
      }
    auto* selected=scope?scope->pool_:State().process;
    if(selected&&(!selected->state_.open||selected->state_.revoked)){Increment(selected->state_.refusals);return nullptr;}
    return selected;
  }
  static void* Malloc(std::size_t n,const char*,int) noexcept {
    try{std::lock_guard lock(State().mutex);auto* p=Selected();return p?Allocate(*p,n):nullptr;}
    catch(...){return nullptr;}
  }
  static void Free(void* p,const char*,int) noexcept {
    if(!p)return;
    // Valid pointers are owned until their final free, including during drain.
    // A mutex failure must not escape into the C provider ABI.
    try{std::lock_guard lock(State().mutex);Release(p);}catch(...){std::terminate();}
  }
  static void* Realloc(void* p,std::size_t bytes,const char*,int) noexcept {
    try{
      std::lock_guard lock(State().mutex);
      if(!p){auto* selected=Selected();return selected?Allocate(*selected,bytes):nullptr;}
      if(!bytes){Release(p);return nullptr;}
      if(!Selected())return nullptr;
      auto* b=static_cast<Block*>(p)-1;auto& owner=*b->pool;auto& s=owner.state_;
      if(!s.open||s.revoked){Increment(s.refusals);return nullptr;}
      if(bytes<=b->capacity){
        if(bytes<b->requested)Wipe(static_cast<unsigned char*>(p)+bytes,b->requested-bytes);
        s.live_bytes=s.live_bytes-b->requested+bytes;b->requested=bytes;return p;
      }
      // Replacement is charged to the original pool, never the current scope.
      // Allocate first: overflow/exhaustion leaves the original bytes intact.
      auto* next=Allocate(owner,bytes);if(!next)return nullptr;
      std::memcpy(next,p,b->requested);Release(p);return next;
    }catch(...){return nullptr;}
  }
};
CryptoMemoryError CryptoMemoryPool::Open(const CryptoMemoryBinding& binding,void* backing,std::size_t bytes) noexcept {
  try{
    std::lock_guard lock(State().mutex);
    if(state_.open)return CryptoMemoryError::busy;
    if(!ValidCryptoMemoryBinding(binding))return CryptoMemoryError::invalid_binding;
    const auto begin=reinterpret_cast<std::uintptr_t>(backing),self=reinterpret_cast<std::uintptr_t>(this);
    if(!backing||begin%alignment||bytes<sizeof(Block)+alignment||bytes>UINTPTR_MAX-begin||
        (begin<self+sizeof(*this)&&self<begin+bytes))return CryptoMemoryError::invalid_backing;
    for(auto* other=State().pools;other;other=other->next_){
      const auto ob=reinterpret_cast<std::uintptr_t>(other->first_),oc=reinterpret_cast<std::uintptr_t>(other);
      if((begin<ob+other->state_.backing_bytes&&ob<begin+bytes)||
         (begin<oc+sizeof(*other)&&oc<begin+bytes)||
         (self<ob+other->state_.backing_bytes&&ob<self+sizeof(*this)))return CryptoMemoryError::invalid_backing;
    }
    state_={};state_.binding=binding;state_.backing_bytes=bytes;state_.open=true;
    first_=new(backing) Block{this,nullptr,nullptr,(bytes-sizeof(Block))/alignment*alignment,0};
    next_=State().pools;State().pools=this;
    return CryptoMemoryError::none;
  }catch(...){return CryptoMemoryError::busy;}
}
CryptoMemoryError CryptoMemoryPool::Close() noexcept {
  try{
    std::lock_guard lock(State().mutex);
    if(State().process==this||state_.live_blocks||state_.scopes)return CryptoMemoryError::busy;
    auto** entry=&State().pools;while(*entry&&*entry!=this)entry=&(*entry)->next_;
    if(*entry)*entry=next_;next_=nullptr;
    if(first_)Wipe(first_,state_.backing_bytes);
    first_=nullptr;state_.open=false;return CryptoMemoryError::none;
  }catch(...){return CryptoMemoryError::busy;}
}
CryptoMemoryError CryptoMemoryPool::Revoke() noexcept {
  try{std::lock_guard lock(State().mutex);state_.revoked=true;return CryptoMemoryError::none;}
  catch(...){return CryptoMemoryError::busy;}
}
CryptoMemorySnapshot CryptoMemoryPool::Snapshot() const noexcept {
  try{std::lock_guard lock(State().mutex);auto out=state_;out.observation_error=CryptoMemoryError::none;return out;}
  catch(...){return {};}
}
CryptoMemoryError InstallCryptoMemoryAdapter(CryptoMemoryPool& pool) noexcept {
  try{
    std::lock_guard lock(State().mutex);auto& s=State();
    if(s.installed)return CryptoMemoryError::already_installed;
    const auto snapshot=pool.state_;
    if(!snapshot.open)return CryptoMemoryError::closed;
    if(snapshot.revoked)return CryptoMemoryError::revoked;
    CRYPTO_malloc_fn m;CRYPTO_realloc_fn r;CRYPTO_free_fn f;
    CRYPTO_get_mem_functions(&m,&r,&f);
    if(m!=CRYPTO_malloc||r!=CRYPTO_realloc||f!=CRYPTO_free)return CryptoMemoryError::custom_allocator;
    if(CRYPTO_set_mem_functions(CryptoMemoryInternals::Malloc,CryptoMemoryInternals::Realloc,CryptoMemoryInternals::Free)!=1)
      return CryptoMemoryError::late_installation;
    s.process=&pool;s.process_binding=snapshot.binding;s.installed=true;return CryptoMemoryError::none;
  }catch(...){return CryptoMemoryError::busy;}
}
CryptoMemoryError StopCryptoMemoryAdapter() noexcept {
  try{
    std::lock_guard lock(State().mutex);auto& s=State();
    if(!s.installed)return CryptoMemoryError::not_installed;
    if(s.blocks||s.scopes)return CryptoMemoryError::busy;
    s.process=nullptr;s.stopped=true;return CryptoMemoryError::none;
  }catch(...){return CryptoMemoryError::busy;}
}
CryptoMemoryError CheckCryptoMemoryAdapter(const CryptoMemoryPool& owner,
                                           const CryptoMemoryBinding& expected) noexcept {
  try {
    std::lock_guard lock(State().mutex);
    const auto& state=State();
    if(!ValidCryptoMemoryBinding(expected))return CryptoMemoryError::invalid_binding;
    if(!state.installed)return CryptoMemoryError::not_installed;
    if(!Same(state.process_binding,expected))return CryptoMemoryError::invalid_binding;
    if(state.stopped||!state.process)return CryptoMemoryError::closed;
    if(state.process!=&owner)return CryptoMemoryError::invalid_binding;
    const auto& pool=state.process->state_;
    if(!pool.open)return CryptoMemoryError::closed;
    if(pool.revoked)return CryptoMemoryError::revoked;
    return CryptoMemoryError::none;
  } catch(...) {return CryptoMemoryError::busy;}
}
CryptoMemoryAdapterSnapshot SnapshotCryptoMemoryAdapter() noexcept {
  try{std::lock_guard lock(State().mutex);const auto& s=State();
    return {s.process_binding,s.blocks,s.scopes,s.installed,s.stopped,CryptoMemoryError::none};
  }catch(...){return {};}
}
CryptoMemoryScope::CryptoMemoryScope(CryptoMemoryPool& p,const CryptoMemoryBinding& b,bool reject) noexcept {
  previous_=scope;scope=this;
  try{
    std::lock_guard lock(State().mutex);
    if(!State().installed||State().stopped)return;
    ++State().scopes;counted_=true;
    if(!Same(p.state_.binding,b)){error_=CryptoMemoryError::invalid_binding;return;}
    if(!p.state_.open){error_=CryptoMemoryError::closed;return;}
    if(p.state_.revoked){error_=CryptoMemoryError::revoked;return;}
    pool_=&p;++p.state_.scopes;reject_=reject;error_=CryptoMemoryError::none;
  }catch(...){error_=CryptoMemoryError::busy;}
}
CryptoMemoryScope::CryptoMemoryScope() noexcept {
  previous_=scope;scope=this;
  try{
    std::lock_guard lock(State().mutex);auto* p=State().process;
    if(!p||State().stopped)return;
    ++State().scopes;counted_=true;
    if(!p->state_.open){error_=CryptoMemoryError::closed;return;}
    if(p->state_.revoked){error_=CryptoMemoryError::revoked;return;}
    pool_=p;++p->state_.scopes;reject_=false;error_=CryptoMemoryError::none;
  }catch(...){error_=CryptoMemoryError::busy;}
}
CryptoMemoryScope::~CryptoMemoryScope(){
  // Scopes are lexical, nonmovable and thread-confined.
  if(scope!=this)std::terminate();
  std::lock_guard lock(State().mutex);
  if(counted_)--State().scopes;if(pool_)--pool_->state_.scopes;scope=previous_;
}
PreparedSha256::~PreparedSha256(){if(Close()==CryptoMemoryError::busy)std::terminate();}
void PreparedSha256::Clear() noexcept {
  EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(context_));context_=nullptr;
  pool_=nullptr;ready_=false;
}
CryptoMemoryError PreparedSha256::Close() noexcept {
  if(busy_.test_and_set(std::memory_order_acquire))return CryptoMemoryError::busy;
  BusyRelease release{busy_};Clear();return CryptoMemoryError::none;
}
CryptoMemoryError PreparedSha256::Prepare(CryptoMemoryPool& pool,const CryptoMemoryBinding& binding) noexcept {
  if(busy_.test_and_set(std::memory_order_acquire))return CryptoMemoryError::busy;
  BusyRelease release{busy_};Clear();
  CryptoMemoryScope operation(pool,binding);if(!operation.ok())return operation.error();
  // Warm process/provider caches in process backing using exactly the same
  // EVP selection route as the existing hash API, including configured legacy
  // engines. Do not replace that route with an explicit provider selection.
  {
    CryptoMemoryScope bootstrap;if(!bootstrap.ok())return bootstrap.error();
    auto* warm=EVP_MD_CTX_new();
    const bool warmed=warm&&EVP_DigestInit_ex(warm,EVP_sha256(),nullptr)==1;
    EVP_MD_CTX_free(warm);if(!warmed)return CryptoMemoryError::provider_failure;
  }
  context_=EVP_MD_CTX_new();
  if(!context_||EVP_DigestInit_ex(static_cast<EVP_MD_CTX*>(context_),EVP_sha256(),nullptr)!=1){
    Clear();return CryptoMemoryError::provider_failure;
  }
  pool_=&pool;binding_=binding;ready_=true;return CryptoMemoryError::none;
}
Sha256PartsResult PreparedSha256::Compute(const HashDigestSegment* parts,std::size_t count) noexcept {
  if(busy_.test_and_set(std::memory_order_acquire))return {Sha256PartsError::provider_failure,{}};
  BusyRelease release{busy_};
  if(count&&!parts)return {Sha256PartsError::segments_missing,{}};
  u64 bytes=0;for(std::size_t i=0;i<count;++i){
    if((parts[i].size&&!parts[i].data)||parts[i].size>std::numeric_limits<u64>::max()/8-bytes)
      return {Sha256PartsError::segment_extent_invalid,{}};
    bytes+=parts[i].size;
  }
  if(!ready_||!pool_)return {Sha256PartsError::provider_failure,{}};
  CryptoMemoryScope guarded(*pool_,binding_);if(!guarded.ok())return {Sha256PartsError::provider_failure,{}};
  ready_=false;auto* ctx=static_cast<EVP_MD_CTX*>(context_);
  if(EVP_DigestInit_ex(ctx,nullptr,nullptr)!=1)return {Sha256PartsError::provider_failure,{}};
  for(std::size_t i=0;i<count;++i)if(parts[i].size&&EVP_DigestUpdate(ctx,parts[i].data,parts[i].size)!=1)
    return {Sha256PartsError::provider_failure,{}};
  Sha256PartsResult result;unsigned int length=0;
  if(EVP_DigestFinal_ex(ctx,result.digest.data(),&length)!=1||length!=result.digest.size())
    return {Sha256PartsError::provider_failure,{}};
  ready_=true;result.error=Sha256PartsError::none;return result;
}
PreparedSha256Scope::PreparedSha256Scope(PreparedSha256& session) noexcept:previous_(prepared){prepared=&session;}
PreparedSha256Scope::~PreparedSha256Scope(){prepared=previous_;}
PreparedSha256* CurrentPreparedSha256() noexcept{return prepared;}
} // namespace scratchbird::core::hash
