// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "filespace_page_zero.hpp"
#include "native_decoded_storage_ranges.hpp"
#include "native_management_operation.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>

namespace scratchbird::storage::database {
using namespace core::platform;
struct NativePageReservationAssignment {
 u64 page_number=0;
 Uuid allocation_uuid,page_uuid;
 u64 page_generation=1;
 u32 page_type=0;
 bool operator==(const NativePageReservationAssignment&) const =default;
};
// Canonical LOCAL transaction-owned reservation proposal. No page is allocated
// by validating/encoding this value. The publisher must revalidate the actual
// source, active transaction, policy/security/resources and every free/zero slot,
// then durably publish the exact map delta before returning a reservation.
struct NativePageReservationIntent {
 Uuid request_uuid,operation_uuid,database_uuid,filespace_uuid,page_size_profile_uuid;
 Uuid locator_uuid,page_zero_uuid,allocation_object_uuid,transaction_uuid,owner_uuid;
 Uuid initiator_uuid,request_context_uuid,policy_snapshot_uuid,security_snapshot_uuid;
 disk::NativePageReference checkpoint;
 Uuid checkpoint_object_uuid;
 std::array<byte,32> checkpoint_sha256{},allocation_sha256{};
 u64 allocation_page_number=0,allocation_page_generation=0;
 u64 local_transaction_id=0,selection_generation=0,checkpoint_generation=0,checkpoint_root_set_generation=0;
 u64 directory_generation=0,page_zero_generation=0,filespace_root_set_generation=0,map_generation=0;
 u64 capacity_generation=0,total_pages=0,catalog_generation=0,configuration_generation=0,security_generation=0;
 u64 maximum_work_bytes=0;
 std::span<const NativePageReservationAssignment> pages;
};
enum class NativePageReservationIntentError {
 none,invalid_header,invalid_identity,invalid_profile,invalid_reference,invalid_range,
 invalid_integrity,invalid_workspace,resource_exhausted,hash_failure,operation_failure,binding_mismatch
};
inline usize NativePageReservationIntentBytes(usize count) noexcept {
 return count&&count<=(std::numeric_limits<usize>::max()-544)/64?544+64*count:0;
}
struct NativePageReservationIntentImage {
 NativePageReservationIntentError error=NativePageReservationIntentError::invalid_header;
 std::optional<NativePageReservationIntent> intent;
 std::span<const byte> bytes;
 NativeManagementOperationError operation_error=NativeManagementOperationError::none;
 bool ok() const noexcept{return error==NativePageReservationIntentError::none&&intent&&!bytes.empty();}
};
namespace native_page_reservation_detail {
using I=NativePageReservationIntent;
using E=NativePageReservationIntentError;
inline constexpr std::array<byte,8> magic{'S','B','P','R','S','V','0','1'};
inline constexpr std::array<Uuid I::*,14> identities{
 &I::request_uuid,&I::operation_uuid,&I::database_uuid,&I::filespace_uuid,&I::page_size_profile_uuid,
 &I::locator_uuid,&I::page_zero_uuid,&I::allocation_object_uuid,&I::transaction_uuid,&I::owner_uuid,
 &I::initiator_uuid,&I::request_context_uuid,&I::policy_snapshot_uuid,&I::security_snapshot_uuid};
inline constexpr std::array<u64 I::*,14> numbers{
 &I::local_transaction_id,&I::selection_generation,&I::checkpoint_generation,&I::checkpoint_root_set_generation,
 &I::directory_generation,&I::page_zero_generation,&I::filespace_root_set_generation,&I::map_generation,
 &I::capacity_generation,&I::total_pages,&I::catalog_generation,&I::configuration_generation,
 &I::security_generation,&I::maximum_work_bytes};
inline bool V7(const Uuid& id) noexcept{return core::uuid::IsEngineIdentityUuid(id);}
inline bool Zero(std::span<const byte> b) noexcept{return std::all_of(b.begin(),b.end(),[](byte v){return !v;});}
inline void Put(byte* p,u64 n,unsigned width){for(unsigned k=0;k<width;++k)p[k]=static_cast<byte>(n>>(8*k));}
inline u64 Get(const byte* p,unsigned width){u64 n=0;for(unsigned k=0;k<width;++k)n|=u64{p[k]}<<(8*k);return n;}
inline void PutId(byte* p,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),p);}
inline Uuid GetId(const byte* p){Uuid id;std::copy_n(p,16,id.bytes.begin());return id;}
inline E Validate(const I& i,std::span<Uuid> scratch) noexcept {
 for(auto field:identities)if(!V7(i.*field))return E::invalid_identity;
 if(i.request_uuid==i.operation_uuid||!V7(i.checkpoint_object_uuid)||!V7(i.checkpoint.filespace_uuid))return E::invalid_identity;
 const auto* p=disk::FindCanonicalFilespacePageProfile(i.page_size_profile_uuid);
 const auto* cp=disk::FindCanonicalFilespacePageProfile(i.checkpoint.page_size_profile_uuid);
 if(!p||!cp)return E::invalid_profile;
 const auto max=std::numeric_limits<u64>::max();
 if(!i.checkpoint.page_number||!i.checkpoint.page_generation||i.checkpoint.page_number>=max/cp->page_size_bytes||
    !i.allocation_page_number||!i.allocation_page_generation||i.allocation_page_number>=i.total_pages||
    (i.checkpoint.filespace_uuid==i.filespace_uuid&&(i.checkpoint.page_size_profile_uuid!=i.page_size_profile_uuid||
       i.checkpoint.page_number>=i.total_pages)))return E::invalid_reference;
 if(Zero(i.checkpoint_sha256)||Zero(i.allocation_sha256))return E::invalid_integrity;
 // Catalog/configuration/security epochs may be zero during bootstrap; every
 // source/generation/transaction/capacity field before them must be positive.
 for(usize n=0;n<10;++n)if(!(i.*numbers[n]))return E::invalid_range;
 if(i.total_pages>max/p->page_size_bytes||i.pages.empty()||i.pages.size()>=i.total_pages||
    i.pages.size()>i.maximum_work_bytes/p->page_size_bytes)return E::invalid_range;
 if(i.pages.size()>scratch.size()/2)return E::resource_exhausted;
 u64 previous=0;
 for(usize n=0;n<i.pages.size();++n){const auto& a=i.pages[n];
  if(a.page_number<=previous||a.page_number>=i.total_pages||a.page_generation!=1)return E::invalid_range;
  if(a.page_number==i.allocation_page_number||
     (i.checkpoint.filespace_uuid==i.filespace_uuid&&a.page_number==i.checkpoint.page_number))return E::invalid_reference;
  if(!a.page_type||!disk::IsRegisteredNativePageType(a.page_type))return E::invalid_profile;
  for(const auto& id:{a.allocation_uuid,a.page_uuid}){
   if(!V7(id)||id==i.checkpoint_object_uuid||id==i.checkpoint.filespace_uuid||id==i.checkpoint.page_size_profile_uuid)return E::invalid_identity;
   for(auto field:identities)if(id==i.*field)return E::invalid_identity;
  }
  scratch[2*n]=a.allocation_uuid;scratch[2*n+1]=a.page_uuid;previous=a.page_number;
 }
 auto used=scratch.first(2*i.pages.size());std::sort(used.begin(),used.end());
 if(std::adjacent_find(used.begin(),used.end())!=used.end())return E::invalid_identity;
 return E::none;
}
} // namespace native_page_reservation_detail

// All regions must be disjoint. On failure output/scratch contents are
// unspecified but no successful image/view escapes. No heap fallback. This
// encoder/decoder requires externally admitted backing, not a mere byte ceiling.
inline NativePageReservationIntentImage EncodeNativePageReservationIntentInto(
    const NativePageReservationIntent& i,std::span<byte> output,std::span<Uuid> scratch,u64 budget) noexcept {
 using namespace native_page_reservation_detail;
 const auto fail=[](E e){return NativePageReservationIntentImage{e,{},{}};};
 const auto bytes=NativePageReservationIntentBytes(i.pages.size());
 if(!bytes||bytes>budget||bytes>output.size())return fail(E::resource_exhausted);
 if(!disk::detail::DisjointNativeDecodeRegions(std::span{&i,1},i.pages,output,scratch))return fail(E::invalid_workspace);
 const auto valid=Validate(i,scratch);if(valid!=E::none)return fail(valid);
 auto b=output.first(bytes);std::fill(b.begin(),b.end(),0);std::copy(magic.begin(),magic.end(),b.begin());
 Put(b.data()+8,1,2);Put(b.data()+10,512,2);Put(b.data()+12,64,2);Put(b.data()+16,bytes,8);Put(b.data()+24,i.pages.size(),8);
 for(usize n=0;n<identities.size();++n)PutId(b.data()+32+16*n,i.*identities[n]);
 PutId(b.data()+256,i.checkpoint.filespace_uuid);Put(b.data()+272,i.checkpoint.page_number,8);
 Put(b.data()+280,i.checkpoint.page_generation,8);PutId(b.data()+288,i.checkpoint.page_size_profile_uuid);
 PutId(b.data()+304,i.checkpoint_object_uuid);std::copy(i.checkpoint_sha256.begin(),i.checkpoint_sha256.end(),b.begin()+320);
 Put(b.data()+352,i.allocation_page_number,8);Put(b.data()+360,i.allocation_page_generation,8);
 std::copy(i.allocation_sha256.begin(),i.allocation_sha256.end(),b.begin()+368);
 for(usize n=0;n<numbers.size();++n)Put(b.data()+400+8*n,i.*numbers[n],8);
 for(usize n=0;n<i.pages.size();++n){const auto& a=i.pages[n];auto* p=b.data()+512+64*n;
  Put(p,a.page_number,8);PutId(p+8,a.allocation_uuid);PutId(p+24,a.page_uuid);Put(p+40,a.page_generation,8);Put(p+48,a.page_type,4);}
 const auto hash=core::hash::ComputeSha256DigestNative(b.data(),b.size()-32);
 if(!hash.ok())return fail(E::hash_failure);
 std::copy(hash.digest.begin(),hash.digest.end(),b.end()-32);
 return {E::none,i,b};
}
inline NativePageReservationIntentImage DecodeNativePageReservationIntentInto(
    std::span<const byte> b,std::span<NativePageReservationAssignment> pages,std::span<Uuid> scratch,u64 budget) noexcept {
 using namespace native_page_reservation_detail;
 const auto fail=[](E e){return NativePageReservationIntentImage{e,{},{}};};
 if(b.size()>budget)return fail(E::resource_exhausted);
 if(b.size()<608||!std::equal(magic.begin(),magic.end(),b.begin())||Get(b.data()+8,2)!=1||
    Get(b.data()+10,2)!=512||Get(b.data()+12,2)!=64||Get(b.data()+14,2)||Get(b.data()+16,8)!=b.size())return fail(E::invalid_header);
 const auto count=Get(b.data()+24,8);
 if(count>std::numeric_limits<usize>::max()||NativePageReservationIntentBytes(static_cast<usize>(count))!=b.size())return fail(E::invalid_header);
 if(count>pages.size()||count>scratch.size()/2)return fail(E::resource_exhausted);
 if(!disk::detail::DisjointNativeDecodeRegions(b,pages,scratch))return fail(E::invalid_workspace);
 const auto hash=core::hash::ComputeSha256DigestNative(b.data(),b.size()-32);
 if(!hash.ok())return fail(E::hash_failure);
 if(!std::equal(hash.digest.begin(),hash.digest.end(),b.end()-32))return fail(E::invalid_integrity);
 I i;for(usize n=0;n<identities.size();++n)i.*identities[n]=GetId(b.data()+32+16*n);
 i.checkpoint={GetId(b.data()+256),Get(b.data()+272,8),Get(b.data()+280,8),GetId(b.data()+288)};
 i.checkpoint_object_uuid=GetId(b.data()+304);std::copy_n(b.data()+320,32,i.checkpoint_sha256.begin());
 i.allocation_page_number=Get(b.data()+352,8);i.allocation_page_generation=Get(b.data()+360,8);
 std::copy_n(b.data()+368,32,i.allocation_sha256.begin());
 for(usize n=0;n<numbers.size();++n)i.*numbers[n]=Get(b.data()+400+8*n,8);
 for(usize n=0;n<count;++n){const auto* p=b.data()+512+64*n;
  if(!Zero(b.subspan(512+64*n+52,12)))return fail(E::invalid_header);
  pages[n]={Get(p,8),GetId(p+8),GetId(p+24),Get(p+40,8),static_cast<u32>(Get(p+48,4))};}
 i.pages=pages.first(static_cast<usize>(count));const auto valid=Validate(i,scratch);if(valid!=E::none)return fail(valid);
 return {E::none,i,b};
}
// Complete operation validation followed by exact LOCAL identity/epoch binding.
// Scratch is reused sequentially: max(2*N,2*step_count+7) identities. All backing
// and the immutable operation outlive the result. This supplies replay material,
// never descriptor selection, authorization, transaction admission or effects.
inline NativePageReservationIntentImage ReadNativePageReservationIntentFromOperationView(
    const NativeManagementOperationView& operation,std::span<NativePageReservationAssignment> pages,
    std::span<Uuid> scratch,u64 budget) noexcept {
 using E=NativePageReservationIntentError;
 const auto fail=[](E e){return NativePageReservationIntentImage{e,{},{}};};
 if(operation.normalized_request_bytes.size()>budget)return fail(E::resource_exhausted);
 // Neither validation nor decoding may overwrite ANY retained record material,
 // even when the corresponding field is not part of this family's binding.
 const auto text=[](std::string_view s){return std::span<const char>{s.data(),s.size()};};
 if(!disk::detail::DisjointNativeDecodeRegions(pages,scratch)||
    !disk::detail::DisjointNativeDecodeRegions(pages,std::span{&operation,1})||
    !disk::detail::DisjointNativeDecodeRegions(pages,operation.steps)||
    !disk::detail::DisjointNativeDecodeRegions(pages,operation.normalized_request_bytes)||
    !disk::detail::DisjointNativeDecodeRegions(pages,text(operation.idempotency_key)))return fail(E::invalid_workspace);
 for(const auto& s:operation.steps)
  if(!disk::detail::DisjointNativeDecodeRegions(pages,text(s.idempotency_key)))return fail(E::invalid_workspace);
 const auto valid=ValidateNativeManagementOperationView(operation,scratch);
 if(valid!=NativeManagementOperationError::none){
  auto r=fail(valid==NativeManagementOperationError::hash_failure?E::hash_failure:
    valid==NativeManagementOperationError::resource_exhausted?E::resource_exhausted:
    valid==NativeManagementOperationError::invalid_workspace?E::invalid_workspace:E::operation_failure);
  r.operation_error=valid;return r;
 }
 auto r=DecodeNativePageReservationIntentInto(operation.normalized_request_bytes,pages,scratch,budget);
 if(!r.ok())return r;
 const auto& i=*r.intent;
 if(operation.scope!=NativeManagementScope::local_node||operation.database_uuid!=i.database_uuid||
    operation.uuid!=i.operation_uuid||operation.target_uuid!=i.filespace_uuid||
    operation.initiator_uuid!=i.initiator_uuid||operation.request_context_uuid!=i.request_context_uuid||
    operation.policy_snapshot_uuid!=i.policy_snapshot_uuid||operation.security_snapshot_uuid!=i.security_snapshot_uuid||
    operation.generation_guards[0]!=std::optional<u64>{i.catalog_generation}||
    operation.generation_guards[1]!=std::optional<u64>{i.configuration_generation}||
    operation.generation_guards[2]!=std::optional<u64>{i.security_generation})return fail(E::binding_mismatch);
 return r;
}
} // namespace scratchbird::storage::database
