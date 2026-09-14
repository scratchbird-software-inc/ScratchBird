// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "physical_mga_cow_store.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <mutex>
#include <set>

namespace scratchbird::storage::database::detail {
namespace disk=scratchbird::storage::disk;
namespace page=scratchbird::storage::page;
namespace mga=scratchbird::transaction::mga;
namespace hash=scratchbird::core::hash;
using Uuid=scratchbird::core::platform::Uuid;
using byte=scratchbird::core::platform::byte;
struct NativeReservedPage {
  disk::NativeCommonPageHeader header;
  Uuid object_uuid;
  mga::TransactionIdentity owner;
  std::vector<disk::NativeFilespaceDevice> devices;
  std::vector<std::unique_lock<std::recursive_mutex>> guards;
  NativeCheckpointDirectoryResult authority;
  page::NativeAllocationChainResult allocation;
  page::NativeAllocationRecord reservation;
  disk::FileDevice* target=nullptr;
  u64 retained_image_bytes=0;
};
// Trusted internal preflight. Returned guards remain held while the owning
// family validates its image and this common path performs durable I/O.
template<class TResult,class E>
std::optional<NativeReservedPage> PrepareNativeReservedPage(
    const std::vector<disk::NativeFilespaceDevice>& supplied,
    const disk::FilespaceRootReference& checkpoint,const mga::TransactionIdentity& owner,
    const disk::NativeCommonPageHeader& h,const Uuid& object_uuid,u32 allowed_roles,
    u64 budget,TResult& failure) {
  const auto fail=[&](E error)->std::optional<NativeReservedPage>{failure.error=error;return {};};
  const auto V7=[](const Uuid& id){return core::uuid::IsEngineIdentityUuid(id);};
  if(supplied.empty()||!budget||!V7(h.database_uuid)||!V7(owner.transaction_uuid.value)||
      owner.transaction_uuid.kind!=core::platform::UuidKind::transaction||!owner.local_id.value||
      owner.scope!=mga::TransactionScope::local_node)return fail(E::invalid_request);
  if(h.flags)return fail(E::header_requires_authority);
    NativeReservedPage prepared;prepared.header=h;prepared.owner=owner;prepared.object_uuid=object_uuid;
    auto& devices=prepared.devices;devices=supplied;std::sort(devices.begin(),devices.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    std::set<disk::FileDevice*> handles;
    auto& guards=prepared.guards;guards.reserve(devices.size());
    for(std::size_t i=0;i<devices.size();++i){const auto& file=devices[i];
      if(!file.device||!V7(file.filespace_uuid)||!handles.insert(file.device).second||
          (i&&devices[i-1].filespace_uuid==file.filespace_uuid))return fail(E::invalid_request);
      guards.push_back(file.device->AcquireOperationGuard());
    }
    const auto target=std::lower_bound(devices.begin(),devices.end(),h.filespace_uuid,[](const auto& f,const auto& id){return f.filespace_uuid<id;});
    if(target==devices.end()||target->filespace_uuid!=h.filespace_uuid||target->page_size_profile_uuid!=h.page_size_profile_uuid||target->device->read_only())return fail(E::invalid_destination);
    auto authority=VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(h.database_uuid,devices,checkpoint,budget);
    if(!authority.ok()){failure.error=E::checkpoint_failure;failure.checkpoint_error=authority.error;failure.directory_error=authority.directory_error;failure.inventory_error=authority.checkpoint_inventory.inventory_error;return {};}
    const auto& pair=authority.checkpoint_inventory;const auto& cp=*pair.checkpoint;
    if(cp.flags&4)return fail(E::cluster_requires_authority);
    bool member=false;
    for(const auto& image:authority.directory.pages)for(const auto& entry:image.directory->records)
      if(entry.bootstrap.filespace_uuid==h.filespace_uuid){member=entry.bootstrap.filespace_role<32&&(allowed_roles&(u32{1}<<entry.bootstrap.filespace_role))!=0&&entry.bootstrap.page_size_bytes==h.page_size_bytes&&h.page_number<entry.total_pages;}
    if(!member)return fail(E::invalid_destination);
    if(h.page_uuid==cp.header.page_uuid)return fail(E::reservation_mismatch);
    for(const auto& image:authority.directory.pages){if(h.page_uuid==image.directory->header.page_uuid)return fail(E::reservation_mismatch);
      for(const auto& entry:image.directory->records)if(h.page_uuid==entry.page_zero_uuid)return fail(E::reservation_mismatch);}
    const auto actor=mga::LookupLocalTransaction(pair.inventory,owner.local_id);
    if(!actor.ok()||actor.entry.identity.transaction_uuid.value!=owner.transaction_uuid.value||actor.entry.identity.transaction_uuid.kind!=owner.transaction_uuid.kind||actor.entry.identity.scope!=owner.scope)return fail(E::creator_mismatch);
    if(actor.entry.state!=mga::TransactionState::active)return fail(E::creator_not_active);
    if(actor.entry.rollback_only)return fail(E::creator_rollback_only);
    const disk::FilespaceBootstrapBinding target_binding{h.database_uuid,h.filespace_uuid,h.page_size_profile_uuid};
    const auto target_zero=disk::ReadFilespacePageZeroFromOpenDevice(*target->device,&target_binding);
    if(!target_zero.ok())return fail(E::invalid_destination);
    if(target_zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted)return fail(E::header_requires_authority);
    const bool selected_primary=std::any_of(target_zero.record->roots.begin(),target_zero.record->roots.end(),[](const auto& r){return r.kind==18;});
    const auto selected_root=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==4;});
    if(selected_primary&&(selected_root==cp.roots.end()||selected_root->page.filespace_uuid!=h.filespace_uuid))return fail(E::root_mismatch);
    page::NativeAllocationChainResult allocation;
    if(selected_primary){const auto& r=*selected_root;const disk::FilespaceRootReference ref{3,r.page_type,r.page.filespace_uuid,r.page.page_number,r.page.page_generation,r.page.page_size_profile_uuid,r.object_uuid};
      allocation=page::ReadNativeAllocationChainAtRootFromOpenDevice(*target->device,target_binding,ref,budget-authority.retained_image_bytes);
    }else allocation=page::ReadNativeAllocationChainFromOpenDevice(*target->device,target_binding,budget-authority.retained_image_bytes);
    if(!allocation.ok()){failure.error=E::allocation_failure;failure.allocation_error=allocation.error;return {};}
    if(selected_primary||h.filespace_uuid==checkpoint.filespace_uuid){const auto& map=*allocation.pages.front().map;
      const auto root=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& value){return value.role==4;});
      const disk::NativePageReference ref{h.filespace_uuid,map.header.page_number,map.header.page_generation,map.header.page_size_profile_uuid};
      if(root==cp.roots.end()||root->page!=ref||root->object_uuid!=map.object_uuid)return fail(E::root_mismatch);
      const auto digest=hash::ComputeSha256Digest(allocation.pages.front().bytes);if(!digest.ok())return fail(E::hash_failure);
      if(digest.digest!=root->sha256)return fail(E::root_mismatch);
    }
    const page::NativeAllocationRecord* reservation=nullptr;
    std::set<Uuid> allocation_ids,page_ids;
    for(const auto& image:allocation.pages){const auto& map=*image.map;
      const auto creator=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(map.creator_local_transaction_id));
      if(!creator.ok()||creator.entry.identity.transaction_uuid.value!=map.creator_transaction_uuid||creator.entry.identity.scope!=mga::TransactionScope::local_node)return fail(E::map_creator_mismatch);
      if(!mga::HasCommittedInventoryOutcome(creator.entry))return fail(E::map_creator_not_committed);
      for(const auto& record:map.records){
        if(!allocation_ids.insert(record.allocation_uuid).second||(!record.page_uuid.is_nil()&&!page_ids.insert(record.page_uuid).second))return fail(E::reservation_mismatch);
        const auto original=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(record.creator_local_transaction_id));
        if(!original.ok()||original.entry.identity.transaction_uuid.value!=record.creator_transaction_uuid||original.entry.identity.scope!=mga::TransactionScope::local_node)return fail(E::map_creator_mismatch);
        if(record.page_number==h.page_number&&map.states[record.page_number-map.first_page]==page::NativeAllocationState::reserved)reservation=&record;
      }
    }
    if(!reservation||reservation->page_uuid!=h.page_uuid||reservation->page_generation!=h.page_generation||reservation->page_type!=h.page_type||
        reservation->owner_uuid!=object_uuid||reservation->creator_transaction_uuid!=owner.transaction_uuid.value||reservation->creator_local_transaction_id!=owner.local_id.value)return fail(E::reservation_mismatch);

  prepared.target=target->device;prepared.reservation=*reservation;
  prepared.retained_image_bytes=authority.retained_image_bytes+allocation.retained_image_bytes;
  prepared.authority=std::move(authority);prepared.allocation=std::move(allocation);
  return prepared;
}
template<class TResult,class E>
TResult WriteNativeReservedPageImage(NativeReservedPage& prepared,
    const std::vector<byte>& image,u64 budget,TResult result={}) {
  const auto fail=[&](E error){result.error=error;return result;};
  const auto& h=prepared.header;
  if(prepared.retained_image_bytes>budget||2*u64{h.page_size_bytes}>budget-prepared.retained_image_bytes)
    return fail(E::resource_exhausted);
  if(image.size()!=h.page_size_bytes)return fail(E::invalid_request);
  const disk::NativeCommonPageHeaderBinding binding{{h.database_uuid,h.filespace_uuid,h.page_size_profile_uuid},
    h.page_number,h.page_generation,h.page_type,h.page_uuid};
  const auto common=disk::DecodeNativeCommonPageHeader(image.data(),128,&binding);
  if(!common.ok()||common.header->flags!=h.flags)return fail(E::invalid_request);
  const auto digest=hash::ComputeSha256Digest(image);if(!digest.ok())return fail(E::hash_failure);
  using Receipt=typename decltype(result.receipt)::value_type;
  Receipt receipt{h.database_uuid,prepared.reservation.allocation_uuid,h.page_uuid,prepared.object_uuid,prepared.owner,
    {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid},digest.digest};
  std::vector<byte> scratch(h.page_size_bytes);const u64 offset=h.page_number*u64{h.page_size_bytes};
  const auto before=prepared.target->ReadAt(offset,scratch.data(),scratch.size());
  if(!before.ok()||before.bytes_transferred!=scratch.size())return fail(E::io_failure);
  if(scratch!=image){if(!std::all_of(scratch.begin(),scratch.end(),[](byte b){return b==0;}))return fail(E::destination_not_empty);
    const auto write=prepared.target->WriteAt(offset,image.data(),image.size());
    if(!write.ok()||write.bytes_transferred!=image.size())return fail(E::io_failure);
  }
  if(!prepared.target->Sync().ok())return fail(E::io_failure);
  const auto after=prepared.target->ReadAt(offset,scratch.data(),scratch.size());
  if(!after.ok()||after.bytes_transferred!=scratch.size())return fail(E::io_failure);
  if(scratch!=image)return fail(E::readback_mismatch);
  result.error=E::none;result.receipt=receipt;return result;
}
} // namespace scratchbird::storage::database::detail

