// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_control_bundle.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "uuid.hpp"
#include "transaction_inventory_page.hpp"
#include "transaction_inventory_validation.hpp"
#include "native_filespace_directory.hpp"
#include "native_decoded_storage_ranges.hpp"
#include "native_metadata_decode_scratch.hpp"
#include "native_management_control_bundle_backing.hpp"
#include <memory_resource>
#include <memory>
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

namespace scratchbird::storage::database {
namespace {
using namespace core::platform;
using E=NativeManagementControlBundleError;
using Root=NativeManagementControlBundleRoot;
using Bytes=std::vector<byte>;
using Pages=std::vector<Bytes>;
using Header=disk::NativeCommonPageHeader;
const char* Magic(const Root& r){return r.growth_image_count?"SBMCB004":r.directory_count?"SBMCB003":r.inventory_count?"SBMCB002":"SBMCB001";}
u16 Version(const Root& r){return r.growth_image_count?4:r.directory_count?3:r.inventory_count?2:1;}
void Require(bool ok,E error){if(!ok)throw error;}
bool V7(const Uuid& id){return core::uuid::IsEngineIdentityUuid(id);}
bool Zero(const byte* b,std::size_t n){return std::all_of(b,b+n,[](byte v){return !v;});}
void Put(byte* b,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b);}
Uuid Get(const byte* b){Uuid id;std::copy_n(b,16,id.bytes.begin());return id;}
template<class T>T Fail(E e){T r;r.error=e;return r;}
auto Hash(std::span<const byte> b){const auto h=core::hash::ComputeSha256DigestNative(b.data(),b.size());Require(h.ok(),E::hash_failure);return h.digest;}
auto Seal(std::span<const byte> b){const std::array<byte,32> zero{};const core::hash::HashDigestSegment parts[]={{b.data(),320},{zero.data(),32},{b.data()+352,b.size()-352}};
  const auto h=core::hash::ComputeSha256DigestPartsNative(parts,3);Require(h.ok(),E::hash_failure);return h.digest;}
auto Self(const Header& h){return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}

using Image=std::span<const byte>;
using Images=std::span<Image>;
using BundleMemory=detail::NativeMetadataMemory;
using BundleHeapMemory=detail::NativeMetadataHeapMemory;
using Scratch=detail::NativeMetadataScratch;
struct InventoryScratch {
  u64 next_local_transaction_id=1,next_commit_sequence=1;
  std::pmr::vector<transaction::mga::TransactionInventoryEntry> entries;
  explicit InventoryScratch(Scratch& scratch):entries(scratch.resource){}
  bool Valid(Scratch& scratch) {
    const page::NativeTransactionInventoryView view{next_local_transaction_id,next_commit_sequence,entries};
    return page::ValidateNativeTransactionInventoryView(view,scratch.Array<std::size_t>(entries.size()),
      scratch.Array<byte>(entries.size())).ok();
  }
};

E CheckPayloadBytes(const Root& r,u64 size,u64& bytes){
  const auto limit=std::numeric_limits<u64>::max();
  if(!r.map_count||r.inventory_count>limit-r.map_count)return E::invalid_extent;
  const auto count=r.map_count+r.inventory_count;
  if(r.directory_count>limit-count)return E::invalid_extent;
  if(r.growth_image_count&&(!r.directory_count||r.growth_image_count!=2))return E::invalid_extent;
  if(!r.directory_count){
    if(r.payload_bytes||count>limit/size)return E::invalid_extent;
    bytes=count*size;return E::none;
  }
  u64 minimum=limit,maximum=0;
  for(const auto& p:disk::kCanonicalFilespacePageProfiles){
    minimum=std::min<u64>(minimum,p.page_size_bytes);maximum=std::max<u64>(maximum,p.page_size_bytes);
  }
  if(r.growth_image_count>limit-count-r.directory_count)return E::invalid_extent;
  const auto frames=count+r.directory_count+r.growth_image_count;
  bytes=r.payload_bytes;
  if(!bytes||frames>bytes/(minimum+8)||bytes/(maximum+8)+(bytes%(maximum+8)!=0)>frames)
    return E::invalid_extent;
  return E::none;
}
u64 PayloadBytes(const Root& r,u64 size){
  u64 bytes=0;const auto error=CheckPayloadBytes(r,size,bytes);Require(error==E::none,error);return bytes;
}
E CheckShape(const Root& r,const Uuid& database,const Uuid& bootstrap,u64 budget,u32& page_size,bool digests=true){
  const auto* profile=disk::FindCanonicalFilespacePageProfile(r.first.page_size_profile_uuid);
  if(!profile||!V7(database)||!V7(bootstrap)||!V7(r.object_uuid)||!V7(r.operation_uuid)||
     !V7(r.first.filespace_uuid))return E::invalid_identity;
  const std::array<Uuid,4> ids{database,bootstrap,r.object_uuid,r.operation_uuid};
  for(std::size_t i=0;i<ids.size();++i)for(std::size_t j=0;j<i;++j)
    if(ids[i]==ids[j])return E::invalid_identity;
  const u64 size=profile->page_size_bytes,capacity=size-384,limit=std::numeric_limits<u64>::max();
  if(!r.first.page_number||!r.first.page_generation)return E::invalid_extent;
  u64 bytes=0;const auto payload=CheckPayloadBytes(r,size,bytes);
  if(payload!=E::none)return payload;
  if(r.page_count!=bytes/capacity+(bytes%capacity!=0))return E::invalid_extent;
  if(r.first.page_number>limit/size||r.page_count-1>limit/size-r.first.page_number)return E::invalid_extent;
  const u64 offset=(r.first.page_number+r.page_count-1)*size;
  const u64 max_offset=static_cast<u64>(std::numeric_limits<std::streamoff>::max());
  const u64 max_bytes=static_cast<u64>(std::numeric_limits<std::streamsize>::max());
  if(size>max_bytes||offset>max_offset||size>max_offset-offset)return E::invalid_extent;
  if(digests&&(Zero(r.aggregate_sha256.data(),32)||Zero(r.first_page_sha256.data(),32)))return E::invalid_integrity;
  if(bytes>std::numeric_limits<std::size_t>::max()||budget<2*size)return E::resource_exhausted;
  u64 remaining=budget-2*size;
  if(r.page_count>remaining/size)return E::resource_exhausted;
  remaining-=r.page_count*size;
  if(bytes>remaining/4)return E::resource_exhausted;
  page_size=static_cast<u32>(size);return E::none;
}
u32 Shape(const Root& r,const Uuid& database,const Uuid& bootstrap,u64 budget,bool digests=true){
  u32 size=0;const auto error=CheckShape(r,database,bootstrap,budget,size,digests);
  Require(error==E::none,error);return size;
}
void Common(const Header& h,const Root& r,const Uuid& db,const Uuid& bootstrap,u32 size,u64 i,std::pmr::set<Uuid>& ids){
  Require(h.database_uuid==db&&h.filespace_uuid==r.first.filespace_uuid&&h.page_size_profile_uuid==r.first.page_size_profile_uuid&&h.page_size_bytes==size&&h.page_number==r.first.page_number+i&&h.page_generation==r.first.page_generation&&h.page_type==0x500&&!h.flags,E::binding_mismatch);
  Require(V7(h.page_uuid)&&h.page_uuid!=db&&h.page_uuid!=bootstrap&&h.page_uuid!=r.object_uuid&&h.page_uuid!=r.operation_uuid&&ids.insert(h.page_uuid).second,E::invalid_identity);
}
template<class ImageRange,class Headers>
u64 Maps(const ImageRange& images,const ImageRange& inventory_images,const Headers& headers,const Root& root,const Uuid& db,u32 size,Scratch& scratch){
  Require(images.size()==root.map_count&&inventory_images.size()==root.inventory_count&&headers.size()==root.page_count,E::invalid_allocation);
  std::pmr::vector<page::NativeAllocationMapView> maps(scratch.resource);maps.reserve(images.size());std::pmr::set<u64> slots(scratch.resource);std::pmr::set<Uuid> page_ids(scratch.resource),allocation_ids(scratch.resource),record_ids(scratch.resource);
  for(const auto& h:headers){Require(slots.insert(h.page_number).second&&page_ids.insert(h.page_uuid).second,E::invalid_identity);}
  u64 covered=0;
  for(const auto& image:images){Require(image.size()==size,E::invalid_allocation);auto decoded=scratch.Map(image);
    if(!decoded.ok())throw decoded.error==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:decoded.error==page::NativeAllocationError::hash_failure?E::hash_failure:E::invalid_allocation;
    auto m=std::move(*decoded.map);const auto& h=m.header;Require(!h.flags,E::cluster_requires_authority);
    Require(h.database_uuid==db&&h.filespace_uuid==root.first.filespace_uuid&&h.page_size_profile_uuid==root.first.page_size_profile_uuid&&h.page_generation==root.first.page_generation&&m.map_generation==root.first.page_generation&&m.creator_transaction_uuid.is_nil()&&!m.creator_local_transaction_id&&m.creator_operation_uuid==root.operation_uuid&&m.first_page==covered,E::binding_mismatch);
    Require(slots.insert(h.page_number).second&&page_ids.insert(h.page_uuid).second,E::invalid_identity);
    if(!maps.empty()){const auto& first=maps.front();const auto& last=maps.back();Require(m.object_uuid==first.object_uuid&&m.total_pages==first.total_pages&&m.capacity_generation==first.capacity_generation&&last.next&&*last.next==Self(h)&&last.next_sha256==Hash(image),E::binding_mismatch);}
    for(const auto& record:m.records)Require(allocation_ids.insert(record.allocation_uuid).second&&(record.page_uuid.is_nil()||record_ids.insert(record.page_uuid).second),E::invalid_identity);
    covered+=m.states.size();maps.push_back(std::move(m));
  }
  Require(covered==maps.front().total_pages&&!maps.back().next,E::binding_mismatch);
  const auto allocated=[&](const Header& h,const Uuid& owner){auto map=std::upper_bound(maps.begin(),maps.end(),h.page_number,[](u64 n,const auto& m){return n<m.first_page;});Require(map!=maps.begin(),E::binding_mismatch);--map;
    Require(h.page_number-map->first_page<map->states.size()&&map->states[h.page_number-map->first_page]==page::NativeAllocationState::allocated,E::binding_mismatch);
    const auto r=std::lower_bound(map->records.begin(),map->records.end(),h.page_number,[](const auto& r,u64 n){return r.page_number<n;});
    Require(r!=map->records.end()&&r->page_number==h.page_number&&r->page_uuid==h.page_uuid&&r->page_generation==h.page_generation&&r->page_type==h.page_type&&r->owner_uuid==owner&&r->creator_transaction_uuid.is_nil()&&!r->creator_local_transaction_id&&r->creator_operation_uuid==root.operation_uuid&&!r->reuse_horizon,E::binding_mismatch);};
  for(const auto& m:maps)allocated(m.header,m.object_uuid);
  for(const auto& h:headers)allocated(h,root.object_uuid);
  InventoryScratch inventory(scratch);
  std::optional<disk::NativePageReference> previous,next;
  Uuid object;
  for(std::size_t i=0;i<inventory_images.size();++i){const auto& raw=inventory_images[i];Require(raw.size()==size,E::invalid_allocation);
    const auto common=disk::DecodeNativeCommonPageHeader(raw.data(),128);Require(common.ok(),E::invalid_header);
    if(common.header->flags&1u)throw E::encrypted_requires_authority;
    auto decoded=scratch.Inventory(raw);
    if(!decoded.ok())throw decoded.error==page::NativeInventoryError::resource_exhausted?E::resource_exhausted:decoded.error==page::NativeInventoryError::hash_failure?E::hash_failure:E::invalid_allocation;
    const auto& p=*decoded.page;const auto& h=p.header;
    Require(!h.flags&&h.database_uuid==db&&h.filespace_uuid==root.first.filespace_uuid&&h.page_size_profile_uuid==root.first.page_size_profile_uuid&&h.page_generation==root.first.page_generation&&p.inventory_generation==root.first.page_generation,E::binding_mismatch);
    Require(slots.insert(h.page_number).second&&page_ids.insert(h.page_uuid).second,E::invalid_identity);
    Require(p.previous==previous&&(!i||next==Self(h)),E::binding_mismatch);
    if(!i){object=p.object_uuid;inventory.next_local_transaction_id=p.inventory.next_local_transaction_id;inventory.next_commit_sequence=p.inventory.next_commit_sequence;}
    else Require(p.object_uuid==object&&p.inventory.next_local_transaction_id==inventory.next_local_transaction_id&&p.inventory.next_commit_sequence==inventory.next_commit_sequence,E::binding_mismatch);
    if(!inventory.entries.empty()&&!p.inventory.entries.empty())Require(inventory.entries.back().identity.local_id.value<p.inventory.entries.front().identity.local_id.value,E::binding_mismatch);
    inventory.entries.insert(inventory.entries.end(),p.inventory.entries.begin(),p.inventory.entries.end());
    previous=Self(h);next=p.next;allocated(h,p.object_uuid);
  }
  Require(!next,E::binding_mismatch);
  if(!inventory_images.empty())Require(inventory.Valid(scratch),E::invalid_allocation);
  return maps.front().total_pages;
}
Header MixedHeader(const byte* bytes,std::size_t length,const Root& root,const Uuid& db){
  Require(length>=128,E::invalid_header);const auto decoded=disk::DecodeNativeCommonPageHeader(bytes,128);Require(decoded.ok(),E::invalid_header);
  const auto h=*decoded.header;const auto* profile=disk::FindCanonicalFilespacePageProfile(h.page_size_profile_uuid);
  Require(profile&&length==profile->page_size_bytes&&h.page_size_bytes==length&&h.database_uuid==db&&h.page_generation==root.first.page_generation,E::binding_mismatch);
  if(h.flags&1u)throw E::encrypted_requires_authority;if(h.flags&2u)throw E::cluster_requires_authority;Require(!h.flags,E::invalid_header);return h;
}
template<class ImageRange,class Headers>
u64 MixedMaps(const ImageRange& images,const ImageRange& inventory_images,const ImageRange& directory_images,const Headers& headers,const Root& root,const Uuid& db,Scratch& scratch){
  Require(images.size()==root.map_count&&inventory_images.size()==root.inventory_count&&directory_images.size()==root.directory_count&&headers.size()==root.page_count,E::invalid_allocation);
  using Maps=std::pmr::vector<page::NativeAllocationMapView>;
  std::pmr::map<Uuid,Maps> groups(scratch.resource);std::pmr::map<Uuid,std::array<byte,32>> head_hashes(scratch.resource);
  std::pmr::set<std::pair<Uuid,u64>> slots(scratch.resource);std::pmr::set<Uuid> page_ids(scratch.resource),allocation_ids(scratch.resource),record_ids(scratch.resource);
  const auto unique=[&](const Header& h){Require(slots.emplace(h.filespace_uuid,h.page_number).second&&page_ids.insert(h.page_uuid).second,E::invalid_identity);};
  for(const auto& h:headers)unique(h);
  Uuid previous_file;
  for(const auto& image:images){const auto h=MixedHeader(image.data(),image.size(),root,db);
    auto decoded=scratch.Map(image);
    if(!decoded.ok())throw decoded.error==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:decoded.error==page::NativeAllocationError::hash_failure?E::hash_failure:E::invalid_allocation;
    auto m=std::move(*decoded.map);Require(previous_file.is_nil()||!(h.filespace_uuid<previous_file),E::binding_mismatch);previous_file=h.filespace_uuid;
    unique(h);auto& group=groups[h.filespace_uuid];
    Require(m.map_generation==root.first.page_generation&&m.creator_transaction_uuid.is_nil()&&!m.creator_local_transaction_id&&m.creator_operation_uuid==root.operation_uuid,E::binding_mismatch);
    if(group.empty()){Require(!m.first_page,E::binding_mismatch);head_hashes.emplace(h.filespace_uuid,Hash(image));}
    else{const auto& first=group.front();const auto& last=group.back();
      Require(h.page_size_profile_uuid==first.header.page_size_profile_uuid&&m.object_uuid==first.object_uuid&&m.total_pages==first.total_pages&&m.capacity_generation==first.capacity_generation&&
        m.first_page==last.first_page+last.states.size()&&last.next&&*last.next==Self(h)&&last.next_sha256==Hash(image),E::binding_mismatch);}
    for(const auto& record:m.records)Require(allocation_ids.insert(record.allocation_uuid).second&&(record.page_uuid.is_nil()||record_ids.insert(record.page_uuid).second),E::invalid_identity);
    group.push_back(std::move(m));
  }
  Require(groups.contains(root.first.filespace_uuid),E::binding_mismatch);
  for(const auto& [id,group]:groups){const auto& last=group.back();Require(!last.next&&last.first_page+last.states.size()==last.total_pages,E::binding_mismatch);}
  const auto allocated=[&](const Header& h,const Uuid& owner){const auto found=groups.find(h.filespace_uuid);Require(found!=groups.end(),E::binding_mismatch);const auto& maps=found->second;
    Require(h.page_size_profile_uuid==maps.front().header.page_size_profile_uuid,E::binding_mismatch);
    auto map=std::upper_bound(maps.begin(),maps.end(),h.page_number,[](u64 n,const auto& m){return n<m.first_page;});Require(map!=maps.begin(),E::binding_mismatch);--map;
    Require(h.page_number-map->first_page<map->states.size()&&map->states[h.page_number-map->first_page]==page::NativeAllocationState::allocated,E::binding_mismatch);
    const auto r=std::lower_bound(map->records.begin(),map->records.end(),h.page_number,[](const auto& r,u64 n){return r.page_number<n;});
    Require(r!=map->records.end()&&r->page_number==h.page_number&&r->page_uuid==h.page_uuid&&r->page_generation==h.page_generation&&r->page_type==h.page_type&&r->owner_uuid==owner&&
      r->creator_transaction_uuid.is_nil()&&!r->creator_local_transaction_id&&r->creator_operation_uuid==root.operation_uuid&&!r->reuse_horizon,E::binding_mismatch);};
  for(const auto& [id,group]:groups)for(const auto& m:group)allocated(m.header,m.object_uuid);
  for(const auto& h:headers)allocated(h,root.object_uuid);
  InventoryScratch inventory(scratch);std::optional<disk::NativePageReference> previous,next;Uuid object;
  for(std::size_t i=0;i<inventory_images.size();++i){const auto& raw=inventory_images[i];const auto h=MixedHeader(raw.data(),raw.size(),root,db);unique(h);
    const auto decoded=scratch.Inventory(raw);
    if(!decoded.ok())throw decoded.error==page::NativeInventoryError::resource_exhausted?E::resource_exhausted:decoded.error==page::NativeInventoryError::hash_failure?E::hash_failure:E::invalid_allocation;
    const auto& p=*decoded.page;Require(p.inventory_generation==root.first.page_generation&&p.previous==previous&&(!i||next==Self(h)),E::binding_mismatch);
    if(!i){object=p.object_uuid;inventory.next_local_transaction_id=p.inventory.next_local_transaction_id;inventory.next_commit_sequence=p.inventory.next_commit_sequence;}
    else Require(p.object_uuid==object&&p.inventory.next_local_transaction_id==inventory.next_local_transaction_id&&p.inventory.next_commit_sequence==inventory.next_commit_sequence,E::binding_mismatch);
    if(!inventory.entries.empty()&&!p.inventory.entries.empty())Require(inventory.entries.back().identity.local_id.value<p.inventory.entries.front().identity.local_id.value,E::binding_mismatch);
    inventory.entries.insert(inventory.entries.end(),p.inventory.entries.begin(),p.inventory.entries.end());previous=Self(h);next=p.next;allocated(h,p.object_uuid);
  }
  Require(!next,E::binding_mismatch);if(!inventory_images.empty())Require(inventory.Valid(scratch),E::invalid_allocation);
  u64 ordinal=0,total=0;Uuid directory_object,last_record;std::array<byte,32> next_hash{};std::pmr::set<Uuid> zero_ids(scratch.resource),bound_groups(scratch.resource);
  for(std::size_t i=0;i<directory_images.size();++i){const auto& raw=directory_images[i];const auto h=MixedHeader(raw.data(),raw.size(),root,db);unique(h);
    const auto decoded=scratch.Directory(raw);
    if(!decoded.ok())throw decoded.error==page::NativeDirectoryError::resource_exhausted?E::resource_exhausted:decoded.error==page::NativeDirectoryError::hash_failure?E::hash_failure:E::invalid_allocation;
    const auto& p=*decoded.directory;Require(h.filespace_uuid==root.first.filespace_uuid&&h.page_size_profile_uuid==root.first.page_size_profile_uuid&&
      p.directory_generation==root.first.page_generation&&p.creator_operation_uuid==root.operation_uuid&&p.creator_transaction_uuid.is_nil()&&!p.creator_local_transaction_id&&p.first_record==ordinal,E::binding_mismatch);
    if(!i){directory_object=p.object_uuid;total=p.total_records;}
    else Require(p.object_uuid==directory_object&&p.total_records==total&&next==Self(h)&&next_hash==Hash(raw),E::binding_mismatch);
    for(const auto& member:p.records){Require((last_record.is_nil()||last_record<member.bootstrap.filespace_uuid)&&zero_ids.insert(member.page_zero_uuid).second,E::binding_mismatch);last_record=member.bootstrap.filespace_uuid;
      const auto found=groups.find(member.bootstrap.filespace_uuid);if(found==groups.end())continue;const auto& map=found->second.front();Require(member.allocation_root.has_value(),E::binding_mismatch);const auto& r=*member.allocation_root;
      Require(member.bootstrap.page_size_profile_uuid==map.header.page_size_profile_uuid&&member.bootstrap.page_size_bytes==map.header.page_size_bytes&&member.total_pages==map.total_pages&&
        r.page==Self(map.header)&&r.object_uuid==map.object_uuid&&r.sha256==head_hashes.at(member.bootstrap.filespace_uuid)&&r.map_generation==map.map_generation&&r.capacity_generation==map.capacity_generation&&bound_groups.insert(member.bootstrap.filespace_uuid).second,E::binding_mismatch);}
    ordinal+=p.records.size();next=p.next;next_hash=p.next_sha256;allocated(h,p.object_uuid);
  }
  Require(ordinal==total&&!next&&bound_groups.size()==groups.size(),E::binding_mismatch);for(const auto& id:zero_ids)Require(!page_ids.contains(id),E::invalid_identity);
  return groups.at(root.first.filespace_uuid).front().total_pages;
}
template<class ImageRange>
void Growth(const ImageRange& images,const ImageRange& maps,const ImageRange& directory,const Root& root,const Uuid& db,const Uuid& bootstrap,Scratch& scratch){
  Require(images.size()==root.growth_image_count,E::invalid_extent);
  if(images.empty())return;
  const auto image_error=[](disk::FilespacePageZeroError e){
    if(e==disk::FilespacePageZeroError::none)return;
    throw e==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:
          e==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:E::invalid_allocation;
  };
  const auto a=scratch.PageZero(images[0]);image_error(a.error);
  const auto b=scratch.PageZero(images[1]);image_error(b.error);
  const auto& before=*a.record;const auto& after=*b.record;
  Require(before.bootstrap.database_uuid==db&&after.bootstrap.database_uuid==db&&images[0].size()==images[1].size(),E::binding_mismatch);
  if((before.bootstrap.flags|after.bootstrap.flags)&disk::FilespaceBootstrapFlag::payload_encrypted)throw E::encrypted_requires_authority;
  if((before.bootstrap.flags|after.bootstrap.flags)&disk::FilespaceBootstrapFlag::cluster_authority_required)throw E::cluster_requires_authority;
  Require(before.page_generation!=std::numeric_limits<u64>::max()&&before.root_set_generation!=std::numeric_limits<u64>::max()&&
    after.page_generation==before.page_generation+1&&after.root_set_generation==before.root_set_generation+1&&after.total_pages>before.total_pages,E::binding_mismatch);
  // Both images have already passed complete canonical validation. Only these
  // five values, the repeated common generation and their dependent checksums
  // may differ. Compare every other byte, including all reserved bytes/roots.
  const std::array<std::pair<std::size_t,std::size_t>,5> changed{{
    {4176,4184},{4192,4200},{4248,4256},{4368,4400},{4448,4480}}};
  std::size_t compared=0;
  for(const auto& [first,last]:changed){
    Require(std::equal(images[0].begin()+compared,images[0].begin()+first,images[1].begin()+compared),E::binding_mismatch);
    compared=last;
  }
  Require(std::equal(images[0].begin()+compared,images[0].end(),images[1].begin()+compared),E::binding_mismatch);
  const auto& fs=after.bootstrap.filespace_uuid;
  Require(fs!=root.first.filespace_uuid||after.page_uuid==bootstrap,E::binding_mismatch);
  bool found_zero=false;u64 free=0,preallocated=0;
  for(const auto& raw:maps){auto m=scratch.Map(raw);
    if(!m.ok())throw m.error==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:m.error==page::NativeAllocationError::hash_failure?E::hash_failure:E::invalid_allocation;
    if(m.map->header.filespace_uuid!=fs)continue;const auto& map=*m.map;
    Require(map.total_pages==after.total_pages&&map.header.page_size_profile_uuid==after.bootstrap.page_size_profile_uuid,E::binding_mismatch);
    for(auto state:map.states){if(state==page::NativeAllocationState::free||state==page::NativeAllocationState::reusable_free)++free;
      if(state==page::NativeAllocationState::preallocated)++preallocated;}
    if(map.first_page)continue;
    Require(!found_zero&&!map.states.empty()&&map.states.front()==page::NativeAllocationState::allocated&&!map.records.empty(),E::binding_mismatch);
    const auto& r=map.records.front();Require(!r.page_number&&r.page_uuid==after.page_uuid&&r.page_generation==after.page_generation&&
      r.page_type==(after.bootstrap.filespace_role<=4?1u:2u)&&r.owner_uuid==fs,E::binding_mismatch);found_zero=true;
  }
  Require(found_zero&&free==after.free_pages&&preallocated==after.preallocated_pages,E::binding_mismatch);
  bool found_member=false;
  for(const auto& raw:directory){const auto decoded=scratch.Directory(raw);
    if(!decoded.ok())throw decoded.error==page::NativeDirectoryError::resource_exhausted?E::resource_exhausted:decoded.error==page::NativeDirectoryError::hash_failure?E::hash_failure:E::invalid_allocation;
    for(const auto& member:decoded.directory->records){if(member.bootstrap.filespace_uuid!=fs)continue;
      const auto& x=member.bootstrap;const auto& y=after.bootstrap;
      Require(!found_member&&x.database_uuid==y.database_uuid&&x.page_size_profile_uuid==y.page_size_profile_uuid&&x.checksum_profile_uuid==y.checksum_profile_uuid&&
        x.encryption_profile_uuid==y.encryption_profile_uuid&&x.page_size_bytes==y.page_size_bytes&&x.durable_format_generation==y.durable_format_generation&&
        x.flags==y.flags&&x.filespace_role==y.filespace_role&&x.lifecycle_state==y.lifecycle_state&&member.page_zero_uuid==after.page_uuid&&
        member.page_zero_generation==after.page_generation&&member.root_set_generation==after.root_set_generation&&member.total_pages==after.total_pages,E::binding_mismatch);
      found_member=true;
    }
  }
  Require(found_member,E::binding_mismatch);
}
template<class PageRange>
NativeManagementControlBundleViewRead DecodeView(const PageRange& pages,const Root& r,const Uuid& db,const Uuid& bootstrap,u64 budget,Scratch& scratch){
  const auto size=Shape(r,db,bootstrap,budget),capacity=size-384;Require(pages.size()==r.page_count,E::invalid_extent);const u64 total=PayloadBytes(r,size);
  auto payload=scratch.Array<byte>(static_cast<std::size_t>(total));auto headers=scratch.Array<Header>(pages.size());std::pmr::set<Uuid> ids(scratch.resource);auto next=r.first_page_sha256;
  for(std::size_t i=0;i<pages.size();++i){const auto& b=pages[i];Require(b.size()==size,E::invalid_header);const auto h=disk::DecodeNativeCommonPageHeader(b.data(),128);Require(h.ok(),E::invalid_header);Common(*h.header,r,db,bootstrap,size,i,ids);headers[i]=*h.header;
    Require(Hash(b)==next,E::invalid_integrity);const auto seal=Seal(b);Require(std::equal(seal.begin(),seal.end(),b.begin()+320),E::invalid_integrity);
    const u64 offset=u64{i}*capacity;const u32 length=static_cast<u32>(std::min<u64>(capacity,total-offset));const auto* f=b.data()+128;
    Require(std::string_view(reinterpret_cast<const char*>(f),8)==Magic(r)&&LoadLittle16(f+8)==Version(r)&&LoadLittle16(f+10)==256&&LoadLittle32(f+12)==384+length&&LoadLittle64(f+64)==r.map_count&&LoadLittle64(f+72)==total&&LoadLittle64(f+80)==offset&&LoadLittle64(f+88)==i&&LoadLittle64(f+96)==r.page_count&&LoadLittle64(f+104)==r.first.page_number&&LoadLittle32(f+144)==length&&LoadLittle64(f+148)==r.inventory_count&&LoadLittle32(f+156)==(r.directory_count?8u:0u)&&LoadLittle64(f+224)==r.directory_count&&LoadLittle64(f+232)==r.growth_image_count&&Zero(f+240,16)&&Zero(b.data()+384+length,b.size()-384-length),E::invalid_header);
    Require(Get(f+16)==r.object_uuid&&Get(f+32)==bootstrap&&Get(f+48)==r.operation_uuid&&std::equal(r.aggregate_sha256.begin(),r.aggregate_sha256.end(),f+112),E::binding_mismatch);
    std::copy_n(f+160,32,next.begin());Require((i+1==pages.size())==Zero(next.data(),32),E::invalid_integrity);std::copy_n(b.begin()+384,length,payload.begin()+offset);
  }
  Require(Hash(payload)==r.aggregate_sha256,E::invalid_integrity);auto images=scratch.Array<Image>(r.map_count),inventory=scratch.Array<Image>(r.inventory_count),directory=scratch.Array<Image>(r.directory_count),growth=scratch.Array<Image>(r.growth_image_count);
  if(r.directory_count){std::size_t at=0;
    const auto frames=[&](u64 count,Images out,bool zero=false){for(u64 i=0;i<count;++i){Require(payload.size()-at>=8,E::invalid_extent);const auto bytes=LoadLittle64(payload.data()+at);at+=8;
      Require(bytes<=payload.size()-at,E::invalid_extent);
      if(zero)Require(std::any_of(disk::kCanonicalFilespacePageProfiles.begin(),disk::kCanonicalFilespacePageProfiles.end(),[&](const auto& p){return p.page_size_bytes==bytes;}),E::invalid_extent);
      else MixedHeader(payload.data()+at,static_cast<std::size_t>(bytes),r,db);
      out[i]=payload.subspan(at,static_cast<std::size_t>(bytes));at+=static_cast<std::size_t>(bytes);}};
    frames(r.map_count,images);frames(r.inventory_count,inventory);frames(r.directory_count,directory);frames(r.growth_image_count,growth,true);Require(at==payload.size(),E::invalid_extent);
  }else for(std::size_t at=0;at<payload.size();at+=size){const auto index=at/size;const auto raw=payload.subspan(at,size);if(index<r.map_count)images[index]=raw;else inventory[index-r.map_count]=raw;}
  const auto total_pages=r.directory_count?MixedMaps(images,inventory,directory,headers,r,db,scratch):Maps(images,inventory,headers,r,db,size,scratch);
  Growth(growth,images,directory,r,db,bootstrap,scratch);
  return {E::none,images,headers,total_pages,inventory,directory,growth};
}
NativeManagementControlBundleRead MaterializeBundle(const NativeManagementControlBundleViewRead& view){
  const auto own=[](auto views){Pages out;out.reserve(views.size());for(const auto raw:views)out.emplace_back(raw.begin(),raw.end());return out;};
  return {E::none,own(view.allocation_images),{view.page_headers.begin(),view.page_headers.end()},
    view.total_pages,own(view.inventory_images),own(view.directory_images),own(view.growth_images)};
}
NativeManagementControlBundleRead Decode(const Pages& pages,const Root& r,const Uuid& db,const Uuid& bootstrap,u64 budget){
  BundleHeapMemory heap;std::pmr::monotonic_buffer_resource backing(&heap);
  Scratch scratch{&backing};
  const auto view=DecodeView(pages,r,db,bootstrap,budget,scratch);
  return MaterializeBundle(view);
}
void EncodePage(const Root& root,const Header& common,const Uuid& bootstrap,
    Image payload,u64 index,std::array<byte,32>& next,std::span<byte> b){
  const auto header=disk::EncodeNativeCommonPageHeader(common);Require(header.ok(),E::invalid_header);
  std::fill(b.begin(),b.end(),0);std::copy(header.bytes->begin(),header.bytes->end(),b.begin());auto* f=b.data()+128;
  const auto capacity=b.size()-384;const u64 offset=index*capacity;
  const u32 length=static_cast<u32>(std::min<u64>(capacity,payload.size()-offset));
  std::copy_n(Magic(root),8,f);StoreLittle16(f+8,Version(root));StoreLittle16(f+10,256);StoreLittle32(f+12,384+length);
  Put(f+16,root.object_uuid);Put(f+32,bootstrap);Put(f+48,root.operation_uuid);StoreLittle64(f+64,root.map_count);StoreLittle64(f+72,payload.size());
  StoreLittle64(f+80,offset);StoreLittle64(f+88,index);StoreLittle64(f+96,root.page_count);StoreLittle64(f+104,root.first.page_number);
  std::copy(root.aggregate_sha256.begin(),root.aggregate_sha256.end(),f+112);StoreLittle32(f+144,length);StoreLittle64(f+148,root.inventory_count);
  StoreLittle32(f+156,root.directory_count?8:0);StoreLittle64(f+224,root.directory_count);StoreLittle64(f+232,root.growth_image_count);
  std::copy(next.begin(),next.end(),f+160);std::copy_n(payload.begin()+offset,length,b.begin()+384);
  const auto seal=Seal(b);std::copy(seal.begin(),seal.end(),f+192);next=Hash(b);
}
} // namespace
NativeManagementControlBundleError ValidateNativeManagementControlBundleRoot(const Root& r,const Uuid& db,const Uuid& bootstrap,u64 budget) noexcept {
  u32 size=0;return CheckShape(r,db,bootstrap,budget,size);
}
NativeManagementControlBundleImage EncodeNativeManagementControlBundle(const Pages& maps,const Uuid& db,const Uuid& bootstrap,const Uuid& object,const Uuid& attempt,const std::vector<Header>& headers,u64 budget,const Pages& inventory,const Pages& directory,const Pages& growth) noexcept {
  try{
    BundleHeapMemory heap;std::pmr::monotonic_buffer_resource backing(&heap);Scratch scratch{&backing};
    Require(!headers.empty()&&!maps.empty(),E::invalid_request);Root root;root.first=Self(headers.front());root.object_uuid=object;root.operation_uuid=attempt;root.map_count=maps.size();root.inventory_count=inventory.size();root.directory_count=directory.size();root.page_count=headers.size();root.growth_image_count=growth.size();
    if(root.directory_count)for(const auto* chain:{&maps,&inventory,&directory,&growth})for(const auto& b:*chain){const auto limit=std::numeric_limits<u64>::max();Require(b.size()<=limit-8&&root.payload_bytes<=limit-8-b.size(),E::invalid_extent);root.payload_bytes+=8+b.size();}
    const auto size=Shape(root,db,bootstrap,budget,false);std::pmr::set<Uuid> ids(scratch.resource);for(std::size_t i=0;i<headers.size();++i)Common(headers[i],root,db,bootstrap,size,i,ids);
    if(root.directory_count)MixedMaps(maps,inventory,directory,headers,root,db,scratch);else Maps(maps,inventory,headers,root,db,size,scratch);
    Growth(growth,maps,directory,root,db,bootstrap,scratch);
    Bytes payload;payload.reserve(static_cast<std::size_t>(PayloadBytes(root,size)));for(const auto* chain:{&maps,&inventory,&directory,&growth})for(const auto& b:*chain){
      if(root.directory_count){const auto at=payload.size();payload.resize(at+8);StoreLittle64(payload.data()+at,b.size());}payload.insert(payload.end(),b.begin(),b.end());}root.aggregate_sha256=Hash(payload);
    Pages pages(headers.size());std::array<byte,32> next{};
    for(std::size_t left=pages.size();left;--left){const auto i=left-1;auto& b=pages[i];b.resize(size);EncodePage(root,headers[i],bootstrap,payload,i,next,b);}
    root.first_page_sha256=next;return {E::none,root,std::move(pages)};
  }catch(E e){return Fail<NativeManagementControlBundleImage>(e);}catch(const std::bad_alloc&){return Fail<NativeManagementControlBundleImage>(E::resource_exhausted);}catch(const std::length_error&){return Fail<NativeManagementControlBundleImage>(E::resource_exhausted);}catch(...){return Fail<NativeManagementControlBundleImage>(E::invalid_extent);}
}
NativeManagementControlBundleEncoding EncodeNativeManagementControlBundleInto(
    std::span<const Image> maps,const Uuid& db,const Uuid& bootstrap,const Uuid& object,const Uuid& attempt,
    std::span<const Header> headers,u64 budget,std::span<byte> output,std::span<byte> backing,
    std::span<const Image> inventory,std::span<const Image> directory,std::span<const Image> growth) noexcept {
  const auto fail=[](E e){return Fail<NativeManagementControlBundleEncoding>(e);};
  const auto disjoint=[&](auto input){return disk::detail::DisjointNativeDecodeRegions(input,output,backing);};
  if(!disjoint(std::span(&db,1))||!disjoint(std::span(&bootstrap,1))||!disjoint(std::span(&object,1))||
     !disjoint(std::span(&attempt,1))||!disjoint(headers)||
     (!headers.empty()&&reinterpret_cast<std::uintptr_t>(headers.data())%alignof(Header)))return fail(E::invalid_workspace);
  const std::array chains{maps,inventory,directory,growth};
  for(const auto chain:chains){
    if(!disjoint(chain)||(!chain.empty()&&reinterpret_cast<std::uintptr_t>(chain.data())%alignof(Image)))return fail(E::invalid_workspace);
    for(const auto raw:chain)if(!disjoint(raw))return fail(E::invalid_workspace);
  }
  try{
    Require(!headers.empty()&&!maps.empty(),E::invalid_request);
    Root root;root.first=Self(headers.front());root.object_uuid=object;root.operation_uuid=attempt;
    root.map_count=maps.size();root.inventory_count=inventory.size();root.directory_count=directory.size();root.page_count=headers.size();root.growth_image_count=growth.size();
    if(root.directory_count)for(const auto chain:chains)for(const auto raw:chain){
      const auto limit=std::numeric_limits<u64>::max();Require(raw.size()<=limit-8&&root.payload_bytes<=limit-8-raw.size(),E::invalid_extent);root.payload_bytes+=8+raw.size();}
    const auto size=Shape(root,db,bootstrap,budget,false);Require(headers.size()<=output.size()/size,E::resource_exhausted);
    BundleMemory memory(backing);Scratch scratch{&memory};std::pmr::set<Uuid> ids(scratch.resource);
    for(std::size_t i=0;i<headers.size();++i)Common(headers[i],root,db,bootstrap,size,i,ids);
    if(root.directory_count)MixedMaps(maps,inventory,directory,headers,root,db,scratch);else Maps(maps,inventory,headers,root,db,size,scratch);
    Growth(growth,maps,directory,root,db,bootstrap,scratch);
    auto payload=scratch.Array<byte>(static_cast<std::size_t>(PayloadBytes(root,size)));std::size_t at=0;
    for(const auto chain:chains)for(const auto raw:chain){
      if(root.directory_count){StoreLittle64(payload.data()+at,raw.size());at+=8;}
      std::copy(raw.begin(),raw.end(),payload.begin()+at);at+=raw.size();}
    Require(at==payload.size(),E::invalid_extent);root.aggregate_sha256=Hash(payload);
    auto pages=output.first(headers.size()*size);std::array<byte,32> next{};
    for(std::size_t left=headers.size();left;--left){const auto i=left-1;EncodePage(root,headers[i],bootstrap,payload,i,next,pages.subspan(i*size,size));}
    root.first_page_sha256=next;return {E::none,root,pages,memory.used()};
  }catch(E e){return fail(e);}catch(const std::bad_alloc&){return fail(E::resource_exhausted);}
   catch(const std::length_error&){return fail(E::resource_exhausted);}catch(...){return fail(E::invalid_extent);}
}
NativeManagementControlBundleRead DecodeNativeManagementControlBundle(const Pages& pages,const Root& r,const Uuid& db,const Uuid& bootstrap,u64 budget) noexcept {
  try{return Decode(pages,r,db,bootstrap,budget);}catch(E e){return Fail<NativeManagementControlBundleRead>(e);}catch(const std::bad_alloc&){return Fail<NativeManagementControlBundleRead>(E::resource_exhausted);}catch(const std::length_error&){return Fail<NativeManagementControlBundleRead>(E::resource_exhausted);}catch(...){return Fail<NativeManagementControlBundleRead>(E::invalid_extent);}
}


NativeManagementControlBundleViewRead detail::DecodeNativeManagementControlBundleBacked(
    std::span<const std::span<const byte>> pages,const Root& root,const Uuid& db,const Uuid& bootstrap,
    u64 budget,std::pmr::memory_resource& resource) noexcept {
  try{Scratch scratch{&resource};return DecodeView(pages,root,db,bootstrap,budget,scratch);}
  catch(E e){return Fail<NativeManagementControlBundleViewRead>(e);}
  catch(const std::bad_alloc&){return Fail<NativeManagementControlBundleViewRead>(E::resource_exhausted);}
  catch(const std::length_error&){return Fail<NativeManagementControlBundleViewRead>(E::resource_exhausted);}
  catch(...){return Fail<NativeManagementControlBundleViewRead>(E::invalid_extent);}
}
NativeManagementControlBundleViewRead DecodeNativeManagementControlBundleInto(
    std::span<const std::span<const byte>> pages,const Root& root,const Uuid& db,const Uuid& bootstrap,
    u64 budget,std::span<byte> backing) noexcept {
  // Validate the entire writable allocation, not merely the eventually used
  // prefix. Inputs may alias each other but no writable byte may alias an input.
  const auto disjoint=[&](auto input){return disk::detail::DisjointNativeDecodeRegions(backing,input);};
  if(!disjoint(pages)||!disjoint(std::span{&root,1})||!disjoint(std::span{&db,1})||
     !disjoint(std::span{&bootstrap,1}))return Fail<NativeManagementControlBundleViewRead>(E::invalid_workspace);
  for(const auto raw:pages)if(!disjoint(raw))return Fail<NativeManagementControlBundleViewRead>(E::invalid_workspace);
  try {
    BundleMemory memory(backing);Scratch scratch{&memory};
    auto result=DecodeView(pages,root,db,bootstrap,budget,scratch);
    result.backing_bytes_used=memory.used();return result;
  }catch(E e){return Fail<NativeManagementControlBundleViewRead>(e);}
   catch(const std::bad_alloc&){return Fail<NativeManagementControlBundleViewRead>(E::resource_exhausted);}
   catch(const std::length_error&){return Fail<NativeManagementControlBundleViewRead>(E::resource_exhausted);}
   catch(...){return Fail<NativeManagementControlBundleViewRead>(E::invalid_extent);}
}

namespace {

bool EqualImage(Image a,Image b){return std::equal(a.begin(),a.end(),b.begin(),b.end());}
NativeManagementControlBundleViewRead ReadBundleView(const disk::NativeFilespaceDevice& file,const Root& r,const Uuid& db,const Uuid& bootstrap,u64 budget,const Image* historical,Scratch& scratch,disk::FileDevice::ReadLatencyBatch* observations,bool result_context=false,NativeManagementControlBundleDeviceRead* receipt=nullptr) noexcept {
  try{
    const auto size=Shape(r,db,bootstrap,budget);Require(file.device&&file.filespace_uuid==r.first.filespace_uuid&&file.page_size_profile_uuid==r.first.page_size_profile_uuid,E::invalid_request);const auto guard=file.device->AcquireOperationGuard();Require(file.device->is_open(),E::invalid_request);
    const disk::FilespaceBootstrapBinding binding{db,file.filespace_uuid,file.page_size_profile_uuid};
    const auto read=[&](u64 offset,void* data,std::size_t bytes){
      auto io=observations?observations->ReadAt(offset,data,bytes):file.device->ReadAt(offset,data,bytes);
      if(receipt){receipt->io_status=io.status;receipt->io_diagnostic=std::move(io.diagnostic);
        Require(io.bytes_transferred<=std::numeric_limits<u64>::max()-receipt->physical_bytes_read,E::invalid_extent);
        receipt->physical_bytes_read+=io.bytes_transferred;}return io;};
    const auto size_observation=[&]{auto value=file.device->Size();
      if(receipt){receipt->io_status=value.status;receipt->io_diagnostic=std::move(value.diagnostic);}return value;};
    Image actual_zero;
    const auto zero=historical?disk::DecodeFilespacePageZeroInto(*historical,scratch.Array<disk::FilespaceRootReference>(32),&binding):detail::ReadNativeMetadataPageZero(binding,scratch,read,size_observation,actual_zero);
    if(!zero.ok())throw zero.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:zero.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:zero.error==disk::FilespacePageZeroError::io_failure?E::io_failure:E::bootstrap_failure;
    const auto& z=*zero.record;Require(z.page_uuid==bootstrap,E::binding_mismatch);Require(!(z.bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted),E::encrypted_requires_authority);Require(!(z.bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required),E::cluster_requires_authority);
    u64 observed_size=0;
    if(historical){const auto extent=size_observation();Require(extent.ok(),E::io_failure);observed_size=extent.size_bytes;
      Require(observed_size>=z.total_pages*u64{size},E::invalid_extent);auto actual=scratch.Array<byte>(size);
      const auto io=read(0,actual.data(),actual.size());Require(io.ok()&&io.bytes_transferred==actual.size(),E::io_failure);
      Require(std::equal(historical->begin(),historical->begin()+4096,actual.begin())&&
        std::equal(historical->begin()+4480,historical->end(),actual.begin()+4480),E::binding_mismatch);}
    Require(std::any_of(z.roots.begin(),z.roots.end(),[](const auto& v){return v.kind==20;})&&std::any_of(z.roots.begin(),z.roots.end(),[](const auto& v){return v.kind==21;}),E::bootstrap_failure);
    Require(r.first.page_number<z.total_pages&&r.page_count<=z.total_pages-r.first.page_number,E::invalid_extent);
    for(const auto& ref:z.roots)if(ref.filespace_uuid==file.filespace_uuid)Require(ref.page_number<r.first.page_number||ref.page_number-r.first.page_number>=r.page_count,E::invalid_extent);
    auto pages=scratch.Array<Image>(static_cast<std::size_t>(r.page_count));for(std::size_t i=0;i<pages.size();++i){auto b=scratch.Array<byte>(size);pages[i]=b;const auto io=read((r.first.page_number+i)*u64{size},b.data(),b.size());Require(io.ok()&&io.bytes_transferred==b.size(),E::io_failure);}
    auto decoded=DecodeView(pages,r,db,bootstrap,budget,scratch);disk::FilespacePageZeroViewResult target;
    if(r.growth_image_count){target=scratch.PageZero(decoded.growth_images[1]);
      if(!target.ok())throw target.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:target.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:E::binding_mismatch;
      if(target.record->bootstrap.filespace_uuid==file.filespace_uuid){if(historical){Require(EqualImage(*historical,decoded.growth_images[result_context?1:0]),E::binding_mismatch);
          if(result_context){const auto& raw=decoded.growth_images[0];const auto before=disk::DecodeFilespacePageZeroInto(raw,scratch.Array<disk::FilespaceRootReference>(32),&binding);
            if(!before.ok())throw before.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:before.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:E::binding_mismatch;
            Require(r.first.page_number<before.record->total_pages&&r.page_count<=before.record->total_pages-r.first.page_number,E::invalid_extent);}}
        else Require(EqualImage(actual_zero,decoded.growth_images[1]),E::binding_mismatch);}}
    const auto& expected=historical&&target.record&&target.record->bootstrap.filespace_uuid==file.filespace_uuid?*target.record:z;
    Require(decoded.total_pages==expected.total_pages,E::binding_mismatch);
    if(r.directory_count){bool found=false;for(const auto& raw:decoded.directory_images){const auto directory=scratch.Directory(raw);
      if(!directory.ok())throw directory.error==page::NativeDirectoryError::resource_exhausted?E::resource_exhausted:directory.error==page::NativeDirectoryError::hash_failure?E::hash_failure:E::binding_mismatch;
      for(const auto& member:directory.directory->records){if(member.bootstrap.filespace_uuid!=file.filespace_uuid)continue;const auto& a=member.bootstrap;const auto& b=expected.bootstrap;
        Require(!found&&a.database_uuid==b.database_uuid&&a.page_size_profile_uuid==b.page_size_profile_uuid&&a.checksum_profile_uuid==b.checksum_profile_uuid&&a.encryption_profile_uuid==b.encryption_profile_uuid&&
          a.page_size_bytes==b.page_size_bytes&&a.durable_format_generation==b.durable_format_generation&&a.flags==b.flags&&a.filespace_role==b.filespace_role&&a.lifecycle_state==b.lifecycle_state&&
          member.page_zero_uuid==expected.page_uuid&&member.page_zero_generation==expected.page_generation&&member.root_set_generation==expected.root_set_generation&&member.total_pages==expected.total_pages,E::binding_mismatch);found=true;}}
      Require(found,E::binding_mismatch);
    }
    if(historical){const auto extent=size_observation();Require(extent.ok(),E::io_failure);Require(extent.size_bytes==observed_size,E::physical_extent_changed);}
    decoded.physical_images=pages;return decoded;
  }catch(E e){return Fail<NativeManagementControlBundleViewRead>(e);}catch(const std::bad_alloc&){return Fail<NativeManagementControlBundleViewRead>(E::resource_exhausted);}catch(const std::length_error&){return Fail<NativeManagementControlBundleViewRead>(E::resource_exhausted);}catch(...){return Fail<NativeManagementControlBundleViewRead>(E::io_failure);}
}
NativeManagementControlBundleRead ReadBundle(const disk::NativeFilespaceDevice& file,const Root& root,
    const Uuid& database,const Uuid& bootstrap,u64 budget,const Bytes* historical,bool result_context=false) noexcept{
  try {
    BundleHeapMemory heap;std::pmr::monotonic_buffer_resource backing(&heap);Scratch scratch{&backing};
    const Image context=historical?Image(*historical):Image{};
    const auto result=ReadBundleView(file,root,database,bootstrap,budget,historical?&context:nullptr,scratch,nullptr,result_context);
    if(!result.ok())return Fail<NativeManagementControlBundleRead>(result.error);
    return MaterializeBundle(result);
  }catch(const std::bad_alloc&){return Fail<NativeManagementControlBundleRead>(E::resource_exhausted);}
   catch(const std::length_error&){return Fail<NativeManagementControlBundleRead>(E::resource_exhausted);}
   catch(...){return Fail<NativeManagementControlBundleRead>(E::io_failure);}
}

} // namespace
NativeManagementControlBundleRead ReadNativeManagementControlBundleFromOpenDevice(const disk::NativeFilespaceDevice& file,const Root& r,const Uuid& db,const Uuid& bootstrap,u64 budget) noexcept {
  return ReadBundle(file,r,db,bootstrap,budget,nullptr);
}
NativeManagementControlBundleRead ReadNativeManagementControlBundleAtHistoricalPageZeroFromOpenDevice(const disk::NativeFilespaceDevice& file,const Root& r,const Uuid& db,const Uuid& bootstrap,const Bytes& historical,u64 budget) noexcept {
  return ReadBundle(file,r,db,bootstrap,budget,&historical);
}
NativeManagementControlBundleRead ReadNativeManagementControlBundleAtHistoricalResultFromOpenDevice(const disk::NativeFilespaceDevice& file,const Root& r,const Uuid& db,const Uuid& bootstrap,const Bytes& historical,u64 budget) noexcept {
  return ReadBundle(file,r,db,bootstrap,budget,&historical,true);
}

NativeManagementControlBundleDeviceRead ReadNativeManagementControlBundleInto(
    const disk::NativeFilespaceDevice& file,const Root& root,const Uuid& database,const Uuid& bootstrap,
    u64 budget,NativeManagementControlReadContext context,Image historical,
    disk::FileDevice::ReadLatencyBatch& observations,std::span<byte> backing) noexcept {
  NativeManagementControlBundleDeviceRead out;
  if(!file.device||&observations.device()!=file.device||
     (context!=NativeManagementControlReadContext::current&&
      context!=NativeManagementControlReadContext::historical_before&&
      context!=NativeManagementControlReadContext::historical_result)||
     (context==NativeManagementControlReadContext::current&&!historical.empty()))
    return out;
  const auto disjoint=[&](auto input){return disk::detail::DisjointNativeDecodeRegions(backing,input);};
  if(!disjoint(std::span{&file,1})||!disjoint(std::span{file.device,1})||
     !disjoint(std::span{&observations,1})||!disjoint(std::span{&root,1})||
     !disjoint(std::span{&database,1})||!disjoint(std::span{&bootstrap,1})||!disjoint(historical))
    {out.bundle.error=E::invalid_workspace;return out;}
  try {
    BundleMemory memory(backing);Scratch scratch{&memory};
    const auto* retained=context==NativeManagementControlReadContext::current?nullptr:&historical;
    out.bundle=ReadBundleView(file,root,database,bootstrap,budget,retained,scratch,&observations,
      context==NativeManagementControlReadContext::historical_result,&out);
    if(out.ok())out.bundle.backing_bytes_used=memory.used();return out;
  }catch(const std::bad_alloc&){out.bundle=Fail<NativeManagementControlBundleViewRead>(E::resource_exhausted);}
   catch(const std::length_error&){out.bundle=Fail<NativeManagementControlBundleViewRead>(E::resource_exhausted);}
   catch(...){out.bundle=Fail<NativeManagementControlBundleViewRead>(E::io_failure);}
  return out;
}

// Internal complete-reader composition. The surrounding owner guarantees
// resource/input separation and observation lifetime before invoking this seam.
NativeManagementControlBundleDeviceRead detail::ReadNativeManagementControlBundleBacked(
    const disk::NativeFilespaceDevice& file,const Root& root,const Uuid& database,const Uuid& bootstrap,
    u64 budget,NativeManagementControlReadContext context,Image historical,
    disk::FileDevice::ReadLatencyBatch* observations,std::pmr::memory_resource& resource) noexcept {
  NativeManagementControlBundleDeviceRead out;
  if((observations&&(!file.device||&observations->device()!=file.device))||
     (context!=NativeManagementControlReadContext::current&&
      context!=NativeManagementControlReadContext::historical_before&&
      context!=NativeManagementControlReadContext::historical_result)||
     (context==NativeManagementControlReadContext::current&&!historical.empty()))return out;
  Scratch scratch{&resource};
  const auto* retained=context==NativeManagementControlReadContext::current?nullptr:&historical;
  out.bundle=ReadBundleView(file,root,database,bootstrap,budget,retained,scratch,observations,
    context==NativeManagementControlReadContext::historical_result,&out);
  return out;
}
} // namespace scratchbird::storage::database
