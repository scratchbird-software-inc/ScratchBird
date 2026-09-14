// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_checkpoint_selection.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <source_location>
#include <stdexcept>
namespace {long allocation_budget=-1;bool counting=false;unsigned long allocations=0;unsigned hash_fault=0;}
void* operator new(std::size_t n){if(counting)++allocations;if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){if(hash_fault==1){hash_fault=0;return nullptr;}return __real_EVP_MD_CTX_new();}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* m,ENGINE* e){if(hash_fault==2){hash_fault=0;return 0;}return __real_EVP_DigestInit_ex(c,m,e);}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* b,size_t n){if(hash_fault==3){hash_fault=0;return 0;}return __real_EVP_DigestUpdate(c,b,n);}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* b,unsigned int* n){if(hash_fault==4){hash_fault=0;return 0;}const int r=__real_EVP_DigestFinal_ex(c,b,n);if(hash_fault==5){hash_fault=0;*n=31;}return r;}
namespace {
namespace db=scratchbird::storage::database;namespace d=scratchbird::storage::disk;using namespace scratchbird::core::platform;
using Bytes=std::vector<byte>;using E=db::NativeCheckpointSelectionError;unsigned checks=0;
void Check(bool ok,const char* why,std::source_location at=std::source_location::current()){++checks;if(!ok)throw std::runtime_error(std::string(why)+" line="+std::to_string(at.line()));}
Uuid Id(byte n){Uuid id;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=n;return id;}
void Num(Bytes& b,std::size_t at,unsigned n,u64 v){for(unsigned i=0;i<n;++i)b[at+i]=static_cast<byte>(v>>(8*i));}
void Put(Bytes& b,std::size_t at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}
void Ref(Bytes& b,std::size_t at,const d::NativePageReference& r){Put(b,at,r.filespace_uuid);Num(b,at+16,8,r.page_number);Num(b,at+24,8,r.page_generation);Put(b,at+32,r.page_size_profile_uuid);}
void Seal(Bytes& b){std::fill(b.begin()+432,b.begin()+464,0);std::array<byte,32> sha{};Check(SHA256(b.data(),b.size(),sha.data())!=nullptr,"independent complete selector digest");std::copy(sha.begin(),sha.end(),b.begin()+432);}
Bytes Oracle(const db::NativeCheckpointSelection& s){const auto& h=s.header;Bytes b(h.page_size_bytes,0);std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,h.page_type);Num(b,20,2,1);Num(b,22,2,1);
  Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Num(b,88,8,h.flags);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);
  u64 fnv=14695981039346656037ull;for(unsigned i=0;i<128;++i){fnv^=b[i];fnv*=1099511628211ull;}Num(b,96,8,fnv);
  std::copy_n("SBDCP001",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,384);Num(b,140,4,512);Put(b,144,s.object_uuid);Put(b,160,s.bootstrap_uuid);Num(b,176,8,s.selection_generation);Put(b,184,s.publication_uuid);
  Ref(b,200,s.checkpoint);Put(b,248,s.checkpoint_object_uuid);std::copy(s.checkpoint_sha256.begin(),s.checkpoint_sha256.end(),b.begin()+264);Num(b,296,8,s.checkpoint_generation);Num(b,304,8,s.root_set_generation);Put(b,312,s.timeline_uuid);
  Num(b,328,8,s.previous_selection_generation);if(s.previous_checkpoint)Ref(b,336,*s.previous_checkpoint);Put(b,384,s.previous_checkpoint_object_uuid);std::copy(s.previous_checkpoint_sha256.begin(),s.previous_checkpoint_sha256.end(),b.begin()+400);Seal(b);return b;}
db::NativeCheckpointSelection Example(unsigned p=0){const auto& profile=d::kCanonicalFilespacePageProfiles[p];db::NativeCheckpointSelection s;
  s.header={profile.page_size_bytes,0x30e,Id(1),Id(2),Id(3),21,7,0,profile.uuid};s.object_uuid=Id(4);s.bootstrap_uuid=Id(5);s.publication_uuid=Id(6);s.selection_generation=1;
  s.checkpoint={Id(2),19,109,profile.uuid};s.checkpoint_object_uuid=Id(49);s.checkpoint_sha256.fill(7);s.checkpoint_generation=5;s.root_set_generation=8;s.timeline_uuid=Id(8);return s;}
auto Other(db::NativeCheckpointSelection s){s.header.page_uuid=Id(9);s.header.page_number=22;return s;}
auto Next(db::NativeCheckpointSelection s){s.previous_selection_generation=s.selection_generation++;s.previous_checkpoint=s.checkpoint;s.previous_checkpoint_object_uuid=s.checkpoint_object_uuid;s.previous_checkpoint_sha256=s.checkpoint_sha256;
  s.publication_uuid=Id(10);s.checkpoint.page_number=23;s.checkpoint.page_generation++;s.checkpoint_sha256.fill(11);s.checkpoint_generation++;s.root_set_generation++;return s;}
void Empty(const db::NativeCheckpointSelectionImage& r){Check(!r.ok()&&!r.selection&&r.bytes.empty(),"image failure returns no prefix");}
void Empty(const db::NativeCheckpointSelectionPair& r){Check(!r.ok()&&!r.selection,"pair failure returns no usable selection");}
void Invalid(const db::NativeCheckpointSelection& s){Empty(db::EncodeNativeCheckpointSelection(s));Empty(db::DecodeNativeCheckpointSelection(Oracle(s)));}
void Test(){
  for(unsigned p=0;p<5;++p)for(bool successor:{false,true}){
    auto s=Example(p);if(successor)s=Next(s);const auto first=Oracle(s),second=Oracle(Other(s));const auto e=db::EncodeNativeCheckpointSelection(s);
    Check(e.ok()&&e.bytes==first,"exact independently packed selector image");const auto decoded=db::DecodeNativeCheckpointSelection(first);Check(decoded.ok()&&Oracle(*decoded.selection)==first,"all selector fields preserved");
    for(bool reverse:{false,true}){const auto pair=db::ClassifyNativeCheckpointSelectionPair(reverse?second:first,reverse?first:second);Check(pair.ok()&&pair.selection->selection_generation==s.selection_generation,"stable pair is independent of slot order");}
    auto torn=second;torn.back()=1;auto pair=db::ClassifyNativeCheckpointSelectionPair(first,torn);Empty(pair);Check(pair.error==E::repair_required,"one damaged slot cannot grant current-root selection");
    pair=db::ClassifyNativeCheckpointSelectionPair(first,Bytes(second.size(),0));Empty(pair);Check(pair.error==E::repair_required,"one missing slot cannot grant current-root selection");
    auto mixed=Other(Next(Example(p)));pair=db::ClassifyNativeCheckpointSelectionPair(Oracle(Example(p)),Oracle(mixed));Empty(pair);Check(pair.error==E::repair_required,"interrupted consecutive publication needs recovery");
    mixed.previous_checkpoint_sha256[0]^=1;pair=db::ClassifyNativeCheckpointSelectionPair(Oracle(Example(p)),Oracle(mixed));Empty(pair);Check(pair.error==E::invalid_pair,"contradictory predecessor is not repairable by choosing higher generation");
  }
  const auto first=Oracle(Example()),second=Oracle(Other(Example()));
  for(std::size_t i=0;i<first.size();++i){auto bad=first;bad[i]^=1;Empty(db::DecodeNativeCheckpointSelection(bad));}
  for(unsigned at:{128u,136u,138u,140u,464u,511u,8191u}){auto b=first;b[at]^=1;Seal(b);Empty(db::DecodeNativeCheckpointSelection(b));}
  for(std::size_t n:{0u,127u,511u,8191u,8193u}){auto b=first;b.resize(n);Empty(db::DecodeNativeCheckpointSelection(b));}
  for(unsigned n=0;n<23;++n){auto s=Example();switch(n){
    case 0:s.header.page_type=8;break;case 1:s.header.flags=2;break;case 2:s.object_uuid={};break;case 3:s.bootstrap_uuid={};break;case 4:s.publication_uuid={};break;
    case 5:s.selection_generation=0;break;case 6:s.checkpoint_generation=0;break;case 7:s.root_set_generation=0;break;case 8:s.timeline_uuid={};break;case 9:s.checkpoint_object_uuid={};break;case 10:s.checkpoint_sha256.fill(0);break;
    case 11:s.checkpoint.page_number=0;break;case 12:s.checkpoint.page_number=s.header.page_number;break;case 13:s.checkpoint.page_generation=0;break;case 14:s.checkpoint.page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;break;
    case 15:s.previous_selection_generation=1;break;case 16:s.previous_checkpoint=s.checkpoint;break;case 17:s.previous_checkpoint_object_uuid=Id(20);break;case 18:s.previous_checkpoint_sha256.fill(1);break;
    case 19:s=Next(s);s.previous_selection_generation=0;break;case 20:s.checkpoint.page_number=std::numeric_limits<u64>::max();break;case 21:s.object_uuid=s.bootstrap_uuid;break;case 22:s.header.page_uuid=s.bootstrap_uuid;break;}Invalid(s);}
  for(unsigned n=0;n<13;++n){auto s=Other(Example());switch(n){
    case 0:s.publication_uuid=Id(24);break;case 1:s.checkpoint_sha256[0]^=1;break;case 2:s.checkpoint_generation++;break;case 3:s.root_set_generation++;break;case 4:s.timeline_uuid=Id(24);break;
    case 5:s.header.page_uuid=Id(3);break;case 6:s.header.page_number=21;break;case 7:s.header.database_uuid=Id(24);break;case 8:s.object_uuid=Id(24);break;
    case 9:s.bootstrap_uuid=Id(24);break;case 10:s.checkpoint.page_number=21;break;case 11:s=Next(s);s.selection_generation=3;s.previous_selection_generation=2;break;case 12:s.checkpoint_object_uuid=Id(24);break;}
    const auto image=Oracle(s);Check(db::DecodeNativeCheckpointSelection(image).ok(),"pair mismatch is individually valid image");const auto pair=db::ClassifyNativeCheckpointSelectionPair(first,image);Empty(pair);Check(pair.error==E::invalid_pair,"same-generation ambiguity and physical/lineage alias refused");}
  allocations=0;counting=true;const auto measured=db::ClassifyNativeCheckpointSelectionPair(first,second);counting=false;Check(measured.ok(),"measure successful pair");const auto count=allocations;
  for(unsigned long n=0;n<=count;++n){allocation_budget=n;const auto pair=db::ClassifyNativeCheckpointSelectionPair(first,second);allocation_budget=-1;if(!pair.ok())Empty(pair);if(n==count)Check(pair.ok(),"allocation sweep terminal success");}
  for(unsigned mode=1;mode<=5;++mode){hash_fault=mode;const auto pair=db::ClassifyNativeCheckpointSelectionPair(first,second);Check(!hash_fault,"multipart failure consumed");Empty(pair);Check(pair.error==E::hash_failure,"provider failure is not classified as damaged durable data");}
  std::cout<<"pair allocations="<<count<<" checks="<<checks<<" image_only=true\n";
}
}
int main(){try{Test();return 0;}catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
