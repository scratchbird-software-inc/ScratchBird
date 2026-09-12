// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "message_vector_set_codec.hpp"

#include <algorithm>
#include <new>
#include <set>
#include <stdexcept>
#include <string_view>

namespace scratchbird::wire::message_vector {
namespace {
using Bytes=std::span<const std::uint8_t>;
using Buffer=std::vector<std::uint8_t>;
using Error=ValueCodecError;
constexpr std::size_t kSetLimit=1024u*1024u,kRecordLimit=256u*1024u;
std::uint16_t U16(Bytes b,std::size_t i) noexcept {
  return std::uint16_t(b[i])|(std::uint16_t(b[i+1])<<8);
}
std::uint32_t U32(Bytes b,std::size_t i) noexcept {
  return std::uint32_t(U16(b,i))|(std::uint32_t(U16(b,i+2))<<16);
}
std::uint64_t U64(Bytes b,std::size_t i) noexcept {
  return std::uint64_t(U32(b,i))|(std::uint64_t(U32(b,i+4))<<32);
}
std::size_t Align4(std::size_t n) noexcept {return (n+3u)&~std::size_t(3);}
bool Zero(Bytes b) noexcept {return std::all_of(b.begin(),b.end(),[](auto x){return x==0;});}
bool Uuid(Bytes b,bool nullable=false) noexcept {
  return b.size()==16 && ((nullable && Zero(b)) || ((b[6]&0xf0)==0x70 && (b[8]&0xc0)==0x80));
}
Bytes TextBytes(std::string_view s) noexcept {
  return {reinterpret_cast<const std::uint8_t*>(s.data()),s.size()};
}
void Put(Buffer& b,std::size_t at,std::uint64_t n,unsigned width) {
  for(unsigned i=0;i<width;++i) b[at+i]=std::uint8_t(n>>(8*i));
}
void PutUuid(Buffer& b,std::size_t at,const MessageUuid& uuid) {
  std::copy(uuid.begin(),uuid.end(),b.begin()+at);
}
MessageUuid GetUuid(Bytes b,std::size_t at) {
  MessageUuid id;std::copy_n(b.begin()+at,16,id.begin());return id;
}
std::uint32_t Crc(Bytes b,std::size_t zero_at=std::size_t(-1)) noexcept {
  std::uint32_t crc=0xffffffff;
  for(std::size_t i=0;i<b.size();++i) {
    crc^=(i>=zero_at && i-zero_at<4)?0:b[i];
    for(unsigned bit=0;bit<8;++bit) crc=(crc>>1)^((crc&1)?0x82f63b78u:0u);
  }
  return ~crc;
}
struct ContextSummary {
  std::uint64_t kind=0,finality=0,registry=0,policy=0;
};
// Called only after complete value-codec validation. Retain context bytes in
// MessageRecord; this projection exists solely for cross-header consistency.
ContextSummary InspectContext(const ValueTlv& context) noexcept {
  ContextSummary result;Bytes b=context.value;
  for(std::size_t at=8;at<b.size();) {
    const auto key_len=U16(b,at),type=U16(b,at+2);
    const auto value_len=U32(b,at+4);
    const std::string_view key(reinterpret_cast<const char*>(b.data()+at+8),key_len);
    if(type==4) {
      const auto value=U64(b,at+8+key_len);
      if(key=="canonical_source_kind") result.kind=value;
      else if(key=="finality_state") result.finality=value;
      else if(key=="registry_epoch") result.registry=value;
      else if(key=="policy_generation") result.policy=value;
    }
    at+=Align4(8+key_len+value_len);
  }
  return result;
}
bool ClassMatches(const MessageRecord& r,std::uint64_t kind) noexcept {
  switch(kind) {
    case 0:
      switch(r.severity) {
        case 0: case 8: return r.message_class==2;
        case 1: return r.message_class==1;
        case 2: case 3: case 4: case 5: case 6: case 9: case 10:
          return r.message_class==0;
        case 7: case 11: case 12: return r.message_class==7;
        default: return false;
      }
    case 1: return r.message_class==2 || r.message_class==5;
    case 2: return r.message_class==3;
    case 3: return r.message_class==4;
    case 4: return r.message_class==5;
    case 5: return r.message_class==6;
    case 6: return r.message_class==7;
    default: return false;
  }
}
bool RecordValid(const MessageRecord& r,std::uint64_t epoch,ContextSummary* summary) noexcept {
  if(r.flags!=1 || r.message_class>7 || r.severity>12 || r.retryability>2 ||
     r.redaction_state>3 || !IsRegisteredSourceComponent(r.source_component) ||
     !Uuid(r.vector_uuid) || !Uuid(r.canonical_source_uuid,true) ||
     !Uuid(r.request_uuid,true) || !Uuid(r.correlation_uuid,true) ||
     r.parameters.size()>64 || r.details.size()>64 || r.causes.size()>16 ||
     r.context.type!=ValueType::context || !IsValidValueTlv(r.context)) return false;
  *summary=InspectContext(r.context);
  if(summary->registry!=epoch || summary->policy!=r.policy_generation ||
     !ClassMatches(r,summary->kind) || r.text[0].empty() || r.text[2].empty() ||
     (summary->kind==0 && (r.text[1].empty() || !Uuid(r.canonical_source_uuid)))) return false;
  for(const auto& s:r.text) if(s.size()>65535 || !IsMessageVectorUtf8(TextBytes(s))) return false;
  for(std::size_t i=0;i<r.parameters.size();++i) {
    const auto& v=r.parameters[i];
    if(v.type!=ValueType::parameter || !IsValidValueTlv(v) || U32(v.value,0)!=i) return false;
  }
  for(const auto& v:r.details) if(v.type!=ValueType::detail || !IsValidValueTlv(v)) return false;
  for(std::size_t i=0;i<r.causes.size();++i) {
    const auto& v=r.causes[i];
    if(v.type!=ValueType::cause || !IsValidValueTlv(v) || U32(v.value,0)!=i) return false;
  }
  return true;
}
bool SetHeaderValid(const MessageSet& s) noexcept {
  return (s.flags&~15u)==0 && s.registry_generation!=0 && Uuid(s.set_uuid) &&
      s.max_render_bytes>0 && s.max_render_bytes<=kSetLimit &&
      s.records.size()<=(kSetLimit-64)/112 && (!s.records.empty() || s.flags==0);
}
bool FlagsMatch(std::uint32_t flags,bool redacted,bool finality,bool notification) noexcept {
  return bool(flags&1)==redacted && bool(flags&2)==finality && (!(flags&4) || notification);
}
Error AppendTlv(const ValueTlv& v,Buffer& out) {
  Buffer encoded;const auto status=EncodeValueTlv(v,&encoded);
  if(status!=Error::none) return status;
  if(encoded.size()>kRecordLimit-out.size()) return Error::malformed;
  out.insert(out.end(),encoded.begin(),encoded.end());return Error::none;
}
Error EncodeRecord(const MessageRecord& r,Buffer& b) {
  b.assign(112,0);Put(b,8,1,2);b[10]=r.message_class;b[11]=r.severity;Put(b,12,r.flags,4);
  PutUuid(b,16,r.vector_uuid);PutUuid(b,32,r.canonical_source_uuid);
  PutUuid(b,48,r.request_uuid);PutUuid(b,64,r.correlation_uuid);
  Put(b,80,r.policy_generation,8);Put(b,88,r.source_component,4);
  for(unsigned i=0;i<5;++i) Put(b,92+2*i,r.text[i].size(),2);
  Put(b,102,r.parameters.size(),2);Put(b,104,r.details.size()+1,2);Put(b,106,r.causes.size(),2);
  b[108]=r.retryability;b[109]=r.redaction_state;
  for(const auto& s:r.text) {
    if(Align4(s.size())>kRecordLimit-b.size()) return Error::malformed;
    b.insert(b.end(),s.begin(),s.end());b.resize(Align4(b.size()),0);
  }
  for(const auto& v:r.parameters) {const auto e=AppendTlv(v,b);if(e!=Error::none) return e;}
  if(const auto e=AppendTlv(r.context,b);e!=Error::none) return e;
  for(const auto& v:r.details) {const auto e=AppendTlv(v,b);if(e!=Error::none) return e;}
  for(const auto& v:r.causes) {const auto e=AppendTlv(v,b);if(e!=Error::none) return e;}
  Put(b,0,b.size(),4);Put(b,4,Crc(b,4),4);return Error::none;
}
Error ReadTlv(Bytes b,std::size_t* at,ValueTlv* v) {
  if(*at>b.size() || b.size()-*at<8) return Error::malformed;
  const auto key_len=U16(b,*at);
  const auto value_len=U32(b,*at+4);
  if(key_len>b.size()-*at-8 || value_len>b.size()-*at-8-key_len) return Error::malformed;
  const auto size=Align4(8+std::size_t(key_len)+value_len);
  if(size>b.size()-*at) return Error::malformed;
  const auto result=DecodeValueTlv(b.subspan(*at,size),v);
  if(result==Error::none) *at+=size;
  return result;
}
Error DecodeRecord(Bytes b,MessageRecord& r) {
  if(b.size()<112 || b.size()>kRecordLimit || b.size()%4 || U32(b,0)!=b.size() ||
     U32(b,4)!=Crc(b,4) || U16(b,8)!=1 || U32(b,12)!=1 || U16(b,110)!=0 ||
     U16(b,102)>64 || U16(b,104)==0 || U16(b,104)>65 || U16(b,106)>16)
    return Error::malformed;
  r.message_class=b[10];r.severity=b[11];r.flags=U32(b,12);
  r.vector_uuid=GetUuid(b,16);r.canonical_source_uuid=GetUuid(b,32);
  r.request_uuid=GetUuid(b,48);r.correlation_uuid=GetUuid(b,64);
  r.policy_generation=U64(b,80);r.source_component=U32(b,88);
  r.retryability=b[108];r.redaction_state=b[109];
  std::size_t at=112;
  for(unsigned i=0;i<5;++i) {
    const auto len=U16(b,92+2*i);
    if(len>b.size()-at || Align4(len)>b.size()-at ||
       !Zero(b.subspan(at+len,Align4(len)-len))) return Error::malformed;
    r.text[i].assign(reinterpret_cast<const char*>(b.data()+at),len);at+=Align4(len);
  }
  for(unsigned i=0;i<U16(b,102);++i) {
    ValueTlv v;const auto e=ReadTlv(b,&at,&v);if(e!=Error::none) return e;
    r.parameters.push_back(std::move(v));
  }
  if(const auto e=ReadTlv(b,&at,&r.context);e!=Error::none) return e;
  for(unsigned i=1;i<U16(b,104);++i) {
    ValueTlv v;const auto e=ReadTlv(b,&at,&v);if(e!=Error::none) return e;
    r.details.push_back(std::move(v));
  }
  for(unsigned i=0;i<U16(b,106);++i) {
    ValueTlv v;const auto e=ReadTlv(b,&at,&v);if(e!=Error::none) return e;
    r.causes.push_back(std::move(v));
  }
  return at==b.size()?Error::none:Error::malformed;
}
} // namespace

ValueCodecError EncodeMessageSet(const MessageSet& input,Buffer* output) noexcept {
  if(!output || !SetHeaderValid(input)) return Error::malformed;
  try {
    Buffer bytes(64,0);std::set<MessageUuid> ids;
    bool redacted=false,finality=false,notification=false;
    for(const auto& r:input.records) {
      ContextSummary summary;
      if(!RecordValid(r,input.registry_generation,&summary) || !ids.insert(r.vector_uuid).second)
        return Error::malformed;
      redacted|=r.redaction_state!=0;finality|=summary.finality!=0 || r.message_class==6;
      notification|=r.message_class==3;
      Buffer record;
      const auto e=EncodeRecord(r,record);if(e!=Error::none) return e;
      if(record.size()>kSetLimit-bytes.size()) return Error::malformed;
      bytes.insert(bytes.end(),record.begin(),record.end());
    }
    if(!FlagsMatch(input.flags,redacted,finality,notification)) return Error::malformed;
    const auto records_crc=input.records.empty()?0:Crc(Bytes(bytes).subspan(64));
    Put(bytes,0,0x564d4253,4);Put(bytes,4,64,2);Put(bytes,6,1,2);
    Put(bytes,8,input.flags,4);Put(bytes,12,input.records.size(),4);Put(bytes,16,bytes.size(),4);
    Put(bytes,20,records_crc,4);Put(bytes,24,input.registry_generation,8);
    PutUuid(bytes,32,input.set_uuid);Put(bytes,48,input.max_render_bytes,4);
    Put(bytes,52,Crc(Bytes(bytes).first(64),52),4);
    output->swap(bytes);return Error::none;
  } catch(const std::bad_alloc&) {return Error::allocation_failure;}
    catch(const std::length_error&) {return Error::allocation_failure;}
}
ValueCodecError DecodeMessageSet(Bytes input,MessageSet* output) noexcept {
  if(!output || input.size()<64 || input.size()>kSetLimit || U32(input,0)!=0x564d4253 ||
     U16(input,4)!=64 || U16(input,6)!=1 || U32(input,16)!=input.size() ||
     !Zero(input.subspan(56,8)) || U32(input,52)!=Crc(input.first(64),52)) return Error::malformed;
  const auto count=U32(input,12),records_crc=U32(input,20);
  if(count>(input.size()-64)/112 ||
     (count==0 && (input.size()!=64 || records_crc!=0)) ||
     (count!=0 && records_crc!=Crc(input.subspan(64)))) return Error::malformed;
  try {
    MessageSet decoded;decoded.flags=U32(input,8);decoded.registry_generation=U64(input,24);
    decoded.set_uuid=GetUuid(input,32);decoded.max_render_bytes=U32(input,48);
    // Validate header without mistaking the not-yet-decoded records for an empty set.
    if((decoded.flags&~15u)!=0 || decoded.registry_generation==0 || !Uuid(decoded.set_uuid) ||
       decoded.max_render_bytes==0 || decoded.max_render_bytes>kSetLimit ||
       (count==0 && decoded.flags!=0)) return Error::malformed;
    std::set<MessageUuid> ids;std::size_t at=64;
    bool redacted=false,finality=false,notification=false;
    for(std::uint32_t i=0;i<count;++i) {
      if(input.size()-at<112) return Error::malformed;
      const auto len=U32(input,at);if(len>input.size()-at) return Error::malformed;
      MessageRecord r;const auto e=DecodeRecord(input.subspan(at,len),r);
      if(e!=Error::none) return e;
      ContextSummary summary;
      if(!RecordValid(r,decoded.registry_generation,&summary) || !ids.insert(r.vector_uuid).second)
        return Error::malformed;
      redacted|=r.redaction_state!=0;finality|=summary.finality!=0 || r.message_class==6;
      notification|=r.message_class==3;
      decoded.records.push_back(std::move(r));at+=len;
    }
    if(at!=input.size() || !FlagsMatch(decoded.flags,redacted,finality,notification))
      return Error::malformed;
    using std::swap;swap(*output,decoded);return Error::none;
  } catch(const std::bad_alloc&) {return Error::allocation_failure;}
    catch(const std::length_error&) {return Error::allocation_failure;}
}
} // namespace scratchbird::wire::message_vector
