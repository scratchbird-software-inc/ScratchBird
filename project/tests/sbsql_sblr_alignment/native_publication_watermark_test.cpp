// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_publication_watermark.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <source_location>
#include <stdexcept>
namespace {long allocation_budget=-1;bool counting=false;unsigned long allocations=0;unsigned hash_fault=0,hash_target=1,hash_seen=0;bool hash_active=false;}
void* operator new(std::size_t n){if(counting)++allocations;if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){hash_active=hash_fault&&++hash_seen==hash_target;if(hash_active&&hash_fault==1){hash_fault=0;return nullptr;}return __real_EVP_MD_CTX_new();}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* m,ENGINE* e){if(hash_active&&hash_fault==2){hash_fault=0;return 0;}return __real_EVP_DigestInit_ex(c,m,e);}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* b,size_t n){if(hash_active&&hash_fault==3){hash_fault=0;return 0;}return __real_EVP_DigestUpdate(c,b,n);}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* b,unsigned int* n){if(hash_active&&hash_fault==4){hash_fault=0;return 0;}const int r=__real_EVP_DigestFinal_ex(c,b,n);if(hash_active&&hash_fault==5){hash_fault=0;*n=31;}return r;}
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* data,size_t bytes,unsigned char* out,unsigned int* size,const EVP_MD* md,ENGINE* engine) {
  const bool selected=hash_fault&&++hash_seen==hash_target;
  const auto mode=selected?hash_fault:0;
  if(selected)hash_fault=0;
  if(mode&&mode!=5)return 0;
  const auto result=__real_EVP_Digest(data,bytes,out,size,md,engine);
  if(mode==5)*size=31;
  return result;
}

namespace {
namespace db=scratchbird::storage::database;namespace d=scratchbird::storage::disk;
using namespace scratchbird::core::platform;
using Bytes=std::vector<byte>;using E=db::NativePublicationWatermarkError;unsigned checks=0;
void Check(bool ok,const char* why,std::source_location at=std::source_location::current()) {
  ++checks;if(!ok)throw std::runtime_error(std::string(why)+" line="+std::to_string(at.line()));
}
Uuid Id(byte n){Uuid id;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=n;return id;}
void Num(Bytes& b,std::size_t at,unsigned n,u64 v){for(unsigned i=0;i<n;++i)b[at+i]=static_cast<byte>(v>>(8*i));}
void Put(Bytes& b,std::size_t at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}
void Ref(Bytes& b,std::size_t at,const d::NativePageReference& r) {
  Put(b,at,r.filespace_uuid);Num(b,at+16,8,r.page_number);Num(b,at+24,8,r.page_generation);Put(b,at+32,r.page_size_profile_uuid);
}
void Seal(Bytes& b,bool state=true) {
  std::array<byte,32> sha{};
  if(state){Check(SHA256(b.data()+128,240,sha.data())!=nullptr,"independent state SHA256");std::copy(sha.begin(),sha.end(),b.begin()+368);}
  std::fill(b.begin()+400,b.begin()+432,0);
  Check(SHA256(b.data(),b.size(),sha.data())!=nullptr,"independent full SHA256");std::copy(sha.begin(),sha.end(),b.begin()+400);
}
Bytes Oracle(const db::NativePublicationWatermark& s) {
  const auto& h=s.header;Bytes b(h.page_size_bytes,0);
  std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);
  Num(b,16,4,h.page_type);Num(b,20,2,1);Num(b,22,2,1);
  Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);
  Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Num(b,88,8,h.flags);
  Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);
  u64 fnv=14695981039346656037ull;for(unsigned i=0;i<128;++i){fnv^=b[i];fnv*=1099511628211ull;}Num(b,96,8,fnv);
  std::copy_n("SBPAG001",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,384);Num(b,140,4,512);
  Put(b,144,s.object_uuid);Put(b,160,s.bootstrap_uuid);Put(b,176,s.timeline_uuid);
  Num(b,192,8,s.watermark);Num(b,200,8,s.base_checkpoint_generation);Num(b,208,8,s.base_root_set_generation);Num(b,216,8,s.previous_watermark);
  Put(b,224,s.operation_uuid);Ref(b,240,s.base_checkpoint);Put(b,288,s.base_checkpoint_object_uuid);
  std::copy(s.base_checkpoint_sha256.begin(),s.base_checkpoint_sha256.end(),b.begin()+304);
  std::copy(s.previous_state_sha256.begin(),s.previous_state_sha256.end(),b.begin()+336);Seal(b);return b;
}
db::NativePublicationWatermark Example(unsigned p=0) {
  const auto& profile=d::kCanonicalFilespacePageProfiles[p];db::NativePublicationWatermark s;
  s.header={profile.page_size_bytes,0x500,Id(1),Id(2),Id(3),21,7,0,profile.uuid};
  s.object_uuid=Id(4);s.bootstrap_uuid=Id(5);s.timeline_uuid=Id(6);s.operation_uuid=Id(7);
  s.watermark=s.base_checkpoint_generation=s.base_root_set_generation=1;
  s.base_checkpoint={Id(2),19,11,profile.uuid};s.base_checkpoint_object_uuid=Id(8);s.base_checkpoint_sha256.fill(13);return s;
}
auto Other(db::NativePublicationWatermark s){s.header.page_uuid=Id(9);s.header.page_number=22;return s;}
auto Next(db::NativePublicationWatermark s) {
  const auto b=Oracle(s);std::copy_n(b.begin()+368,32,s.previous_state_sha256.begin());
  s.previous_watermark=s.watermark++;s.operation_uuid.bytes[15]+=16;return s;
}
void Empty(const db::NativePublicationWatermarkImage& r) {
  Check(!r.ok()&&!r.state&&r.bytes.empty()&&std::all_of(r.state_sha256.begin(),r.state_sha256.end(),[](byte v){return !v;}),
        "no state/image/digest on failure");
}
void Invalid(const db::NativePublicationWatermark& s){Empty(db::EncodeNativePublicationWatermark(s));Empty(db::DecodeNativePublicationWatermark(Oracle(s)));}
void Test() {
  for(unsigned p=0;p<5;++p)for(bool advanced:{false,true}) {
    auto s=Example(p);if(advanced)s=Next(s);
    const auto first=Oracle(s),second=Oracle(Other(s));const auto encoded=db::EncodeNativePublicationWatermark(s);
    Check(encoded.ok()&&encoded.bytes==first,"exact independent encoding");
    const auto decoded=db::DecodeNativePublicationWatermark(first);
    Check(decoded.ok()&&Oracle(*decoded.state)==first,"all fields preserved");
    Check(db::DecodeNativePublicationWatermark(second).state_sha256==decoded.state_sha256,"state digest excludes slot identity");
    Check(!std::equal(first.begin()+400,first.begin()+432,second.begin()+400),"full seal binds slot identity");
    for(bool reverse:{false,true}) {
      const auto pair=db::ClassifyNativePublicationWatermarkPair(reverse?second:first,reverse?first:second);
      Check(pair.ok()&&pair.state->watermark==s.watermark,"stable pair independent of order");
    }
    auto torn=second;torn.back()^=1;
    auto pair=db::ClassifyNativePublicationWatermarkPair(first,torn);Empty(pair);Check(pair.error==E::repair_required,"one damaged replica requires recovery");
    pair=db::ClassifyNativePublicationWatermarkPair(torn,torn);Empty(pair);Check(pair.error==E::invalid_pair,"two damaged replicas");
    const auto newer=Oracle(Other(Next(s)));
    for(bool reverse:{false,true}) {
      pair=db::ClassifyNativePublicationWatermarkPair(reverse?newer:first,reverse?first:newer);
      Empty(pair);Check(pair.error==E::repair_required,"adjacent publication cannot self-promote");
    }
  }
  const auto first=Oracle(Example()),second=Oracle(Other(Example()));
  for(std::size_t i=0;i<first.size();++i){auto b=first;b[i]^=1;Empty(db::DecodeNativePublicationWatermark(b));}
  for(unsigned at:{128u,136u,138u,140u,432u,511u,512u,8191u}){auto b=first;b[at]^=1;Seal(b);Empty(db::DecodeNativePublicationWatermark(b));}
  auto b=first;b[368]^=1;Seal(b,false);Empty(db::DecodeNativePublicationWatermark(b));
  for(std::size_t n:{0u,127u,511u,8191u,8193u}){b=first;b.resize(n);Empty(db::DecodeNativePublicationWatermark(b));}
  for(unsigned i=0;i<25;++i) {
    auto s=Example();switch(i) {
      case 0:s.header.page_type=0x30d;break;case 1:s.header.flags=2;break;
      case 2:s.object_uuid={};break;case 3:s.bootstrap_uuid={};break;case 4:s.timeline_uuid={};break;
      case 5:s.operation_uuid={};break;case 6:s.base_checkpoint_object_uuid={};break;
      case 7:s.watermark=0;break;case 8:s.base_checkpoint_generation=0;break;case 9:s.base_root_set_generation=0;break;
      case 10:s.base_checkpoint_generation=2;break;case 11:s.base_checkpoint_sha256.fill(0);break;
      case 12:s.previous_watermark=1;break;case 13:s.previous_state_sha256.fill(1);break;
      case 14:s=Next(s);s.previous_watermark=0;break;case 15:s=Next(s);s.previous_state_sha256.fill(0);break;
      case 16:s.base_checkpoint.page_number=0;break;case 17:s.base_checkpoint.page_number=s.header.page_number;break;
      case 18:s.base_checkpoint.page_generation=0;break;case 19:s.base_checkpoint.page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;break;
      case 20:s.base_checkpoint.filespace_uuid={};break;case 21:s.base_checkpoint.page_number=UINT64_MAX;break;
      case 22:s.object_uuid=s.bootstrap_uuid;break;case 23:s.header.page_uuid=s.bootstrap_uuid;break;case 24:s.header.page_uuid=s.object_uuid;break;
    }Invalid(s);
  }
  for(unsigned version=0;version<16;++version)if(version!=7){auto s=Example();s.operation_uuid.bytes[6]=static_cast<byte>(version<<4);Invalid(s);}
  for(unsigned variant:{0u,0x40u,0xc0u}){auto s=Example();s.operation_uuid.bytes[8]=static_cast<byte>(variant);Invalid(s);}
  for(unsigned i=0;i<15;++i) {
    auto s=Other(Example());switch(i) {
      case 0:s.header.page_number=21;break;case 1:s.header.page_uuid=Id(3);break;case 2:s.header.database_uuid=Id(42);break;
      case 3:s.header.filespace_uuid=Id(42);break;case 4:s.object_uuid=Id(42);break;case 5:s.bootstrap_uuid=Id(42);break;
      case 6:s.timeline_uuid=Id(42);break;case 7:s.operation_uuid=Id(42);break;case 8:s.base_checkpoint_sha256[0]^=1;break;
      case 9:s.base_checkpoint_object_uuid=Id(42);break;case 10:s.base_checkpoint.page_number=21;break;
      case 11:s=Next(s);s.previous_state_sha256[0]^=1;break;case 12:s=Next(s);s.operation_uuid=Id(7);break;
      case 13:s=Next(s);s.base_root_set_generation=2;break;case 14:s=Next(Next(s));break;
    }
    const auto image=Oracle(s);Check(db::DecodeNativePublicationWatermark(image).ok(),"pair control individually valid");
    const auto pair=db::ClassifyNativePublicationWatermarkPair(first,image);Empty(pair);Check(pair.error==E::invalid_pair,"pair lineage/alias disagreement");
  }
  auto older=Next(Example()),newer=Next(Other(older));newer.base_checkpoint_generation=2;newer.base_root_set_generation=2;
  newer.base_checkpoint.page_number=25;newer.base_checkpoint_object_uuid=Id(32);newer.base_checkpoint_sha256.fill(34);
  auto pair=db::ClassifyNativePublicationWatermarkPair(Oracle(older),Oracle(newer));Empty(pair);
  Check(pair.error==E::repair_required,"legal base advancement still requires recovery");
  older=newer;older.header=Example().header;
  for(unsigned defect=0;defect<5;++defect) {
    newer=Next(Other(older));
    switch(defect) {
      case 0:newer.base_checkpoint_generation=1;break;
      case 1:newer.base_root_set_generation=1;break;
      case 2:newer.base_checkpoint_generation=newer.watermark;break;
      case 3:newer.base_checkpoint.page_number=27;break;
      case 4:newer.base_checkpoint_sha256[1]^=1;break;
    }
    const auto candidate=Oracle(newer);
    Check(db::DecodeNativePublicationWatermark(candidate).ok(),"base control individually valid");
    pair=db::ClassifyNativePublicationWatermarkPair(Oracle(older),candidate);Empty(pair);
    Check(pair.error==E::invalid_pair,"base regression/future/mismatched immutable base refused");
  }
  auto maximum=Example();maximum.watermark=UINT64_MAX;maximum.previous_watermark=UINT64_MAX-1;maximum.previous_state_sha256.fill(45);
  Check(db::EncodeNativePublicationWatermark(maximum).ok(),"max generation representable without increment");
  allocation_budget=0;auto no_memory=db::EncodeNativePublicationWatermark(Example());allocation_budget=-1;
  Empty(no_memory);Check(no_memory.error==E::resource_exhausted,"encoder allocation fault");
  allocation_budget=0;no_memory=db::DecodeNativePublicationWatermark(first);allocation_budget=-1;
  Empty(no_memory);Check(no_memory.error==E::resource_exhausted,"decoder retained-image allocation fault");
  allocations=0;counting=true;const auto measured=db::ClassifyNativePublicationWatermarkPair(first,second);counting=false;
  Check(measured.ok(),"measure pair");const auto count=allocations;
  for(unsigned long at=0;at<=count;++at){allocation_budget=at;const auto r=db::ClassifyNativePublicationWatermarkPair(first,second);allocation_budget=-1;
    if(!r.ok())Empty(r);if(at==count)Check(r.ok(),"complete allocation sweep");}
  for(unsigned mode=1;mode<=5;++mode)for(unsigned target=1;target<=4;++target){
    hash_fault=mode;hash_target=target;hash_seen=0;hash_active=false;const auto r=db::ClassifyNativePublicationWatermarkPair(first,second);
    Check(hash_fault==0,"decode hash fault consumed");Empty(r);Check(r.error==E::hash_failure,"backend failure not durable damage");
  }
  for(unsigned mode=1;mode<=5;++mode)for(unsigned target=1;target<=2;++target){
    hash_fault=mode;hash_target=target;hash_seen=0;hash_active=false;const auto r=db::EncodeNativePublicationWatermark(Example());
    Check(hash_fault==0,"encode hash fault consumed");Empty(r);Check(r.error==E::hash_failure,"encoder does not invent zero digest");
  }
  std::cout<<"PASS native publication watermark checks="<<checks<<" allocations="<<count<<" image_only=true\n";
}
}
int main(){try{Test();return 0;}catch(const std::exception& e){allocation_budget=-1;hash_fault=0;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<"\n";return 1;}}
