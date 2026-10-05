// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_publication_watermark.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
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
  if(state){if(b[136]>=2&&b[136]<=5){const std::string_view domain=b[136]==5?"SBPAGST5":b[136]==4?"SBPAGST4":b[136]==3?"SBPAGST3":"SBPAGST2";Bytes material(domain.begin(),domain.end());
      material.insert(material.end(),b.begin()+128,b.begin()+368);material.insert(material.end(),b.begin()+432,b.begin()+(b[136]>=3?768:640));
      Check(SHA256(material.data(),material.size(),sha.data())!=nullptr,"independent intent state SHA256");
    }else Check(SHA256(b.data()+128,240,sha.data())!=nullptr,"independent state SHA256");
    std::copy(sha.begin(),sha.end(),b.begin()+368);}
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
  const bool profiled=s.intent&&s.intent->recovery_profile;
  const bool startup=s.intent&&s.intent->startup_binding;
  std::copy_n(startup?"SBPAG005":profiled?"SBPAG004":s.publication_plan?"SBPAG003":s.intent?"SBPAG002":"SBPAG001",8,b.begin()+128);
  Num(b,136,2,startup?5:profiled?4:s.publication_plan?3:s.intent?2:1);Num(b,138,2,(startup||profiled||s.publication_plan)?640:s.intent?512:384);Num(b,140,4,(startup||profiled||s.publication_plan)?768:s.intent?640:512);
  Put(b,144,s.object_uuid);Put(b,160,s.bootstrap_uuid);Put(b,176,s.timeline_uuid);
  Num(b,192,8,s.watermark);Num(b,200,8,s.base_checkpoint_generation);Num(b,208,8,s.base_root_set_generation);Num(b,216,8,s.previous_watermark);
  Put(b,224,s.operation_uuid);Ref(b,240,s.base_checkpoint);Put(b,288,s.base_checkpoint_object_uuid);
  std::copy(s.base_checkpoint_sha256.begin(),s.base_checkpoint_sha256.end(),b.begin()+304);
  std::copy(s.previous_state_sha256.begin(),s.previous_state_sha256.end(),b.begin()+336);
  if(s.intent){const auto& i=*s.intent;Put(b,432,i.initiator_uuid);Put(b,448,i.request_context_uuid);Put(b,464,i.policy_snapshot_uuid);
    std::copy(i.normalized_request_sha256.begin(),i.normalized_request_sha256.end(),b.begin()+480);Num(b,512,2,i.initiator_kind);Num(b,514,2,1);}
  if(s.publication_plan){const auto& p=*s.publication_plan;Ref(b,516,p.page);Put(b,564,p.object_uuid);
    std::copy(p.sha256.begin(),p.sha256.end(),b.begin()+580);std::copy(p.reservation_state_sha256.begin(),p.reservation_state_sha256.end(),b.begin()+612);}
  if(profiled)Num(b,644,2,s.intent->recovery_profile);
  if(s.abandonment){Num(b,646,2,s.intent->recovery_profile);Put(b,648,s.abandonment->resolution_uuid);std::copy(s.abandonment->pending_state_sha256.begin(),s.abandonment->pending_state_sha256.end(),b.begin()+664);}
  if(startup){const auto& binding=*s.intent->startup_binding;
    Put(b,696,binding.operation_uuid);Put(b,712,binding.session_uuid);Put(b,728,binding.transaction_uuid);
    Num(b,744,8,binding.local_transaction_id);Num(b,752,8,binding.fence_generation);}
  Seal(b);return b;
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
template<class Call> auto NoHeap(Call&& call){
  allocations=0;counting=true;allocation_budget=0;auto result=call();
  const auto count=allocations;const auto left=allocation_budget;
  allocation_budget=-1;counting=false;
  Check(count==0&&left==0,"bounded watermark makes no C++ allocation on success or failure");return result;
}
template<class Result> void EmptyFixed(const Result& r){
  Check(!r.ok()&&!r.state&&std::all_of(r.state_sha256.begin(),r.state_sha256.end(),[](byte v){return !v;}),
    "fixed watermark failure has no state or digest prefix");
  if constexpr(requires{r.bytes;})Check(r.bytes.empty(),"failed bounded watermark has no usable image");
}
void DecodeParity(const Bytes& image){
  const auto expected=db::DecodeNativePublicationWatermark(image);
  const auto actual=NoHeap([&]{return db::DecodeNativePublicationWatermarkValue(image);});
  Check(actual.error==expected.error,"fixed decode preserves exact error class");
  if(actual.ok())Check(Oracle(*actual.state)==image&&actual.state_sha256==expected.state_sha256,"fixed decode retains all fields");
  else EmptyFixed(actual);
}
void Invalid(const db::NativePublicationWatermark& s){
  const auto expected=db::EncodeNativePublicationWatermark(s);Empty(expected);
  Bytes output(d::kCanonicalFilespacePageProfiles.back().page_size_bytes,0xa5);
  const auto actual=NoHeap([&]{return db::EncodeNativePublicationWatermarkInto(s,output);});
  EmptyFixed(actual);Check(actual.error==expected.error,"bounded malformed input preserves exact error");
  const auto image=Oracle(s);Empty(db::DecodeNativePublicationWatermark(image));DecodeParity(image);
}
void Bounded(const db::NativePublicationWatermark& state){
  const auto expected=Oracle(state),other=Oracle(Other(state));
  Bytes output(expected.size()+2,0xa5);auto span=std::span(output).subspan(1,expected.size());
  const auto check_image=[&](const auto& result){Check(result.ok()&&result.bytes.data()==span.data()&&
    result.bytes.size()==expected.size()&&std::equal(result.bytes.begin(),result.bytes.end(),expected.begin())&&
    Oracle(*result.state)==expected&&std::equal(result.state_sha256.begin(),result.state_sha256.end(),expected.begin()+368),
    "bounded encoding exact independent bytes and complete value");};
  check_image(NoHeap([&]{return db::EncodeNativePublicationWatermarkInto(state,span);}));
  Check(output.front()==0xa5&&output.back()==0xa5,"unaligned encoding preserves prefix and suffix");
  const auto wide=NoHeap([&]{return db::EncodeNativePublicationWatermarkInto(state,std::span(output).subspan(1));});
  check_image(wide);Check(output.back()==0xa5,"oversized destination keeps suffix unchanged");
  for(const auto size:{std::size_t(0),std::size_t(1),expected.size()-1}){
    std::fill(output.begin(),output.end(),0xa5);
    const auto r=NoHeap([&]{return db::EncodeNativePublicationWatermarkInto(state,span.first(size));});
    EmptyFixed(r);Check(r.error==E::resource_exhausted&&std::all_of(output.begin(),output.end(),[](byte v){return v==0xa5;}),"short backing refuses before output write");
  }
  auto alias=state;std::array<byte,sizeof(alias)> before{};std::memcpy(before.data(),&alias,sizeof(alias));
  for(auto offset:{std::size_t(0),sizeof(alias)-1}){
    const auto r=NoHeap([&]{return db::EncodeNativePublicationWatermarkInto(alias,{reinterpret_cast<byte*>(&alias)+offset,sizeof(alias)-offset});});
    EmptyFixed(r);Check(r.error==E::invalid_backing&&std::memcmp(before.data(),&alias,sizeof(alias))==0,"entire input value excluded before modification");
  }
  // A suffix alias is forbidden even when the written physical prefix would
  // not itself overwrite the input. Use a real aligned object in valid storage.
  constexpr auto suffix_offset=d::kCanonicalFilespacePageProfiles.back().page_size_bytes;
  std::vector<std::max_align_t> shared((suffix_offset+sizeof(state)+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t));
  auto* bytes=reinterpret_cast<byte*>(shared.data());auto* suffix=std::construct_at(reinterpret_cast<db::NativePublicationWatermark*>(bytes+suffix_offset),state);
  const Bytes shared_before(bytes,bytes+shared.size()*sizeof(std::max_align_t));
  const auto overlap=NoHeap([&]{return db::EncodeNativePublicationWatermarkInto(*suffix,{bytes,shared_before.size()});});
  EmptyFixed(overlap);Check(overlap.error==E::invalid_backing&&std::equal(shared_before.begin(),shared_before.end(),bytes),"suffix input alias preserves all bytes");std::destroy_at(suffix);
  DecodeParity(expected);
  for(const auto size:{std::size_t(0),std::size_t(127),expected.size()-1,expected.size()+1}){
    auto short_image=expected;short_image.resize(size);DecodeParity(short_image);
  }
  for(const auto at:{std::size_t(0),std::size_t(128),std::size_t(136),std::size_t(368),std::size_t(400),std::size_t(767),expected.size()-1}){
    auto corrupt=expected;corrupt[at]^=1;DecodeParity(corrupt);
    if(at>=128){Seal(corrupt);DecodeParity(corrupt);}
  }
  auto torn=other;torn.back()^=1;
  auto next=Next(Other(state));next.publication_plan.reset();next.abandonment.reset();
  const auto next_bytes=Oracle(next);
  for(const auto* first:std::array<const Bytes*,2>{&expected,&torn})for(const auto* second:std::array<const Bytes*,4>{&other,&expected,&torn,&next_bytes}){
    const auto expected_pair=db::ClassifyNativePublicationWatermarkPair(*first,*second);
    const auto actual=NoHeap([&]{return db::ClassifyNativePublicationWatermarkPairValue(*first,*second);});
    Check(actual.error==expected_pair.error,"fixed stable damaged adjacent and conflicting pair parity");
    if(actual.ok())Check(Oracle(*actual.state)==*first&&actual.state_sha256==expected_pair.state_sha256,"stable pair returns exact first state");else EmptyFixed(actual);
  }
  const unsigned sites=2+state.publication_plan.has_value()+state.abandonment.has_value();
  for(unsigned route=0;route<3;++route)for(unsigned mode=1;mode<=5;++mode)for(unsigned site=1;site<=sites*(route==2?2u:1u);++site){
    hash_fault=mode;hash_target=site;hash_seen=0;hash_active=false;
    if(!route){const auto r=NoHeap([&]{return db::EncodeNativePublicationWatermarkInto(state,span);});EmptyFixed(r);Check(r.error==E::hash_failure&&!hash_fault,"every bounded encoder hash fault consumed");}
    else {const auto r=NoHeap([&]{return route==1?db::DecodeNativePublicationWatermarkValue(expected):db::ClassifyNativePublicationWatermarkPairValue(expected,other);});EmptyFixed(r);Check(r.error==E::hash_failure&&!hash_fault,"every fixed decoder/pair hash fault consumed");}
    hash_active=false;check_image(NoHeap([&]{return db::EncodeNativePublicationWatermarkInto(state,span);}));
    const auto decoded=NoHeap([&]{return db::DecodeNativePublicationWatermarkValue(expected);});
    const auto pair=NoHeap([&]{return db::ClassifyNativePublicationWatermarkPairValue(expected,other);});
    Check(decoded.ok()&&pair.ok()&&decoded.state_sha256==pair.state_sha256,"exact retry after each provider fault");
  }
}
void BoundedTests(){
  for(unsigned profile=0;profile<5;++profile){Bounded(Example(profile));Bounded(Next(Example(profile)));
    for(unsigned kind=0;kind<6;++kind)for(bool attached:{false,true})for(bool resolved:{false,true}){
      if(resolved&&(kind!=1&&kind!=2))continue;
      auto state=Next(Example(profile));state.intent=db::NativePublicationIntent{Id(40),Id(41),Id(42),{},4,u16(kind==5?2:kind)};
      state.intent->normalized_request_sha256.fill(43);
      if(kind==5)state.intent->startup_binding=db::NativeStartupBinding{Id(50),Id(51),Id(52),13,19};
      if(attached){const auto origin=Oracle(state);db::NativePublicationWatermark::PlanAnchor anchor;
        anchor.page={Id(2),30,2,state.header.page_size_profile_uuid};anchor.object_uuid=Id(44);anchor.sha256.fill(45);
        std::copy_n(origin.begin()+368,32,anchor.reservation_state_sha256.begin());state.publication_plan=anchor;}
      if(resolved){const auto pending=Oracle(state);state.abandonment=db::NativePublicationWatermark::Abandonment{Id(46),{}};
        std::copy_n(pending.begin()+368,32,state.abandonment->pending_state_sha256.begin());}
      Bounded(state);
    }
    auto overflow=Example(profile);overflow.base_checkpoint.page_number=u64(std::numeric_limits<std::streamoff>::max())/overflow.header.page_size_bytes;
    Invalid(overflow);
  }
}
void IntentTests(){
  for(unsigned profile=0;profile<5;++profile){auto state=Next(Example(profile));
    db::NativePublicationIntent intent;intent.initiator_uuid=Id(40);intent.request_context_uuid=Id(41);intent.policy_snapshot_uuid=Id(42);
    intent.normalized_request_sha256.fill(43);intent.initiator_kind=4;state.intent=intent;
    const auto bytes=Oracle(state),other=Oracle(Other(state));
    const auto encoded=db::EncodeNativePublicationWatermark(state);Check(encoded.ok()&&encoded.bytes==bytes,"independent complete intent bytes");
    const auto decoded=db::DecodeNativePublicationWatermark(bytes);Check(decoded.ok()&&decoded.state->intent==state.intent&&Oracle(*decoded.state)==bytes,"binary intent retained");
    Check(db::ClassifyNativePublicationWatermarkPair(bytes,other).ok(),"stable intent replicas");
    for(unsigned field=0;field<11;++field){auto bad=state;
      if(field==0)bad.intent->initiator_uuid={};if(field==1)bad.intent->request_context_uuid={};if(field==2)bad.intent->policy_snapshot_uuid={};
      if(field==3)bad.intent->initiator_uuid.bytes[6]=0x40;if(field==4)bad.intent->request_context_uuid.bytes[8]=0;
      if(field==5)bad.intent->policy_snapshot_uuid.bytes[6]=0x40;if(field==6)bad.intent->normalized_request_sha256={};
      if(field==7)bad.intent->initiator_kind=0;if(field==8)bad.intent->initiator_kind=9;if(field==9)bad.intent->initiator_kind=65535;
      if(field==10){bad.watermark=1;bad.previous_watermark=0;bad.previous_state_sha256={};}Invalid(bad);}
    for(unsigned kind=1;kind<=8;++kind){auto s=state;s.intent->initiator_kind=kind;Check(db::EncodeNativePublicationWatermark(s).bytes==Oracle(s),"all declared initiator kinds");}
    for(unsigned at=516;at<640;++at){auto bad=bytes;bad[at]=1;Seal(bad);Empty(db::DecodeNativePublicationWatermark(bad));}
    for(unsigned at:{135u,136u,138u,140u,514u,515u}){auto bad=bytes;bad[at]^=8;Seal(bad);Empty(db::DecodeNativePublicationWatermark(bad));}
    for(unsigned at:{447u,463u,479u,511u,512u}){auto bad=bytes;bad[at]^=1;Seal(bad,false);const auto r=db::DecodeNativePublicationWatermark(bad);
      Empty(r);Check(r.error==E::invalid_integrity,"intent bound by state digest, not only outer seal");}
    auto without=state;without.intent.reset();auto alias=Oracle(without);alias[135]='2';Num(alias,136,2,2);Num(alias,138,2,512);Num(alias,140,4,640);Num(alias,514,2,1);Seal(alias);Empty(db::DecodeNativePublicationWatermark(alias));
    auto different=Other(state);different.intent->normalized_request_sha256[0]^=1;
    auto pair=db::ClassifyNativePublicationWatermarkPair(bytes,Oracle(different));Empty(pair);Check(pair.error==E::invalid_pair,"same generation cannot change request");
    auto newer=Next(Other(state));pair=db::ClassifyNativePublicationWatermarkPair(bytes,Oracle(newer));Empty(pair);
    Check(pair.error==E::invalid_pair,"pending intent cannot be displaced against old checkpoint");
    newer.base_checkpoint_generation=state.watermark;newer.base_root_set_generation++;newer.base_checkpoint.page_number=25;
    pair=db::ClassifyNativePublicationWatermarkPair(bytes,Oracle(newer));Empty(pair);Check(pair.error==E::repair_required,"selected prior intent allows next request but pair needs repair");
    const auto old=Oracle(Example(profile));pair=db::ClassifyNativePublicationWatermarkPair(old,other);Empty(pair);
    Check(pair.error==E::repair_required,"version1 to intent transition retains exact previous state");
    for(unsigned mode=0;mode<3;++mode){bool success=false;
      for(long n=0;n<20;++n){allocation_budget=n;const auto r=mode==0?db::EncodeNativePublicationWatermark(state):mode==1?db::DecodeNativePublicationWatermark(bytes):db::ClassifyNativePublicationWatermarkPair(bytes,other);allocation_budget=-1;
        if(r.ok()){success=true;break;}Empty(r);Check(r.error==E::resource_exhausted,"intent allocation failure");}Check(success,"intent allocation sweep complete");
      for(unsigned fault=1;fault<=5;++fault)for(unsigned target=1;target<=(mode==2?4u:2u);++target){hash_fault=fault;hash_target=target;hash_seen=0;hash_active=false;
        const auto r=mode==0?db::EncodeNativePublicationWatermark(state):mode==1?db::DecodeNativePublicationWatermark(bytes):db::ClassifyNativePublicationWatermarkPair(bytes,other);
        Check(hash_fault==0&&r.error==E::hash_failure,"intent state/image provider fault");Empty(r);}
    }
  }
}
void AnchorTests(){
  for(unsigned profile=0;profile<5;++profile){auto origin=Next(Example(profile));
    db::NativePublicationIntent intent;intent.initiator_uuid=Id(40);intent.request_context_uuid=Id(41);intent.policy_snapshot_uuid=Id(42);
    intent.normalized_request_sha256.fill(43);intent.initiator_kind=4;origin.intent=intent;
    const auto bare=Oracle(origin);auto state=origin;
    db::NativePublicationWatermark::PlanAnchor anchor;anchor.page={state.header.filespace_uuid,40,1,state.header.page_size_profile_uuid};
    anchor.object_uuid=Id(50);anchor.sha256.fill(51);std::copy_n(bare.begin()+368,32,anchor.reservation_state_sha256.begin());state.publication_plan=anchor;
    const auto bytes=Oracle(state),other=Oracle(Other(state));
    const auto encoded=db::EncodeNativePublicationWatermark(state);Check(encoded.ok()&&encoded.bytes==bytes,"independent version3 complete bytes");
    const auto decoded=db::DecodeNativePublicationWatermark(bytes);Check(decoded.ok()&&decoded.state->publication_plan==state.publication_plan&&Oracle(*decoded.state)==bytes,"anchor roundtrip");
    Check(db::ClassifyNativePublicationWatermarkPair(bytes,other).ok(),"stable anchored replicas");
    for(bool reverse:{false,true}){const auto pair=db::ClassifyNativePublicationWatermarkPair(reverse?bare:bytes,reverse?other:Oracle(Other(origin)));
      Check(pair.error==E::repair_required,"structural same-generation origin transition");Empty(pair);}
    for(unsigned at=644;at<768;++at){auto bad=bytes;bad[at]=1;Seal(bad);Empty(db::DecodeNativePublicationWatermark(bad));}
    for(unsigned at:{612u,643u}){auto bad=bytes;bad[at]^=1;Seal(bad);Empty(db::DecodeNativePublicationWatermark(bad));}
    for(unsigned field=0;field<14;++field){auto bad=state;auto& a=*bad.publication_plan;
      switch(field){case 0:a.page.page_number=0;break;case 1:a.page.page_generation=0;break;
        case 2:a.page.page_number=state.header.page_number;break;case 3:a.page=state.base_checkpoint;break;
        case 4:a.page.filespace_uuid=Id(60);break;case 5:a.page.page_size_profile_uuid=Id(61);break;
        case 6:a.object_uuid={};break;case 7:a.object_uuid.bytes[6]=0x40;break;
        case 8:a.object_uuid=state.object_uuid;break;case 9:a.object_uuid=state.bootstrap_uuid;break;
        case 10:a.sha256={};break;case 11:a.reservation_state_sha256={};break;
        case 12:a.reservation_state_sha256[0]^=1;break;case 13:bad.intent.reset();break;}Invalid(bad);}
    auto alias=state;alias.publication_plan->page.page_number=Other(state).header.page_number;
    Check(db::EncodeNativePublicationWatermark(alias).ok(),"other slot alias individually shape valid");
    Check(db::ClassifyNativePublicationWatermarkPair(Oracle(alias),other).error==E::invalid_pair,"anchor cannot own other replica");
    auto conflicting=Other(state);conflicting.publication_plan->sha256[0]^=1;
    Check(db::ClassifyNativePublicationWatermarkPair(bytes,Oracle(conflicting)).error==E::invalid_pair,"same watermark cannot replace plan");
    auto newer=Next(Other(state));newer.publication_plan.reset();newer.base_checkpoint_generation=state.watermark;
    newer.base_root_set_generation=2;newer.base_checkpoint.page_number=25;
    const auto next_bare=Oracle(newer);Check(db::ClassifyNativePublicationWatermarkPair(bytes,next_bare).error==E::repair_required,"next reservation retains complete anchored lineage");
    newer.publication_plan=anchor;std::copy_n(next_bare.begin()+368,32,newer.publication_plan->reservation_state_sha256.begin());
    Check(db::ClassifyNativePublicationWatermarkPair(bytes,Oracle(newer)).error==E::invalid_pair,"new generation cannot arrive already anchored");
    for(unsigned mode=0;mode<3;++mode){bool success=false;
      for(long n=0;n<20;++n){allocation_budget=n;const auto r=mode==0?db::EncodeNativePublicationWatermark(state):mode==1?db::DecodeNativePublicationWatermark(bytes):db::ClassifyNativePublicationWatermarkPair(bytes,other);allocation_budget=-1;
        if(r.ok()){success=true;break;}Empty(r);Check(r.error==E::resource_exhausted,"anchor allocation failure");}Check(success,"anchor allocation sweep complete");
      for(unsigned fault=1;fault<=5;++fault)for(unsigned target=1;target<=(mode==2?6u:3u);++target){hash_fault=fault;hash_target=target;hash_seen=0;hash_active=false;
        const auto r=mode==0?db::EncodeNativePublicationWatermark(state):mode==1?db::DecodeNativePublicationWatermark(bytes):db::ClassifyNativePublicationWatermarkPair(bytes,other);
        Check(hash_fault==0&&r.error==E::hash_failure,"anchor origin/state/image hash fault");Empty(r);}
    }
  }
}
void ResolutionTests(){
 for(u16 recovery_profile:{u16(1),u16(2)})for(unsigned profile=0;profile<5;++profile)for(unsigned attached=0;attached<2;++attached){auto pending=Next(Example(profile));
  pending.intent=db::NativePublicationIntent{Id(40),Id(41),Id(42),{},4,recovery_profile};pending.intent->normalized_request_sha256.fill(43);
  if(attached){const auto origin=Oracle(pending);db::NativePublicationWatermark::PlanAnchor anchor;
   anchor.page={Id(2),30,2,pending.header.page_size_profile_uuid};anchor.object_uuid=Id(44);anchor.sha256.fill(45);std::copy_n(origin.begin()+368,32,anchor.reservation_state_sha256.begin());pending.publication_plan=anchor;
  }
  const auto before=Oracle(pending);auto abandoned=pending;db::NativePublicationWatermark::Abandonment resolution;resolution.resolution_uuid=Id(46);std::copy_n(before.begin()+368,32,resolution.pending_state_sha256.begin());abandoned.abandonment=resolution;
  for(unsigned resolved=0;resolved<2;++resolved){const auto& state=resolved?abandoned:pending;const auto bytes=Oracle(state),other=Oracle(Other(state));
   const auto encoded=db::EncodeNativePublicationWatermark(state);Check(encoded.ok()&&encoded.bytes==bytes,"independent exact profiled watermark bytes");const auto decoded=db::DecodeNativePublicationWatermark(bytes);
   Check(decoded.ok()&&decoded.state->intent==state.intent&&decoded.state->abandonment==state.abandonment&&decoded.state->publication_plan==state.publication_plan&&Oracle(*decoded.state)==bytes,"complete profiled state round trip");
   Check(db::ClassifyNativePublicationWatermarkPair(bytes,other).ok(),"equal canonical profiled pair");
   for(const auto offset:{644u,646u,696u,767u}){auto wrong=bytes;wrong[offset]=offset==644?5:offset==646?3-recovery_profile:1;Seal(wrong);Empty(db::DecodeNativePublicationWatermark(wrong));}
   if(!resolved){auto wrong=bytes;Put(wrong,648,Id(47));Seal(wrong);Empty(db::DecodeNativePublicationWatermark(wrong));}
   for(unsigned route=0;route<3;++route){const auto call=[&]{return route==0?db::EncodeNativePublicationWatermark(state):route==1?db::DecodeNativePublicationWatermark(bytes):db::ClassifyNativePublicationWatermarkPair(bytes,other);};
    bool completed=false;for(long at=0;at<20;++at){allocation_budget=at;const auto result=call();const auto left=allocation_budget;allocation_budget=-1;if(result.ok()){Check(left==0,"complete profiled allocation sweep");completed=true;break;}Empty(result);Check(left==-1&&result.error==E::resource_exhausted,"every profiled allocation fault consumed and preserved");}Check(completed,"profiled allocation sweep terminates");
    const unsigned hashes=(2+attached+resolved)*(route==2?2:1);for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto result=call();Check(!hash_fault&&result.error==E::hash_failure,"all profiled origin/resolution/state/image hash faults preserved");Empty(result);}
   }
  }
  Check(db::ClassifyNativePublicationWatermarkPair(Oracle(abandoned),Oracle(Other(pending))).error==E::repair_required,"pending-to-abandoned state link is explicit repair");
  auto next=Next(Other(abandoned));next.abandonment.reset();next.publication_plan.reset();Check(db::ClassifyNativePublicationWatermarkPair(Oracle(abandoned),Oracle(next)).error==E::repair_required,"abandoned request permits a new generation with unchanged selected base");
  auto unselected_next=Next(Other(pending));unselected_next.publication_plan.reset();const auto unselected_bytes=Oracle(unselected_next);Check(db::DecodeNativePublicationWatermark(unselected_bytes).ok(),"unselected successor negative is individually canonical");Check(db::ClassifyNativePublicationWatermarkPair(Oracle(pending),unselected_bytes).error==E::invalid_pair,"unresolved profile still cannot be displaced");
  for(unsigned defect=0;defect<7;++defect){auto wrong=abandoned;if(defect==0)wrong.intent->recovery_profile=0;if(defect==1)wrong.intent->recovery_profile=3-recovery_profile;if(defect==2)wrong.abandonment->resolution_uuid={};if(defect==3)wrong.abandonment->resolution_uuid=wrong.operation_uuid;if(defect==4)wrong.abandonment->pending_state_sha256={};if(defect==5)wrong.abandonment->pending_state_sha256[0]^=1;if(defect==6)wrong.intent->policy_snapshot_uuid=Id(47);Invalid(wrong);}
  auto conflict=Other(abandoned);conflict.abandonment->resolution_uuid=Id(48);Check(db::EncodeNativePublicationWatermark(conflict).ok(),"distinct resolution individually canonical");Check(db::ClassifyNativePublicationWatermarkPair(Oracle(abandoned),Oracle(conflict)).error==E::invalid_pair,"conflicting valid resolutions refuse");
 }
}
void InventoryIntentTests(){
 for(unsigned profile=0;profile<5;++profile)for(unsigned attached=0;attached<2;++attached){auto state=Next(Example(profile));state.intent=db::NativePublicationIntent{Id(40),Id(41),Id(42),{},4,2};state.intent->normalized_request_sha256.fill(43);
  if(attached){const auto origin=Oracle(state);db::NativePublicationWatermark::PlanAnchor anchor;anchor.page={Id(2),30,2,state.header.page_size_profile_uuid};anchor.object_uuid=Id(44);anchor.sha256.fill(45);std::copy_n(origin.begin()+368,32,anchor.reservation_state_sha256.begin());state.publication_plan=anchor;}
  const auto bytes=Oracle(state),other=Oracle(Other(state));const auto encoded=db::EncodeNativePublicationWatermark(state);Check(encoded.ok()&&encoded.bytes==bytes,"independent inventory intent bytes and exact origin");const auto decoded=db::DecodeNativePublicationWatermark(bytes);Check(decoded.ok()&&decoded.state->intent==state.intent&&decoded.state->publication_plan==state.publication_plan&&Oracle(*decoded.state)==bytes,"inventory intent exact round trip");Check(db::ClassifyNativePublicationWatermarkPair(bytes,other).ok(),"inventory intent equal pair");
auto wrong=state;wrong.intent->recovery_profile=5;Invalid(wrong);wrong=state;wrong.abandonment=db::NativePublicationWatermark::Abandonment{Id(46),{}};std::copy_n(bytes.begin()+368,32,wrong.abandonment->pending_state_sha256.begin());const auto resolved=Oracle(wrong);Check(db::EncodeNativePublicationWatermark(wrong).bytes==resolved&&db::DecodeNativePublicationWatermark(resolved).ok(),"inventory resolution has its own canonical disposition");auto metadata_disposition=resolved;Num(metadata_disposition,646,2,1);Seal(metadata_disposition);Empty(db::DecodeNativePublicationWatermark(metadata_disposition));
  auto metadata=state;metadata.publication_plan.reset();metadata.intent->recovery_profile=1;Check(db::ClassifyNativePublicationWatermarkPair(bytes,Oracle(Other(metadata))).error==E::invalid_pair,"metadata intent cannot replace inventory intent at same generation");
  if(attached){auto forged=bytes;Num(forged,644,2,1);Seal(forged);Empty(db::DecodeNativePublicationWatermark(forged));}
  for(unsigned route=0;route<3;++route){const auto call=[&]{return route==0?db::EncodeNativePublicationWatermark(state):route==1?db::DecodeNativePublicationWatermark(bytes):db::ClassifyNativePublicationWatermarkPair(bytes,other);};bool complete=false;
   for(long at=0;at<20;++at){allocation_budget=at;const auto r=call();const auto left=allocation_budget;allocation_budget=-1;if(r.ok()){Check(left==0,"inventory intent complete allocation sweep");complete=true;break;}Check(left==-1&&r.error==E::resource_exhausted,"inventory intent consumes allocation failures");Empty(r);}Check(complete,"inventory intent allocation sweep terminates");
   const unsigned hashes=(2+attached)*(route==2?2:1);for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto r=call();Check(!hash_fault&&r.error==E::hash_failure,"inventory intent exact origin and image hash failures");Empty(r);}
  }
 }
}
void StartupBindingTests() {
  for(unsigned profile=0;profile<5;++profile)for(bool attached:{false,true}) {
    auto state=Next(Example(profile));
    state.intent=db::NativePublicationIntent{Id(40),Id(41),Id(42),{},4,2};
    state.intent->normalized_request_sha256.fill(43);
    state.intent->startup_binding=db::NativeStartupBinding{Id(50),Id(51),Id(52),13,19};
    const auto origin=Oracle(state);
    if(attached){db::NativePublicationWatermark::PlanAnchor anchor;
      anchor.page={Id(2),30,2,state.header.page_size_profile_uuid};anchor.object_uuid=Id(44);anchor.sha256.fill(45);
      std::copy_n(origin.begin()+368,32,anchor.reservation_state_sha256.begin());state.publication_plan=anchor;}
    const auto raw=Oracle(state),other=Oracle(Other(state));
    Check(db::EncodeNativePublicationWatermark(state).bytes==raw,"independent complete startup watermark bytes");
    const auto decoded=db::DecodeNativePublicationWatermark(raw);
    Check(decoded.ok()&&decoded.state->intent==state.intent&&decoded.state->publication_plan==state.publication_plan&&
      Oracle(*decoded.state)==raw,"startup binary binding and original anchor round trip");
    Check(db::ClassifyNativePublicationWatermarkPair(raw,other).ok(),"stable startup-bound pair");
    if(attached)for(bool reverse:{false,true})Check(db::ClassifyNativePublicationWatermarkPair(
      reverse?other:origin,reverse?origin:other).error==E::repair_required,"startup anchor interruption is repair not admission");
    for(unsigned field=0;field<5;++field){auto changed=Other(state);auto& b=*changed.intent->startup_binding;
      if(field==0)b.operation_uuid=Id(53);if(field==1)b.session_uuid=Id(53);if(field==2)b.transaction_uuid=Id(53);
      if(field==3)++b.local_transaction_id;if(field==4)++b.fence_generation;
      const auto altered=Oracle(changed);
      if(attached)Empty(db::DecodeNativePublicationWatermark(altered));
      else {Check(db::DecodeNativePublicationWatermark(altered).ok(),"changed binding individually valid");
        Check(db::ClassifyNativePublicationWatermarkPair(raw,altered).error==E::invalid_pair,"same generation cannot substitute startup binding");}
    }
    for(unsigned field=0;field<3;++field)for(unsigned invalid=0;invalid<3;++invalid){auto bad=state;
      auto& b=*bad.intent->startup_binding;auto& id=field==0?b.operation_uuid:field==1?b.session_uuid:b.transaction_uuid;
      if(!invalid)id={};if(invalid==1)id.bytes[6]=0x40;if(invalid==2)id.bytes[8]=0xc0;Invalid(bad);}
    for(unsigned field=0;field<3;++field){auto bad=state;auto& b=*bad.intent->startup_binding;
      if(!field)b.local_transaction_id=0;if(field==1)b.local_transaction_id=UINT64_MAX;if(field==2)b.fence_generation=0;Invalid(bad);}
    for(u16 profile_value:{0,1,3,4,5}){auto bad=state;bad.intent->recovery_profile=profile_value;Invalid(bad);}
    auto abandoned=state;abandoned.abandonment=db::NativePublicationWatermark::Abandonment{Id(60),{}};
    std::copy_n(raw.begin()+368,32,abandoned.abandonment->pending_state_sha256.begin());Invalid(abandoned);
    for(unsigned at=696;at<760;++at){auto bad=raw;bad[at]^=1;Seal(bad,false);
      const auto r=db::DecodeNativePublicationWatermark(bad);Empty(r);Check(r.error==E::invalid_integrity,"every startup byte bound by inner digest/origin");}
    for(unsigned at=760;at<768;++at){auto bad=raw;bad[at]=1;Seal(bad);Empty(db::DecodeNativePublicationWatermark(bad));}
    auto legacy=raw;legacy[135]='4';Num(legacy,136,2,4);Seal(legacy);Empty(db::DecodeNativePublicationWatermark(legacy));
    auto maximum=state;maximum.publication_plan.reset();maximum.intent->startup_binding->local_transaction_id=UINT64_MAX-1;
    maximum.intent->startup_binding->fence_generation=UINT64_MAX;
    Check(db::EncodeNativePublicationWatermark(maximum).bytes==Oracle(maximum)&&db::DecodeNativePublicationWatermark(Oracle(maximum)).ok(),"exact unsigned startup bounds without overflow");
    for(unsigned route=0;route<3;++route){const auto call=[&]{return route==0?db::EncodeNativePublicationWatermark(state):route==1?db::DecodeNativePublicationWatermark(raw):db::ClassifyNativePublicationWatermarkPair(raw,other);};
      bool complete=false;for(long at=0;at<20;++at){allocation_budget=at;const auto result=call();const auto left=allocation_budget;allocation_budget=-1;
        if(result.ok()){Check(left==0,"startup complete allocation sweep");complete=true;break;}
        Check(left==-1&&result.error==E::resource_exhausted,"startup allocation failure consumed");Empty(result);}
      Check(complete,"startup allocation sweep terminates");
      for(unsigned mode=1;mode<=5;++mode)for(unsigned site=1;site<=(2+unsigned(attached))*(route==2?2u:1u);++site){
        hash_fault=mode;hash_target=site;hash_seen=0;hash_active=false;const auto result=call();
        Check(!hash_fault&&result.error==E::hash_failure,"startup every digest/origin fault consumed");Empty(result);}
    }
  }
}
void Test() {
  BoundedTests();
  StartupBindingTests();
  for(unsigned recovery=3;recovery<=4;++recovery)for(unsigned profile=0;profile<5;++profile)for(bool attached:{false,true}){
    auto state=Next(Example(profile));state.intent=db::NativePublicationIntent{Id(40),Id(41),Id(42),{},4,u16(recovery)};
    state.intent->normalized_request_sha256.fill(43);
    if(attached){const auto origin=Oracle(state);db::NativePublicationWatermark::PlanAnchor anchor;
      anchor.page={Id(2),30,2,state.header.page_size_profile_uuid};anchor.object_uuid=Id(44);anchor.sha256.fill(45);
      std::copy_n(origin.begin()+368,32,anchor.reservation_state_sha256.begin());state.publication_plan=anchor;}
    const auto bytes=Oracle(state);const auto encoded=db::EncodeNativePublicationWatermark(state);
    Check(encoded.ok()&&encoded.bytes==bytes&&db::DecodeNativePublicationWatermark(bytes).ok()&&
      db::ClassifyNativePublicationWatermarkPair(bytes,Oracle(Other(state))).ok(),"independent preallocation and growth watermark profiles");
    auto cancelled=state;cancelled.abandonment=db::NativePublicationWatermark::Abandonment{Id(46),{}};
    std::copy_n(bytes.begin()+368,32,cancelled.abandonment->pending_state_sha256.begin());Invalid(cancelled);
    auto unknown=state;unknown.intent->recovery_profile=5;Invalid(unknown);
    auto crossed=Other(state);crossed.intent->recovery_profile=7-recovery;crossed.publication_plan.reset();
    Check(db::ClassifyNativePublicationWatermarkPair(bytes,Oracle(crossed)).error==E::invalid_pair,"storage profiles cannot substitute one another at the same generation");
    auto substituted=Other(state);substituted.intent->recovery_profile=1;
    substituted.publication_plan.reset();
    Check(db::EncodeNativePublicationWatermark(substituted).ok(),"substituted metadata intent individually canonical");
    Check(db::ClassifyNativePublicationWatermarkPair(bytes,Oracle(substituted)).error==E::invalid_pair,
      "preallocation cannot be replaced by metadata-only intent");
    for(unsigned route=0;route<3;++route){const auto other=Oracle(Other(state));const auto call=[&]{return route==0?db::EncodeNativePublicationWatermark(state):route==1?db::DecodeNativePublicationWatermark(bytes):db::ClassifyNativePublicationWatermarkPair(bytes,other);};
      bool completed=false;for(long at=0;at<20;++at){allocation_budget=at;const auto result=call();const auto left=allocation_budget;allocation_budget=-1;
        if(result.ok()){Check(!left,"complete storage-profile allocation sweep");completed=true;break;}
        Check(left==-1&&result.error==E::resource_exhausted,"storage profile consumes exact allocation failure");Empty(result);}
      Check(completed,"storage-profile allocation sweep terminates");
      for(unsigned mode=1;mode<=5;++mode)for(unsigned site=1;site<=(2+attached)*(route==2?2u:1u);++site){hash_fault=mode;hash_target=site;hash_seen=0;hash_active=false;const auto result=call();
        Check(!hash_fault&&result.error==E::hash_failure,"every storage-profile digest failure consumed");Empty(result);}
    }
  }
  InventoryIntentTests();
  ResolutionTests();
  AnchorTests();
  IntentTests();
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
