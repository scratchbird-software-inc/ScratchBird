// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "message_vector_value_codec.hpp"

#include <algorithm>
#include <array>
#include <new>
#include <stdexcept>
#include <string_view>

namespace scratchbird::wire::message_vector {
namespace {
using Bytes=std::span<const std::uint8_t>;
constexpr std::size_t kMaxBytes=256u*1024u;
std::uint16_t U16(Bytes b,std::size_t at) noexcept {
  return std::uint16_t(b[at]) | (std::uint16_t(b[at+1])<<8);
}
std::uint32_t U32(Bytes b,std::size_t at) noexcept {
  return std::uint32_t(U16(b,at)) | (std::uint32_t(U16(b,at+2))<<16);
}
std::uint64_t U64(Bytes b) noexcept {
  return std::uint64_t(U32(b,0)) | (std::uint64_t(U32(b,4))<<32);
}
std::size_t Align4(std::size_t n) noexcept { return (n+3u)&~std::size_t(3); }
bool Zero(Bytes b) noexcept {
  return std::all_of(b.begin(),b.end(),[](auto c){return c==0;});
}
bool Uuid(Bytes b,bool nullable=false) noexcept {
  return b.size()==16 && ((nullable && Zero(b)) ||
                         ((b[6]&0xf0)==0x70 && (b[8]&0xc0)==0x80));
}
bool Utf8(Bytes b) noexcept {
  for(std::size_t i=0;i<b.size();) {
    const auto c=b[i++];
    if(c==0) return false;
    if(c<0x80) continue;
    unsigned count=0; std::uint32_t scalar=0,minimum=0;
    if(c>=0xc2 && c<=0xdf) { count=1;scalar=c&0x1f;minimum=0x80; }
    else if(c>=0xe0 && c<=0xef) { count=2;scalar=c&0x0f;minimum=0x800; }
    else if(c>=0xf0 && c<=0xf4) { count=3;scalar=c&7;minimum=0x10000; }
    else return false;
    if(count>b.size()-i) return false;
    while(count--) {
      const auto next=b[i++];
      if((next&0xc0)!=0x80) return false;
      scalar=(scalar<<6)|(next&0x3f);
    }
    if(scalar<minimum || scalar>0x10ffff || (scalar>=0xd800 && scalar<=0xdfff))
      return false;
  }
  return true;
}
Bytes AsBytes(std::string_view value) noexcept {
  return {reinterpret_cast<const std::uint8_t*>(value.data()),value.size()};
}
bool Scalar(std::uint16_t type,Bytes b) noexcept {
  switch(type) {
    case 1: return Utf8(b);
    case 2: return b.size()==1 && b[0]<=1;
    case 3: case 4: return b.size()==8;
    case 5: return Uuid(b);
    case 6: return true;
    case 7: return b.empty();
    case 8: return b.size()>=16 && Uuid(b.first(16));
    default: return false;
  }
}
bool Padded(Bytes b,std::size_t* at,std::size_t len,Bytes* value) noexcept {
  if(*at>b.size() || len>b.size()-*at) return false;
  *value=b.subspan(*at,len);
  const auto end=*at+len, padded=Align4(end);
  if(padded>b.size() || !Zero(b.subspan(end,padded-end))) return false;
  *at=padded;
  return true;
}
struct EntryView { std::string_view key; std::uint16_t type; Bytes value; };
// No allocation and bounded before inspecting any variable section.
bool Entry(Bytes b,std::size_t* used,EntryView* out) noexcept {
  if(b.size()<8) return false;
  const auto key_len=U16(b,0),type=U16(b,2);
  const auto value_len=U32(b,4);
  if(key_len==0 || key_len>b.size()-8 || value_len>b.size()-8-key_len)
    return false;
  const auto end=std::size_t(8)+key_len+value_len,padded=Align4(end);
  if(padded>kMaxBytes || padded>b.size() || !Utf8(b.subspan(8,key_len)) ||
     !Zero(b.subspan(end,padded-end))) return false;
  *out={std::string_view(reinterpret_cast<const char*>(b.data()+8),key_len),
        type,b.subspan(8+key_len,value_len)};
  *used=padded;
  return true;
}
bool Parameter(Bytes b) noexcept {
  if(b.size()<32 || U32(b,0)>=64 || b[4]>11 || b[5]>3 || b[6]>7 ||
     b[7]!=0 || !Uuid(b.subspan(8,16))) return false;
  const auto type=U16(b,26);
  const bool encoding_matches=(b[6]==0 && type==8) || (b[6]==1 && type==5) ||
      (b[6]==2 && (type==3 || type==4)) || ((b[6]==3 || b[6]==4) && type==1) ||
      (b[6]==5 && type==7) || (b[6]==6 && type==2) || (b[6]==7 && type==6);
  if(!encoding_matches) return false;
  Bytes hint,value;std::size_t at=32;
  if(!Padded(b,&at,U16(b,24),&hint) || !Utf8(hint) ||
     !Padded(b,&at,U32(b,28),&value) || at!=b.size() || !Scalar(type,value)) return false;
  // A typed-value descriptor is the same canonical descriptor as the parameter.
  return type!=8 || std::equal(b.begin()+8,b.begin()+24,value.begin());
}
bool Detail(Bytes b) noexcept {
  if(b.size()<8 || b[0]>12 || b[1]>3) return false;
  std::size_t at=8;Bytes value;
  return Padded(b,&at,U32(b,4),&value) && at==b.size() && Scalar(U16(b,2),value);
}
bool Cause(Bytes b,std::string_view key) noexcept {
  if(b.size()<32 || U32(b,0)>=16 || !Uuid(b.subspan(4,16),true) || U32(b,28)!=0)
    return false;
  const auto ordinal=U32(b,0);
  if(ordinal<10) {
    if(key.size()!=1 || key[0]!=char('0'+ordinal)) return false;
  } else if(key.size()!=2 || key[0]!='1' || key[1]!=char('0'+ordinal-10)) return false;
  std::size_t at=32;
  for(std::size_t i=20;i<28;i+=2) {
    Bytes value;
    if(!Padded(b,&at,U16(b,i),&value) || value.empty() || !Utf8(value)) return false;
  }
  return at==b.size();
}
struct ContextField { std::string_view key;std::uint16_t type;bool nullable; };
constexpr std::array<ContextField,21> kContextFields{{
  {"audit_event_uuid",5,true},{"canonical_source_kind",4,false},
  {"catalog_generation",4,true},{"channel_uuid",5,true},{"database_uuid",5,true},
  {"donor_rendering_hint",1,true},{"donor_rendering_profile_uuid",5,true},
  {"execution_ref_uuid",5,true},{"finality_state",4,false},{"locale_uuid",5,true},
  {"message_template_uuid",5,true},{"policy_generation",4,true},
  {"redaction_policy_uuid",5,false},{"registry_epoch",4,false},
  {"required_outcome",1,false},{"retry_class",1,false},{"security_generation",4,true},
  {"server_uuid",5,true},{"session_uuid",5,true},{"source_map_ref",6,true},
  {"transaction_uuid",5,true}
}};
bool Context(Bytes b) noexcept {
  if(b.size()<8 || U16(b,0)!=1 || U16(b,2)!=kContextFields.size() || U32(b,4)!=0)
    return false;
  std::size_t at=8;
  for(const auto& field:kContextFields) {
    EntryView entry;std::size_t used=0;
    if(!Entry(b.subspan(at),&used,&entry) || entry.key!=field.key ||
       !(entry.type==field.type || (field.nullable && entry.type==7)) ||
       !Scalar(entry.type,entry.value)) return false;
    if(entry.type==4) {
      const auto number=U64(entry.value);
      if(field.key=="canonical_source_kind") { if(number>6) return false; }
      else if(field.key=="finality_state") { if(number>7) return false; }
      else if(number==0) return false;
    }
    if(entry.type==1 && !field.nullable && entry.value.empty()) return false;
    at+=used;
  }
  return at==b.size();
}
bool Valid(const EntryView& entry) noexcept {
  if(entry.key=="$context" && entry.type!=259) return false;
  switch(entry.type) {
    case 256: return Parameter(entry.value);
    case 257: return Detail(entry.value);
    case 258: return Cause(entry.value,entry.key);
    case 259: return entry.key=="$context" && Context(entry.value);
    default: return Scalar(entry.type,entry.value);
  }
}
void Put(std::vector<std::uint8_t>* b,std::size_t at,std::uint64_t n,unsigned width) {
  for(unsigned i=0;i<width;++i) (*b)[at+i]=std::uint8_t(n>>(i*8));
}
} // namespace

bool IsRegisteredSourceComponent(std::uint32_t code) noexcept { return code>=1 && code<=10; }
bool IsMessageVectorUtf8(Bytes value) noexcept { return Utf8(value); }
bool IsValidValueTlv(const ValueTlv& input) noexcept {
  if(input.key.empty() || input.key.size()>65535 || input.value.size()>kMaxBytes ||
     !Utf8(AsBytes(input.key))) return false;
  return Align4(8+input.key.size()+input.value.size())<=kMaxBytes &&
         Valid({input.key,static_cast<std::uint16_t>(input.type),input.value});
}

ValueCodecError EncodeValueTlv(const ValueTlv& input,std::vector<std::uint8_t>* output) noexcept {
  if(!output || !IsValidValueTlv(input)) return ValueCodecError::malformed;
  const auto raw=8+input.key.size()+input.value.size(),size=Align4(raw);
  try {
    std::vector<std::uint8_t> bytes(size,0);
    Put(&bytes,0,input.key.size(),2);Put(&bytes,2,static_cast<std::uint16_t>(input.type),2);
    Put(&bytes,4,input.value.size(),4);
    std::copy(input.key.begin(),input.key.end(),bytes.begin()+8);
    std::copy(input.value.begin(),input.value.end(),bytes.begin()+8+input.key.size());
    output->swap(bytes);
    return ValueCodecError::none;
  } catch(const std::bad_alloc&) { return ValueCodecError::allocation_failure; }
    catch(const std::length_error&) { return ValueCodecError::allocation_failure; }
}
ValueCodecError DecodeValueTlv(Bytes input,ValueTlv* output) noexcept {
  if(!output || input.size()>kMaxBytes) return ValueCodecError::malformed;
  EntryView view;std::size_t used=0;
  if(!Entry(input,&used,&view) || used!=input.size() || !Valid(view))
    return ValueCodecError::malformed;
  try {
    ValueTlv decoded{std::string(view.key),static_cast<ValueType>(view.type),
                     {view.value.begin(),view.value.end()}};
    output->key.swap(decoded.key);output->value.swap(decoded.value);output->type=decoded.type;
    return ValueCodecError::none;
  } catch(const std::bad_alloc&) { return ValueCodecError::allocation_failure; }
    catch(const std::length_error&) { return ValueCodecError::allocation_failure; }
}
} // namespace scratchbird::wire::message_vector
