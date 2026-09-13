// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_index_btree_page.hpp"
#include "hash_digest_parts.hpp"
#include "disk_device.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <new>
#include <stdexcept>
#include <string_view>

namespace scratchbird::storage::page {
namespace {
using namespace scratchbird::core::platform;
namespace hash=scratchbird::core::hash;
using E=NativeBtreeError;
constexpr std::size_t start=640,seal=512;
bool Zero(const byte* p,std::size_t n){return std::all_of(p,p+n,[](byte b){return b==0;});}
bool V7(const Uuid& u){return !u.is_nil()&&(u.bytes[6]>>4)==7&&(u.bytes[8]&0xc0)==0x80;}
Uuid Get(const byte* p){Uuid u;std::copy_n(p,16,u.bytes.begin());return u;}
void Put(byte* p,const Uuid& u){std::copy(u.bytes.begin(),u.bytes.end(),p);}
NativeBtreePageResult Fail(E e){NativeBtreePageResult r;r.error=e;return r;}
disk::NativePageReference Self(const NativeBtreePage& p){const auto& h=p.header;return {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
bool ValidRef(const disk::NativePageReference& r){return V7(r.filespace_uuid)&&r.page_number&&r.page_generation&&disk::FindCanonicalFilespacePageProfile(r.page_size_profile_uuid);}
bool ValidDependencies(const NativeBtreeDependencies& d){return V7(d.index_uuid)&&d.descriptor_generation&&d.storage_generation&&V7(d.key_profile_uuid)
  &&V7(d.visibility_profile_uuid)&&V7(d.dependency_map_uuid)&&!Zero(d.dependency_map_sha256.data(),32);}
bool ValidKey(const NativeBtreeKey& k){return !k.encoded_key.empty()&&V7(k.row_uuid)&&V7(k.version_uuid);}
std::optional<disk::NativePageReference> GetRef(const byte* p){if(Zero(p,48))return {};return disk::NativePageReference{Get(p),LoadLittle64(p+16),LoadLittle64(p+24),Get(p+32)};}
void PutRef(byte* p,const std::optional<disk::NativePageReference>& r){if(!r)return;Put(p,r->filespace_uuid);StoreLittle64(p+16,r->page_number);StoreLittle64(p+24,r->page_generation);Put(p+32,r->page_size_profile_uuid);}
auto Digest(const std::vector<byte>& b){const std::array<byte,32> zeros{};const hash::HashDigestSegment parts[]={{b.data(),seal},{zeros.data(),32},{b.data()+seal+32,b.size()-seal-32}};return hash::ComputeSha256DigestParts(parts,3);}
E Validate(const NativeBtreePage& p) {
  const auto h=disk::EncodeNativeCommonPageHeader(p.header);
  if(!h.ok()||p.header.page_type<0x200||p.header.page_type>0x202)return E::invalid_header;
  if(p.header.flags&1u)return E::encrypted_requires_crypto_authority;
  if(!ValidDependencies(p.dependencies))return E::invalid_dependencies;
  if(!V7(p.creator_transaction_uuid)||!p.creator_local_transaction_id||p.maintenance_state<1||p.maintenance_state>9)return E::invalid_family;
  const bool root=p.header.page_type==0x200,branch=p.tree_level>0;
  if((p.header.page_type==0x201&&!branch)||(p.header.page_type==0x202&&branch)
    ||(root&&(p.parent||p.left||p.right||p.low_fence||p.high_fence))||(!root&&!p.parent)
    ||(p.cells.empty()&&(!root||branch))||branch!=p.first_child.has_value())return E::invalid_family;
  if((p.left&&!p.low_fence)||(p.right&&!p.high_fence))return E::invalid_fence;
  if((p.low_fence&&!ValidKey(*p.low_fence))||(p.high_fence&&!ValidKey(*p.high_fence))
    ||(p.low_fence&&p.high_fence&&CompareNativeBtreeKeys(*p.low_fence,*p.high_fence)>=0))return E::invalid_fence;
  std::map<Uuid,Uuid> profiles{{p.header.filespace_uuid,p.header.page_size_profile_uuid}};
  std::set<std::pair<Uuid,u64>> children;
  const auto ref=[&](const auto& r,bool child){if(!r)return true;
    if(!ValidRef(*r)||(r->filespace_uuid==p.header.filespace_uuid&&r->page_number==p.header.page_number))return false;
    const auto [it,inserted]=profiles.emplace(r->filespace_uuid,r->page_size_profile_uuid);
    if(!inserted&&it->second!=r->page_size_profile_uuid)return false;
    return !child||children.emplace(r->filespace_uuid,r->page_number).second;};
  if(!ref(p.parent,false)||!ref(p.left,false)||!ref(p.right,false)||!ref(p.first_child,true))return E::invalid_reference;
  for(std::size_t i=0;i<p.cells.size();++i){const auto& c=p.cells[i];
    if(!ValidKey(c.key)||branch!=c.child.has_value()||(branch&&(c.base_page||c.deleted)))return E::invalid_family;
    if(!ref(c.child,true)||!ref(c.base_page,false))return E::invalid_reference;
    if(i&&CompareNativeBtreeKeys(p.cells[i-1].key,c.key)>=0)return E::invalid_order;
    if((p.low_fence&&CompareNativeBtreeKeys(c.key,*p.low_fence)<0)||(p.high_fence&&CompareNativeBtreeKeys(c.key,*p.high_fence)>=0))return E::invalid_fence;
  }
  return E::none;
}
} // namespace

int CompareNativeBtreeKeys(const NativeBtreeKey& a,const NativeBtreeKey& b) noexcept {
  if(a.encoded_key!=b.encoded_key)return std::lexicographical_compare(a.encoded_key.begin(),a.encoded_key.end(),b.encoded_key.begin(),b.encoded_key.end())?-1:1;
  if(a.row_uuid!=b.row_uuid)return a.row_uuid<b.row_uuid?-1:1;
  if(a.version_uuid!=b.version_uuid)return a.version_uuid<b.version_uuid?-1:1;
  return 0;
}
NativeBtreePageResult EncodeNativeBtreePage(const NativeBtreePage& p) noexcept {
  try {
    const auto valid=Validate(p);if(valid!=E::none)return Fail(valid);
    const std::size_t size=p.header.page_size_bytes;
    if(p.cells.size()>(size-start)/149)return Fail(E::resource_exhausted);
    std::size_t used=start+4*p.cells.size();
    const auto charge=[&](std::size_t n){if(n>size-used)return false;used+=n;return true;};
    for(const auto* fence:{&p.low_fence,&p.high_fence})if(*fence&&(!charge(40)||!charge((*fence)->encoded_key.size())))return Fail(E::resource_exhausted);
    for(const auto& c:p.cells)if(!charge(144)||!charge(c.key.encoded_key.size()))return Fail(E::resource_exhausted);
    std::vector<byte> b(size,0);const auto common=disk::EncodeNativeCommonPageHeader(p.header);std::copy(common.bytes->begin(),common.bytes->end(),b.begin());
    auto* f=b.data()+128;const std::string_view magic="SBBTP001";std::copy(magic.begin(),magic.end(),f);StoreLittle16(f+8,1);StoreLittle16(f+10,512);StoreLittle32(f+12,used);
    const auto& d=p.dependencies;Put(f+16,d.index_uuid);StoreLittle64(f+32,d.descriptor_generation);StoreLittle64(f+40,d.storage_generation);
    Put(f+48,d.key_profile_uuid);Put(f+64,d.visibility_profile_uuid);Put(f+80,d.dependency_map_uuid);std::copy(d.dependency_map_sha256.begin(),d.dependency_map_sha256.end(),f+96);
    Put(f+128,p.creator_transaction_uuid);StoreLittle64(f+144,p.creator_local_transaction_id);StoreLittle16(f+152,p.maintenance_state);StoreLittle16(f+154,p.tree_level);
    PutRef(f+160,p.parent);PutRef(f+208,p.left);PutRef(f+256,p.right);PutRef(f+304,p.first_child);
    StoreLittle32(f+368,p.cells.size());StoreLittle32(f+372,std::count_if(p.cells.begin(),p.cells.end(),[](const auto& c){return c.deleted;}));StoreLittle32(f+376,start);
    std::size_t at=start+4*p.cells.size();
    const auto fence=[&](const auto& key,std::size_t offset){if(!key)return;StoreLittle32(f+offset,at);StoreLittle32(f+offset+4,40+key->encoded_key.size());
      StoreLittle32(b.data()+at,key->encoded_key.size());Put(b.data()+at+8,key->row_uuid);Put(b.data()+at+24,key->version_uuid);
      std::copy(key->encoded_key.begin(),key->encoded_key.end(),b.begin()+at+40);at+=40+key->encoded_key.size();};
    fence(p.low_fence,352);fence(p.high_fence,360);
    for(std::size_t i=0;i<p.cells.size();++i){const auto& c=p.cells[i];StoreLittle32(b.data()+start+4*i,at);auto* out=b.data()+at;
      StoreLittle32(out,144+c.key.encoded_key.size());StoreLittle32(out+4,c.key.encoded_key.size());StoreLittle32(out+8,c.deleted?1:0);
      Put(out+16,c.key.row_uuid);Put(out+32,c.key.version_uuid);PutRef(out+48,c.child);PutRef(out+96,c.base_page);
      std::copy(c.key.encoded_key.begin(),c.key.encoded_key.end(),out+144);at+=144+c.key.encoded_key.size();}
    const auto digest=Digest(b);if(!digest.ok())return Fail(E::hash_failure);std::copy(digest.digest.begin(),digest.digest.end(),b.begin()+seal);
    return {E::none,p,std::move(b)};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_family);}
}
NativeBtreePageResult DecodeNativeBtreePage(const std::vector<byte>& b) noexcept {
  try {
    if(b.size()<start)return Fail(E::invalid_header);
    const auto common=disk::DecodeNativeCommonPageHeader(b.data(),128);
    if(!common.ok()||common.header->page_size_bytes!=b.size()||common.header->page_type<0x200||common.header->page_type>0x202)return Fail(E::invalid_header);
    if(common.header->flags&1u)return Fail(E::encrypted_requires_crypto_authority);
    const auto digest=Digest(b);if(!digest.ok())return Fail(E::hash_failure);
    if(!std::equal(digest.digest.begin(),digest.digest.end(),b.begin()+seal))return Fail(E::invalid_integrity);
    const auto* f=b.data()+128;const auto used=LoadLittle32(f+12),count=LoadLittle32(f+368),deleted=LoadLittle32(f+372);
    if(std::string_view(reinterpret_cast<const char*>(f),8)!="SBBTP001"||LoadLittle16(f+8)!=1||LoadLittle16(f+10)!=512
      ||used<start||used>b.size()||count>(used-start)/4||LoadLittle32(f+376)!=start||deleted>count
      ||!Zero(f+156,4)||!Zero(f+380,4)||!Zero(f+416,96)||!Zero(b.data()+used,b.size()-used))return Fail(E::invalid_family);
    NativeBtreePage p;p.header=*common.header;auto& d=p.dependencies;
    d.index_uuid=Get(f+16);d.descriptor_generation=LoadLittle64(f+32);d.storage_generation=LoadLittle64(f+40);d.key_profile_uuid=Get(f+48);
    d.visibility_profile_uuid=Get(f+64);d.dependency_map_uuid=Get(f+80);std::copy_n(f+96,32,d.dependency_map_sha256.begin());
    p.creator_transaction_uuid=Get(f+128);p.creator_local_transaction_id=LoadLittle64(f+144);p.maintenance_state=LoadLittle16(f+152);p.tree_level=LoadLittle16(f+154);
    p.parent=GetRef(f+160);p.left=GetRef(f+208);p.right=GetRef(f+256);p.first_child=GetRef(f+304);
    std::size_t at=start+4*count;
    const auto fence=[&](auto& key,std::size_t offset){const auto pos=LoadLittle32(f+offset),length=LoadLittle32(f+offset+4);
      if(!pos&&!length)return true;if(pos!=at||length<41||length>used-at)return false;
      const auto* in=b.data()+at;const auto n=LoadLittle32(in);if(n!=length-40||!Zero(in+4,4))return false;
      NativeBtreeKey k;k.row_uuid=Get(in+8);k.version_uuid=Get(in+24);k.encoded_key.assign(in+40,in+length);key=std::move(k);at+=length;return true;};
    if(!fence(p.low_fence,352)||!fence(p.high_fence,360))return Fail(E::invalid_fence);
    if(count>(used-at)/145)return Fail(E::invalid_family);
    for(u32 i=0;i<count;++i){if(LoadLittle32(b.data()+start+4*i)!=at||used-at<145)return Fail(E::invalid_family);
      const auto* in=b.data()+at;const auto length=LoadLittle32(in),n=LoadLittle32(in+4),flags=LoadLittle32(in+8);
      if(length<145||length>used-at||n!=length-144||flags>1||!Zero(in+12,4))return Fail(E::invalid_family);
      NativeBtreeCell c;c.key.row_uuid=Get(in+16);c.key.version_uuid=Get(in+32);c.key.encoded_key.assign(in+144,in+length);
      c.deleted=flags==1;c.child=GetRef(in+48);c.base_page=GetRef(in+96);p.cells.push_back(std::move(c));at+=length;}
    if(at!=used||deleted!=std::count_if(p.cells.begin(),p.cells.end(),[](const auto& c){return c.deleted;}))return Fail(E::invalid_family);
    const auto valid=Validate(p);if(valid!=E::none)return Fail(valid);return {E::none,std::move(p),b};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_family);}
}
NativeBtreePageResult ReadNativeBtreePageFromOpenDevice(disk::FileDevice& device,const Uuid& db,
    const disk::NativePageReference& ref,u32 type,const NativeBtreeDependencies& dependencies) noexcept {
  try {
    if(!V7(db)||!ValidRef(ref)||type<0x200||type>0x202)return Fail(E::invalid_reference);
    if(!ValidDependencies(dependencies))return Fail(E::invalid_dependencies);
    const auto guard=device.AcquireOperationGuard();const disk::FilespaceBootstrapBinding expected{db,ref.filespace_uuid,ref.page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(device,&expected);
    if(!zero.ok()){
      if(zero.error==disk::FilespacePageZeroError::resource_exhausted)return Fail(E::resource_exhausted);
      if(zero.error==disk::FilespacePageZeroError::hash_provider_failure)return Fail(E::hash_failure);
      if(zero.error==disk::FilespacePageZeroError::io_failure)return Fail(E::io_failure);
      return Fail(E::invalid_filespace);}
    const auto& z=*zero.record;
    if((z.bootstrap.filespace_role!=5&&z.bootstrap.filespace_role!=6)||ref.page_number>=z.total_pages)return Fail(E::invalid_filespace);
    if(z.bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted)return Fail(E::encrypted_requires_crypto_authority);
    std::vector<byte> bytes(z.bootstrap.page_size_bytes);const auto io=device.ReadAt(ref.page_number*z.bootstrap.page_size_bytes,bytes.data(),bytes.size());
    if(!io.ok()||io.bytes_transferred!=bytes.size())return Fail(E::io_failure);
    const disk::NativeCommonPageHeaderBinding header_binding{{db,ref.filespace_uuid,ref.page_size_profile_uuid},ref.page_number,ref.page_generation,type,{}};
    const auto common=disk::DecodeNativeCommonPageHeader(bytes.data(),128,&header_binding);
    if(!common.ok())return Fail(common.error==disk::NativeCommonPageHeaderError::binding_mismatch?E::binding_mismatch:E::invalid_header);
    if(common.header->flags&1u)return Fail(E::encrypted_requires_crypto_authority);
    if(common.header->flags&2u)return Fail(E::cluster_requires_authority);
    if(common.header->flags&12u)return Fail(E::header_policy_requires_authority);
    auto result=DecodeNativeBtreePage(bytes);if(!result.ok())return result;const auto& p=*result.page;
    if(p.header.database_uuid!=db||Self(p)!=ref||p.header.page_type!=type||p.dependencies!=dependencies)return Fail(E::binding_mismatch);
    const auto in_range=[&](const auto& r){return !r||r->filespace_uuid!=ref.filespace_uuid||r->page_number<z.total_pages;};
    if(!in_range(p.parent)||!in_range(p.left)||!in_range(p.right)||!in_range(p.first_child))return Fail(E::invalid_reference);
    for(const auto& c:p.cells)if(!in_range(c.child)||!in_range(c.base_page))return Fail(E::invalid_reference);
    return result;
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::io_failure);}
}
} // namespace scratchbird::storage::page
