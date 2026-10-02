// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "database_dirty_manifest.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>
#include <span>
#include <source_location>

namespace {
thread_local long budget=-1;
thread_local bool measuring=false;
thread_local unsigned allocations=0;
unsigned fault=0,fault_context=1,hashes=0,hash_at=0,checks=0;
}
void* operator new(std::size_t n){if(measuring)++allocations;if(budget==0){budget=-1;throw std::bad_alloc();}
  if(budget>0)--budget;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){++hashes;if(hash_at&&hashes==hash_at)return nullptr;return __real_EVP_MD_CTX_new();}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* m,ENGINE* e){if(fault==1&&hashes==fault_context){fault=0;return 0;}return __real_EVP_DigestInit_ex(c,m,e);}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* p,size_t n){if(fault==2&&hashes==fault_context){fault=0;return 0;}return __real_EVP_DigestUpdate(c,p,n);}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* p,unsigned int* n){
  if(fault==3&&hashes==fault_context){fault=0;return 0;}const auto result=__real_EVP_DigestFinal_ex(c,p,n);if(fault==4&&hashes==fault_context){fault=0;*n=31;}return result;}
namespace {
namespace db=scratchbird::storage::database;namespace d=scratchbird::storage::disk;
using namespace scratchbird::core::platform;
using Bytes=std::vector<byte>;using E=db::NativeCheckpointError;
void Check(bool ok,const char* why,std::source_location at=std::source_location::current()){
  ++checks;if(!ok){std::cerr<<at.line()<<": "<<why<<'\n';throw why;}}
Uuid Id(byte n){Uuid id;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=n;return id;}
void Num(Bytes& b,usize at,unsigned n,u64 v){for(unsigned i=0;i<n;++i)b[at+i]=byte(v>>(8*i));}
void Put(Bytes& b,usize at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}
void Ref(Bytes& b,usize at,const d::NativePageReference& p){Put(b,at,p.filespace_uuid);Num(b,at+16,8,p.page_number);Num(b,at+24,8,p.page_generation);Put(b,at+32,p.page_size_profile_uuid);}
auto Hash(const Bytes& b){std::array<byte,32> h{};Check(SHA256(b.data(),b.size(),h.data())!=nullptr,"independent SHA256");return h;}
void Seal(Bytes& b,bool roots=true){
  if(roots){u32 used=0;for(unsigned i=0;i<4;++i)used|=u32(b[140+i])<<(8*i);
    Bytes material{'S','B','C','P','S','E','T',byte(b[136]==2?'2':'1')};
    material.insert(material.end(),b.begin()+160,b.begin()+256);
    if(b[136]==2)material.insert(material.end(),b.begin()+408,b.begin()+424);
    material.insert(material.end(),b.begin()+512,b.begin()+used);const auto h=Hash(material);std::copy(h.begin(),h.end(),b.begin()+336);}
  std::fill(b.begin()+368,b.begin()+400,0);const auto h=Hash(b);std::copy(h.begin(),h.end(),b.begin()+368);
}
template<class Root> Bytes Oracle(const Root& r){const auto& h=r.header;Bytes b(h.page_size_bytes,0);
  std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,h.page_type);Num(b,20,2,1);Num(b,22,2,1);
  Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Num(b,88,8,h.flags);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);
  u64 fnv=14695981039346656037ull;for(unsigned i=0;i<128;++i){fnv^=b[i];fnv*=1099511628211ull;}Num(b,96,8,fnv);
  const bool operation=!r.creator_operation_uuid.is_nil();std::copy_n(operation?"SBCPNT02":"SBCPNT01",8,b.begin()+128);
  Num(b,136,2,operation?2:1);Num(b,138,2,384);Num(b,140,4,512+112*r.roots.size());Put(b,144,r.object_uuid);
  Num(b,160,8,r.checkpoint_generation);Num(b,168,8,r.root_set_generation);Num(b,176,8,r.selected_local_transaction_id);
  Num(b,184,8,r.stable_local_transaction_id);Num(b,192,8,r.local_durable_transaction_id);Num(b,200,8,r.cluster_quorum_transaction_id);
  Put(b,208,r.timeline_uuid);Put(b,224,r.creator_transaction_uuid);Num(b,240,8,r.creator_local_transaction_id);Num(b,248,8,r.flags);
  if(r.predecessor)Ref(b,256,*r.predecessor);std::copy(r.predecessor_sha256.begin(),r.predecessor_sha256.end(),b.begin()+304);Num(b,400,8,r.completed?1:0);Put(b,408,r.creator_operation_uuid);
  for(usize i=0;i<r.roots.size();++i){const auto& root=r.roots[i];const auto at=512+112*i;
    Num(b,at,2,root.role);Num(b,at+4,4,root.page_type);Ref(b,at+8,root.page);Put(b,at+56,root.object_uuid);std::copy(root.sha256.begin(),root.sha256.end(),b.begin()+at+72);}
  Seal(b);return b;
}
db::NativeCheckpointRoot Example(unsigned profile=0,unsigned member=0,unsigned count=10){
  const auto& own=d::kCanonicalFilespacePageProfiles[profile];const auto& target=d::kCanonicalFilespacePageProfiles[member];
  db::NativeCheckpointRoot r;r.header={own.page_size_bytes,0x300,Id(1),Id(2),Id(10),19,109,0,own.uuid};
  r.object_uuid=Id(20);r.checkpoint_generation=1;r.root_set_generation=8;r.selected_local_transaction_id=17;
  r.stable_local_transaction_id=12;r.local_durable_transaction_id=16;r.timeline_uuid=Id(21);r.creator_transaction_uuid=Id(22);r.creator_local_transaction_id=17;
  constexpr unsigned types[]={0,769,770,9,3,5,10,11,8,5,771,773,775,776,777,779,1280};
  for(unsigned role=1;role<=count;++role){db::NativeCheckpointRootReference ref;
    ref.role=role;ref.page_type=types[role];ref.page={Id(3),30+role,80+role,target.uuid};ref.object_uuid=Id(100+role);ref.sha256.fill(byte(role));r.roots.push_back(ref);}
  if(count>=14){r.flags=4;r.cluster_quorum_transaction_id=15;}return r;
}
void Codecs(){
  for(unsigned profile=0;profile<5;++profile)for(unsigned member=0;member<5;++member)
    for(unsigned count=10;count<=16;++count)for(bool operation:{false,true})for(bool complete:{false,true}){
      auto value=Example(profile,member,count);value.completed=complete;
      if(operation){value.creator_transaction_uuid={};value.creator_local_transaction_id=0;value.creator_operation_uuid=Id(23);}
      auto bytes=Oracle(value);std::array<db::NativeCheckpointRootReference,16> roots;
      allocations=0;measuring=true;budget=0;auto r=db::DecodeNativeCheckpointRootInto(bytes,roots);
      const auto remaining=budget;budget=-1;measuring=false;
      Check(r.ok()&&!allocations&&remaining==0,"caller-backed checkpoint has no hidden allocations");
      Check(r.root->roots.data()==roots.data()&&r.root->roots.size()==count&&Oracle(*r.root)==bytes,"full independent image and exact-length native root span");
      auto short_result=db::DecodeNativeCheckpointRootInto(bytes,std::span(roots).first(count-1));
      Check(!short_result.root&&short_result.error==E::resource_exhausted,"one-short roots refuse without partial image");
      auto alias=db::DecodeNativeCheckpointRootInto(bytes,{reinterpret_cast<db::NativeCheckpointRootReference*>(bytes.data()),count});
      Check(!alias.root&&alias.error==E::invalid_backing&&bytes==Oracle(value),"overlapping storage refuses before writes");
      Bytes unaligned(1);unaligned.insert(unaligned.end(),bytes.begin(),bytes.end());
      Check(db::DecodeNativeCheckpointRootInto(std::span<const byte>(unaligned).subspan(1),roots).ok(),"unaligned image is decoded bytewise");
      std::fill(bytes.begin(),bytes.end(),0);Check(Oracle(*r.root)==Oracle(value),"decoded fields do not borrow encoded image lifetime");
    }
  auto value=Example();auto good=Oracle(value);std::array<db::NativeCheckpointRootReference,16> roots;
  for(usize at=0;at<good.size();++at){auto bad=good;bad[at]^=1;auto r=db::DecodeNativeCheckpointRootInto(bad,roots);Check(!r.ok()&&!r.root,"every-byte corruption returns no root view");}
  for(unsigned at:{128u,136u,138u,140u,160u,184u,208u,248u,400u,408u,511u,512u,514u,516u,520u,584u,616u,8191u}){
    auto bad=good;bad[at]^=0x80;Seal(bad,false);const auto owned=db::DecodeNativeCheckpointRoot(bad);const auto r=db::DecodeNativeCheckpointRootInto(bad,roots);
    Check(!r.root&&r.error==owned.error,"resealed malformed checkpoint preserves exact refusal");}
  for(unsigned n=0;n<5;++n){auto bad=value;
    if(n==0){bad.roots[1].page=bad.roots[0].page;}
    if(n==1)bad.roots[1].page.page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;
    if(n==2)bad.roots[1].role=1;
    if(n==3)bad.roots.front().sha256={};
    if(n==4)bad.roots.front().object_uuid={};
    const auto r=db::DecodeNativeCheckpointRootInto(Oracle(bad),roots);Check(!r.root&&r.error==E::invalid_roots,"fully resealed invalid root semantics refused");}
  for(unsigned which=1;which<=2;++which){hashes=0;hash_at=which;const auto r=db::DecodeNativeCheckpointRootInto(good,roots);hash_at=0;
    Check(!r.root&&r.error==E::hash_failure&&hashes==which,"both real digest contexts fail independently");}
  for(unsigned context=1;context<=2;++context)for(unsigned n=1;n<=4;++n){hashes=0;fault_context=context;fault=n;
    const auto r=db::DecodeNativeCheckpointRootInto(good,roots);Check(!fault&&!r.root&&r.error==E::hash_failure,"both full-page and root-set provider failures remain typed");}
  for(unsigned size:{0u,127u,511u,8191u,8193u}){auto bad=good;bad.resize(size);const auto r=db::DecodeNativeCheckpointRootInto(bad,roots);Check(!r.root&&r.error==E::invalid_header,"invalid image lengths refuse");}
  value.checkpoint_generation=2;value.predecessor=d::NativePageReference{Id(4),7,9,d::kCanonicalFilespacePageProfiles[4].uuid};value.predecessor_sha256.fill(0x91);
  const auto linked=Oracle(value);const auto r=db::DecodeNativeCheckpointRootInto(linked,roots);Check(r.ok()&&Oracle(*r.root)==linked,"full predecessor linkage preserved without granting authority");
  auto shared=Example();shared.roots[8]=shared.roots[4];shared.roots[8].role=9;
  const auto shared_image=Oracle(shared);const auto shared_result=db::DecodeNativeCheckpointRootInto(shared_image,roots);
  Check(shared_result.ok()&&Oracle(*shared_result.root)==shared_image,"exact catalog-feature root alias remains valid");
  for(unsigned field=0;field<4;++field){auto bad=shared;
    if(field==0)++bad.roots[8].page.page_generation;
    if(field==1)bad.roots[8].object_uuid=Id(199);
    if(field==2)bad.roots[8].sha256[0]^=1;
    if(field==3)bad.roots[8].page.page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;
    const auto result=db::DecodeNativeCheckpointRootInto(Oracle(bad),roots);
    Check(!result.root&&result.error==E::invalid_roots,"conflicting shared root metadata cannot be interpreted as a valid alias");}
}
}
int main(){try{Codecs();std::cout<<"PASS borrowed checkpoint checks="<<checks<<" not_SQL_E2E=true\n";return 0;}
  catch(...){budget=-1;std::cerr<<"FAIL checks="<<checks<<'\n';return 1;}}
