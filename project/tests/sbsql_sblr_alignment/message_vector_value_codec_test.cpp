// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "message_vector_value_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <vector>

// Fault the actual allocation path, not a codec success/failure hook.
namespace { long allocations_before_failure=-1; }
void* operator new(std::size_t size) {
  if(allocations_before_failure==0) throw std::bad_alloc();
  if(allocations_before_failure>0) --allocations_before_failure;
  if(void* p=std::malloc(size?size:1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }

namespace {
namespace m=scratchbird::wire::message_vector;
using B=std::vector<std::uint8_t>;
using V=m::ValueTlv;
unsigned checks=0,failures=0;
void Check(bool ok,const char* message) {
  ++checks;if(!ok) {++failures;std::cerr<<message<<'\n';}
}
void Put(B& b,std::size_t at,std::uint64_t n,unsigned width) {
  for(unsigned i=0;i<width;++i) b[at+i]=std::uint8_t(n>>(8*i));
}
B Number(std::uint64_t n) { B b(8);Put(b,0,n,8);return b; }
B Text(const std::string& s) { return {s.begin(),s.end()}; }
B Id() { return {1,160,0,0,0,0,0x70,1,0x80,0,0,0,0,0,0,1}; }
void Pad(B& b) { while(b.size()%4) b.push_back(0); }
void Append(B& out,const B& bytes,bool pad=true) {
  out.insert(out.end(),bytes.begin(),bytes.end());if(pad) Pad(out);
}
// Independent construction from Core field offsets; never asks the production
// encoder for expected bytes, nested payloads, or mutation resealing.
B Raw(const V& value) {
  B out(8);Put(out,0,value.key.size(),2);Put(out,2,unsigned(value.type),2);
  Put(out,4,value.value.size(),4);Append(out,Text(value.key),false);
  Append(out,value.value);return out;
}
V Value(unsigned type,B bytes,std::string key="key") {
  return {std::move(key),static_cast<m::ValueType>(type),std::move(bytes)};
}
void Accept(const V& value,bool truncations=false) {
  const auto oracle=Raw(value);
  B encoded={0xfa};
  Check(m::EncodeValueTlv(value,&encoded)==m::ValueCodecError::none && encoded==oracle,
        "encode differed from independent exact bytes");
  V decoded=Value(1,{0xfa},"unchanged");
  Check(m::DecodeValueTlv(oracle,&decoded)==m::ValueCodecError::none && decoded==value,
        "decode lost typed fields or exact binary data");
  if(truncations) for(std::size_t n=0;n<oracle.size();++n) {
    V out=Value(1,{0xfa},"unchanged");const auto before=out;
    Check(m::DecodeValueTlv(std::span(oracle).first(n),&out)==m::ValueCodecError::malformed && out==before,
          "truncated TLV admitted or output modified");
  }
  auto trailing=oracle;trailing.push_back(0);
  const auto before=decoded;
  Check(m::DecodeValueTlv(trailing,&decoded)==m::ValueCodecError::malformed && decoded==before,
        "trailing bytes admitted or output modified");
}
void Refuse(const V& value) {
  B out={0xfa,0xfb};const auto before=out;
  Check(m::EncodeValueTlv(value,&out)==m::ValueCodecError::malformed && out==before,
        "invalid typed value encoded or output modified");
  const auto raw=Raw(value);V decoded=Value(1,{0xfa},"unchanged"),prior=decoded;
  Check(m::DecodeValueTlv(raw,&decoded)==m::ValueCodecError::malformed && decoded==prior,
        "invalid typed value decoded or output modified");
}
B Parameter(unsigned encoding,unsigned type,const B& bytes) {
  B p(32);p[4]=11;p[6]=encoding;
  const auto id=Id();std::copy(id.begin(),id.end(),p.begin()+8);
  Put(p,24,3,2);Put(p,26,type,2);Put(p,28,bytes.size(),4);
  Append(p,Text("fmt"));Append(p,bytes);return p;
}
B Detail(unsigned kind,unsigned type,const B& bytes) {
  B p(8);p[0]=kind;Put(p,2,type,2);Put(p,4,bytes.size(),4);Append(p,bytes);return p;
}
B Cause(unsigned ordinal,bool uuid=true) {
  B b(32);Put(b,0,ordinal,4);
  if(uuid) {const auto id=Id();std::copy(id.begin(),id.end(),b.begin()+4);}
  const std::array<std::string,4> fields={"MESSAGE_VECTOR.MALFORMED","message_vector.malformed","reject","no"};
  for(unsigned i=0;i<4;++i) Put(b,20+i*2,fields[i].size(),2);
  for(const auto& f:fields) Append(b,Text(f));
  return b;
}
std::map<std::string,V> ContextFields() {
  std::map<std::string,V> f;
  for(const auto* key:{"audit_event_uuid","channel_uuid","database_uuid",
      "donor_rendering_profile_uuid","execution_ref_uuid","locale_uuid",
      "message_template_uuid","redaction_policy_uuid","server_uuid","session_uuid","transaction_uuid"})
    f.emplace(key,Value(5,Id(),key));
  for(const auto* key:{"catalog_generation","policy_generation","registry_epoch","security_generation"})
    f.emplace(key,Value(4,Number(1),key));
  f.emplace("canonical_source_kind",Value(4,Number(0),"canonical_source_kind"));
  f.emplace("finality_state",Value(4,Number(0),"finality_state"));
  f.emplace("donor_rendering_hint",Value(1,Text("native"),"donor_rendering_hint"));
  f.emplace("required_outcome",Value(1,Text("reject"),"required_outcome"));
  f.emplace("retry_class",Value(1,Text("no"),"retry_class"));
  f.emplace("source_map_ref",Value(6,{0,0xff,1},"source_map_ref"));
  return f;
}
B Context(const std::map<std::string,V>& fields) {
  B b(8);Put(b,0,1,2);Put(b,2,fields.size(),2);
  for(const auto& [key,value]:fields) Append(b,Raw(value));
  return b;
}
}
int main() {
  const std::vector<V> scalars={Value(1,Text("A\xc3\xa9\xe2\x82\xac\xf0\x90\x80\x80")),
      Value(1,{}),Value(2,{0}),Value(2,{1}),Value(3,Number(0x8000000000000000ULL)),
      Value(3,Number(~std::uint64_t(0))),Value(4,Number(0)),Value(4,Number(~std::uint64_t(0))),
      Value(5,Id()),Value(6,{0,1,0xff,0x80}),Value(7,{})};
  for(const auto& v:scalars) Accept(v,true);
  auto user_uuid=Id();user_uuid[6]=0x40;
  B typed=Id();Append(typed,user_uuid,false);Accept(Value(8,typed),true);
  for(unsigned version=0;version<16;++version) if(version!=7) {
    auto id=Id();id[6]=std::uint8_t(version<<4);Refuse(Value(5,id));
    B t=id;Append(t,user_uuid,false);Refuse(Value(8,t));
  }
  for(unsigned variant:{0u,0x40u,0xc0u}) {
    auto id=Id();id[8]=variant;Refuse(Value(5,id));
  }
  Refuse(Value(5,B(16)));Refuse(Value(5,Text("01a00000-0000-7001-8000-000000000001")));
  for(unsigned type:{2u,3u,4u,5u,7u,8u}) {
    Refuse(Value(type,B(type==7?1:3,0)));
  }
  Refuse(Value(2,{2}));Refuse(Value(2,{0,0}));
  for(const auto& bad:std::vector<B>{{0},{0x80},{0xc0,0x80},{0xc1,0xbf},{0xc2},
      {0xe0,0x80,0x80},{0xed,0xa0,0x80},{0xf0,0x80,0x80,0x80},
      {0xf4,0x90,0x80,0x80},{0xf5,0x80,0x80,0x80},{0xff},{0xe2,0x28,0xa1}}) {
    Refuse(Value(1,bad));Refuse(Value(1,Text("ok"),std::string(bad.begin(),bad.end())));
  }
  Refuse(Value(1,Text("ok"),""));Refuse(Value(1,{},"$context"));
  for(unsigned type=0;type<=65535;++type) {
    if((type>=1 && type<=8) || (type>=256 && type<=259)) continue;
    Refuse(Value(type,{}));
  }
  const std::array<unsigned,8> value_types={8,5,3,1,1,7,2,6};
  const std::array<B,8> values={typed,Id(),Number(42),Text("key"),Text("redacted"),B{},B{1},B{0,0xff}};
  for(unsigned encoding=0;encoding<8;++encoding) {
    auto p=Parameter(encoding,value_types[encoding],values[encoding]);Accept(Value(256,p),true);
    for(unsigned bad:{0u,9u,256u,257u,258u,259u,65535u}) {
      auto b=p;Put(b,26,bad,2);Refuse(Value(256,b));
    }
    for(unsigned off:{7u,35u}) {auto b=p;b[off]=1;Refuse(Value(256,b));}
    {auto b=p;b[5]=4;Refuse(Value(256,b));}
    {auto b=p;b[4]=12;Refuse(Value(256,b));}
    {auto b=p;b[6]=8;Refuse(Value(256,b));}
    {auto b=p;Put(b,0,64,4);Refuse(Value(256,b));}
    {auto b=p;Put(b,28,0xffffffff,4);Refuse(Value(256,b));}
    {auto b=p;b[14]=0x40;Refuse(Value(256,b));}
  }
  {auto p=Parameter(0,8,typed);p[36+15]^=1;Refuse(Value(256,p));}
  Accept(Value(256,Parameter(2,4,Number(~std::uint64_t(0)))),true);
  for(unsigned kind=0;kind<13;++kind) for(unsigned visibility=0;visibility<4;++visibility) {
    auto d=Detail(kind,5,Id());d[1]=visibility;Accept(Value(257,d));
  }
  {auto d=Detail(13,1,Text("x"));Refuse(Value(257,d));d[0]=0;d[1]=4;Refuse(Value(257,d));}
  {auto d=Detail(0,1,Text("x"));d.back()=1;Refuse(Value(257,d));}
  for(unsigned i=0;i<16;++i) Accept(Value(258,Cause(i),std::to_string(i)),true);
  Accept(Value(258,Cause(0,false),"0"));Refuse(Value(258,Cause(0),"00"));
  Refuse(Value(258,Cause(16),"16"));Refuse(Value(258,Cause(0),"1"));
  for(unsigned offset=28;offset<32;++offset) {auto c=Cause(0);c[offset]=1;Refuse(Value(258,c,"0"));}
  {auto c=Cause(0);c[10]=0x40;Refuse(Value(258,c,"0"));}
  auto fields=ContextFields();Accept(Value(259,Context(fields),"$context"),true);
  Refuse(Value(259,Context(fields),"context"));
  for(const auto& [key,value]:fields) {
    auto missing=fields;missing.erase(key);Refuse(Value(259,Context(missing),"$context"));
    auto wrong=fields;wrong.at(key)=Value(256,Parameter(2,3,Number(1)),key);
    Refuse(Value(259,Context(wrong),"$context"));
    auto nullable=fields;nullable.at(key)=Value(7,{},key);
    const bool required=key=="canonical_source_kind" || key=="finality_state" ||
        key=="redaction_policy_uuid" || key=="registry_epoch" ||
        key=="required_outcome" || key=="retry_class";
    if(required) Refuse(Value(259,Context(nullable),"$context"));
    else Accept(Value(259,Context(nullable),"$context"));
  }
  for(const auto* key:{"catalog_generation","policy_generation","registry_epoch","security_generation"}) {
    auto f=fields;f.at(key).value=Number(0);Refuse(Value(259,Context(f),"$context"));
  }
  for(const auto* key:{"canonical_source_kind","finality_state"}) {
    auto f=fields;f.at(key).value=Number(8);Refuse(Value(259,Context(f),"$context"));
  }
  for(const auto* key:{"required_outcome","retry_class"}) {
    auto f=fields;f.at(key).value={};Refuse(Value(259,Context(f),"$context"));
  }
  for(unsigned byte=4;byte<8;++byte) {
    auto c=Context(fields);c[byte]=1;Refuse(Value(259,c,"$context"));
  }
  {
    auto duplicate=fields;
    auto first=duplicate.begin();auto second=std::next(first);
    second->second.key=first->second.key;
    Refuse(Value(259,Context(duplicate),"$context"));
    B reordered(8);Put(reordered,0,1,2);Put(reordered,2,fields.size(),2);
    for(auto i=fields.rbegin();i!=fields.rend();++i) Append(reordered,Raw(i->second));
    Refuse(Value(259,reordered,"$context"));
    auto unknown=fields;unknown.begin()->second.key="unknown_key";
    Refuse(Value(259,Context(unknown),"$context"));
  }
  // Exhaustively perturb every bit of every structured layout. An accepted
  // mutation must still have one exact canonical encoding; invalid inputs must
  // leave all previous output untouched. No mutation is assumed invalid just
  // because a meaningful scalar value or UUID payload bit changed.
  for(const auto& value:std::vector<V>{Value(256,Parameter(0,8,typed)),
      Value(257,Detail(3,5,Id())),Value(258,Cause(0),"0"),
      Value(259,Context(fields),"$context")}) {
    const auto original=Raw(value);
    for(std::size_t i=0;i<original.size();++i) for(unsigned bit=0;bit<8;++bit) {
      auto mutated=original;mutated[i]^=std::uint8_t(1u<<bit);
      V decoded=Value(1,{0xfa},"unchanged"),before=decoded;
      const auto result=m::DecodeValueTlv(mutated,&decoded);
      if(result==m::ValueCodecError::none) {
        B encoded;
        Check(m::EncodeValueTlv(decoded,&encoded)==m::ValueCodecError::none && encoded==mutated,
              "accepted mutation did not preserve canonical bytes");
      } else Check(result==m::ValueCodecError::malformed && decoded==before,
                   "malformed mutation modified prior output");
    }
  }
  // Header length overflow, nonzero padding, and output-pointer admission.
  auto bytes=Raw(Value(1,Text("ok")));
  for(unsigned field:{0u,4u}) {
    auto bad=bytes;Put(bad,field,0xffffffff,field==0?2:4);
    V out=Value(1,{1},"unchanged"),before=out;
    Check(m::DecodeValueTlv(bad,&out)==m::ValueCodecError::malformed && out==before,"length overflow admitted");
  }
  {auto bad=bytes;bad.back()=1;V out;Check(m::DecodeValueTlv(bad,&out)==m::ValueCodecError::malformed,"padding admitted");}
  Check(m::DecodeValueTlv(bytes,nullptr)==m::ValueCodecError::malformed,"null decode output accepted");
  Check(m::EncodeValueTlv(scalars[0],nullptr)==m::ValueCodecError::malformed,"null encode output accepted");
  Accept(Value(6,B(262144-9,0xaa),"k"));Refuse(Value(6,B(262144-8,0xaa),"k"));
  Accept(Value(1,{},std::string(65535,'k')));Refuse(Value(1,{},std::string(65536,'k')));
  const auto large=Value(6,B(100,0xaa),std::string(100,'k'));const auto raw=Raw(large);
  for(long failure=0;failure<=2;++failure) {
    B out={1,2,3},before=out;
    allocations_before_failure=failure;const auto result=m::EncodeValueTlv(large,&out);allocations_before_failure=-1;
    Check((failure==0 && result==m::ValueCodecError::allocation_failure && out==before) ||
          (failure>0 && result==m::ValueCodecError::none && out==raw),"encode allocation failure fabricated success or modified output");
    V decoded=Value(1,{1},"unchanged"),prior=decoded;
    allocations_before_failure=failure;const auto decoded_result=m::DecodeValueTlv(raw,&decoded);allocations_before_failure=-1;
    Check((failure<2 && decoded_result==m::ValueCodecError::allocation_failure && decoded==prior) ||
          (failure==2 && decoded_result==m::ValueCodecError::none && decoded==large),"decode allocation failure fabricated success or modified output");
  }
  for(unsigned i=0;i<=11;++i) Check(m::IsRegisteredSourceComponent(i)==(i>=1 && i<=10),"wrong source component registry");
  Check(!m::IsRegisteredSourceComponent(0x52565253),"legacy private component admitted");
  std::cout<<"message_vector_value checks="<<checks<<" failures="<<failures<<'\n';
  return failures?1:0;
}
