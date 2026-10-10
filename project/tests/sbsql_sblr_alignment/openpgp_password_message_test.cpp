// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "openpgp_password_message.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
namespace c=scratchbird::core::crypto;
using Code=c::PgpCode;
using Bytes=std::vector<std::uint8_t>;
unsigned checks=0;
#ifdef SB_MESSAGE_PROVIDER_FAULTS
unsigned fault_kind=0,fault_at=0,provider_calls=0;
bool Fail(unsigned kind){return fault_kind==kind && ++provider_calls==fault_at;}
extern "C" {
EVP_CIPHER_CTX* __real_EVP_CIPHER_CTX_new();
EVP_CIPHER_CTX* __wrap_EVP_CIPHER_CTX_new(){return Fail(1)?nullptr:__real_EVP_CIPHER_CTX_new();}
EVP_PKEY_CTX* __real_EVP_PKEY_CTX_new_id(int,ENGINE*);
EVP_PKEY_CTX* __wrap_EVP_PKEY_CTX_new_id(int id,ENGINE* engine){return Fail(2)?nullptr:__real_EVP_PKEY_CTX_new_id(id,engine);}
int __real_EVP_CipherFinal_ex(EVP_CIPHER_CTX*,unsigned char*,int*);
int __wrap_EVP_CipherFinal_ex(EVP_CIPHER_CTX* ctx,unsigned char* out,int* n){
  if(Fail(3)){out[0]=0xcc;return 0;}return __real_EVP_CipherFinal_ex(ctx,out,n);}
}
#endif
void Check(bool b,const char* why){++checks;if(!b)throw std::runtime_error(why);}
c::PgpInput In(const Bytes& v){return {v.data(),v.size()};}
c::PgpOutput Out(Bytes& v){return {v.data(),v.size()};}
bool Filled(const Bytes& v,unsigned n){return std::all_of(v.begin(),v.end(),[n](auto b){return b==n;});}
struct Work {
  std::vector<std::uint64_t> words;
  explicit Work(std::size_t n):words(n/8+2,UINT64_MAX){Check(n%8==0,"aligned workspace size");}
  c::PgpOutput out(){return {reinterpret_cast<std::uint8_t*>(words.data()+1),(words.size()-2)*8};}
  bool zero(){return words.front()==UINT64_MAX && words.back()==UINT64_MAX &&
    std::all_of(words.begin()+1,words.end()-1,[](auto n){return !n;});}
  void dirty(){std::fill(words.begin(),words.end(),UINT64_MAX);}
};
struct Fixture {
  c::PasswordMessageProfile profile{};
  Bytes password{'p',0,'w'},data,session,salt,nonce,data_salt;
  Fixture(std::size_t bytes=2000){profile.s2k={1,1,3};profile.data.chunk=0;
    data.resize(bytes);for(std::size_t i=0;i<bytes;++i)data[i]=i*71;
    session.resize(32,0x31);salt.resize(16,0x73);nonce.resize(15,0x25);data_salt.resize(32,0x52);}
  c::PasswordMessageEntropy entropy(){return {In(session),In(salt),In(nonce),In(data_salt)};}
  Bytes encode(c::PgpCancellation probe={}){
    const auto size=c::PasswordMessageEncodedSize(profile,data.size());Check(size.code==Code::ok,"message size query");
    Work work(size.workspace_bytes);Bytes literal(size.literal_bytes,0xa5),out(size.message_bytes,0xa5);
    Check(c::EncryptPasswordMessage(profile,In(password),In(data),entropy(),work.out(),Out(literal),Out(out),probe)==Code::ok,
      "complete native password message encryption");
    Check(work.zero()&&Filled(literal,0),"encryption erases all scratch");return out;
  }
  void decode(const Bytes& wire,const Bytes& expected){
    const auto info=c::InspectPasswordMessage(In(wire));Check(info.code==Code::ok,"message inspect");
    Work work(info.workspace_bytes);Bytes body(info.data_body_bytes,0xa5),plain(info.data_body_bytes,0xa5);
    const auto result=c::DecryptPasswordMessage(In(password),In(wire),work.out(),Out(body),Out(plain));
    Check(result.code==Code::ok && result.literal_format=='b' && result.data.data==plain.data() &&
      result.data.size==expected.size() && std::equal(expected.begin(),expected.end(),result.data.data),"complete message plaintext bytes");
    Check(work.zero()&&Filled(body,0)&&std::all_of(plain.begin()+expected.size(),plain.end(),[](auto n){return !n;}),
      "receiver key/work/body/unused plaintext cleanup");
  }
};
Bytes Frame(unsigned type,const Bytes& body){
  Bytes packet(c::PacketEncodedSize(type,body.size()).bytes);
  Check(c::EncodePacket(type,In(body),Out(packet))==Code::ok,"frame component oracle");return packet;
}
void CrossProfiles(){
  for(unsigned cipher=7;cipher<=9;++cipher)for(unsigned aead:{2u,3u})for(unsigned wrapping=7;wrapping<=9;++wrapping)
    for(std::size_t size:{0u,1u,63u,64u,65u,511u,512u,65536u}){
      Fixture f(size);f.profile.data.cipher=cipher;f.profile.data.aead=aead;
      f.profile.wrapping_cipher=wrapping;f.profile.wrapping_aead=aead==2?3:2;
      f.session.resize(16+(cipher-7)*8);f.nonce.resize(f.profile.wrapping_aead==2?15:12);
      const auto wire=f.encode();f.decode(wire,f.data);
      const auto first=c::InspectPacket(In(wire));
      Check(first.code==Code::ok&&first.type==3&&!first.partial,"SKESK framing");
      const c::PgpInput tail{wire.data()+first.packet_bytes,wire.size()-first.packet_bytes};
      const auto second=c::InspectPacket(tail);
      Check(second.code==Code::ok&&second.type==18&&second.packet_bytes==tail.size,"exact second packet and no trailing data");
      // Component decomposition independently checks the owner's complete packet
      // placement and byte boundaries; RFC/provider KATs remain in seipd_test.
      Bytes keybody(first.body_bytes);Work work(c::Argon2S2kWorkspaceSize(f.profile.s2k).bytes);
      Check(c::EncryptArgon2SkeskV6(f.profile.wrapping_cipher,f.profile.wrapping_aead,f.profile.s2k,
        In(f.password),In(f.salt),In(f.nonce),In(f.session),work.out(),Out(keybody))==Code::ok,"component SKESK oracle");
      Check(std::equal(keybody.begin(),keybody.end(),wire.data()+first.first_body_offset),"message exact independent key-body placement");
      Bytes literal(c::LiteralPacketEncodedSize(f.data.size()).bytes),body(second.body_bytes);
      Check(c::EncodeLiteralPacket(In(f.data),Out(literal))==Code::ok &&
        c::EncryptSeipdV2(f.profile.data,In(f.session),In(f.data_salt),In(literal),Out(body))==Code::ok,
        "component literal/container oracle");
      Check(std::equal(body.begin(),body.end(),tail.data+second.first_body_offset),"message exact complete container placement");
    }
  Fixture defaults(33);defaults.profile={};defaults.nonce.resize(15);
  const auto size=c::PasswordMessageEncodedSize(defaults.profile,defaults.data.size());
  Check(size.workspace_bytes==64*1024*1024,"approved default64MiB not silently reduced");
  defaults.decode(defaults.encode(),defaults.data);
}
void Tampering(){
  Fixture f(80);const auto wire=f.encode();const auto info=c::InspectPasswordMessage(In(wire));
  Work work(info.workspace_bytes);Bytes body(info.data_body_bytes,0xa5),plain(info.data_body_bytes,0xa5);
  for(std::size_t i=0;i<wire.size();++i){
    auto bad=wire;bad[i]^=1;const auto inspected=c::InspectPasswordMessage(In(bad));
    // A changed structural/resource profile requires fresh caller admission;
    // never allocate according to an attacker-upgraded work factor in this test.
    if(inspected.code==Code::ok && inspected.workspace_bytes!=info.workspace_bytes)continue;
    work.dirty();std::fill(body.begin(),body.end(),0xa5);std::fill(plain.begin(),plain.end(),0xa5);
    const auto result=c::DecryptPasswordMessage(In(f.password),In(bad),work.out(),Out(body),Out(plain));
    Check(result.code!=Code::ok&&!result.data.data&&!result.data.size,"all packet/key/salt/ciphertext/tag corruptions refuse");
    Check((Filled(plain,0)&&Filled(body,0)&&work.zero()) || (Filled(plain,0xa5)&&Filled(body,0xa5)),
      "pre-admission unchanged or accepted-stage fully erased");
  }
  for(std::size_t n=0;n<wire.size();++n)
    Check(c::InspectPasswordMessage({wire.data(),n}).code!=Code::ok,"every message truncation refused");
  auto extra=wire;extra.push_back(0);Check(c::InspectPasswordMessage(In(extra)).code!=Code::ok,"trailing bytes not ignored");
  auto wrong=f.password;wrong[0]^=1;
  Check(c::DecryptPasswordMessage(In(wrong),In(wire),work.out(),Out(body),Out(plain)).code==Code::authentication_failed&&
    Filled(plain,0)&&Filled(body,0)&&work.zero(),"wrong password publishes nothing");
}
Bytes ReencryptInner(Fixture& f,const Bytes& wire,const Bytes& inner){
  const auto key=c::InspectPacket(In(wire));Bytes result(wire.begin(),wire.begin()+key.packet_bytes);
  Bytes body(c::SeipdEncryptedSize(f.profile.data,inner.size()).bytes);
  Check(c::EncryptSeipdV2(f.profile.data,In(f.session),In(f.data_salt),In(inner),Out(body))==Code::ok,"authenticated inner grammar candidate");
  const auto packet=Frame(18,body);result.insert(result.end(),packet.begin(),packet.end());return result;
}
void InnerAndPartial(){
  Fixture f(2000);auto wire=f.encode();auto key=c::InspectPacket(In(wire));
  c::PgpInput tail{wire.data()+key.packet_bytes,wire.size()-key.packet_bytes};
  auto description=c::InspectPacket(tail);Bytes body(description.body_bytes);
  Check(c::CopyPacketBody(tail,Out(body))==Code::ok,"extract encrypted body");
  Bytes partial(wire.begin(),wire.begin()+key.packet_bytes);partial.insert(partial.end(),{0xd2,233});
  partial.insert(partial.end(),body.begin(),body.begin()+512);
  // Shortest final definite length independent of the encoder under test.
  const auto remaining=body.size()-512;Check(remaining>=192&&remaining<8384,"test two-byte tail");
  partial.push_back(((remaining-192)>>8)+192);partial.push_back(remaining-192);
  partial.insert(partial.end(),body.begin()+512,body.end());f.decode(partial,f.data);
  // Authenticated partial literal must also be reassembled, not read as raw data.
  Bytes literal_body{'b',0,0,0,0,0};literal_body.insert(literal_body.end(),f.data.begin(),f.data.end());
  Bytes inner{0xcb,233};inner.insert(inner.end(),literal_body.begin(),literal_body.begin()+512);
  const auto rest=literal_body.size()-512;inner.push_back(((rest-192)>>8)+192);inner.push_back(rest-192);
  inner.insert(inner.end(),literal_body.begin()+512,literal_body.end());
  f.decode(ReencryptInner(f,wire,inner),f.data);
  const Bytes valid_inner=Frame(11,literal_body);
  for(unsigned kind=0;kind<4;++kind){
    auto bad=valid_inner;
    if(kind==0)bad[0]=0xc8; // compressed data is not a literal
    if(kind==1)bad.push_back(0); // authenticated trailing bytes
    if(kind==2)bad[c::InspectPacket(In(bad)).first_body_offset]=0; // invalid literal format
    if(kind==3)bad.pop_back(); // authenticated inner length mismatch
    const auto encrypted=ReencryptInner(f,wire,bad);const auto info=c::InspectPasswordMessage(In(encrypted));
    Work work(info.workspace_bytes);Bytes scratch(info.data_body_bytes,0xa5),plain(info.data_body_bytes,0xa5);
    Check(c::DecryptPasswordMessage(In(f.password),In(encrypted),work.out(),Out(scratch),Out(plain)).code==Code::invalid_packet&&
      work.zero()&&Filled(scratch,0)&&Filled(plain,0),"valid AEAD never excuses malformed or unsupported inner grammar");
  }
}
struct Interrupted{};
struct Probe{
  unsigned calls=0,stop=0;bool throwing=false;
  static bool Poll(void* v){auto& p=*static_cast<Probe*>(v);if(++p.calls!=p.stop)return false;if(p.throwing)throw Interrupted{};return true;}
  c::PgpCancellation callback(){return {Poll,this};}
};
void Cancellation(){
  Fixture f(75);const auto wire=f.encode();const auto info=c::InspectPasswordMessage(In(wire));
  const auto size=c::PasswordMessageEncodedSize(f.profile,f.data.size());
  for(bool decrypt:{false,true}){
    Work work(info.workspace_bytes);Bytes scratch(decrypt?info.data_body_bytes:size.literal_bytes,0xa5);
    Bytes output(decrypt?info.data_body_bytes:size.message_bytes,0xa5);
    auto run=[&](Probe& p){return decrypt?c::DecryptPasswordMessage(In(f.password),In(wire),work.out(),Out(scratch),Out(output),p.callback()).code:
      c::EncryptPasswordMessage(f.profile,In(f.password),In(f.data),f.entropy(),work.out(),Out(scratch),Out(output),p.callback());};
    Probe baseline;Check(run(baseline)==Code::ok,"message cancellation baseline");
    Probe preflight;Check(c::InspectPasswordMessage(In(wire),preflight.callback()).code==Code::ok,"message preflight probe count");
    for(bool throwing:{false,true})for(unsigned stop=1;stop<=baseline.calls;++stop){
      work.dirty();std::fill(scratch.begin(),scratch.end(),0xa5);std::fill(output.begin(),output.end(),0xa5);
      Probe p{0,stop,throwing};bool threw=false;Code code=Code::ok;
      try{code=run(p);}catch(const Interrupted&){threw=true;}
      Check(throwing?threw:code==Code::cancelled,"every cancellation/exception returns no message or plaintext");
      const bool untouched=decrypt&&stop<=preflight.calls;
      Check(Filled(scratch,untouched?0xa5:0)&&Filled(output,untouched?0xa5:0)&&(untouched||work.zero()),
        "all accepted message scratch erased across cancellation/exception");
    }
  }
}
void Extents(){
  Check(c::PasswordMessageEncodedSize({},std::numeric_limits<std::size_t>::max()).code==Code::size_overflow,
    "message data plus literal/frame overhead cannot overflow");
  for(unsigned field=0;field<8;++field){c::PasswordMessageProfile p;
    if(field==0)p.data.cipher=0;
    if(field==1)p.data.aead=0;
    if(field==2)p.data.chunk=17;
    if(field==3)p.wrapping_cipher=0;
    if(field==4)p.wrapping_aead=0;
    if(field==5)p.s2k.passes=0;
    if(field==6)p.s2k.lanes=0;
    if(field==7)p.s2k.memory_exponent=32;
    Check(c::PasswordMessageEncodedSize(p,0).code==Code::invalid_profile,"invalid message profile never selects fallback");
  }
  Fixture f(10);const auto size=c::PasswordMessageEncodedSize(f.profile,f.data.size());
  Work work(size.workspace_bytes);Bytes literal(size.literal_bytes,0xa5),message(size.message_bytes,0xa5);
  auto run=[&](c::PgpInput password,c::PasswordMessageEntropy entropy,c::PgpOutput w,c::PgpOutput l,c::PgpOutput out){
    return c::EncryptPasswordMessage(f.profile,password,In(f.data),entropy,w,l,out);};
  Check(run({nullptr,1},f.entropy(),work.out(),Out(literal),Out(message))==Code::invalid_extent,"null password");
  auto entropy=f.entropy();entropy.session_key={message.data(),32};
  Check(run(In(f.password),entropy,work.out(),Out(literal),Out(message))==Code::invalid_extent,"key/result overlap");
  entropy=f.entropy();entropy.password_salt.size=15;
  Check(run(In(f.password),entropy,work.out(),Out(literal),Out(message))==Code::invalid_extent,"short entropy");
  Check(run(In(f.password),f.entropy(),work.out(),{message.data(),literal.size()},Out(message))==Code::invalid_extent,"scratch/output overlap");
  Check(run(In(f.password),f.entropy(),{work.out().data+1,work.out().size},Out(literal),Out(message))==Code::invalid_extent,"unaligned workspace");
  Check(Filled(literal,0xa5)&&Filled(message,0xa5),"pre-admission extent failures have no writes");
  const auto wire=f.encode();
  const auto info=c::InspectPasswordMessage(In(wire));
  Bytes body(info.data_body_bytes,0xa5),plain(info.data_body_bytes,0xa5);
  Check(c::DecryptPasswordMessage(In(f.password),In(wire),work.out(),Out(body),Out(body)).code==Code::invalid_extent,"decrypt scratch overlap");
  Check(c::DecryptPasswordMessage(In(f.password),In(wire),work.out(),Out(body),{plain.data(),plain.size()-1}).code==Code::invalid_extent&&Filled(plain,0xa5),"decrypt exact capacity");
}
void ProviderFailures(){
#ifdef SB_MESSAGE_PROVIDER_FAULTS
  Fixture f(130);const auto wire=f.encode();const auto info=c::InspectPasswordMessage(In(wire));
  const auto size=c::PasswordMessageEncodedSize(f.profile,f.data.size());
  for(bool decrypt:{false,true}){
    Work work(info.workspace_bytes);Bytes scratch(decrypt?info.data_body_bytes:size.literal_bytes,0xa5);
    Bytes output(decrypt?info.data_body_bytes:size.message_bytes,0xa5);
    auto run=[&]{return decrypt?c::DecryptPasswordMessage(In(f.password),In(wire),work.out(),Out(scratch),Out(output)).code:
      c::EncryptPasswordMessage(f.profile,In(f.password),In(f.data),f.entropy(),work.out(),Out(scratch),Out(output));};
    for(unsigned kind=1;kind<=3;++kind){
      fault_kind=kind;fault_at=0;provider_calls=0;const auto baseline=run();const auto count=provider_calls;fault_kind=0;
      Check(baseline==Code::ok&&count>0,"message provider baseline");
      for(unsigned at=1;at<=count;++at){
        work.dirty();std::fill(scratch.begin(),scratch.end(),0xa5);std::fill(output.begin(),output.end(),0xa5);
        fault_kind=kind;fault_at=at;provider_calls=0;const auto code=run();fault_kind=0;
        Check(code!=Code::ok&&Filled(scratch,0)&&Filled(output,0)&&work.zero(),"provider failure including partial output clears entire message stages");
        Check(run()==Code::ok,"provider failure permits independent retry");
      }
    }
  }
#endif
}
int main(){try{CrossProfiles();Tampering();InnerAndPartial();Cancellation();Extents();ProviderFailures();}
  catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
  std::cout<<"openpgp_password_message checks="<<checks<<'\n';}
