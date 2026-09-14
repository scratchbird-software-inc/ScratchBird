// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "physical_mga_cow_store.hpp"
#include "hash_digest_parts.hpp"
#include "disk_device.hpp"
#include <algorithm>
#include <mutex>
#include <limits>
#include <set>

namespace scratchbird::storage::database {
namespace {
namespace disk=scratchbird::storage::disk;
namespace page=scratchbird::storage::page;
namespace mga=scratchbird::transaction::mga;
namespace hash=scratchbird::core::hash;
using Uuid=scratchbird::core::platform::Uuid;
using byte=scratchbird::core::platform::byte;
using E=NativeCatalogRootStageError;
bool V7(const Uuid& id){return !id.is_nil()&&(id.bytes[6]>>4)==7&&(id.bytes[8]&0xc0)==0x80;}
NativeCatalogRootStageResult Fail(E error){NativeCatalogRootStageResult r;r.error=error;return r;}
}

NativeCatalogRootStageResult StageNativeCatalogRootSuccessorFromOpenDevices(
    const std::vector<disk::NativeFilespaceDevice>& supplied,
    const disk::FilespaceRootReference& checkpoint,
    const mga::TransactionIdentity& owner,const page::NativeCatalogRoot& root,u64 budget) noexcept {
  try {
    const auto& h=root.header;
    if(root.creator_transaction_uuid!=owner.transaction_uuid.value||root.creator_local_transaction_id!=owner.local_id.value)return Fail(E::creator_mismatch);
    if(supplied.empty()||!budget||!V7(h.database_uuid)||!V7(owner.transaction_uuid.value)||owner.transaction_uuid.kind!=scratchbird::core::platform::UuidKind::transaction||
        !owner.local_id.value||owner.scope!=mga::TransactionScope::local_node)return Fail(E::invalid_request);
    if(h.flags)return Fail(E::header_requires_authority);
    auto devices=supplied;std::sort(devices.begin(),devices.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    std::set<disk::FileDevice*> handles;
    std::vector<std::unique_lock<std::recursive_mutex>> guards;guards.reserve(devices.size());
    for(std::size_t i=0;i<devices.size();++i){const auto& file=devices[i];
      if(!file.device||!V7(file.filespace_uuid)||!handles.insert(file.device).second||
          (i&&devices[i-1].filespace_uuid==file.filespace_uuid))return Fail(E::invalid_request);
      guards.push_back(file.device->AcquireOperationGuard());
    }
    const auto target=std::lower_bound(devices.begin(),devices.end(),h.filespace_uuid,[](const auto& f,const auto& id){return f.filespace_uuid<id;});
    if(target==devices.end()||target->filespace_uuid!=h.filespace_uuid||target->page_size_profile_uuid!=h.page_size_profile_uuid||target->device->read_only())return Fail(E::invalid_destination);
    auto authority=VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(h.database_uuid,devices,checkpoint,budget);
    if(!authority.ok()){auto r=Fail(E::checkpoint_failure);r.checkpoint_error=authority.error;r.directory_error=authority.directory_error;r.inventory_error=authority.checkpoint_inventory.inventory_error;return r;}
    const auto& pair=authority.checkpoint_inventory;const auto& cp=*pair.checkpoint;
    if(cp.flags&4)return Fail(E::cluster_requires_authority);
    bool member=false;
    for(const auto& image:authority.directory.pages)for(const auto& entry:image.directory->records)
      if(entry.bootstrap.filespace_uuid==h.filespace_uuid){member=entry.bootstrap.filespace_role<=4&&entry.bootstrap.page_size_bytes==h.page_size_bytes&&h.page_number<entry.total_pages;}
    if(!member)return Fail(E::invalid_destination);
    if(h.page_uuid==cp.header.page_uuid)return Fail(E::reservation_mismatch);
    for(const auto& image:authority.directory.pages){if(h.page_uuid==image.directory->header.page_uuid)return Fail(E::reservation_mismatch);
      for(const auto& entry:image.directory->records)if(h.page_uuid==entry.page_zero_uuid)return Fail(E::reservation_mismatch);}
    const auto actor=mga::LookupLocalTransaction(pair.inventory,owner.local_id);
    if(!actor.ok()||actor.entry.identity.transaction_uuid.value!=owner.transaction_uuid.value||actor.entry.identity.transaction_uuid.kind!=owner.transaction_uuid.kind||actor.entry.identity.scope!=owner.scope)return Fail(E::creator_mismatch);
    if(actor.entry.state!=mga::TransactionState::active)return Fail(E::creator_not_active);
    if(actor.entry.rollback_only)return Fail(E::creator_rollback_only);
    const disk::FilespaceBootstrapBinding target_binding{h.database_uuid,h.filespace_uuid,h.page_size_profile_uuid};
    const auto target_zero=disk::ReadFilespacePageZeroFromOpenDevice(*target->device,&target_binding);
    if(!target_zero.ok())return Fail(E::invalid_destination);
    if(target_zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted)return Fail(E::header_requires_authority);
    const bool selected_primary=std::any_of(target_zero.record->roots.begin(),target_zero.record->roots.end(),[](const auto& r){return r.kind==18;});
    const auto selected_root=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==4;});
    if(selected_primary&&(selected_root==cp.roots.end()||selected_root->page.filespace_uuid!=h.filespace_uuid))return Fail(E::root_mismatch);
    page::NativeAllocationChainResult allocation;
    if(selected_primary){const auto& r=*selected_root;const disk::FilespaceRootReference ref{3,r.page_type,r.page.filespace_uuid,r.page.page_number,r.page.page_generation,r.page.page_size_profile_uuid,r.object_uuid};
      allocation=page::ReadNativeAllocationChainAtRootFromOpenDevice(*target->device,target_binding,ref,budget-authority.retained_image_bytes);
    }else allocation=page::ReadNativeAllocationChainFromOpenDevice(*target->device,target_binding,budget-authority.retained_image_bytes);
    if(!allocation.ok()){auto r=Fail(E::allocation_failure);r.allocation_error=allocation.error;return r;}
    if(selected_primary||h.filespace_uuid==checkpoint.filespace_uuid){const auto& map=*allocation.pages.front().map;
      const auto root=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& value){return value.role==4;});
      const disk::NativePageReference ref{h.filespace_uuid,map.header.page_number,map.header.page_generation,map.header.page_size_profile_uuid};
      if(root==cp.roots.end()||root->page!=ref||root->object_uuid!=map.object_uuid)return Fail(E::root_mismatch);
      const auto digest=hash::ComputeSha256Digest(allocation.pages.front().bytes);if(!digest.ok())return Fail(E::hash_failure);
      if(digest.digest!=root->sha256)return Fail(E::root_mismatch);
    }
    const page::NativeAllocationRecord* reservation=nullptr;
    std::set<Uuid> allocation_ids,page_ids;
    for(const auto& image:allocation.pages){const auto& map=*image.map;
      const auto creator=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(map.creator_local_transaction_id));
      if(!creator.ok()||creator.entry.identity.transaction_uuid.value!=map.creator_transaction_uuid||creator.entry.identity.scope!=mga::TransactionScope::local_node)return Fail(E::map_creator_mismatch);
      if(!mga::HasCommittedInventoryOutcome(creator.entry))return Fail(E::map_creator_not_committed);
      for(const auto& record:map.records){
        if(!allocation_ids.insert(record.allocation_uuid).second||(!record.page_uuid.is_nil()&&!page_ids.insert(record.page_uuid).second))return Fail(E::reservation_mismatch);
        const auto original=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(record.creator_local_transaction_id));
        if(!original.ok()||original.entry.identity.transaction_uuid.value!=record.creator_transaction_uuid||original.entry.identity.scope!=mga::TransactionScope::local_node)return Fail(E::map_creator_mismatch);
        if(record.page_number==h.page_number&&map.states[record.page_number-map.first_page]==page::NativeAllocationState::reserved)reservation=&record;
      }
    }
    if(!reservation||reservation->page_uuid!=h.page_uuid||reservation->page_generation!=h.page_generation||reservation->page_type!=h.page_type||
        reservation->owner_uuid!=root.object_uuid||reservation->creator_transaction_uuid!=owner.transaction_uuid.value||reservation->creator_local_transaction_id!=owner.local_id.value)return Fail(E::reservation_mismatch);
    const u16 selected_role=root.root_kind==2?5:root.root_kind==6?6:root.root_kind==7?7:root.root_kind==8?9:0;
    if(!selected_role)return Fail(E::invalid_request);
    const auto previous=std::find_if(cp.roots.begin(),cp.roots.end(),[&](const auto& r){return r.role==selected_role;});
    if(previous==cp.roots.end()||previous->object_uuid!=root.object_uuid||previous->page_type!=h.page_type||
        !root.predecessor||*root.predecessor!=previous->page)return Fail(E::predecessor_mismatch);
    for(const auto& image:authority.directory.pages)for(const auto& entry:image.directory->records)
      if(entry.bootstrap.filespace_uuid==previous->page.filespace_uuid){
        if(entry.bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required)return Fail(E::cluster_requires_authority);
        if(entry.bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted)return Fail(E::header_requires_authority);
      }
    const auto prior_file=std::lower_bound(devices.begin(),devices.end(),previous->page.filespace_uuid,[](const auto& f,const auto& id){return f.filespace_uuid<id;});
    if(prior_file==devices.end()||prior_file->filespace_uuid!=previous->page.filespace_uuid||
        prior_file->page_size_profile_uuid!=previous->page.page_size_profile_uuid)return Fail(E::invalid_destination);
    const auto* prior_profile=disk::FindCanonicalFilespacePageProfile(previous->page.page_size_profile_uuid);
    u64 used=authority.retained_image_bytes+allocation.retained_image_bytes;
    if(!prior_profile||prior_profile->page_size_bytes>budget-used)return Fail(E::resource_exhausted);
    const disk::FilespaceRootReference prior_ref{root.root_kind,previous->page_type,previous->page.filespace_uuid,
      previous->page.page_number,previous->page.page_generation,previous->page.page_size_profile_uuid,previous->object_uuid};
    auto prior=page::ReadNativeCatalogRootFromOpenDevice(*prior_file->device,h.database_uuid,prior_ref);
    if(!prior.ok()){auto r=Fail(E::predecessor_failure);r.root_error=prior.error;return r;}
    const auto prior_digest=hash::ComputeSha256Digest(prior.bytes);if(!prior_digest.ok())return Fail(E::hash_failure);
    if(prior_digest.digest!=previous->sha256||prior_digest.digest!=root.predecessor_sha256)return Fail(E::predecessor_mismatch);
    const auto& predecessor=*prior.root;
    if(predecessor.header.flags)return Fail(E::header_requires_authority);
    if(predecessor.root_kind!=root.root_kind||predecessor.header.page_uuid==h.page_uuid)return Fail(E::predecessor_mismatch);
    if(predecessor.catalog_generation==std::numeric_limits<u64>::max()||root.catalog_generation!=predecessor.catalog_generation+1||
        root.schema_epoch<predecessor.schema_epoch||root.security_epoch<predecessor.security_epoch||root.resource_epoch<predecessor.resource_epoch)return Fail(E::predecessor_mismatch);
    const auto original=mga::LookupLocalTransaction(pair.inventory,mga::MakeLocalTransactionId(predecessor.creator_local_transaction_id));
    if(!original.ok()||original.entry.identity.transaction_uuid.value!=predecessor.creator_transaction_uuid||
        original.entry.identity.scope!=mga::TransactionScope::local_node||!mga::HasCommittedInventoryOutcome(original.entry))return Fail(E::predecessor_creator_mismatch);
    used+=prior.bytes.size();
    if(2*u64{h.page_size_bytes}>budget-used)return Fail(E::resource_exhausted);
    auto encoded=page::EncodeNativeCatalogRoot(root);if(!encoded.ok()){auto r=Fail(E::root_failure);r.root_error=encoded.error;return r;}
    const auto digest=hash::ComputeSha256Digest(encoded.bytes);if(!digest.ok())return Fail(E::hash_failure);
    NativeCatalogRootStageReceipt receipt{h.database_uuid,reservation->allocation_uuid,h.page_uuid,root.object_uuid,owner,
      {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid},digest.digest};
    std::vector<byte> scratch(h.page_size_bytes);const u64 offset=h.page_number*u64{h.page_size_bytes};
    const auto before=target->device->ReadAt(offset,scratch.data(),scratch.size());
    if(!before.ok()||before.bytes_transferred!=scratch.size())return Fail(E::io_failure);
    if(scratch!=encoded.bytes){if(!std::all_of(scratch.begin(),scratch.end(),[](byte b){return b==0;}))return Fail(E::destination_not_empty);
      const auto write=target->device->WriteAt(offset,encoded.bytes.data(),encoded.bytes.size());
      if(!write.ok()||write.bytes_transferred!=encoded.bytes.size())return Fail(E::io_failure);
    }
    if(!target->device->Sync().ok())return Fail(E::io_failure);
    const auto after=target->device->ReadAt(offset,scratch.data(),scratch.size());
    if(!after.ok()||after.bytes_transferred!=scratch.size())return Fail(E::io_failure);
    if(scratch!=encoded.bytes)return Fail(E::readback_mismatch);
    NativeCatalogRootStageResult result;result.error=E::none;result.receipt=receipt;return result;
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
   catch(...){return Fail(E::io_failure);}
}
}  // namespace scratchbird::storage::database
