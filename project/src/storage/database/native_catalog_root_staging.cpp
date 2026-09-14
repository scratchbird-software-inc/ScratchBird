// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_reserved_page_staging.hpp"
#include <limits>

namespace scratchbird::storage::database {
namespace {
namespace disk=scratchbird::storage::disk;
namespace page=scratchbird::storage::page;
namespace mga=scratchbird::transaction::mga;
namespace hash=scratchbird::core::hash;
using E=NativeCatalogRootStageError;
NativeCatalogRootStageResult Fail(E error){NativeCatalogRootStageResult r;r.error=error;return r;}
}
NativeCatalogRootStageResult StageNativeCatalogRootSuccessorFromOpenDevices(
    const std::vector<disk::NativeFilespaceDevice>& supplied,
    const disk::FilespaceRootReference& checkpoint,
    const mga::TransactionIdentity& owner,const page::NativeCatalogRoot& root,u64 budget) noexcept {
  try {
    const auto& h=root.header;
    if(root.creator_transaction_uuid!=owner.transaction_uuid.value||root.creator_local_transaction_id!=owner.local_id.value)return Fail(E::creator_mismatch);
    NativeCatalogRootStageResult result;
    auto prepared=detail::PrepareNativeReservedPage<NativeCatalogRootStageResult,E>(
      supplied,checkpoint,owner,h,root.object_uuid,0x1eu,budget,result);
    if(!prepared)return result;
    const auto& authority=prepared->authority;const auto& pair=authority.checkpoint_inventory;
    const auto& cp=*pair.checkpoint;const auto& devices=prepared->devices;
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
    u64 used=prepared->retained_image_bytes;
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
    used+=prior.bytes.size();prepared->retained_image_bytes=used;
    if(2*u64{h.page_size_bytes}>budget-used)return Fail(E::resource_exhausted);
    auto encoded=page::EncodeNativeCatalogRoot(root);if(!encoded.ok()){auto r=Fail(E::root_failure);r.root_error=encoded.error;return r;}

    return detail::WriteNativeReservedPageImage<NativeCatalogRootStageResult,E>(*prepared,encoded.bytes,budget);
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
   catch(...){return Fail(E::io_failure);}
}
} // namespace scratchbird::storage::database
