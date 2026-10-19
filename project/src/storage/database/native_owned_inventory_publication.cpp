// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_owned_inventory_publication.hpp"
#include "hash_digest_parts.hpp"

namespace scratchbird::storage::database {
namespace {
using E=NativePublicationError;
using Phase=NativeOwnedInventoryPublicationPhase;
using Image=std::span<const byte>;
void Require(bool value,E error){if(!value)throw error;}
bool Same(Image a,Image b){return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin());}
bool SameSelection(const NativeCheckpointSelection& a,const NativeCheckpointSelection& b){
  return a.bootstrap_uuid==b.bootstrap_uuid&&a.publication_uuid==b.publication_uuid&&
    a.selection_generation==b.selection_generation&&a.checkpoint==b.checkpoint&&
    a.checkpoint_object_uuid==b.checkpoint_object_uuid&&a.checkpoint_sha256==b.checkpoint_sha256&&
    a.checkpoint_generation==b.checkpoint_generation&&a.root_set_generation==b.root_set_generation&&
    a.timeline_uuid==b.timeline_uuid&&a.previous_selection_generation==b.previous_selection_generation&&
    a.previous_checkpoint==b.previous_checkpoint&&a.previous_checkpoint_object_uuid==b.previous_checkpoint_object_uuid&&
    a.previous_checkpoint_sha256==b.previous_checkpoint_sha256;
}
auto Hash(Image image){auto hash=core::hash::ComputeSha256DigestNative(image.data(),image.size());Require(hash.ok(),E::hash_failure);return hash.digest;}
std::span<byte> Region(NativeStorageArena& arena,usize count){
  auto block=arena.Allocate(count,alignof(std::max_align_t));Require(block.ok(),E::resource_exhausted);
  return {static_cast<byte*>(block.pointer),block.bytes};
}
NativeStorageArena Arena(NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding,usize bytes){
  Require(bytes,E::invalid_request);auto result=memory.CreateArena(binding,bytes,alignof(std::max_align_t));
  Require(result.ok(),E::resource_exhausted);return std::move(result.arena);
}
}
NativeOwnedInventoryPublication::~NativeOwnedInventoryPublication(){
  CheckOwner();if(entering_)std::terminate();entering_=true;
  // Cleanup callbacks cannot publish from a half-retired owner. Original
  // operation/full-host custody remains in reservation_ until all backing dies.
  phase_=Phase::preparation_failed;graph_={};artifacts_={};scratch_={};selectors_={};selector_preimages_={};
  workspace_arena_={};graph_arena_={};source_.Reset();
}
NativeOwnedInventoryPublicationResult NativeOwnedInventoryPublication::Result(E error) const noexcept {
  NativeOwnedInventoryPublicationResult r;r.error=error;r.phase=phase_;r.group=group_;r.index=index_;
  r.mutation_started=mutation_started_;r.installed_graph_verified=installed_graph_verified_;
  r.selected_graph_verified=selected_graph_verified_;if(selected_graph_verified_)r.snapshot=selected_;
  r.effects=reservation_.continuity_?reservation_.continuity_.BorrowOperation().effects():completed_effects_;return r;
}
NativeOwnedInventoryPublicationResult NativeOwnedInventoryPublication::IoFailure(NativeWriterIoResult&& io) const noexcept {
  auto result=Result(E::io_failure);result.io=std::move(io);return result;
}
NativeOwnedInventoryPublicationResult NativeOwnedInventoryPublication::RetryPreparation(
    const NativeManagementOperationView& record,const page::NativeTransactionInventoryConstView& inventory,
    core::uuid::StandaloneUuidV7Issuer& issuer) noexcept {
  CheckOwner();
  if(entering_)return Result(E::operation_pending);
  if(phase_!=Phase::preparation_failed||mutation_started_||artifact_started_||anchored_||
      !reservation_.continuity_||reservation_.phase_!=NativeOwnedWatermarkPhase::complete)
    return Result(E::invalid_request);
  entering_=true;struct Leave{bool& flag;~Leave(){flag=false;}}leave{entering_};
  // No graph write has occurred. Cleanup callbacks still see the original
  // busy writer and cannot change this attempt's immutable reservation intent.
  graph_={};artifacts_={};scratch_={};selectors_={};selector_preimages_={};
  source_.Reset();workspace_arena_={};graph_arena_={};
  phase_=Phase::preparing;
  return PrepareImpl(record,inventory,issuer);
}
NativeOwnedInventoryPublicationResult NativeOwnedInventoryPublication::PrepareImpl(
    const NativeManagementOperationView& record,const page::NativeTransactionInventoryConstView& inventory,
    core::uuid::StandaloneUuidV7Issuer& issuer) noexcept {
  try {
    auto operation=reservation_.continuity_.BorrowOperation();const auto& reserved=*reservation_.target_;
    Require(reserved.watermark.intent&&reserved.watermark.intent->recovery_profile==2&&
      !reserved.watermark.publication_plan&&!reserved.watermark.abandonment&&
      reserved.watermark.operation_uuid==operation.identity(),E::request_mismatch);
    Require(memory_->CheckBinding(binding_)==NativeStorageMemoryError::none&&
      binding_.database_uuid==reserved.watermark.header.database_uuid,E::binding_mismatch);
    auto source=operation.ObserveWithMemory(limits_.selected,*memory_,binding_);
    Require(source.ok(),E::checkpoint_failure);source_=std::move(source.observation);
    const auto& base=source_.selection();const auto& directory=source_.directory().directory;
    Require(SameSelection(*base.selection,reserved.selection),E::stale_base);
    {
      auto control=operation.ReadControlWithMemory(limits_.control,*memory_,binding_);
      if(!control.ok()){phase_=Phase::preparation_failed;auto out=Result(E::allocation_mismatch);out.control_error=control.authority_error;return out;}
      Require(control.authority.selection&&SameSelection(*control.authority.selection,reserved.selection),E::stale_base);
    }
    auto history=operation.ReadHistoryWithMemory(limits_.history,*memory_,binding_);
    if(!history.ok()){phase_=Phase::preparation_failed;auto out=Result(E::checkpoint_failure);out.history_error=history.history_error;return out;}
    Require(history.history.selection&&SameSelection(*history.history.selection,reserved.selection),E::stale_base);
    workspace_arena_=Arena(*memory_,binding_,limits_.workspace_backing_bytes);
    detail::NativeMetadataMemory metadata(Region(workspace_arena_,limits_.workspace_backing_bytes));
    detail::NativeMetadataScratch scratch{&metadata};
    const auto size=reservation_.page_bytes_;
    auto zero_bytes=scratch.Array<byte>(size);
    auto io=operation.ReadAtBounded(reservation_.primary_,reservation_.profile_,0,zero_bytes);
    if(!io.ok()){phase_=Phase::preparation_failed;return IoFailure(std::move(io));}
    auto zero=scratch.PageZero(zero_bytes);Require(zero.ok(),E::bootstrap_failure);
    graph_arena_=Arena(*memory_,binding_,limits_.graph_backing_bytes);
    const auto graph_region=Region(graph_arena_,limits_.graph_backing_bytes);
    scratch_=scratch.Array<byte>(size);
    // A free, recordless allocation slot can contain an earlier unselected
    // attempt. Preserve those bytes; only actual all-zero destinations may be
    // used for this fresh graph. Each failed probe monotonically excludes a
    // slot, within the admitted workspace, without consuming a generation or
    // manufacturing any reclamation decision.
    const auto excluded_capacity=std::min<u64>(zero.record->total_pages,
      limits_.workspace_backing_bytes/(4*sizeof(u64)));
    auto excluded=scratch.Array<u64>(excluded_capacity);usize excluded_count=0;
    for(;;){
      const auto built=BuildNativeInventoryGraphInto({base,directory,*zero.record,reserved,record,inventory,
        excluded.first(excluded_count),limits_.maximum_image_bytes},issuer,graph_region);
      Require(built.ok(),built.error);graph_=built.graph;
      const auto before=excluded_count;
      const auto probe=[&](Image image){Require(image.size()==size,E::image_failure);
        const auto decoded=disk::DecodeNativeCommonPageHeader(image.data(),128);Require(decoded.ok(),E::image_failure);
        const auto& h=*decoded.header;
        Require(h.filespace_uuid==reservation_.primary_&&h.page_size_profile_uuid==reservation_.profile_&&
          h.page_number<zero.record->total_pages&&h.page_number<=UINT64_MAX/size,E::binding_mismatch);
        auto result=operation.ReadAtBounded(h.filespace_uuid,h.page_size_profile_uuid,h.page_number*u64{size},scratch_);
        if(!result.ok())throw std::move(result);
        if(std::any_of(scratch_.begin(),scratch_.end(),[](byte b){return b!=0;})){
          Require(excluded_count<excluded.size(),E::resource_exhausted);excluded[excluded_count++]=h.page_number;}
      };
      probe(graph_.plan_image);probe(graph_.checkpoint);
      for(const auto group:{graph_.extent,graph_.bundle,graph_.inventory,graph_.directory,graph_.allocation})
        for(const auto image:group)probe(image);
      if(excluded_count==before)break;
    }
    auto validation_arena=Arena(*memory_,binding_,limits_.validation_backing_bytes);
    const auto validation=Region(validation_arena,limits_.validation_backing_bytes);
    const auto append=ValidateNativeManagementHistoryAppendInto(history.history,record,validation,graph_.plan.intent.startup_binding);
    if(!append.ok()){phase_=Phase::preparation_failed;auto out=Result(E::request_mismatch);out.history_error=append.error;return out;}
    // Exact original images are read through the same retained operation; no
    // re-opened path or caller-supplied allocation/inventory base participates.
    auto checkpoint=scratch.Array<byte>(size);
    io=operation.ReadAtBounded(reservation_.primary_,reservation_.profile_,reserved.selection.checkpoint.page_number*u64{size},checkpoint);
    if(!io.ok()){phase_=Phase::preparation_failed;return IoFailure(std::move(io));}
    Require(Hash(checkpoint)==reserved.selection.checkpoint_sha256,E::preimage_changed);
    auto maps=scratch.Array<Image>(base.allocation.pages.size());
    for(usize n=0;n<maps.size();++n)maps[n]=base.allocation.pages[n].image;
    auto directories=scratch.Array<Image>(directory.pages.size());
    for(usize n=0;n<directories.size();++n)directories[n]=directory.pages[n].image;
    auto inventories=scratch.Array<Image>(base.checkpoint_inventory.inventory_pages.size());
    for(usize n=0;n<inventories.size();++n){const auto& h=base.checkpoint_inventory.inventory_pages[n].header;
      auto image=scratch.Array<byte>(h.page_size_bytes);
      Require(h.page_number<=UINT64_MAX/h.page_size_bytes,E::binding_mismatch);
      io=operation.ReadAtBounded(h.filespace_uuid,h.page_size_profile_uuid,h.page_number*u64{h.page_size_bytes},image);
      if(!io.ok()){phase_=Phase::preparation_failed;return IoFailure(std::move(io));}inventories[n]=image;
    }
    const std::array<Image,1> zero_images{zero_bytes};
    const NativeManagementDirectoryBaseView directory_base{directories,zero_images};
    const NativeManagementControlAllocationInputs input{checkpoint,graph_.checkpoint,graph_.plan_image,graph_.extent,
      maps,graph_.allocation,graph_.bundle,inventories,graph_.directory.empty()?nullptr:&directory_base,limits_.maximum_image_bytes};
    const auto delta=ValidateNativeManagementControlAllocationInto(input,validation);
    if(!delta.ok()){phase_=Phase::preparation_failed;auto out=Result(E::allocation_mismatch);out.allocation_error=delta.error;return out;}
    anchor_={graph_.plan.target_checkpoint,graph_.plan.target_checkpoint_object_uuid,Hash(graph_.checkpoint),
      graph_.plan.reserved_generation,graph_.plan.target_root_set_generation,graph_.plan.timeline_uuid};
    const auto count=2+graph_.extent.size()+graph_.bundle.size()+graph_.inventory.size()+graph_.directory.size()+graph_.allocation.size();
    artifacts_=scratch.Array<Artifact>(count);usize n=0;
    const auto add=[&](Image image){Require(image.size()>=128,E::image_failure);const auto h=disk::DecodeNativeCommonPageHeader(image.data(),128);
      Require(h.ok(),E::image_failure);Require(h.header->filespace_uuid==reservation_.primary_&&
        h.header->page_size_profile_uuid==reservation_.profile_&&h.header->page_size_bytes==size&&image.size()==size&&
        h.header->database_uuid==binding_.database_uuid&&h.header->page_number<zero.record->total_pages,E::binding_mismatch);
      for(usize previous=0;previous<n;++previous)Require(artifacts_[previous].header.page_number!=h.header->page_number,E::allocation_mismatch);
      for(const auto& root:zero.record->roots)Require(root.filespace_uuid!=h.header->filespace_uuid||root.page_number!=h.header->page_number,E::allocation_mismatch);
      bool free=false;
      for(const auto& page:base.allocation.pages){const auto& m=page.map;
        if(h.header->page_number<m.first_page||h.header->page_number-m.first_page>=m.states.size())continue;
        free=m.states[h.header->page_number-m.first_page]==page::NativeAllocationState::free&&
          std::none_of(m.records.begin(),m.records.end(),[&](const auto& r){return r.page_number==h.header->page_number;});break;}
      Require(free,E::allocation_mismatch);artifacts_[n++]={*h.header,image};};
    groups_[0]=0;add(graph_.plan_image);groups_[1]=n;
    for(const auto image:graph_.extent)add(image);groups_[2]=n;
    for(const auto image:graph_.bundle)add(image);groups_[3]=n;
    for(const auto image:graph_.inventory)add(image);for(const auto image:graph_.directory)add(image);groups_[4]=n;
    for(const auto image:graph_.allocation)add(image);groups_[5]=n;add(graph_.checkpoint);groups_[6]=n;
    for(auto& image:selectors_)image=scratch.Array<byte>(size);
    for(usize i=0;i<2;++i){selector_preimages_[i]=scratch.Array<byte>(size);
      Require(base.slots[i].size()==size,E::binding_mismatch);
      std::copy(base.slots[i].begin(),base.slots[i].end(),selector_preimages_[i].begin());}
    for(const auto& artifact:artifacts_){const auto& h=artifact.header;
      Require(h.page_number<=UINT64_MAX/size,E::binding_mismatch);
      io=operation.ReadAtBounded(h.filespace_uuid,h.page_size_profile_uuid,h.page_number*u64{size},scratch_);
      if(!io.ok()){phase_=Phase::preparation_failed;return IoFailure(std::move(io));}
      Require(std::all_of(scratch_.begin(),scratch_.end(),[](byte b){return !b;}),E::preimage_changed);
    }
    // Keep only the exact selector preimages after construction and complete
    // before-graph validation. Original continuity remains in reservation_;
    // the large read arena is available again for installed-graph verification.
    source_.Reset();phase_=Phase::prepared;return Result(E::none);
  }catch(NativeWriterIoResult& io){phase_=Phase::preparation_failed;return IoFailure(std::move(io));}
   catch(E error){phase_=Phase::preparation_failed;return Result(error);}
   catch(const std::bad_alloc&){phase_=Phase::preparation_failed;return Result(E::resource_exhausted);}
   catch(const std::length_error&){phase_=Phase::preparation_failed;return Result(E::resource_exhausted);}
   catch(const std::system_error&){phase_=Phase::preparation_failed;return Result(E::resource_exhausted);}
}
NativePublicationError NativeOwnedInventoryPublication::PrepareAnchor() noexcept {
  try {
    auto& w=reservation_;Require(w.phase_==NativeOwnedWatermarkPhase::complete&&w.target_&&!w.target_->watermark.publication_plan,E::operation_pending);
    auto next=*w.target_;const auto& p=graph_.plan;
    Require(p.reservation_state_sha256==next.state_sha256&&p.operation_uuid==next.watermark.operation_uuid&&
      p.reserved_generation==next.watermark.watermark,E::binding_mismatch);
    const disk::NativePageReference ref{p.header.filespace_uuid,p.header.page_number,p.header.page_generation,p.header.page_size_profile_uuid};
    next.watermark.publication_plan=NativePublicationWatermark::PlanAnchor{ref,p.object_uuid,Hash(graph_.plan_image),p.reservation_state_sha256};
    for(usize i=0;i<2;++i){next.watermark.header=w.headers_[i];const auto encoded=EncodeNativePublicationWatermarkInto(next.watermark,selectors_[i]);
      if(!encoded.ok())return encoded.error==NativePublicationWatermarkError::hash_failure?E::hash_failure:E::image_failure;
      if(!i)next.state_sha256=encoded.state_sha256;else Require(next.state_sha256==encoded.state_sha256,E::binding_mismatch);}
    next.watermark.header=w.headers_[0];
    for(usize i=0;i<2;++i){std::copy(w.Image(3+i).begin(),w.Image(3+i).end(),w.Image(1+i).begin());
      std::copy(selectors_[i].begin(),selectors_[i].end(),w.Image(3+i).begin());}
    w.target_=next;w.phase_=NativeOwnedWatermarkPhase::preimages;w.sync_index_=w.replica_=0;return E::none;
  }catch(E error){return error;}
}
NativeOwnedInventoryPublicationResult NativeOwnedInventoryPublication::Publish() noexcept {
  CheckOwner();if(entering_)return Result(E::operation_pending);
  if(phase_==Phase::complete)return Result(E::none);
  if(phase_==Phase::idle||phase_==Phase::preparing||phase_==Phase::preparation_failed||phase_==Phase::handed_off)return Result(E::invalid_request);
  entering_=true;struct Leave {bool& flag;~Leave(){flag=false;}} leave{entering_};
  auto operation=reservation_.continuity_.BorrowOperation();
  const auto read=[&](const Artifact& a){return operation.ReadAtBounded(a.header.filespace_uuid,a.header.page_size_profile_uuid,
    a.header.page_number*u64{a.header.page_size_bytes},scratch_);};
  try {
    if(phase_==Phase::prepared){phase_=Phase::group_write;index_=groups_[1];}
    for(;;){
      if(phase_==Phase::group_write){
        const usize anchor_boundary=graph_.plan.intent.startup_binding?3:0;
        if(!anchored_&&group_==anchor_boundary){const auto error=PrepareAnchor();Require(error==E::none,error);phase_=Phase::anchor;continue;}
        if(group_==6){phase_=Phase::verify_installed;continue;}
        while(index_>groups_[group_]){const auto& a=artifacts_[index_-1];
          if(!artifact_started_){auto io=read(a);if(!io.ok())return IoFailure(std::move(io));
            Require(std::all_of(scratch_.begin(),scratch_.end(),[](byte b){return !b;}),E::preimage_changed);}
          artifact_started_=true;mutation_started_=true;
          auto io=operation.WriteAtBounded(a.header.filespace_uuid,a.header.page_size_profile_uuid,
            a.header.page_number*u64{a.header.page_size_bytes},a.image);
          if(!io.ok())return IoFailure(std::move(io));--index_;artifact_started_=false;
        }
        phase_=Phase::group_sync;
      }
      if(phase_==Phase::anchor){mutation_started_=true;
        auto anchored=reservation_.Publish();
        if(!anchored.ok()){auto out=Result(anchored.error);out.watermark=std::move(anchored);return out;}
        anchored_=true;phase_=Phase::group_write;continue;
      }
      if(phase_==Phase::group_sync){
        if(groups_[group_]!=groups_[group_+1]){auto io=operation.Sync(reservation_.primary_,reservation_.profile_);if(!io.ok())return IoFailure(std::move(io));}
        index_=groups_[group_];phase_=Phase::group_readback;
      }
      if(phase_==Phase::group_readback){
        while(index_<groups_[group_+1]){const auto& a=artifacts_[index_];auto io=read(a);if(!io.ok())return IoFailure(std::move(io));
          Require(Same(scratch_,a.image),E::readback_mismatch);++index_;}
        ++group_;if(group_<6)index_=groups_[group_+1];phase_=Phase::group_write;continue;
      }
      if(phase_==Phase::verify_installed){
        auto proof=operation.ReadControlWithMemory(limits_.control,*memory_,binding_,&anchor_);
        if(!proof.ok()){auto out=Result(E::allocation_mismatch);out.control_error=proof.authority_error;return out;}
        Require(proof.authority.anchor&&*proof.authority.anchor==anchor_,E::binding_mismatch);
        installed_graph_verified_=true;phase_=Phase::selector_prepare;
      }
      if(phase_==Phase::selector_prepare){
        const auto& plan=graph_.plan;Require(plan.base_selection_generation.has_value(),E::binding_mismatch);
        for(usize i=0;i<2;++i){const auto old=DecodeNativeCheckpointSelectionValue(selector_preimages_[i]);Require(old.ok(),E::image_failure);
          auto next=*old.selection;selector_pages_[i]=next.header.page_number;
          next.selection_generation=*plan.base_selection_generation+1;next.previous_selection_generation=*plan.base_selection_generation;
          next.previous_checkpoint=plan.base_checkpoint;next.previous_checkpoint_object_uuid=plan.base_checkpoint_object_uuid;
          next.previous_checkpoint_sha256=plan.base_checkpoint_sha256;next.publication_uuid=plan.operation_uuid;
          next.checkpoint=plan.target_checkpoint;next.checkpoint_object_uuid=plan.target_checkpoint_object_uuid;next.checkpoint_sha256=anchor_.checkpoint_sha256;
          next.checkpoint_generation=plan.reserved_generation;next.root_set_generation=plan.target_root_set_generation;
          const auto encoded=EncodeNativeCheckpointSelectionInto(next,selectors_[i]);Require(encoded.ok(),E::image_failure);
          if(!i){selected_=*reservation_.target_;selected_->selection=next;}
          auto io=operation.ReadAtBounded(reservation_.primary_,reservation_.profile_,selector_pages_[i]*u64{scratch_.size()},scratch_);
          if(!io.ok())return IoFailure(std::move(io));Require(Same(scratch_,selector_preimages_[i]),E::preimage_changed);
        }
        index_=0;phase_=Phase::selector_members_sync;
      }
      if(phase_==Phase::selector_members_sync){
        while(index_<reservation_.member_count_){const auto& member=reservation_.members_[index_];
          if(!member.read_only){auto io=operation.Sync(member.filespace,member.profile);if(!io.ok())return IoFailure(std::move(io));}++index_;}
        phase_=Phase::selector_write;
      }
      if(phase_==Phase::selector_write){auto io=operation.WriteAtBounded(reservation_.primary_,reservation_.profile_,
          selector_pages_[replica_]*u64{scratch_.size()},selectors_[replica_]);
        if(!io.ok())return IoFailure(std::move(io));phase_=Phase::selector_sync;
      }
      if(phase_==Phase::selector_sync){auto io=operation.Sync(reservation_.primary_,reservation_.profile_);
        if(!io.ok())return IoFailure(std::move(io));phase_=Phase::selector_readback;
      }
      if(phase_==Phase::selector_readback){auto io=operation.ReadAtBounded(reservation_.primary_,reservation_.profile_,
          selector_pages_[replica_]*u64{scratch_.size()},scratch_);
        if(!io.ok())return IoFailure(std::move(io));Require(Same(scratch_,selectors_[replica_]),E::readback_mismatch);
        if(++replica_<2){phase_=Phase::selector_write;continue;}phase_=Phase::verify_selected;
      }
      if(phase_==Phase::verify_selected){
        auto proof=operation.ReadControlWithMemory(limits_.control,*memory_,binding_);
        if(!proof.ok()){auto out=Result(E::allocation_mismatch);out.control_error=proof.authority_error;return out;}
        Require(proof.authority.anchor&&*proof.authority.anchor==anchor_&&proof.authority.selection&&
          SameSelection(*proof.authority.selection,selected_->selection),E::binding_mismatch);
        for(usize i=0;i<2;++i){auto io=operation.ReadAtBounded(reservation_.primary_,reservation_.profile_,
            selector_pages_[i]*u64{scratch_.size()},scratch_);
          if(!io.ok())return IoFailure(std::move(io));Require(Same(scratch_,selectors_[i]),E::readback_mismatch);}
        selected_graph_verified_=true;phase_=Phase::complete;return Result(E::none);
      }
    }
  }catch(E error){return Result(error);}
   catch(const std::bad_alloc&){return Result(E::resource_exhausted);}
   catch(const std::length_error&){return Result(E::resource_exhausted);}
   catch(const std::system_error&){return Result(E::resource_exhausted);}
}
} // namespace scratchbird::storage::database
