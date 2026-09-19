// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_publication_recovery.hpp"
#include "native_management_control_authority.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "transaction_inventory_page.hpp"
#include "transaction_inventory_validation.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <mutex>
#include <set>
#include <stdexcept>

namespace scratchbird::storage::database {
namespace {
using E=NativePublicationError;
using Bytes=std::vector<byte>;
namespace mga=transaction::mga;
void Require(bool ok,E error){if(!ok)throw error;}
bool V7(const Uuid& id){return core::uuid::IsEngineIdentityUuid(id);}
bool Nonzero(const std::array<byte,32>& hash){return std::any_of(hash.begin(),hash.end(),[](byte b){return b!=0;});}
auto Self(const disk::NativeCommonPageHeader& h){return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
auto Hash(const Bytes& bytes){const auto hash=core::hash::ComputeSha256Digest(bytes);Require(hash.ok(),E::hash_failure);return hash.digest;}
const NativeCheckpointRootReference& Root(const NativeCheckpointRoot& cp,core::platform::u16 role){
  const auto found=std::find_if(cp.roots.begin(),cp.roots.end(),[&](const auto& r){return r.role==role;});
  Require(found!=cp.roots.end(),E::binding_mismatch);return *found;
}
void WatermarkError(NativePublicationWatermarkError e){if(e==NativePublicationWatermarkError::none)return;
  throw e==NativePublicationWatermarkError::hash_failure?E::hash_failure:e==NativePublicationWatermarkError::resource_exhausted?E::resource_exhausted:e==NativePublicationWatermarkError::repair_required?E::repair_required:E::image_failure;
}
void SelectionBackend(NativeCheckpointSelectionError e){
  if(e==NativeCheckpointSelectionError::hash_failure)throw E::hash_failure;
  if(e==NativeCheckpointSelectionError::resource_exhausted)throw E::resource_exhausted;
}
void Header(const Bytes& bytes,const disk::NativeCommonPageHeader& h){
  const disk::NativeCommonPageHeaderBinding binding{{h.database_uuid,h.filespace_uuid,h.page_size_profile_uuid},h.page_number,h.page_generation,h.page_type,h.page_uuid};
  const auto actual=disk::DecodeNativeCommonPageHeader(bytes.data(),128,&binding);
  if(actual.error==disk::NativeCommonPageHeaderError::resource_exhausted)throw E::resource_exhausted;
  Require(actual.ok()&&actual.header->flags==h.flags&&actual.header->page_size_bytes==h.page_size_bytes,E::binding_mismatch);
}
struct Checkpoint {NativeCheckpointRoot root;std::array<byte,32> sha;};
struct Recovery {
  Uuid database,primary;u64 budget=0,used=0,size=0;
  std::vector<disk::NativeFilespaceDevice> files;
  std::vector<std::unique_lock<std::recursive_mutex>> guards;
  disk::FileDevice* device=nullptr;disk::FilespacePageZero zero;
  void Charge(u64 count,u64 unit=1){Require(unit&&count<=(budget-used)/unit,E::resource_exhausted);used+=count*unit;}
  Bytes Read(u64 page){Require(page&&page<zero.total_pages,E::binding_mismatch);Bytes bytes(size);
    const auto io=device->ReadAt(page*size,bytes.data(),bytes.size());Require(io.ok()&&io.bytes_transferred==bytes.size(),E::io_failure);return bytes;
  }
  Checkpoint ReadCheckpoint(const disk::NativePageReference& ref,const Uuid& object){
    Require(ref.filespace_uuid==primary&&ref.page_size_profile_uuid==zero.bootstrap.page_size_profile_uuid&&ref.page_number<zero.total_pages,E::binding_mismatch);Charge(3,size);
    const disk::FilespaceRootReference root{9,0x300,ref.filespace_uuid,ref.page_number,ref.page_generation,ref.page_size_profile_uuid,object};
    auto result=ReadNativeCheckpointRootFromOpenDevice(*device,database,root);
    if(!result.ok())throw result.error==NativeCheckpointError::hash_failure?E::hash_failure:result.error==NativeCheckpointError::resource_exhausted?E::resource_exhausted:result.error==NativeCheckpointError::io_failure?E::io_failure:result.error==NativeCheckpointError::encrypted_requires_crypto_authority?E::encrypted_requires_authority:E::checkpoint_failure;
    Require(result.root->completed,E::checkpoint_failure);const auto sha=Hash(result.bytes);return {std::move(*result.root),sha};
  }
  page::NativeAllocationChainResult Maps(const NativeCheckpointRoot& cp){
    const auto& ref=Root(cp,4);Require(ref.page_type==3&&ref.page.filespace_uuid==primary&&ref.page.page_size_profile_uuid==zero.bootstrap.page_size_profile_uuid,E::binding_mismatch);
    const disk::FilespaceRootReference root{3,3,ref.page.filespace_uuid,ref.page.page_number,ref.page.page_generation,ref.page.page_size_profile_uuid,ref.object_uuid};
    auto result=page::ReadNativeAllocationChainAtRootFromOpenDevice(*device,{database,primary,zero.bootstrap.page_size_profile_uuid},root,budget-used);
    if(!result.ok())throw result.error==page::NativeAllocationError::hash_failure?E::hash_failure:result.error==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:result.error==page::NativeAllocationError::io_failure?E::io_failure:result.error==page::NativeAllocationError::cluster_requires_authority?E::cluster_requires_authority:E::allocation_mismatch;
    Charge(result.retained_image_bytes);Require(Hash(result.pages.front().bytes)==ref.sha256,E::binding_mismatch);return result;
  }
  page::NativeTransactionInventoryChainResult Inventory(const NativeCheckpointRoot& cp){
    const auto& ref=Root(cp,1);const disk::FilespaceRootReference root{4,ref.page_type,ref.page.filespace_uuid,ref.page.page_number,ref.page.page_generation,ref.page.page_size_profile_uuid,ref.object_uuid};
    auto result=page::ReadNativeTransactionInventoryChainFromOpenDevices(database,files,root,budget-used);
    if(!result.ok())throw result.error==page::NativeInventoryError::hash_failure?E::hash_failure:result.error==page::NativeInventoryError::resource_exhausted?E::resource_exhausted:result.error==page::NativeInventoryError::io_failure?E::io_failure:result.error==page::NativeInventoryError::encrypted_requires_crypto_authority?E::encrypted_requires_authority:E::checkpoint_failure;
    Charge(result.retained_image_bytes);Require(Hash(result.pages.front().bytes)==ref.sha256&&result.inventory.next_local_transaction_id&&result.inventory.next_local_transaction_id-1==cp.selected_local_transaction_id,E::binding_mismatch);return result;
  }
};
const page::NativeAllocationRecord& Allocated(const page::NativeAllocationChainResult& chain,const disk::FilespaceRootReference& root){
  auto image=std::upper_bound(chain.pages.begin(),chain.pages.end(),root.page_number,[](u64 n,const auto& p){return n<p.map->first_page;});Require(image!=chain.pages.begin(),E::allocation_mismatch);--image;const auto& map=*image->map;
  Require(root.page_number-map.first_page<map.states.size()&&map.states[root.page_number-map.first_page]==page::NativeAllocationState::allocated,E::allocation_mismatch);
  const auto record=std::lower_bound(map.records.begin(),map.records.end(),root.page_number,[](const auto& r,u64 n){return r.page_number<n;});
  Require(record!=map.records.end()&&record->page_number==root.page_number&&record->page_type==root.page_type&&record->page_generation==root.page_generation&&record->owner_uuid==root.object_uuid,E::allocation_mismatch);return *record;
}
bool OldSelection(const NativeCheckpointSelection& old,const NativePublicationPlan& plan,const NativeCheckpointRoot& base){
  if(old.selection_generation!=*plan.base_selection_generation||old.previous_selection_generation!=*plan.base_selection_generation-1||old.publication_uuid==plan.operation_uuid||
    old.checkpoint!=plan.base_checkpoint||old.checkpoint_object_uuid!=plan.base_checkpoint_object_uuid||old.checkpoint_sha256!=plan.base_checkpoint_sha256||old.checkpoint_generation!=plan.base_checkpoint_generation||old.root_set_generation!=plan.base_root_set_generation||old.timeline_uuid!=plan.timeline_uuid)return false;
  if(!base.creator_operation_uuid.is_nil()&&old.publication_uuid!=base.creator_operation_uuid)return false;
  if(*plan.base_selection_generation==1)return !old.previous_checkpoint&&old.previous_checkpoint_object_uuid.is_nil()&&!Nonzero(old.previous_checkpoint_sha256);
  return old.previous_checkpoint==base.predecessor&&old.previous_checkpoint_object_uuid==base.object_uuid&&old.previous_checkpoint_sha256==base.predecessor_sha256;
}
} // namespace

NativePublicationInspection RecoverNativeManagementCheckpointPublicationOnOpenDevices(
  const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& supplied,const Uuid& primary,
  const Uuid& attempt,const NativePublicationIntent& intent,u64 budget) noexcept {
  try {
    Require(V7(database)&&V7(primary)&&V7(attempt)&&V7(intent.initiator_uuid)&&V7(intent.request_context_uuid)&&V7(intent.policy_snapshot_uuid)&&intent.initiator_kind>=1&&intent.initiator_kind<=8&&Nonzero(intent.normalized_request_sha256)&&!supplied.empty(),E::invalid_request);
    Recovery c;c.database=database;c.primary=primary;c.budget=budget;c.files=supplied;
    std::sort(c.files.begin(),c.files.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    std::set<disk::FileDevice*> handles;c.guards.reserve(c.files.size());
    for(std::size_t i=0;i<c.files.size();++i){const auto& file=c.files[i];Require(V7(file.filespace_uuid)&&disk::FindCanonicalFilespacePageProfile(file.page_size_profile_uuid)&&file.device&&handles.insert(file.device).second&&(!i||c.files[i-1].filespace_uuid!=file.filespace_uuid),E::invalid_device);
      c.guards.push_back(file.device->AcquireOperationGuard());Require(file.device->is_open(),E::invalid_device);if(file.filespace_uuid==primary){c.device=file.device;c.size=disk::FindCanonicalFilespacePageProfile(file.page_size_profile_uuid)->page_size_bytes;}}
    Require(c.device&&!c.device->read_only(),E::invalid_device);c.Charge(38,c.size);
    for(const auto& file:c.files){c.Charge(2,disk::FindCanonicalFilespacePageProfile(file.page_size_profile_uuid)->page_size_bytes);
      const disk::FilespaceBootstrapBinding binding{database,file.filespace_uuid,file.page_size_profile_uuid};auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*file.device,&binding);
      if(!zero.ok())throw zero.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:zero.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:zero.error==disk::FilespacePageZeroError::io_failure?E::io_failure:E::bootstrap_failure;
      Require(!(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required),E::cluster_requires_authority);Require(!(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted),E::encrypted_requires_authority);
      if(file.filespace_uuid==primary)c.zero=std::move(*zero.record);
    }
    Require(c.zero.bootstrap.filespace_role<=4,E::invalid_device);
    std::array<disk::FilespaceRootReference,4> roots;std::array<Bytes,4> original;
    for(unsigned i=0;i<4;++i){const auto root=std::find_if(c.zero.roots.begin(),c.zero.roots.end(),[&](const auto& r){return r.kind==18+i;});Require(root!=c.zero.roots.end()&&root->filespace_uuid==primary&&root->page_size_profile_uuid==c.zero.bootstrap.page_size_profile_uuid,E::bootstrap_failure);roots[i]=*root;original[i]=c.Read(root->page_number);}
    auto watermark=ClassifyNativePublicationWatermarkPair(original[2],original[3]);WatermarkError(watermark.error);const auto& w=*watermark.state;
    Require(w.header.database_uuid==database&&w.header.filespace_uuid==primary&&w.header.page_size_profile_uuid==c.zero.bootstrap.page_size_profile_uuid&&w.bootstrap_uuid==c.zero.page_uuid&&w.object_uuid==roots[2].object_uuid&&w.object_uuid==roots[3].object_uuid,E::binding_mismatch);
    Require(w.intent&&*w.intent==intent&&w.operation_uuid==attempt,E::request_mismatch);Require(w.publication_plan.has_value(),E::invalid_request);const auto& anchor=*w.publication_plan;
    Require(anchor.page.filespace_uuid==primary&&anchor.page.page_size_profile_uuid==c.zero.bootstrap.page_size_profile_uuid,E::binding_mismatch);
    auto plan_image=DecodeNativePublicationPlan(c.Read(anchor.page.page_number));
    if(!plan_image.ok())throw plan_image.error==NativePublicationPlanError::hash_failure?E::hash_failure:plan_image.error==NativePublicationPlanError::resource_exhausted?E::resource_exhausted:E::binding_mismatch;
    const auto& p=*plan_image.plan;
    Require(p.base_selection_generation&&Self(p.header)==anchor.page&&p.object_uuid==anchor.object_uuid&&plan_image.sha256==anchor.sha256&&p.header.database_uuid==database&&p.bootstrap_uuid==c.zero.page_uuid&&p.timeline_uuid==w.timeline_uuid&&p.operation_uuid==attempt&&p.intent==intent&&p.reservation_state_sha256==anchor.reservation_state_sha256&&p.reserved_generation==w.watermark&&p.base_checkpoint==w.base_checkpoint&&p.base_checkpoint_object_uuid==w.base_checkpoint_object_uuid&&p.base_checkpoint_sha256==w.base_checkpoint_sha256&&p.base_checkpoint_generation==w.base_checkpoint_generation&&p.base_root_set_generation==w.base_root_set_generation,E::binding_mismatch);
    const auto base=c.ReadCheckpoint(p.base_checkpoint,p.base_checkpoint_object_uuid);const auto target=c.ReadCheckpoint(p.target_checkpoint,p.target_checkpoint_object_uuid);
    Require(base.sha==p.base_checkpoint_sha256&&base.root.checkpoint_generation==p.base_checkpoint_generation&&base.root.root_set_generation==p.base_root_set_generation&&base.root.timeline_uuid==p.timeline_uuid&&((*p.base_selection_generation==1)==!base.root.predecessor),E::binding_mismatch);
    const auto& head=Root(target.root,16);Require(head.page_type==0x500&&head.page==anchor.page&&head.object_uuid==anchor.object_uuid&&head.sha256==anchor.sha256,E::binding_mismatch);
    const NativeManagementCheckpointAnchor graph_anchor{p.target_checkpoint,p.target_checkpoint_object_uuid,target.sha,p.reserved_generation,p.target_root_set_generation,p.timeline_uuid};
    const auto graph=ReadNativeManagementControlGraphFromOpenDevices(database,c.files,primary,graph_anchor,budget-c.used);
    if(!graph.ok()){using G=NativeManagementControlAuthorityError;throw graph.error==G::hash_failure?E::hash_failure:graph.error==G::resource_exhausted?E::resource_exhausted:graph.error==G::io_failure?E::io_failure:graph.error==G::encrypted_requires_authority?E::encrypted_requires_authority:graph.error==G::cluster_requires_authority?E::cluster_requires_authority:E::allocation_mismatch;}
    c.Charge(graph.verified_image_bytes);Require(MatchesNativeManagementPublishedCheckpoint(graph,target.root,target.sha),E::binding_mismatch);
    const auto before=c.Maps(base.root),after=c.Maps(target.root);const auto inventory=c.Inventory(target.root);
    std::array<disk::NativeCommonPageHeader,4> headers;
    for(unsigned i=0;i<4;++i){const auto& old=Allocated(before,roots[i]);const auto& record=Allocated(after,roots[i]);
      Require(old==record&&record.creator_operation_uuid.is_nil(),E::allocation_mismatch);
      const auto creator=mga::LookupLocalTransaction(inventory.inventory,mga::MakeLocalTransactionId(record.creator_local_transaction_id));
      Require(creator.ok()&&creator.entry.identity.transaction_uuid.value==record.creator_transaction_uuid&&creator.entry.identity.scope==mga::TransactionScope::local_node&&mga::HasCommittedInventoryOutcome(creator.entry),E::allocation_mismatch);
      headers[i]={c.zero.bootstrap.page_size_bytes,roots[i].page_type,database,primary,record.page_uuid,roots[i].page_number,roots[i].page_generation,0,c.zero.bootstrap.page_size_profile_uuid};
      if(i>=2)Header(original[i],headers[i]);
    }
    std::array<Bytes,2> images;std::array<unsigned,2> classification{};
    for(unsigned i=0;i<2;++i){NativeCheckpointSelection next;next.header=headers[i];next.object_uuid=roots[i].object_uuid;next.bootstrap_uuid=c.zero.page_uuid;next.publication_uuid=attempt;next.selection_generation=*p.base_selection_generation+1;next.previous_selection_generation=*p.base_selection_generation;
      next.checkpoint=p.target_checkpoint;next.checkpoint_object_uuid=p.target_checkpoint_object_uuid;next.checkpoint_sha256=target.sha;next.checkpoint_generation=p.reserved_generation;next.root_set_generation=p.target_root_set_generation;next.timeline_uuid=p.timeline_uuid;
      next.previous_checkpoint=p.base_checkpoint;next.previous_checkpoint_object_uuid=p.base_checkpoint_object_uuid;next.previous_checkpoint_sha256=p.base_checkpoint_sha256;
      auto encoded=EncodeNativeCheckpointSelection(next);SelectionBackend(encoded.error);Require(encoded.ok(),E::image_failure);images[i]=std::move(encoded.bytes);
      // A valid common header retains its allocation identity even if the
      // family seal/payload is damaged. Do not overwrite a contradictory
      // valid header by calling the whole page merely "damaged".
      const auto common=disk::DecodeNativeCommonPageHeader(original[i].data(),128);
      if(common.error==disk::NativeCommonPageHeaderError::resource_exhausted)throw E::resource_exhausted;
      if(common.ok())Header(original[i],headers[i]);
      const auto actual=DecodeNativeCheckpointSelection(original[i]);SelectionBackend(actual.error);
      if(actual.ok()){Header(original[i],headers[i]);Require(actual.selection->object_uuid==roots[i].object_uuid&&actual.selection->bootstrap_uuid==c.zero.page_uuid,E::binding_mismatch);
        if(original[i]==images[i])classification[i]=2;else{Require(OldSelection(*actual.selection,p,base.root),E::binding_mismatch);classification[i]=1;}}
    }
    Require(classification[0]||classification[1],E::image_failure);
    if(classification[0]&&classification[1]){
      Require(!(classification[0]==1&&classification[1]==2),E::image_failure);
      const auto pair=ClassifyNativeCheckpointSelectionPair(original[0],original[1]);SelectionBackend(pair.error);
      Require(classification[0]==classification[1]?pair.ok():pair.error==NativeCheckpointSelectionError::repair_required,E::image_failure);
    }
    Bytes scratch(c.size);
    const auto verify=[&](unsigned i,const Bytes& expected,E mismatch){const auto io=c.device->ReadAt(roots[i].page_number*c.size,scratch.data(),scratch.size());Require(io.ok()&&io.bytes_transferred==scratch.size(),E::io_failure);Require(scratch==expected,mismatch);};
    for(unsigned i=0;i<4;++i)verify(i,original[i],E::preimage_changed);
    for(const auto& file:c.files)if(!file.device->read_only())Require(file.device->Sync().ok(),E::io_failure);
    for(unsigned i=0;i<2;++i){if(images[i]!=original[i]){const auto io=c.device->WriteAt(roots[i].page_number*c.size,images[i].data(),images[i].size());Require(io.ok()&&io.bytes_transferred==images[i].size(),E::io_failure);}
      Require(c.device->Sync().ok(),E::io_failure);verify(i,images[i],E::readback_mismatch);}
    auto final=InspectNativePublicationGenerationOnOpenDevices(database,c.files,primary,budget-c.used+6*c.size);if(!final.ok())return final;
    Require(final.snapshot->state_sha256==watermark.state_sha256&&final.snapshot->watermark.publication_plan==w.publication_plan,E::binding_mismatch);
    const auto selected=EncodeNativeCheckpointSelection(final.snapshot->selection);SelectionBackend(selected.error);Require(selected.ok()&&selected.bytes==images[0],E::binding_mismatch);return final;
  }catch(E e){return {e,{}};}catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}catch(const std::length_error&){return {E::resource_exhausted,{}};}catch(...){return {E::io_failure,{}};}
}
} // namespace scratchbird::storage::database
