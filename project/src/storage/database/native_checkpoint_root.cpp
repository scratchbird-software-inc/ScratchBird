// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0


// SB_DRIFT_NATIVE_SHADOW_INTENT_HISTORY_LAYER: moved verbatim from database_dirty_manifest.cpp (R0).
// Deleted with the drift layer (R4).

#include "native_checkpoint_metadata_scratch.hpp"
#include "database_dirty_manifest.hpp"
#include "native_checkpoint_inventory_backing.hpp"
#include "native_current_checkpoint_source_backing.hpp"
#include "native_bound_checkpoint_selection_backing.hpp"
#include "native_allocation_chain_backing.hpp"
#include "native_directory_chain_backing.hpp"
#include "native_metadata_decode_scratch.hpp"
#include "native_inventory_chain_backing.hpp"
#include "native_management_control_authority_backing.hpp"
#include "native_decoded_storage_ranges.hpp"
#include "native_checkpoint_selection.hpp"
#include "native_management_control_authority.hpp"
#include "hash_digest_parts.hpp"
#include "disk_device.hpp"
#include "transaction_inventory_validation.hpp"

#include <algorithm>
#include <fstream>
#include <filesystem>
#include <functional>
#include <map>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>

namespace scratchbird::storage::database {

namespace native_checkpoint {
namespace disk=scratchbird::storage::disk;
namespace hash=scratchbird::core::hash;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using Error=NativeCheckpointError;
constexpr std::size_t family=128,entries=512,root_digest_at=336,digest_at=368;
constexpr std::array<byte,8> magic{{'S','B','C','P','N','T','0','1'}};
constexpr std::array<byte,8> domain{{'S','B','C','P','S','E','T','1'}};
constexpr std::array<byte,8> operation_magic{{'S','B','C','P','N','T','0','2'}};
constexpr std::array<byte,8> operation_domain{{'S','B','C','P','S','E','T','2'}};
constexpr std::array<u32,17> types{{0,0x301,0x302,9,3,5,10,11,8,5,0x303,0x305,0x307,0x308,0x309,0x30b,0x500}};
bool V7(const Uuid& id) { return scratchbird::core::uuid::IsEngineIdentityUuid(id); }
bool Zero(const byte* b,std::size_t size) { return std::all_of(b,b+size,[](byte v){return v==0;}); }
void Put(byte* b,const Uuid& id) {std::copy(id.bytes.begin(),id.bytes.end(),b);}
Uuid Get(const byte* b) {Uuid id;std::copy_n(b,16,id.bytes.begin());return id;}
void PutRef(byte* b,const disk::NativePageReference& r) {
  Put(b,r.filespace_uuid);StoreLittle64(b+16,r.page_number);StoreLittle64(b+24,r.page_generation);Put(b+32,r.page_size_profile_uuid);
}
disk::NativePageReference GetRef(const byte* b) {return {Get(b),LoadLittle64(b+16),LoadLittle64(b+24),Get(b+32)};}
template<class Root> disk::NativePageReference Self(const Root& r) {
  return {r.header.filespace_uuid,r.header.page_number,r.header.page_generation,r.header.page_size_profile_uuid};
}
bool RefValid(const disk::NativePageReference& r) {
  const auto* profile=disk::FindCanonicalFilespacePageProfile(r.page_size_profile_uuid);
  return V7(r.filespace_uuid)&&r.page_number&&r.page_generation&&profile
    &&r.page_number<std::numeric_limits<u64>::max()/profile->page_size_bytes
    &&disk::CheckFileDeviceExtent(r.page_number*profile->page_size_bytes,profile->page_size_bytes).ok();
}
bool SameSlot(const disk::NativePageReference& a,const disk::NativePageReference& b) {return a.filespace_uuid==b.filespace_uuid&&a.page_number==b.page_number;}
bool ProfilesAgree(const disk::NativePageReference& a,const disk::NativePageReference& b) {return a.filespace_uuid!=b.filespace_uuid||a.page_size_profile_uuid==b.page_size_profile_uuid;}
NativeCheckpointRootResult Fail(Error error) {return {error,std::nullopt,{}};}
template<class Root> Error Validate(const Root& r) {
  if(!disk::EncodeNativeCommonPageHeader(r.header).ok()||r.header.page_type!=0x300)return Error::invalid_header;
  const bool transaction_owner=r.creator_operation_uuid.is_nil();
  const bool valid_creator=transaction_owner?
    V7(r.creator_transaction_uuid)&&r.creator_local_transaction_id&&r.creator_local_transaction_id<=r.selected_local_transaction_id:
    V7(r.creator_operation_uuid)&&r.creator_transaction_uuid.is_nil()&&r.creator_local_transaction_id==0;
  if(!V7(r.object_uuid)||!V7(r.timeline_uuid)||!valid_creator||!r.checkpoint_generation
    ||!r.root_set_generation||!r.selected_local_transaction_id
    ||r.stable_local_transaction_id>r.local_durable_transaction_id
    ||r.local_durable_transaction_id>r.selected_local_transaction_id
    ||r.cluster_quorum_transaction_id>r.local_durable_transaction_id||(r.flags&~15ull))return Error::invalid_family;
  const auto self=Self(r);if(!RefValid(self))return Error::invalid_reference;
  const bool empty_digest=Zero(r.predecessor_sha256.data(),32);
  if((r.checkpoint_generation==1&&(r.predecessor||!empty_digest))
    ||(r.checkpoint_generation>1&&(!r.predecessor||empty_digest)))return Error::invalid_reference;
  if(r.predecessor&&(!RefValid(*r.predecessor)||SameSlot(self,*r.predecessor)||!ProfilesAgree(self,*r.predecessor)))return Error::invalid_reference;
  if(r.roots.size()<10||r.roots.size()>16)return Error::invalid_roots;
  u16 prior=0;u32 roles=0;
  for(std::size_t i=0;i<r.roots.size();++i) {
    const auto& target=r.roots[i];
    if(target.role<=prior||target.role>=types.size()||target.page_type!=types[target.role]
      ||!V7(target.object_uuid)||!RefValid(target.page)||Zero(target.sha256.data(),32)
      ||SameSlot(self,target.page)||!ProfilesAgree(self,target.page))return Error::invalid_roots;
    if(r.predecessor&&(SameSlot(*r.predecessor,target.page)||!ProfilesAgree(*r.predecessor,target.page)))return Error::invalid_roots;
    for(std::size_t j=0;j<i;++j) {
      const auto& other=r.roots[j];
      if(!ProfilesAgree(target.page,other.page))return Error::invalid_roots;
      if(SameSlot(target.page,other.page)&&(target.page!=other.page||target.object_uuid!=other.object_uuid
        ||target.page_type!=other.page_type||target.sha256!=other.sha256))return Error::invalid_roots;
    }
    prior=target.role;roles|=u32{1}<<target.role;
  }
  if((roles&0x7feu)!=0x7feu||static_cast<bool>(roles&(1u<<14))!=static_cast<bool>(r.flags&4)
    ||(!(r.flags&4)&&r.cluster_quorum_transaction_id))return Error::invalid_roots;
  return Error::none;
}
hash::Sha256PartsResult RootDigest(std::span<const byte> b,std::size_t used) {
  if(LoadLittle16(b.data()+family+8)==2){
    const hash::HashDigestSegment parts[]={{operation_domain.data(),operation_domain.size()},
      {b.data()+family+32,96},{b.data()+family+280,16},{b.data()+entries,used-entries}};
    return hash::ComputeSha256DigestPartsNative(parts,4);
  }
  const hash::HashDigestSegment parts[]={{domain.data(),domain.size()},{b.data()+family+32,96},{b.data()+entries,used-entries}};
  return hash::ComputeSha256DigestPartsNative(parts,3);
}
hash::Sha256PartsResult FullDigest(std::span<const byte> b) {
  const std::array<byte,32> zero{};
  const hash::HashDigestSegment parts[]={{b.data(),digest_at},{zero.data(),zero.size()},{b.data()+digest_at+32,b.size()-digest_at-32}};
  return hash::ComputeSha256DigestPartsNative(parts,3);
}
struct LockedFilespaces {
  Error error=Error::invalid_filespace;
  std::vector<disk::NativeFilespaceDevice> ordered;
  std::vector<std::unique_lock<std::recursive_mutex>> guards;
};
LockedFilespaces LockFilespaces(const std::vector<disk::NativeFilespaceDevice>& devices) {
  LockedFilespaces result;result.ordered=devices;auto& ordered=result.ordered;
  if(ordered.empty())return result;
  std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.filespace_uuid.bytes<b.filespace_uuid.bytes;});
  for(std::size_t i=0;i<ordered.size();++i){const auto& fs=ordered[i];
    if(!V7(fs.filespace_uuid)||!disk::FindCanonicalFilespacePageProfile(fs.page_size_profile_uuid)
      ||!fs.device||(i&&fs.filespace_uuid==ordered[i-1].filespace_uuid))return result;
    for(std::size_t j=0;j<i;++j)if(fs.device==ordered[j].device)return result;}
  result.guards.reserve(ordered.size());
  for(const auto& fs:ordered)result.guards.push_back(fs.device->AcquireOperationGuard());
  result.error=Error::none;return result;
}
} // namespace native_checkpoint

namespace native_checkpoint {
template<class Root> Error EncodeValues(const Root& r,std::span<byte> storage){
    const auto valid=Validate(r);if(valid!=Error::none)return (valid);
    if(storage.size()<r.header.page_size_bytes)return Error::resource_exhausted;
    auto b=storage.first(r.header.page_size_bytes);std::fill(b.begin(),b.end(),0);
    const auto common=disk::EncodeNativeCommonPageHeader(r.header);
    std::copy(common.bytes->begin(),common.bytes->end(),b.begin());auto* f=b.data()+family;
    const bool operation_owner=!r.creator_operation_uuid.is_nil();
    const auto& image_magic=operation_owner?operation_magic:magic;
    std::copy(image_magic.begin(),image_magic.end(),f);StoreLittle16(f+8,operation_owner?2:1);StoreLittle16(f+10,384);
    const auto used=entries+112*r.roots.size();StoreLittle32(f+12,static_cast<u32>(used));Put(f+16,r.object_uuid);
    StoreLittle64(f+32,r.checkpoint_generation);StoreLittle64(f+40,r.root_set_generation);
    StoreLittle64(f+48,r.selected_local_transaction_id);StoreLittle64(f+56,r.stable_local_transaction_id);
    StoreLittle64(f+64,r.local_durable_transaction_id);StoreLittle64(f+72,r.cluster_quorum_transaction_id);
    Put(f+80,r.timeline_uuid);Put(f+96,r.creator_transaction_uuid);StoreLittle64(f+112,r.creator_local_transaction_id);StoreLittle64(f+120,r.flags);
    if(r.predecessor)PutRef(f+128,*r.predecessor);
    std::copy(r.predecessor_sha256.begin(),r.predecessor_sha256.end(),f+176);
    StoreLittle64(f+272,r.completed?1:0);
    Put(f+280,r.creator_operation_uuid);
    for(std::size_t i=0;i<r.roots.size();++i){const auto& target=r.roots[i];auto* out=b.data()+entries+112*i;
      StoreLittle16(out,target.role);StoreLittle32(out+4,target.page_type);PutRef(out+8,target.page);Put(out+56,target.object_uuid);std::copy(target.sha256.begin(),target.sha256.end(),out+72);}
    const auto root_digest=RootDigest(b,used);if(!root_digest.ok())return (Error::hash_failure);
    std::copy(root_digest.digest.begin(),root_digest.digest.end(),b.begin()+root_digest_at);
    const auto full=FullDigest(b);if(!full.ok())return (Error::hash_failure);
    std::copy(full.digest.begin(),full.digest.end(),b.begin()+digest_at);return Error::none;
}
}
NativeCheckpointRootResult EncodeNativeCheckpointRoot(const NativeCheckpointRoot& r) noexcept {
  using namespace native_checkpoint;
  try {
    const auto valid=Validate(r);if(valid!=Error::none)return Fail(valid);
    std::vector<byte> b(r.header.page_size_bytes);
    const auto error=EncodeValues(r,b);if(error!=Error::none)return Fail(error);
    return {Error::none,r,std::move(b)};
  }catch(const std::bad_alloc&){return Fail(Error::resource_exhausted);}
   catch(const std::length_error&){return Fail(Error::resource_exhausted);}
   catch(...){return Fail(Error::invalid_family);}
}
NativeCheckpointRootEncodedView EncodeNativeCheckpointRootInto(
    const NativeCheckpointRootView& root,std::span<core::platform::byte> backing) noexcept {
  using namespace native_checkpoint;
  if(!disk::detail::DisjointNativeDecodeRegions(backing,std::span{&root,1},root.roots))
    return {Error::invalid_backing,{}};
  const auto error=EncodeValues(root,backing);
  if(error!=Error::none)return {error,{}};
  return {Error::none,backing.first(root.header.page_size_bytes)};
}

namespace native_checkpoint {
template<class Root,class Prepare> Error DecodeValues(std::span<const byte> b,Root& r,Prepare prepare) {
    const auto common=disk::DecodeNativeCommonPageHeader(b.data(),std::min<std::size_t>(b.size(),128));
    if(!common.ok()||common.header->page_type!=0x300||b.size()!=common.header->page_size_bytes)return Error::invalid_header;
    const auto full=FullDigest(b);if(!full.ok())return Error::hash_failure;
    if(!std::equal(full.digest.begin(),full.digest.end(),b.begin()+digest_at))return Error::invalid_integrity;
    const auto* f=b.data()+family;const auto used=LoadLittle32(f+12);
    const auto version=LoadLittle16(f+8);
    if(!((version==1&&std::equal(magic.begin(),magic.end(),f))||
         (version==2&&std::equal(operation_magic.begin(),operation_magic.end(),f)))||LoadLittle16(f+10)!=384
      ||used<entries+112*10||used>entries+112*16||(used-entries)%112||LoadLittle64(f+272)>1
      ||!Zero(f+(version==1?280:296),version==1?104:88)||!Zero(b.data()+used,b.size()-used))return Error::invalid_family;
    const auto digest=RootDigest(b,used);if(!digest.ok())return Error::hash_failure;
    if(!std::equal(digest.digest.begin(),digest.digest.end(),b.begin()+root_digest_at))return Error::invalid_integrity;
    if(!prepare(r,(used-entries)/112))return Error::resource_exhausted;
    r.header=*common.header;r.object_uuid=Get(f+16);
    r.checkpoint_generation=LoadLittle64(f+32);r.root_set_generation=LoadLittle64(f+40);
    r.selected_local_transaction_id=LoadLittle64(f+48);r.stable_local_transaction_id=LoadLittle64(f+56);
    r.local_durable_transaction_id=LoadLittle64(f+64);r.cluster_quorum_transaction_id=LoadLittle64(f+72);
    r.timeline_uuid=Get(f+80);r.creator_transaction_uuid=Get(f+96);r.creator_local_transaction_id=LoadLittle64(f+112);r.flags=LoadLittle64(f+120);
    if(version==2){r.creator_operation_uuid=Get(f+280);if(r.creator_operation_uuid.is_nil())return Error::invalid_family;}
    if(!Zero(f+128,48))r.predecessor=GetRef(f+128);
    std::copy_n(f+176,32,r.predecessor_sha256.begin());r.completed=LoadLittle64(f+272)==1;
    for(std::size_t at=entries;at<used;at+=112){const auto* in=b.data()+at;NativeCheckpointRootReference target;
      if(!Zero(in+2,2)||!Zero(in+104,8))return Error::invalid_roots;
      target.role=LoadLittle16(in);target.page_type=LoadLittle32(in+4);target.page=GetRef(in+8);target.object_uuid=Get(in+56);
      std::copy_n(in+72,32,target.sha256.begin());r.roots[(at-entries)/112]=target;}
    return Validate(r);
}
} // namespace native_checkpoint

NativeCheckpointRootResult DecodeNativeCheckpointRoot(const std::vector<scratchbird::core::platform::byte>& b) noexcept {
  using namespace native_checkpoint;
  try {
    NativeCheckpointRoot root;
    const auto error=DecodeValues(b,root,[](auto& out,std::size_t count){out.roots.resize(count);return true;});
    if(error!=Error::none)return Fail(error);
    return {Error::none,std::move(root),b};
  }catch(const std::bad_alloc&){return Fail(Error::resource_exhausted);}
   catch(const std::length_error&){return Fail(Error::resource_exhausted);}
   catch(...){return Fail(Error::invalid_family);}
}

NativeCheckpointRootViewResult DecodeNativeCheckpointRootInto(
    std::span<const scratchbird::core::platform::byte> b,std::span<NativeCheckpointRootReference> roots) noexcept {
  using namespace native_checkpoint;
  try {
    if(!page::detail::DisjointNativeDecodeRegions(b,roots))return {Error::invalid_backing,{}};
    NativeCheckpointRootView root;
    const auto error=DecodeValues(b,root,[&](auto& out,std::size_t count){
      if(count>roots.size())return false;out.roots=roots.first(count);return true;
    });
    if(error!=Error::none)return {error,{}};
    return {Error::none,std::move(root)};
  }catch(const std::bad_alloc&){return {Error::resource_exhausted,{}};}
   catch(const std::length_error&){return {Error::resource_exhausted,{}};}
   catch(...){return {Error::invalid_family,{}};}
}

namespace {
using CheckpointImage=std::span<const core::platform::byte>;
using CheckpointBatch=disk::FileDevice::ReadLatencyBatch;
template<class Receipt,class Nested> void MergeCheckpointIo(Receipt& out,Nested& nested){
  if(nested.physical_bytes_read>std::numeric_limits<u64>::max()-out.physical_bytes_read)throw NativeCheckpointError::resource_exhausted;
  out.physical_bytes_read+=nested.physical_bytes_read;
  out.io_status=nested.io_status;out.io_diagnostic=std::move(nested.io_diagnostic);
}
NativeCheckpointRoot OwnCheckpoint(const NativeCheckpointRootView& v){
  NativeCheckpointRoot out;
  out.header=v.header;out.object_uuid=v.object_uuid;out.checkpoint_generation=v.checkpoint_generation;
  out.root_set_generation=v.root_set_generation;out.selected_local_transaction_id=v.selected_local_transaction_id;
  out.stable_local_transaction_id=v.stable_local_transaction_id;out.local_durable_transaction_id=v.local_durable_transaction_id;
  out.cluster_quorum_transaction_id=v.cluster_quorum_transaction_id;out.timeline_uuid=v.timeline_uuid;
  out.creator_transaction_uuid=v.creator_transaction_uuid;out.creator_local_transaction_id=v.creator_local_transaction_id;
  out.flags=v.flags;out.predecessor=v.predecessor;out.predecessor_sha256=v.predecessor_sha256;
  out.completed=v.completed;out.creator_operation_uuid=v.creator_operation_uuid;out.roots.assign(v.roots.begin(),v.roots.end());return out;
}
}
NativeCheckpointRootDeviceRead detail::ReadNativeCheckpointRootBacked(
    disk::FileDevice& device,const core::platform::Uuid& database_uuid,const disk::FilespaceRootReference& ref,
    CheckpointBatch& observation,std::pmr::memory_resource& resource) noexcept {
  using namespace native_checkpoint;NativeCheckpointRootDeviceRead out;
  const auto fail=[&](Error e){out.error=e;out.root.reset();out.bytes={};return std::move(out);};
  try{
    const disk::NativePageReference target{ref.filespace_uuid,ref.page_number,ref.page_generation,ref.page_size_profile_uuid};
    if(!V7(database_uuid)||!V7(ref.object_uuid)||ref.kind!=9||ref.page_type!=0x300||!RefValid(target)||&observation.device()!=&device)return fail(Error::invalid_reference);
    detail::NativeCheckpointMetadataScratch scratch{&resource};const auto guard=device.AcquireOperationGuard();
    const disk::FilespaceBootstrapBinding binding{database_uuid,ref.filespace_uuid,ref.page_size_profile_uuid};
    const auto read=[&](u64 offset,void* bytes,std::size_t n){auto io=observation.ReadAt(offset,bytes,n);
      out.io_status=io.status;out.io_diagnostic=std::move(io.diagnostic);
      if(io.bytes_transferred>std::numeric_limits<u64>::max()-out.physical_bytes_read)throw Error::resource_exhausted;
      out.physical_bytes_read+=io.bytes_transferred;return io;};
    const auto size=[&]{auto io=device.Size();out.io_status=io.status;out.io_diagnostic=std::move(io.diagnostic);return io;};
    CheckpointImage observed;const auto zero=detail::ReadNativeMetadataPageZero(binding,scratch,read,size,observed);
    if(!zero.ok()){
      if(zero.error==disk::FilespacePageZeroError::resource_exhausted)return fail(Error::resource_exhausted);
      if(zero.error==disk::FilespacePageZeroError::hash_provider_failure)return fail(Error::hash_failure);
      if(zero.error==disk::FilespacePageZeroError::io_failure)return fail(Error::io_failure);
      return fail(Error::invalid_filespace);}
    const auto& z=*zero.record;
    if(z.bootstrap.filespace_role>4||ref.page_number>=z.total_pages)return fail(Error::invalid_filespace);
    if(z.bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted)return fail(Error::encrypted_requires_crypto_authority);
    auto b=scratch.Array<byte>(z.bootstrap.page_size_bytes);const auto io=read(ref.page_number*u64{z.bootstrap.page_size_bytes},b.data(),b.size());
    if(!io.ok()||io.bytes_transferred!=b.size())return fail(Error::io_failure);
    const auto common=disk::DecodeNativeCommonPageHeader(b.data(),128);if(!common.ok())return fail(Error::invalid_header);
    if(common.header->flags&1u)return fail(Error::encrypted_requires_crypto_authority);
    auto result=scratch.Checkpoint(b);if(!result.ok())return fail(result.error);const auto& r=*result.root;
    if(r.header.database_uuid!=database_uuid||Self(r)!=target||r.object_uuid!=ref.object_uuid)return fail(Error::binding_mismatch);
    if(r.predecessor&&r.predecessor->filespace_uuid==ref.filespace_uuid&&r.predecessor->page_number>=z.total_pages)return fail(Error::invalid_reference);
    for(const auto& root:r.roots)if(root.page.filespace_uuid==ref.filespace_uuid&&root.page.page_number>=z.total_pages)return fail(Error::invalid_reference);
    out.error=Error::none;out.root=r;out.bytes=b;return out;
  }catch(Error error){return fail(error);}catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}
   catch(const std::length_error&){return fail(Error::resource_exhausted);}catch(...){return fail(Error::io_failure);}
}
NativeCheckpointRootResult ReadNativeCheckpointRootFromOpenDevice(
    disk::FileDevice& device,const core::platform::Uuid& database,const disk::FilespaceRootReference& ref) noexcept{
  using namespace native_checkpoint;
  try{
    detail::NativeMetadataHeapMemory heap;std::pmr::monotonic_buffer_resource resource(&heap);
    NativeCheckpointRootDeviceRead read;{CheckpointBatch batch(device);read=detail::ReadNativeCheckpointRootBacked(device,database,ref,batch,resource);}
    if(!read.ok())return Fail(read.error);
    return {Error::none,OwnCheckpoint(*read.root),{read.bytes.begin(),read.bytes.end()}};
  }catch(const std::bad_alloc&){return Fail(Error::resource_exhausted);}
   catch(const std::length_error&){return Fail(Error::resource_exhausted);}catch(...){return Fail(Error::io_failure);}
}
NativeCheckpointInventoryDeviceRead detail::VerifyNativeCheckpointInventoryBacked(
    const core::platform::Uuid& database_uuid,std::span<const disk::NativeFilespaceDevice> devices,
    const disk::FilespaceRootReference& checkpoint,u64 maximum_retained_image_bytes,
    std::span<CheckpointBatch* const> observations,std::pmr::memory_resource& resource,
    NativeSharedControlAuthority* shared) noexcept {
  using namespace native_checkpoint;namespace mga=transaction::mga;
  NativeCheckpointInventoryDeviceRead out;
  const auto fail=[&](Error e,page::NativeInventoryError nested=page::NativeInventoryError::none){
    out.inventory={};out.inventory.error=e;out.inventory.inventory_error=nested;return std::move(out);};
  try{
    if(!V7(database_uuid)||devices.empty()||!maximum_retained_image_bytes||observations.size()!=devices.size())return fail(Error::invalid_reference);
    detail::NativeMetadataScratch scratch{&resource};std::pmr::vector<disk::NativeFilespaceDevice> ordered(devices.begin(),devices.end(),&resource);
    std::pmr::vector<CheckpointBatch*> batches(&resource);batches.reserve(ordered.size());
    std::pmr::vector<std::unique_lock<std::recursive_mutex>> guards(&resource);guards.reserve(ordered.size());
    for(std::size_t i=0;i<devices.size();++i)if(!devices[i].device||!observations[i]||&observations[i]->device()!=devices[i].device)return fail(Error::invalid_filespace);
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    for(std::size_t i=0;i<ordered.size();++i){const auto& fs=ordered[i];
      if(!V7(fs.filespace_uuid)||!disk::FindCanonicalFilespacePageProfile(fs.page_size_profile_uuid)||!fs.device||(i&&fs.filespace_uuid==ordered[i-1].filespace_uuid))return fail(Error::invalid_filespace);
      for(std::size_t j=0;j<i;++j)if(fs.device==ordered[j].device)return fail(Error::invalid_filespace);
      const auto at=std::find_if(devices.begin(),devices.end(),[&](const auto& file){return file.device==fs.device;});
      batches.push_back(observations[at-devices.begin()]);}
    for(const auto& file:ordered)guards.push_back(file.device->AcquireOperationGuard());
    const auto fs=std::lower_bound(ordered.begin(),ordered.end(),checkpoint.filespace_uuid,[](const auto& a,const auto& id){return a.filespace_uuid<id;});
    if(fs==ordered.end()||fs->filespace_uuid!=checkpoint.filespace_uuid||fs->page_size_profile_uuid!=checkpoint.page_size_profile_uuid)return fail(Error::invalid_filespace);
    const auto* profile=disk::FindCanonicalFilespacePageProfile(checkpoint.page_size_profile_uuid);
    if(!profile||profile->page_size_bytes>=maximum_retained_image_bytes)return fail(Error::resource_exhausted);
    auto loaded=detail::ReadNativeCheckpointRootBacked(*fs->device,database_uuid,checkpoint,*batches[fs-ordered.begin()],resource);
    MergeCheckpointIo(out,loaded);if(!loaded.ok())return fail(loaded.error);
    if(!loaded.root->completed)return fail(Error::incomplete);
    const auto& target=loaded.root->roots.front();
    const disk::FilespaceRootReference inventory_ref{4,target.page_type,target.page.filespace_uuid,
      target.page.page_number,target.page.page_generation,target.page.page_size_profile_uuid,target.object_uuid};
    auto read=page::detail::ReadNativeInventoryChainBacked(database_uuid,ordered,inventory_ref,
      maximum_retained_image_bytes-loaded.bytes.size(),page::NativeInventoryChainReadContext::current,nullptr,{},batches,resource);
    // SB_COMPACT_TRANSACTION_STATE_PAGE (owner rulings 2026-11-05): an
    // in-place inventory (family SBTIPW01 and its header chain) is updated in
    // place and carries no digest, so it is not bound to the checkpoint's
    // role-1 digest, and Next is not tied to the checkpoint's selected
    // transaction. Only the constructor's checkpoint binds one today. The same
    // reader serves the control authority (SB_TRANSITIONAL_LEGACY_INVENTORY).
    const bool in_place=read.in_place_head;
    if(in_place){MergeCheckpointIo(out,read);
      auto again=page::detail::ReadNativeInPlaceInventoryBacked(database_uuid,ordered,inventory_ref,
        maximum_retained_image_bytes-loaded.bytes.size(),batches,resource,read.in_place_head_image);
      if(!again.handled)return fail(Error::inventory_failure,page::NativeInventoryError::invalid_family);
      read=std::move(again.read);}
    MergeCheckpointIo(out,read);const auto& chain=read.chain;
    if(!chain.ok()){using I=page::NativeInventoryError;const auto error=chain.error==I::resource_exhausted?Error::resource_exhausted:
      chain.error==I::hash_failure?Error::hash_failure:chain.error==I::io_failure?Error::io_failure:
      chain.error==I::encrypted_requires_crypto_authority?Error::encrypted_requires_crypto_authority:Error::inventory_failure;
      return fail(error,chain.error);}
    const auto& root=*loaded.root;
    if(in_place){if(!root.creator_operation_uuid.is_nil())return fail(Error::inventory_mismatch);}
    else{
    const auto head=chain.pages.front().image;const auto digest=hash::ComputeSha256DigestNative(head.data(),head.size());
    if(!digest.ok())return fail(Error::hash_failure);
    if(digest.digest!=target.sha256)return fail(Error::invalid_integrity);
    if(chain.inventory.next_local_transaction_id<=root.selected_local_transaction_id||
      chain.inventory.next_local_transaction_id-1!=root.selected_local_transaction_id)return fail(Error::inventory_mismatch);
    }
    const auto checkpoint_digest=hash::ComputeSha256DigestNative(loaded.bytes.data(),loaded.bytes.size());
    if(!checkpoint_digest.ok())return fail(Error::hash_failure);
    u64 operation_work=0;
    if(root.creator_operation_uuid.is_nil()){
      const auto id=mga::MakeLocalTransactionId(root.creator_local_transaction_id);
      const auto creator=std::find_if(chain.inventory.entries.begin(),chain.inventory.entries.end(),[&](const auto& entry){return entry.identity.local_id.value==id.value;});
      if(!id.valid()||creator==chain.inventory.entries.end()||creator->identity.transaction_uuid.value!=root.creator_transaction_uuid||
        (!(root.flags&4)&&creator->identity.scope!=mga::TransactionScope::local_node))return fail(Error::inventory_mismatch);
      if(!mga::HasCommittedInventoryOutcome(*creator))return fail(Error::creator_not_committed);
    }else{
      const u64 remaining=maximum_retained_image_bytes-loaded.bytes.size()-chain.retained_image_bytes;
      // MEM-CONTROL-AUTHORITY-READ-ONCE: reuse the selection's authority of
      // this filespace, still charged and budget-checked here.
      const bool share=shared&&shared->filespace==root.header.filespace_uuid;
      NativeManagementControlAuthorityView authority;
      if(share&&shared->view){
        if(shared->view->verified_image_bytes>remaining)return fail(Error::resource_exhausted);
        authority=*shared->view;
      }else{
        auto proof=detail::ReadNativeManagementControlAuthorityBacked(database_uuid,ordered,root.header.filespace_uuid,remaining,
          NativeManagementHistoryReadContext::selected,nullptr,{},batches,resource);MergeCheckpointIo(out,proof);
        if(!proof.ok()){using P=NativeManagementControlAuthorityError;return fail(proof.authority.error==P::hash_failure?Error::hash_failure:
          proof.authority.error==P::resource_exhausted?Error::resource_exhausted:proof.authority.error==P::io_failure?Error::io_failure:
          proof.authority.error==P::encrypted_requires_authority?Error::encrypted_requires_crypto_authority:
          proof.authority.error==P::cluster_requires_authority?Error::cluster_requires_authority:Error::creator_not_committed);}
        authority=proof.authority;
        if(share)shared->view=authority;
      }
      if(!MatchesNativeManagementPublishedCheckpoint(authority,root,checkpoint_digest.digest))return fail(Error::creator_not_committed);
      operation_work=authority.verified_image_bytes;
    }
    NativeCheckpointInventoryView result;result.error=Error::none;result.checkpoint_sha256=checkpoint_digest.digest;
    result.inventory_generation=chain.pages.front().page.inventory_generation;
    result.retained_image_bytes=loaded.bytes.size()+chain.retained_image_bytes+operation_work;
    auto identities=scratch.Array<NativeInventoryPageBinding>(chain.pages.size());
    for(std::size_t n=0;n<chain.pages.size();++n)identities[n]={chain.pages[n].page.header,chain.pages[n].page.object_uuid};
    result.inventory_pages=identities;result.checkpoint=root;result.inventory=chain.inventory;out.inventory=result;return out;
  }catch(Error error){return fail(error);}catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}
   catch(const std::length_error&){return fail(Error::resource_exhausted);}catch(...){return fail(Error::io_failure);}
}
NativeCheckpointInventoryDeviceRead VerifyNativeCheckpointInventoryInto(
    const core::platform::Uuid& database,std::span<const disk::NativeFilespaceDevice> files,
    const disk::FilespaceRootReference& root,u64 budget,std::span<CheckpointBatch* const> observations,
    std::span<core::platform::byte> backing) noexcept {
  NativeCheckpointInventoryDeviceRead out;
  const auto disjoint=[&](auto v){return disk::detail::DisjointNativeDecodeRegions(backing,v);};
  bool valid=disjoint(std::span{&database,1})&&disjoint(files)&&disjoint(std::span{&root,1})&&disjoint(observations);
  for(const auto& file:files)valid=valid&&(!file.device||disjoint(std::span{file.device,1}));
  for(const auto* observation:observations)valid=valid&&(!observation||disjoint(std::span{observation,1}));
  if(!valid){out.inventory.error=NativeCheckpointError::invalid_backing;return out;}
  detail::NativeMetadataMemory resource(backing);
  out=detail::VerifyNativeCheckpointInventoryBacked(database,files,root,budget,observations,resource);
  if(out.ok())out.inventory.backing_bytes_used=resource.used();return out;
}
NativeCheckpointInventoryResult VerifyNativeCheckpointInventoryFromOpenDevices(
    const core::platform::Uuid& database,const std::vector<disk::NativeFilespaceDevice>& files,
    const disk::FilespaceRootReference& root,u64 budget) noexcept{
  using namespace native_checkpoint;
  const auto fail=[](Error error){NativeCheckpointInventoryResult r;r.error=error;return r;};
  try{
    detail::NativeMetadataHeapMemory heap;std::pmr::monotonic_buffer_resource resource(&heap);detail::NativeMetadataScratch scratch{&resource};
    auto pointers=scratch.Array<CheckpointBatch*>(files.size());
    if(files.size()>std::numeric_limits<std::size_t>::max()/sizeof(CheckpointBatch))throw std::bad_alloc();
    auto* batches=static_cast<CheckpointBatch*>(resource.allocate(files.size()*sizeof(CheckpointBatch),alignof(CheckpointBatch)));
    NativeCheckpointInventoryDeviceRead read;
    {
      struct Cleanup{CheckpointBatch* batches;std::size_t count=0;~Cleanup(){while(count)std::destroy_at(batches+--count);}} cleanup{batches};
      for(std::size_t n=0;n<files.size();++n){if(!files[n].device)return fail(Error::invalid_filespace);
        pointers[n]=std::construct_at(batches+n,*files[n].device);++cleanup.count;}
      read=detail::VerifyNativeCheckpointInventoryBacked(database,files,root,budget,pointers,resource);
    }
    const auto& v=read.inventory;
    if(!read.ok()){auto r=fail(v.error);r.inventory_error=v.inventory_error;return r;}
    NativeCheckpointInventoryResult result;result.error=Error::none;result.checkpoint=OwnCheckpoint(*v.checkpoint);
    result.checkpoint_sha256=v.checkpoint_sha256;result.inventory.next_local_transaction_id=v.inventory.next_local_transaction_id;
    result.inventory.next_commit_sequence=v.inventory.next_commit_sequence;result.inventory.entries.assign(v.inventory.entries.begin(),v.inventory.entries.end());
    result.inventory_pages.assign(v.inventory_pages.begin(),v.inventory_pages.end());result.inventory_generation=v.inventory_generation;
    result.retained_image_bytes=v.retained_image_bytes;return result;
  }catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}
   catch(const std::length_error&){return fail(Error::resource_exhausted);}catch(...){return fail(Error::io_failure);}
}

NativeCheckpointCatalogResult VerifyNativeCheckpointCatalogRootsFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u64 maximum_retained_image_bytes) noexcept {
  using namespace native_checkpoint;
  const auto fail=[](Error error){NativeCheckpointCatalogResult r;r.error=error;return r;};
  try {
    if(!V7(database_uuid)||devices.empty()||!maximum_retained_image_bytes)return fail(Error::invalid_reference);
    auto locked=LockFilespaces(devices);if(locked.error!=Error::none)return fail(locked.error);
    auto pair=VerifyNativeCheckpointInventoryFromOpenDevices(database_uuid,locked.ordered,checkpoint,
                                                           maximum_retained_image_bytes);
    if(!pair.ok()) {
      auto failure=fail(pair.error);
      failure.checkpoint_inventory.error=pair.error;
      failure.checkpoint_inventory.inventory_error=pair.inventory_error;
      return failure;
    }
    NativeCheckpointCatalogResult result;
    result.retained_image_bytes=pair.retained_image_bytes;
    result.catalogs.reserve(2);
    const auto& roots=pair.checkpoint->roots;
    const auto catalog=std::find_if(roots.begin(),roots.end(),[](const auto& root){return root.role==5;});
    const auto feature=std::find_if(roots.begin(),roots.end(),[](const auto& root){return root.role==9;});
    if(catalog==roots.end()||feature==roots.end())return fail(Error::invalid_roots);
    for(const auto* target:{&*catalog,&*feature}) {
      if(target==&*feature&&target->page==catalog->page&&target->object_uuid==catalog->object_uuid
          &&target->page_type==catalog->page_type&&target->sha256==catalog->sha256)continue;
      const auto fs=std::lower_bound(locked.ordered.begin(),locked.ordered.end(),target->page.filespace_uuid,
        [](const auto& device,const auto& id){return device.filespace_uuid<id;});
      if(fs==locked.ordered.end()||fs->filespace_uuid!=target->page.filespace_uuid
          ||fs->page_size_profile_uuid!=target->page.page_size_profile_uuid)return fail(Error::invalid_filespace);
      const auto* profile=disk::FindCanonicalFilespacePageProfile(target->page.page_size_profile_uuid);
      if(!profile||profile->page_size_bytes>maximum_retained_image_bytes-result.retained_image_bytes)
        return fail(Error::resource_exhausted);
      const disk::FilespaceRootReference ref{static_cast<u16>(target->role==5?2:8),target->page_type,
        target->page.filespace_uuid,target->page.page_number,target->page.page_generation,
        target->page.page_size_profile_uuid,target->object_uuid};
      auto loaded=scratchbird::storage::page::ReadNativeCatalogRootFromOpenDevice(*fs->device,database_uuid,ref);
      if(!loaded.ok()) {auto failure=fail(Error::catalog_failure);failure.catalog_error=loaded.error;return failure;}
      const auto digest=hash::ComputeSha256Digest(loaded.bytes);
      if(!digest.ok())return fail(Error::hash_failure);
      if(digest.digest!=target->sha256)return fail(Error::invalid_integrity);
      const auto creator=scratchbird::transaction::mga::LookupLocalTransaction(pair.inventory,
        scratchbird::transaction::mga::MakeLocalTransactionId(loaded.root->creator_local_transaction_id));
      if(!creator.ok()||creator.entry.identity.transaction_uuid.value!=loaded.root->creator_transaction_uuid
          ||(!(pair.checkpoint->flags&4)&&creator.entry.identity.scope!=scratchbird::transaction::mga::TransactionScope::local_node))
        return fail(Error::catalog_creator_mismatch);
      if(!scratchbird::transaction::mga::HasCommittedInventoryOutcome(creator.entry))
        return fail(Error::catalog_creator_not_committed);
      result.retained_image_bytes+=loaded.bytes.size();
      if(target==&*feature)result.feature_root_index=1;
      result.catalogs.push_back(std::move(loaded));
    }
    result.checkpoint_inventory=std::move(pair);
    result.error=Error::none;
    return result;
  }catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}
   catch(const std::length_error&){return fail(Error::resource_exhausted);}
   catch(...){return fail(Error::io_failure);}
}

NativeCheckpointPolicyRootsResult VerifyNativeCheckpointPolicyRootsFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u64 maximum_retained_image_bytes) noexcept {
  using namespace native_checkpoint;
  namespace mga=scratchbird::transaction::mga;
  const auto fail=[](Error error){NativeCheckpointPolicyRootsResult r;r.error=error;return r;};
  try {
    if(!V7(database_uuid)||devices.empty()||!maximum_retained_image_bytes)return fail(Error::invalid_reference);
    auto locked=LockFilespaces(devices);if(locked.error!=Error::none)return fail(locked.error);
    auto catalog=VerifyNativeCheckpointCatalogRootsFromOpenDevices(database_uuid,locked.ordered,checkpoint,
                                                                  maximum_retained_image_bytes);
    if(!catalog.ok()) {
      auto failure=fail(catalog.error);failure.catalog_error=catalog.catalog_error;
      failure.catalog.error=catalog.error;
      failure.catalog.checkpoint_inventory.error=catalog.checkpoint_inventory.error;
      failure.catalog.checkpoint_inventory.inventory_error=catalog.checkpoint_inventory.inventory_error;
      return failure;
    }
    NativeCheckpointPolicyRootsResult result;result.retained_image_bytes=catalog.retained_image_bytes;
    const auto& pair=catalog.checkpoint_inventory;
    for(std::size_t index=0;index<2;++index) {
      const u16 kind=static_cast<u16>(index+6);
      const auto target=std::find_if(pair.checkpoint->roots.begin(),pair.checkpoint->roots.end(),
                                     [kind](const auto& root){return root.role==kind;});
      if(target==pair.checkpoint->roots.end())return fail(Error::invalid_roots);
      const auto fs=std::lower_bound(locked.ordered.begin(),locked.ordered.end(),target->page.filespace_uuid,
          [](const auto& file,const auto& id){return file.filespace_uuid<id;});
      if(fs==locked.ordered.end()||fs->filespace_uuid!=target->page.filespace_uuid||
          fs->page_size_profile_uuid!=target->page.page_size_profile_uuid)return fail(Error::invalid_filespace);
      const auto* profile=disk::FindCanonicalFilespacePageProfile(target->page.page_size_profile_uuid);
      if(!profile||profile->page_size_bytes>maximum_retained_image_bytes-result.retained_image_bytes)
        return fail(Error::resource_exhausted);
      const disk::FilespaceRootReference ref{kind,target->page_type,target->page.filespace_uuid,
          target->page.page_number,target->page.page_generation,target->page.page_size_profile_uuid,target->object_uuid};
      auto root=scratchbird::storage::page::ReadNativeCatalogRootFromOpenDevice(*fs->device,database_uuid,ref);
      if(!root.ok()){auto failure=fail(Error::catalog_failure);failure.catalog_error=root.error;return failure;}
      const auto digest=hash::ComputeSha256Digest(root.bytes);
      if(!digest.ok())return fail(Error::hash_failure);
      if(digest.digest!=target->sha256)return fail(Error::invalid_integrity);
      const auto creator=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(root.root->creator_local_transaction_id));
      if(!creator.ok()||creator.entry.identity.transaction_uuid.value!=root.root->creator_transaction_uuid||
          (!(pair.checkpoint->flags&4)&&creator.entry.identity.scope!=mga::TransactionScope::local_node))
        return fail(Error::catalog_creator_mismatch);
      if(!mga::HasCommittedInventoryOutcome(creator.entry))return fail(Error::catalog_creator_not_committed);
      const auto& actual=root.root->roots.front();
      const auto& expected=catalog.catalogs.front().root->roots[index==0?4:3];
      if(actual.role!=expected.role||actual.page_type!=expected.page_type||actual.page!=expected.page||
          actual.object_uuid!=expected.object_uuid)return fail(Error::policy_relation_mismatch);
      result.retained_image_bytes+=root.bytes.size();result.policies[index]=std::move(root);
    }
    result.catalog=std::move(catalog);result.error=Error::none;return result;
  }catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}
   catch(const std::length_error&){return fail(Error::resource_exhausted);}
   catch(...){return fail(Error::io_failure);}
}

namespace {
struct CurrentCheckpointInventory {
  NativeCheckpointInventoryResult pair;
  bool selector_bound=false;
};
CurrentCheckpointInventory ResolveCurrentCheckpointInventory(
    const core::platform::Uuid& database_uuid,const std::vector<disk::NativeFilespaceDevice>& devices,
    const disk::FilespaceRootReference& requested,u64 budget) {
  CurrentCheckpointInventory result;const disk::NativeFilespaceDevice* selector_owner=nullptr;
  for(const auto& file:devices){
    const disk::FilespaceBootstrapBinding binding{database_uuid,file.filespace_uuid,file.page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*file.device,&binding);
    if(!zero.ok()){result.pair.error=zero.error==disk::FilespacePageZeroError::resource_exhausted?NativeCheckpointError::resource_exhausted:
      zero.error==disk::FilespacePageZeroError::hash_provider_failure?NativeCheckpointError::hash_failure:
      zero.error==disk::FilespacePageZeroError::io_failure?NativeCheckpointError::io_failure:NativeCheckpointError::invalid_filespace;return result;}
    if(std::any_of(zero.record->roots.begin(),zero.record->roots.end(),[](const auto& r){return r.kind==18;})){
      if(selector_owner){result.pair.error=NativeCheckpointError::binding_mismatch;return result;}selector_owner=&file;
    }
  }
  if(!selector_owner){result.pair=VerifyNativeCheckpointInventoryFromOpenDevices(database_uuid,devices,requested,budget);return result;}
  auto selected=ReadNativeBoundCheckpointSelectionFromOpenDevices(database_uuid,devices,selector_owner->filespace_uuid,budget);
  if(!selected.ok()){result.pair.error=selected.error==NativeCheckpointSelectionError::resource_exhausted?NativeCheckpointError::resource_exhausted:
    selected.error==NativeCheckpointSelectionError::hash_failure?NativeCheckpointError::hash_failure:
    selected.error==NativeCheckpointSelectionError::io_failure?NativeCheckpointError::io_failure:NativeCheckpointError::binding_mismatch;
    result.pair.inventory_error=selected.checkpoint_inventory.inventory_error;return result;}
  const auto& s=*selected.selection;
  if(requested.kind!=9||requested.page_type!=0x300||requested.object_uuid!=s.checkpoint_object_uuid||
    disk::NativePageReference{requested.filespace_uuid,requested.page_number,requested.page_generation,requested.page_size_profile_uuid}!=s.checkpoint){
    result.pair.error=NativeCheckpointError::binding_mismatch;return result;
  }
  result.pair=std::move(selected.checkpoint_inventory);
  // Selection admission also read predecessors, allocation maps and exact
  // operation lineage. Its downstream consumers must charge that complete
  // work, not just the retained checkpoint/inventory subset moved above.
  result.pair.retained_image_bytes=selected.retained_image_bytes;
  result.selector_bound=true;return result;
}
}

namespace {
using CurrentSourceError=NativeCheckpointError;
struct CurrentInventoryView {NativeCheckpointInventoryView pair;bool selector_bound=false;};
template<class Receipt> struct CurrentSourceContext {
 const Uuid& database;
 detail::NativeMetadataScratch scratch;
 Receipt& receipt;
 std::pmr::vector<disk::NativeFilespaceDevice> files;
 std::pmr::vector<CheckpointBatch*> batches;
 std::pmr::vector<std::unique_lock<std::recursive_mutex>> guards;
 CurrentSourceContext(const Uuid& db,std::span<const disk::NativeFilespaceDevice> supplied,
   std::span<CheckpointBatch* const> observations,std::pmr::memory_resource& resource,Receipt& out)
   :database(db),scratch{&resource},receipt(out),files(supplied.begin(),supplied.end(),&resource),
    batches(&resource),guards(&resource){
  using E=CurrentSourceError;
  if(observations.size()!=supplied.size())throw E::invalid_filespace;
  batches.reserve(files.size());guards.reserve(files.size());
  for(std::size_t n=0;n<supplied.size();++n)
   if(!supplied[n].device||!observations[n]||&observations[n]->device()!=supplied[n].device)throw E::invalid_filespace;
  std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
  for(std::size_t n=0;n<files.size();++n){const auto& f=files[n];
   if(!native_checkpoint::V7(f.filespace_uuid)||!disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid)||
    !f.device||(n&&files[n-1].filespace_uuid==f.filespace_uuid))throw E::invalid_filespace;
   for(std::size_t j=0;j<n;++j)if(files[j].device==f.device)throw E::invalid_filespace;
   const auto at=std::find_if(supplied.begin(),supplied.end(),[&](const auto& file){return file.device==f.device;});
   batches.push_back(observations[at-supplied.begin()]);
  }
  for(const auto& file:files)guards.push_back(file.device->AcquireOperationGuard());
 }
 auto Read(const disk::NativeFilespaceDevice& file,u64 offset,void* bytes,std::size_t size){
  const auto index=&file-files.data();auto io=batches[index]->ReadAt(offset,bytes,size);
  receipt.io_status=io.status;receipt.io_diagnostic=std::move(io.diagnostic);
  if(io.bytes_transferred>std::numeric_limits<u64>::max()-receipt.physical_bytes_read)throw CurrentSourceError::resource_exhausted;
  receipt.physical_bytes_read+=io.bytes_transferred;return io;
 }
 auto Zero(const disk::NativeFilespaceDevice& file){
  const disk::FilespaceBootstrapBinding binding{database,file.filespace_uuid,file.page_size_profile_uuid};CheckpointImage image;
  return detail::ReadNativeMetadataPageZero(binding,scratch,[&](u64 at,void* p,std::size_t n){return Read(file,at,p,n);},
   [&]{auto io=file.device->Size();receipt.io_status=io.status;receipt.io_diagnostic=std::move(io.diagnostic);return io;},image);
 }
 auto Inventory(const disk::FilespaceRootReference& root,u64 budget){
  auto read=detail::VerifyNativeCheckpointInventoryBacked(database,files,root,budget,batches,*scratch.resource);
  MergeCheckpointIo(receipt,read);return read.inventory;
 }
 auto Selection(const Uuid& owner,u64 budget){
  auto read=detail::ReadNativeBoundCheckpointSelectionBacked(database,files,owner,budget,batches,*scratch.resource);
  MergeCheckpointIo(receipt,read);return read.selection;
 }
 auto Directory(const disk::FilespaceRootReference& root,u64 budget){
  auto read=page::detail::ReadNativeDirectoryChainBacked(database,files,root,budget,
   page::NativeDirectoryChainReadContext::current,nullptr,{},batches,*scratch.resource);
  MergeCheckpointIo(receipt,read);return read.chain;
 }
 auto Allocation(const disk::NativeFilespaceDevice& file,const disk::FilespaceRootReference* root,u64 budget){
  const disk::FilespaceBootstrapBinding binding{database,file.filespace_uuid,file.page_size_profile_uuid};
  auto read=page::detail::ReadNativeAllocationChainBacked(*file.device,binding,budget,
   root?page::NativeAllocationChainReadContext::selected:page::NativeAllocationChainReadContext::bootstrap,
   root,nullptr,{},*batches[&file-files.data()],*scratch.resource);
  MergeCheckpointIo(receipt,read);return read.chain;
 }
 auto Proof(const Uuid& primary,u64 budget){
  auto read=detail::ReadNativeManagementControlAuthorityBacked(database,files,primary,budget,
   NativeManagementHistoryReadContext::selected,nullptr,{},batches,*scratch.resource);
  MergeCheckpointIo(receipt,read);return read.authority;
 }
 CurrentInventoryView Current(const disk::FilespaceRootReference& requested,u64 budget){
  CurrentInventoryView out;const disk::NativeFilespaceDevice* owner=nullptr;
  for(const auto& file:files){const auto zero=Zero(file);
   if(!zero.ok()){out.pair.error=zero.error==disk::FilespacePageZeroError::resource_exhausted?CurrentSourceError::resource_exhausted:
    zero.error==disk::FilespacePageZeroError::hash_provider_failure?CurrentSourceError::hash_failure:
    zero.error==disk::FilespacePageZeroError::io_failure?CurrentSourceError::io_failure:CurrentSourceError::invalid_filespace;return out;}
   if(std::any_of(zero.record->roots.begin(),zero.record->roots.end(),[](const auto& r){return r.kind==18;})){
    if(owner){out.pair.error=CurrentSourceError::binding_mismatch;return out;}owner=&file;
   }
  }
  if(!owner){out.pair=Inventory(requested,budget);return out;}
  const auto selected=Selection(owner->filespace_uuid,budget);
  if(!selected.ok()){out.pair.error=selected.error==NativeCheckpointSelectionError::resource_exhausted?CurrentSourceError::resource_exhausted:
   selected.error==NativeCheckpointSelectionError::hash_failure?CurrentSourceError::hash_failure:
   selected.error==NativeCheckpointSelectionError::io_failure?CurrentSourceError::io_failure:CurrentSourceError::binding_mismatch;
   out.pair.inventory_error=selected.checkpoint_inventory.inventory_error;return out;}
  const auto& s=*selected.selection;
  if(requested.kind!=9||requested.page_type!=0x300||requested.object_uuid!=s.checkpoint_object_uuid||
   disk::NativePageReference{requested.filespace_uuid,requested.page_number,requested.page_generation,requested.page_size_profile_uuid}!=s.checkpoint){
   out.pair.error=CurrentSourceError::binding_mismatch;return out;
  }
  out.pair=selected.checkpoint_inventory;out.pair.retained_image_bytes=selected.retained_image_bytes;out.selector_bound=true;return out;
 }
};
auto CurrentCreator(const page::NativeTransactionInventoryView& inventory,u64 number){
 return std::find_if(inventory.entries.begin(),inventory.entries.end(),[&](const auto& entry){return entry.identity.local_id.value==number;});
}
template<class Receipt> NativeCheckpointAllocationView ReadCurrentAllocationView(
 const Uuid& database_uuid,std::span<const disk::NativeFilespaceDevice> devices,
 const disk::FilespaceRootReference& checkpoint,u64 maximum_retained_image_bytes,
 std::span<CheckpointBatch* const> observations,std::pmr::memory_resource& resource,Receipt& receipt) noexcept {
  using namespace native_checkpoint;
  namespace mga = scratchbird::transaction::mga;
  namespace page = scratchbird::storage::page;
  const auto fail=[](Error error){NativeCheckpointAllocationView result;result.error=error;return result;};
  try {
    if(!V7(database_uuid)||devices.empty()||!maximum_retained_image_bytes)return fail(Error::invalid_reference);
    CurrentSourceContext context(database_uuid,devices,observations,resource,receipt);
    auto current_inventory=context.Current(checkpoint,maximum_retained_image_bytes);
    auto pair=std::move(current_inventory.pair);
    if(!pair.ok()) {
      auto result=fail(pair.error);result.checkpoint_inventory.error=pair.error;
      result.checkpoint_inventory.inventory_error=pair.inventory_error;return result;
    }
    const auto fs=std::lower_bound(context.files.begin(),context.files.end(),checkpoint.filespace_uuid,
        [](const auto& entry,const auto& id){return entry.filespace_uuid<id;});
    if(fs==context.files.end()||fs->filespace_uuid!=checkpoint.filespace_uuid||
        fs->page_size_profile_uuid!=checkpoint.page_size_profile_uuid)return fail(Error::invalid_filespace);
    const disk::FilespaceBootstrapBinding binding{database_uuid,fs->filespace_uuid,fs->page_size_profile_uuid};
    const auto zero=context.Zero(*fs);
    if(!zero.ok()) {
      if(zero.error==disk::FilespacePageZeroError::resource_exhausted)return fail(Error::resource_exhausted);
      if(zero.error==disk::FilespacePageZeroError::hash_provider_failure)return fail(Error::hash_failure);
      if(zero.error==disk::FilespacePageZeroError::io_failure)return fail(Error::io_failure);
      return fail(Error::invalid_filespace);
    }
    const auto& z=*zero.record;
    const auto same=[](const auto& a,const auto& b){return a.kind==b.kind&&a.page_type==b.page_type&&
        a.filespace_uuid==b.filespace_uuid&&a.page_number==b.page_number&&a.page_generation==b.page_generation&&
        a.page_size_profile_uuid==b.page_size_profile_uuid&&a.object_uuid==b.object_uuid;};
    const auto current=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& ref){return ref.kind==9;});
    if(!current_inventory.selector_bound&&(current==z.roots.end()||!same(*current,checkpoint)||pair.checkpoint->root_set_generation!=z.root_set_generation))
      return fail(Error::binding_mismatch);
    const auto target=std::find_if(pair.checkpoint->roots.begin(),pair.checkpoint->roots.end(),
                                  [](const auto& ref){return ref.role==4;});
    const auto actual=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& ref){return ref.kind==3;});
    if(target==pair.checkpoint->roots.end()||(!current_inventory.selector_bound&&actual==z.roots.end()))return fail(Error::invalid_roots);
    const disk::FilespaceRootReference expected{3,target->page_type,target->page.filespace_uuid,
        target->page.page_number,target->page.page_generation,target->page.page_size_profile_uuid,target->object_uuid};
    if(!current_inventory.selector_bound&&!same(*actual,expected))return fail(Error::binding_mismatch);
    const auto map_file=std::lower_bound(context.files.begin(),context.files.end(),expected.filespace_uuid,[](const auto& f,const auto& id){return f.filespace_uuid<id;});
    if(map_file==context.files.end()||map_file->filespace_uuid!=expected.filespace_uuid||map_file->page_size_profile_uuid!=expected.page_size_profile_uuid)return fail(Error::invalid_filespace);
    const disk::FilespaceBootstrapBinding map_binding{database_uuid,map_file->filespace_uuid,map_file->page_size_profile_uuid};
    auto allocation=context.Allocation(*map_file,current_inventory.selector_bound?&expected:nullptr,maximum_retained_image_bytes-pair.retained_image_bytes);
    if(!allocation.ok()) {
      auto result=fail(Error::allocation_failure);result.allocation_error=allocation.error;return result;
    }
    const auto digest=hash::ComputeSha256DigestNative(allocation.pages.front().image.data(),allocation.pages.front().image.size());
    if(!digest.ok())return fail(Error::hash_failure);
    if(digest.digest!=target->sha256)return fail(Error::invalid_integrity);
    if(pair.retained_image_bytes>maximum_retained_image_bytes||allocation.retained_image_bytes>maximum_retained_image_bytes-pair.retained_image_bytes)
      return fail(Error::resource_exhausted);
    u64 retained=pair.retained_image_bytes+allocation.retained_image_bytes;
    std::optional<NativeManagementControlAuthorityView> operation_proof;
    const bool operation_map=std::any_of(allocation.pages.begin(),allocation.pages.end(),[](const auto& image){return !image.map.creator_operation_uuid.is_nil();});
    const bool operation_owned=operation_map||std::any_of(allocation.pages.begin(),allocation.pages.end(),[](const auto& image){
      return std::any_of(image.map.records.begin(),image.map.records.end(),
        [](const auto& record){return !record.creator_operation_uuid.is_nil();});
    });
    if(operation_owned){
      if(retained==maximum_retained_image_bytes)return fail(Error::resource_exhausted);
      auto proof=context.Proof(pair.checkpoint->header.filespace_uuid,maximum_retained_image_bytes-retained);
      if(!proof.ok()){using P=NativeManagementControlAuthorityError;
        return fail(proof.error==P::resource_exhausted?Error::resource_exhausted:
          proof.error==P::hash_failure?Error::hash_failure:proof.error==P::io_failure?Error::io_failure:
          proof.error==P::encrypted_requires_authority?Error::encrypted_requires_crypto_authority:
          proof.error==P::cluster_requires_authority?Error::cluster_requires_authority:
          operation_map?Error::allocation_creator_mismatch:Error::allocation_record_creator_mismatch);
      }
      const auto& h=pair.checkpoint->header;
      if(proof.selection->checkpoint!=disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid}||
        proof.selection->checkpoint_sha256!=pair.checkpoint_sha256)return fail(Error::binding_mismatch);
      if(proof.verified_image_bytes>maximum_retained_image_bytes-retained)return fail(Error::resource_exhausted);
      retained+=proof.verified_image_bytes;operation_proof=std::move(proof);
    }
    for(const auto& image:allocation.pages) {
      const auto& map=image.map;
      if(map.creator_operation_uuid.is_nil()){
        const auto creator=CurrentCreator(pair.inventory,map.creator_local_transaction_id);
        if(!map.creator_local_transaction_id||creator==pair.inventory.entries.end()||creator->identity.transaction_uuid.value!=map.creator_transaction_uuid||
            (!(pair.checkpoint->flags&4)&&creator->identity.scope!=mga::TransactionScope::local_node))
          return fail(Error::allocation_creator_mismatch);
        if(!mga::HasCommittedInventoryOutcome(*creator))return fail(Error::allocation_creator_not_committed);
      }else if(!operation_proof||!MatchesNativeManagementControlMap(*operation_proof,map))return fail(Error::allocation_creator_mismatch);
      for(const auto& record:map.records) {
        if(record.creator_operation_uuid.is_nil()){
          const auto original=CurrentCreator(pair.inventory,record.creator_local_transaction_id);
          if(!record.creator_local_transaction_id||original==pair.inventory.entries.end()||original->identity.transaction_uuid.value!=record.creator_transaction_uuid||
              (!(pair.checkpoint->flags&4)&&original->identity.scope!=mga::TransactionScope::local_node))
            return fail(Error::allocation_record_creator_mismatch);
        }else if(!operation_proof||!MatchesNativeManagementControlAllocation(*operation_proof,map.header.filespace_uuid,record,map.states[record.page_number-map.first_page]))return fail(Error::allocation_record_creator_mismatch);
      }
    }
    NativeCheckpointAllocationView result;result.error=Error::none;
    result.retained_image_bytes=retained;
    result.checkpoint_inventory=std::move(pair);result.allocation=std::move(allocation);return result;
  }catch(Error error){return fail(error);}catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}
   catch(const std::length_error&){return fail(Error::resource_exhausted);}
   catch(...){return fail(Error::io_failure);}
}
template<class Receipt> NativeCheckpointDirectoryView ReadCurrentDirectoryView(
 const Uuid& database_uuid,std::span<const disk::NativeFilespaceDevice> devices,
 const disk::FilespaceRootReference& checkpoint,u64 maximum_retained_image_bytes,
 std::span<CheckpointBatch* const> observations,std::pmr::memory_resource& resource,Receipt& receipt,
 const NativeBoundCheckpointSelectionView* retained_selection=nullptr) noexcept {
  using namespace native_checkpoint;
  namespace mga=scratchbird::transaction::mga;
  namespace page=scratchbird::storage::page;
  const auto fail=[](Error error){NativeCheckpointDirectoryView r;r.error=error;return r;};
  try {
    if(!V7(database_uuid)||devices.empty()||!maximum_retained_image_bytes)return fail(Error::invalid_reference);
    CurrentSourceContext context(database_uuid,devices,observations,resource,receipt);
    CurrentInventoryView current_inventory;
    if(retained_selection){
      if(!retained_selection->ok()||retained_selection->selection->header.database_uuid!=database_uuid)return fail(Error::binding_mismatch);
      const auto& selected=*retained_selection->selection;
      if(checkpoint.kind!=9||checkpoint.page_type!=0x300||checkpoint.object_uuid!=selected.checkpoint_object_uuid||
          disk::NativePageReference{checkpoint.filespace_uuid,checkpoint.page_number,checkpoint.page_generation,checkpoint.page_size_profile_uuid}!=selected.checkpoint)
        return fail(Error::binding_mismatch);
      current_inventory.pair=retained_selection->checkpoint_inventory;
      current_inventory.pair.retained_image_bytes=retained_selection->retained_image_bytes;
      current_inventory.selector_bound=true;
    }else current_inventory=context.Current(checkpoint,maximum_retained_image_bytes);
    auto pair=std::move(current_inventory.pair);
    if(!pair.ok()){auto failure=fail(pair.error);failure.checkpoint_inventory.error=pair.error;
      failure.checkpoint_inventory.inventory_error=pair.inventory_error;return failure;}
    const auto fs=std::lower_bound(context.files.begin(),context.files.end(),checkpoint.filespace_uuid,
      [](const auto& file,const auto& id){return file.filespace_uuid<id;});
    if(fs==context.files.end()||fs->filespace_uuid!=checkpoint.filespace_uuid||
        fs->page_size_profile_uuid!=checkpoint.page_size_profile_uuid)return fail(Error::invalid_filespace);
    const disk::FilespaceBootstrapBinding binding{database_uuid,fs->filespace_uuid,fs->page_size_profile_uuid};
    const auto zero=context.Zero(*fs);
    if(!zero.ok()){
      if(zero.error==disk::FilespacePageZeroError::resource_exhausted)return fail(Error::resource_exhausted);
      if(zero.error==disk::FilespacePageZeroError::hash_provider_failure)return fail(Error::hash_failure);
      if(zero.error==disk::FilespacePageZeroError::io_failure)return fail(Error::io_failure);
      return fail(Error::invalid_filespace);
    }
    const auto& z=*zero.record;
    const auto same=[](const auto& a,const auto& b){return a.kind==b.kind&&a.page_type==b.page_type&&
      a.filespace_uuid==b.filespace_uuid&&a.page_number==b.page_number&&a.page_generation==b.page_generation&&
      a.page_size_profile_uuid==b.page_size_profile_uuid&&a.object_uuid==b.object_uuid;};
    const auto current=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==9;});
    if(!current_inventory.selector_bound&&(current==z.roots.end()||!same(*current,checkpoint)||pair.checkpoint->root_set_generation!=z.root_set_generation))
      return fail(Error::binding_mismatch);
    const auto target=std::find_if(pair.checkpoint->roots.begin(),pair.checkpoint->roots.end(),[](const auto& r){return r.role==3;});
    const auto actual=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==5;});
    if(target==pair.checkpoint->roots.end()||(!current_inventory.selector_bound&&actual==z.roots.end()))return fail(Error::invalid_roots);
    const disk::FilespaceRootReference expected{5,target->page_type,target->page.filespace_uuid,target->page.page_number,
      target->page.page_generation,target->page.page_size_profile_uuid,target->object_uuid};
    if(!current_inventory.selector_bound&&!same(*actual,expected))return fail(Error::binding_mismatch);
    const u64 inventory_charge=retained_selection?0:pair.retained_image_bytes;
    if(inventory_charge>=maximum_retained_image_bytes)return fail(Error::resource_exhausted);
    auto directory=context.Directory(expected,maximum_retained_image_bytes-inventory_charge);
    if(!directory.ok()){auto failure=fail(Error::directory_failure);failure.directory_error=directory.error;return failure;}
    const auto digest=hash::ComputeSha256DigestNative(directory.pages.front().image.data(),directory.pages.front().image.size());
    if(!digest.ok())return fail(Error::hash_failure);
    if(digest.digest!=target->sha256)return fail(Error::invalid_integrity);
    if(directory.retained_image_bytes>maximum_retained_image_bytes-inventory_charge)
      return fail(Error::resource_exhausted);
    u64 retained=inventory_charge+directory.retained_image_bytes;
    const auto& root=directory.pages.front().directory;
    if(root.creator_operation_uuid.is_nil()){
      const auto creator=CurrentCreator(pair.inventory,root.creator_local_transaction_id);
      if(!root.creator_local_transaction_id||creator==pair.inventory.entries.end()||creator->identity.transaction_uuid.value!=root.creator_transaction_uuid||
          (!(pair.checkpoint->flags&4)&&creator->identity.scope!=mga::TransactionScope::local_node))
        return fail(Error::directory_creator_mismatch);
      if(!mga::HasCommittedInventoryOutcome(*creator))return fail(Error::directory_creator_not_committed);
    }else{
      if(retained==maximum_retained_image_bytes)return fail(Error::resource_exhausted);
      const auto proof=context.Proof(pair.checkpoint->header.filespace_uuid,maximum_retained_image_bytes-retained);
      if(!proof.ok()){using P=NativeManagementControlAuthorityError;
        return fail(proof.error==P::resource_exhausted?Error::resource_exhausted:
          proof.error==P::hash_failure?Error::hash_failure:proof.error==P::io_failure?Error::io_failure:
          proof.error==P::encrypted_requires_authority?Error::encrypted_requires_crypto_authority:
          proof.error==P::cluster_requires_authority?Error::cluster_requires_authority:Error::directory_creator_mismatch);
      }
      const auto& h=pair.checkpoint->header;
      if(proof.selection->checkpoint!=disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid}||
         proof.selection->checkpoint_sha256!=pair.checkpoint_sha256)return fail(Error::binding_mismatch);
      if(proof.verified_image_bytes>maximum_retained_image_bytes-retained)return fail(Error::resource_exhausted);
      retained+=proof.verified_image_bytes;
      if(!MatchesNativeManagementPublishedDirectory(proof,directory,digest.digest))return fail(Error::directory_creator_mismatch);
    }
    NativeCheckpointDirectoryView result;result.error=Error::none;
    result.retained_image_bytes=retained;
    result.checkpoint_inventory=std::move(pair);result.directory=std::move(directory);return result;
  }catch(Error error){return fail(error);}catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}
   catch(const std::length_error&){return fail(Error::resource_exhausted);}
   catch(...){return fail(Error::io_failure);}
}
NativeCheckpointInventoryResult OwnCurrentInventory(const NativeCheckpointInventoryView& v){
 NativeCheckpointInventoryResult out;out.error=v.error;out.inventory_error=v.inventory_error;
 if(!v.ok())return out;
 out.checkpoint=OwnCheckpoint(*v.checkpoint);out.checkpoint_sha256=v.checkpoint_sha256;
 out.inventory.next_local_transaction_id=v.inventory.next_local_transaction_id;
 out.inventory.next_commit_sequence=v.inventory.next_commit_sequence;
 out.inventory.entries.assign(v.inventory.entries.begin(),v.inventory.entries.end());
 out.inventory_pages.assign(v.inventory_pages.begin(),v.inventory_pages.end());
 out.inventory_generation=v.inventory_generation;out.retained_image_bytes=v.retained_image_bytes;return out;
}
page::NativeAllocationChainResult OwnCurrentAllocation(const page::NativeAllocationChainView& v){
 page::NativeAllocationChainResult out;out.error=v.error;if(!v.ok())return out;
 out.state_counts=v.state_counts;out.retained_image_bytes=v.retained_image_bytes;out.pages.reserve(v.pages.size());
 for(const auto& image:v.pages){const auto& x=image.map;page::NativeAllocationMap map;
  map.header=x.header;map.object_uuid=x.object_uuid;map.map_generation=x.map_generation;map.capacity_generation=x.capacity_generation;
  map.total_pages=x.total_pages;map.first_page=x.first_page;map.creator_transaction_uuid=x.creator_transaction_uuid;
  map.creator_local_transaction_id=x.creator_local_transaction_id;map.creator_operation_uuid=x.creator_operation_uuid;
  map.next=x.next;map.next_sha256=x.next_sha256;
  map.states.assign(x.states.begin(),x.states.end());map.records.assign(x.records.begin(),x.records.end());
  page::NativeAllocationMapResult value;value.error=page::NativeAllocationError::none;
  value.map=std::move(map);value.bytes.assign(image.image.begin(),image.image.end());out.pages.push_back(std::move(value));
 }return out;
}
page::NativeFilespaceDirectoryChainResult OwnCurrentDirectory(const page::NativeDirectoryChainView& v){
 page::NativeFilespaceDirectoryChainResult out;out.error=v.error;if(!v.ok())return out;
 out.retained_image_bytes=v.retained_image_bytes;out.pages.reserve(v.pages.size());
 for(const auto& image:v.pages){const auto& x=image.directory;page::NativeFilespaceDirectory d;
  d.header=x.header;d.object_uuid=x.object_uuid;d.directory_generation=x.directory_generation;
  d.creator_transaction_uuid=x.creator_transaction_uuid;d.creator_local_transaction_id=x.creator_local_transaction_id;
  d.total_records=x.total_records;d.first_record=x.first_record;d.next=x.next;d.next_sha256=x.next_sha256;
  d.records.assign(x.records.begin(),x.records.end());d.creator_operation_uuid=x.creator_operation_uuid;
  page::NativeFilespaceDirectoryResult p;p.error=page::NativeDirectoryError::none;
  p.directory=std::move(d);p.bytes.assign(image.image.begin(),image.image.end());out.pages.push_back(std::move(p));
 }return out;
}
NativeCheckpointAllocationResult OwnCurrent(const NativeCheckpointAllocationView& v){
 NativeCheckpointAllocationResult out;out.error=v.error;out.allocation_error=v.allocation_error;
 out.checkpoint_inventory=OwnCurrentInventory(v.checkpoint_inventory);if(!v.ok())return out;
 out.allocation=OwnCurrentAllocation(v.allocation);out.retained_image_bytes=v.retained_image_bytes;return out;
}
NativeCheckpointDirectoryResult OwnCurrent(const NativeCheckpointDirectoryView& v){
 NativeCheckpointDirectoryResult out;out.error=v.error;out.directory_error=v.directory_error;
 out.checkpoint_inventory=OwnCurrentInventory(v.checkpoint_inventory);if(!v.ok())return out;
 out.directory=OwnCurrentDirectory(v.directory);out.retained_image_bytes=v.retained_image_bytes;return out;
}
} // namespace
NativeCurrentCheckpointAllocationDeviceRead detail::VerifyCurrentNativeCheckpointAllocationBacked(
 const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,const disk::FilespaceRootReference& root,
 u64 budget,std::span<CheckpointBatch* const> observations,std::pmr::memory_resource& resource) noexcept {
 NativeCurrentCheckpointAllocationDeviceRead out;
 out.allocation=ReadCurrentAllocationView(database,files,root,budget,observations,resource,out);return out;
}
NativeCurrentCheckpointAllocationDeviceRead VerifyCurrentNativeCheckpointAllocationInto(
 const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,const disk::FilespaceRootReference& root,
 u64 budget,std::span<CheckpointBatch* const> observations,std::span<byte> backing) noexcept {
 NativeCurrentCheckpointAllocationDeviceRead out;
 const auto disjoint=[&](auto value){return disk::detail::DisjointNativeDecodeRegions(backing,value);};
 bool valid=disjoint(std::span{&database,1})&&disjoint(files)&&disjoint(std::span{&root,1})&&disjoint(observations);
 for(const auto& file:files)valid=valid&&(!file.device||disjoint(std::span{file.device,1}));
 for(const auto* batch:observations)valid=valid&&(!batch||disjoint(std::span{batch,1}));
 if(!valid){out.allocation.error=NativeCheckpointError::invalid_backing;return out;}
 detail::NativeMetadataMemory resource(backing);
 out=detail::VerifyCurrentNativeCheckpointAllocationBacked(database,files,root,budget,observations,resource);
 if(out.ok())out.allocation.backing_bytes_used=resource.used();return out;
}
NativeCheckpointAllocationResult VerifyCurrentNativeCheckpointAllocationFromOpenDevices(
 const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& files,
 const disk::FilespaceRootReference& root,u64 budget) noexcept {
 const auto fail=[](NativeCheckpointError error){NativeCheckpointAllocationResult out;out.error=error;return out;};
 try{
  if(!native_checkpoint::V7(database)||files.empty()||!budget)return fail(NativeCheckpointError::invalid_reference);
  detail::NativeMetadataHeapMemory heap;std::pmr::monotonic_buffer_resource resource(&heap);
  detail::NativeMetadataScratch scratch{&resource};auto pointers=scratch.Array<CheckpointBatch*>(files.size());
  if(files.size()>std::numeric_limits<std::size_t>::max()/sizeof(CheckpointBatch))throw std::bad_alloc();
  auto* batches=static_cast<CheckpointBatch*>(resource.allocate(files.size()*sizeof(CheckpointBatch),alignof(CheckpointBatch)));
  NativeCurrentCheckpointAllocationDeviceRead read;
  {struct Cleanup{CheckpointBatch* batches;std::size_t count=0;~Cleanup(){while(count)std::destroy_at(batches+--count);}} cleanup{batches};
   for(std::size_t n=0;n<files.size();++n){if(!files[n].device)return fail(NativeCheckpointError::invalid_filespace);
    pointers[n]=std::construct_at(batches+n,*files[n].device);++cleanup.count;}
   read=detail::VerifyCurrentNativeCheckpointAllocationBacked(database,files,root,budget,pointers,resource);
  }
  return OwnCurrent(read.allocation);
 }catch(const std::bad_alloc&){return fail(NativeCheckpointError::resource_exhausted);}
  catch(const std::length_error&){return fail(NativeCheckpointError::resource_exhausted);}
  catch(...){return fail(NativeCheckpointError::io_failure);}
}
NativeCurrentCheckpointDirectoryDeviceRead detail::VerifyCurrentNativeCheckpointDirectoryBacked(
 const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,const disk::FilespaceRootReference& root,
 u64 budget,std::span<CheckpointBatch* const> observations,std::pmr::memory_resource& resource) noexcept {
 NativeCurrentCheckpointDirectoryDeviceRead out;
 out.directory=ReadCurrentDirectoryView(database,files,root,budget,observations,resource,out);return out;
}
NativeCurrentCheckpointDirectoryDeviceRead detail::VerifyRetainedNativeCheckpointDirectoryBacked(
 const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,const disk::FilespaceRootReference& root,
 const NativeBoundCheckpointSelectionView& selected,u64 budget,std::span<CheckpointBatch* const> observations,
 std::pmr::memory_resource& resource) noexcept {
 NativeCurrentCheckpointDirectoryDeviceRead out;
 out.directory=ReadCurrentDirectoryView(database,files,root,budget,observations,resource,out,&selected);return out;
}
NativeCurrentCheckpointDirectoryDeviceRead VerifyCurrentNativeCheckpointDirectoryInto(
 const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,const disk::FilespaceRootReference& root,
 u64 budget,std::span<CheckpointBatch* const> observations,std::span<byte> backing) noexcept {
 NativeCurrentCheckpointDirectoryDeviceRead out;
 const auto disjoint=[&](auto value){return disk::detail::DisjointNativeDecodeRegions(backing,value);};
 bool valid=disjoint(std::span{&database,1})&&disjoint(files)&&disjoint(std::span{&root,1})&&disjoint(observations);
 for(const auto& file:files)valid=valid&&(!file.device||disjoint(std::span{file.device,1}));
 for(const auto* batch:observations)valid=valid&&(!batch||disjoint(std::span{batch,1}));
 if(!valid){out.directory.error=NativeCheckpointError::invalid_backing;return out;}
 detail::NativeMetadataMemory resource(backing);
 out=detail::VerifyCurrentNativeCheckpointDirectoryBacked(database,files,root,budget,observations,resource);
 if(out.ok())out.directory.backing_bytes_used=resource.used();return out;
}
NativeCheckpointDirectoryResult VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(
 const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& files,
 const disk::FilespaceRootReference& root,u64 budget) noexcept {
 const auto fail=[](NativeCheckpointError error){NativeCheckpointDirectoryResult out;out.error=error;return out;};
 try{
  if(!native_checkpoint::V7(database)||files.empty()||!budget)return fail(NativeCheckpointError::invalid_reference);
  detail::NativeMetadataHeapMemory heap;std::pmr::monotonic_buffer_resource resource(&heap);
  detail::NativeMetadataScratch scratch{&resource};auto pointers=scratch.Array<CheckpointBatch*>(files.size());
  if(files.size()>std::numeric_limits<std::size_t>::max()/sizeof(CheckpointBatch))throw std::bad_alloc();
  auto* batches=static_cast<CheckpointBatch*>(resource.allocate(files.size()*sizeof(CheckpointBatch),alignof(CheckpointBatch)));
  NativeCurrentCheckpointDirectoryDeviceRead read;
  {struct Cleanup{CheckpointBatch* batches;std::size_t count=0;~Cleanup(){while(count)std::destroy_at(batches+--count);}} cleanup{batches};
   for(std::size_t n=0;n<files.size();++n){if(!files[n].device)return fail(NativeCheckpointError::invalid_filespace);
    pointers[n]=std::construct_at(batches+n,*files[n].device);++cleanup.count;}
   read=detail::VerifyCurrentNativeCheckpointDirectoryBacked(database,files,root,budget,pointers,resource);
  }
  return OwnCurrent(read.directory);
 }catch(const std::bad_alloc&){return fail(NativeCheckpointError::resource_exhausted);}
  catch(const std::length_error&){return fail(NativeCheckpointError::resource_exhausted);}
  catch(...){return fail(NativeCheckpointError::io_failure);}
}

static NativeCheckpointHistoryResult ReadNativeCheckpointHistoryFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& head,
    const scratchbird::storage::disk::FilespaceRootReference& terminal,
    u64 maximum_retained_image_bytes,
    std::optional<NativeCheckpointInventoryResult> admitted_head) noexcept {
  using namespace native_checkpoint;
  const auto fail=[](Error error){NativeCheckpointHistoryResult r;r.error=error;return r;};
  const auto page_ref=[](const disk::FilespaceRootReference& r){return disk::NativePageReference{r.filespace_uuid,r.page_number,r.page_generation,r.page_size_profile_uuid};};
  try {
    if(!V7(database_uuid)||!V7(head.object_uuid)||head.object_uuid!=terminal.object_uuid
      ||head.kind!=9||terminal.kind!=9||head.page_type!=0x300||terminal.page_type!=0x300
      ||!RefValid(page_ref(head))||!RefValid(page_ref(terminal))||!maximum_retained_image_bytes)return fail(Error::invalid_reference);
    if(admitted_head){
      if(!admitted_head->ok()||!admitted_head->checkpoint)return fail(Error::binding_mismatch);
      const auto& cp=*admitted_head->checkpoint;
      const disk::NativePageReference actual{cp.header.filespace_uuid,cp.header.page_number,cp.header.page_generation,cp.header.page_size_profile_uuid};
      if(cp.header.database_uuid!=database_uuid||actual!=page_ref(head)||cp.object_uuid!=head.object_uuid)
        return fail(Error::binding_mismatch);
    }
    auto locked=LockFilespaces(devices);if(locked.error!=Error::none)return fail(locked.error);
    NativeCheckpointHistoryResult result;auto next=head;
    std::set<std::pair<Uuid,u64>> slots;std::set<Uuid> page_ids;
    for(;;) {
      if(result.retained_image_bytes>=maximum_retained_image_bytes)return fail(Error::resource_exhausted);
      if(!slots.emplace(next.filespace_uuid,next.page_number).second)return fail(Error::history_mismatch);
      // Only current-root consumers can supply this already admitted head.
      // Its charge includes selector and control proof work, not just the
      // checkpoint/inventory images retained by the raw-root history reader.
      auto pair=admitted_head?std::move(*admitted_head):VerifyNativeCheckpointInventoryFromOpenDevices(database_uuid,locked.ordered,next,
        maximum_retained_image_bytes-result.retained_image_bytes);
      admitted_head.reset();
      if(!pair.ok()){auto error=fail(pair.error);error.inventory_error=pair.inventory_error;return error;}
      if(pair.retained_image_bytes>maximum_retained_image_bytes-result.retained_image_bytes)return fail(Error::resource_exhausted);
      const auto& older=*pair.checkpoint;
      if(!page_ids.insert(older.header.page_uuid).second)return fail(Error::history_mismatch);
      if(!result.checkpoints.empty()) {
        const auto& newer_pair=result.checkpoints.back();const auto& newer=*newer_pair.checkpoint;
        if(newer.predecessor_sha256!=pair.checkpoint_sha256)return fail(Error::invalid_integrity);
        if(older.object_uuid!=newer.object_uuid||older.timeline_uuid!=newer.timeline_uuid
          ||newer.checkpoint_generation<=1||older.checkpoint_generation!=newer.checkpoint_generation-1
          ||older.root_set_generation>newer.root_set_generation||older.selected_local_transaction_id>newer.selected_local_transaction_id
          ||older.stable_local_transaction_id>newer.stable_local_transaction_id||older.local_durable_transaction_id>newer.local_durable_transaction_id
          ||older.cluster_quorum_transaction_id>newer.cluster_quorum_transaction_id||pair.inventory_generation>newer_pair.inventory_generation
          ||pair.inventory.next_local_transaction_id>newer_pair.inventory.next_local_transaction_id
          ||pair.inventory.next_commit_sequence>newer_pair.inventory.next_commit_sequence)return fail(Error::history_mismatch);
        if((older.root_set_generation==newer.root_set_generation&&older.roots!=newer.roots)
          ||(pair.inventory_generation==newer_pair.inventory_generation&&older.roots.front()!=newer.roots.front()))return fail(Error::history_mismatch);
        if(*scratchbird::transaction::mga::ValidateLocalTransactionInventoryEvolution(pair.inventory,newer_pair.inventory))
          return fail(Error::inventory_mismatch);
      }
      result.retained_image_bytes+=pair.retained_image_bytes;result.checkpoints.push_back(std::move(pair));
      if(page_ref(next)==page_ref(terminal)){result.error=Error::none;return result;}
      const auto& current=*result.checkpoints.back().checkpoint;
      if(!current.predecessor)return fail(Error::history_mismatch);
      const auto& previous=*current.predecessor;
      next={9,0x300,previous.filespace_uuid,previous.page_number,previous.page_generation,previous.page_size_profile_uuid,current.object_uuid};
    }
  }catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}
   catch(const std::length_error&){return fail(Error::resource_exhausted);}
   catch(...){return fail(Error::io_failure);}
}

NativeCheckpointHistoryResult VerifyNativeCheckpointHistoryFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& head,
    const scratchbird::storage::disk::FilespaceRootReference& terminal,
    u64 maximum_retained_image_bytes) noexcept {
  return ReadNativeCheckpointHistoryFromOpenDevices(database_uuid,devices,head,terminal,maximum_retained_image_bytes,{});
}

NativeCheckpointSystemStateResult VerifyCurrentNativeCheckpointSystemStateFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,u64 budget) noexcept {
  using namespace native_checkpoint;namespace mga=scratchbird::transaction::mga;
  const auto fail=[](Error e){NativeCheckpointSystemStateResult r;r.error=e;return r;};
  const auto same=[](const auto& a,const auto& b){return a.kind==b.kind&&a.page_type==b.page_type&&a.object_uuid==b.object_uuid&&
    a.filespace_uuid==b.filespace_uuid&&a.page_size_profile_uuid==b.page_size_profile_uuid&&a.page_number==b.page_number&&a.page_generation==b.page_generation;};
  try{
    if(!V7(database_uuid)||devices.empty()||!budget)return fail(Error::invalid_reference);
    auto locked=LockFilespaces(devices);if(locked.error!=Error::none)return fail(locked.error);
    auto current_inventory=ResolveCurrentCheckpointInventory(database_uuid,locked.ordered,checkpoint,budget);
    auto pair=std::move(current_inventory.pair);
    if(!pair.ok()){auto r=fail(pair.error);r.checkpoints.error=pair.error;r.checkpoints.inventory_error=pair.inventory_error;return r;}
    const auto primary=std::lower_bound(locked.ordered.begin(),locked.ordered.end(),checkpoint.filespace_uuid,[](const auto& f,const auto& id){return f.filespace_uuid<id;});
    if(primary==locked.ordered.end()||primary->filespace_uuid!=checkpoint.filespace_uuid||primary->page_size_profile_uuid!=checkpoint.page_size_profile_uuid)return fail(Error::invalid_filespace);
    const disk::FilespaceBootstrapBinding binding{database_uuid,primary->filespace_uuid,primary->page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*primary->device,&binding);
    if(!zero.ok())return fail(zero.error==disk::FilespacePageZeroError::resource_exhausted?Error::resource_exhausted:
      zero.error==disk::FilespacePageZeroError::hash_provider_failure?Error::hash_failure:zero.error==disk::FilespacePageZeroError::io_failure?Error::io_failure:Error::invalid_filespace);
    const auto& z=*zero.record;const auto current=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==9;});
    if(!current_inventory.selector_bound&&(current==z.roots.end()||!same(*current,checkpoint)||pair.checkpoint->root_set_generation!=z.root_set_generation))return fail(Error::binding_mismatch);
    const auto actual=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==1;});
    const auto target=std::find_if(pair.checkpoint->roots.begin(),pair.checkpoint->roots.end(),[](const auto& r){return r.role==8;});
    if((!current_inventory.selector_bound&&actual==z.roots.end())||target==pair.checkpoint->roots.end())return fail(Error::invalid_roots);
    const disk::FilespaceRootReference expected{1,target->page_type,target->page.filespace_uuid,target->page.page_number,target->page.page_generation,target->page.page_size_profile_uuid,target->object_uuid};
    if(!current_inventory.selector_bound&&!same(*actual,expected))return fail(Error::binding_mismatch);
    const auto file=std::lower_bound(locked.ordered.begin(),locked.ordered.end(),expected.filespace_uuid,[](const auto& f,const auto& id){return f.filespace_uuid<id;});
    if(file==locked.ordered.end()||file->filespace_uuid!=expected.filespace_uuid||file->page_size_profile_uuid!=expected.page_size_profile_uuid)return fail(Error::invalid_filespace);
    const auto* profile=disk::FindCanonicalFilespacePageProfile(expected.page_size_profile_uuid);
    if(!profile||profile->page_size_bytes>budget-pair.retained_image_bytes)return fail(Error::resource_exhausted);
    auto state=ReadNativeSystemStateFromOpenDevice(*file->device,database_uuid,expected);
    if(!state.ok()){auto r=fail(Error::system_state_failure);r.system_error=state.error;return r;}
    const auto digest=hash::ComputeSha256Digest(state.bytes);if(!digest.ok())return fail(Error::hash_failure);
    if(digest.digest!=target->sha256)return fail(Error::invalid_integrity);
    const auto& s=*state.state;const bool cluster=pair.checkpoint->flags&4;
    if(bool(s.flags&NativeSystemFlag::cluster)!=cluster||bool(z.bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required)!=cluster||
      bool(s.flags&NativeSystemFlag::clean)!=bool(pair.checkpoint->flags&1))return fail(Error::binding_mismatch);
    const auto creator_check=[&](const Uuid& id,u64 local,Error mismatch,Error uncommitted){
      const auto entry=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(local));
      if(!entry.ok()||entry.entry.identity.transaction_uuid.value!=id||(!cluster&&entry.entry.identity.scope!=mga::TransactionScope::local_node))return mismatch;
      return mga::HasCommittedInventoryOutcome(entry.entry)?Error::none:uncommitted;};
    const auto creator=creator_check(s.creator_transaction_uuid,s.creator_local_transaction_id,Error::system_state_creator_mismatch,Error::system_state_creator_not_committed);
    if(creator!=Error::none)return fail(creator);
    if(s.clean_local_transaction_id){const auto clean=creator_check(s.clean_transaction_uuid,s.clean_local_transaction_id,Error::system_state_clean_mismatch,Error::system_state_clean_not_committed);if(clean!=Error::none)return fail(clean);}
    NativeCheckpointHistoryResult history;
    if(s.checkpoint_generation){const auto& ref=*s.checkpoint;
      const disk::FilespaceRootReference observed{9,0x300,ref.filespace_uuid,ref.page_number,ref.page_generation,ref.page_size_profile_uuid,s.checkpoint_object_uuid};
      if(same(observed,checkpoint)){if(s.checkpoint_generation!=pair.checkpoint->checkpoint_generation)return fail(Error::system_state_observation_mismatch);}
      else {
        if(s.checkpoint_object_uuid!=pair.checkpoint->object_uuid||s.checkpoint_generation>=pair.checkpoint->checkpoint_generation)return fail(Error::system_state_observation_mismatch);
        history=ReadNativeCheckpointHistoryFromOpenDevices(database_uuid,locked.ordered,checkpoint,observed,budget-state.bytes.size(),std::move(pair));
        if(!history.ok()){auto r=fail(history.error);r.checkpoints.error=history.error;r.checkpoints.inventory_error=history.inventory_error;return r;}
        if(history.checkpoints.back().checkpoint->checkpoint_generation!=s.checkpoint_generation)return fail(Error::system_state_observation_mismatch);
      }
    }
    if(history.checkpoints.empty()){history.error=Error::none;history.retained_image_bytes=pair.retained_image_bytes;history.checkpoints.push_back(std::move(pair));}
    NativeCheckpointSystemStateResult result;result.error=Error::none;result.retained_image_bytes=history.retained_image_bytes+state.bytes.size();
    result.checkpoints=std::move(history);result.system_state=std::move(state);return result;
  }catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}catch(const std::length_error&){return fail(Error::resource_exhausted);}catch(...){return fail(Error::io_failure);}
}

NativeCheckpointHorizonResult VerifyCurrentNativeCheckpointHorizonFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,u64 budget) noexcept {
  using namespace native_checkpoint;namespace mga=scratchbird::transaction::mga;namespace page=scratchbird::storage::page;
  const auto fail=[](Error e){NativeCheckpointHorizonResult r;r.error=e;return r;};
  const auto ref=[](const auto& r){return disk::NativePageReference{r.filespace_uuid,r.page_number,r.page_generation,r.page_size_profile_uuid};};
  try{
    if(!V7(database_uuid)||devices.empty()||!budget)return fail(Error::invalid_reference);
    auto locked=LockFilespaces(devices);if(locked.error!=Error::none)return fail(locked.error);
    auto current_inventory=ResolveCurrentCheckpointInventory(database_uuid,locked.ordered,checkpoint,budget);
    auto pair=std::move(current_inventory.pair);
    if(!pair.ok()){auto r=fail(pair.error);r.checkpoints.error=pair.error;r.checkpoints.inventory_error=pair.inventory_error;return r;}
    const auto primary=std::lower_bound(locked.ordered.begin(),locked.ordered.end(),checkpoint.filespace_uuid,[](const auto& f,const auto& id){return f.filespace_uuid<id;});
    if(primary==locked.ordered.end()||primary->filespace_uuid!=checkpoint.filespace_uuid||primary->page_size_profile_uuid!=checkpoint.page_size_profile_uuid)return fail(Error::invalid_filespace);
    const disk::FilespaceBootstrapBinding binding{database_uuid,primary->filespace_uuid,primary->page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*primary->device,&binding);
    if(!zero.ok())return fail(zero.error==disk::FilespacePageZeroError::resource_exhausted?Error::resource_exhausted:
      zero.error==disk::FilespacePageZeroError::hash_provider_failure?Error::hash_failure:zero.error==disk::FilespacePageZeroError::io_failure?Error::io_failure:Error::invalid_filespace);
    const auto current=std::find_if(zero.record->roots.begin(),zero.record->roots.end(),[](const auto& r){return r.kind==9;});
    if(!current_inventory.selector_bound&&(current==zero.record->roots.end()||current->page_type!=checkpoint.page_type||current->object_uuid!=checkpoint.object_uuid||
        ref(*current)!=ref(checkpoint)||zero.record->root_set_generation!=pair.checkpoint->root_set_generation))return fail(Error::binding_mismatch);
    const auto target=std::find_if(pair.checkpoint->roots.begin(),pair.checkpoint->roots.end(),[](const auto& r){return r.role==2;});
    const auto retention=std::find_if(pair.checkpoint->roots.begin(),pair.checkpoint->roots.end(),[](const auto& r){return r.role==10;});
    if(target==pair.checkpoint->roots.end()||retention==pair.checkpoint->roots.end())return fail(Error::invalid_roots);
    auto horizons=page::ReadNativeHorizonRootFromOpenDevices(database_uuid,locked.ordered,target->object_uuid,target->page,budget-pair.retained_image_bytes);
    if(!horizons.ok()){auto r=fail(Error::horizon_failure);r.horizon_error=horizons.error;return r;}
    const auto digest=hash::ComputeSha256Digest(horizons.pages.front().bytes);if(!digest.ok())return fail(Error::hash_failure);
    if(digest.digest!=target->sha256)return fail(Error::invalid_integrity);
    const auto& h=*horizons.pages.front().root;const bool cluster=pair.checkpoint->flags&4;
    if(bool(h.flags&1)!=cluster||bool(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required)!=cluster)return fail(Error::binding_mismatch);
    const auto creator=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(h.creator_local_transaction_id));
    if(!creator.ok()||creator.entry.identity.transaction_uuid.value!=h.creator_transaction_uuid||(!cluster&&creator.entry.identity.scope!=mga::TransactionScope::local_node))return fail(Error::horizon_creator_mismatch);
    if(!mga::HasCommittedInventoryOutcome(creator.entry))return fail(Error::horizon_creator_not_committed);
    if(h.retention!=retention->page||h.retention_object_uuid!=retention->object_uuid||h.retention_sha256!=retention->sha256)return fail(Error::horizon_retention_mismatch);
    std::optional<disk::FilespaceRootReference> oldest;u64 oldest_generation=pair.checkpoint->checkpoint_generation;
    for(const auto& image:horizons.pages)for(const auto& record:image.root->records){
      if(record.local_boundary>pair.inventory.next_local_transaction_id)return fail(Error::horizon_boundary_mismatch);
      if(!record.checkpoint)continue;
      if(record.checkpoint_object_uuid!=pair.checkpoint->object_uuid||record.checkpoint_generation>pair.checkpoint->checkpoint_generation)return fail(Error::horizon_observation_mismatch);
      if(record.checkpoint_generation<oldest_generation){const auto& r=*record.checkpoint;oldest_generation=record.checkpoint_generation;oldest=disk::FilespaceRootReference{9,0x300,r.filespace_uuid,r.page_number,r.page_generation,r.page_size_profile_uuid,record.checkpoint_object_uuid};}
    }
    NativeCheckpointHistoryResult history;
    if(oldest){history=ReadNativeCheckpointHistoryFromOpenDevices(database_uuid,locked.ordered,checkpoint,*oldest,budget-horizons.retained_image_bytes,std::move(pair));
      if(!history.ok()){auto r=fail(history.error);r.checkpoints.error=history.error;r.checkpoints.inventory_error=history.inventory_error;return r;}}
    else{history.error=Error::none;history.retained_image_bytes=pair.retained_image_bytes;history.checkpoints.push_back(std::move(pair));}
    std::map<u64,const NativeCheckpointRoot*> observed;std::set<Uuid> checkpoint_ids;
    for(const auto& value:history.checkpoints){observed.emplace(value.checkpoint->checkpoint_generation,&*value.checkpoint);checkpoint_ids.insert(value.checkpoint->header.page_uuid);}
    for(const auto& image:horizons.pages){if(checkpoint_ids.contains(image.root->header.page_uuid))return fail(Error::binding_mismatch);
      for(const auto& record:image.root->records)if(record.checkpoint){const auto at=observed.find(record.checkpoint_generation);
        if(at==observed.end()||at->second->object_uuid!=record.checkpoint_object_uuid||ref(at->second->header)!=*record.checkpoint)return fail(Error::horizon_observation_mismatch);}}
    const u64 used=history.retained_image_bytes+horizons.retained_image_bytes;
    auto pins=page::ReadNativeRetentionRootFromOpenDevices(database_uuid,locked.ordered,h.retention_object_uuid,h.retention,budget-used);
    if(!pins.ok()){auto r=fail(Error::retention_failure);r.retention_error=pins.error;return r;}
    const auto pin_digest=hash::ComputeSha256Digest(pins.images.front().bytes);if(!pin_digest.ok())return fail(Error::hash_failure);
    if(pin_digest.digest!=h.retention_sha256)return fail(Error::invalid_integrity);
    const auto& root=*pins.images.front().page;const auto& inventory=history.checkpoints.front().inventory;
    if(bool(root.flags&1)!=cluster)return fail(Error::binding_mismatch);
    const auto pin_creator=mga::LookupLocalTransaction(inventory,mga::MakeLocalTransactionId(root.creator_local_transaction_id));
    if(!pin_creator.ok()||pin_creator.entry.identity.transaction_uuid.value!=root.creator_transaction_uuid||
        (!cluster&&pin_creator.entry.identity.scope!=mga::TransactionScope::local_node))return fail(Error::retention_creator_mismatch);
    if(!mga::HasCommittedInventoryOutcome(pin_creator.entry))return fail(Error::retention_creator_not_committed);
    std::set<Uuid> page_ids=checkpoint_ids;for(const auto& image:horizons.pages)page_ids.insert(image.root->header.page_uuid);
    std::map<Uuid,const page::NativeRetentionPin*> pin_index;
    for(const auto& image:pins.images){if(!page_ids.insert(image.page->header.page_uuid).second)return fail(Error::binding_mismatch);
      for(const auto& pin:image.page->records)pin_index.emplace(pin.pin_uuid,&pin);}
    for(const auto& image:horizons.pages)for(const auto& record:image.root->records){
      if(record.pin_uuid.is_nil())continue;
      const auto pin=pin_index.find(record.pin_uuid);if(pin==pin_index.end())return fail(Error::horizon_pin_missing);
      if(pin->second->timeline_uuid!=record.timeline_uuid)return fail(Error::horizon_pin_lineage_mismatch);
    }
    NativeCheckpointHorizonResult result;result.error=Error::none;result.retained_image_bytes=used+pins.retained_image_bytes;
    result.checkpoints=std::move(history);result.horizons=std::move(horizons);result.retention=std::move(pins);return result;
  }catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}catch(const std::length_error&){return fail(Error::resource_exhausted);}catch(...){return fail(Error::io_failure);}
}

}  // namespace scratchbird::storage::database
