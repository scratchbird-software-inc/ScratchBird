// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_hash_key_record.hpp"
#include "crypto_memory_adapter.hpp"
#include "memory.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <thread>
#include <type_traits>
#include <vector>
#include <unistd.h>

namespace c = scratchbird::core::crypto;
namespace h = scratchbird::core::hash;
namespace m = scratchbird::core::memory;
namespace d = scratchbird::storage::disk;
namespace s = scratchbird::storage::database;
using E = c::NativeHashKeyEnvelopeErrorV1;
using R = s::NativeHashKeyRecordError;
using Uuid = scratchbird::core::platform::Uuid;
using Record = c::NativeHashKeyEnvelopeV1;
unsigned checks = 0;
thread_local bool deny_cpp = false;
unsigned fault_kind = 0, fault_at = 0, fault_calls = 0;
bool fixed_entropy = false;
bool independent_derived_key_matches = false;
enum class IoFault { none, full_write, partial_write, sync, readback, read, extent };
IoFault io_fault = IoFault::none;
constexpr off_t slot = 1024;
std::atomic<unsigned> slot_reads{0};
struct ProtectedWatch { void* pointer=nullptr; std::size_t bytes=0; };
std::array<ProtectedWatch,4> protected_watch{};
unsigned protected_issued=0,protected_erased=0;
bool protected_erasure_valid=true;
bool ProviderFault(unsigned kind) { return fault_kind == kind && ++fault_calls == fault_at; }
void* operator new(std::size_t n) {
    if (deny_cpp) throw std::bad_alloc();
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
extern "C" {
int __real_RAND_bytes(unsigned char*, int);
int __wrap_RAND_bytes(unsigned char* out, int n) {
    if (ProviderFault(8)) { if (n) out[0] = 0xcc; return 0; }
    if (fixed_entropy && n == 44) { for (int i = 0; i < n; ++i) out[i] = 0xa0 + i; return 1; }
    return __real_RAND_bytes(out, n);
}
EVP_PKEY_CTX* __real_EVP_PKEY_CTX_new_id(int, ENGINE*);
EVP_PKEY_CTX* __wrap_EVP_PKEY_CTX_new_id(int n, ENGINE* e) { return ProviderFault(1) ? nullptr : __real_EVP_PKEY_CTX_new_id(n,e); }
int __real_EVP_PKEY_derive(EVP_PKEY_CTX*, unsigned char*, std::size_t*);
int __wrap_EVP_PKEY_derive(EVP_PKEY_CTX* p, unsigned char* out, std::size_t* n) {
    if (ProviderFault(2)) { if (*n) out[0] = 0xcc; return 0; }
    const auto result=__real_EVP_PKEY_derive(p,out,n);
    if(fixed_entropy && result>0 && *n==32) {
        constexpr unsigned char expected[]={0x58,0x36,0x2c,0xed,0x30,0xe3,0xa7,0x82,0x53,0xc6,0xf4,0xfa,0x3e,0x10,0x42,0x1d,
            0x4b,0xe6,0x8f,0xb0,0x1c,0xce,0x3a,0xab,0x24,0x25,0xff,0x46,0x1a,0xd9,0xcf,0xbf};
        independent_derived_key_matches=std::equal(out,out+32,std::begin(expected));
    }
    return result;
}
EVP_CIPHER_CTX* __real_EVP_CIPHER_CTX_new();
EVP_CIPHER_CTX* __wrap_EVP_CIPHER_CTX_new() { return ProviderFault(3) ? nullptr : __real_EVP_CIPHER_CTX_new(); }
int __real_EVP_CipherInit_ex(EVP_CIPHER_CTX*,const EVP_CIPHER*,ENGINE*,const unsigned char*,const unsigned char*,int);
int __wrap_EVP_CipherInit_ex(EVP_CIPHER_CTX* p,const EVP_CIPHER* c,ENGINE* e,const unsigned char* k,const unsigned char* iv,int n) {
    return ProviderFault(4) ? 0 : __real_EVP_CipherInit_ex(p,c,e,k,iv,n);
}
int __real_EVP_CipherUpdate(EVP_CIPHER_CTX*,unsigned char*,int*,const unsigned char*,int);
int __wrap_EVP_CipherUpdate(EVP_CIPHER_CTX* p,unsigned char* out,int* written,const unsigned char* in,int n) {
    if (ProviderFault(5)) { if (out && n) out[0] = 0xcc; return 0; } return __real_EVP_CipherUpdate(p,out,written,in,n);
}
int __real_EVP_CipherFinal_ex(EVP_CIPHER_CTX*,unsigned char*,int*);
int __wrap_EVP_CipherFinal_ex(EVP_CIPHER_CTX* p,unsigned char* out,int* n) {
    if (ProviderFault(6)) { out[0] = 0xcc; return 0; } return __real_EVP_CipherFinal_ex(p,out,n);
}
int __real_EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX*,int,int,void*);
int __wrap_EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX* p,int op,int n,void* v) {
    return ProviderFault(7) ? 0 : __real_EVP_CIPHER_CTX_ctrl(p,op,n,v);
}
ssize_t __real_pwrite(int,const void*,size_t,off_t);
ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t at) {
    if (io_fault == IoFault::full_write && at == slot) { errno=EIO; return -1; }
    if (io_fault == IoFault::partial_write) {
        if (at == slot) return __real_pwrite(fd,p,96,at);
        if (at == slot+96) { errno=EIO; return -1; }
    }
    return __real_pwrite(fd,p,n,at);
}
ssize_t __real_pread(int,void*,size_t,off_t);
ssize_t __wrap_pread(int fd,void* p,size_t n,off_t at) {
    if (at == slot && io_fault == IoFault::read) { errno=EIO; return -1; }
    const auto r=__real_pread(fd,p,n,at);
    if (at == slot && r == 288) {
        const auto seen=++slot_reads;
        if (io_fault == IoFault::readback && seen == 2) static_cast<unsigned char*>(p)[240]^=1;
        if (io_fault == IoFault::extent && seen == 1 && ::ftruncate(fd,8193) != 0) return -1;
    }
    return r;
}
int __real_fsync(int);
int __wrap_fsync(int fd) { if (io_fault == IoFault::sync) { errno=EIO; return -1; } return __real_fsync(fd); }
int __real_munlock(const void*,size_t);
int __wrap_munlock(const void* p,size_t n) {
    for(auto& watch:protected_watch) if(watch.pointer==p) {
        protected_erasure_valid &= n==watch.bytes && std::all_of(
            static_cast<const unsigned char*>(p),static_cast<const unsigned char*>(p)+n,[](auto b){return b==0;});
        watch.pointer=nullptr; ++protected_erased;
    }
    return __real_munlock(p,n);
}
}
void Check(bool ok,const char* label) {
    ++checks;
    if (!ok) { deny_cpp=false; std::cerr<<"FAIL "<<label<<'\n'; std::exit(1); }
}
Uuid Id(unsigned n) { Uuid id{}; id.bytes[0]=1; id.bytes[6]=0x70; id.bytes[8]=0x80; id.bytes[15]=n; return id; }
c::NativeHashKeyBindingV1 Binding() {
    c::NativeHashKeyBindingV1 b;
    b.node=Id(1); b.index=Id(2); b.key=Id(3); b.hash_profile=Id(4);
    b.seed_policy=Id(5); b.wrapping_root=Id(6); b.operation=Id(7);
    b.key_generation=5; b.hash_profile_generation=7; b.seed_policy_generation=11; b.wrapping_root_generation=13;
    return b;
}
template<class T> c::NativeHashKeyInputV1 In(const T& v) { return {v.data(),v.size()}; }
template<class T> c::NativeHashKeyOutputV1 Out(T& v) { return {v.data(),v.size()}; }
bool Filled(c::NativeHashKeyInputV1 bytes,unsigned char v) { return std::all_of(bytes.data,bytes.data+bytes.size,[v](auto b){return b==v;}); }
Record Oracle() {
    // Independent Python cryptography HKDF(SHA256)/AESGCM fixture, not generated
    // by the implementation under test. Fixed entropy exists ONLY in this
    // test's linker wrapper; production always calls the real Core RNG.
    constexpr char hex[] = "5342484b455930310100f000010000000100000000007000800000000000000101000000000070008000000000000002010000000000700080000000000000030500000000000000010000000000700080000000000000040700000000000000010000000000700080000000000000050b00000000000000010000000000700080000000000000060d00000000000000010000000000700080000000000000072000000020000000a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacb0000000000000000000000000000000000000000000000000000000013fd6376ad06bc1d06f28b0403d07e70543f901ae90d45e700547af18723c8432f024cc075638a849b5ddefb8259b370";
    Record r{}; auto nib=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for (std::size_t i=0;i<r.size();++i) r[i]=nib(hex[2*i])*16+nib(hex[2*i+1]);
    return r;
}
struct Secrets {
    m::MemoryManager manager;
    m::ScopedProtectedBuffer provider,root,key,plain;
    h::CryptoMemoryPool pool;
    h::CryptoMemoryBinding binding{Id(1),Id(20),Id(21),Id(22)};
    static auto Policy() { auto p=m::DefaultLocalEngineMemoryPolicy(); p.hard_limit_bytes=8*1024*1024; p.per_context_limit_bytes=p.hard_limit_bytes; return p; }
    Secrets():manager(Policy()) {
        const auto page=static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        for(auto* owner:{&provider,&root,&key,&plain}) {
            m::ProtectedMemoryRequest r; r.bytes=owner==&provider?4*1024*1024:page; r.alignment=page;
            r.platform_policy=m::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump;
            r.tag.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database.bytes;
            r.tag.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner.bytes;
            r.tag.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context.bytes;
            auto got=manager.AllocateProtected(std::move(r)); Check(got.ok(),"actual charged locked/no-dump backing");
            *owner=std::move(got.buffer);
            protected_watch[protected_issued++]={owner->data(),owner->size()};
            Check(!got.buffer.valid() && owner->evidence().platform_lock_succeeded && owner->evidence().no_dump_succeeded,
                  "genuine protected allocation survives ownership transfer");
            Check(owner->tag().binary_ownership[m::MemoryBinaryScopeKind::database]==binding.database.bytes,
                  "immutable actual allocation binary ownership");
        }
        for(unsigned i=0;i<32;++i) { Root()[i]=i; Key()[i]=static_cast<unsigned char>(255-7*i); }
        Check(root.data()!=key.data() && root.data()!=plain.data() && key.data()!=plain.data(),"independent protected secret buffers");
        for(unsigned i=0;i<32;++i) Check(Root()[i]==i && Key()[i]==static_cast<unsigned char>(255-7*i),"exact independent plaintext fixture");
        Check(pool.Open(binding,provider.data(),provider.size())==h::CryptoMemoryError::none,"actual protected provider pool");
        Check(h::InstallCryptoMemoryAdapter(pool)==h::CryptoMemoryError::none,"early actual bounded crypto adapter");
    }
    unsigned char* Root() { return static_cast<unsigned char*>(root.data()); }
    unsigned char* Key() { return static_cast<unsigned char*>(key.data()); }
    unsigned char* Plain() { return static_cast<unsigned char*>(plain.data()); }
    c::NativeHashKeyInputV1 RootIn() { return {Root(),32}; }
    c::NativeHashKeyInputV1 KeyIn() { return {Key(),32}; }
    c::NativeHashKeyOutputV1 PlainOut() { return {Plain(),32}; }
    void Dirty() { std::memset(Plain(),0xa5,plain.size()); }
    void Drain() {
        OPENSSL_thread_stop(); OPENSSL_cleanup();
        Check(pool.Snapshot().live_blocks==0,"actual provider allocations drained before backing");
        Check(h::StopCryptoMemoryAdapter()==h::CryptoMemoryError::none,"terminal adapter drain");
        Check(pool.Close()==h::CryptoMemoryError::none,"provider owner close");
        for(auto* owner:{&plain,&key,&root,&provider}) Check(owner->Reset().ok(),"protected backing cleanup");
        Check(protected_issued==4 && protected_erased==4 && protected_erasure_valid,
              "all actual secret/provider backing erased before OS unlock");
        Check(manager.Snapshot().current_bytes==0,"zero actual physical memory charge after drain");
    }
};
void MissingAdapterChecks() {
    const auto b=Binding(); const auto oracle=Oracle();
    std::array<unsigned char,32> fixture{},out{}; Record wire{};
    wire.fill(0xa5);out.fill(0xa5);
    Check(h::CheckExplicitCryptoMemoryScope(b.node)==h::CryptoMemoryError::not_installed,
          "missing real adapter cannot supply explicit scope");
    Check(c::SealNativeHashKeyEnvelopeV1(b,In(fixture),In(fixture),Out(wire))==E::memory_scope_unavailable &&
          Filled(In(wire),0),"no unaccounted OpenSSL allocation or entropy before adapter admission");
    Check(c::OpenNativeHashKeyEnvelopeV1(b,In(fixture),In(oracle),Out(out))==E::memory_scope_unavailable &&
          Filled(In(out),0),"no default-heap decrypt path before adapter admission");
}
void ExplicitMemoryChecks(Secrets& sec) {
    const auto b=Binding(); const auto oracle=Oracle(); Record wire{};
    const auto before=sec.pool.Snapshot().allocations;
    auto refused=[&] {
        sec.Dirty();wire.fill(0xa5);
        Check(c::SealNativeHashKeyEnvelopeV1(b,sec.RootIn(),sec.KeyIn(),Out(wire))==E::memory_scope_unavailable &&
              Filled(In(wire),0),"unavailable explicit scope refuses sealing and scrubs output");
        Check(c::OpenNativeHashKeyEnvelopeV1(b,sec.RootIn(),In(oracle),sec.PlainOut())==E::memory_scope_unavailable &&
              Filled({sec.Plain(),32},0),"unavailable explicit scope refuses opening and scrubs output");
    };
    Check(h::CheckExplicitCryptoMemoryScope(b.node)==h::CryptoMemoryError::no_explicit_scope,
          "installed process pool is not implicit operation admission");
    refused();
    {
        auto wrong=sec.binding;wrong.owner=Id(99);
        h::CryptoMemoryScope invalid(sec.pool,wrong);
        Check(!invalid.ok(),"actual invalid scope binding");refused();
    }
    {
        h::CryptoMemoryScope outer(sec.pool,sec.binding,true);
        h::CryptoMemoryScope inner(sec.pool,sec.binding);
        Check(outer.ok() && inner.ok() &&
              h::CheckExplicitCryptoMemoryScope(b.node)==h::CryptoMemoryError::allocations_refused,
              "inner valid scope cannot lift outer allocation prohibition");
        refused();
    }
    {
        h::CryptoMemoryScope admitted(sec.pool,sec.binding);
        Check(admitted.ok(),"actual node-bound scope");
        auto foreign=b;foreign.node=Id(88);auto foreign_wire=oracle;
        std::copy(foreign.node.bytes.begin(),foreign.node.bytes.end(),foreign_wire.begin()+16);
        sec.Dirty();wire.fill(0xa5);
        Check(h::CheckExplicitCryptoMemoryScope(foreign.node)==h::CryptoMemoryError::invalid_binding &&
              c::SealNativeHashKeyEnvelopeV1(foreign,sec.RootIn(),sec.KeyIn(),Out(wire))==E::memory_scope_unavailable &&
              Filled(In(wire),0),"foreign-node crypto scope cannot seal");
        Check(c::OpenNativeHashKeyEnvelopeV1(foreign,sec.RootIn(),In(foreign_wire),sec.PlainOut())==E::memory_scope_unavailable &&
              Filled({sec.Plain(),32},0),"foreign-node crypto scope cannot decrypt");
        std::thread unadmitted([&] {
            Check(h::CheckExplicitCryptoMemoryScope(b.node)==h::CryptoMemoryError::no_explicit_scope,
                  "scope is lexical and thread-confined");
            refused();
        });
        unadmitted.join();
    }
    Check(sec.pool.Snapshot().allocations==before,"all unavailable scopes refuse before backend allocation");
}
void CryptoChecks(Secrets& sec) {
    const auto b=Binding();
    const auto oracle=Oracle();
    h::CryptoMemoryScope scope(sec.pool,sec.binding); Check(scope.ok(),"admitted provider scope");
    Record actual{};
    fixed_entropy=true; deny_cpp=true;
    auto code=c::SealNativeHashKeyEnvelopeV1(b,sec.RootIn(),sec.KeyIn(),Out(actual));
    deny_cpp=false; fixed_entropy=false;
    if(code!=E::none || actual!=oracle) {
        const auto mismatch=std::mismatch(actual.begin(),actual.end(),oracle.begin());
        const auto snapshot=sec.pool.Snapshot();
        std::cerr<<"envelope_code="<<static_cast<unsigned>(code)
                 <<" first_differing_offset="<<(mismatch.first-actual.begin())
                 <<" backend_refusals="<<snapshot.refusals
                 <<" derived_key_matches="<<independent_derived_key_matches<<'\n';
    }
    Check(code==E::none && actual==oracle,"exact independent AES256GCM/HKDF binary envelope");
    Check(independent_derived_key_matches,"independent exact domain/header HKDF key vector");
    sec.Dirty(); deny_cpp=true;
    code=c::OpenNativeHashKeyEnvelopeV1(b,sec.RootIn(),In(oracle),sec.PlainOut()); deny_cpp=false;
    Check(code==E::none && !std::memcmp(sec.Plain(),sec.Key(),32),"independent ciphertext full authenticated private open");
    for(std::size_t i=0;i<oracle.size();++i) {
        auto bad=oracle; bad[i]^=1; sec.Dirty();
        deny_cpp=true; code=c::OpenNativeHashKeyEnvelopeV1(b,sec.RootIn(),In(bad),sec.PlainOut()); deny_cpp=false;
        Check(code!=E::none && Filled({sec.Plain(),32},0),"every envelope byte protected / no plaintext prefix");
    }
    for(unsigned bit=0;bit<256;++bit) {
        sec.Root()[bit/8]^=1U<<(bit%8); sec.Dirty();
        code=c::OpenNativeHashKeyEnvelopeV1(b,sec.RootIn(),In(oracle),sec.PlainOut());
        Check(code==E::authentication_failed && Filled({sec.Plain(),32},0),"all wrapping-root bits authenticated");
        sec.Root()[bit/8]^=1U<<(bit%8);
    }
    for(unsigned generation=0;generation<4;++generation) {
        auto foreign=b;
        auto* at=generation==0?&foreign.key_generation:generation==1?&foreign.hash_profile_generation:
                 generation==2?&foreign.seed_policy_generation:&foreign.wrapping_root_generation;
        ++*at; sec.Dirty();
        Check(c::OpenNativeHashKeyEnvelopeV1(foreign,sec.RootIn(),In(oracle),sec.PlainOut())==E::binding_mismatch &&
              Filled({sec.Plain(),32},0),"foreign generation cannot open");
        *at=0; actual.fill(0xa5);
        Check(c::SealNativeHashKeyEnvelopeV1(foreign,sec.RootIn(),sec.KeyIn(),Out(actual))==E::invalid_binding &&
              Filled(In(actual),0xa5),"unissued generation leaves seal output unchanged");
    }
    for(unsigned which=0;which<7;++which) {
        auto foreign=b;
        Uuid* fields[]={&foreign.node,&foreign.index,&foreign.key,&foreign.hash_profile,&foreign.seed_policy,&foreign.wrapping_root,&foreign.operation};
        for(unsigned byte=0;byte<16;++byte) {
            auto saved=*fields[which]; fields[which]->bytes[byte]^=1; sec.Dirty();
            Check(c::OpenNativeHashKeyEnvelopeV1(foreign,sec.RootIn(),In(oracle),sec.PlainOut())!=E::none,
                  "all binary identity bytes bound"); *fields[which]=saved;
        }
    }
    for(std::size_t n=0;n<=289;++n) if(n!=288) {
        actual.fill(0xa5);
        Check(c::SealNativeHashKeyEnvelopeV1(b,sec.RootIn(),sec.KeyIn(),{actual.data(),n})==E::invalid_extent &&
              Filled(In(actual),0xa5),"every wrong ciphertext extent refuses before output mutation");
        sec.Dirty();
        Check(c::OpenNativeHashKeyEnvelopeV1(b,sec.RootIn(),{oracle.data(),n},sec.PlainOut())==E::invalid_extent &&
              Filled({sec.Plain(),32},0xa5),"every truncated/extended ciphertext refuses before plaintext admission");
    }
    for(std::size_t n=0;n<=33;++n) if(n!=32) {
        actual.fill(0xa5);
        Check(c::SealNativeHashKeyEnvelopeV1(b,{sec.Root(),n},sec.KeyIn(),Out(actual))==E::invalid_extent &&
              Filled(In(actual),0xa5),"exact binary wrapping-root extent");
        Check(c::SealNativeHashKeyEnvelopeV1(b,sec.RootIn(),{sec.Key(),n},Out(actual))==E::invalid_extent,
              "exact binary routing-key extent");
        sec.Dirty();
        Check(c::OpenNativeHashKeyEnvelopeV1(b,sec.RootIn(),In(oracle),{sec.Plain(),n})==E::invalid_extent &&
              Filled({sec.Plain(),32},0xa5),"exact plaintext extent / no writes on bad extent");
    }
    for(unsigned which=0;which<2;++which) for(std::size_t at=0;at<actual.size();++at) {
        actual.fill(0xa5);
        const c::NativeHashKeyInputV1 overlap{actual.data()+at,32};
        Check(c::SealNativeHashKeyEnvelopeV1(b,which?sec.RootIn():overlap,which?overlap:sec.KeyIn(),Out(actual))==E::overlap &&
              Filled(In(actual),0xa5),"all seal input/output overlap offsets");
    }
    auto* maximum=reinterpret_cast<unsigned char*>(std::numeric_limits<std::uintptr_t>::max()-7);
    Check(c::SealNativeHashKeyEnvelopeV1(b,{maximum,32},sec.KeyIn(),Out(actual))==E::invalid_extent,
          "wrapping pointer extent overflow refuses before dereference");
    Check(c::OpenNativeHashKeyEnvelopeV1(b,sec.RootIn(),In(oracle),{maximum,32})==E::invalid_extent,
          "plaintext pointer extent overflow refuses before dereference");
    for(std::size_t at=0;at<oracle.size();++at) {
        std::array<unsigned char,320> overlap{};
        std::copy(oracle.begin(),oracle.end(),overlap.begin());
        const auto before=overlap;
        Check(c::OpenNativeHashKeyEnvelopeV1(b,sec.RootIn(),{overlap.data(),288},{overlap.data()+at,32})==E::overlap &&
              overlap==before,"every ciphertext/plaintext overlap offset refuses unchanged");
    }
    Check(c::OpenNativeHashKeyEnvelopeV1(b,sec.RootIn(),In(oracle),{sec.Root(),32})==E::overlap,
          "plaintext cannot overwrite its admitted wrapping root");
    for(unsigned i=0;i<32;++i) Check(sec.Root()[i]==i,"overlap refusal preserves original root");
    for(auto value:{std::uint64_t{1},UINT64_MAX}) {
        auto edge=b;edge.key_generation=edge.hash_profile_generation=edge.seed_policy_generation=edge.wrapping_root_generation=value;
        Record wire{};
        Check(c::SealNativeHashKeyEnvelopeV1(edge,sec.RootIn(),sec.KeyIn(),Out(wire))==E::none &&
              c::OpenNativeHashKeyEnvelopeV1(edge,sec.RootIn(),In(wire),sec.PlainOut())==E::none &&
              !std::memcmp(sec.Plain(),sec.Key(),32),"minimum/maximum exact unsigned generation encoding");
    }
    for(bool seal:{false,true}) for(unsigned kind=1;kind<=8;++kind) {
        bool failed=false,finished=false;
        if(!seal && kind==8) continue;
        for(unsigned nth=1;nth<16;++nth) {
            actual.fill(0xa5); sec.Dirty(); fault_kind=kind; fault_at=nth; fault_calls=0;
            deny_cpp=true;
            code=seal?c::SealNativeHashKeyEnvelopeV1(b,sec.RootIn(),sec.KeyIn(),Out(actual)):
                      c::OpenNativeHashKeyEnvelopeV1(b,sec.RootIn(),In(oracle),sec.PlainOut());
            deny_cpp=false; const bool fired=fault_calls>=nth; fault_kind=0;
            if(fired) { failed=true; Check(code!=E::none && (seal?Filled(In(actual),0):Filled({sec.Plain(),32},0)),
                                         "actual RNG/provider fault must erase admitted output"); }
            else { Check(code==E::none,"complete normal provider call after every injected position"); finished=true; break; }
        }
        Check(failed&&finished,"every actual backend fault position exhausted without weakening success");
    }
    struct Probe { unsigned count=0,at=0; bool throwing=false;
        static bool Poll(void* p) { auto& q=*static_cast<Probe*>(p); if(++q.count!=q.at)return false; if(q.throwing)throw 17; return true; }};
    for(bool seal:{false,true}) for(bool throwing:{false,true}) {
        bool finished=false;
        for(unsigned at=1;at<16;++at) {
            Probe p{0,at,throwing}; actual.fill(0xa5); sec.Dirty(); bool threw=false;
            try { code=seal?c::SealNativeHashKeyEnvelopeV1(b,sec.RootIn(),sec.KeyIn(),Out(actual),{Probe::Poll,&p}):
                           c::OpenNativeHashKeyEnvelopeV1(b,sec.RootIn(),In(oracle),sec.PlainOut(),{Probe::Poll,&p}); }
            catch(int n) { Check(n==17,"exact caller exception retained"); threw=true; }
            if(p.count>=at) Check((threw || code==E::cancelled) && (seal?Filled(In(actual),0):Filled({sec.Plain(),32},0)),
                                 "every cancellation/exception checkpoint scrubs private output");
            else { Check(code==E::none,"all probe positions complete"); finished=true; break; }
        }
        Check(finished,"probe sweep reached a full normal completion");
    }
    Record first{},second{};
    Check(c::SealNativeHashKeyEnvelopeV1(b,sec.RootIn(),sec.KeyIn(),Out(first))==E::none &&
          c::SealNativeHashKeyEnvelopeV1(b,sec.RootIn(),sec.KeyIn(),Out(second))==E::none && first!=second,
          "actual RNG supplies fresh per-envelope salt/nonce,not a default key/seed");
    {
        h::CryptoMemoryScope denied(sec.pool,sec.binding,true); Check(denied.ok(),"deny new backend allocation scope"); sec.Dirty();
        Check(c::OpenNativeHashKeyEnvelopeV1(b,sec.RootIn(),In(oracle),sec.PlainOut())==E::memory_scope_unavailable &&
              Filled({sec.Plain(),32},0),"no ordinary heap/backend fallback when allocation is denied");
    }
    OPENSSL_thread_stop();
}

struct TempFile {
    std::filesystem::path directory,path;
    d::FileDevice device;
    TempFile() {
        auto name=(std::filesystem::temp_directory_path()/"sb-key-record-XXXXXX").string();
        std::vector<char> chars(name.begin(),name.end()); chars.push_back(0);
        auto* made=::mkdtemp(chars.data()); Check(made,"private actual-file test directory");
        directory=made; path=directory/"encrypted-records.bin";
        Check(device.Open(path.string(),d::FileOpenMode::create_new).ok(),"actual custody fixture file");
        std::array<unsigned char,8192> zeros{};
        Check(device.WriteAt(0,zeros.data(),zeros.size()).ok() && device.Sync().ok(),"actual preallocated test slot extent");
    }
    ~TempFile() { io_fault=IoFault::none; (void)device.Close(); std::error_code e;std::filesystem::remove_all(directory,e); }
};
void StorageChecks(Secrets& sec) {
    const auto binding=Binding();
    const auto original=Oracle();
    for(auto mode:{IoFault::none,IoFault::full_write,IoFault::partial_write,IoFault::sync,IoFault::readback,IoFault::read,IoFault::extent}) {
        TempFile file; slot_reads=0; io_fault=mode;
        const auto result=s::StageNativeHashKeyRecordOnOpenDevice(file.device,slot,binding,original);
        io_fault=IoFault::none;
        if(mode==IoFault::none) Check(result.ok() && result.effects.write_calls==1 && result.effects.write_bytes_observed==288 &&
            result.effects.sync_completed && result.effects.readback_verified,"actual ciphertext stage/sync/readback");
        else {
            Check(!result.ok(),"actual native I/O fault cannot report completion");
            if(mode==IoFault::partial_write) Check(result.effects.write_bytes_observed==96 && !result.effects.sync_attempted &&
                result.effects.original_or_write_may_remain,"partial write receipt retains real prefix/uncertainty");
            if(mode==IoFault::sync) Check(result.effects.write_bytes_observed==288 && result.effects.sync_attempted &&
                !result.effects.sync_completed && result.effects.original_or_write_may_remain,"sync failure preserves written original");
            if(mode==IoFault::readback) Check(result.error==R::readback_mismatch && result.effects.sync_completed &&
                !result.effects.readback_verified,"readback failure cannot erase physical sync observations");
            if(mode==IoFault::read) Check(result.effects.write_calls==0 && result.last_io.error!=d::BoundedIoError::none,
                "source read failure cause retained before any write");
            if(mode==IoFault::extent) Check(result.error==R::extent_changed && result.effects.write_calls==0,
                "changed actual extent refuses before write");
        }
        if(mode==IoFault::extent) continue;
        auto at=static_cast<std::uint64_t>(slot);
        if(mode==IoFault::partial_write) {
            const auto retry=s::StageNativeHashKeyRecordOnOpenDevice(file.device,at,binding,original);
            Check(retry.error==R::conflicting_record && !retry.effects.write_calls,"partial original is not an empty retry slot");
            at=4096; // Original operation, new explicitly allocated fixture slot; no fresh key/intent.
        }
        const auto done=s::StageNativeHashKeyRecordOnOpenDevice(file.device,at,binding,original);
        Check(done.ok() && done.effects.sync_completed && done.effects.readback_verified,"reconcile exact original after actual failure");
        const auto replay=s::StageNativeHashKeyRecordOnOpenDevice(file.device,at,binding,original);
        Check(replay.ok() && replay.effects.exact_existing_record && replay.effects.write_calls==0,"duplicate does not reallocate/rewrite");
        auto conflict=original; conflict[240]^=1;
        const auto refused=s::StageNativeHashKeyRecordOnOpenDevice(file.device,at,binding,conflict);
        Check(refused.error==R::conflicting_record && !refused.effects.write_calls,"conflicting ciphertext never overwrites original");
        Check(file.device.Close().ok() && file.device.Open(file.path.string(),d::FileOpenMode::open_existing).ok(),"real close/reopen");
        const auto read=s::ReadNativeHashKeyRecordFromOpenDevice(file.device,at,binding);
        Check(read.ok() && read.record==original && read.effects.read_bytes_observed==288,"read exact reopened original ciphertext");
        if(at==slot) {
            io_fault=IoFault::read;
            const auto failed_read=s::ReadNativeHashKeyRecordFromOpenDevice(file.device,at,binding);
            io_fault=IoFault::none;
            Check(!failed_read.ok() && failed_read.record==Record{} && failed_read.effects.read_calls==1 &&
                  failed_read.last_io.error!=d::BoundedIoError::none,"failed real read returns no prefix and retains exact backend cause");
        }
        {
            h::CryptoMemoryScope scope(sec.pool,sec.binding); Check(scope.ok(),"reopen key memory scope");sec.Dirty();
            Check(c::OpenNativeHashKeyEnvelopeV1(binding,sec.RootIn(),In(read.record),sec.PlainOut())==E::none &&
                !std::memcmp(sec.Plain(),sec.Key(),32),"actual reopened ciphertext yields original protected binary key");OPENSSL_thread_stop();
        }
        const auto before=file.device.Size().size_bytes;
        Check(s::StageNativeHashKeyRecordOnOpenDevice(file.device,before-1,binding,original).error==R::slot_not_allocated &&
            s::StageNativeHashKeyRecordOnOpenDevice(file.device,UINT64_MAX,binding,original).error==R::invalid_extent &&
            file.device.Size().size_bytes==before,"no hidden growth or overflowing placement");
        auto foreign=binding; ++foreign.key_generation;
        const auto wrong=s::ReadNativeHashKeyRecordFromOpenDevice(file.device,at,foreign);
        Check(!wrong.ok() && wrong.record==Record{},"foreign binding returns no ciphertext prefix");
        std::array<unsigned char,8192> physical{};
        Check(file.device.ReadAt(0,physical.data(),physical.size()).ok(),"actual file disclosure inspection");
        Check(std::search(physical.begin(),physical.end(),sec.Root(),sec.Root()+32)==physical.end() &&
              std::search(physical.begin(),physical.end(),sec.Key(),sec.Key()+32)==physical.end(),
              "neither wrapping root nor routing plaintext persisted in the encrypted file");
    }
    {
        TempFile file;
        Check(s::StageNativeHashKeyRecordOnOpenDevice(file.device,slot,binding,original).ok(),
              "prepare actual record for read-only reopen");
        Check(file.device.Close().ok() &&
              file.device.Open(file.path.string(),d::FileOpenMode::open_existing_read_only).ok(),
              "actual read-only custody device reopen");
        const auto read=s::ReadNativeHashKeyRecordFromOpenDevice(file.device,slot,binding);
        Check(read.ok() && read.record==original && !read.effects.sync_attempted,
              "read-only ciphertext read does not claim synchronization");
        const auto duplicate=s::StageNativeHashKeyRecordOnOpenDevice(file.device,slot,binding,original);
        Check(duplicate.error==R::synchronization_not_performed && duplicate.effects.exact_existing_record &&
              duplicate.effects.write_calls==0 && duplicate.effects.original_or_write_may_remain &&
              duplicate.effects.sync_attempted && !duplicate.effects.sync_completed &&
              !duplicate.effects.readback_verified && duplicate.last_io.read_only_noop &&
              !duplicate.last_io.native_attempted && !duplicate.last_io.synchronized,
              "read-only sync no-op cannot complete duplicate staging");
        const auto empty=s::StageNativeHashKeyRecordOnOpenDevice(file.device,4096,binding,original);
        Check(empty.error==R::io_failure && empty.last_io.error==d::BoundedIoError::read_only &&
              empty.effects.write_calls==1 && empty.effects.write_bytes_observed==0 &&
              !empty.last_io.native_attempted && !empty.effects.original_or_write_may_remain &&
              !empty.effects.sync_attempted && !empty.effects.sync_completed,
              "read-only write refusal preserves actual pre-native no-effect outcome");
        std::array<unsigned char,288> zero{};
        Check(file.device.ReadAt(4096,zero.data(),zero.size()).ok() && Filled({zero.data(),zero.size()},0) &&
              file.device.Size().size_bytes==8192,
              "read-only staging leaves empty slot and extent unchanged");
    }
    TempFile concurrent; std::array<s::NativeHashKeyRecordResult,2> results;
    std::thread a([&]{results[0]=s::StageNativeHashKeyRecordOnOpenDevice(concurrent.device,slot,binding,original);});
    std::thread b([&]{results[1]=s::StageNativeHashKeyRecordOnOpenDevice(concurrent.device,slot,binding,original);});
    a.join();b.join();
    Check(results[0].ok() && results[1].ok() && results[0].effects.write_calls+results[1].effects.write_calls==1 &&
        results[0].effects.exact_existing_record!=results[1].effects.exact_existing_record,
        "actual device fence serializes duplicate writers without duplicate physical effects");
}
void FinalScopeRevocationChecks(Secrets& sec) {
    const auto b=Binding();const auto oracle=Oracle();
    h::CryptoMemoryScope scope(sec.pool,sec.binding);Check(scope.ok(),"actual scope before revocation");
    struct Probe {
        h::CryptoMemoryPool* pool; unsigned count=0,at=0;
        static bool Poll(void* p) {
            auto& q=*static_cast<Probe*>(p);
            if(++q.count==q.at) Check(q.pool->Revoke()==h::CryptoMemoryError::none,"actual final-probe pool revocation");
            return false;
        }
    };
    Probe baseline{&sec.pool};sec.Dirty();
    Check(c::OpenNativeHashKeyEnvelopeV1(b,sec.RootIn(),In(oracle),sec.PlainOut(),{Probe::Poll,&baseline})==E::none &&
          baseline.count>0,"observe actual complete decrypt probe sequence");
    Probe revoked{&sec.pool,0,baseline.count};sec.Dirty();
    Check(c::OpenNativeHashKeyEnvelopeV1(b,sec.RootIn(),In(oracle),sec.PlainOut(),{Probe::Poll,&revoked})==E::memory_scope_unavailable &&
          revoked.count==baseline.count && Filled({sec.Plain(),32},0),
          "revocation at final success probe prevents plaintext completion");
    Check(h::CheckExplicitCryptoMemoryScope(b.node)==h::CryptoMemoryError::revoked,
          "entry-time scope success is not perpetual resource permission");
    h::CryptoMemoryScope new_scope(sec.pool,sec.binding);
    Check(!new_scope.ok() && new_scope.error()==h::CryptoMemoryError::revoked,
          "revoked pool cannot issue a new scope");
}
int main() {
    static_assert(!std::is_constructible_v<m::ScopedProtectedBuffer,m::BoundedAllocator*,void*,std::size_t,std::size_t,m::MemoryTag,m::ProtectedMemoryEvidence>);
    static_assert(sizeof(Record)==288);
    MissingAdapterChecks();
    Secrets secrets;
    ExplicitMemoryChecks(secrets);
    CryptoChecks(secrets);
    StorageChecks(secrets);
    FinalScopeRevocationChecks(secrets);
    secrets.Drain();
    std::cout<<"PASS native encrypted HASH key record checks="<<checks<<" (not native HASH provider completion)\n";
}
