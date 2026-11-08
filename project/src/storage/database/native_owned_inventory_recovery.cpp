// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_checkpoint_metadata_scratch.hpp"
#include "native_owned_inventory_recovery.hpp"
#include "hash_digest_parts.hpp"

namespace scratchbird::storage::database {
namespace {
using E=NativePublicationError;
using Phase=NativeOwnedInventoryRecoveryPhase;
using Image=std::span<const byte>;
void Require(bool value,E error){if(!value)throw error;}
bool Same(Image a,Image b){return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin());}
auto Hash(Image bytes){const auto h=core::hash::ComputeSha256DigestNative(bytes.data(),bytes.size());
  Require(h.ok(),E::hash_failure);return h.digest;}
auto Self(const disk::NativeCommonPageHeader& h){
  return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};
}
bool Selection(const NativeCheckpointSelection& a,const NativeCheckpointSelection& b){
  return a.bootstrap_uuid==b.bootstrap_uuid&&a.publication_uuid==b.publication_uuid&&
    a.selection_generation==b.selection_generation&&a.checkpoint==b.checkpoint&&
    a.checkpoint_object_uuid==b.checkpoint_object_uuid&&a.checkpoint_sha256==b.checkpoint_sha256&&
    a.checkpoint_generation==b.checkpoint_generation&&a.root_set_generation==b.root_set_generation&&
    a.timeline_uuid==b.timeline_uuid&&a.previous_selection_generation==b.previous_selection_generation&&
    a.previous_checkpoint==b.previous_checkpoint&&a.previous_checkpoint_object_uuid==b.previous_checkpoint_object_uuid&&
    a.previous_checkpoint_sha256==b.previous_checkpoint_sha256;
}
}
NativeOwnedInventoryRecovery::~NativeOwnedInventoryRecovery(){
  CheckOwner();if(entering_)std::terminate();entering_=true;
  phase_=Phase::preparation_failed;selected_.reset();preimages_={};selectors_={};scratch_={};workspace_={};
}
NativeOwnedInventoryRecoveryResult NativeOwnedInventoryRecovery::Result(E error)const noexcept {
  NativeOwnedInventoryRecoveryResult out;out.error=error;out.phase=phase_;
  out.installed_graph_verified=installed_graph_verified_;out.selected_graph_verified=selected_graph_verified_;
  out.mutation_started=mutation_started_;if(selected_graph_verified_)out.snapshot=selected_;
  if(reservation_.continuity_)out.effects=reservation_.continuity_.BorrowOperation().effects();return out;
}
NativeOwnedInventoryRecoveryResult NativeOwnedInventoryRecovery::IoFailure(NativeWriterIoResult&& io)const noexcept {
  auto out=Result(E::io_failure);out.io=std::move(io);return out;
}
NativeOwnedInventoryRecoveryResult NativeOwnedInventoryRecovery::PrepareImpl() noexcept {
  try{
    auto operation=reservation_.continuity_.BorrowOperation();const auto& observed=*reservation_.observed_;
    const auto& w=observed.watermark;const auto& anchor=*w.publication_plan;
    Require(memory_->CheckBinding(binding_)==NativeStorageMemoryError::none&&
      binding_.database_uuid==w.header.database_uuid,E::binding_mismatch);
    Require(limits_.workspace_backing_bytes&&limits_.selected.maximum_verification_image_bytes&&
      limits_.history.maximum_verification_image_bytes&&limits_.control.maximum_verification_image_bytes,E::invalid_request);
    auto allocation=memory_->CreateArena(binding_,limits_.workspace_backing_bytes,alignof(std::max_align_t));
    Require(allocation.ok(),E::resource_exhausted);workspace_=std::move(allocation.arena);
    auto block=workspace_.Allocate(limits_.workspace_backing_bytes,alignof(std::max_align_t));
    Require(block.ok(),E::resource_exhausted);
    detail::NativeMetadataMemory resource({static_cast<byte*>(block.pointer),block.bytes});
    detail::NativeCheckpointMetadataScratch scratch{&resource};const auto size=reservation_.page_bytes_;
    scratch_=scratch.Array<byte>(size);
    const auto read=[&](const disk::NativePageReference& ref,std::span<byte> bytes){
      Require(ref.filespace_uuid==reservation_.primary_&&ref.page_size_profile_uuid==reservation_.profile_&&
        ref.page_number<=UINT64_MAX/size,E::binding_mismatch);
      auto io=operation.ReadAtBounded(ref.filespace_uuid,ref.page_size_profile_uuid,ref.page_number*u64{size},bytes);
      if(!io.ok())throw std::move(io);
    };
    auto plan_bytes=scratch.Array<byte>(size);read(anchor.page,plan_bytes);
    const auto decoded=DecodeNativePublicationPlanView(plan_bytes);
    Require(decoded.ok(),decoded.error==NativePublicationPlanError::hash_failure?E::hash_failure:E::image_failure);
    plan_=*decoded.plan;const auto& p=plan_;
    Require(p.base_selection_generation&&*p.base_selection_generation<UINT64_MAX&&
      Self(p.header)==anchor.page&&p.object_uuid==anchor.object_uuid&&decoded.sha256==anchor.sha256&&
      p.header.database_uuid==binding_.database_uuid&&p.bootstrap_uuid==w.bootstrap_uuid&&
      p.timeline_uuid==w.timeline_uuid&&p.operation_uuid==w.operation_uuid&&p.intent==*w.intent&&
      p.reservation_state_sha256==anchor.reservation_state_sha256&&p.reserved_generation==w.watermark&&
      p.base_checkpoint==w.base_checkpoint&&p.base_checkpoint_object_uuid==w.base_checkpoint_object_uuid&&
      p.base_checkpoint_sha256==w.base_checkpoint_sha256&&p.base_checkpoint_generation==w.base_checkpoint_generation&&
      p.base_root_set_generation==w.base_root_set_generation,E::binding_mismatch);
    auto target_bytes=scratch.Array<byte>(size);read(p.target_checkpoint,target_bytes);
    const auto target=scratch.Checkpoint(target_bytes);Require(target.ok(),E::checkpoint_failure);
    const auto& cp=*target.root;const auto target_sha=Hash(target_bytes);
    Require(Self(cp.header)==p.target_checkpoint&&cp.header.database_uuid==binding_.database_uuid&&
      cp.object_uuid==p.target_checkpoint_object_uuid&&cp.checkpoint_generation==p.reserved_generation&&
      cp.root_set_generation==p.target_root_set_generation&&cp.timeline_uuid==p.timeline_uuid&&cp.completed&&
      cp.creator_operation_uuid==p.operation_uuid,E::binding_mismatch);
    const auto plan_root=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==16;});
    Require(plan_root!=cp.roots.end()&&plan_root->page==anchor.page&&plan_root->object_uuid==anchor.object_uuid&&
      plan_root->sha256==anchor.sha256&&plan_root->page_type==0x500,E::binding_mismatch);
    anchor_={p.target_checkpoint,p.target_checkpoint_object_uuid,target_sha,p.reserved_generation,
      p.target_root_set_generation,p.timeline_uuid};
    {
      auto history=operation.ReadHistoryWithMemory(limits_.history,*memory_,binding_,&anchor_);
      if(!history.ok()){phase_=Phase::preparation_failed;auto out=Result(E::checkpoint_failure);
        out.history_error=history.history_error;return out;}
      Require(!history.history.entries.empty(),E::binding_mismatch);
      const auto& latest=history.history.entries.back();
      Require(latest.plan.operation_uuid==p.operation_uuid&&latest.plan.intent==p.intent&&
        Same(latest.plan_image,plan_bytes),E::request_mismatch);
    }
    {
      auto control=operation.ReadControlWithMemory(limits_.control,*memory_,binding_,&anchor_);
      if(!control.ok()){phase_=Phase::preparation_failed;auto out=Result(E::allocation_mismatch);
        out.control_error=control.authority_error;return out;}
      Require(control.authority.anchor&&*control.authority.anchor==anchor_&&
        MatchesNativeManagementPublishedCheckpoint(control.authority,cp,target_sha),E::binding_mismatch);
    }
    auto source=operation.ObserveWithMemory(limits_.selected,*memory_,binding_);
    Require(source.ok(),E::checkpoint_failure);const auto& s=source.observation.selection();
    Require(s.selection&&Selection(*s.selection,observed.selection),E::stale_base);
    const bool already_selected=s.selection->checkpoint==p.target_checkpoint;
    if(already_selected)Require(s.selection->checkpoint_sha256==target_sha&&
      s.selection->publication_uuid==p.operation_uuid&&s.selection->selection_generation==*p.base_selection_generation+1,
      E::binding_mismatch);
    else Require(s.selection->checkpoint==p.base_checkpoint&&s.selection->checkpoint_sha256==p.base_checkpoint_sha256&&
      s.selection->checkpoint_object_uuid==p.base_checkpoint_object_uuid&&
      s.selection->selection_generation==*p.base_selection_generation&&
      s.selection->checkpoint_generation==p.base_checkpoint_generation&&s.selection->root_set_generation==p.base_root_set_generation&&
      s.selection->timeline_uuid==p.timeline_uuid,E::stale_base);
    for(usize i=0;i<2;++i){
      preimages_[i]=scratch.Array<byte>(size);selectors_[i]=scratch.Array<byte>(size);
      Require(s.slots[i].size()==size,E::image_failure);std::copy(s.slots[i].begin(),s.slots[i].end(),preimages_[i].begin());
      const auto old=DecodeNativeCheckpointSelectionValue(preimages_[i]);Require(old.ok(),E::image_failure);
      auto next=*old.selection;selector_pages_[i]=next.header.page_number;
      next.selection_generation=*p.base_selection_generation+1;next.previous_selection_generation=*p.base_selection_generation;
      next.previous_checkpoint=p.base_checkpoint;next.previous_checkpoint_object_uuid=p.base_checkpoint_object_uuid;
      next.previous_checkpoint_sha256=p.base_checkpoint_sha256;next.publication_uuid=p.operation_uuid;
      next.checkpoint=p.target_checkpoint;next.checkpoint_object_uuid=p.target_checkpoint_object_uuid;next.checkpoint_sha256=target_sha;
      next.checkpoint_generation=p.reserved_generation;next.root_set_generation=p.target_root_set_generation;
      const auto image=EncodeNativeCheckpointSelectionInto(next,selectors_[i]);Require(image.ok(),E::image_failure);
      if(!i){selected_=observed;selected_->selection=next;}
      if(already_selected)Require(Same(preimages_[i],selectors_[i]),E::binding_mismatch);
    }
    source.observation.Reset();installed_graph_verified_=true;
    // Even two exact target selectors still require the explicit recovery
    // barriers, readbacks and fresh ordinary admission before completion.
    phase_=Phase::prepared;
    return Result(E::none);
  }catch(NativeWriterIoResult& io){phase_=Phase::preparation_failed;return IoFailure(std::move(io));}
   catch(E error){phase_=Phase::preparation_failed;return Result(error);}
   catch(const std::bad_alloc&){phase_=Phase::preparation_failed;return Result(E::resource_exhausted);}
   catch(const std::length_error&){phase_=Phase::preparation_failed;return Result(E::resource_exhausted);}
   catch(const std::system_error&){phase_=Phase::preparation_failed;return Result(E::resource_exhausted);}
}
NativeOwnedInventoryRecoveryResult NativeOwnedInventoryRecovery::Publish() noexcept {
  CheckOwner();if(entering_)return Result(E::operation_pending);
  if(phase_==Phase::complete)return Result(E::none);
  if(phase_==Phase::idle||phase_==Phase::preparing||phase_==Phase::preparation_failed)return Result(E::invalid_request);
  entering_=true;struct Leave{bool& flag;~Leave(){flag=false;}}leave{entering_};
  auto operation=reservation_.continuity_.BorrowOperation();const auto size=reservation_.page_bytes_;
  const auto read=[&](u64 page){return operation.ReadAtBounded(reservation_.primary_,reservation_.profile_,page*u64{size},scratch_);};
  try{
    if(phase_==Phase::prepared)phase_=Phase::preimages;
    if(phase_==Phase::preimages){
      for(usize i=0;i<2;++i){auto io=read(reservation_.headers_[i].page_number);if(!io.ok())return IoFailure(std::move(io));
        Require(Same(scratch_,reservation_.Image(i+1)),E::preimage_changed);
        io=read(selector_pages_[i]);if(!io.ok())return IoFailure(std::move(io));
        Require(Same(scratch_,preimages_[i]),E::preimage_changed);}
      phase_=Phase::members_sync;
    }
    if(phase_==Phase::members_sync){
      while(member_<reservation_.member_count_){const auto& m=reservation_.members_[member_];
        if(!m.read_only){auto io=operation.Sync(m.filespace,m.profile);if(!io.ok())return IoFailure(std::move(io));}++member_;}
      phase_=Phase::selector_write;
    }
    for(;;){
      if(phase_==Phase::selector_write){
        if(!Same(preimages_[replica_],selectors_[replica_])){
          mutation_started_=true;
          auto io=operation.WriteAtBounded(reservation_.primary_,reservation_.profile_,selector_pages_[replica_]*u64{size},selectors_[replica_]);
          if(!io.ok())return IoFailure(std::move(io));
        }
        phase_=Phase::selector_sync;}
      if(phase_==Phase::selector_sync){auto io=operation.Sync(reservation_.primary_,reservation_.profile_);
        if(!io.ok())return IoFailure(std::move(io));phase_=Phase::selector_readback;}
      if(phase_==Phase::selector_readback){auto io=read(selector_pages_[replica_]);if(!io.ok())return IoFailure(std::move(io));
        Require(Same(scratch_,selectors_[replica_]),E::readback_mismatch);
        if(++replica_<2){phase_=Phase::selector_write;continue;}phase_=Phase::verify_selected;}
      if(phase_==Phase::verify_selected){
        auto control=operation.ReadControlWithMemory(limits_.control,*memory_,binding_);
        if(!control.ok()){auto out=Result(E::allocation_mismatch);out.control_error=control.authority_error;return out;}
        Require(control.authority.anchor&&*control.authority.anchor==anchor_&&control.authority.selection&&
          Selection(*control.authority.selection,selected_->selection),E::binding_mismatch);
        for(usize i=0;i<2;++i){auto io=read(selector_pages_[i]);if(!io.ok())return IoFailure(std::move(io));
          Require(Same(scratch_,selectors_[i]),E::readback_mismatch);
          io=read(reservation_.headers_[i].page_number);if(!io.ok())return IoFailure(std::move(io));
          Require(Same(scratch_,reservation_.Image(i+1)),E::preimage_changed);}
        selected_graph_verified_=true;phase_=Phase::complete;return Result(E::none);
      }
    }
  }catch(E error){return Result(error);}
   catch(const std::bad_alloc&){return Result(E::resource_exhausted);}
   catch(const std::length_error&){return Result(E::resource_exhausted);}
   catch(const std::system_error&){return Result(E::resource_exhausted);}
}
} // namespace scratchbird::storage::database
