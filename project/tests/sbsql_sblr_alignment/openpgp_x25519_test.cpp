// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "openpgp_x25519.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace c=scratchbird::core::crypto;
using Bytes=std::vector<std::uint8_t>;
using Code=c::PgpCode;
unsigned checks=0;
bool deny_heap=false;
unsigned unexpected_heap=0;
void* operator new(std::size_t n){if(deny_heap){++unexpected_heap;throw std::bad_alloc();}if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
c::PgpInput In(const Bytes& b){return {b.data(),b.size()};}
c::PgpOutput Out(Bytes& b){return {b.data(),b.size()};}
bool Zero(const Bytes& b){return std::all_of(b.begin(),b.end(),[](auto x){return x==0;});}
Bytes Hex(std::string_view text){
  Check(text.size()%2==0,"oracle hex size");Bytes out;
  const auto nibble=[](char x){return x<='9'?x-'0':x-'a'+10;};
  for(std::size_t at=0;at<text.size();at+=2)out.push_back(nibble(text[at])*16+nibble(text[at+1]));return out;
}

#ifdef SB_X25519_PROVIDER_FAULTS
unsigned failure_kind=0,failure_at=0,provider_calls=0;
unsigned fault_mode=0; //0 refusal,1 incorrect returned length,2 all-zero shared.
bool observe_cleanup=false,scrub_failed=false;
unsigned scrub_calls=0,live_keys=0,live_key_contexts=0,live_digest_contexts=0,live_cipher_contexts=0;
void Track(unsigned kind,const void* p){if(!observe_cleanup||!p)return;
  if(kind==1)++live_digest_contexts;else if(kind==5||kind==7)++live_keys;
  else if(kind==8||kind==12)++live_key_contexts;else if(kind==17)++live_cipher_contexts;}
bool Fail(unsigned kind){return kind==failure_kind&&++provider_calls==failure_at;}
extern "C" {
#define WRAP_POINTER(name,kind,type,args,call) \
  type* __real_##name args; type* __wrap_##name args {auto* p=Fail(kind)?nullptr:__real_##name call;Track(kind,p);return p;}
#define WRAP_INT(name,kind,args,call) \
  int __real_##name args; int __wrap_##name args {return Fail(kind)?0:__real_##name call;}
WRAP_POINTER(EVP_MD_CTX_new,1,EVP_MD_CTX,(),())
WRAP_INT(EVP_DigestInit_ex,2,(EVP_MD_CTX* a,const EVP_MD* b,ENGINE* d),(a,b,d))
WRAP_INT(EVP_DigestUpdate,3,(EVP_MD_CTX* a,const void* b,std::size_t n),(a,b,n))
int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned*);
int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* a,unsigned char* out,unsigned* n){
  if(Fail(4)){out[0]=0xcc;if(fault_mode){*n=31;return 1;}return 0;}return __real_EVP_DigestFinal_ex(a,out,n);}
WRAP_POINTER(EVP_PKEY_new_raw_private_key,5,EVP_PKEY,(int a,ENGINE* b,const unsigned char* d,std::size_t n),(a,b,d,n))
int __real_EVP_PKEY_get_raw_public_key(const EVP_PKEY*,unsigned char*,std::size_t*);
int __wrap_EVP_PKEY_get_raw_public_key(const EVP_PKEY* a,unsigned char* out,std::size_t* n){
  if(Fail(6)){out[0]=0xcc;if(fault_mode){*n=31;return 1;}return 0;}return __real_EVP_PKEY_get_raw_public_key(a,out,n);}
WRAP_POINTER(EVP_PKEY_new_raw_public_key,7,EVP_PKEY,(int a,ENGINE* b,const unsigned char* d,std::size_t n),(a,b,d,n))
WRAP_POINTER(EVP_PKEY_CTX_new,8,EVP_PKEY_CTX,(EVP_PKEY* a,ENGINE* b),(a,b))
WRAP_INT(EVP_PKEY_derive_init,9,(EVP_PKEY_CTX* a),(a))
WRAP_INT(EVP_PKEY_derive_set_peer,10,(EVP_PKEY_CTX* a,EVP_PKEY* b),(a,b))
int __real_EVP_PKEY_derive(EVP_PKEY_CTX*,unsigned char*,std::size_t*);
int __wrap_EVP_PKEY_derive(EVP_PKEY_CTX* a,unsigned char* out,std::size_t* n){
  if(Fail(11)){if(fault_mode==2){std::fill_n(out,*n,0);return 1;}out[0]=0xcc;if(fault_mode){--*n;return 1;}return 0;}
  return __real_EVP_PKEY_derive(a,out,n);}
WRAP_POINTER(EVP_PKEY_CTX_new_id,12,EVP_PKEY_CTX,(int a,ENGINE* b),(a,b))
WRAP_INT(EVP_PKEY_CTX_set_hkdf_md,13,(EVP_PKEY_CTX* a,const EVP_MD* b),(a,b))
WRAP_INT(EVP_PKEY_CTX_set1_hkdf_salt,14,(EVP_PKEY_CTX* a,const unsigned char* b,int n),(a,b,n))
WRAP_INT(EVP_PKEY_CTX_set1_hkdf_key,15,(EVP_PKEY_CTX* a,const unsigned char* b,int n),(a,b,n))
WRAP_INT(EVP_PKEY_CTX_add1_hkdf_info,16,(EVP_PKEY_CTX* a,const unsigned char* b,int n),(a,b,n))
WRAP_POINTER(EVP_CIPHER_CTX_new,17,EVP_CIPHER_CTX,(),())
WRAP_INT(EVP_CipherInit_ex,18,(EVP_CIPHER_CTX* a,const EVP_CIPHER* b,ENGINE* d,const unsigned char* e,const unsigned char* f,int g),(a,b,d,e,f,g))
int __real_EVP_CipherUpdate(EVP_CIPHER_CTX*,unsigned char*,int*,const unsigned char*,int);
int __wrap_EVP_CipherUpdate(EVP_CIPHER_CTX* a,unsigned char* out,int* n,const unsigned char* b,int size){
  if(Fail(19)){if(out)out[0]=0xcc;if(fault_mode){*n=-1;return 1;}return 0;}return __real_EVP_CipherUpdate(a,out,n,b,size);}
int __real_EVP_CipherFinal_ex(EVP_CIPHER_CTX*,unsigned char*,int*);
int __wrap_EVP_CipherFinal_ex(EVP_CIPHER_CTX* a,unsigned char* out,int* n){
  if(Fail(20)){out[0]=0xcc;if(fault_mode){*n=1;return 1;}return 0;}return __real_EVP_CipherFinal_ex(a,out,n);}
WRAP_POINTER(EVP_sha256,21,const EVP_MD,(),())
WRAP_POINTER(EVP_aes_128_wrap,22,const EVP_CIPHER,(),())
void __real_EVP_PKEY_free(EVP_PKEY*);
void __wrap_EVP_PKEY_free(EVP_PKEY* p){if(observe_cleanup&&p)--live_keys;__real_EVP_PKEY_free(p);}
void __real_EVP_PKEY_CTX_free(EVP_PKEY_CTX*);
void __wrap_EVP_PKEY_CTX_free(EVP_PKEY_CTX* p){if(observe_cleanup&&p)--live_key_contexts;__real_EVP_PKEY_CTX_free(p);}
void __real_EVP_MD_CTX_free(EVP_MD_CTX*);
void __wrap_EVP_MD_CTX_free(EVP_MD_CTX* p){if(observe_cleanup&&p)--live_digest_contexts;__real_EVP_MD_CTX_free(p);}
void __real_EVP_CIPHER_CTX_free(EVP_CIPHER_CTX*);
void __wrap_EVP_CIPHER_CTX_free(EVP_CIPHER_CTX* p){if(observe_cleanup&&p)--live_cipher_contexts;__real_EVP_CIPHER_CTX_free(p);}
void __real_OPENSSL_cleanse(void*,std::size_t);
void __wrap_OPENSSL_cleanse(void* p,std::size_t size){__real_OPENSSL_cleanse(p,size);if(observe_cleanup){++scrub_calls;
  const auto* bytes=static_cast<unsigned char*>(p);for(std::size_t i=0;i<size;++i)if(bytes[i])scrub_failed=true;}}
#undef WRAP_POINTER
#undef WRAP_INT
}
#endif

// Published RFC9580 A.3/A.4/A.8 values, not generated by the subject codec.
const Bytes public_body=Hex("0663877fe319000000208693248367f9e5015db922f8f48095dda784987f2d5985b12fbad16caf5e4435");
const Bytes private_key=Hex("4d600a4f794d44775c57a26e0feefed558e9afffd6ad0d582d57fb2ba2dcedb8");
const Bytes ephemeral_secret=Hex("af1e43c0d123efe893a7d4d390f3a761e3fac33dfc7f3edaa830c9011352c779");
const Bytes ephemeral_public=Hex("87cf18d5f1b53f817cce5a004cf393cc8958bddc065f25f84af509b17dd36764");
const Bytes fingerprint=Hex("12c83f1e706f6308fe151a417743a1f033790e93e9978488d1db378da9930885");
const Bytes shared=Hex("67e30e69cdc7bab2a2680d78aca46a2f8b6e2ae44d398bdc6f92c5ad4a492514");
const Bytes kek=Hex("f66dadcff64592239b254539b64ff607");
const Bytes session=Hex("dd708f6fa1ed65114d68d2343e7c2f1d");
const Bytes rfc_body=Hex("06210612c83f1e706f6308fe151a417743a1f033790e93e9978488d1db378da993088519"
    "87cf18d5f1b53f817cce5a004cf393cc8958bddc065f25f84af509b17dd3676418"
    "dea355437956617901e06957fbca8a6a47a5b5153e8d3ab7");
Bytes ReferenceWrap(const Bytes& wrapping,const Bytes& key){
  // Independent RFC3394 index-based six-round construction using ECB blocks,
  // NEVER the subject EVP AES-wrap routine. Pinned separately to RFC3394 4.1.
  std::unique_ptr<EVP_CIPHER_CTX,decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);
  Check(ctx&&EVP_EncryptInit_ex(ctx.get(),EVP_aes_128_ecb(),nullptr,wrapping.data(),nullptr)==1&&
      EVP_CIPHER_CTX_set_padding(ctx.get(),0)==1,"reference AES ECB");
  Bytes out(key.size()+8,0xa6);std::copy(key.begin(),key.end(),out.begin()+8);
  const auto n=key.size()/8;
  for(unsigned round=0;round<6;++round)for(std::size_t i=1;i<=n;++i){
    std::array<unsigned char,16> block{},encrypted{};
    std::copy_n(out.begin(),8,block.begin());std::copy_n(out.begin()+i*8,8,block.begin()+8);int written=0;
    Check(EVP_EncryptUpdate(ctx.get(),encrypted.data(),&written,block.data(),16)==1&&written==16,"reference AES block");
    std::copy_n(encrypted.begin(),8,out.begin());
    auto counter=n*round+i;for(unsigned b=0;b<8;++b){out[7-b]^=counter&255;counter>>=8;}
    std::copy_n(encrypted.begin()+8,8,out.begin()+i*8);
  }
  return out;
}
void KnownAnswers(){
  Check(ReferenceWrap(Hex("000102030405060708090a0b0c0d0e0f"),Hex("00112233445566778899aabbccddeeff"))==
      Hex("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5"),"RFC3394 independent wrap oracle known answer");
  Check(ReferenceWrap(kek,session)==Bytes(rfc_body.begin()+69,rfc_body.end()),"RFC9580 published wrapped session key");
  std::array<unsigned char,32> prk{},expanded{},zeros{};unsigned length=0;
  Bytes ikm=ephemeral_public;ikm.insert(ikm.end(),public_body.begin()+10,public_body.end());ikm.insert(ikm.end(),shared.begin(),shared.end());
  const Bytes info{'O','p','e','n','P','G','P',' ','X','2','5','5','1','9',1};
  Check(HMAC(EVP_sha256(),zeros.data(),32,ikm.data(),ikm.size(),prk.data(),&length)&&length==32&&
      HMAC(EVP_sha256(),prk.data(),32,info.data(),info.size(),expanded.data(),&length)&&length==32&&
      std::equal(kek.begin(),kek.end(),expanded.begin()),"independent HMAC extract/expand matches published RFC9580 KEK");
  Bytes hash_input{0x9b,0,0,0,42};hash_input.insert(hash_input.end(),public_body.begin(),public_body.end());
  std::array<unsigned char,32> direct_hash{};Check(SHA256(hash_input.data(),hash_input.size(),direct_hash.data())&&
      std::equal(fingerprint.begin(),fingerprint.end(),direct_hash.begin()),"published fingerprint hashes complete v6 public body");
  Bytes fp(32,0xa5);deny_heap=true;const auto fp_code=c::X25519PublicKeyFingerprintV6(In(public_body),Out(fp));deny_heap=false;
  Check(fp_code==Code::ok&&fp==fingerprint&&!unexpected_heap,"fingerprint exact bytes without C++ heap");
  for(unsigned size:{16u,24u,32u}){
    Bytes key=session;key.resize(size);for(unsigned i=16;i<size;++i)key[i]=i;
    auto expected=Bytes(rfc_body.begin(),rfc_body.begin()+69);expected[68]=size+8;
    const auto wrapped=ReferenceWrap(kek,key);expected.insert(expected.end(),wrapped.begin(),wrapped.end());
    const auto measure=c::X25519PkeskV6Size(size);Check(measure.code==Code::ok&&measure.bytes==size+77,"all AES sizes exact envelope");
    Bytes body(measure.bytes,0xa5);deny_heap=true;
    const auto encrypted=c::EncryptX25519PkeskV6(In(public_body),In(ephemeral_secret),In(key),Out(body));deny_heap=false;
    Check(encrypted==Code::ok&&body==expected&&!unexpected_heap,"all session lengths match independent frozen-input wrap oracle");
    if(size==16)Check(body==rfc_body,"RFC9580 A.8 complete PKESK body known answer");
    const auto inspected=c::InspectX25519PkeskV6(In(body));
    Check(inspected.code==Code::ok&&inspected.session_key_bytes==size&&
        std::equal(fingerprint.begin(),fingerprint.end(),inspected.recipient_fingerprint.begin())&&
        std::equal(ephemeral_public.begin(),ephemeral_public.end(),inspected.ephemeral_public_key.begin()),"inspection exact nonauthoritative recipient observation");
    Bytes recovered(size,0xa5);deny_heap=true;
    const auto decrypted=c::DecryptX25519PkeskV6(In(public_body),In(private_key),In(body),Out(recovered));deny_heap=false;
    Check(decrypted==Code::ok&&recovered==key&&!unexpected_heap,"RFC native byte order exact round trip for all AES lengths");
  }
}
void EmptyDescription(c::X25519PkeskDescription d){
  Check(d.code!=Code::ok&&!d.session_key_bytes&&
      std::all_of(d.recipient_fingerprint.begin(),d.recipient_fingerprint.end(),[](auto x){return !x;})&&
      std::all_of(d.ephemeral_public_key.begin(),d.ephemeral_public_key.end(),[](auto x){return !x;}),"refused inspection has no partial fields");
}
void Mutations(){
  for(std::size_t size=0;size<rfc_body.size();++size){
    EmptyDescription(c::InspectX25519PkeskV6({rfc_body.data(),size}));Bytes output(16,0xa5),saved=output;
    Check(c::DecryptX25519PkeskV6(In(public_body),In(private_key),{rfc_body.data(),size},Out(output))!=Code::ok&&output==saved,
        "every truncated PKESK rejected before output acceptance");
  }
  for(std::size_t at=0;at<rfc_body.size();++at)for(unsigned bit=0;bit<8;++bit){
    auto bad=rfc_body;bad[at]^=1u<<bit;Bytes output(16,0xa5);const auto inspected=c::InspectX25519PkeskV6(In(bad));
    const auto code=c::DecryptX25519PkeskV6(In(public_body),In(private_key),In(bad),Out(output));
    Check(code!=Code::ok,"every PKESK bit mutation refuses");
    if(inspected.code==Code::ok)Check(Zero(output),"fingerprint/ephemeral/wrapped-key mutation erases accepted output");
    else{EmptyDescription(inspected);Check(output==Bytes(16,0xa5),"malformed framing untouched");}
  }
  auto trailing=rfc_body;trailing.push_back(0);EmptyDescription(c::InspectX25519PkeskV6(In(trailing)));
  for(std::size_t size=0;size<public_body.size();++size){Bytes out(32,0xa5);
    Check(c::X25519PublicKeyFingerprintV6({public_body.data(),size},Out(out))!=Code::ok&&out==Bytes(32,0xa5),"every truncated public body untouched");}
  for(unsigned field:{0u,5u,6u,7u,8u,9u})for(unsigned bit=0;bit<8;++bit){
    auto bad=public_body;bad[field]^=1u<<bit;Bytes out(32,0xa5);
    Check(c::X25519PublicKeyFingerprintV6(In(bad),Out(out))==Code::invalid_packet&&out==Bytes(32,0xa5),"exact native key version/algorithm/material length");
  }
  for(unsigned at:{1u,2u,3u,4u,10u,31u,41u}){
    auto bad=public_body;bad[at]^=1;Bytes out(16,0xa5);
    Check(c::DecryptX25519PkeskV6(In(bad),In(private_key),In(rfc_body),Out(out))==Code::authentication_failed&&Zero(out),"fingerprint includes time and every key octet");
  }
  auto wrong=private_key;wrong[10]^=0x80;Bytes out(16,0xa5);
  Check(c::DecryptX25519PkeskV6(In(public_body),In(wrong),In(rfc_body),Out(out))==Code::authentication_failed&&Zero(out),"private key must derive exact declared recipient public material");
  // RFC7748 scalar decoding clamps these bits; they are not distinct secrets.
  auto equivalent=private_key;equivalent[0]^=7;equivalent[31]^=0xc0;
  Check(c::DecryptX25519PkeskV6(In(public_body),In(equivalent),In(rfc_body),Out(out))==Code::ok&&out==session,"native X25519 scalar clamp semantics");
  for(unsigned low_order:{0u,1u}){
    auto low=public_body;std::fill(low.begin()+10,low.end(),0);low[10]=low_order;Bytes body(rfc_body.size(),0xa5);
    Check(c::EncryptX25519PkeskV6(In(low),In(ephemeral_secret),In(session),Out(body))!=Code::ok&&Zero(body),"actual low-order recipient never produces a PKESK");
    auto bad=rfc_body;std::fill(bad.begin()+36,bad.begin()+68,0);bad[36]=low_order;out.assign(16,0xa5);
    Check(c::DecryptX25519PkeskV6(In(public_body),In(private_key),In(bad),Out(out))!=Code::ok&&Zero(out),"actual low-order ephemeral never produces a session key");
  }
}
void Extents(){
  for(std::size_t n:{0u,1u,15u,17u,23u,25u,31u,33u})Check(c::X25519PkeskV6Size(n).code==Code::invalid_extent,"non-AES lengths refused");
  const auto near=reinterpret_cast<const std::uint8_t*>(std::numeric_limits<std::uintptr_t>::max()-3);
  EmptyDescription(c::InspectX25519PkeskV6({near,93}));EmptyDescription(c::InspectX25519PkeskV6({nullptr,93}));
  for(bool decrypt:{false,true}){
    const auto count=decrypt?16:rfc_body.size();Bytes output(count,0xa5),unchanged=output;
    const auto run=[&](c::PgpInput pub,c::PgpInput secret,c::PgpInput data,c::PgpOutput dest){return decrypt?
        c::DecryptX25519PkeskV6(pub,secret,data,dest):c::EncryptX25519PkeskV6(pub,secret,data,dest);};
    const auto secret=decrypt?In(private_key):In(ephemeral_secret),data=decrypt?In(rfc_body):In(session);
    for(auto bad:{c::PgpInput{nullptr,42},c::PgpInput{near,42}})
      Check(run(bad,secret,data,Out(output))==Code::invalid_extent&&output==unchanged,"invalid public extent untouched");
    for(auto bad:{c::PgpInput{nullptr,32},c::PgpInput{near,32},c::PgpInput{secret.data,31}})
      Check(run(In(public_body),bad,data,Out(output))==Code::invalid_extent&&output==unchanged,"invalid private extent untouched");
    for(auto bad:{c::PgpInput{nullptr,data.size},c::PgpInput{near,data.size}})
      Check(run(In(public_body),secret,bad,Out(output))==Code::invalid_extent&&output==unchanged,"invalid data extent untouched");
    for(auto bad:{c::PgpOutput{nullptr,count},c::PgpOutput{const_cast<std::uint8_t*>(near),count},c::PgpOutput{output.data(),count-1}})
      Check(run(In(public_body),secret,data,bad)==Code::invalid_extent&&output==unchanged,"invalid output extent untouched");
    for(unsigned which=0;which<3;++which)for(unsigned offset:{0u,1u,15u})for(bool reverse:{false,true}){
      Bytes shared_buffer(256,0xa5);auto pub=In(public_body),priv=secret,source=data;
      const auto input=which==0?pub:which==1?priv:source;const auto input_at=reverse?0:offset,output_at=reverse?offset:0;
      std::copy_n(input.data,input.size,shared_buffer.data()+input_at);
      const c::PgpInput aliased{shared_buffer.data()+input_at,input.size};if(which==0)pub=aliased;else if(which==1)priv=aliased;else source=aliased;
      const auto saved=shared_buffer;
      Check(run(pub,priv,source,{shared_buffer.data()+output_at,count})==Code::invalid_extent&&shared_buffer==saved,"every direction of output/input overlap untouched");
    }
    for(unsigned which=0;which<3;++which)for(bool reverse:{false,true}){
      Bytes adjacent(256,0xa5);auto pub=In(public_body),priv=secret,source=data;
      const auto input=which==0?pub:which==1?priv:source;const auto input_at=reverse?count:0,output_at=reverse?0:input.size;
      std::copy_n(input.data,input.size,adjacent.data()+input_at);const Bytes saved(input.data,input.data+input.size);
      const c::PgpInput alias{adjacent.data()+input_at,input.size};if(which==0)pub=alias;else if(which==1)priv=alias;else source=alias;
      Check(run(pub,priv,source,{adjacent.data()+output_at,count})==Code::ok&&std::equal(saved.begin(),saved.end(),adjacent.begin()+input_at),
          "exactly adjacent extents remain disjoint and leave the borrowed input unchanged");
    }
  }
  Bytes shared_buffer(100,0xa5);std::copy(public_body.begin(),public_body.end(),shared_buffer.begin());const auto saved=shared_buffer;
  Check(c::X25519PublicKeyFingerprintV6({shared_buffer.data(),42},{shared_buffer.data()+1,32})==Code::invalid_extent&&shared_buffer==saved,"fingerprint overlap untouched");
}
struct ProbeException{};
struct Probe {
  unsigned calls=0,at=0;bool throwing=false,final_fence=false;
  static bool Invoke(void* p){auto& s=*static_cast<Probe*>(p);if(++s.calls!=s.at)return false;
#ifdef SB_X25519_PROVIDER_FAULTS
    if(s.final_fence)Check(!live_keys&&!live_key_contexts&&!live_digest_contexts&&!live_cipher_contexts&&!scrub_failed,
        "all actual backend key/context copies released and scratch erased before final fence");
#endif
    if(s.throwing)throw ProbeException{};return true;}
  c::PgpCancellation callback(){return {Invoke,this};}
};
void CancellationAndProviders(){
  for(unsigned operation=0;operation<3;++operation){
    Bytes output(operation==0?32:operation==1?rfc_body.size():16,0xa5);
    const auto run=[&](c::PgpCancellation probe={}){return operation==0?c::X25519PublicKeyFingerprintV6(In(public_body),Out(output),probe):
        operation==1?c::EncryptX25519PkeskV6(In(public_body),In(ephemeral_secret),In(session),Out(output),probe):
        c::DecryptX25519PkeskV6(In(public_body),In(private_key),In(rfc_body),Out(output),probe);};
    Probe baseline;Check(run(baseline.callback())==Code::ok&&baseline.calls>=2,"bounded cancellation probe baseline");
    for(bool throwing:{false,true})for(unsigned at=1;at<=baseline.calls;++at){
      Probe probe{0,at,throwing,at==baseline.calls};std::fill(output.begin(),output.end(),0xa5);bool threw=false;Code code=Code::ok;
#ifdef SB_X25519_PROVIDER_FAULTS
      observe_cleanup=true;scrub_calls=0;scrub_failed=false;
#endif
      try{code=run(probe.callback());}catch(const ProbeException&){threw=true;}
#ifdef SB_X25519_PROVIDER_FAULTS
      observe_cleanup=false;Check(!live_keys&&!live_key_contexts&&!live_digest_contexts&&!live_cipher_contexts&&scrub_calls&&!scrub_failed,
          "every cancelled/throwing path releases contexts and performs actual erasure");
#endif
      Check(probe.calls==at&&threw==throwing&&(throwing||code==Code::cancelled)&&Zero(output),"every cancellation/throw including final fence erases all output");
    }
#ifdef SB_X25519_PROVIDER_FAULTS
    for(unsigned kind=1;kind<=22;++kind){
      failure_kind=kind;failure_at=0;provider_calls=0;const auto initial=run();const auto count=provider_calls;failure_kind=0;
      Check(initial==Code::ok,"provider phase clean baseline");
      for(unsigned at=1;at<=count;++at){
        std::fill(output.begin(),output.end(),0xa5);failure_kind=kind;failure_at=at;provider_calls=0;
        const auto code=run();const auto calls=provider_calls;failure_kind=0;
        Check(code!=Code::ok&&calls==at&&Zero(output),"every actual provider call refusal erases partial output");
        Check(run()==Code::ok,"provider refusal does not poison independent retry");
      }
    }
    for(unsigned kind:{4u,6u,11u,19u,20u}){
      failure_kind=kind;failure_at=0;provider_calls=0;Check(run()==Code::ok,"returned-length baseline");const auto count=provider_calls;failure_kind=0;
      for(unsigned at=1;at<=count;++at){std::fill(output.begin(),output.end(),0xa5);failure_kind=kind;failure_at=at;provider_calls=0;fault_mode=1;
        const auto code=run();failure_kind=fault_mode=0;Check(code!=Code::ok&&Zero(output),"incorrect successful provider lengths cannot publish output");}
    }
    if(operation){std::fill(output.begin(),output.end(),0xa5);failure_kind=11;failure_at=1;provider_calls=0;fault_mode=2;
      const auto code=run();failure_kind=fault_mode=0;Check(code==Code::authentication_failed&&Zero(output),"explicit all-zero shared secret rejected even if provider reports success");}
#endif
  }
}
#ifndef SB_OPENPGP_X25519_FIXTURE_ONLY
int main()try{
  KnownAnswers();Mutations();Extents();CancellationAndProviders();
  std::cout<<"PASS X25519 native v6 PKESK "<<checks<<" checks; not certificate or runtime admission\n";return 0;
}catch(const std::exception& error){deny_heap=false;std::cerr<<"FAIL "<<error.what()<<" checks="<<checks<<'\n';return 1;}
#endif
