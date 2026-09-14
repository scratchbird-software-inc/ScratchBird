// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_checkpoint_selection.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

namespace scratchbird::storage::database {
namespace {
using namespace scratchbird::core::platform;
using E=NativeCheckpointSelectionError;
constexpr std::size_t used=512,seal=432;
bool Zero(const byte* p,std::size_t n){return std::all_of(p,p+n,[](byte b){return b==0;});}
bool V7(const Uuid& id){return !id.is_nil()&&(id.bytes[6]>>4)==7&&(id.bytes[8]&0xc0)==0x80;}
Uuid GetUuid(const byte* p){Uuid id;std::copy_n(p,16,id.bytes.begin());return id;}
void PutUuid(byte* p,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),p);}
disk::NativePageReference GetRef(const byte* p){return {GetUuid(p),LoadLittle64(p+16),LoadLittle64(p+24),GetUuid(p+32)};}
void PutRef(byte* p,const disk::NativePageReference& r){PutUuid(p,r.filespace_uuid);StoreLittle64(p+16,r.page_number);StoreLittle64(p+24,r.page_generation);PutUuid(p+32,r.page_size_profile_uuid);}
bool Ref(const disk::NativePageReference& r){const auto* p=disk::FindCanonicalFilespacePageProfile(r.page_size_profile_uuid);
  return V7(r.filespace_uuid)&&p&&r.page_number&&r.page_generation&&r.page_number<std::numeric_limits<u64>::max()/p->page_size_bytes&&
    disk::CheckFileDeviceExtent(r.page_number*p->page_size_bytes,p->page_size_bytes).ok();}
NativeCheckpointSelectionImage Fail(E e){NativeCheckpointSelectionImage r;r.error=e;return r;}
auto Digest(const std::vector<byte>& b){const std::array<byte,32> zero{};const core::hash::HashDigestSegment parts[]={{b.data(),seal},{zero.data(),32},{b.data()+seal+32,b.size()-seal-32}};
  return core::hash::ComputeSha256DigestParts(parts,3);}
E Validate(const NativeCheckpointSelection& s){
  const auto& h=s.header;
  if(!disk::EncodeNativeCommonPageHeader(h).ok()||h.page_type!=0x30e||h.flags)return E::invalid_header;
  if(!V7(s.object_uuid)||!V7(s.bootstrap_uuid)||!V7(s.publication_uuid)||!V7(s.checkpoint_object_uuid)||!V7(s.timeline_uuid)||
    s.object_uuid==s.bootstrap_uuid||h.page_uuid==s.object_uuid||h.page_uuid==s.bootstrap_uuid)return E::invalid_identity;
  if(!s.selection_generation||!s.checkpoint_generation||!s.root_set_generation||Zero(s.checkpoint_sha256.data(),32))return E::invalid_family;
  if(!Ref(s.checkpoint))return E::invalid_reference;
  if(s.selection_generation==1){
    if(s.previous_selection_generation||s.previous_checkpoint||!s.previous_checkpoint_object_uuid.is_nil()||!Zero(s.previous_checkpoint_sha256.data(),32))return E::invalid_reference;
  }else if(s.previous_selection_generation!=s.selection_generation-1||!s.previous_checkpoint||!Ref(*s.previous_checkpoint)||
      !V7(s.previous_checkpoint_object_uuid)||Zero(s.previous_checkpoint_sha256.data(),32))return E::invalid_reference;
  const auto legal=[&](const auto& r){return r.filespace_uuid!=h.filespace_uuid||
    (r.page_size_profile_uuid==h.page_size_profile_uuid&&r.page_number!=h.page_number);};
  if(!legal(s.checkpoint)||(s.previous_checkpoint&&!legal(*s.previous_checkpoint)))return E::invalid_reference;
  if(s.previous_checkpoint&&s.previous_checkpoint->filespace_uuid==s.checkpoint.filespace_uuid&&
    (s.previous_checkpoint->page_size_profile_uuid!=s.checkpoint.page_size_profile_uuid||s.previous_checkpoint->page_number==s.checkpoint.page_number))return E::invalid_reference;
  return E::none;
}
bool SamePayload(const NativeCheckpointSelection& a,const NativeCheckpointSelection& b){
  return a.object_uuid==b.object_uuid&&a.bootstrap_uuid==b.bootstrap_uuid&&a.publication_uuid==b.publication_uuid&&a.selection_generation==b.selection_generation&&
    a.checkpoint==b.checkpoint&&a.checkpoint_object_uuid==b.checkpoint_object_uuid&&a.checkpoint_sha256==b.checkpoint_sha256&&
    a.checkpoint_generation==b.checkpoint_generation&&a.root_set_generation==b.root_set_generation&&a.timeline_uuid==b.timeline_uuid&&
    a.previous_selection_generation==b.previous_selection_generation&&a.previous_checkpoint==b.previous_checkpoint&&
    a.previous_checkpoint_object_uuid==b.previous_checkpoint_object_uuid&&a.previous_checkpoint_sha256==b.previous_checkpoint_sha256;
}
}
NativeCheckpointSelectionImage EncodeNativeCheckpointSelection(const NativeCheckpointSelection& s) noexcept {
  try {
    const auto valid=Validate(s);if(valid!=E::none)return Fail(valid);
    std::vector<byte> bytes(s.header.page_size_bytes,0);const auto header=disk::EncodeNativeCommonPageHeader(s.header);
    if(!header.ok())return Fail(E::invalid_header);std::copy(header.bytes->begin(),header.bytes->end(),bytes.begin());
    auto* f=bytes.data()+128;std::copy_n("SBDCP001",8,f);StoreLittle16(f+8,1);StoreLittle16(f+10,384);StoreLittle32(f+12,used);
    PutUuid(f+16,s.object_uuid);PutUuid(f+32,s.bootstrap_uuid);StoreLittle64(f+48,s.selection_generation);PutUuid(f+56,s.publication_uuid);
    PutRef(f+72,s.checkpoint);PutUuid(f+120,s.checkpoint_object_uuid);std::copy(s.checkpoint_sha256.begin(),s.checkpoint_sha256.end(),f+136);
    StoreLittle64(f+168,s.checkpoint_generation);StoreLittle64(f+176,s.root_set_generation);PutUuid(f+184,s.timeline_uuid);
    StoreLittle64(f+200,s.previous_selection_generation);if(s.previous_checkpoint)PutRef(f+208,*s.previous_checkpoint);
    PutUuid(f+256,s.previous_checkpoint_object_uuid);std::copy(s.previous_checkpoint_sha256.begin(),s.previous_checkpoint_sha256.end(),f+272);
    const auto digest=Digest(bytes);if(!digest.ok())return Fail(E::hash_failure);std::copy(digest.digest.begin(),digest.digest.end(),bytes.begin()+seal);
    return {E::none,s,std::move(bytes)};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_family);}
}
NativeCheckpointSelectionImage DecodeNativeCheckpointSelection(const std::vector<byte>& bytes) noexcept {
  try {
    if(bytes.size()<used)return Fail(E::invalid_header);const auto header=disk::DecodeNativeCommonPageHeader(bytes.data(),128);
    if(!header.ok()||header.header->page_type!=0x30e||header.header->flags||bytes.size()!=header.header->page_size_bytes)return Fail(E::invalid_header);
    const auto digest=Digest(bytes);if(!digest.ok())return Fail(E::hash_failure);
    if(!std::equal(digest.digest.begin(),digest.digest.end(),bytes.begin()+seal))return Fail(E::invalid_integrity);
    const auto* f=bytes.data()+128;
    if(std::string_view(reinterpret_cast<const char*>(f),8)!="SBDCP001"||LoadLittle16(f+8)!=1||LoadLittle16(f+10)!=384||LoadLittle32(f+12)!=used||
        !Zero(f+336,48)||!Zero(bytes.data()+used,bytes.size()-used))return Fail(E::invalid_family);
    NativeCheckpointSelection s;s.header=*header.header;s.object_uuid=GetUuid(f+16);s.bootstrap_uuid=GetUuid(f+32);s.selection_generation=LoadLittle64(f+48);s.publication_uuid=GetUuid(f+56);
    s.checkpoint=GetRef(f+72);s.checkpoint_object_uuid=GetUuid(f+120);std::copy_n(f+136,32,s.checkpoint_sha256.begin());
    s.checkpoint_generation=LoadLittle64(f+168);s.root_set_generation=LoadLittle64(f+176);s.timeline_uuid=GetUuid(f+184);s.previous_selection_generation=LoadLittle64(f+200);
    if(!Zero(f+208,48))s.previous_checkpoint=GetRef(f+208);s.previous_checkpoint_object_uuid=GetUuid(f+256);std::copy_n(f+272,32,s.previous_checkpoint_sha256.begin());
    const auto valid=Validate(s);if(valid!=E::none)return Fail(valid);return {E::none,std::move(s),bytes};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_family);}
}
NativeCheckpointSelectionPair ClassifyNativeCheckpointSelectionPair(const std::vector<byte>& first,const std::vector<byte>& second) noexcept {
  const auto fail=[](E e){NativeCheckpointSelectionPair r;r.error=e;return r;};
  try {
    const auto left=DecodeNativeCheckpointSelection(first),right=DecodeNativeCheckpointSelection(second);
    for(const auto* result:{&left,&right})if(result->error==E::hash_failure||result->error==E::resource_exhausted)return fail(result->error);
    if(!left.ok()||!right.ok())return fail(left.ok()||right.ok()?E::repair_required:E::invalid_pair);
    const auto& a=*left.selection;const auto& b=*right.selection;
    if(a.header.database_uuid!=b.header.database_uuid||a.header.filespace_uuid!=b.header.filespace_uuid||a.header.page_size_profile_uuid!=b.header.page_size_profile_uuid||
        a.header.page_number==b.header.page_number||a.header.page_uuid==b.header.page_uuid||a.object_uuid!=b.object_uuid||a.bootstrap_uuid!=b.bootstrap_uuid)return fail(E::invalid_pair);
    for(const auto* s:{&a,&b})for(const auto* h:{&a.header,&b.header}){
      const auto aliases=[&](const auto& r){return r.filespace_uuid==h->filespace_uuid&&r.page_number==h->page_number;};
      if(aliases(s->checkpoint)||(s->previous_checkpoint&&aliases(*s->previous_checkpoint)))return fail(E::invalid_pair);
    }
    if(a.selection_generation==b.selection_generation){if(!SamePayload(a,b))return fail(E::invalid_pair);return {E::none,a};}
    const auto& older=a.selection_generation<b.selection_generation?a:b;const auto& newer=a.selection_generation>b.selection_generation?a:b;
    if(newer.previous_selection_generation!=older.selection_generation||!newer.previous_checkpoint||*newer.previous_checkpoint!=older.checkpoint||
      newer.previous_checkpoint_object_uuid!=older.checkpoint_object_uuid||newer.previous_checkpoint_sha256!=older.checkpoint_sha256)return fail(E::invalid_pair);
    return fail(E::repair_required);
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted);}catch(const std::length_error&){return fail(E::resource_exhausted);}catch(...){return fail(E::invalid_pair);}
}
}  // namespace scratchbird::storage::database
