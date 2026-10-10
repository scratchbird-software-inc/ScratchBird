// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "openpgp_armor.hpp"
#include "../datatypes/canonical_utf8.hpp"
#include <openssl/crypto.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>

namespace scratchbird::core::crypto {
namespace {
constexpr std::string_view begin="-----BEGIN PGP MESSAGE-----\n\n";
constexpr std::string_view end="-----END PGP MESSAGE-----\n";
constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
bool Extent(PgpInput b) noexcept {return (!b.size||b.data)&&b.size<=UINTPTR_MAX-reinterpret_cast<std::uintptr_t>(b.data);}
bool Valid(PgpInput in,PgpOutput out) noexcept {
  if(!Extent(in)||!Extent({out.data,out.size}))return false;
  if(!in.size||!out.size)return true;
  auto a=reinterpret_cast<std::uintptr_t>(in.data),b=reinterpret_cast<std::uintptr_t>(out.data);
  return !(a<b+out.size&&b<a+in.size);
}
bool Cancel(PgpCancellation p){return p.requested&&p.requested(p.context);}
bool Space(unsigned char c){return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\v'||c=='\f';}
int Digit(unsigned char c){
  if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;
  if(c>='0'&&c<='9')return c-'0'+52;return c=='+'?62:c=='/'?63:-1;
}
struct Pending {
  PgpOutput out;bool accepted=false;
  ~Pending(){if(!accepted&&out.size)OPENSSL_cleanse(out.data,out.size);}
};
struct Reader {
  PgpInput in;PgpCancellation probe;std::size_t at=0;bool cancelled=false;std::string_view raw;
  bool Line(std::string_view& line){
    if(Cancel(probe)){cancelled=true;return false;}
    if(at==in.size)return false;
    const auto first=at;
    while(at<in.size&&in.data[at]!='\n') {
      if(!(at%4096)&&Cancel(probe)){cancelled=true;return false;}++at;
    }
    line={reinterpret_cast<const char*>(in.data+first),at-first};
    raw=line;
    if(at<in.size)++at;
    while(!line.empty()&&Space(line.back())){
      if(!(line.size()%4096)&&Cancel(probe)){cancelled=true;return false;}line.remove_suffix(1);
    }
    return true;
  }
};
// One allocation-free scan serves inspection and actual decoding. Checked
// output capacity prevents malformed inputs from escaping the staging extent.
PgpSize Scan(PgpInput input,PgpOutput output,bool writing,PgpCancellation probe){
  if(Cancel(probe))return {PgpCode::cancelled,0};
  Reader reader{input,probe};std::string_view line,label;
  while(reader.Line(line)&&line.empty()){}
  if(reader.cancelled)return {PgpCode::cancelled,0};
  for(auto name:{"MESSAGE","PUBLIC KEY BLOCK","PRIVATE KEY BLOCK","SIGNATURE"}) {
    std::string_view candidate=name;
    if(line.size()==15+candidate.size()+5 && line.substr(0,15)=="-----BEGIN PGP " &&
       line.substr(15,candidate.size())==candidate&&line.substr(15+candidate.size())=="-----")label=candidate;
  }
  if(label.empty())return {PgpCode::invalid_packet,0};
  bool separator=false;
  while(reader.Line(line)) {
    if(line.empty()){separator=true;break;}
    std::size_t colon=0;
    while(colon<line.size()&&line[colon]!=':'){
      if(!(colon%4096)&&Cancel(probe))return {PgpCode::cancelled,0};
      const auto c=static_cast<unsigned char>(line[colon]);
      if(c<33||c>126)return {PgpCode::invalid_packet,0};++colon;
    }
    if(colon==line.size()||!colon||colon+1>=reader.raw.size()||reader.raw[colon+1]!=' ')return {PgpCode::invalid_packet,0};
    std::size_t i=std::min(colon+2,line.size()),last=i;std::uint32_t scalar=0;
    while(i<line.size()){
      if(i-last>=4096){if(Cancel(probe))return {PgpCode::cancelled,0};last=i;}
      if(!scratchbird::core::datatypes::DecodeCanonicalUtf8Scalar(
          reinterpret_cast<const std::uint8_t*>(line.data()),line.size(),&i,&scalar))return {PgpCode::invalid_packet,0};
    }
  }
  if(reader.cancelled)return {PgpCode::cancelled,0};
  if(!separator)return {PgpCode::invalid_packet,0};
  struct Quantum {std::array<unsigned,4> bytes{};~Quantum(){OPENSSL_cleanse(bytes.data(),sizeof(bytes));}} quantum;
  auto& q=quantum.bytes;unsigned used=0;bool padded=false,checksum=false,tail=false;
  std::size_t count=0;
  while(reader.Line(line)) {
    if(line.size()==13+label.size()+5&&line.substr(0,13)=="-----END PGP "&&
       line.substr(13,label.size())==label&&line.substr(13+label.size())=="-----"){tail=true;break;}
    auto trimmed=line;while(!trimmed.empty()&&Space(trimmed.front())){
      if(!(trimmed.size()%4096)&&Cancel(probe))return {PgpCode::cancelled,0};trimmed.remove_prefix(1);
    }
    if(trimmed.empty())continue;
    if(!used&&!checksum&&trimmed.front()=='='){checksum=true;continue;} // RFC9580: ignore even malformed CRC footer.
    if(checksum)return {PgpCode::invalid_packet,0};
    for(std::size_t i=0;i<line.size();++i) {
      if(!(i%4096)&&Cancel(probe))return {PgpCode::cancelled,0};
      const auto ch=static_cast<unsigned char>(line[i]);if(Space(ch))continue;
      if(padded)return {PgpCode::invalid_packet,0};
      const auto digit=Digit(ch);if(ch!='='&&digit<0)return {PgpCode::invalid_packet,0};
      q[used++]=ch=='='?64:static_cast<unsigned>(digit);
      if(used!=4)continue;
      if(q[0]==64||q[1]==64||(q[2]==64&&q[3]!=64)||
         (q[2]==64&&(q[1]&15))||(q[3]==64&&q[2]!=64&&(q[2]&3)))return {PgpCode::invalid_packet,0};
      const std::size_t n=q[2]==64?1:q[3]==64?2:3;
      if(count>std::numeric_limits<std::size_t>::max()-n)return {PgpCode::size_overflow,0};
      if(writing){
        if(count>output.size||n>output.size-count)return {PgpCode::invalid_extent,0};
        output.data[count]=(q[0]<<2)|(q[1]>>4);
        if(n>1)output.data[count+1]=(q[1]<<4)|(q[2]>>2);
        if(n>2)output.data[count+2]=(q[2]<<6)|q[3];
      }
      count+=n;used=0;padded=n!=3;
    }
  }
  if(reader.cancelled)return {PgpCode::cancelled,0};
  if(!tail||used)return {PgpCode::invalid_packet,0};
  while(reader.Line(line))if(!line.empty())return {PgpCode::invalid_packet,0};
  if(reader.cancelled||Cancel(probe))return {PgpCode::cancelled,0};
  return {PgpCode::ok,count};
}
} // namespace
PgpSize ArmorEncodedSize(std::size_t input) noexcept {
  constexpr auto max=std::numeric_limits<std::size_t>::max();
  const auto groups=input/3+(input%3!=0);
  if(groups>max/4)return {PgpCode::size_overflow,0};
  const auto encoded=groups*4,lines=encoded/64+(encoded%64!=0);
  if(encoded>max-lines||encoded+lines>max-begin.size()-end.size())return {PgpCode::size_overflow,0};
  return {PgpCode::ok,begin.size()+encoded+lines+end.size()};
}
PgpSize ArmorDecodedSize(PgpInput input,PgpCancellation p){
  if(!Extent(input))return {PgpCode::invalid_extent,0};
  return Scan(input,{},false,p);
}
PgpCode EncodeArmor(PgpInput in,PgpOutput out,PgpCancellation p){
  if(!Valid(in,out))return PgpCode::invalid_extent;
  const auto size=ArmorEncodedSize(in.size);if(size.code!=PgpCode::ok)return size.code;
  if(out.size!=size.bytes)return PgpCode::invalid_extent;
  Pending pending{out};if(Cancel(p))return PgpCode::cancelled;
  std::memcpy(out.data,begin.data(),begin.size());std::size_t at=begin.size();unsigned column=0;
  for(std::size_t i=0;i<in.size;) {
    if(!(i%4095)&&Cancel(p))return PgpCode::cancelled;
    const auto n=std::min(std::size_t{3},in.size-i);
    const std::uint32_t v=(std::uint32_t(in.data[i])<<16)|(n>1?std::uint32_t(in.data[i+1])<<8:0)|(n>2?in.data[i+2]:0);
    out.data[at++]=alphabet[v>>18];out.data[at++]=alphabet[(v>>12)&63];
    out.data[at++]=n>1?alphabet[(v>>6)&63]:'=';out.data[at++]=n>2?alphabet[v&63]:'=';
    i+=n;column+=4;if(column==64||i==in.size){out.data[at++]='\n';column=0;}
  }
  std::memcpy(out.data+at,end.data(),end.size());
  if(Cancel(p))return PgpCode::cancelled;
  pending.accepted=true;return PgpCode::ok;
}
PgpCode DecodeArmor(PgpInput in,PgpOutput out,PgpCancellation p){
  if(!Valid(in,out))return PgpCode::invalid_extent;
  Pending pending{out};const auto size=Scan(in,out,true,p);
  if(size.code!=PgpCode::ok)return size.code;
  if(size.bytes!=out.size)return PgpCode::invalid_extent;
  pending.accepted=true;return PgpCode::ok;
}
} // namespace scratchbird::core::crypto
