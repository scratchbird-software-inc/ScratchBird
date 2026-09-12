// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "message_vector_set_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <new>
#include <string>

namespace { long fail_after=-1; }
void* operator new(std::size_t n) {
  if(fail_after==0) throw std::bad_alloc();
  if(fail_after>0) --fail_after;
  if(void* p=std::malloc(n?n:1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}

namespace {
namespace m=scratchbird::wire::message_vector;
using B=std::vector<std::uint8_t>;
using V=m::ValueTlv;
using E=m::ValueCodecError;
unsigned checks=0,failures=0;
void Check(bool ok,const char* message) {
  ++checks;if(!ok) {++failures;std::cerr<<message<<'\n';}
}
void Put(B& b,std::size_t at,std::uint64_t n,unsigned width) {
  for(unsigned i=0;i<width;++i) b[at+i]=std::uint8_t(n>>(i*8));
}
std::uint32_t Get32(const B& b,std::size_t at) {
  std::uint32_t n=0;for(unsigned i=0;i<4;++i) n|=std::uint32_t(b[at+i])<<(8*i);return n;
}
void Pad(B& b) {while(b.size()%4) b.push_back(0);}
void Add(B& b,const B& v,bool pad=true) {
  b.insert(b.end(),v.begin(),v.end());if(pad) Pad(b);
}
B Text(const std::string& s) {return {s.begin(),s.end()};}
B Number(std::uint64_t n) {B b(8);Put(b,0,n,8);return b;}
m::MessageUuid Id(unsigned n=1) {return {1,160,0,0,0,0,0x70,1,0x80,0,0,0,0,0,0,std::uint8_t(n)};}
B Identity(unsigned n=1) {const auto id=Id(n);return {id.begin(),id.end()};}
V Value(std::string key,unsigned type,B value) {
  return {std::move(key),static_cast<m::ValueType>(type),std::move(value)};
}
// Independent Core-offset writer and table-based Castagnoli oracle. Production
// uses neither this writer nor this CRC table. Check the standard check vector.
std::uint32_t Crc(std::span<const std::uint8_t> b) {
  static const auto table=[] {
    std::array<std::uint32_t,256> t{};
    for(unsigned i=0;i<256;++i) {
      auto c=i;for(unsigned j=0;j<8;++j) c=(c>>1)^((c&1)?0x82f63b78u:0u);t[i]=c;
    }
    return t;
  }();
  std::uint32_t c=~0u;for(auto byte:b) c=table[(c^byte)&255]^(c>>8);return ~c;
}
B RawTlv(const V& v) {
  B b(8);Put(b,0,v.key.size(),2);Put(b,2,unsigned(v.type),2);Put(b,4,v.value.size(),4);
  Add(b,Text(v.key),false);Add(b,v.value);return b;
}
std::map<std::string,V> Fields(unsigned kind=0,unsigned finality=0) {
  std::map<std::string,V> fields;
  for(const auto* key:{"audit_event_uuid","catalog_generation","channel_uuid","database_uuid",
      "donor_rendering_hint","donor_rendering_profile_uuid","execution_ref_uuid","locale_uuid",
      "message_template_uuid","security_generation","server_uuid","session_uuid","source_map_ref","transaction_uuid"})
    fields.emplace(key,Value(key,7,{}));
  for(const auto* key:{"channel_uuid","server_uuid","session_uuid"}) fields.at(key)=Value(key,5,Identity(8));
  fields.emplace("canonical_source_kind",Value("canonical_source_kind",4,Number(kind)));
  fields.emplace("finality_state",Value("finality_state",4,Number(finality)));
  fields.emplace("policy_generation",Value("policy_generation",4,Number(3)));
  fields.emplace("redaction_policy_uuid",Value("redaction_policy_uuid",5,Identity(7)));
  fields.emplace("registry_epoch",Value("registry_epoch",4,Number(2)));
  fields.emplace("required_outcome",Value("required_outcome",1,Text("reject_and_require_explicit_target")));
  fields.emplace("retry_class",Value("retry_class",1,Text("never_retry_without_catalog_or_authorization_change")));
  return fields;
}
V Context(const std::map<std::string,V>& fields) {
  B b(8);Put(b,0,1,2);Put(b,2,fields.size(),2);
  for(const auto& [key,v]:fields) Add(b,RawTlv(v));
  return Value("$context",259,b);
}
V Parameter(unsigned ordinal,const std::string& key,const std::string& value) {
  B b(32);Put(b,0,ordinal,4);b[6]=3;
  const auto id=Id(20);std::copy(id.begin(),id.end(),b.begin()+8);
  Put(b,26,1,2);Put(b,28,value.size(),4);Add(b,Text(value));return Value(key,256,b);
}
V Detail() {
  B b(8);Put(b,2,4,2);Put(b,4,8,4);Add(b,Number(31));return Value("count",257,b);
}
V Cause() {
  B b(32);const auto id=Id(30);std::copy(id.begin(),id.end(),b.begin()+4);
  const std::array<std::string,4> fields={"SB_RESOURCE_ALIAS_AMBIGUOUS","resource.alias.ambiguous","reject","no"};
  for(unsigned i=0;i<4;++i) Put(b,20+2*i,fields[i].size(),2);
  for(const auto& field:fields) Add(b,Text(field));
  return Value("0",258,b);
}
m::MessageSet Base() {
  m::MessageSet s;s.registry_generation=2;s.set_uuid=Id(1);s.max_render_bytes=4096;
  m::MessageRecord r;r.vector_uuid=Id(2);r.canonical_source_uuid=Id(3);
  r.request_uuid=Id(4);r.correlation_uuid=Id(5);r.policy_generation=3;r.source_component=2;
  r.text={"en","SB_RESOURCE_ALIAS_AMBIGUOUS","resource.alias.ambiguous","",""};
  r.context=Context(Fields());r.parameters={Parameter(0,"resource_family","charset"),Parameter(1,"alias","gb2312")};
  r.details={Detail()};r.causes={Cause()};s.records={r};return s;
}
B RawRecord(const m::MessageRecord& r) {
  B b(112);Put(b,8,1,2);b[10]=r.message_class;b[11]=r.severity;Put(b,12,r.flags,4);
  for(const auto& [offset,id]:std::array<std::pair<unsigned,m::MessageUuid>,4>{{
      {16,r.vector_uuid},{32,r.canonical_source_uuid},{48,r.request_uuid},{64,r.correlation_uuid}}})
    std::copy(id.begin(),id.end(),b.begin()+offset);
  Put(b,80,r.policy_generation,8);Put(b,88,r.source_component,4);
  for(unsigned i=0;i<5;++i) Put(b,92+2*i,r.text[i].size(),2);
  Put(b,102,r.parameters.size(),2);Put(b,104,r.details.size()+1,2);Put(b,106,r.causes.size(),2);
  b[108]=r.retryability;b[109]=r.redaction_state;
  for(const auto& s:r.text) Add(b,Text(s));
  for(const auto& p:r.parameters) Add(b,RawTlv(p));
  Add(b,RawTlv(r.context));
  for(const auto& d:r.details) Add(b,RawTlv(d));
  for(const auto& c:r.causes) Add(b,RawTlv(c));
  Put(b,0,b.size(),4);Put(b,4,Crc(b),4);return b;
}
B Raw(const m::MessageSet& s) {
  B b(64);for(const auto& r:s.records) Add(b,RawRecord(r));
  Put(b,0,0x564d4253,4);Put(b,4,64,2);Put(b,6,1,2);Put(b,8,s.flags,4);
  Put(b,12,s.records.size(),4);Put(b,16,b.size(),4);
  Put(b,20,s.records.empty()?0:Crc(std::span(b).subspan(64)),4);
  Put(b,24,s.registry_generation,8);std::copy(s.set_uuid.begin(),s.set_uuid.end(),b.begin()+32);
  Put(b,48,s.max_render_bytes,4);Put(b,52,Crc(std::span(b).first(64)),4);return b;
}
void Reseal(B& b,bool record=true) {
  if(record) {
    Put(b,68,0,4);Put(b,68,Crc(std::span(b).subspan(64,Get32(b,64))),4);
  }
  Put(b,20,b.size()==64?0:Crc(std::span(b).subspan(64)),4);
  Put(b,52,0,4);Put(b,52,Crc(std::span(b).first(64)),4);
}
// Construct a real zero CRC independently of the production encoder. The
// final32 source UUID bits are unconstrained random bits; version, variant and
// timestamp remain intact. Solve the table-oracle CRC linear system over GF2.
m::MessageRecord ZeroCrcRecord(m::MessageRecord record) {
  std::fill(record.canonical_source_uuid.begin()+12,record.canonical_source_uuid.end(),0);
  auto checksum=[](const m::MessageRecord& r) {
    auto bytes=RawRecord(r);Put(bytes,4,0,4);return Crc(bytes);
  };
  const auto origin=checksum(record);
  std::array<std::uint32_t,32> basis{},combinations{};
  for(unsigned bit=0;bit<32;++bit) {
    auto candidate=record;
    candidate.canonical_source_uuid[12+bit/8]^=std::uint8_t(1u<<(bit%8));
    auto column=checksum(candidate)^origin;
    std::uint32_t combination=std::uint32_t{1}<<bit;
    for(int pivot=31;pivot>=0;--pivot) {
      if(!(column&(std::uint32_t{1}<<pivot))) continue;
      if(basis[pivot]) {column^=basis[pivot];combination^=combinations[pivot];}
      else {basis[pivot]=column;combinations[pivot]=combination;break;}
    }
  }
  auto remainder=origin;
  std::uint32_t solution=0;
  for(int pivot=31;pivot>=0;--pivot) {
    if(!(remainder&(std::uint32_t{1}<<pivot))) continue;
    remainder^=basis[pivot];solution^=combinations[pivot];
  }
  Check(remainder==0,"independent zero-CRC system has no solution");
  for(unsigned byte=0;byte<4;++byte)
    record.canonical_source_uuid[12+byte]=std::uint8_t(solution>>(8*byte));
  const auto bytes=RawRecord(record);
  Check(Get32(bytes,4)==0 && Crc(bytes)==0,"constructed record does not have exact zero CRC");
  return record;
}
void RejectBytes(const B& b) {
  auto out=Base();out.max_render_bytes=123;const auto before=out;
  Check(m::DecodeMessageSet(b,&out)==E::malformed && out==before,
        "malformed set admitted or decoded output changed");
}
void Reject(const m::MessageSet& s) {
  B bytes={1,2,3},before=bytes;
  Check(m::EncodeMessageSet(s,&bytes)==E::malformed && bytes==before,
        "malformed set encoded or prior output changed");
  RejectBytes(Raw(s));
}
void Accept(const m::MessageSet& s,bool truncate=false) {
  B out;const auto expected=Raw(s);
  Check(m::EncodeMessageSet(s,&out)==E::none && out==expected,"set encoding differs from exact Core oracle");
  m::MessageSet decoded;
  Check(m::DecodeMessageSet(expected,&decoded)==E::none && decoded==s,"set decoder dropped or changed canonical data");
  if(truncate) for(std::size_t i=0;i<expected.size();++i)
    RejectBytes(B(expected.begin(),expected.begin()+i));
}
}
int main() {
  Check(Crc(Text("123456789"))==0xe3069283u,"independent Castagnoli check value wrong");
  const auto base=Base();Accept(base,true);
  auto empty=base;empty.records.clear();Accept(empty,true);
  {
    auto zero=base;zero.records[0]=ZeroCrcRecord(zero.records[0]);
    for(unsigned count=1;count<=2;++count) {
      const auto bytes=Raw(zero);
      Check(Get32(bytes,12)==count && bytes.size()>64 && Get32(bytes,20)==0 &&
                Crc(std::span(bytes).subspan(64))==0,
            "nonempty exact-zero record-range CRC fixture is invalid");
      Accept(zero);
      auto damaged=bytes;damaged[64+44]^=1;RejectBytes(damaged);
      if(count==1) {
        auto next=zero.records[0];next.vector_uuid=Id(90);
        zero.records.push_back(ZeroCrcRecord(next));
      }
    }
  }
  for(unsigned bit=4;bit<32;++bit) {auto s=base;s.flags=1u<<bit;Reject(s);}
  for(unsigned flags:{1u,2u,4u,7u}) {auto s=base;s.flags=flags;Reject(s);}
  {auto s=base;s.flags=8;Accept(s);}
  {auto s=empty;s.flags=8;Reject(s);}
  for(unsigned offset:{0u,4u,6u,12u,16u,24u,48u,56u,60u}) {
    auto bytes=Raw(base);Put(bytes,offset,offset==48?0:0xffffffff,offset==4 || offset==6?2:4);
    if(offset==24) Put(bytes,24,0,8);
    Reseal(bytes,false);RejectBytes(bytes);
  }
  for(unsigned version=0;version<16;++version) if(version!=7) {
    auto s=base;s.set_uuid[6]=version<<4;Reject(s);
    for(unsigned which=0;which<4;++which) {
      s=base;auto& r=s.records.front();
      auto* id=which==0?&r.vector_uuid:which==1?&r.canonical_source_uuid:which==2?&r.request_uuid:&r.correlation_uuid;
      (*id)[6]=version<<4;Reject(s);
    }
  }
  for(unsigned variant:{0u,0x40u,0xc0u}) {auto s=base;s.set_uuid[8]=variant;Reject(s);}
  {auto s=base;s.set_uuid={};Reject(s);}
  {auto s=base;s.records[0].vector_uuid={};Reject(s);}
  {auto s=base;s.records[0].canonical_source_uuid={};Reject(s);}
  {auto s=base;s.records[0].request_uuid={};s.records[0].correlation_uuid={};Accept(s);}
  for(unsigned which=0;which<3;++which) {
    auto s=base;
    if(which==0) s.registry_generation=0;
    else if(which==1) s.max_render_bytes=0;
    else s.max_render_bytes=1048577;
    Reject(s);
  }
  for(unsigned which=0;which<7;++which) {
    auto s=base;auto& r=s.records[0];
    if(which==0) r.flags=0;else if(which==1) r.flags=3;
    else if(which==2) r.source_component=0x52565253;
    else if(which==3) r.message_class=8;else if(which==4) r.severity=13;
    else if(which==5) r.retryability=3;else r.redaction_state=4;
    Reject(s);
  }
  // Independent exact Core table, not the production classification helper.
  constexpr std::array<std::uint8_t,13> severity_classes{2,1,0,0,0,0,0,7,2,0,0,7,7};
  for(unsigned severity=0;severity<severity_classes.size();++severity) {
    auto s=base;auto& r=s.records[0];r.severity=severity;
    r.message_class=severity_classes[severity];Accept(s);
    for(unsigned other=0;other<8;++other) {
      if(other==severity_classes[severity]) continue;
      r.message_class=other;Reject(s);
    }
  }
  for(unsigned severity=13;severity<256;++severity) {
    auto s=base;s.records[0].severity=severity;Reject(s);
  }
  for(unsigned kind=1;kind<=6;++kind) {
    auto s=base;auto& r=s.records[0];r.context=Context(Fields(kind));
    r.message_class=kind==1?5:kind+1;r.canonical_source_uuid={};
    if(kind==5) s.flags=2;
    if(kind==2) s.flags=4;
    Accept(s);
    r.message_class=0;Reject(s);
  }
  for(unsigned state=1;state<=3;++state) {auto s=base;s.records[0].redaction_state=state;s.flags=1;Accept(s);}
  for(unsigned finality=1;finality<=7;++finality) {
    auto s=base;s.records[0].context=Context(Fields(0,finality));s.flags=2;Accept(s);
  }
  for(unsigned which=0;which<2;++which) {
    auto s=base;auto f=Fields();f.at(which==0?"registry_epoch":"policy_generation").value=Number(9);
    s.records[0].context=Context(f);Reject(s);
  }
  {auto s=base;auto f=Fields();f.at("policy_generation")=Value("policy_generation",7,{});
    s.records[0].context=Context(f);s.records[0].policy_generation=0;Accept(s);}
  for(unsigned which=0;which<5;++which) {
    auto s=base;s.records[0].text[which]=std::string(1,char(0xff));Reject(s);
    s=base;s.records[0].text[which]=std::string(65536,'x');Reject(s);
  }
  for(unsigned which:{0u,1u,2u}) {auto s=base;s.records[0].text[which].clear();Reject(s);}
  {auto s=base;s.records[0].parameters[0].type=m::ValueType::utf8;Reject(s);}
  {auto s=base;s.records[0].details[0]=s.records[0].parameters[0];Reject(s);}
  {auto s=base;std::swap(s.records[0].parameters[0],s.records[0].parameters[1]);Reject(s);}
  {auto s=base;s.records[0].context=Value("wrong",259,{});Reject(s);}
  {auto s=base;s.records[0].causes[0].key="1";Put(s.records[0].causes[0].value,0,1,4);Reject(s);}
  {auto s=base;s.records.push_back(s.records[0]);Reject(s);s.records[1].vector_uuid=Id(9);Accept(s,true);}
  {auto s=base;s.records[0].details.assign(64,Detail());Accept(s);s.records[0].details.push_back(Detail());Reject(s);}
  {auto s=base;s.records[0].parameters.clear();for(unsigned i=0;i<64;++i) s.records[0].parameters.push_back(Parameter(i,"p","x"));
    Accept(s);s.records[0].parameters.push_back(Parameter(64,"p","x"));Reject(s);}
  {auto s=base;s.records[0].causes.clear();for(unsigned i=0;i<16;++i) {auto c=Cause();c.key=std::to_string(i);Put(c.value,0,i,4);s.records[0].causes.push_back(c);}
    Accept(s);s.records[0].causes.push_back(Cause());Reject(s);}
  {
    auto s=base;
    s.records[0].text[3]=std::string(65535,'a');
    s.records[0].text[4]=std::string(65535,'b');
    Accept(s);
    s.records[0].text[0]=std::string(65535,'c');
    s.records[0].text[2]=std::string(65535,'d');
    Reject(s); // individually valid strings exceed the record budget together
  }
  {
    auto s=base;s.records[0].text[4]=std::string(65535,'x');
    const auto record=s.records[0];
    for(unsigned i=1;i<15;++i) {auto r=record;r.vector_uuid=Id(40+i);s.records.push_back(r);}
    Accept(s);
    auto r=record;r.vector_uuid=Id(70);s.records.push_back(r);
    Reject(s); // all records valid individually, complete set exceeds 1 MiB
  }
  const auto original=Raw(base);
  {
    auto b=original;Put(b,20,0,4);Put(b,52,0,4);Put(b,52,Crc(std::span(b).first(64)),4);
    RejectBytes(b); // zero is not an unchecked CRC sentinel for nonempty records
    b=Raw(empty);Put(b,20,1,4);Put(b,52,0,4);Put(b,52,Crc(std::span(b).first(64)),4);
    RejectBytes(b);
  }
  for(unsigned offset:{8u,110u}) {
    auto b=original;b[64+offset]=1;if(offset==8) b[72]=2;Reseal(b);RejectBytes(b);
  }
  for(unsigned offset:{102u,104u,106u}) {
    auto b=original;Put(b,64+offset,65535,2);Reseal(b);RejectBytes(b);
  }
  {auto b=original;Put(b,64+104,0,2);Reseal(b);RejectBytes(b);}
  {auto b=original;b[64+115]=1;Reseal(b);RejectBytes(b);} // language padding
  {auto b=original;b.push_back(0);Put(b,16,b.size(),4);Reseal(b);RejectBytes(b);}
  // Both unrepaired corruption and checksum-valid mutations must be admitted
  // semantically, never just accepted because the outer frame could validate.
  for(std::size_t i=0;i<original.size();++i) for(unsigned bit=0;bit<8;++bit) {
    auto b=original;b[i]^=std::uint8_t(1u<<bit);RejectBytes(b);
    if(i<64) Reseal(b,false);
    else if(i>=68) Reseal(b);
    else continue; // record length mutations need their own bounded oracle above.
    m::MessageSet out=empty,before=out;
    const auto e=m::DecodeMessageSet(b,&out);
    if(e==E::none) {B encoded;Check(m::EncodeMessageSet(out,&encoded)==E::none && encoded==b,"accepted resealed mutation lost canonical information");}
    else Check(e==E::malformed && out==before,"resealed invalid mutation modified output");
  }
  // Walk every actual allocation point in successful encoding and decoding.
  // A fail count is advanced only between calls, never inside codec logic.
  long encode_points=-1,decode_points=-1;
  for(long n=0;n<2000;++n) {
    B out={1},prior=out;fail_after=n;const auto e=m::EncodeMessageSet(base,&out);fail_after=-1;
    if(e==E::none) {Check(out==original,"allocation sweep encode bytes changed");encode_points=n;break;}
    Check(e==E::allocation_failure && out==prior,"encode allocation failure hid error or changed output");
  }
  for(long n=0;n<2000;++n) {
    auto out=empty;const auto before=out;fail_after=n;const auto e=m::DecodeMessageSet(original,&out);fail_after=-1;
    if(e==E::none) {Check(out==base,"allocation sweep decode lost values");decode_points=n;break;}
    Check(e==E::allocation_failure && out==before,"decode allocation failure hid error or changed output");
  }
  Check(encode_points>0 && decode_points>0,"allocation sweep did not reach successful terminal call");
  Check(m::EncodeMessageSet(base,nullptr)==E::malformed,"null encode output accepted");
  Check(m::DecodeMessageSet(original,nullptr)==E::malformed,"null decode output accepted");
  std::cout<<"message_vector_set checks="<<checks<<" failures="<<failures
           <<" encode_allocation_points="<<encode_points<<" decode_allocation_points="<<decode_points<<'\n';
  return failures?1:0;
}
