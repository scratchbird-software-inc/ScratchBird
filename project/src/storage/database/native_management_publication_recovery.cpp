// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_publication_recovery.hpp"
#include "native_management_control_authority.hpp"
#include "native_management_control_allocation.hpp"
#include "native_storage_action_intent.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "transaction_inventory_page.hpp"
#include "transaction_inventory_validation.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <mutex>
#include <limits>
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
  page::NativeAllocationChainResult Maps(const NativeCheckpointRoot& cp,const Bytes* historical_zero=nullptr){
    const auto& ref=Root(cp,4);Require(ref.page_type==3&&ref.page.filespace_uuid==primary&&ref.page.page_size_profile_uuid==zero.bootstrap.page_size_profile_uuid,E::binding_mismatch);
    const disk::FilespaceRootReference root{3,3,ref.page.filespace_uuid,ref.page.page_number,ref.page.page_generation,ref.page.page_size_profile_uuid,ref.object_uuid};
    auto result=historical_zero?page::ReadNativeAllocationChainAtHistoricalRootFromOpenDevice(*device,
      {database,primary,zero.bootstrap.page_size_profile_uuid},root,ref.sha256,*historical_zero,budget-used):
      page::ReadNativeAllocationChainAtRootFromOpenDevice(*device,{database,primary,zero.bootstrap.page_size_profile_uuid},root,budget-used);
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
  NativePublicationEffects effects{true};
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
    Require(!w.abandonment,E::invalid_request);
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
    effects.installed_graph_verified=true;
    Bytes original_primary_zero;
    if(p.intent.recovery_profile==4){
      // This selector primitive requires completed physical growth and an exact
      // current after body. It neither extends nor repairs page-zero metadata.
      const auto history=ReadNativeManagementGraphHistoryFromOpenDevices(database,c.files,primary,graph_anchor,budget-c.used);
      if(!history.ok()){using H=NativeManagementHistoryError;throw history.error==H::hash_failure?E::hash_failure:
        history.error==H::resource_exhausted?E::resource_exhausted:history.error==H::io_failure?E::io_failure:E::binding_mismatch;}
      c.Charge(history.verified_image_bytes);
      Require(!history.entries.empty()&&history.entries.back().plan.operation_uuid==attempt,E::binding_mismatch);
      const auto& growth=history.entries.back().control_growth_images;Require(growth.size()==2,E::binding_mismatch);
      c.Charge(growth.front().size(),3);
      const auto before_zero=disk::DecodeFilespacePageZero(growth.front().data(),growth.front().size());
      if(!before_zero.ok())throw before_zero.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:
        before_zero.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:E::binding_mismatch;
      const auto& z=*before_zero.record;const auto file=std::find_if(c.files.begin(),c.files.end(),[&](const auto& f){return f.filespace_uuid==z.bootstrap.filespace_uuid;});
      Require(file!=c.files.end(),E::invalid_device);
      Bytes actual(growth.back().size());const auto read=file->device->ReadAt(0,actual.data(),actual.size());
      Require(read.ok()&&read.bytes_transferred==actual.size(),E::io_failure);
      Require(actual==growth.back(),E::preimage_changed);
      if(z.bootstrap.filespace_uuid==primary)original_primary_zero=growth.front();
    }
    const auto before=c.Maps(base.root,original_primary_zero.empty()?nullptr:&original_primary_zero),after=c.Maps(target.root);const auto inventory=c.Inventory(target.root);
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
    const auto sync=[&](disk::FileDevice& file){Require(effects.sync_attempts!=std::numeric_limits<u64>::max(),E::resource_exhausted);
      ++effects.sync_attempts;Require(file.Sync().ok(),E::io_failure);++effects.successful_syncs;};
    for(const auto& file:c.files)if(!file.device->read_only())sync(*file.device);
    for(unsigned i=0;i<2;++i){if(images[i]!=original[i]){
        Require(effects.write_attempts!=std::numeric_limits<u64>::max()&&images[i].size()<=std::numeric_limits<u64>::max()-effects.attempted_bytes,E::resource_exhausted);
        ++effects.write_attempts;effects.attempted_bytes+=images[i].size();effects.selector_write_attempted=true;
        effects.uncertain_write=true;
        const auto io=c.device->WriteAt(roots[i].page_number*c.size,images[i].data(),images[i].size());
        if(io.bytes_transferred<=images[i].size())effects.confirmed_bytes+=io.bytes_transferred;
        Require(io.ok()&&io.bytes_transferred==images[i].size(),E::io_failure);effects.uncertain_write=false;}
      sync(*c.device);verify(i,images[i],E::readback_mismatch);}
    auto final=InspectNativePublicationGenerationOnOpenDevices(database,c.files,primary,budget-c.used+6*c.size);final.effects=effects;if(!final.ok())return final;
    Require(final.snapshot->state_sha256==watermark.state_sha256&&final.snapshot->watermark.publication_plan==w.publication_plan,E::binding_mismatch);
    const auto selected=EncodeNativeCheckpointSelection(final.snapshot->selection);SelectionBackend(selected.error);Require(selected.ok()&&selected.bytes==images[0],E::binding_mismatch);
    effects.selected_graph_verified=true;final.effects=effects;return final;
  }catch(E e){return {e,{},effects};}catch(const std::bad_alloc&){return {E::resource_exhausted,{},effects};}catch(const std::length_error&){return {E::resource_exhausted,{},effects};}catch(...){return {E::io_failure,{},effects};}
}
NativeGrowthRecoveryResult RecoverNativeFilespaceGrowthOnOpenDevices(
  const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& supplied,const Uuid& primary,
  const Uuid& attempt,const NativeManagementOperation& record,u64 budget) noexcept {
  NativeGrowthRecoveryResult result;result.publication_attempt_uuid=attempt;
  try {
    const auto decoded=ReadNativeStorageActionIntentFromOperation(record,budget);
    if(!decoded.ok())throw decoded.error==NativeStorageIntentError::resource_exhausted?E::resource_exhausted:
      decoded.error==NativeStorageIntentError::hash_failure?E::hash_failure:E::invalid_request;
    const auto& request=*decoded.intent;
    result.request_uuid=request.request_uuid;result.operation_uuid=request.operation_uuid;
    Require(V7(attempt)&&request.database_uuid==database&&request.checkpoint.filespace_uuid==primary&&
      request.action==NativeStorageAction::physical_growth&&!supplied.empty(),E::invalid_request);
    Recovery c;c.database=database;c.primary=primary;c.budget=std::min(budget,request.maximum_retained_image_bytes);c.files=supplied;
    c.Charge(kNativeStorageActionIntentBytes);
    std::sort(c.files.begin(),c.files.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    std::set<disk::FileDevice*> handles;std::map<Uuid,Bytes> actual,contexts;
    std::map<Uuid,u64> sizes;disk::FileDevice* target_device=nullptr;
    for(std::size_t i=0;i<c.files.size();++i){const auto& f=c.files[i];const auto* profile=disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid);
      Require(V7(f.filespace_uuid)&&profile&&f.device&&handles.insert(f.device).second&&(!i||c.files[i-1].filespace_uuid!=f.filespace_uuid),E::invalid_device);
      c.guards.push_back(f.device->AcquireOperationGuard());Require(f.device->is_open(),E::invalid_device);
      c.Charge(12,profile->page_size_bytes);const auto size=f.device->Size();Require(size.ok(),E::io_failure);sizes.emplace(f.filespace_uuid,size.size_bytes);
      Bytes raw(profile->page_size_bytes);const auto io=f.device->ReadAt(0,raw.data(),raw.size());Require(io.ok()&&io.bytes_transferred==raw.size(),E::io_failure);actual.emplace(f.filespace_uuid,std::move(raw));
      if(f.filespace_uuid==primary){c.device=f.device;c.size=profile->page_size_bytes;}
      if(f.filespace_uuid==request.filespace_uuid){target_device=f.device;Require(f.page_size_profile_uuid==request.page_size_profile_uuid,E::binding_mismatch);}
    }
    Require(c.device&&target_device&&!c.device->read_only()&&!target_device->read_only(),E::invalid_device);c.Charge(40,c.size);
    const auto primary_file=std::find_if(c.files.begin(),c.files.end(),[&](const auto& f){return f.filespace_uuid==primary;});
    result.phase=NativeGrowthRecoveryPhase::anchor;
    const auto probe=disk::ProbeFilespaceRecoveryRootCandidatesFromOpenDevice(*c.device,{database,primary,primary_file->page_size_profile_uuid},c.budget-c.used);
    if(!probe.ok())throw probe.error==disk::FilespaceRecoveryRootError::resource_exhausted?E::resource_exhausted:
      probe.error==disk::FilespaceRecoveryRootError::hash_failure?E::hash_failure:probe.error==disk::FilespaceRecoveryRootError::io_failure?E::io_failure:
      probe.error==disk::FilespaceRecoveryRootError::encrypted_requires_authority?E::encrypted_requires_authority:
      probe.error==disk::FilespaceRecoveryRootError::cluster_requires_authority?E::cluster_requires_authority:E::bootstrap_failure;
    c.zero.total_pages=probe.observed_size_bytes/c.size;
    std::array<disk::FilespaceRootReference,4> roots;std::array<Bytes,4> slots;
    for(unsigned i=0;i<4;++i){const auto at=std::find_if(probe.roots.begin(),probe.roots.end(),[&](const auto& r){return r.kind==18+i;});
      Require(at!=probe.roots.end()&&at->filespace_uuid==primary&&at->page_size_profile_uuid==primary_file->page_size_profile_uuid,E::binding_mismatch);roots[i]=*at;slots[i]=c.Read(at->page_number);}
    const auto watermarks=ClassifyNativePublicationWatermarkPair(slots[2],slots[3]);WatermarkError(watermarks.error);const auto& w=*watermarks.state;
    const NativePublicationIntent intent{record.initiator_uuid,record.request_context_uuid,record.policy_snapshot_uuid,record.normalized_request_sha256,record.initiator_kind,4};
    Require(w.intent&&*w.intent==intent&&w.operation_uuid==attempt&&!w.abandonment&&w.publication_plan,E::request_mismatch);
    const auto& anchor=*w.publication_plan;Require(anchor.page.filespace_uuid==primary&&anchor.page.page_size_profile_uuid==primary_file->page_size_profile_uuid,E::binding_mismatch);
    const auto plan_bytes=c.Read(anchor.page.page_number);const auto image=DecodeNativePublicationPlan(plan_bytes);
    if(!image.ok())throw image.error==NativePublicationPlanError::resource_exhausted?E::resource_exhausted:image.error==NativePublicationPlanError::hash_failure?E::hash_failure:E::binding_mismatch;
    const auto& p=*image.plan;
    Require(p.base_selection_generation&&p.management_extent&&p.control_bundle&&p.control_bundle->growth_image_count==2&&
      p.header.database_uuid==database&&Self(p.header)==anchor.page&&image.sha256==anchor.sha256&&p.object_uuid==anchor.object_uuid&&
      p.bootstrap_uuid==w.bootstrap_uuid&&p.timeline_uuid==w.timeline_uuid&&p.operation_uuid==attempt&&p.intent==intent&&
      p.reservation_state_sha256==anchor.reservation_state_sha256&&p.reserved_generation==w.watermark&&
      p.base_checkpoint==w.base_checkpoint&&p.base_checkpoint_object_uuid==w.base_checkpoint_object_uuid&&p.base_checkpoint_sha256==w.base_checkpoint_sha256&&
      p.base_checkpoint_generation==w.base_checkpoint_generation&&p.base_root_set_generation==w.base_root_set_generation,E::binding_mismatch);
    const auto read_run=[&](const auto& root){Require(root.first.filespace_uuid==primary&&root.first.page_size_profile_uuid==primary_file->page_size_profile_uuid&&
        root.first.page_number<c.zero.total_pages&&root.page_count<=c.zero.total_pages-root.first.page_number,E::binding_mismatch);
      c.Charge(root.page_count,4*c.size);std::vector<Bytes> pages;pages.reserve(root.page_count);
      for(u64 i=0;i<root.page_count;++i)pages.push_back(c.Read(root.first.page_number+i));return pages;};
    result.phase=NativeGrowthRecoveryPhase::reconstruction;
    const auto extent=read_run(*p.management_extent),bundle=read_run(*p.control_bundle);
    const auto retained=DecodeNativeManagementExtent(extent,*p.management_extent,database,p.bootstrap_uuid,c.budget-c.used);
    if(!retained.ok())throw retained.error==NativeManagementExtentError::resource_exhausted?E::resource_exhausted:retained.error==NativeManagementExtentError::hash_failure?E::hash_failure:E::binding_mismatch;
    Require(*retained.record==record,E::request_mismatch);
    const auto bound_record=BindNativePublicationPlanToManagementRecord(p,record);
    if(bound_record!=NativePublicationPlanError::none)throw bound_record==NativePublicationPlanError::resource_exhausted?E::resource_exhausted:bound_record==NativePublicationPlanError::hash_failure?E::hash_failure:E::request_mismatch;
    const auto contents=DecodeNativeManagementControlBundle(bundle,*p.control_bundle,database,p.bootstrap_uuid,c.budget-c.used);
    if(!contents.ok())throw contents.error==NativeManagementControlBundleError::resource_exhausted?E::resource_exhausted:contents.error==NativeManagementControlBundleError::hash_failure?E::hash_failure:E::binding_mismatch;
    Require(contents.growth_images.size()==2&&contents.inventory_images.empty()&&!contents.directory_images.empty(),E::binding_mismatch);
    for(const auto& f:c.files){const auto& raw=f.filespace_uuid==request.filespace_uuid?contents.growth_images.front():actual.at(f.filespace_uuid);
      const disk::FilespaceBootstrapBinding binding{database,f.filespace_uuid,f.page_size_profile_uuid};const auto zero=disk::DecodeFilespacePageZero(raw.data(),raw.size(),&binding);
      if(!zero.ok())throw zero.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:zero.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:E::bootstrap_failure;
      Require(!(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required),E::cluster_requires_authority);
      Require(!(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted),E::encrypted_requires_authority);
      const auto bytes=zero.record->total_pages*u64{raw.size()};Require(sizes.at(f.filespace_uuid)>=bytes,E::preimage_changed);
      if(f.filespace_uuid!=request.filespace_uuid)Require(sizes.at(f.filespace_uuid)==bytes,E::preimage_changed);
      const auto& observed=actual.at(f.filespace_uuid);
      Require(raw.size()==observed.size()&&std::equal(raw.begin(),raw.begin()+4096,observed.begin())&&std::equal(raw.begin()+4480,raw.end(),observed.begin()+4480),E::preimage_changed);
      contexts.emplace(f.filespace_uuid,raw);if(f.filespace_uuid==primary)c.zero=*zero.record;
    }
    Require(c.zero.page_uuid==p.bootstrap_uuid&&w.header.database_uuid==database&&w.header.filespace_uuid==primary&&
      w.header.page_size_profile_uuid==primary_file->page_size_profile_uuid&&w.object_uuid==roots[2].object_uuid&&w.object_uuid==roots[3].object_uuid,E::binding_mismatch);
    result.original_extent_bytes=request.current_total_pages*u64{request.page_size_bytes};
    result.target_extent_bytes=(request.current_total_pages+request.page_count)*u64{request.page_size_bytes};result.observed_extent_bytes=sizes.at(request.filespace_uuid);
    Require(result.observed_extent_bytes>=result.original_extent_bytes&&result.observed_extent_bytes<=result.target_extent_bytes,E::preimage_changed);
    const auto observed_body=disk::DecodeFilespacePageZero(actual.at(request.filespace_uuid).data(),request.page_size_bytes);
    if(observed_body.error==disk::FilespacePageZeroError::hash_provider_failure)throw E::hash_failure;
    if(observed_body.error==disk::FilespacePageZeroError::resource_exhausted)throw E::resource_exhausted;
    if(observed_body.ok())Require(actual.at(request.filespace_uuid)==contents.growth_images.front()||actual.at(request.filespace_uuid)==contents.growth_images.back(),E::preimage_changed);
    const auto base_bytes=c.Read(p.base_checkpoint.page_number),target_bytes=c.Read(p.target_checkpoint.page_number);
    const auto base=DecodeNativeCheckpointRoot(base_bytes),target=DecodeNativeCheckpointRoot(target_bytes);
    if(base.error==NativeCheckpointError::hash_failure||target.error==NativeCheckpointError::hash_failure)throw E::hash_failure;
    if(base.error==NativeCheckpointError::resource_exhausted||target.error==NativeCheckpointError::resource_exhausted)throw E::resource_exhausted;
    Require(base.ok()&&target.ok()&&Hash(base_bytes)==p.base_checkpoint_sha256,E::checkpoint_failure);
    const NativeManagementCheckpointAnchor base_anchor{p.base_checkpoint,p.base_checkpoint_object_uuid,p.base_checkpoint_sha256,p.base_checkpoint_generation,p.base_root_set_generation,p.timeline_uuid};
    result.phase=NativeGrowthRecoveryPhase::base_graph;
    const auto graph=ReadNativeManagementControlGraphAtHistoricalContextFromOpenDevices(database,c.files,primary,base_anchor,contexts,c.budget-c.used);
    if(!graph.ok())throw graph.error==NativeManagementControlAuthorityError::resource_exhausted?E::resource_exhausted:
      graph.error==NativeManagementControlAuthorityError::hash_failure?E::hash_failure:graph.error==NativeManagementControlAuthorityError::io_failure?E::io_failure:E::allocation_mismatch;
    c.Charge(graph.verified_image_bytes);
    const auto history=ReadNativeManagementGraphHistoryAtHistoricalContextFromOpenDevices(database,c.files,primary,base_anchor,contexts,c.budget-c.used);
    if(!history.ok())throw history.error==NativeManagementHistoryError::resource_exhausted?E::resource_exhausted:
      history.error==NativeManagementHistoryError::hash_failure?E::hash_failure:history.error==NativeManagementHistoryError::io_failure?E::io_failure:E::binding_mismatch;
    c.Charge(history.verified_image_bytes);
    const auto append=ValidateNativeManagementHistoryAppend(history,record,c.budget-c.used);
    if(append!=NativeManagementHistoryError::none)throw append==NativeManagementHistoryError::resource_exhausted?E::resource_exhausted:
      append==NativeManagementHistoryError::hash_failure?E::hash_failure:E::request_mismatch;
    for(const auto& entry:history.entries)if(entry.plan.intent.recovery_profile==3||entry.plan.intent.recovery_profile==4){
      const auto old=ReadNativeStorageActionIntentFromOperation(entry.record,c.budget-c.used);
      if(!old.ok())throw old.error==NativeStorageIntentError::resource_exhausted?E::resource_exhausted:old.error==NativeStorageIntentError::hash_failure?E::hash_failure:E::binding_mismatch;
      Require(old.intent->request_uuid!=request.request_uuid&&old.intent->operation_uuid!=request.operation_uuid,E::request_mismatch);
    }
    const auto& dr=Root(*base.root,3);const disk::FilespaceRootReference directory_ref{5,dr.page_type,dr.page.filespace_uuid,dr.page.page_number,dr.page.page_generation,dr.page.page_size_profile_uuid,dr.object_uuid};
    std::vector<page::NativeHistoricalFilespaceImage> historical;for(const auto& [fs,raw]:contexts)historical.push_back({fs,raw});
    const auto directory=page::ReadNativeFilespaceDirectoryAtHistoricalRootFromOpenDevices(database,c.files,directory_ref,dr.sha256,historical,c.budget-c.used);
    if(!directory.ok())throw directory.error==page::NativeDirectoryError::resource_exhausted?E::resource_exhausted:directory.error==page::NativeDirectoryError::hash_failure?E::hash_failure:directory.error==page::NativeDirectoryError::io_failure?E::io_failure:E::binding_mismatch;
    std::set<Uuid> directory_members;
    for(const auto& page:directory.pages)for(const auto& member:page.directory->records)
      Require(contexts.contains(member.bootstrap.filespace_uuid)&&directory_members.insert(member.bootstrap.filespace_uuid).second,E::binding_mismatch);
    Require(directory_members.size()==contexts.size(),E::binding_mismatch);
    c.Charge(directory.retained_image_bytes);NativeManagementDirectoryBase original;std::vector<Bytes> maps;std::set<Uuid> changed_members;
    for(const auto& raw:contents.allocation_images){const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);
      if(!h.ok())throw h.error==disk::NativeCommonPageHeaderError::resource_exhausted?E::resource_exhausted:E::binding_mismatch;
      changed_members.insert(h.header->filespace_uuid);}
    Require(changed_members.contains(primary)&&changed_members.contains(request.filespace_uuid),E::binding_mismatch);
    for(const auto& fs:changed_members){const auto at=contexts.find(fs);Require(at!=contexts.end(),E::binding_mismatch);original.page_zero_images.push_back(at->second);}
    for(const auto& page:directory.pages)original.directory_images.push_back(page.bytes);
    const auto primary_maps=c.Maps(*base.root,&contexts.at(primary));for(const auto& page:primary_maps.pages)maps.push_back(page.bytes);
    for(const auto& f:c.files)if(f.filespace_uuid!=primary&&changed_members.contains(f.filespace_uuid)){const page::NativeFilespaceDirectoryRecord* member=nullptr;
      for(const auto& page:directory.pages)for(const auto& m:page.directory->records)if(m.bootstrap.filespace_uuid==f.filespace_uuid)member=&m;
      Require(member,E::binding_mismatch);disk::FilespaceRootReference root;std::array<byte,32> hash{};
      if(member->allocation_root){const auto& r=*member->allocation_root;root={3,3,r.page.filespace_uuid,r.page.page_number,r.page.page_generation,r.page.page_size_profile_uuid,r.object_uuid};hash=r.sha256;}
      else{const auto z=disk::DecodeFilespacePageZero(contexts.at(f.filespace_uuid).data(),contexts.at(f.filespace_uuid).size());
        if(!z.ok())throw z.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:z.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:E::binding_mismatch;
        const auto at=std::find_if(z.record->roots.begin(),z.record->roots.end(),[](const auto& r){return r.kind==3;});Require(at!=z.record->roots.end(),E::binding_mismatch);root=*at;
        c.Charge(z.record->bootstrap.page_size_bytes);Bytes raw(z.record->bootstrap.page_size_bytes);const auto io=f.device->ReadAt(root.page_number*u64{raw.size()},raw.data(),raw.size());Require(io.ok()&&io.bytes_transferred==raw.size(),E::io_failure);hash=Hash(raw);}
      const auto chain=page::ReadNativeAllocationChainAtHistoricalRootFromOpenDevice(*f.device,{database,f.filespace_uuid,f.page_size_profile_uuid},root,hash,contexts.at(f.filespace_uuid),c.budget-c.used);
      if(!chain.ok())throw chain.error==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:chain.error==page::NativeAllocationError::hash_failure?E::hash_failure:chain.error==page::NativeAllocationError::io_failure?E::io_failure:E::allocation_mismatch;
      c.Charge(chain.retained_image_bytes);for(const auto& page:chain.pages)maps.push_back(page.bytes);
    }
    result.phase=NativeGrowthRecoveryPhase::allocation_delta;
    const auto delta=ValidateNativeManagementDirectoryControlAllocation(base_bytes,target_bytes,plan_bytes,extent,maps,contents.allocation_images,original,c.budget-c.used,bundle);
    if(delta!=NativeManagementControlAllocationError::none)throw delta==NativeManagementControlAllocationError::resource_exhausted?E::resource_exhausted:delta==NativeManagementControlAllocationError::hash_failure?E::hash_failure:E::allocation_mismatch;
    const auto& ir=Root(*base.root,1);const disk::FilespaceRootReference inventory_ref{4,ir.page_type,ir.page.filespace_uuid,ir.page.page_number,ir.page.page_generation,ir.page.page_size_profile_uuid,ir.object_uuid};
    const auto inventory=page::ReadNativeTransactionInventoryChainAtHistoricalRootFromOpenDevices(database,c.files,inventory_ref,ir.sha256,contexts,c.budget-c.used);
    if(!inventory.ok())throw inventory.error==page::NativeInventoryError::resource_exhausted?E::resource_exhausted:inventory.error==page::NativeInventoryError::hash_failure?E::hash_failure:inventory.error==page::NativeInventoryError::io_failure?E::io_failure:E::checkpoint_failure;
    c.Charge(inventory.retained_image_bytes);
    const auto transaction=[&](const Uuid& uuid,u64 local,bool committed){const auto tx=mga::LookupLocalTransaction(inventory.inventory,mga::MakeLocalTransactionId(local));
      Require(tx.ok()&&tx.entry.identity.transaction_uuid.value==uuid&&tx.entry.identity.scope==mga::TransactionScope::local_node&&(!committed||mga::HasCommittedInventoryOutcome(tx.entry)),E::allocation_mismatch);};
    for(const auto& raw:maps){const auto image=page::DecodeNativeAllocationMap(raw);
      if(!image.ok())throw image.error==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:image.error==page::NativeAllocationError::hash_failure?E::hash_failure:E::allocation_mismatch;
      const auto& map=*image.map;if(map.creator_operation_uuid.is_nil())transaction(map.creator_transaction_uuid,map.creator_local_transaction_id,true);
      else Require(MatchesNativeManagementControlMap(graph,map),E::allocation_mismatch);
      for(const auto& r:map.records){if(r.creator_operation_uuid.is_nil())transaction(r.creator_transaction_uuid,r.creator_local_transaction_id,false);
        else Require(MatchesNativeManagementControlAllocation(graph,map.header.filespace_uuid,r,map.states[r.page_number-map.first_page]),E::allocation_mismatch);}
    }
    result.phase=NativeGrowthRecoveryPhase::installed_graph;
    const auto installed=[&](const Bytes& expected){const auto h=disk::DecodeNativeCommonPageHeader(expected.data(),128);
      if(!h.ok())throw h.error==disk::NativeCommonPageHeaderError::resource_exhausted?E::resource_exhausted:E::binding_mismatch;
      const auto file=std::find_if(c.files.begin(),c.files.end(),[&](const auto& f){return f.filespace_uuid==h.header->filespace_uuid;});Require(file!=c.files.end(),E::binding_mismatch);
      c.Charge(expected.size());Bytes raw(expected.size());const auto io=file->device->ReadAt(h.header->page_number*u64{raw.size()},raw.data(),raw.size());Require(io.ok()&&io.bytes_transferred==raw.size(),E::io_failure);Require(raw==expected,E::preimage_changed);};
    for(const auto& raw:contents.allocation_images)installed(raw);for(const auto& raw:contents.directory_images)installed(raw);
    // Original slot allocations, and at least one valid old/new selector, bind
    // the mutable recovery locations before admitting physical work.
    std::array<unsigned,2> classification{};
    for(unsigned i=0;i<4;++i){const auto& r=Allocated(primary_maps,roots[i]);Require(r.creator_operation_uuid.is_nil(),E::allocation_mismatch);
      transaction(r.creator_transaction_uuid,r.creator_local_transaction_id,true);
      const disk::NativeCommonPageHeader header{c.zero.bootstrap.page_size_bytes,roots[i].page_type,database,primary,r.page_uuid,roots[i].page_number,roots[i].page_generation,0,c.zero.bootstrap.page_size_profile_uuid};
      const auto common=disk::DecodeNativeCommonPageHeader(slots[i].data(),128);if(common.error==disk::NativeCommonPageHeaderError::resource_exhausted)throw E::resource_exhausted;
      if(i>=2||common.ok())Header(slots[i],header);if(i>=2)continue;
      const auto slot=DecodeNativeCheckpointSelection(slots[i]);SelectionBackend(slot.error);if(!slot.ok())continue;
      Require(slot.selection->object_uuid==roots[i].object_uuid&&slot.selection->bootstrap_uuid==c.zero.page_uuid,E::binding_mismatch);
      if(OldSelection(*slot.selection,p,*base.root))classification[i]=1;
      else{const auto& s=*slot.selection;Require(s.selection_generation==*p.base_selection_generation+1&&s.previous_selection_generation==*p.base_selection_generation&&
        s.publication_uuid==attempt&&s.checkpoint==p.target_checkpoint&&s.checkpoint_object_uuid==p.target_checkpoint_object_uuid&&s.checkpoint_sha256==Hash(target_bytes)&&
        s.checkpoint_generation==p.reserved_generation&&s.root_set_generation==p.target_root_set_generation&&s.timeline_uuid==p.timeline_uuid&&
        s.previous_checkpoint==p.base_checkpoint&&s.previous_checkpoint_object_uuid==p.base_checkpoint_object_uuid&&s.previous_checkpoint_sha256==p.base_checkpoint_sha256,E::binding_mismatch);classification[i]=2;}
    }
    Require((classification[0]||classification[1])&&!(classification[0]==1&&classification[1]==2),E::image_failure);
    if(classification[0]&&classification[1]){const auto pair=ClassifyNativeCheckpointSelectionPair(slots[0],slots[1]);SelectionBackend(pair.error);Require(classification[0]==classification[1]?pair.ok():pair.error==NativeCheckpointSelectionError::repair_required,E::image_failure);}
    for(const auto& f:c.files){const auto size=f.device->Size();Require(size.ok(),E::io_failure);Require(size.size_bytes==sizes.at(f.filespace_uuid),E::preimage_changed);
      Bytes raw(actual.at(f.filespace_uuid).size());const auto io=f.device->ReadAt(0,raw.data(),raw.size());Require(io.ok()&&io.bytes_transferred==raw.size(),E::io_failure);Require(raw==actual.at(f.filespace_uuid),E::preimage_changed);}
    for(unsigned i=0;i<4;++i)Require(c.Read(roots[i].page_number)==slots[i],E::preimage_changed);
    result.original_graph_verified=true;
    result.already_selected=classification[0]==2&&classification[1]==2;
    if(result.already_selected)Require(result.observed_extent_bytes==result.target_extent_bytes&&actual.at(request.filespace_uuid)==contents.growth_images.back(),E::preimage_changed);
    else{
      result.phase=NativeGrowthRecoveryPhase::physical;
      result.physical_attempted=true;result.physical=target_device->PreallocateExtent(request.first_page*u64{request.page_size_bytes},request.page_count*u64{request.page_size_bytes});Require(result.physical->ok(),E::io_failure);
      result.physical_sync_attempted=true;result.physical_sync=target_device->Sync();Require(result.physical_sync->ok(),E::io_failure);
      result.phase=NativeGrowthRecoveryPhase::page_zero;
      result.page_zero=disk::RepairFilespacePageZeroGrowthBodyFromOpenDevice(*target_device,{database,request.filespace_uuid,request.page_size_profile_uuid},contents.growth_images.front(),contents.growth_images.back(),c.budget-c.used);
      if(!result.page_zero->ok())throw result.page_zero->error==disk::FilespacePageZeroBodyError::resource_exhausted?E::resource_exhausted:result.page_zero->error==disk::FilespacePageZeroBodyError::hash_failure?E::hash_failure:result.page_zero->error==disk::FilespacePageZeroBodyError::io_failure?E::io_failure:E::binding_mismatch;
    }
    result.phase=NativeGrowthRecoveryPhase::selection;
    result.selection=RecoverNativeManagementCheckpointPublicationOnOpenDevices(database,c.files,primary,attempt,intent,c.budget-c.used);
    result.error=result.selection.error;
    if(result.ok())result.phase=NativeGrowthRecoveryPhase::complete;
  }catch(E e){result.error=e;}catch(const std::bad_alloc&){result.error=E::resource_exhausted;}catch(const std::length_error&){result.error=E::resource_exhausted;}catch(...){result.error=E::io_failure;}
  return result;
}
} // namespace scratchbird::storage::database
