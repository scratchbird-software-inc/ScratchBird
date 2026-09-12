// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/sblr/sblr_table_truncate_runtime.hpp"
#include "../common/single_tu_allocation_fault.hpp"
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <algorithm>
#include <iostream>
#include <string_view>
#ifdef SB_TRUNCATE_TEST_HASH_FAULT
namespace { bool hash_failure=false; }
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* data,size_t n,unsigned char* md,unsigned int* size,const EVP_MD* type,ENGINE* impl){
  return hash_failure?0:__real_EVP_Digest(data,n,md,size,type,impl);
}
#endif
namespace {
namespace s=scratchbird::engine::sblr;
using Bytes=std::vector<std::uint8_t>;
unsigned checks=0,failures=0;
void Check(bool ok,const char* why){++checks;if(!ok&&failures++<20)std::cerr<<why<<'\n';}
template<class B>void Put(B& b,std::size_t at,std::uint64_t v,unsigned n){
  for(unsigned i=0;i<n;++i)b[at+i]=static_cast<std::uint8_t>(v>>(8*i));
}
template<class B>void Id(B& b,std::size_t at,unsigned seed){
  for(unsigned i=0;i<16;++i)b[at+i]=static_cast<std::uint8_t>(seed+i);
  b[at+6]=0x7a;b[at+8]=0x9b;
}
// Independent specification-derived byte fixtures; no production field helper.
s::SblrTableTruncateDescriptorV1 Descriptor(){
  s::SblrTableTruncateDescriptorV1 d;
  for(auto at:{16,32,48,64,80,96,112,272,288,304,320})Id(d.canonical_body,at-16,at);
  for(auto at:{128,136,176,184})Put(d.canonical_body,at-16,0x100000001ull,8);
  Put(d.canonical_body,128,257,4);
  d.canonical_body[132]=1;d.canonical_body[133]=1;
  Put(d.canonical_body,136,2,4);Put(d.canonical_body,140,3,4);
  for(auto at:{208,240,336})for(unsigned i=0;i<32;++i)d.canonical_body[at-16+i]=static_cast<std::uint8_t>(at+i);
  d.availability_generation=0x100000003ull;return d;
}
s::SblrTableTruncateResultV1 Result(){
  s::SblrTableTruncateResultV1 r;
  for(auto at:{16,32,48,64,80,96})Id(r.canonical_body,at-16,at);
  Put(r.canonical_body,96,3,4);Put(r.canonical_body,112,1,4);Put(r.canonical_body,116,1,4);
  r.availability_generation=0x100000003ull;return r;
}
void Seal(Bytes& b,std::size_t end,std::string_view domain){
  Bytes material(domain.begin(),domain.end());material.insert(material.end(),b.begin()+16,b.begin()+end);
  Check(SHA256(material.data(),material.size(),b.data()+end)!=nullptr,"oracle SHA256 failed");
}
template<class T>Bytes Frame(const T& v,std::string_view magic,std::string_view domain){
  const auto end=16+v.canonical_body.size();Bytes b(end+40,0);
  std::copy(magic.begin(),magic.end(),b.begin());Put(b,4,1,2);Put(b,6,b.size(),2);Put(b,8,b.size(),4);
  std::copy(v.canonical_body.begin(),v.canonical_body.end(),b.begin()+16);
  Seal(b,end,domain);Put(b,end+32,v.availability_generation,8);return b;
}
template<class T>bool Same(const T&a,const T&b){
  return a.canonical_body==b.canonical_body&&a.evidence==b.evidence&&a.availability_generation==b.availability_generation;
}
template<class T,class E,class D>void Carrier(const T& value,Bytes expected,E encode,D decode,
    std::initializer_list<unsigned> ids,std::size_t reserved,std::string_view domain){
  const auto end=16+value.canonical_body.size();
  Check(encode(value)==expected,"independent exact carrier mismatch");
  T parsed=value;Check(decode(expected.data(),expected.size(),&parsed,nullptr),"valid decode rejected");
  Check(parsed.canonical_body==value.canonical_body&&std::equal(parsed.evidence.begin(),parsed.evidence.end(),expected.begin()+end),"decoded body/digest mismatch");
  Check(encode(parsed)==expected,"reencoding mismatch");
  auto reject=[&](const Bytes& b,std::size_t n){
    T out=parsed;Check(!decode(b.data(),n,&out,nullptr),"malformed carrier accepted");
    Check(Same(out,parsed),"failed decode mutated output");
  };
  for(std::size_t n=0;n<expected.size();++n)reject(expected,n);
  auto extra=expected;extra.push_back(0);reject(extra,extra.size());
  Check(!decode(nullptr,expected.size(),&parsed,nullptr),"null input accepted");
  Check(!decode(expected.data(),expected.size(),nullptr,nullptr),"null output accepted");
  {
    T out=parsed;std::string detail;bool threw=false,rejected=false;
    allocations_before_failure=0;
    try{rejected=!decode(nullptr,0,&out,&detail);}catch(...){threw=true;}
    allocations_before_failure=-1;
    Check(!threw&&rejected&&Same(out,parsed),"diagnostic allocation failure escaped or changed output");
  }
  for(std::size_t at=0;at<end+32;++at){auto b=expected;b[at]^=1;reject(b,b.size());}
  for(auto at:ids)for(unsigned v=0;v<256;++v){
    if((v&0xf0)!=0x70){auto b=expected;b[at+6]=v;Seal(b,end,domain);reject(b,b.size());}
    if((v&0xc0)!=0x80){auto b=expected;b[at+8]=v;Seal(b,end,domain);reject(b,b.size());}
  }
  for(auto at=reserved;at<end;++at){auto b=expected;b[at]=1;Seal(b,end,domain);reject(b,b.size());}
  auto bad=value;bad.canonical_body.fill(0);bad.canonical_body[0]=1;
  Check(encode(bad).empty(),"fabricated one-byte body encoded");
  bad=value;bad.availability_generation=0;Check(encode(bad).empty(),"zero availability encoded");
  bad=parsed;bad.evidence[0]^=1;Check(encode(bad).empty(),"wrong supplied hash encoded");
#ifdef SB_TRUNCATE_TEST_HASH_FAULT
  {
    auto out=parsed;hash_failure=true;
    const bool rejected_encode=encode(value).empty();
    const bool rejected_decode=!decode(expected.data(),expected.size(),&out,nullptr);
    hash_failure=false;
    Check(rejected_encode&&rejected_decode&&Same(out,parsed),"hash-provider failure published carrier");
  }
#endif
  for(bool encoding:{false,true}){
    bool finished=false;unsigned injected=0;
    for(int cut=0;cut<100;++cut){
      T out=parsed;bool ok=false,threw=false;allocations_before_failure=cut;
      try{ok=encoding?!encode(value).empty():decode(expected.data(),expected.size(),&out,nullptr);}
      catch(...){threw=true;}allocations_before_failure=-1;
      Check(!threw,"allocation failure escaped codec");
      if(ok){finished=true;break;}++injected;Check(Same(out,parsed),"allocation failure published output");
    }
    Check(finished&&injected>0,"allocation sweep incomplete");
  }
}
void Requests(){
  s::SblrTableTruncateRequestV1 q;Id(q.receipt,0,3);q.occurrence=0x100000002ull;q.truncate_occurrence=0xffffffffu;
  Bytes b(64,0);std::copy_n("TTRQ",4,b.begin());Put(b,4,1,2);Put(b,6,64,2);Put(b,8,64,4);
  std::copy(q.receipt.begin(),q.receipt.end(),b.begin()+16);Put(b,32,q.occurrence,8);Put(b,40,q.truncate_occurrence,4);
  Check(s::EncodeSblrTableTruncateRequestV1(q)==b,"independent exact request mismatch");
  auto reject=[&](const Bytes& v,std::size_t n){auto out=q;
    Check(!s::DecodeSblrTableTruncateRequestV1(v.data(),n,&out,nullptr),"invalid request accepted");
    Check(out.receipt==q.receipt&&out.occurrence==q.occurrence&&out.truncate_occurrence==q.truncate_occurrence,"request output changed");
  };
  for(std::size_t n=0;n<64;++n)reject(b,n);
  auto extra=b;extra.push_back(0);reject(extra,extra.size());
  for(unsigned at=0;at<16;++at){auto v=b;v[at]^=1;reject(v,v.size());}
  for(auto [at,width]:{std::pair{32u,8u},{40u,4u}}){auto v=b;Put(v,at,0,width);reject(v,v.size());}
  for(unsigned at=44;at<64;++at){auto v=b;v[at]=1;reject(v,v.size());}
  for(unsigned v=0;v<256;++v){
    if((v&0xf0)!=0x70){auto x=b;x[22]=v;reject(x,x.size());}
    if((v&0xc0)!=0x80){auto x=b;x[24]=v;reject(x,x.size());}
  }
  allocations_before_failure=0;bool threw=false;Bytes refused;
  try{refused=s::EncodeSblrTableTruncateRequestV1(q);}catch(...){threw=true;}
  allocations_before_failure=-1;Check(!threw&&refused.empty(),"request allocation failure escaped/published");
  q.receipt[6]=0x4a;Check(s::EncodeSblrTableTruncateRequestV1(q).empty(),"request UUIDv4 encoded");
}
void Fields(){
  const auto original=Descriptor();
  auto bad=[&](unsigned at,std::uint64_t n,unsigned width){
    auto d=original;Put(d.canonical_body,at-16,n,width);
    auto b=Frame(d,"TTRD","ScratchBird.SblrTableTruncateDescriptor.V1");auto out=original;
    Check(s::EncodeSblrTableTruncateDescriptorV1(d,false).empty(),"invalid field encoded");
    Check(!s::DecodeSblrTableTruncateDescriptorV1(b.data(),b.size(),&out,nullptr,false),"resealed invalid field accepted");
    Check(Same(out,original),"invalid field output changed");
  };
  for(auto at:{128,136,176,184})bad(at,0,8);
  bad(144,0,4);for(auto at:{148,149}){bad(at,0,1);bad(at,3,1);}
  bad(150,2,2);bad(150,1,2);bad(192,1,8);
  bad(152,0,4);bad(152,4,4);bad(156,1,4);bad(156,65536,4);
  for(auto at:{160,164,168,172})bad(at,1048577,4);
  bad(172,1,4);
  for(auto at:{208,240,336}){auto d=original;std::fill_n(d.canonical_body.begin()+at-16,32,0);Check(s::EncodeSblrTableTruncateDescriptorV1(d,false).empty(),"zero proof digest encoded");}
  auto full=original;full.canonical_body[132]=2;full.canonical_body[133]=2;
  Put(full.canonical_body,134,1,2);Put(full.canonical_body,176,1,8);
  Put(full.canonical_body,136,65535,4);Put(full.canonical_body,140,65535,4);
  for(auto at:{160,164,168,172})Put(full.canonical_body,at-16,1048576,4);
  Put(full.canonical_body,184,~std::uint64_t{0},8);
  Check(!s::EncodeSblrTableTruncateDescriptorV1(full,false).empty(),"valid boundary fields rejected");
  for(auto [at,n]:{std::pair{112u,0u},{112u,65536u},{116u,1048577u},{128u,0u},{128u,2u},{132u,0u},{132u,3u}}){
    auto r=Result();Put(r.canonical_body,at-16,n,4);auto b=Frame(r,"TTRS","ScratchBird.SblrTableTruncateResult.V1");auto out=Result();
    Check(s::EncodeSblrTableTruncateResultV1(r).empty(),"invalid result field encoded");
    Check(!s::DecodeSblrTableTruncateResultV1(b.data(),b.size(),&out,nullptr),"invalid result field decoded");
  }
}
}
int main(){
  Requests();
  for(bool op:{false,true}){
    auto d=Descriptor();
    Carrier(d,Frame(d,op?"TTRO":"TTRD","ScratchBird.SblrTableTruncateDescriptor.V1"),
      [=](const auto&v){return s::EncodeSblrTableTruncateDescriptorV1(v,op);},
      [=](const std::uint8_t*b,std::size_t n,s::SblrTableTruncateDescriptorV1*v,std::string*e){return s::DecodeSblrTableTruncateDescriptorV1(b,n,v,e,op);},
      {16,32,48,64,80,96,112,272,288,304,320},368,"ScratchBird.SblrTableTruncateDescriptor.V1");
  }
  Carrier(Result(),Frame(Result(),"TTRS","ScratchBird.SblrTableTruncateResult.V1"),
    s::EncodeSblrTableTruncateResultV1,s::DecodeSblrTableTruncateResultV1,
    {16,32,48,64,80,96},136,"ScratchBird.SblrTableTruncateResult.V1");
  Fields();std::cout<<"truncate_codec checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
