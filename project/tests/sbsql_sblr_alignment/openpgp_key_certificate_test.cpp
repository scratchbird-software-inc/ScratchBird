// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "openpgp_key_certificate.hpp"
#include "openpgp_public_key_message.hpp"
#include "crypto_memory_adapter.hpp"
#include "memory.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace c = scratchbird::core::crypto;
namespace h = scratchbird::core::hash;
namespace m = scratchbird::core::memory;
using Code = c::NativeKeyCertificateCode;
using Bytes = std::vector<unsigned char>;
using Uuid = scratchbird::core::platform::Uuid;
unsigned checks=0, fault_kind=0, fault_at=0, fault_calls=0;
thread_local bool deny_cpp=false;
bool Fault(unsigned kind) { return kind==fault_kind && ++fault_calls==fault_at; }
void* operator new(std::size_t n) {
  if(deny_cpp) throw std::bad_alloc();
  if(auto* p=std::malloc(n?n:1)) return p; throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
extern "C" {
EVP_MD_CTX* __real_EVP_MD_CTX_new();
EVP_MD_CTX* __wrap_EVP_MD_CTX_new() { return Fault(1)?nullptr:__real_EVP_MD_CTX_new(); }
EVP_PKEY* __real_EVP_PKEY_new_raw_public_key(int,ENGINE*,const unsigned char*,std::size_t);
EVP_PKEY* __wrap_EVP_PKEY_new_raw_public_key(int t,ENGINE* e,const unsigned char* p,std::size_t n) {
  return Fault(2)?nullptr:__real_EVP_PKEY_new_raw_public_key(t,e,p,n);
}
int __real_EVP_DigestVerifyInit(EVP_MD_CTX*,EVP_PKEY_CTX**,const EVP_MD*,ENGINE*,EVP_PKEY*);
int __wrap_EVP_DigestVerifyInit(EVP_MD_CTX* p,EVP_PKEY_CTX** k,const EVP_MD* d,ENGINE* e,EVP_PKEY* s) {
  return Fault(3)?0:__real_EVP_DigestVerifyInit(p,k,d,e,s);
}
int __real_EVP_DigestVerify(EVP_MD_CTX*,const unsigned char*,std::size_t,const unsigned char*,std::size_t);
int __wrap_EVP_DigestVerify(EVP_MD_CTX* p,const unsigned char* s,std::size_t n,const unsigned char* v,std::size_t m) {
  return Fault(4)?-1:__real_EVP_DigestVerify(p,s,n,v,m);
}
EVP_PKEY_CTX* __real_EVP_PKEY_CTX_new_id(int,ENGINE*);
EVP_PKEY_CTX* __wrap_EVP_PKEY_CTX_new_id(int t,ENGINE* e) { return Fault(5)?nullptr:__real_EVP_PKEY_CTX_new_id(t,e); }
int __real_EVP_PKEY_derive(EVP_PKEY_CTX*,unsigned char*,std::size_t*);
int __wrap_EVP_PKEY_derive(EVP_PKEY_CTX* p,unsigned char* o,std::size_t* n) {
  if(Fault(6)) { if(*n) o[0]=0xcc; return 0; } return __real_EVP_PKEY_derive(p,o,n);
}
EVP_CIPHER_CTX* __real_EVP_CIPHER_CTX_new();
EVP_CIPHER_CTX* __wrap_EVP_CIPHER_CTX_new() { return Fault(7)?nullptr:__real_EVP_CIPHER_CTX_new(); }
int __real_EVP_CipherInit_ex(EVP_CIPHER_CTX*,const EVP_CIPHER*,ENGINE*,const unsigned char*,const unsigned char*,int);
int __wrap_EVP_CipherInit_ex(EVP_CIPHER_CTX* p,const EVP_CIPHER* c,ENGINE* e,const unsigned char* k,const unsigned char* v,int d) {
  return Fault(8)?0:__real_EVP_CipherInit_ex(p,c,e,k,v,d);
}
int __real_EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX*,int,int,void*);
int __wrap_EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX* p,int t,int n,void* v) { return Fault(9)?0:__real_EVP_CIPHER_CTX_ctrl(p,t,n,v); }
int __real_EVP_CipherUpdate(EVP_CIPHER_CTX*,unsigned char*,int*,const unsigned char*,int);
int __wrap_EVP_CipherUpdate(EVP_CIPHER_CTX* p,unsigned char* o,int* n,const unsigned char* i,int s) {
  if(Fault(10)) { if(o && s) o[0]=0xcc; return 0; } return __real_EVP_CipherUpdate(p,o,n,i,s);
}
int __real_EVP_CipherFinal_ex(EVP_CIPHER_CTX*,unsigned char*,int*);
int __wrap_EVP_CipherFinal_ex(EVP_CIPHER_CTX* p,unsigned char* o,int* n) {
  if(Fault(11)) { o[0]=0xcc; return 0; } return __real_EVP_CipherFinal_ex(p,o,n);
}
EVP_PKEY* __real_EVP_PKEY_new_raw_private_key(int,ENGINE*,const unsigned char*,std::size_t);
EVP_PKEY* __wrap_EVP_PKEY_new_raw_private_key(int t,ENGINE* e,const unsigned char* p,std::size_t n) {
  return Fault(12)?nullptr:__real_EVP_PKEY_new_raw_private_key(t,e,p,n);
}
}
void Check(bool value,const char* label) {
  ++checks; if(!value) { deny_cpp=false; std::cerr<<"FAIL "<<label<<'\n'; std::exit(1); }
}
template<class T> c::PgpInput In(const T& b) { return {b.data(),b.size()}; }
bool Filled(c::PgpInput b,unsigned char v) { return std::all_of(b.data,b.data+b.size,[=](auto x){return x==v;}); }
Bytes Hex(std::string_view h) {
  Check(h.size()%2==0,"fixture hex extent"); Bytes out(h.size()/2);
  auto nib=[](char b)->unsigned { return b<='9'?b-'0':b-'a'+10; };
  for(std::size_t i=0;i<out.size();++i) out[i]=16*nib(h[2*i])+nib(h[2*i+1]); return out;
}
Bytes B64(std::string_view s) {
  Bytes out(3*s.size()/4); const int n=EVP_DecodeBlock(out.data(),reinterpret_cast<const unsigned char*>(s.data()),s.size());
  Check(n>=0,"published RFC base64 fixture"); std::size_t end=n;
  if(s.back()=='=') --end; if(s[s.size()-2]=='=') --end; out.resize(end); return out;
}
void Append(Bytes& a,const Bytes& b) { a.insert(a.end(),b.begin(),b.end()); }
void Put32(Bytes& a,std::uint32_t n) { for(int s=24;s>=0;s-=8) a.push_back(n>>s); }
Bytes Word(std::uint32_t n) { Bytes out;Put32(out,n);return out; }
Bytes Packet(unsigned tag,const Bytes& body) {
  // Independent shortest definite-length fixture encoder (not EncodePacket).
  Bytes out{static_cast<unsigned char>(192|tag)};
  if(body.size()<192) out.push_back(body.size());
  else if(body.size()<8384) { auto n=body.size()-192;out.push_back((n>>8)+192);out.push_back(n); }
  else {out.push_back(255);Put32(out,body.size());} Append(out,body);return out;
}
struct Part { unsigned tag; Bytes body; };
std::vector<Part> Split(const Bytes& certificate) {
  std::vector<Part> out;
  for(std::size_t at=0;at<certificate.size();) {
    Check(at+2<=certificate.size(),"fixture packet prefix");unsigned tag=certificate[at++]&63,n=certificate[at++];
    if(n>=192 && n<224) {Check(at<certificate.size(),"fixture two byte size");n=(n-192)*256+certificate[at++]+192;}
    else if(n==255) {Check(at+4<=certificate.size(),"fixture five byte size");n=0;for(unsigned i=0;i<4;++i)n=(n<<8)|certificate[at++];}
    Check(n<=certificate.size()-at,"fixture packet body");out.push_back({tag,Bytes(certificate.begin()+at,certificate.begin()+at+n)});at+=n;
  } return out;
}
Bytes Join(const std::vector<Part>& parts) {Bytes out;for(const auto& p:parts)Append(out,Packet(p.tag,p.body));return out;}
Bytes Public(const Bytes& b) {return Bytes(b.begin(),b.begin()+42);}
Bytes Fingerprint(const Bytes& b) {
  Bytes pre{0x9b};Put32(pre,42);Append(pre,Public(b));Bytes out(32);unsigned n=0;
  Check(EVP_Digest(pre.data(),pre.size(),out.data(),&n,EVP_sha256(),nullptr)==1 && n==32,"independent fixture fingerprint");return out;
}
constexpr std::uint32_t created=0x63877fe3;
constexpr c::NativeKeyCertificateLimits limits{1024*1024,256,2048};
c::NativeKeyCertificateQuery Encrypt(std::uint64_t now=created+100) {return {now,c::NativeKeyCertificatePurpose::encrypt,{}};}
c::NativeKeyCertificateQuery Decrypt(const Bytes& fp,std::uint64_t now=created+100) {return {now,c::NativeKeyCertificatePurpose::decrypt,In(fp)};}
Code Validate(const Bytes& b,c::NativeKeyCertificateQuery q=Encrypt()) {
  const auto r=c::ValidateNativeKeyCertificate(In(b),q,limits);
  if(r.code!=Code::ok) Check(!r.public_body.size && !r.secret_body.size &&
      std::all_of(r.recipient_fingerprint.begin(),r.recipient_fingerprint.end(),[](auto v){return !v;}),"failed certificate publishes no selection");
  return r.code;
}
Bytes PublicFixture() {
  // RFC9580 Appendix A.3, independent unmodified transferable certificate.
  // https://www.rfc-editor.org/rfc/rfc9580.html#appendix-A.3
  return B64("xioGY4d/4xsAAAAg+U2nu0jWCmHlZ3BqZYfQMxmZu52JGggkLq2EVD34laPCsQYf"
    "GwoAAABCBYJjh3/jAwsJBwUVCg4IDAIWAAKbAwIeCSIhBssYbE8GCaaX5NUt+mxy"
    "KwwfHifBilZwj2Ul7Ce62azJBScJAgcCAAAAAK0oIBA+LX0ifsDm185Ecds2v8lw"
    "gyU2kCcUmKfvBXbAf6rhRYWzuQOwEn7E/aLwIwRaLsdry0+VcallHhSu4RN6HWaE"
    "QsiPlR4zxP/TP7mhfVEe7XWPxtnMUMtf15OyA51YBM4qBmOHf+MZAAAAIIaTJINn"
    "+eUBXbki+PSAld2nhJh/LVmFsS+60WyvXkQ1wpsGGBsKAAAALAWCY4d/4wKbDCIh"
    "BssYbE8GCaaX5NUt+mxyKwwfHifBilZwj2Ul7Ce62azJAAAAAAQBIKbpGG2dWTX8"
    "j+VjFM21J0hqWlEg+bdiojWnKfA5AQpWUWtnNwDEM0g12vYxoWM8Y81W+bHBw805"
    "I8kWVkXU6vFOi+HWvv/ira7ofJu16NnoUkhclkUrk0mXubZvyl4GBg==");
}
Bytes SecretFixture() {
  // RFC9580 Appendix A.4, exact clear Ed25519 and X25519 private scalars.
  auto p=Split(PublicFixture());p[0]={5,Hex("0663877fe31b00000020f94da7bb48d60a61e567706a6587d0331999bb9d891a08242ead84543df895a3001972817b12be707e8d5f586ce61361201d344eb266a2c82fde6835762b65b0b7")};
  p[2]={7,Hex("0663877fe319000000208693248367f9e5015db922f8f48095dda784987f2d5985b12fbad16caf5e4435004d600a4f794d44775c57a26e0feefed558e9afffd6ad0d582d57fb2ba2dcedb8")};return Join(p);
}
void Sub(Bytes& b,unsigned type,const Bytes& value) {
  Check(value.size()<191,"fixture short subpacket");b.push_back(value.size()+1);b.push_back(type);Append(b,value);
}
struct SignatureOptions {
  unsigned hash=8,flags=12;std::uint32_t time=created,signature_expiry=0,key_expiry=0;
  bool flags_hashed=true,unknown_critical=false,nonrevocable=false,duplicate_conflict=false;
};
Bytes Signature(const Bytes& primary,const Bytes& subject,unsigned subject_type,unsigned type,
                SignatureOptions o={}) {
  // Independent one-shot RFC preimage construction for adversarial certificates;
  // the production implementation streams the preimage and never uses this code.
  Bytes hashed;Sub(hashed,0x82,Word(o.time));Bytes issuer{6};Append(issuer,Fingerprint(primary));Sub(hashed,33,issuer);
  if(o.flags_hashed)Sub(hashed,27,Bytes{static_cast<unsigned char>(o.flags)});
  if(o.signature_expiry)Sub(hashed,3,Word(o.signature_expiry));
  if(o.key_expiry)Sub(hashed,9,Word(o.key_expiry));
  if(o.nonrevocable)Sub(hashed,0x87,Bytes{0});
  if(o.unknown_critical)Sub(hashed,128|100,Bytes{1});
  if(o.duplicate_conflict)Sub(hashed,27,Bytes{3});
  Bytes sig{6,static_cast<unsigned char>(type),27,static_cast<unsigned char>(o.hash)};Put32(sig,hashed.size());Append(sig,hashed);
  const EVP_MD* hash=o.hash==8?EVP_sha256():o.hash==9?EVP_sha384():o.hash==10?EVP_sha512():o.hash==12?EVP_sha3_256():EVP_sha3_512();
  const std::size_t salt_n=EVP_MD_size(hash)/2;Bytes salt(salt_n);for(std::size_t i=0;i<salt.size();++i)salt[i]=0xa0+i;
  Bytes pre=salt;pre.push_back(0x9b);Put32(pre,42);Append(pre,Public(primary));
  if(subject_type){pre.push_back(subject_type==13?0xb4:subject_type==17?0xd1:0x9b);Put32(pre,subject.size());Append(pre,subject);}
  Append(pre,sig);pre.push_back(6);pre.push_back(255);Put32(pre,sig.size());
  std::array<unsigned char,64> digest{};unsigned n=0;
  Check(EVP_Digest(pre.data(),pre.size(),digest.data(),&n,hash,nullptr)==1,"independent one-shot signed preimage");
  const auto seed=Hex("1972817b12be707e8d5f586ce61361201d344eb266a2c82fde6835762b65b0b7");
  std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519,nullptr,seed.data(),seed.size()),EVP_PKEY_free);
  std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
  std::array<unsigned char,64> signature{};std::size_t signature_n=64;
  Check(key && ctx && EVP_DigestSignInit(ctx.get(),nullptr,nullptr,nullptr,key.get())==1 &&
        EVP_DigestSign(ctx.get(),signature.data(),&signature_n,digest.data(),n)==1 && signature_n==64,"actual Ed25519 fixture signature");
  Bytes unhashed;if(!o.flags_hashed)Sub(unhashed,27,Bytes{static_cast<unsigned char>(o.flags)});
  Put32(sig,unhashed.size());Append(sig,unhashed);sig.push_back(digest[0]);sig.push_back(digest[1]);sig.push_back(salt.size());Append(sig,salt);
  sig.insert(sig.end(),signature.begin(),signature.end());return sig;
}
struct Secrets {
  m::MemoryManager manager;
  m::ScopedProtectedBuffer provider,workspace,secret;
  h::CryptoMemoryPool pool;
  h::CryptoMemoryBinding binding;
  static Uuid Id(unsigned n){Uuid u{};u.bytes[0]=1;u.bytes[6]=0x70;u.bytes[8]=0x80;u.bytes[15]=n;return u;}
  static auto Policy(){auto p=m::DefaultLocalEngineMemoryPolicy();p.hard_limit_bytes=8*1024*1024;p.per_context_limit_bytes=p.hard_limit_bytes;return p;}
  Secrets():manager(Policy()),binding{Id(1),Id(2),Id(3),Id(4)} {
    const auto page=static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    for(auto* owner:{&provider,&workspace,&secret}) {
      m::ProtectedMemoryRequest r;r.bytes=owner==&provider?4*1024*1024:owner==&workspace?8192:page;r.alignment=page;
      r.platform_policy=m::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump;
      r.tag.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database.bytes;
      r.tag.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner.bytes;
      r.tag.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context.bytes;
      auto got=manager.AllocateProtected(std::move(r));Check(got.ok(),"actual charged protected key/provider workspace");
      *owner=std::move(got.buffer);Check(owner->evidence().platform_lock_succeeded && owner->evidence().no_dump_succeeded,"actual lock/no-dump backing");
    }
    Check(pool.Open(binding,provider.data(),provider.size())==h::CryptoMemoryError::none,"actual bounded provider arena");
    Check(h::InstallCryptoMemoryAdapter(pool)==h::CryptoMemoryError::none,"install provider before any OpenSSL allocation");
  }
  c::PgpOutput Work(){return {static_cast<unsigned char*>(workspace.data()),8192};}
  c::PgpOutput Out(){return {static_cast<unsigned char*>(secret.data()),32};}
  void Dirty(){std::memset(workspace.data(),0xa5,workspace.size());std::memset(secret.data(),0xa5,secret.size());}
  void Drain(){OPENSSL_thread_stop();OPENSSL_cleanup();Check(pool.Snapshot().live_blocks==0,"all provider objects drained");
    Check(h::StopCryptoMemoryAdapter()==h::CryptoMemoryError::none,"stop actual adapter");Check(pool.Close()==h::CryptoMemoryError::none,"close provider arena");
    for(auto* owner:{&secret,&workspace,&provider})Check(owner->Reset().ok(),"erase/release actual protected owner");
    Check(manager.Snapshot().current_bytes==0,"zero memory-manager charge after drain");}
};
void PublishedCertificates(Secrets& sec) {
  const auto pub=PublicFixture(),clear=SecretFixture();const auto parts=Split(pub);
  const auto fp=Hex("12c83f1e706f6308fe151a417743a1f033790e93e9978488d1db378da9930885");
  const auto primary=Hex("cb186c4f0609a697e4d52dfa6c722b0c1f1e27c18a56708f6525ec27bad9acc9");
  const auto result=c::ValidateNativeKeyCertificate(In(pub),Encrypt(),limits);
  Check(result.code==Code::ok && std::equal(fp.begin(),fp.end(),result.recipient_fingerprint.begin()) &&
    std::equal(primary.begin(),primary.end(),result.primary_fingerprint.begin()) &&
    result.public_body.size==42 && result.public_body.data>=pub.data() && result.public_body.data+42<=pub.data()+pub.size(),"independent RFC A.3 fingerprints/signatures/borrowed selection");
  Check(Validate(clear)==Code::ok,"complete RFC A.4 secret certificate validates");
  sec.Dirty();Check(c::UnlockNativeCertificateKey(In(clear),Decrypt(fp),limits,std::nullopt,{},sec.Out())==Code::ok &&
    std::memcmp(sec.Out().data,Split(clear)[2].body.data()+43,32)==0,"exact RFC A.4 X25519 secret unlock and public-derived match");
  deny_cpp=true;const auto no_heap=c::ValidateNativeKeyCertificate(In(pub),Encrypt(),limits);deny_cpp=false;
  Check(no_heap.code==Code::ok,"certificate validation no ordinary C++ heap");
  for(std::size_t n=0;n<pub.size();++n) {
    Bytes truncated(pub.begin(),pub.begin()+n);Check(Validate(truncated)!=Code::ok,"every truncated RFC certificate refuses without publication");
  }
  for(std::size_t i=0;i<pub.size();++i) {auto b=pub;b[i]^=1;
    // An advisory unhashed field may legally change without invalidating the
    // certificate. Changes of authenticated material may not select bad keys.
    const auto selected=c::ValidateNativeKeyCertificate(In(b),Encrypt(),limits);
    if(selected.code==Code::ok) Check(selected.public_body.size==42 &&
      std::memcmp(selected.public_body.data,parts[2].body.data(),42)==0,"mutation never changes selected public identity without authentic binding");
  }
  auto q=Encrypt();q.recipient_fingerprint=In(fp);Check(Validate(pub,q)==Code::invalid_extent,"encrypt cannot accept recipient selector");
  Check(Validate(pub,Decrypt(fp))==Code::recipient_not_found,"public certificate cannot release private key");
  Check(Validate(pub,Encrypt(created-1))!=Code::ok,"admitted time before actual key creation");
  auto bad=limits;bad.maximum_packets=3;Check(c::ValidateNativeKeyCertificate(In(pub),Encrypt(),bad).code==Code::resource_limit,"actual packet bound");
  bad=limits;bad.maximum_signature_subpackets=1;Check(c::ValidateNativeKeyCertificate(In(pub),Encrypt(),bad).code==Code::resource_limit,"actual subpacket bound");
  bad=limits;bad.maximum_bytes=pub.size()-1;Check(c::ValidateNativeKeyCertificate(In(pub),Encrypt(),bad).code==Code::resource_limit,"actual byte bound");
  bad=limits;bad.maximum_packets=0;Check(c::ValidateNativeKeyCertificate(In(pub),Encrypt(),bad).code==Code::invalid_limits,"no invented bound default");
}
void PolicyCertificates() {
  const auto original=Split(PublicFixture());const auto primary=original[0].body,sub=original[2].body;
  for(unsigned hash:{8u,9u,10u,12u,14u}) {
    SignatureOptions o;o.hash=hash;
    auto p=original;p[1].body=Signature(primary,{},0,0x1f,o);p[3].body=Signature(primary,sub,14,0x18,o);
    Check(Validate(Join(p))==Code::ok,"all native admitted Ed25519 hash/salt profiles");
  }
  for(unsigned kind=0;kind<6;++kind) {
    auto p=original;SignatureOptions o;
    if(kind==0)o.flags_hashed=false;if(kind==1)o.flags=3;if(kind==2)o.unknown_critical=true;
    if(kind==3)o.duplicate_conflict=true;if(kind==4)o.key_expiry=10;if(kind==5)o.signature_expiry=10;
    p[3].body=Signature(primary,sub,14,0x18,o);Check(Validate(Join(p))!=Code::ok,"unhashed/invalid flags, critical fields, ambiguous and expired policy cannot authorize");
    if(kind==2){p.push_back(original[3]);Check(Validate(Join(p))==Code::ok,"bad critical signature does not suppress alternative valid binding");}
  }
  auto p=original;SignatureOptions o;o.time=created+1;o.flags=3;
  p.push_back({2,Signature(primary,sub,14,0x18,o)});Check(Validate(Join(p))==Code::no_eligible_key,"newest authenticated binding controls eligibility");
  o.time=created;o.flags=3;p=original;p.push_back({2,Signature(primary,sub,14,0x18,o)});
  Check(Validate(Join(p))==Code::invalid_certificate,"equal-new conflicting binding refuses independent of packet order");
  std::swap(p[3],p[4]);Check(Validate(Join(p))==Code::invalid_certificate,"conflicting binding reversed order same refusal");
  p=original;o={};o.flags=12;p.push_back({2,Signature(primary,sub,14,0x18,o)});Check(Validate(Join(p))==Code::ok,"equal policy alternative signatures accepted");
  p=original;p.push_back({2,Signature(primary,sub,14,0x28)});Check(Validate(Join(p))==Code::no_eligible_key,"authenticated subkey revocation never ignored");
  p=original;p.insert(p.begin()+3,{2,Signature(primary,sub,14,0x28)});Check(Validate(Join(p))==Code::no_eligible_key,"subkey revocation before binding identical");
  p=original;p.push_back({2,Signature(primary,{},0,0x20)});Check(Validate(Join(p))==Code::revoked,"primary revocation observed after subkey block");
  p=original;p.insert(p.begin()+1,{2,Signature(primary,{},0,0x20)});Check(Validate(Join(p))==Code::revoked,"primary revocation before direct signature");
  p=original;p.insert(p.begin()+2,{2,Signature(primary,{},0,0x30)});Check(Validate(Join(p))==Code::invalid_signature,"direct certification revocation applies");
  o={};o.nonrevocable=true;p[1].body=Signature(primary,{},0,0x1f,o);Check(Validate(Join(p))==Code::ok,"authenticated non-revocable direct certification");
  p=original;p.push_back(original[2]);p.push_back(original[3]);Check(Validate(Join(p))==Code::invalid_certificate,"duplicate subkey cannot reset retained revocation/binding");
  for(unsigned type:{13u,17u}) {
    const Bytes id=type==13?Bytes{'a','l','i','a','s'}:Bytes{2,100,42};
    p={original[0],original[1],{type,id},{2,Signature(primary,id,type,0x13)},original[2],original[3]};
    Check(Validate(Join(p))==Code::ok,"UID/user-attribute certificates do not become engine principal identity");
    p.insert(p.begin()+3,{2,Signature(primary,id,type,0x30)});Check(Validate(Join(p))==Code::ok,"revoked human label does not revoke actual primary/subkey");
  }
  p=original;p.insert(p.begin()+2,{17,Bytes{9,100,1}});Check(Validate(Join(p))==Code::invalid_certificate,"truncated attribute subpacket framing");
  p=original;p.insert(p.begin()+1,{10,Bytes{'P','G','P'}});p.push_back({21,Bytes{1,2,3}});Check(Validate(Join(p))==Code::ok,"marker and final padding preserve complete certificate");
  p.push_back({13,Bytes{'x'}});Check(Validate(Join(p))==Code::invalid_certificate,"semantic packet cannot follow final padding");
  p=original;p.push_back({40,Bytes{1}});Check(Validate(Join(p))==Code::ok,"noncritical packet ignored without authority");
  p=original;p.push_back({22,Bytes{1}});Check(Validate(Join(p))==Code::invalid_certificate,"unknown critical packet refused");
  // Newer actual subkey creation wins; canonical fingerprint order breaks ties.
  auto newer=sub;newer[4]++;p=original;p.push_back({14,newer});o={};o.time=created+1;p.push_back({2,Signature(primary,newer,14,0x18,o)});
  auto joined=Join(p);auto selected=c::ValidateNativeKeyCertificate(In(joined),Encrypt(),limits);
  Check(selected.code==Code::ok && selected.creation_seconds==created+1,"actual newest eligible subkey selected");
  auto tied=sub;tied[10]^=1;p=original;p.push_back({14,tied});p.push_back({2,Signature(primary,tied,14,0x18)});
  auto expected=std::min(Fingerprint(sub),Fingerprint(tied));joined=Join(p);selected=c::ValidateNativeKeyCertificate(In(joined),Encrypt(),limits);
  Check(selected.code==Code::ok && std::equal(expected.begin(),expected.end(),selected.recipient_fingerprint.begin()),"equal creation chooses canonical lowest binary fingerprint");
  std::swap(p[2],p[4]);std::swap(p[3],p[5]);joined=Join(p);selected=c::ValidateNativeKeyCertificate(In(joined),Encrypt(),limits);
  Check(selected.code==Code::ok && std::equal(expected.begin(),expected.end(),selected.recipient_fingerprint.begin()),"tie selection independent of key packet order");
}
void SignatureValidityAndRevocation() {
  const auto original=Split(PublicFixture());const auto primary=original[0].body,sub=original[2].body;
  // A signature's expiration invalidates that signature, not every older valid
  // signature. Key expiration and flags are policy of a valid signature and
  // MUST NOT receive this fallback treatment.
  for(bool direct:{false,true}) for(bool newer_first:{false,true}) {
    SignatureOptions o;o.time=created+1;o.signature_expiry=1;
    const auto newer=Signature(primary,direct?Bytes{}:sub,direct?0:14,direct?0x1f:0x18,o);
    auto p=original;const std::size_t at=direct?1:3;
    p.insert(p.begin()+at+(newer_first?0:1),{2,newer});
    Check(Validate(Join(p))==Code::ok,"newer signature-expired direct/binding cannot suppress older still-valid signature in either order");
    p=original;p[at].body=newer;
    Check(Validate(Join(p))==(direct?Code::expired:Code::no_eligible_key),"only expired self-signature retains exact primary-expired or no-eligible result");
    o.signature_expiry=0;o.key_expiry=10;const auto key_expired=Signature(primary,direct?Bytes{}:sub,direct?0:14,direct?0x1f:0x18,o);
    p=original;p.insert(p.begin()+at+(newer_first?0:1),{2,key_expired});
    Check(Validate(Join(p))==(direct?Code::expired:Code::no_eligible_key),"newer VALID key-expiration policy never falls back to old policy");
    if(!direct) {
      auto secret=Split(SecretFixture());secret[3].body=newer;const auto fp=Fingerprint(secret[2].body);
      Check(Validate(Join(secret),Decrypt(fp))==Code::expired,"exact recipient with only signature-expired binding reports expired");
      secret=Split(SecretFixture());secret.push_back({2,key_expired});
      Check(Validate(Join(secret),Decrypt(fp))==Code::expired,"exact recipient newer valid key-expiration policy reports expired without fallback");
    }
  }
  const auto revoked=Signature(primary,{},0,0x30);
  for(unsigned position:{1u,2u,4u}) {
    auto p=original;p.insert(p.begin()+position,{2,revoked});
    Check(Validate(Join(p))==Code::invalid_signature,"primary certification revocation applies before/after direct and after subkey");
    SignatureOptions nonrevocable;nonrevocable.nonrevocable=true;
    // Locate the actual direct signature after insertion, not an index that
    // accidentally replaced the revocation when its packet order changes.
    p[position<=1?2:1].body=Signature(primary,{},0,0x1f,nonrevocable);
    Check(Validate(Join(p))==Code::ok,"nonrevocable direct certification survives primary-target revocation in every placement");
  }
  SignatureOptions renewed;renewed.time=created+1;
  auto p=original;p.push_back({2,revoked});p.push_back({2,Signature(primary,{},0,0x1f,renewed)});
  Check(Validate(Join(p))==Code::ok,"newer valid direct self-certification after prior certification revocation");
  std::swap(p[4],p[5]);Check(Validate(Join(p))==Code::ok,"renewed direct policy independent of revocation packet order");
  for(unsigned type:{13u,17u}) {
    const Bytes label=type==13?Bytes{'n','a','m','e'}:Bytes{2,100,42};
    const auto label_revoked=Signature(primary,label,type,0x30);
    p={original[0],original[1],{type,label},{2,Signature(primary,label,type,0x13)},original[2],original[3],{2,revoked}};
    Check(Validate(Join(p))==Code::invalid_signature,"primary-target certification revocation after UID and subkey is not misbound to label");
    p.back().body=label_revoked;
    Check(Validate(Join(p))==Code::ok,"UID/attribute-target revocation after subkey is not misbound to primary");
    p.insert(p.begin()+2,{type,label});
    Check(Validate(Join(p))==Code::ok,"duplicate identical human-label target is one signed target, not ambiguous key identity");
    p.back().body.back()^=1;Check(Validate(Join(p))==Code::ok,"forged label revocation never suppresses independent valid key policy");
  }
  // The conflicting flags occur inside a single authenticated signature, not
  // merely in two alternative binding packets. It must not last-field-win.
  SignatureOptions conflict;conflict.duplicate_conflict=true;
  p=original;p[3].body=Signature(primary,sub,14,0x18,conflict);
  Check(Validate(Join(p))==Code::invalid_signature,"conflicting duplicate authenticated security subpackets invalidate signature");
  p.push_back(original[3]);Check(Validate(Join(p))==Code::ok,"ambiguous signature cannot suppress separate unambiguous valid binding");
}
Bytes LockedBody(unsigned aead) {
  // Independent installed libargon2.so.1 argon2id_hash_raw(version0x13)
  // t=1,m=8KiB,p=1, salt=000102...0f, passphrase below, output32:
  // d17ea6341ca93da6079ea2f64dc4aa31dd1aaf9caa67fb42ac4afd0714706f26
  // Python cryptography HKDF(SHA256,empty salt,info=c70609{02|03}),
  // AESOCB3/AESGCM encrypt exact RFC A.4 private scalar with AAD=c7||public42.
  // Nonce=101112...1e (OCB)/...1b (GCM). These pinned outputs were derived
  // outside the product, not by Seal/Unlock or implementation helpers.
  return Hex(aead==2?
    "0663877fe319000000208693248367f9e5015db922f8f48095dda784987f2d5985b12fbad16caf5e4435fd2609021404000102030405060708090a0b0c0d0e0f010103101112131415161718191a1b1c1d1ec2661aa51eb910b5fb4e64c2f885bbb2a0d79da12810b73f449d20a1d2817aac80850648eae404ffeecded9b0e92460e":
    "0663877fe319000000208693248367f9e5015db922f8f48095dda784987f2d5985b12fbad16caf5e4435fd2309031404000102030405060708090a0b0c0d0e0f010103101112131415161718191a1bfddc4800beb03db35e0633abc6200519e033ab20e4b95bb895ba4cec3157c14c1e13e5b93b22661ae39b2c4551d73998");
}
constexpr std::string_view password="correct horse battery staple";
c::PgpInput Pass(){return {reinterpret_cast<const unsigned char*>(password.data()),password.size()};}
void EncryptedSecrets(Secrets& sec) {
  const auto expected=Hex("4d600a4f794d44775c57a26e0feefed558e9afffd6ad0d582d57fb2ba2dcedb8");
  for(unsigned aead:{2u,3u}) {
    auto parts=Split(SecretFixture());parts[2].body=LockedBody(aead);auto certificate=Join(parts);const auto fp=Fingerprint(parts[2].body);
    const auto query=Decrypt(fp);const auto desc=c::InspectNativeSecretKey(7,In(parts[2].body));
    Check(desc.code==Code::ok && desc.encrypted && desc.cipher==9 && desc.aead==aead && desc.workspace_bytes==8192 &&
      desc.argon2.memory_kib==8 && desc.argon2.passes==1 && desc.argon2.lanes==1,"exact independent encrypted-key KDF query (not a resource grant)");
    sec.Dirty();deny_cpp=true;auto result=c::UnlockNativeCertificateKey(In(certificate),query,limits,Pass(),sec.Work(),sec.Out());deny_cpp=false;
    Check(result==Code::ok && std::memcmp(sec.Out().data,expected.data(),32)==0 && Filled({sec.Work().data,sec.Work().size},0),"independent authenticated OCB/GCM scalar unlock, public match, exact workspace erasure");
    sec.Dirty();Check(c::UnlockNativeCertificateKey(In(certificate),query,limits,std::nullopt,sec.Work(),sec.Out())==Code::passphrase_required &&
      Filled({sec.Out().data,32},0) && Filled({sec.Work().data,sec.Work().size},0),"missing passphrase not silently guessed");
    sec.Dirty();Check(c::UnlockNativeCertificateKey(In(certificate),query,limits,c::PgpInput{},sec.Work(),sec.Out())==Code::authentication_failed &&
      Filled({sec.Out().data,32},0) && Filled({sec.Work().data,sec.Work().size},0),"explicit empty passphrase authenticates rather than bypasses");
    for(auto index:{parts[2].body.size()-1,parts[2].body.size()-17,std::size_t{67}}) {
      auto bad=parts;bad[2].body[index]^=1;auto wire=Join(bad);sec.Dirty();
      Check(c::UnlockNativeCertificateKey(In(wire),query,limits,Pass(),sec.Work(),sec.Out())==Code::authentication_failed &&
        Filled({sec.Out().data,32},0) && Filled({sec.Work().data,sec.Work().size},0),"tag/ciphertext/nonce modification rejects and scrubs accepted staging");
    }
    auto changed=parts;changed[2].body[4]++;auto changed_pub=Public(changed[2].body);SignatureOptions o;o.time=created+1;
    changed[3].body=Signature(Public(parts[0].body),changed_pub,14,0x18,o);auto wire=Join(changed);auto changed_fp=Fingerprint(changed[2].body);
    sec.Dirty();Check(c::UnlockNativeCertificateKey(In(wire),Decrypt(changed_fp),limits,Pass(),sec.Work(),sec.Out())==Code::authentication_failed &&
      Filled({sec.Out().data,32},0),"validly re-bound public AAD modification cannot authenticate retained encrypted private value");
    sec.Dirty();auto short_work=sec.Work();--short_work.size;
    Check(c::UnlockNativeCertificateKey(In(certificate),query,limits,Pass(),short_work,sec.Out())==Code::invalid_extent &&
      Filled({sec.Out().data,32},0xa5) && Filled({sec.Work().data,sec.Work().size},0xa5),"workspace mismatch untouched before acceptance");
    if(SIZE_MAX>UINT32_MAX) {
      sec.Dirty();const c::PgpInput oversized{Pass().data,static_cast<std::size_t>(UINT32_MAX)+1};
      Check(c::UnlockNativeCertificateKey(In(certificate),query,limits,oversized,sec.Work(),sec.Out())==Code::invalid_extent &&
        Filled({sec.Out().data,32},0xa5) && Filled({sec.Work().data,sec.Work().size},0xa5),
        "oversized claimed passphrase refused before dereference/admission with exact invalid extent");
    }
    auto wrong=fp;wrong[0]^=1;sec.Dirty();Check(c::UnlockNativeCertificateKey(In(certificate),Decrypt(wrong),limits,Pass(),sec.Work(),sec.Out())==Code::recipient_not_found &&
      Filled({sec.Out().data,32},0xa5),"exact binary recipient fingerprint, no fallback key");
    auto alias=sec.Out();alias.data=sec.Work().data;sec.Dirty();Check(c::UnlockNativeCertificateKey(In(certificate),query,limits,Pass(),sec.Work(),alias)==Code::invalid_extent &&
      Filled({sec.Work().data,sec.Work().size},0xa5),"secret/work alias rejected before effects");
  }
  // RFC9580 A.5 intentionally uses m=2^21 KiB (2GiB). Inspect exact work;
  // never lower its KDF, allocate it implicitly, or claim qualified unlock.
  const auto huge=Hex("0663877fe319000000208693248367f9e5015db922f8f48095dda784987f2d5985b12fbad16caf5e4435fd26090214040e61846829da869abe0ea61545dc14cc0104152e13ca9ff4e724fb1c2eb1df86020941af92d5cabfe5ba9df1c50bc75c54a38c0da16977e837ac5419174310723afae7af2a3551bf9a00c60bbb789a5a5cd8");
  const auto desc=c::InspectNativeSecretKey(7,In(huge));
  Check(desc.code==Code::ok && desc.workspace_bytes==2147483648ULL && desc.argon2.lanes==4 && desc.argon2.passes==1,
    "unmodified RFC A.5 subkey 2GiB exact inspect-only, no hidden allocation/downscale");
  for(unsigned i:{42u,43u,44u,45u,46u,47u,64u,65u,66u}) {
    auto bad=LockedBody(2);bad[i]=0;Check(c::InspectNativeSecretKey(7,In(bad)).code!=Code::ok,"malformed secret protection parameters refused");
  }
}
void IntegratedCertificateMessage(Secrets& sec) {
  // The actual certificate/PKESK/private-unlock/message adapters share the
  // selected native body/fingerprint, not an invented raw-key success route.
  const auto pub=PublicFixture();const auto selected=c::ValidateNativeKeyCertificate(In(pub),Encrypt(),limits);
  Check(selected.code==Code::ok,"integrated full transferable public certificate admission mechanism");
  auto parts=Split(SecretFixture());parts[2].body=LockedBody(2);const auto secret_certificate=Join(parts);
  const Bytes data{0,255,1,2,0,3,0x80,0x40,0x7f};
  const c::SeipdProfile profile{9,2,10};const auto size=c::X25519LiteralMessageEncodedSize(profile,data.size());
  Check(size.code==c::PgpCode::ok && size.literal_bytes<64 && size.message_bytes<384,"integrated exact mechanism sizes");
  auto* storage=static_cast<unsigned char*>(sec.secret.data());
  const auto region=[&](std::size_t at,std::size_t bytes)->c::PgpOutput{return {storage+at,bytes};};
  const auto entropy=[&](std::size_t at,unsigned start)->c::PgpInput{
    for(unsigned i=0;i<32;++i)storage[at+i]=static_cast<unsigned char>(start+i);return {storage+at,32};};
  const auto session=entropy(1536,0),ephemeral=entropy(1568,32),salt=entropy(1600,64);
  const auto message=region(128,size.message_bytes),literal=region(64,size.literal_bytes);
  Check(c::EncryptX25519LiteralMessage(profile,selected.public_body,In(data),{session,ephemeral,salt},literal,message)==c::PgpCode::ok &&
      Filled({literal.data,literal.size},0),"actual certificate-selected recipient encrypts authenticated v2SEIPD literal");
  const auto inspected=c::InspectX25519LiteralMessage({message.data,message.size});
  Check(inspected.code==c::PgpCode::ok && inspected.recipient.recipient_fingerprint==selected.recipient_fingerprint &&
      inspected.data_body_bytes<256,"actual PKESK carries exact selected binary fingerprint");
  const Bytes recipient(inspected.recipient.recipient_fingerprint.begin(),inspected.recipient.recipient_fingerprint.end());
  const auto query=Decrypt(recipient);unsigned decrypt_attempts=0;
  auto wrong=parts;wrong[2].body[4]++;SignatureOptions options;options.time=created+1;
  wrong[3].body=Signature(Public(wrong[0].body),Public(wrong[2].body),14,0x18,options);const auto wrong_certificate=Join(wrong);
  const auto wrong_selection=c::ValidateNativeKeyCertificate(In(wrong_certificate),query,limits);
  Check(wrong_selection.code==Code::recipient_not_found,"different authentic certificate cannot substitute message recipient");
  if(wrong_selection.code==Code::ok)++decrypt_attempts;
  Check(decrypt_attempts==0,"failed exact certificate selection does not enter decryption");
  std::memset(sec.Out().data,0xa5,32);std::memset(sec.Work().data,0xa5,sec.Work().size);
  const auto bad_unlock=c::UnlockNativeCertificateKey(In(secret_certificate),query,limits,c::PgpInput{},sec.Work(),sec.Out());
  if(bad_unlock==Code::ok)++decrypt_attempts;
  Check(bad_unlock==Code::authentication_failed && decrypt_attempts==0 && Filled({sec.Out().data,32},0),
      "failed locked certificate unlock does not enter message decrypt or publish private material");
  const auto usable=c::ValidateNativeKeyCertificate(In(secret_certificate),query,limits);
  Check(usable.code==Code::ok && c::UnlockNativeCertificateKey(In(secret_certificate),query,limits,Pass(),sec.Work(),sec.Out())==Code::ok,
      "actual locked full secret certificate revalidated and authenticated before private message use");
  const auto body=region(512,inspected.data_body_bytes),plain=region(1024,inspected.data_body_bytes);
  ++decrypt_attempts;
  const auto result=c::DecryptX25519LiteralMessage(usable.public_body,{sec.Out().data,32},{message.data,message.size},body,plain);
  Check(decrypt_attempts==1 && result.code==c::PgpCode::ok && result.data.size==data.size() &&
      std::memcmp(result.data.data,data.data(),data.size())==0 && Filled({body.data,body.size},0),
      "certificate selection -> actual PKESK -> locked private unlock -> full authenticated exact bytes");
  // The enclosing runtime, not these mechanisms, owns permission, publication,
  // key-use fences and result release. Retain/erase actual protected copies.
  OPENSSL_cleanse(storage,sec.secret.size());
}
struct Cancel {unsigned at=0,calls=0;bool throws=false;};
bool CancelProbe(void* p) {auto& v=*static_cast<Cancel*>(p);if(++v.calls!=v.at)return false;
  if(v.throws)throw std::runtime_error("owned cancellation exception");return true;}
void Failures(Secrets& sec) {
  auto parts=Split(SecretFixture());parts[2].body=LockedBody(3);const auto wire=Join(parts),fp=Fingerprint(parts[2].body);
  const auto query=Decrypt(fp);
  for(unsigned kind=1;kind<=12;++kind) for(unsigned nth=1;nth<=64;++nth) {
    sec.Dirty();fault_kind=kind;fault_at=nth;fault_calls=0;
    const auto code=c::UnlockNativeCertificateKey(In(wire),query,limits,Pass(),sec.Work(),sec.Out());
    const auto calls=fault_calls;fault_kind=0;
    if(calls<nth){Check(code==Code::ok,"fault sweep exhausted actual provider calls");break;}
    Check(code!=Code::ok,"every reached provider failure prevents secret publication");
    Check(Filled({sec.Out().data,32},0)||Filled({sec.Out().data,32},0xa5),"provider fault leaves only unchanged or erased private staging");
    Check(Filled({sec.Work().data,sec.Work().size},0)||Filled({sec.Work().data,sec.Work().size},0xa5),"provider fault never retains dirty KDF workspace");
    Check(nth<64,"provider fault sweep reaches all bounded calls");
  }
  for(bool throws:{false,true}) for(unsigned nth=1;nth<=1024;++nth) {
    Cancel cancel{nth,0,throws};sec.Dirty();Code code=Code::provider_failure;bool caught=false;
    try {code=c::UnlockNativeCertificateKey(In(wire),query,limits,Pass(),sec.Work(),sec.Out(),{CancelProbe,&cancel});}
    catch(const std::runtime_error&){caught=true;}
    if(cancel.calls<nth){Check(!caught && code==Code::ok,"cancellation sweep exhausted real safe points");break;}
    Check(throws?caught:code==Code::cancelled,"every reached cancellation/exception preserves exact control outcome");
    Check(Filled({sec.Out().data,32},0)||Filled({sec.Out().data,32},0xa5),"cancellation/exception never releases partial secret");
    Check(Filled({sec.Work().data,sec.Work().size},0)||Filled({sec.Work().data,sec.Work().size},0xa5),"cancellation/exception KDF workspace erased after acceptance");
    Check(nth<1024,"bounded cancellation sweep completed");
  }
}
int main() {
  Secrets sec;
  {
    h::CryptoMemoryScope scope(sec.pool,sec.binding);Check(scope.ok(),"actual explicit binary-bound provider scope");
    PublishedCertificates(sec);PolicyCertificates();SignatureValidityAndRevocation();EncryptedSecrets(sec);IntegratedCertificateMessage(sec);Failures(sec);
  }
  sec.Drain();std::cout<<"openpgp_key_certificate "<<checks<<" PASS\n";
}
