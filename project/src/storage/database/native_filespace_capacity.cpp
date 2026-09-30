// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_filespace_capacity.hpp"
#include "disk_device.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>

namespace scratchbird::storage::database {
namespace {
using E=NativeFilespaceCapacityError;
namespace mga=transaction::mga;
NativeFilespaceCapacityResult Fail(E error){NativeFilespaceCapacityResult r;r.error=error;return r;}
}
NativeFilespaceCapacityResult ReadNativeFilespaceCapacityFromOpenDevices(
    const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& supplied,
    const disk::FilespaceRootReference& expected,const Uuid& target_uuid,u64 budget) noexcept {
  try {
    if(!core::uuid::IsEngineIdentityUuid(database)||!core::uuid::IsEngineIdentityUuid(target_uuid)||
        supplied.empty()||!budget)return Fail(E::invalid_request);
    auto devices=supplied;
    std::sort(devices.begin(),devices.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    std::set<disk::FileDevice*> handles;
    for(std::size_t i=0;i<devices.size();++i)
      if(!devices[i].device||!core::uuid::IsEngineIdentityUuid(devices[i].filespace_uuid)||
          !handles.insert(devices[i].device).second||
          (i&&devices[i-1].filespace_uuid==devices[i].filespace_uuid))return Fail(E::invalid_request);
    std::vector<std::unique_lock<std::recursive_mutex>> guards;guards.reserve(devices.size());
    for(const auto& d:devices)guards.push_back(d.device->AcquireOperationGuard());
    const auto target=std::lower_bound(devices.begin(),devices.end(),target_uuid,
      [](const auto& d,const auto& id){return d.filespace_uuid<id;});
    if(target==devices.end()||target->filespace_uuid!=target_uuid)return Fail(E::invalid_target);
    const disk::FilespaceBootstrapBinding binding{database,target_uuid,target->page_size_profile_uuid};
    auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*target->device,&binding);
    if(!zero.ok()){auto r=Fail(E::bootstrap_failure);r.bootstrap_error=zero.error;return r;}
    if(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required)return Fail(E::cluster_requires_authority);
    if(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted)return Fail(E::encrypted_requires_authority);
    auto directory=VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(database,devices,expected,budget);
    if(!directory.ok()){
      auto r=Fail(E::checkpoint_failure);r.checkpoint_error=directory.error;
      r.inventory_error=directory.checkpoint_inventory.inventory_error;
      r.directory_error=directory.directory_error;return r;
    }
    const auto& cp=*directory.checkpoint_inventory.checkpoint;
    if(cp.flags&4)return Fail(E::cluster_requires_authority);
    const page::NativeFilespaceDirectoryRecord* member=nullptr;
    for(const auto& image:directory.directory.pages)for(const auto& row:image.directory->records)
      if(row.bootstrap.filespace_uuid==target_uuid)member=&row;
    if(!member)return Fail(E::invalid_target);
    const bool primary=target_uuid==expected.filespace_uuid||
      std::any_of(zero.record->roots.begin(),zero.record->roots.end(),[](const auto& r){return r.kind==18;});
    const auto selected=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==4;});
    const auto bootstrap=std::find_if(zero.record->roots.begin(),zero.record->roots.end(),[](const auto& r){return r.kind==3;});
    disk::FilespaceRootReference root;
    std::optional<std::array<byte,32>> expected_hash;
    if(primary){
      if(selected==cp.roots.end()||selected->page.filespace_uuid!=target_uuid)return Fail(E::root_mismatch);
      root={3,selected->page_type,target_uuid,selected->page.page_number,selected->page.page_generation,
        selected->page.page_size_profile_uuid,selected->object_uuid};expected_hash=selected->sha256;
    }
    if(member->allocation_root){const auto& explicit_root=*member->allocation_root;
      if(primary&&(selected->page!=explicit_root.page||selected->object_uuid!=explicit_root.object_uuid||
          selected->sha256!=explicit_root.sha256))return Fail(E::root_mismatch);
      root={3,3,explicit_root.page.filespace_uuid,explicit_root.page.page_number,explicit_root.page.page_generation,
        explicit_root.page.page_size_profile_uuid,explicit_root.object_uuid};expected_hash=explicit_root.sha256;
    }else if(!primary){if(bootstrap==zero.record->roots.end())return Fail(E::root_mismatch);root=*bootstrap;}
    if(root.filespace_uuid!=target_uuid||root.page_size_profile_uuid!=target->page_size_profile_uuid)return Fail(E::root_mismatch);
    if(directory.retained_image_bytes>=budget)return Fail(E::resource_exhausted);
    auto allocation=(primary||member->allocation_root)?
      page::ReadNativeAllocationChainAtRootFromOpenDevice(*target->device,binding,root,budget-directory.retained_image_bytes):
      page::ReadNativeAllocationChainFromOpenDevice(*target->device,binding,budget-directory.retained_image_bytes);
    if(!allocation.ok()){auto r=Fail(E::allocation_failure);r.allocation_error=allocation.error;return r;}
    if(allocation.retained_image_bytes>budget-directory.retained_image_bytes)return Fail(E::resource_exhausted);
    u64 retained=directory.retained_image_bytes+allocation.retained_image_bytes;
    const auto& head=*allocation.pages.front().map;
    const disk::NativePageReference actual{target_uuid,head.header.page_number,head.header.page_generation,head.header.page_size_profile_uuid};
    if(head.object_uuid!=root.object_uuid||actual!=disk::NativePageReference{root.filespace_uuid,root.page_number,root.page_generation,root.page_size_profile_uuid}||
        head.total_pages!=member->total_pages)return Fail(E::root_mismatch);
    if(member->allocation_root&&(member->allocation_root->map_generation!=head.map_generation||
        member->allocation_root->capacity_generation!=head.capacity_generation))return Fail(E::root_mismatch);
    const auto hash=core::hash::ComputeSha256Digest(allocation.pages.front().bytes);
    if(!hash.ok())return Fail(E::hash_failure);
    if(expected_hash&&*expected_hash!=hash.digest)return Fail(E::root_mismatch);
    std::optional<NativeManagementControlAuthority> proof;
    bool operation_owned=false;
    for(const auto& image:allocation.pages){
      operation_owned|=!image.map->creator_operation_uuid.is_nil();
      for(const auto& record:image.map->records)operation_owned|=!record.creator_operation_uuid.is_nil();
    }
    if(operation_owned){
      if(retained>=budget)return Fail(E::resource_exhausted);
      auto admitted=ReadNativeManagementControlAuthorityFromOpenDevices(database,devices,cp.header.filespace_uuid,budget-retained);
      if(!admitted.ok()){auto r=Fail(E::management_failure);r.management_error=admitted.error;return r;}
      if(admitted.selection->checkpoint!=disk::NativePageReference{cp.header.filespace_uuid,cp.header.page_number,cp.header.page_generation,cp.header.page_size_profile_uuid}||
          admitted.selection->checkpoint_sha256!=directory.checkpoint_inventory.checkpoint_sha256)return Fail(E::root_mismatch);
      if(admitted.verified_image_bytes>budget-retained)return Fail(E::resource_exhausted);
      retained+=admitted.verified_image_bytes;proof=std::move(admitted);
    }
    const auto creator=[&](const Uuid& id,u64 number,bool committed){
      const auto row=mga::LookupLocalTransaction(directory.checkpoint_inventory.inventory,mga::MakeLocalTransactionId(number));
      return row.ok()&&row.entry.identity.transaction_uuid.value==id&&
        row.entry.identity.scope==mga::TransactionScope::local_node&&(!committed||mga::HasCommittedInventoryOutcome(row.entry));
    };
    std::set<Uuid> allocation_ids,page_ids;
    for(const auto& image:allocation.pages){const auto& map=*image.map;
      if(map.creator_operation_uuid.is_nil()){
        if(!creator(map.creator_transaction_uuid,map.creator_local_transaction_id,true))return Fail(E::creator_mismatch);
      }else if(!proof||!MatchesNativeManagementControlMap(*proof,map))return Fail(E::creator_mismatch);
      for(const auto& row:map.records){
        if(!allocation_ids.insert(row.allocation_uuid).second||(!row.page_uuid.is_nil()&&!page_ids.insert(row.page_uuid).second))return Fail(E::creator_mismatch);
        if(row.creator_operation_uuid.is_nil()){
          if(!creator(row.creator_transaction_uuid,row.creator_local_transaction_id,false))return Fail(E::creator_mismatch);
        }else if(!proof||!MatchesNativeManagementControlAllocation(*proof,target_uuid,row,map.states[row.page_number-map.first_page]))return Fail(E::creator_mismatch);
      }
    }
    if(head.total_pages>std::numeric_limits<u64>::max()/head.header.page_size_bytes)return Fail(E::physical_capacity_mismatch);
    const u64 bytes=head.total_pages*head.header.page_size_bytes;
    const auto size=target->device->Size();if(!size.ok())return Fail(E::io_failure);
    if(size.size_bytes!=bytes)return Fail(E::physical_capacity_mismatch);
    NativeFilespaceCapacityObservation observed;
    observed.database_uuid=database;observed.filespace_uuid=target_uuid;observed.locator_uuid=member->locator_uuid;
    observed.page_zero_uuid=member->page_zero_uuid;observed.page_size_profile_uuid=head.header.page_size_profile_uuid;
    observed.page_size_bytes=head.header.page_size_bytes;observed.filespace_role=member->bootstrap.filespace_role;
    observed.lifecycle_state=member->bootstrap.lifecycle_state;observed.page_zero_generation=member->page_zero_generation;
    observed.filespace_root_set_generation=member->root_set_generation;
    observed.directory_generation=directory.directory.pages.front().directory->directory_generation;
    observed.checkpoint_generation=cp.checkpoint_generation;observed.checkpoint_root_set_generation=cp.root_set_generation;
    observed.checkpoint=expected;observed.checkpoint_sha256=directory.checkpoint_inventory.checkpoint_sha256;
    observed.allocation_root=root;observed.allocation_sha256=hash.digest;
    observed.map_generation=head.map_generation;observed.capacity_generation=head.capacity_generation;
    observed.total_pages=head.total_pages;observed.physical_bytes=bytes;observed.state_counts=allocation.state_counts;
    NativeFilespaceCapacityResult result;result.error=E::none;result.observation=observed;result.retained_image_bytes=retained;return result;
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
   catch(...){return Fail(E::io_failure);}
}
} // namespace scratchbird::storage::database
