// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_checkpoint_selection.hpp"
#include "disk_device.hpp"
#include "hash_digest.hpp"
#include "transaction_inventory_validation.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <mutex>
#include <set>

namespace scratchbird::storage::database {
namespace {
using E=NativeCheckpointSelectionError;
namespace mga=scratchbird::transaction::mga;
bool V7(const Uuid& id){return core::uuid::IsEngineIdentityUuid(id);}
NativeBoundCheckpointSelection Fail(E e){NativeBoundCheckpointSelection r;r.error=e;return r;}
NativeBoundCheckpointSelection CheckpointFailure(const NativeCheckpointInventoryResult& source){
  using C=NativeCheckpointError;using I=page::NativeInventoryError;
  const auto nested=source.error==C::inventory_failure?source.inventory_error:I::none;
  auto r=Fail(source.error==C::hash_failure||nested==I::hash_failure?E::hash_failure:
    source.error==C::resource_exhausted||nested==I::resource_exhausted?E::resource_exhausted:
    source.error==C::io_failure||nested==I::io_failure?E::io_failure:E::checkpoint_failure);
  r.checkpoint_error=source.error;return r;
}
NativeBoundCheckpointSelection AllocationFailure(page::NativeAllocationError error){
  using A=page::NativeAllocationError;
  auto r=Fail(error==A::hash_failure?E::hash_failure:error==A::resource_exhausted?E::resource_exhausted:
    error==A::io_failure?E::io_failure:E::allocation_failure);r.allocation_error=error;return r;
}
}
NativeBoundCheckpointSelection ReadNativeBoundCheckpointSelectionFromOpenDevices(
    const Uuid& database_uuid,const std::vector<disk::NativeFilespaceDevice>& supplied,
    const Uuid& primary_uuid,u64 budget) noexcept {
  try {
    if(!V7(database_uuid)||!V7(primary_uuid)||supplied.empty())return Fail(E::invalid_filespace);
    auto devices=supplied;std::sort(devices.begin(),devices.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    std::set<disk::FileDevice*> handles;std::vector<std::unique_lock<std::recursive_mutex>> guards;guards.reserve(devices.size());
    for(std::size_t i=0;i<devices.size();++i){const auto& f=devices[i];
      if(!V7(f.filespace_uuid)||!disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid)||!f.device||!handles.insert(f.device).second||
        (i&&devices[i-1].filespace_uuid==f.filespace_uuid))return Fail(E::invalid_filespace);guards.push_back(f.device->AcquireOperationGuard());}
    const auto primary=std::lower_bound(devices.begin(),devices.end(),primary_uuid,[](const auto& f,const auto& id){return f.filespace_uuid<id;});
    if(primary==devices.end()||primary->filespace_uuid!=primary_uuid)return Fail(E::invalid_filespace);
    const disk::FilespaceBootstrapBinding binding{database_uuid,primary_uuid,primary->page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*primary->device,&binding);
    if(!zero.ok())return Fail(zero.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:
      zero.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:zero.error==disk::FilespacePageZeroError::io_failure?E::io_failure:E::bootstrap_failure);
    const auto& z=*zero.record;if(z.bootstrap.filespace_role>4)return Fail(E::invalid_filespace);
    const auto first=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==18;});
    const auto second=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==19;});
    if(first==z.roots.end()||second==z.roots.end())return Fail(E::slot_binding_mismatch);
    const u64 size=z.bootstrap.page_size_bytes;
    // Pair classification temporarily decodes two additional owned images.
    if(budget<4*size)return Fail(E::resource_exhausted);
    NativeBoundCheckpointSelection result;std::array<disk::NativeCommonPageHeader,2> headers;
    const disk::FilespaceRootReference* refs[]={&*first,&*second};
    for(unsigned i=0;i<2;++i){auto& bytes=result.slots[i];bytes.resize(size);const auto& ref=*refs[i];
      const auto io=primary->device->ReadAt(ref.page_number*size,bytes.data(),bytes.size());
      if(!io.ok()||io.bytes_transferred!=bytes.size())return Fail(E::io_failure);
      const disk::NativeCommonPageHeaderBinding expected{binding,ref.page_number,ref.page_generation,0x30e,{}};
      const auto h=disk::DecodeNativeCommonPageHeader(bytes.data(),128,&expected);if(!h.ok())return Fail(E::slot_binding_mismatch);headers[i]=*h.header;}
    const auto classified=ClassifyNativeCheckpointSelectionPair(result.slots[0],result.slots[1]);
    if(!classified.ok())return Fail(classified.error);const auto& selection=*classified.selection;
    if(selection.bootstrap_uuid!=z.page_uuid||selection.object_uuid!=first->object_uuid||selection.object_uuid!=second->object_uuid)return Fail(E::slot_binding_mismatch);
    const auto& ref=selection.checkpoint;
    const disk::FilespaceRootReference checkpoint{9,0x300,ref.filespace_uuid,ref.page_number,ref.page_generation,ref.page_size_profile_uuid,selection.checkpoint_object_uuid};
    result.retained_image_bytes=2*size;
    result.checkpoint_inventory=VerifyNativeCheckpointInventoryFromOpenDevices(database_uuid,devices,checkpoint,budget-result.retained_image_bytes);
    if(!result.checkpoint_inventory.ok())return CheckpointFailure(result.checkpoint_inventory);
    const auto& pair=result.checkpoint_inventory;const auto& cp=*pair.checkpoint;
    if(pair.checkpoint_sha256!=selection.checkpoint_sha256||cp.checkpoint_generation!=selection.checkpoint_generation||
      cp.root_set_generation!=selection.root_set_generation||cp.timeline_uuid!=selection.timeline_uuid||
      bool(cp.flags&4)!=bool(z.bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required))return Fail(E::checkpoint_binding_mismatch);
    result.retained_image_bytes+=pair.retained_image_bytes;
    if(selection.previous_checkpoint){
      if(!cp.predecessor||*cp.predecessor!=*selection.previous_checkpoint||cp.predecessor_sha256!=selection.previous_checkpoint_sha256||
        cp.object_uuid!=selection.previous_checkpoint_object_uuid)return Fail(E::checkpoint_binding_mismatch);
      const auto& p=*selection.previous_checkpoint;
      const disk::FilespaceRootReference previous{9,0x300,p.filespace_uuid,p.page_number,p.page_generation,p.page_size_profile_uuid,selection.previous_checkpoint_object_uuid};
      result.predecessor=VerifyNativeCheckpointInventoryFromOpenDevices(database_uuid,devices,previous,budget-result.retained_image_bytes);
      if(!result.predecessor.ok())return CheckpointFailure(result.predecessor);
      const auto& before=result.predecessor;const auto& old=*before.checkpoint;
      if(before.checkpoint_sha256!=selection.previous_checkpoint_sha256||old.checkpoint_generation>=cp.checkpoint_generation||old.root_set_generation>=cp.root_set_generation||
        old.timeline_uuid!=cp.timeline_uuid||before.inventory.next_local_transaction_id>pair.inventory.next_local_transaction_id||
        before.inventory.next_commit_sequence>pair.inventory.next_commit_sequence)return Fail(E::checkpoint_binding_mismatch);
      // Individually valid, fully hashed snapshots can still contradict one
      // another. Apply the same retained-history rule as inventory publication
      // before this selection can become any downstream caller's authority.
      if(*mga::ValidateLocalTransactionInventoryEvolution(before.inventory,pair.inventory))
        return Fail(E::checkpoint_binding_mismatch);
      result.retained_image_bytes+=before.retained_image_bytes;
    }
    const auto target=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==4;});
    if(target==cp.roots.end()||target->page.filespace_uuid!=primary_uuid||target->page.page_size_profile_uuid!=primary->page_size_profile_uuid)return Fail(E::allocation_binding_mismatch);
    const disk::FilespaceRootReference allocation{3,3,primary_uuid,target->page.page_number,target->page.page_generation,target->page.page_size_profile_uuid,target->object_uuid};
    result.allocation=page::ReadNativeAllocationChainAtRootFromOpenDevice(*primary->device,binding,allocation,budget-result.retained_image_bytes);
    if(!result.allocation.ok())return AllocationFailure(result.allocation.error);
    const auto digest=core::hash::ComputeSha256Digest(result.allocation.pages.front().bytes);
    if(!digest.ok())return Fail(E::hash_failure);if(digest.digest!=target->sha256)return Fail(E::allocation_binding_mismatch);
    std::set<Uuid> allocation_ids,page_ids;
    std::set<Uuid> controls{z.page_uuid,cp.header.page_uuid,headers[0].page_uuid,headers[1].page_uuid};
    if(controls.size()!=4)return Fail(E::slot_binding_mismatch);
    for(const auto& image:result.allocation.pages)if(!controls.insert(image.map->header.page_uuid).second)return Fail(E::allocation_binding_mismatch);
    bool matched[2]={false,false};
    for(const auto& image:result.allocation.pages){const auto& map=*image.map;
      const auto creator=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(map.creator_local_transaction_id));
      if(!creator.ok()||creator.entry.identity.transaction_uuid.value!=map.creator_transaction_uuid||
        creator.entry.identity.scope!=mga::TransactionScope::local_node||!mga::HasCommittedInventoryOutcome(creator.entry))return Fail(E::creator_mismatch);
      for(const auto& r:map.records){
        if(!allocation_ids.insert(r.allocation_uuid).second||(!r.page_uuid.is_nil()&&!page_ids.insert(r.page_uuid).second))return Fail(E::allocation_binding_mismatch);
        const auto owner=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(r.creator_local_transaction_id));
        if(!owner.ok()||owner.entry.identity.transaction_uuid.value!=r.creator_transaction_uuid||owner.entry.identity.scope!=mga::TransactionScope::local_node)return Fail(E::creator_mismatch);
        for(unsigned i=0;i<2;++i)if(r.page_number==headers[i].page_number){
          if(map.states[r.page_number-map.first_page]!=page::NativeAllocationState::allocated||r.page_uuid!=headers[i].page_uuid||
            r.page_generation!=headers[i].page_generation||r.page_type!=0x30e||r.owner_uuid!=selection.object_uuid||!mga::HasCommittedInventoryOutcome(owner.entry))return Fail(E::allocation_binding_mismatch);
          matched[i]=true;
        }
      }
    }
    if(!matched[0]||!matched[1])return Fail(E::allocation_binding_mismatch);
    result.retained_image_bytes+=result.allocation.retained_image_bytes;
    // MGA-NATIVE-SELECTED-CONTROL-ALLOCATION-001. Image integrity and a
    // committed checkpoint creator do not prove its physical allocation.
    std::vector<NativeInventoryPageBinding> required{{cp.header,cp.object_uuid}};
    required.insert(required.end(),pair.inventory_pages.begin(),pair.inventory_pages.end());
    if(result.predecessor.ok()){
      const auto& old=*result.predecessor.checkpoint;required.push_back({old.header,old.object_uuid});
      required.insert(required.end(),result.predecessor.inventory_pages.begin(),result.predecessor.inventory_pages.end());
    }
    for(const auto& file:devices){
      if(std::none_of(required.begin(),required.end(),[&](const auto& r){return r.header.filespace_uuid==file.filespace_uuid;}))continue;
      page::NativeAllocationChainResult secondary;
      const auto* maps=&result.allocation;
      if(file.filespace_uuid!=primary_uuid){
        secondary=page::ReadNativeAllocationChainFromOpenDevice(*file.device,
          {database_uuid,file.filespace_uuid,file.page_size_profile_uuid},budget-result.retained_image_bytes);
        if(!secondary.ok())return AllocationFailure(secondary.error);
        maps=&secondary;
        for(const auto& image:maps->pages){const auto& map=*image.map;
          const auto creator=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(map.creator_local_transaction_id));
          if(!creator.ok()||creator.entry.identity.transaction_uuid.value!=map.creator_transaction_uuid||
            creator.entry.identity.scope!=mga::TransactionScope::local_node||!mga::HasCommittedInventoryOutcome(creator.entry))return Fail(E::creator_mismatch);
          for(const auto& record:map.records){
            if(!allocation_ids.insert(record.allocation_uuid).second||(!record.page_uuid.is_nil()&&!page_ids.insert(record.page_uuid).second))return Fail(E::allocation_binding_mismatch);
            const auto original=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(record.creator_local_transaction_id));
            if(!original.ok()||original.entry.identity.transaction_uuid.value!=record.creator_transaction_uuid||
              original.entry.identity.scope!=mga::TransactionScope::local_node)return Fail(E::creator_mismatch);
          }
        }
      }
      for(const auto& control:required){const auto& h=control.header;if(h.filespace_uuid!=file.filespace_uuid)continue;
        const page::NativeAllocationRecord* found=nullptr;
        for(const auto& image:maps->pages){const auto& map=*image.map;
          if(h.page_number<map.first_page||h.page_number-map.first_page>=map.states.size())continue;
          if(map.states[h.page_number-map.first_page]!=page::NativeAllocationState::allocated)return Fail(E::allocation_binding_mismatch);
          const auto record=std::lower_bound(map.records.begin(),map.records.end(),h.page_number,
            [](const auto& r,u64 n){return r.page_number<n;});
          if(record!=map.records.end()&&record->page_number==h.page_number)found=&*record;
          break;
        }
        if(!found||found->page_uuid!=h.page_uuid||found->page_generation!=h.page_generation||
          found->page_type!=h.page_type||found->owner_uuid!=control.object_uuid)return Fail(E::allocation_binding_mismatch);
        const auto original=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(found->creator_local_transaction_id));
        if(!original.ok()||original.entry.identity.transaction_uuid.value!=found->creator_transaction_uuid||
          original.entry.identity.scope!=mga::TransactionScope::local_node||!mga::HasCommittedInventoryOutcome(original.entry))return Fail(E::creator_mismatch);
      }
    }
    result.selection=selection;result.error=E::none;return result;
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::io_failure);}
}
}  // namespace scratchbird::storage::database
