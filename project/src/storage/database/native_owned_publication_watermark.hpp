// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_owned_checkpoint_writer.hpp"
#include "native_publication_coordinator.hpp"
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace scratchbird::storage::database {
using core::platform::usize;

enum class NativeOwnedWatermarkPhase {
  idle, inspected, preimages, member_sync, write, synchronize, readback, complete
};
struct NativeOwnedWatermarkResult {
  NativePublicationError error=NativePublicationError::invalid_request;
  NativeOwnedWatermarkPhase phase=NativeOwnedWatermarkPhase::idle;
  NativeWriterError writer_error=NativeWriterError::none;
  NativeOwnedSourceDiagnostics source;
  disk::FilespacePageZeroError page_zero_error=disk::FilespacePageZeroError::none;
  NativePublicationWatermarkError watermark_error=NativePublicationWatermarkError::none;
  std::optional<disk::BoundedIoResult> read_failure;
  std::optional<NativeWriterIoResult> io;
  std::optional<NativePublicationSnapshot> snapshot;
  NativeWriterPhysicalEffects effects;
  usize member_index=0,replica_index=0;
  // Once true, retry/reconcile THIS attempt and its retained operation. An error
  // does not restore the previous counter or grant a new attempt identity.
  bool mutation_started=false;
  bool ok()const noexcept{return error==NativePublicationError::none&&snapshot.has_value();}
};
struct NativeOwnedWatermarkPreparedResult;
class NativePublicationWatermarkLeaseReader;

// Prepared publication-counter owner. All variable backing is admitted before
// selected-source guards. The actual writer operation excludes other native
// operations across guard retirement, allocation/provider callbacks and I/O.
// No raw device escapes; writes/syncs use its bounded retained-cohort API.
// This reserves a physical generation only: no policy/provider/session, BEGIN,
// inventory finality, installed graph or startup readiness is issued here.
class NativeOwnedPublicationWatermark {
 public:
  NativeOwnedPublicationWatermark() noexcept=default;
  ~NativeOwnedPublicationWatermark(){
    CheckOwner();if(entering_)std::terminate();entering_=true;
    // Cleanup callbacks see no usable observation/target and cannot reenter
    // this retiring owner. Governed backing retires before original custody.
    observed_.reset();target_.reset();phase_=NativeOwnedWatermarkPhase::idle;
    bytes_={};roots_={};members_={};arena_={};continuity_={};
  }
  NativeOwnedPublicationWatermark(const NativeOwnedPublicationWatermark&)=delete;
  NativeOwnedPublicationWatermark& operator=(const NativeOwnedPublicationWatermark&)=delete;
  NativeOwnedPublicationWatermark(NativeOwnedPublicationWatermark&& other) noexcept {Take(other);}
  NativeOwnedPublicationWatermark& operator=(NativeOwnedPublicationWatermark&& other) noexcept {
    if(this!=&other){CheckOwner();if(entering_)std::terminate();Take(other);}return *this;
  }
  explicit operator bool()const noexcept{CheckOwner();return bool(arena_);}

  template<class Operation>
  NativeOwnedWatermarkResult Inspect(Operation& operation,NativeSelectedCheckpointMemoryLimits limits,
      NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding,
      std::optional<NativeStartupBinding> startup=std::nullopt) noexcept {
    return InspectImpl(operation,limits,memory,binding,startup,false);
  }
  // Explicit duplex repair preserves the surviving original intent/counter.
  // It reserves no generation and resolves no transaction/management finality.
  template<class Operation>
  NativeOwnedWatermarkResult Recover(Operation&,NativeSelectedCheckpointMemoryLimits,
      NativeStorageMemory&,const NativeStorageMemoryBinding&) noexcept;
  template<class Operation>
  NativeOwnedWatermarkResult Reserve(Operation& operation,const NativePublicationSnapshot& expected,
      const NativePublicationIntent& intent) noexcept;
  template<class Operation>
  NativeOwnedWatermarkResult Resume(Operation& operation) noexcept;

 private:
  void CheckOwner()const noexcept {
    if(process_!=CurrentProcess()||thread_!=std::this_thread::get_id())std::terminate();
  }
  static u64 CurrentProcess()noexcept {
#if defined(_WIN32)
    return static_cast<u64>(::_getpid());
#else
    return static_cast<u64>(::getpid());
#endif
  }
  void Take(NativeOwnedPublicationWatermark& other)noexcept {
    other.CheckOwner();if(other.entering_)std::terminate();
    entering_=true;other.entering_=true;
    // Retain incoming continuity before retiring the old operation/custody.
    auto incoming=std::move(other.continuity_);
    arena_=std::move(other.arena_);bytes_=std::exchange(other.bytes_,{});roots_=std::exchange(other.roots_,{});
    members_=std::exchange(other.members_,{});maximum_page_bytes_=other.maximum_page_bytes_;
    page_bytes_=other.page_bytes_;member_count_=other.member_count_;sync_index_=other.sync_index_;replica_=other.replica_;
    operation_=other.operation_;primary_=other.primary_;profile_=other.profile_;headers_=other.headers_;
    observed_=std::exchange(other.observed_,{});target_=std::exchange(other.target_,{});
    startup_=std::exchange(other.startup_,{});phase_=std::exchange(other.phase_,NativeOwnedWatermarkPhase::idle);
    stable_=std::exchange(other.stable_,false);
    mutation_started_=std::exchange(other.mutation_started_,false);continuity_=std::move(incoming);
    other.entering_=false;entering_=false;
  }
  const u64 process_=CurrentProcess();
  const std::thread::id thread_=std::this_thread::get_id();
  // Declared first: original operation/full-host custody outlives all backing.
  NativeOwnedCheckpointWriter::OperationContinuity continuity_;
  struct Member {Uuid filespace,profile;bool read_only=false;};
  NativeStorageArena arena_;
  std::span<byte> bytes_;
  std::span<disk::FilespaceRootReference> roots_;
  std::span<Member> members_;
  usize maximum_page_bytes_=0,page_bytes_=0,member_count_=0,sync_index_=0,replica_=0;
  Uuid operation_,primary_,profile_;
  std::array<disk::NativeCommonPageHeader,2> headers_;
  std::optional<NativePublicationSnapshot> observed_,target_;
  std::optional<NativeStartupBinding> startup_;
  NativeOwnedWatermarkPhase phase_=NativeOwnedWatermarkPhase::idle;
  bool mutation_started_=false,entering_=false,stable_=false;
  std::span<byte> Image(usize n)noexcept{return bytes_.subspan(n*maximum_page_bytes_,page_bytes_);}
  NativeOwnedWatermarkResult Result(NativePublicationError error)const noexcept {
    NativeOwnedWatermarkResult r;r.error=error;r.phase=phase_;r.member_index=sync_index_;
    r.replica_index=replica_;r.mutation_started=mutation_started_;return r;
  }
  NativeOwnedWatermarkResult Publish()noexcept;
  template<class Operation>
  NativeOwnedWatermarkResult InspectImpl(Operation&,NativeSelectedCheckpointMemoryLimits,
      NativeStorageMemory&,const NativeStorageMemoryBinding&,std::optional<NativeStartupBinding>,bool)noexcept;
  friend class NativePublicationWatermarkLeaseReader;
  friend class NativeOwnedInventoryPublication;
  friend class NativeOwnedInventoryRecovery;
  friend usize NativeOwnedPublicationWatermarkWorkspaceBytes(const Uuid&,usize)noexcept;
  friend NativeOwnedWatermarkPreparedResult PrepareNativeOwnedPublicationWatermark(
      const Uuid&,usize,NativeStorageMemory&,const NativeStorageMemoryBinding&)noexcept;
};
inline usize NativeOwnedPublicationWatermarkWorkspaceBytes(const Uuid& profile,usize members)noexcept {
  const auto* p=disk::FindCanonicalFilespacePageProfile(profile);
  constexpr auto member_bytes=sizeof(NativeOwnedPublicationWatermark::Member);
  if(!p||!members||members>(SIZE_MAX-6*p->page_size_bytes-32*sizeof(disk::FilespaceRootReference)-
      3*(alignof(std::max_align_t)-1))/member_bytes)return 0;
  return 6*p->page_size_bytes+32*sizeof(disk::FilespaceRootReference)+members*member_bytes+
      3*(alignof(std::max_align_t)-1);
}
struct NativeOwnedWatermarkPreparedResult {
  NativePublicationError error=NativePublicationError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  core::platform::Status allocation_status;
  core::platform::DiagnosticRecord allocation_diagnostic;
  NativeOwnedPublicationWatermark workspace;
  bool ok()const noexcept{return error==NativePublicationError::none&&bool(workspace);}
};
inline NativeOwnedWatermarkPreparedResult PrepareNativeOwnedPublicationWatermark(
    const Uuid& profile,usize members,NativeStorageMemory& memory,
    const NativeStorageMemoryBinding& binding)noexcept {
  NativeOwnedWatermarkPreparedResult out;
  try {
    const auto size=NativeOwnedPublicationWatermarkWorkspaceBytes(profile,members);if(!size)return out;
    auto allocation=memory.CreateArena(binding,size,alignof(std::max_align_t));
    out.memory_error=allocation.error;out.allocation_status=allocation.backing.status;
    out.allocation_diagnostic=std::move(allocation.backing.diagnostic);
    if(!allocation.ok()){out.error=NativePublicationError::resource_exhausted;return out;}
    auto& w=out.workspace;w.arena_=std::move(allocation.arena);
    const auto array=[&]<class T>(usize n)->std::span<T>{
      static_assert(std::is_trivially_destructible_v<T>);
      const auto block=w.arena_.Allocate(n*sizeof(T),alignof(T));if(!block.ok())throw std::bad_alloc();
      auto* p=static_cast<T*>(block.pointer);std::uninitialized_value_construct_n(p,n);return {p,n};};
    w.maximum_page_bytes_=disk::FindCanonicalFilespacePageProfile(profile)->page_size_bytes;
    w.bytes_=array.template operator()<byte>(6*w.maximum_page_bytes_);
    w.roots_=array.template operator()<disk::FilespaceRootReference>(32);
    w.members_=array.template operator()<NativeOwnedPublicationWatermark::Member>(members);
    out.error=NativePublicationError::none;
  }catch(const std::bad_alloc&){out.error=NativePublicationError::resource_exhausted;}
   catch(const std::system_error&){out.error=NativePublicationError::resource_exhausted;}
  return out;
}

// Heap-free observation through the privately constructed actual selected lease.
// Mutable image spans remain private to the prepared owner. Complete bindings
// and both intact replicas are required before a new reservation can be made.
class NativePublicationWatermarkLeaseReader {
 public:
  static NativeOwnedWatermarkResult Read(const NativeSelectedCheckpointMemoryLease& source,
      NativeOwnedPublicationWatermark& w,u64 maximum_image_bytes,bool recovering)noexcept {
    using E=NativePublicationError;source.CheckThread();
    const auto fail=[&](E e){w.observed_.reset();w.phase_=NativeOwnedWatermarkPhase::idle;return w.Result(e);};
    if(!w||!source.selection_.ok())return fail(E::invalid_request);
    const auto& selected=source.selection_;const auto& cp=*selected.checkpoint_inventory.checkpoint;
    if(w.arena_.binding().database_uuid!=cp.header.database_uuid)return fail(E::binding_mismatch);
    if(source.devices_.size()>w.members_.size())return fail(E::resource_exhausted);
    const auto file=std::find_if(source.devices_.begin(),source.devices_.end(),[&](const auto& f){
      return f.filespace_uuid==cp.header.filespace_uuid;});
    if(file==source.devices_.end())return fail(E::invalid_device);
    const auto* profile=disk::FindCanonicalFilespacePageProfile(file->page_size_profile_uuid);
    if(!profile||profile->page_size_bytes>w.maximum_page_bytes_)return fail(E::resource_exhausted);
    if(source.retained_image_bytes_>maximum_image_bytes||3*u64{profile->page_size_bytes}>
        maximum_image_bytes-source.retained_image_bytes_)return fail(E::resource_exhausted);
    w.page_bytes_=profile->page_size_bytes;w.primary_=file->filespace_uuid;w.profile_=profile->uuid;
    auto& batch=source.batches_[file-source.devices_.begin()];
    std::optional<disk::BoundedIoResult> failure;
    const auto read=[&](u64 offset,std::span<byte> bytes){
      const auto io=batch.ReadAtBounded(offset,bytes.data(),bytes.size());
      if(!io.ok()){failure=io;return false;}return true;};
    const auto io_failure=[&]{auto out=fail(E::io_failure);out.read_failure=failure;return out;};
    if(!read(0,w.Image(0)))return io_failure();
    const disk::FilespaceBootstrapBinding binding{cp.header.database_uuid,w.primary_,w.profile_};
    const auto zero=disk::DecodeFilespacePageZeroInto(w.Image(0),w.roots_,&binding);
    if(!zero.ok()){auto r=fail(E::bootstrap_failure);r.page_zero_error=zero.error;return r;}
    const auto& z=*zero.record;
    if(z.bootstrap.filespace_role>4)return fail(E::invalid_device);
    if(z.bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required)return fail(E::cluster_requires_authority);
    if(z.bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted)return fail(E::encrypted_requires_authority);
    const auto size=batch.SizeBounded();
    if(!size.ok()){failure=size;return io_failure();}
    if(z.total_pages>UINT64_MAX/w.page_bytes_||size.size_bytes!=z.total_pages*w.page_bytes_)return fail(E::binding_mismatch);
    std::array<NativePublicationWatermarkValue,2> states;
    for(usize i=0;i<2;++i){
      const auto root=std::find_if(z.roots.begin(),z.roots.end(),[&](const auto& r){return r.kind==20+i;});
      if(root==z.roots.end())return fail(E::bootstrap_failure);
      const page::NativeAllocationRecord* allocation=nullptr;
      for(const auto& image:selected.allocation.pages){const auto& m=image.map;
        if(root->page_number<m.first_page||root->page_number-m.first_page>=m.states.size())continue;
        if(m.states[root->page_number-m.first_page]!=page::NativeAllocationState::allocated)return fail(E::allocation_mismatch);
        const auto it=std::lower_bound(m.records.begin(),m.records.end(),root->page_number,
            [](const auto& a,u64 n){return a.page_number<n;});
        if(it!=m.records.end()&&it->page_number==root->page_number)allocation=&*it;break;
      }
      if(!allocation||allocation->page_type!=0x500||allocation->page_generation!=root->page_generation||
          allocation->owner_uuid!=root->object_uuid)return fail(E::allocation_mismatch);
      const auto& entries=selected.checkpoint_inventory.inventory.entries;
      const auto creator=std::lower_bound(entries.begin(),entries.end(),allocation->creator_local_transaction_id,
          [](const auto& a,u64 n){return a.identity.local_id.value<n;});
      if(creator==entries.end()||creator->identity.local_id.value!=allocation->creator_local_transaction_id||
          creator->identity.transaction_uuid.value!=allocation->creator_transaction_uuid||
          creator->identity.scope!=transaction::mga::TransactionScope::local_node||
          !transaction::mga::HasCommittedInventoryOutcome(*creator))return fail(E::allocation_mismatch);
      w.headers_[i]={z.bootstrap.page_size_bytes,0x500,binding.database_uuid,w.primary_,allocation->page_uuid,
          root->page_number,root->page_generation,0,w.profile_};
      if(root->page_number>UINT64_MAX/w.page_bytes_)return fail(E::binding_mismatch);
      if(!read(root->page_number*w.page_bytes_,w.Image(i+1)))return io_failure();
      const disk::NativeCommonPageHeaderBinding expected{binding,root->page_number,root->page_generation,0x500,allocation->page_uuid};
      const auto common=disk::DecodeNativeCommonPageHeader(w.Image(i+1).data(),128);
      if(common.error==disk::NativeCommonPageHeaderError::resource_exhausted)return fail(E::resource_exhausted);
      const auto header=disk::DecodeNativeCommonPageHeader(w.Image(i+1).data(),128,&expected);
      // Never overwrite a contradictory independently intact header under the
      // single-damaged-replica recovery rule. Actual allocated IDs stay fixed.
      if(common.ok()&&(!header.ok()||header.header->flags||header.header->page_size_bytes!=w.page_bytes_))return fail(E::binding_mismatch);
      states[i]=DecodeNativePublicationWatermarkValue(w.Image(i+1));const auto& state=states[i];
      if(state.error==NativePublicationWatermarkError::hash_failure||state.error==NativePublicationWatermarkError::resource_exhausted){
        auto r=fail(state.error==NativePublicationWatermarkError::hash_failure?E::hash_failure:E::resource_exhausted);
        r.watermark_error=state.error;return r;}
      if(!state.ok())continue;
      const auto& v=*state.state;
      if(v.object_uuid!=root->object_uuid||v.bootstrap_uuid!=z.page_uuid||v.timeline_uuid!=cp.timeline_uuid||
          v.watermark<cp.checkpoint_generation||(v.abandonment&&v.watermark<=cp.checkpoint_generation))return fail(E::binding_mismatch);
      const auto* base=&selected.checkpoint_inventory;
      if(v.watermark==cp.checkpoint_generation&&v.watermark>1){
        if(!selected.predecessor.ok()||v.operation_uuid!=selected.selection->publication_uuid)return fail(E::binding_mismatch);
        base=&selected.predecessor;
      }else if(v.watermark==1){
        if(cp.checkpoint_generation!=1||cp.root_set_generation!=1||cp.predecessor||v.operation_uuid!=z.creation_operation_uuid)
          return fail(E::binding_mismatch);
      }else if(v.operation_uuid==selected.selection->publication_uuid)return fail(E::binding_mismatch);
      const auto& b=*base->checkpoint;
      const disk::NativePageReference ref{b.header.filespace_uuid,b.header.page_number,b.header.page_generation,b.header.page_size_profile_uuid};
      if(v.base_checkpoint!=ref||v.base_checkpoint_object_uuid!=b.object_uuid||v.base_checkpoint_sha256!=base->checkpoint_sha256||
          v.base_checkpoint_generation!=b.checkpoint_generation||v.base_root_set_generation!=b.root_set_generation)return fail(E::binding_mismatch);
    }
    if(w.headers_[0].page_uuid==w.headers_[1].page_uuid)return fail(E::allocation_mismatch);
    const auto pair=ClassifyNativePublicationWatermarkPairValue(w.Image(1),w.Image(2));
    usize selected_replica=0;w.stable_=pair.ok();
    if(!pair.ok()){
      if(!recovering||pair.error!=NativePublicationWatermarkError::repair_required){
        auto r=fail(pair.error==NativePublicationWatermarkError::repair_required?E::repair_required:
            pair.error==NativePublicationWatermarkError::hash_failure?E::hash_failure:E::image_failure);
        r.watermark_error=pair.error;return r;}
      if(states[0].ok()&&states[1].ok()){
        const auto& a=*states[0].state;const auto& b=*states[1].state;
        if(!(a.watermark>b.watermark||(a.watermark==b.watermark&&
            ((a.publication_plan&&!b.publication_plan)||(a.abandonment&&!b.abandonment)))))return fail(E::image_failure);
      }else selected_replica=states[0].ok()?0:1;
    }
    if(w.startup_){
      const auto& b=*w.startup_;bool present=false;
      for(const auto& entry:selected.checkpoint_inventory.inventory.entries){
        const bool same_local=entry.identity.local_id.value==b.local_transaction_id;
        const bool same_uuid=entry.identity.transaction_uuid.value==b.transaction_uuid;
        if(same_local!=same_uuid)return fail(E::request_mismatch);
        if(same_local){if(entry.identity.scope!=transaction::mga::TransactionScope::local_node)
            return fail(E::cluster_requires_authority);present=true;}
      }
      if(!present&&b.local_transaction_id!=selected.checkpoint_inventory.inventory.next_local_transaction_id)
        return fail(E::request_mismatch);
    }
    w.member_count_=source.devices_.size();
    for(usize i=0;i<w.member_count_;++i)w.members_[i]={source.devices_[i].filespace_uuid,
        source.devices_[i].page_size_profile_uuid,source.devices_[i].device->read_only()};
    w.observed_=NativePublicationSnapshot{*selected.selection,*states[selected_replica].state,states[selected_replica].state_sha256};
    w.phase_=NativeOwnedWatermarkPhase::inspected;auto out=w.Result(E::none);out.snapshot=w.observed_;return out;
  }
};

template<class Operation>
NativeOwnedWatermarkResult NativeOwnedPublicationWatermark::InspectImpl(Operation& operation,
    NativeSelectedCheckpointMemoryLimits limits,NativeStorageMemory& memory,
    const NativeStorageMemoryBinding& binding,std::optional<NativeStartupBinding> startup,bool recovering)noexcept {
  using E=NativePublicationError;
  CheckOwner();
  if(entering_||mutation_started_)return Result(E::operation_pending);
  if(!*this||!operation)return Result(E::invalid_request);
  entering_=true;struct Leave{bool& flag;~Leave(){flag=false;}}leave{entering_};
  observed_.reset();target_.reset();startup_=startup;stable_=false;phase_=NativeOwnedWatermarkPhase::idle;
  continuity_=operation.RetainContinuity();
  if(!continuity_)return Result(E::invalid_request);
  auto acquired=continuity_.BorrowOperation();
  auto read=acquired.ReadWithMemory(limits,memory,binding);
  if(!read.ok()){auto out=Result(E::checkpoint_failure);out.writer_error=read.writer_error;
    out.source=std::move(static_cast<NativeOwnedSourceDiagnostics&>(read));return out;}
  operation_=acquired.identity();
  auto out=NativePublicationWatermarkLeaseReader::Read(read.lease.source(),*this,limits.maximum_verification_image_bytes,recovering);
  // Retire all device guards before any caller cleanup, metrics or allocating
  // publication preparation. The unchanged actual operation stays reserved.
  read.lease.Reset();out.source=std::move(static_cast<NativeOwnedSourceDiagnostics&>(read));
  out.effects=acquired.effects();return out;
}

template<class Operation>
NativeOwnedWatermarkResult NativeOwnedPublicationWatermark::Recover(Operation& operation,
    NativeSelectedCheckpointMemoryLimits limits,NativeStorageMemory& memory,
    const NativeStorageMemoryBinding& binding)noexcept {
  using E=NativePublicationError;
  auto inspected=InspectImpl(operation,limits,memory,binding,std::nullopt,true);
  if(!inspected.ok())return inspected;
  entering_=true;struct Leave{bool& flag;~Leave(){flag=false;}}leave{entering_};
  auto w=observed_->watermark;
  for(usize i=0;i<2;++i){w.header=headers_[i];const auto encoded=EncodeNativePublicationWatermarkInto(w,Image(3+i));
    if(!encoded.ok()){auto out=Result(encoded.error==NativePublicationWatermarkError::hash_failure?E::hash_failure:E::image_failure);
      out.watermark_error=encoded.error;return out;}
    if(encoded.state_sha256!=observed_->state_sha256)return Result(E::binding_mismatch);
  }
  target_=observed_;phase_=NativeOwnedWatermarkPhase::preimages;sync_index_=replica_=0;
  return Publish();
}

template<class Operation>
NativeOwnedWatermarkResult NativeOwnedPublicationWatermark::Reserve(Operation& operation,
    const NativePublicationSnapshot& expected,const NativePublicationIntent& intent)noexcept {
  using E=NativePublicationError;
  CheckOwner();
  if(entering_||mutation_started_)return Result(E::operation_pending);
  if(!operation||!observed_||phase_!=NativeOwnedWatermarkPhase::inspected||!operation.HasContinuity(continuity_))
    return Result(E::invalid_request);
  if(!stable_)return Result(E::repair_required);
  // The hash binds complete state; compare the selected physical checkpoint as
  // well, since the same counter image may accompany different selected roots.
  const auto& s=observed_->selection;const auto& x=expected.selection;
  if(expected.state_sha256!=observed_->state_sha256||x.checkpoint_sha256!=s.checkpoint_sha256||
      x.checkpoint!=s.checkpoint||x.selection_generation!=s.selection_generation||x.publication_uuid!=s.publication_uuid)
    return Result(E::stale_base);
  auto w=observed_->watermark;
  if(w.intent&&!w.abandonment&&w.watermark>s.checkpoint_generation)return Result(E::operation_pending);
  if(operation_==w.operation_uuid||operation_==s.publication_uuid)return Result(E::invalid_request);
  if(w.watermark==UINT64_MAX)return Result(E::generation_exhausted);
  // Exact tuple checked against actual inventory while its source guards were
  // retained. That lookup still supplies no service/session/BEGIN authority.
  if(intent.startup_binding!=startup_)return Result(E::request_mismatch);
  entering_=true;struct Leave{bool& flag;~Leave(){flag=false;}}leave{entering_};
  w.previous_watermark=w.watermark;++w.watermark;w.previous_state_sha256=observed_->state_sha256;
  w.operation_uuid=operation_;w.intent=intent;w.publication_plan.reset();w.abandonment.reset();
  w.base_checkpoint=s.checkpoint;w.base_checkpoint_object_uuid=s.checkpoint_object_uuid;
  w.base_checkpoint_sha256=s.checkpoint_sha256;w.base_checkpoint_generation=s.checkpoint_generation;
  w.base_root_set_generation=s.root_set_generation;
  std::array<byte,32> digest{};
  for(usize i=0;i<2;++i){w.header=headers_[i];const auto encoded=EncodeNativePublicationWatermarkInto(w,Image(3+i));
    if(!encoded.ok()){auto out=Result(encoded.error==NativePublicationWatermarkError::hash_failure?E::hash_failure:E::image_failure);
      out.watermark_error=encoded.error;return out;}if(!i){w=*encoded.state;digest=encoded.state_sha256;}}
  w.header=headers_[0];target_=NativePublicationSnapshot{s,w,digest};
  phase_=NativeOwnedWatermarkPhase::preimages;sync_index_=replica_=0;
  return Publish();
}

template<class Operation>
NativeOwnedWatermarkResult NativeOwnedPublicationWatermark::Resume(Operation& operation)noexcept {
  using E=NativePublicationError;
  CheckOwner();
  if(entering_)return Result(E::operation_pending);
  if(!operation||!target_||!operation.HasContinuity(continuity_))return Result(E::request_mismatch);
  entering_=true;struct Leave{bool& flag;~Leave(){flag=false;}}leave{entering_};
  return Publish();
}

inline NativeOwnedWatermarkResult NativeOwnedPublicationWatermark::Publish()noexcept {
  using E=NativePublicationError;
  // Actual private reservation state, never a UUID-based reacquisition. A
  // callback may Reset the external facade; this original operation/full-host
  // pin must still own every remaining physical phase and its exact receipt.
  auto operation=continuity_.BorrowOperation();
  const auto failed=[&](NativeWriterIoResult&& io){auto out=Result(E::io_failure);
    out.writer_error=io.error;out.effects=io.effects;out.io=std::move(io);return out;};
  const auto mismatch=[&](E error){auto out=Result(error);out.effects=operation.effects();return out;};
  const auto read=[&](usize replica){return operation.ReadAtBounded(primary_,profile_,headers_[replica].page_number*page_bytes_,Image(5));};
  if(phase_==NativeOwnedWatermarkPhase::preimages){
    for(usize i=0;i<2;++i){auto io=read(i);if(!io.ok())return failed(std::move(io));
      if(!std::equal(Image(5).begin(),Image(5).end(),Image(1+i).begin()))return mismatch(E::preimage_changed);}
    phase_=NativeOwnedWatermarkPhase::member_sync;
  }
  if(phase_==NativeOwnedWatermarkPhase::member_sync){
    while(sync_index_<member_count_){const auto& member=members_[sync_index_];
      if(member.read_only){++sync_index_;continue;}
      auto io=operation.Sync(member.filespace,member.profile);if(!io.ok())return failed(std::move(io));++sync_index_;}
    phase_=NativeOwnedWatermarkPhase::write;
  }
  while(replica_<2){
    if(phase_==NativeOwnedWatermarkPhase::write){
      if(!std::equal(Image(1+replica_).begin(),Image(1+replica_).end(),Image(3+replica_).begin())){
        mutation_started_=true;
        auto io=operation.WriteAtBounded(primary_,profile_,headers_[replica_].page_number*page_bytes_,Image(3+replica_));
        if(!io.ok())return failed(std::move(io));}
      phase_=NativeOwnedWatermarkPhase::synchronize;
    }
    if(phase_==NativeOwnedWatermarkPhase::synchronize){
      auto io=operation.Sync(primary_,profile_);if(!io.ok())return failed(std::move(io));phase_=NativeOwnedWatermarkPhase::readback;
    }
    if(phase_==NativeOwnedWatermarkPhase::readback){
      auto io=read(replica_);if(!io.ok())return failed(std::move(io));
      if(!std::equal(Image(5).begin(),Image(5).end(),Image(3+replica_).begin()))return mismatch(E::readback_mismatch);
      ++replica_;phase_=NativeOwnedWatermarkPhase::write;
    }
  }
  phase_=NativeOwnedWatermarkPhase::complete;auto out=Result(E::none);out.snapshot=target_;
  out.effects=operation.effects();return out;
}
} // namespace scratchbird::storage::database
