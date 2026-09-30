// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_storage_action_intent.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace scratchbird::storage::database {
namespace {
using I=NativeStorageActionIntent;
using E=NativeStorageIntentError;
constexpr std::array<byte,8> magic{'S','B','S','I','N','T','0','3'};
constexpr std::array<Uuid I::*,11> ids{{
  &I::request_uuid,&I::operation_uuid,&I::database_uuid,&I::filespace_uuid,
  &I::locator_uuid,&I::page_zero_uuid,&I::page_size_profile_uuid,&I::policy_snapshot_uuid,
  &I::storage_profile_uuid,&I::initiator_uuid,&I::request_context_uuid}};
constexpr std::array<Uuid I::*,5> selected_ids{{&I::policy_uuid,&I::policy_version_uuid,
  &I::attachment_uuid,&I::attachment_version_uuid,&I::storage_profile_version_uuid}};
constexpr std::array<u64 I::*,16> numbers{{
  &I::checkpoint_generation,&I::checkpoint_root_set_generation,&I::directory_generation,
  &I::filespace_root_set_generation,&I::page_zero_generation,&I::map_generation,
  &I::capacity_generation,&I::catalog_generation,&I::policy_generation,&I::security_generation,
  &I::current_total_pages,&I::first_page,&I::page_count,&I::maximum_total_pages,
  &I::maximum_work_bytes,&I::maximum_retained_image_bytes}};
bool Zero(const byte* p,std::size_t n){return std::all_of(p,p+n,[](byte b){return b==0;});}
bool Root(const disk::FilespaceRootReference& r,u16 kind,u32 type){
  const auto* profile=disk::FindCanonicalFilespacePageProfile(r.page_size_profile_uuid);
  return r.kind==kind&&r.page_type==type&&r.page_number&&r.page_generation&&
    core::uuid::IsEngineIdentityUuid(r.filespace_uuid)&&core::uuid::IsEngineIdentityUuid(r.object_uuid)&&
    profile&&r.page_number<std::numeric_limits<u64>::max()/profile->page_size_bytes;
}
NativeStorageIntentImage Fail(E e){NativeStorageIntentImage r;r.error=e;return r;}
void Put(byte* p,u64 v,unsigned n){for(unsigned k=0;k<n;++k)p[k]=static_cast<byte>(v>>(8*k));}
u64 Get(const byte* p,unsigned n){u64 v=0;for(unsigned k=0;k<n;++k)v|=u64(p[k])<<(8*k);return v;}
void PutId(byte* p,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),p);}
Uuid GetId(const byte* p){Uuid id;std::copy_n(p,16,id.bytes.begin());return id;}
void PutRoot(byte* p,const disk::FilespaceRootReference& r){
  Put(p,r.kind,2);Put(p+4,r.page_type,4);PutId(p+8,r.filespace_uuid);
  Put(p+24,r.page_number,8);Put(p+32,r.page_generation,8);
  PutId(p+40,r.page_size_profile_uuid);PutId(p+56,r.object_uuid);
}
disk::FilespaceRootReference GetRoot(const byte* p){
  return {static_cast<u16>(Get(p,2)),static_cast<u32>(Get(p+4,4)),GetId(p+8),
    Get(p+24,8),Get(p+32,8),GetId(p+40),GetId(p+56)};
}
}
NativeStorageIntentError ValidateNativeStorageActionIntent(const I& i) noexcept {
  if(i.action!=NativeStorageAction::physical_growth&&i.action!=NativeStorageAction::page_preallocation)
    return E::invalid_header;
  for(const auto member:ids)if(!core::uuid::IsEngineIdentityUuid(i.*member))return E::invalid_identity;
  for(const auto member:selected_ids)if(!core::uuid::IsEngineIdentityUuid(i.*member))return E::invalid_identity;
  if(!i.attachment_generation||!i.storage_profile_generation)return E::invalid_range;
  const auto* profile=disk::FindCanonicalFilespacePageProfile(i.page_size_profile_uuid);
  if(!profile||profile->page_size_bytes!=i.page_size_bytes)return E::invalid_profile;
  if(!Root(i.checkpoint,9,0x300)||!Root(i.allocation_root,3,3)||
      i.allocation_root.filespace_uuid!=i.filespace_uuid||
      i.allocation_root.page_size_profile_uuid!=i.page_size_profile_uuid||
      i.allocation_root.page_number>=i.current_total_pages||
      (i.checkpoint.filespace_uuid==i.filespace_uuid&&
        (i.checkpoint.page_size_profile_uuid!=i.page_size_profile_uuid||
         i.checkpoint.page_number>=i.current_total_pages)))return E::invalid_reference;
  if(Zero(i.checkpoint_sha256.data(),32)||Zero(i.allocation_sha256.data(),32))return E::invalid_integrity;
  for(unsigned n=0;n<7;++n)if(!(i.*numbers[n]))return E::invalid_range;
  const auto capacity_limit=std::numeric_limits<u64>::max()/i.page_size_bytes;
  if(!i.current_total_pages||i.current_total_pages>i.maximum_total_pages||i.maximum_total_pages>capacity_limit||
      !i.page_count||i.page_count>i.maximum_work_bytes/i.page_size_bytes||
      i.maximum_retained_image_bytes<kNativeStorageActionIntentBytes)return E::invalid_range;
  if(i.action==NativeStorageAction::physical_growth){
    if(i.first_page!=i.current_total_pages||i.page_count>i.maximum_total_pages-i.current_total_pages||
        (i.intended_state!=NativeStorageIntentState::free&&i.intended_state!=NativeStorageIntentState::preallocated))
      return E::invalid_range;
  }else if(!i.first_page||i.first_page>=i.current_total_pages||
      i.page_count>i.current_total_pages-i.first_page||i.intended_state!=NativeStorageIntentState::preallocated)
    return E::invalid_range;
  return E::none;
}
NativeStorageIntentImage EncodeNativeStorageActionIntent(const I& i,u64 budget) noexcept {
  try {
    if(budget<kNativeStorageActionIntentBytes)return Fail(E::resource_exhausted);
    const auto error=ValidateNativeStorageActionIntent(i);if(error!=E::none)return Fail(error);
    std::vector<byte> b(kNativeStorageActionIntentBytes,0);std::copy(magic.begin(),magic.end(),b.begin());
    Put(b.data()+8,3,2);Put(b.data()+10,static_cast<u16>(i.action),2);Put(b.data()+12,b.size(),4);
    for(std::size_t n=0;n<ids.size();++n)PutId(b.data()+16+16*n,i.*ids[n]);
    PutRoot(b.data()+192,i.checkpoint);PutRoot(b.data()+272,i.allocation_root);
    std::copy(i.checkpoint_sha256.begin(),i.checkpoint_sha256.end(),b.begin()+352);
    std::copy(i.allocation_sha256.begin(),i.allocation_sha256.end(),b.begin()+384);
    for(std::size_t n=0;n<numbers.size();++n)Put(b.data()+416+8*n,i.*numbers[n],8);
    Put(b.data()+544,i.page_size_bytes,4);Put(b.data()+548,static_cast<u16>(i.intended_state),2);
    Put(b.data()+552,i.configuration_generation,8);
    for(std::size_t n=0;n<selected_ids.size();++n)PutId(b.data()+560+16*n,i.*selected_ids[n]);
    Put(b.data()+640,i.attachment_generation,8);Put(b.data()+648,i.storage_profile_generation,8);
    const auto hash=core::hash::ComputeSha256Digest(b.data(),736);
    if(!hash.ok())return Fail(E::hash_failure);
    std::copy(hash.digest.begin(),hash.digest.end(),b.begin()+736);
    NativeStorageIntentImage r;r.intent=i;r.bytes=std::move(b);r.error=E::none;return r;
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
  catch(const std::length_error&){return Fail(E::resource_exhausted);}
}
NativeStorageIntentImage DecodeNativeStorageActionIntent(const std::vector<byte>& b,u64 budget) noexcept {
  try {
    if(budget<kNativeStorageActionIntentBytes)return Fail(E::resource_exhausted);
    if(b.size()!=kNativeStorageActionIntentBytes||!std::equal(magic.begin(),magic.end(),b.begin())||
        Get(b.data()+8,2)!=3||Get(b.data()+12,4)!=b.size()||!Zero(b.data()+550,2)||!Zero(b.data()+656,80)||
        !Zero(b.data()+194,2)||!Zero(b.data()+264,8)||!Zero(b.data()+274,2)||!Zero(b.data()+344,8))
      return Fail(E::invalid_header);
    I i;i.action=static_cast<NativeStorageAction>(Get(b.data()+10,2));
    for(std::size_t n=0;n<ids.size();++n)i.*ids[n]=GetId(b.data()+16+16*n);
    i.checkpoint=GetRoot(b.data()+192);i.allocation_root=GetRoot(b.data()+272);
    std::copy_n(b.data()+352,32,i.checkpoint_sha256.begin());std::copy_n(b.data()+384,32,i.allocation_sha256.begin());
    for(std::size_t n=0;n<numbers.size();++n)i.*numbers[n]=Get(b.data()+416+8*n,8);
    i.page_size_bytes=static_cast<u32>(Get(b.data()+544,4));i.intended_state=static_cast<NativeStorageIntentState>(Get(b.data()+548,2));
    i.configuration_generation=Get(b.data()+552,8);
    for(std::size_t n=0;n<selected_ids.size();++n)i.*selected_ids[n]=GetId(b.data()+560+16*n);
    i.attachment_generation=Get(b.data()+640,8);i.storage_profile_generation=Get(b.data()+648,8);
    const auto error=ValidateNativeStorageActionIntent(i);if(error!=E::none)return Fail(error);
    const auto hash=core::hash::ComputeSha256Digest(b.data(),736);
    if(!hash.ok())return Fail(E::hash_failure);
    if(!std::equal(hash.digest.begin(),hash.digest.end(),b.begin()+736))return Fail(E::invalid_integrity);
    NativeStorageIntentImage r;r.intent=i;r.bytes=b;r.error=E::none;return r;
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
  catch(const std::length_error&){return Fail(E::resource_exhausted);}
}
NativeStorageIntentImage ReadNativeStorageActionIntentFromOperation(const NativeManagementOperation& o,u64 budget) noexcept {
  if(budget<kNativeStorageActionIntentBytes)return Fail(E::resource_exhausted);
  // Bound framing before hashing arbitrary retained input.
  if(o.normalized_request_bytes.size()!=kNativeStorageActionIntentBytes)return Fail(E::invalid_header);
  const auto validation=ValidateNativeManagementOperation(o);
  if(validation!=NativeManagementOperationError::none){
    auto r=Fail(validation==NativeManagementOperationError::hash_failure?E::hash_failure:
      validation==NativeManagementOperationError::resource_exhausted?E::resource_exhausted:E::operation_failure);
    r.operation_error=validation;return r;
  }
  auto r=DecodeNativeStorageActionIntent(o.normalized_request_bytes,budget);if(!r.ok())return r;
  const auto& i=*r.intent;
  if(o.scope==NativeManagementScope::cluster||o.uuid!=i.operation_uuid||o.database_uuid!=i.database_uuid||
      o.target_uuid!=i.filespace_uuid||o.initiator_uuid!=i.initiator_uuid||
      o.request_context_uuid!=i.request_context_uuid||o.policy_snapshot_uuid!=i.policy_snapshot_uuid||
      (o.generation_guards[0]&&*o.generation_guards[0]!=i.catalog_generation)||
      (o.generation_guards[1]&&*o.generation_guards[1]!=i.configuration_generation)||
      (o.generation_guards[2]&&*o.generation_guards[2]!=i.security_generation))return Fail(E::binding_mismatch);
  return r;
}
NativeStorageCapacityCheck CheckNativeStorageIntentCapacityFromOpenDevices(
    const I& i,const std::vector<disk::NativeFilespaceDevice>& devices,u64 budget) noexcept {
  using C=NativeStorageCapacityCheckError;
  const auto fail=[](C error){NativeStorageCapacityCheck r;r.error=error;return r;};
  const auto validation=ValidateNativeStorageActionIntent(i);
  if(validation!=E::none){auto r=fail(C::invalid_intent);r.intent_error=validation;return r;}
  // The call allowance and retained intent are ceilings, not actual grants.
  const auto allowance=std::min(budget,i.maximum_retained_image_bytes);
  if(allowance<=kNativeStorageActionIntentBytes){
    auto r=fail(C::capacity_failure);r.capacity.error=NativeFilespaceCapacityError::resource_exhausted;return r;
  }
  auto observed=ReadNativeFilespaceCapacityFromOpenDevices(i.database_uuid,devices,i.checkpoint,
    i.filespace_uuid,allowance-kNativeStorageActionIntentBytes);
  if(!observed.ok()){auto r=fail(C::capacity_failure);r.capacity=std::move(observed);return r;}
  const auto matched=MatchNativeStorageIntentCapacityObservation(i,*observed.observation);
  if(matched!=C::none)return fail(matched);
  observed.retained_image_bytes+=kNativeStorageActionIntentBytes;
  NativeStorageCapacityCheck r;r.error=C::none;r.capacity=std::move(observed);return r;
}
NativeStorageCapacityCheckError MatchNativeStorageIntentCapacityObservation(
    const I& i,const NativeFilespaceCapacityObservation& a) noexcept {
  using C=NativeStorageCapacityCheckError;
  if(ValidateNativeStorageActionIntent(i)!=E::none)return C::invalid_intent;
  if(a.database_uuid!=i.database_uuid||a.filespace_uuid!=i.filespace_uuid||
      a.locator_uuid!=i.locator_uuid||a.page_zero_uuid!=i.page_zero_uuid)return C::identity_mismatch;
  if(a.page_size_profile_uuid!=i.page_size_profile_uuid||a.page_size_bytes!=i.page_size_bytes)
    return C::profile_mismatch;
  const auto same=[](const disk::FilespaceRootReference& x,const disk::FilespaceRootReference& y){
    return x.kind==y.kind&&x.page_type==y.page_type&&x.filespace_uuid==y.filespace_uuid&&
      x.page_number==y.page_number&&x.page_generation==y.page_generation&&
      x.page_size_profile_uuid==y.page_size_profile_uuid&&x.object_uuid==y.object_uuid;
  };
  if(!same(a.checkpoint,i.checkpoint)||a.checkpoint_sha256!=i.checkpoint_sha256||
      a.checkpoint_generation!=i.checkpoint_generation||
      a.checkpoint_root_set_generation!=i.checkpoint_root_set_generation)return C::checkpoint_mismatch;
  if(!same(a.allocation_root,i.allocation_root)||a.allocation_sha256!=i.allocation_sha256)
    return C::allocation_mismatch;
  if(a.page_zero_generation!=i.page_zero_generation||a.filespace_root_set_generation!=i.filespace_root_set_generation||
      a.directory_generation!=i.directory_generation||a.map_generation!=i.map_generation||
      a.capacity_generation!=i.capacity_generation)return C::generation_mismatch;
  if(a.total_pages!=i.current_total_pages||a.physical_bytes!=i.current_total_pages*u64{i.page_size_bytes})
    return C::capacity_mismatch;
  return C::none;
}
} // namespace scratchbird::storage::database
