// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Reuse the independent RFC vectors, provider-fault wrappers and actual cleanup
// observers, not another executable's main or a production deterministic hook.
#define SB_OPENPGP_X25519_FIXTURE_ONLY
#include "openpgp_x25519_test.cpp"
#undef SB_OPENPGP_X25519_FIXTURE_ONLY
#include "openpgp_public_key_message.hpp"

#ifdef SB_X25519_PROVIDER_FAULTS
extern "C" {
int __real_EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX*,int,int,void*);
int __wrap_EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX* a,int type,int count,void* value){return Fail(23)?0:__real_EVP_CIPHER_CTX_ctrl(a,type,count,value);}
int __real_EVP_CIPHER_CTX_reset(EVP_CIPHER_CTX*);
int __wrap_EVP_CIPHER_CTX_reset(EVP_CIPHER_CTX* a){return Fail(24)?0:__real_EVP_CIPHER_CTX_reset(a);}
}
#endif
namespace {
const Bytes data_salt(32,0x42);
Bytes Session(unsigned cipher){Bytes key(16+(cipher-7)*8);for(unsigned i=0;i<key.size();++i)key[i]=i*13+7;return key;}
Bytes Pattern(unsigned size){Bytes data(size);for(unsigned i=0;i<size;++i)data[i]=i*17;return data;}
void Length(Bytes& out,std::size_t size){
  if(size<192)out.push_back(size);
  else if(size<8384){out.push_back(((size-192)>>8)+192);out.push_back((size-192)&255);}
  else{out.push_back(255);for(int i=3;i>=0;--i)out.push_back((size>>(8*i))&255);}
}
Bytes Frame(unsigned type,const Bytes& body,bool partial=false){
  Bytes out{static_cast<std::uint8_t>(0xc0|type)};std::size_t at=0;
  if(partial)while(body.size()-at>=512){out.push_back(0xe9);out.insert(out.end(),body.begin()+at,body.begin()+at+512);at+=512;}
  Length(out,body.size()-at);out.insert(out.end(),body.begin()+at,body.end());return out;
}
Bytes Inner(const Bytes& data,unsigned format='b',Bytes filename={}){
  Bytes body{static_cast<std::uint8_t>(format),static_cast<std::uint8_t>(filename.size())};
  body.insert(body.end(),filename.begin(),filename.end());body.insert(body.end(),{0,0,0,0});body.insert(body.end(),data.begin(),data.end());return body;
}
Bytes Compose(c::SeipdProfile profile,const Bytes& inner,bool partial=false){
  const auto key=Session(profile.cipher);Bytes pkesk(c::X25519PkeskV6Size(key.size()).bytes,0xa5);
  Check(c::EncryptX25519PkeskV6(In(public_body),In(ephemeral_secret),In(key),Out(pkesk))==Code::ok,"qualified component PKESK composition");
  Bytes encrypted(c::SeipdEncryptedSize(profile,inner.size()).bytes,0xa5);
  Check(c::EncryptSeipdV2(profile,In(key),In(data_salt),In(inner),Out(encrypted))==Code::ok,"qualified component SEIPD composition");
  auto message=Frame(1,pkesk);const auto second=Frame(18,encrypted,partial);message.insert(message.end(),second.begin(),second.end());return message;
}
Bytes Encrypt(c::SeipdProfile profile,const Bytes& data){
  const auto size=c::X25519LiteralMessageEncodedSize(profile,data.size());Check(size.code==Code::ok,"message size");
  const auto key=Session(profile.cipher);Bytes scratch(size.literal_bytes,0xa5),message(size.message_bytes,0xa5);
  deny_heap=true;const auto code=c::EncryptX25519LiteralMessage(profile,In(public_body),In(data),
      {In(key),In(ephemeral_secret),In(data_salt)},Out(scratch),Out(message));deny_heap=false;
  Check(code==Code::ok&&Zero(scratch)&&!unexpected_heap,"full private encrypt erases literal without C++ heap");return message;
}
void Decode(const Bytes& message,const Bytes& data,unsigned format='b'){
  const auto inspected=c::InspectX25519LiteralMessage(In(message));Check(inspected.code==Code::ok,"structural message prescan");
  Check(std::equal(fingerprint.begin(),fingerprint.end(),inspected.recipient.recipient_fingerprint.begin()),"actual declared recipient observation");
  Bytes body(inspected.data_body_bytes,0xa5),plain(body.size(),0xa5);
  deny_heap=true;const auto result=c::DecryptX25519LiteralMessage(In(public_body),In(private_key),In(message),Out(body),Out(plain));deny_heap=false;
  Check(result.code==Code::ok&&result.data.data==plain.data()&&result.data.size==data.size()&&result.literal_format==format&&
      std::equal(data.begin(),data.end(),plain.begin())&&Zero(body)&&
      std::all_of(plain.begin()+data.size(),plain.end(),[](auto x){return !x;})&&!unexpected_heap,
      "all bytes authenticated, literal reassembled, metadata/body/tail erased, exact native bytes returned");
}
void ProfilesAndFraming(){
  for(unsigned cipher:{7u,8u,9u})for(unsigned aead:{2u,3u})for(unsigned chunk:{0u,10u,16u}){
    const c::SeipdProfile profile{static_cast<std::uint8_t>(cipher),static_cast<std::uint8_t>(aead),static_cast<std::uint8_t>(chunk)};
    for(unsigned size:{0u,1u,55u,56u,57u,191u,192u,504u,505u,506u,1024u,4097u}){
      const auto data=Pattern(size),message=Encrypt(profile,data);
      Check(message==Compose(profile,Frame(11,Inner(data))),"full encryption matches independent outer/literal framing and qualified components");Decode(message,data);
    }
  }
  const auto data=Pattern(1600);
  for(unsigned format:{'b','t','u'})for(bool partial_literal:{false,true})for(bool partial_data:{false,true}){
    const auto literal=Frame(11,Inner(data,format,Bytes{'/','.','.','/','x',0,'z'}),partial_literal);
    Decode(Compose({9,2,0},literal,partial_data),data,format);
  }
  for(auto invalid:{c::SeipdProfile{6,2,10},c::SeipdProfile{9,1,10},c::SeipdProfile{9,2,17}})
    Check(c::X25519LiteralMessageEncodedSize(invalid,1).code==Code::invalid_profile,"unadmitted data profile never creates a size");
  Check(c::X25519LiteralMessageEncodedSize({},std::numeric_limits<std::size_t>::max()).code!=Code::ok,"full message size arithmetic cannot wrap");
}
void Refused(const Bytes& message){
  const auto inspected=c::InspectX25519LiteralMessage(In(message));
  const auto amount=inspected.code==Code::ok?inspected.data_body_bytes:256;Bytes body(amount,0xa5),plain(amount,0xa5);
  const auto result=c::DecryptX25519LiteralMessage(In(public_body),In(private_key),In(message),Out(body),Out(plain));
  Check(result.code!=Code::ok&&!result.data.data&&!result.data.size&&!result.literal_format,"refusal exposes no plaintext/metadata prefix");
  if(inspected.code==Code::ok)Check(Zero(body)&&Zero(plain),"accepted message failure clears both full staging regions");
  else Check(body==Bytes(amount,0xa5)&&plain==Bytes(amount,0xa5),"structural prescan refusal untouched");
}
void MalformedAndAuthentication(){
  const auto message=Encrypt({9,2,0},Pattern(31));
  for(std::size_t n=0;n<message.size();++n)Refused(Bytes(message.begin(),message.begin()+n));
  auto trailing=message;trailing.push_back(0);Refused(trailing);
  for(std::size_t at=0;at<message.size();++at){auto bad=message;bad[at]^=1;Refused(bad);}
  // Authenticated but outside this explicitly single-literal mechanism. These
  // are not silent ignores, nor declarations that the enclosing builtin may
  // permanently refuse compression, signatures or padding dispatch.
  const auto literal=Frame(11,Inner(Pattern(7)));
  for(auto wrong:{Frame(11,Inner(Pattern(7),'x')),Frame(8,Pattern(3)),Frame(10,Pattern(4)),Frame(21,Pattern(4))})
    Refused(Compose({9,2,0},wrong));
  auto two=literal;two.insert(two.end(),literal.begin(),literal.end());Refused(Compose({9,2,0},two));
  auto garbage=literal;garbage.push_back(0);Refused(Compose({9,2,0},garbage));
  auto malformed=Inner(Pattern(4));malformed[1]=255;Refused(Compose({9,2,0},Frame(11,malformed)));
  auto wrong_key=private_key;wrong_key[10]^=1;const auto inspected=c::InspectX25519LiteralMessage(In(message));
  Bytes body(inspected.data_body_bytes,0xa5),plain(body.size(),0xa5);
  auto result=c::DecryptX25519LiteralMessage(In(public_body),In(wrong_key),In(message),Out(body),Out(plain));
  Check(result.code==Code::authentication_failed&&Zero(body)&&Zero(plain)&&!result.data.data,"wrong actual private key erases complete staging");
  // Keep a malformed literal encrypted but corrupt the final tag: the result
  // MUST be authentication failure, proving the parser was not reached first.
  auto unauthenticated=Compose({9,2,0},Frame(11,malformed));unauthenticated.back()^=1;
  const auto parsed=c::InspectX25519LiteralMessage(In(unauthenticated));body.assign(parsed.data_body_bytes,0xa5);plain=body;
  result=c::DecryptX25519LiteralMessage(In(public_body),In(private_key),In(unauthenticated),Out(body),Out(plain));
  Check(result.code==Code::authentication_failed&&Zero(body)&&Zero(plain),"final length/tag is authenticated before malformed literal metadata is interpreted");
}
void BufferAdmission(){
  const auto data=Pattern(13),message=Encrypt({},data),key=Session(9);
  const auto size=c::X25519LiteralMessageEncodedSize({},data.size());const auto desc=c::InspectX25519LiteralMessage(In(message));
  const auto near=reinterpret_cast<const std::uint8_t*>(std::numeric_limits<std::uintptr_t>::max()-3);
  for(bool decrypt:{false,true}){
    const auto first_size=decrypt?desc.data_body_bytes:size.literal_bytes,second_size=decrypt?desc.data_body_bytes:size.message_bytes;
    Bytes first(first_size,0xa5),second(second_size,0xa5);
    const auto run=[&](const std::array<c::PgpInput,5>& inputs,c::PgpOutput a,c::PgpOutput b){return decrypt?
      c::DecryptX25519LiteralMessage(inputs[0],inputs[1],inputs[2],a,b).code:
      c::EncryptX25519LiteralMessage({},inputs[0],inputs[1],{inputs[2],inputs[3],inputs[4]},a,b);};
    const std::array<c::PgpInput,5> inputs=decrypt?std::array<c::PgpInput,5>{In(public_body),In(private_key),In(message),{}, {}}:
      std::array<c::PgpInput,5>{In(public_body),In(data),In(key),In(ephemeral_secret),In(data_salt)};
    for(unsigned which=0;which<(decrypt?3u:5u);++which)for(bool overflow:{false,true}){
      auto bad=inputs;bad[which].data=overflow?near:nullptr;
      Check(run(bad,Out(first),Out(second))==Code::invalid_extent&&first==Bytes(first_size,0xa5)&&second==Bytes(second_size,0xa5),"invalid/overflow input leaves both outputs untouched");
    }
    for(unsigned which=0;which<2;++which)for(unsigned defect=0;defect<3;++defect){
      auto a=Out(first),b=Out(second);auto& bad=which?b:a;
      if(defect==0)bad.data=nullptr;else if(defect==1)bad.data=const_cast<std::uint8_t*>(near);else --bad.size;
      Check(run(inputs,a,b)==Code::invalid_extent&&first==Bytes(first_size,0xa5)&&second==Bytes(second_size,0xa5),"invalid/short output leaves both outputs untouched");
    }
    for(unsigned which=0;which<(decrypt?3u:5u);++which)for(unsigned destination=0;destination<2;++destination)for(bool reverse:{false,true}){
      Bytes shared_buffer(1024,0xa5);auto aliased=inputs;const auto input_at=reverse?0u:1u,output_at=reverse?1u:0u;
      std::copy_n(inputs[which].data,inputs[which].size,shared_buffer.data()+input_at);
      aliased[which]={shared_buffer.data()+input_at,inputs[which].size};const auto saved=shared_buffer;
      const auto a=destination?Out(first):c::PgpOutput{shared_buffer.data()+output_at,first_size};
      const auto b=destination?c::PgpOutput{shared_buffer.data()+output_at,second_size}:Out(second);
      Check(run(aliased,a,b)==Code::invalid_extent&&shared_buffer==saved&&first==Bytes(first_size,0xa5)&&second==Bytes(second_size,0xa5),"every staging/input overlap unchanged");
    }
    Bytes shared_buffer(1024,0xa5);const auto saved=shared_buffer;
    Check(run(inputs,{shared_buffer.data(),first_size},{shared_buffer.data()+1,second_size})==Code::invalid_extent&&shared_buffer==saved,"staging buffers cannot overlap one another");
    auto bad=inputs;Bytes malformed_public=public_body;malformed_public[5]=27;bad[0]=In(malformed_public);
    Check(run(bad,Out(first),Out(second))==Code::invalid_packet&&first==Bytes(first_size,0xa5)&&second==Bytes(second_size,0xa5),"wrong native key profile refused before staging acceptance");
  }
}
void MessageCancellationAndProviders(){
  const auto data=Pattern(700),message=Encrypt({9,2,0},data),key=Session(9);
  const auto size=c::X25519LiteralMessageEncodedSize({9,2,0},data.size());const auto desc=c::InspectX25519LiteralMessage(In(message));
  for(bool decrypt:{false,true}){
    Bytes scratch(decrypt?desc.data_body_bytes:size.literal_bytes,0xa5),output(decrypt?desc.data_body_bytes:size.message_bytes,0xa5);
    const auto run=[&](c::PgpCancellation probe={}){return decrypt?
      c::DecryptX25519LiteralMessage(In(public_body),In(private_key),In(message),Out(scratch),Out(output),probe).code:
      c::EncryptX25519LiteralMessage({9,2,0},In(public_body),In(data),{In(key),In(ephemeral_secret),In(data_salt)},Out(scratch),Out(output),probe);};
    Probe baseline;Check(run(baseline.callback())==Code::ok,"complete message cancellation baseline");
    Probe prescan;if(decrypt)Check(c::InspectX25519LiteralMessage(In(message),prescan.callback()).code==Code::ok,"actual structural probe count");
    for(bool throwing:{false,true})for(unsigned at=1;at<=baseline.calls;++at){
      std::fill(scratch.begin(),scratch.end(),0xa5);std::fill(output.begin(),output.end(),0xa5);
      Probe probe{0,at,throwing,at==baseline.calls};bool threw=false;Code code=Code::ok;
#ifdef SB_X25519_PROVIDER_FAULTS
      observe_cleanup=true;scrub_calls=0;scrub_failed=false;
#endif
      try{code=run(probe.callback());}catch(const ProbeException&){threw=true;}
#ifdef SB_X25519_PROVIDER_FAULTS
      observe_cleanup=false;Check(!live_keys&&!live_key_contexts&&!live_digest_contexts&&!live_cipher_contexts&&!scrub_failed,"message cancellation releases actual provider state");
#endif
      Check(threw==throwing&&probe.calls==at&&(throwing||code==Code::cancelled),"exact message cancellation/throw phase");
      if(decrypt&&at<=prescan.calls)Check(scratch==Bytes(scratch.size(),0xa5)&&output==Bytes(output.size(),0xa5),"pre-admission cancelled structural prescan leaves buffers untouched");
      else Check(Zero(scratch)&&Zero(output),"every accepted cancellation including final publication fence erases both buffers");
    }
#ifdef SB_X25519_PROVIDER_FAULTS
    for(unsigned kind=1;kind<=24;++kind){
      failure_kind=kind;failure_at=0;provider_calls=0;Check(run()==Code::ok,"whole-message actual provider baseline");const auto count=provider_calls;failure_kind=0;
      for(unsigned at=1;at<=count;++at){
        std::fill(scratch.begin(),scratch.end(),0xa5);std::fill(output.begin(),output.end(),0xa5);
        observe_cleanup=true;scrub_calls=0;scrub_failed=false;failure_kind=kind;failure_at=at;provider_calls=0;
        const auto code=run();const auto calls=provider_calls;failure_kind=0;observe_cleanup=false;
        Check(code!=Code::ok&&calls==at&&Zero(scratch)&&Zero(output)&&scrub_calls&&!scrub_failed&&
          !live_keys&&!live_key_contexts&&!live_digest_contexts&&!live_cipher_contexts,"every actual whole-message provider failure scrubs all staging and releases backend copies");
        Check(run()==Code::ok,"whole-message provider retry remains independent");
      }
    }
#endif
  }
}
}  // namespace
int main()try{
  ProfilesAndFraming();MalformedAndAuthentication();BufferAdmission();MessageCancellationAndProviders();
  std::cout<<"PASS X25519 single-literal public-key message "<<checks<<" checks; not certificate or runtime admission\n";return 0;
}catch(const std::exception& error){deny_heap=false;std::cerr<<"FAIL "<<error.what()<<" checks="<<checks<<'\n';return 1;}
