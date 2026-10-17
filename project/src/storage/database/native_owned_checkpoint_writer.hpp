// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_owned_checkpoint_source.hpp"
#include <span>
#include <thread>

namespace scratchbird::storage::database {
enum class NativeWriterError {
  none,invalid_request,invalid_operation,wrong_process,withdrawn,busy,read_handle_live,
  source_failure,resource_exhausted,filespace_absent,profile_mismatch,invalid_buffer,
  extent_invalid,extent_outside_file,read_only_member,io_failure,synchronization_failure,close_failure
};
// Bounded-I/O primitive observations; selected reads retain their separate
// complete source diagnostics. Neither a sync nor successful bytes certify MGA,
// authorization, a recovery decision, an installed graph or durable publication.
struct NativeWriterPhysicalEffects {
  u64 read_attempts=0,read_bytes=0,write_attempts=0,write_bytes=0;
  u64 size_attempts=0,sync_attempts=0,successful_syncs=0;
  bool uncertain_write=false,counts_saturated=false;
  bool operator==(const NativeWriterPhysicalEffects&)const=default;
};
struct NativeWriterIoResult {
  NativeWriterError error=NativeWriterError::invalid_operation;
  Uuid operation_uuid,filespace_uuid;
  disk::BoundedIoError extent_error=disk::BoundedIoError::none;
  std::optional<disk::BoundedIoResult> size_observation,io;
  int synchronization_native_error=0;
  NativeWriterPhysicalEffects effects;
  bool ok()const noexcept{return error==NativeWriterError::none&&io&&io->ok();}
};
struct NativeOwnedWriterResult;
enum class NativeWriterHandleCloseDisposition {not_present,released,retained,released_after_error,indeterminate_quarantined};
struct NativeWriterHandleCloseReceipt {
  NativeWriterHandleCloseDisposition disposition=NativeWriterHandleCloseDisposition::not_present;
  u32 native_error=0;bool attempted=false;
};
struct NativeWriterMemberCloseReceipt {
  Uuid filespace_uuid,page_size_profile_uuid;
  std::optional<disk::BoundedIoResult> synchronization;
  NativeWriterHandleCloseReceipt data,ownership;
};
struct NativeWriterCloseResult {
  NativeWriterError error=NativeWriterError::invalid_request;
  usize member_count=0;
  // Adopted cohort FileDevice handles only, NOT the retained original route
  // owner/OS node locks, which remain held until this actual owner retires.
  // This is not a complete node-shutdown or durable publication receipt.
  bool all_members_retired=false,historical_close_uncertainty=false;
  Uuid first_uncertain_filespace;
  u32 first_uncertain_native_error=0;
  int synchronization_native_error=0;
  bool ok()const noexcept{return error==NativeWriterError::none&&all_members_retired&&!historical_close_uncertainty;}
};
class NativeOwnedCheckpointWriter final {
  struct OperationState;
  struct ReaderState;
 public:
  class ReadLease;
  struct ReadResult;
  class Operation;
  struct OperationResult;
  NativeOwnedCheckpointWriter(const NativeOwnedCheckpointWriter&)=delete;
  NativeOwnedCheckpointWriter& operator=(const NativeOwnedCheckpointWriter&)=delete;
  // Initial physical custody only. Consumes supplied members and persistent
  // grant ONLY on success; never upgrades an existing read source. No admitted
  // principal/configuration/provider/session/recovery/BEGIN authority results.
  static NativeOwnedWriterResult AdoptRouteWithMemory(
      const std::shared_ptr<disk::RouteSourceTransition>&,const Uuid&,const Uuid&,
      std::vector<std::unique_ptr<disk::FileDevice>>&,NativeOwnedCheckpointMemoryLimits,
      NativeStorageMemory&,const NativeStorageMemoryBinding&,
      NativeStorageMemory&,const NativeStorageMemoryBinding&) noexcept;
  // Nonblocking reservation, not a held mutex. Linearized actual ownership
  // survives selected-read Reset; no control/registry lock spans callbacks/I/O.
  static OperationResult TryBeginOperation(std::shared_ptr<NativeOwnedCheckpointWriter>,
      const Uuid& operation_uuid) noexcept;
  // Preallocated exact member output, no implicit drain wait. Capacity is
  // validated before withdrawal/effects. Busy withdraws NEW admission while
  // accepted operations retain responsibility. Retry resynchronizes only
  // retained members; uncertain retired native handles are NEVER retried.
  static NativeWriterCloseResult Close(std::shared_ptr<NativeOwnedCheckpointWriter>,
      std::span<NativeWriterMemberCloseReceipt>) noexcept;
  void Withdraw()noexcept{accepting_.store(false,std::memory_order_release);}
  const Uuid& database_uuid()const noexcept{return cohort_->database_;}
  const Uuid& primary_uuid()const noexcept{return cohort_->primary_;}
  // Reporting-only cumulative loss counters, never storage/admission evidence.
  // Fixed observations are safe even when an optional metrics allocation fails.
  u64 failed_io_latency_observations()const noexcept {
    if(!InOwningProcess())return 0;
    u64 total=0;for(const auto& member:cohort_->memory_state_->devices){
      const auto n=member.device->failed_io_latency_observations();
      if(n>UINT64_MAX-total)return UINT64_MAX;total+=n;
    }return total;
  }

  class ReadLease final {
   public:
    ReadLease()noexcept=default;
    ReadLease(const ReadLease&)=delete;ReadLease& operator=(const ReadLease&)=delete;
    ReadLease(ReadLease&& other)noexcept{other.CheckAccess();state_=std::move(other.state_);}
    ReadLease& operator=(ReadLease&& other)noexcept{if(this!=&other){other.CheckAccess();Reset();state_=std::move(other.state_);}return *this;}
    ~ReadLease(){Reset();}
    explicit operator bool()const noexcept{CheckAccess();return bool(state_);}
    // No mutable devices, owner facade or selected-handle extraction API.
    const NativeSelectedCheckpointMemoryLease& source()const noexcept;
    void Reset()noexcept;
   private:
    friend class NativeOwnedCheckpointWriter;friend class Operation;
    void CheckAccess()const noexcept;
    std::unique_ptr<ReaderState> state_;
  };
  struct ReadResult:NativeOwnedSourceDiagnostics {
    NativeWriterError writer_error=NativeWriterError::invalid_operation;
    ReadLease lease;
    bool ok()const noexcept{return writer_error==NativeWriterError::none&&error==NativeOwnedSourceError::none&&bool(lease);}
  };
  class Operation final {
   public:
    Operation()noexcept=default;
    Operation(const Operation&)=delete;Operation& operator=(const Operation&)=delete;
    Operation(Operation&& other)noexcept{if(other.state_&&other.Check()!=NativeWriterError::none)std::terminate();state_=std::move(other.state_);}
    Operation& operator=(Operation&& other)noexcept{if(this!=&other){if(other.state_&&other.Check()!=NativeWriterError::none)std::terminate();Reset();state_=std::move(other.state_);}return *this;}
    ~Operation(){Reset();}
    explicit operator bool()const noexcept{return Check()==NativeWriterError::none;}
    void Reset()noexcept;
    const Uuid& identity()const noexcept;
    const NativeWriterPhysicalEffects& effects()const noexcept;
    ReadResult ReadWithMemory(NativeSelectedCheckpointMemoryLimits,NativeStorageMemory&,
        const NativeStorageMemoryBinding&) noexcept;
    // Exclusive buffers remain caller-owned. No extension, page allocation,
    // journal, implicit repair or new attempt identity. A failed call can have
    // effects: retain this operation and the original owning journal request.
    NativeWriterIoResult ReadAtBounded(const Uuid&,const Uuid& expected_profile,u64,std::span<byte>) noexcept;
    NativeWriterIoResult WriteAtBounded(const Uuid&,const Uuid& expected_profile,u64,std::span<const byte>) noexcept;
    NativeWriterIoResult Sync(const Uuid&,const Uuid& expected_profile) noexcept;
   private:
    friend class NativeOwnedCheckpointWriter;
    NativeWriterError Check()const noexcept;
    std::shared_ptr<OperationState> state_;
  };
  struct OperationResult {
    NativeWriterError error=NativeWriterError::invalid_request;
    disk::RouteSourceTransitionError route_error=disk::RouteSourceTransitionError::none;
    Operation operation;
    bool ok()const noexcept{return error==NativeWriterError::none&&bool(operation);}
  };
 private:
  NativeOwnedCheckpointWriter()=default;
  bool InOwningProcess()const noexcept{return cohort_&&cohort_->InOwningProcess();}
  struct OperationState {
    std::shared_ptr<NativeOwnedCheckpointWriter> owner;
    Uuid identity;
    std::thread::id thread=std::this_thread::get_id();
    NativeWriterPhysicalEffects effects;
    bool registered=false,reader_live=false,io_active=false;
    ~OperationState(){if(registered&&owner->InOwningProcess()){
      if(thread!=std::this_thread::get_id()||reader_live||io_active)std::terminate();
      owner->busy_.store(false,std::memory_order_release);}}
  };
  struct ReaderState {
    std::shared_ptr<OperationState> operation;
    NativeSelectedCheckpointMemoryHandle selected;
    ~ReaderState(){selected.Reset();if(operation)operation->reader_live=false;}
  };
  static NativeWriterIoResult Io(Operation&,disk::BoundedIoOperation,const Uuid&,const Uuid&,u64,
      std::span<byte>,std::span<const byte>) noexcept;
  std::shared_ptr<disk::RouteSourceTransition> transition_;
  // Never returned through the legacy mutable-device read facade.
  std::shared_ptr<NativeOwnedCheckpointSource> cohort_;
  std::atomic<bool> accepting_{true},busy_{false};
  bool close_uncertain_=false;
  Uuid first_uncertain_filespace_;
  u32 first_uncertain_native_error_=0;
};
struct NativeOwnedWriterResult:NativeOwnedSourceDiagnostics {
  std::shared_ptr<NativeOwnedCheckpointWriter> writer;
  bool ok()const noexcept{return error==NativeOwnedSourceError::none&&writer!=nullptr;}
};
inline NativeOwnedWriterResult NativeOwnedCheckpointWriter::AdoptRouteWithMemory(
    const std::shared_ptr<disk::RouteSourceTransition>& transition,const Uuid& database,const Uuid& primary,
    std::vector<std::unique_ptr<disk::FileDevice>>& secondary,NativeOwnedCheckpointMemoryLimits limits,
    NativeStorageMemory& persistent,const NativeStorageMemoryBinding& persistent_binding,
    NativeStorageMemory& transient,const NativeStorageMemoryBinding& transient_binding)noexcept {
  NativeOwnedWriterResult out;
  try{
    if(!transition)return out;
    // Reject inherited registry state before any wrapper allocation/callback.
    if(transition->ObserveDrain().state==disk::RouteSourceDrainState::wrong_process){
      out.error=NativeOwnedSourceError::route_admission;out.route_error=disk::RouteSourceTransitionError::wrong_process;return out;}
    // All wrapper allocation precedes the existing atomic cohort-consumption.
    auto writer=std::shared_ptr<NativeOwnedCheckpointWriter>(new NativeOwnedCheckpointWriter);
    writer->transition_=transition;
    auto adopted=NativeOwnedCheckpointSource::AdoptRouteWithMemoryImpl(transition,database,primary,secondary,
        limits,persistent,persistent_binding,transient,transient_binding,disk::RouteSourceAdmissionKind::initial_writer);
    static_cast<NativeOwnedSourceDiagnostics&>(out)=std::move(static_cast<NativeOwnedSourceDiagnostics&>(adopted));
    if(!adopted.ok())return out;
    writer->cohort_=std::move(adopted.owner);out.writer=std::move(writer);return out;
  }catch(const std::bad_alloc&){out.error=NativeOwnedSourceError::resource_exhausted;}
   catch(const std::system_error&){out.error=NativeOwnedSourceError::lock_failure;}
  return out;
}
inline NativeOwnedCheckpointWriter::OperationResult NativeOwnedCheckpointWriter::TryBeginOperation(
    std::shared_ptr<NativeOwnedCheckpointWriter> owner,const Uuid& identity)noexcept {
  OperationResult out;using E=NativeWriterError;
  if(!owner||!core::uuid::IsEngineIdentityUuid(identity))return out;
  if(!owner->InOwningProcess()){out.error=E::wrong_process;return out;}
  if(!owner->accepting_.load(std::memory_order_acquire)){out.error=E::withdrawn;return out;}
  // A reentrant attempt while this thread owns a selected-read device guard
  // must refuse BEFORE allocating another control object. CAS below still
  // provides the actual admission linearization under competing callers.
  if(owner->busy_.load(std::memory_order_acquire)){out.error=E::busy;return out;}
  try{
    auto state=std::make_shared<OperationState>();state->owner=owner;state->identity=identity;
    bool idle=false;if(!owner->busy_.compare_exchange_strong(idle,true,std::memory_order_acq_rel)){
      out.error=E::busy;return out;}
    state->registered=true;
    out.route_error=owner->transition_->CheckTransferredWriterAdmission();
    if(out.route_error!=disk::RouteSourceTransitionError::none){
      out.error=out.route_error==disk::RouteSourceTransitionError::wrong_process?E::wrong_process:E::withdrawn;return out;}
    if(!owner->accepting_.load(std::memory_order_acquire)){out.error=E::withdrawn;return out;}
    out.operation.state_=std::move(state);out.error=E::none;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
  return out;
}
inline NativeWriterCloseResult NativeOwnedCheckpointWriter::Close(std::shared_ptr<NativeOwnedCheckpointWriter> owner,
    std::span<NativeWriterMemberCloseReceipt> receipts)noexcept {
  NativeWriterCloseResult out;
  if(!owner)return out;
  if(!owner->InOwningProcess()){out.error=NativeWriterError::wrong_process;return out;}
  const auto& devices=owner->cohort_->memory_state_->devices;
  if(receipts.size()<devices.size()||!receipts.data()||
      receipts.size()>std::numeric_limits<std::size_t>::max()/sizeof(NativeWriterMemberCloseReceipt)||
      reinterpret_cast<std::uintptr_t>(receipts.data())>std::numeric_limits<std::uintptr_t>::max()-receipts.size_bytes())return out;
  owner->accepting_.store(false,std::memory_order_release);
  bool idle=false;if(!owner->busy_.compare_exchange_strong(idle,true,std::memory_order_acq_rel)){
    out.error=NativeWriterError::busy;return out;}
  struct Reservation {NativeOwnedCheckpointWriter& owner;~Reservation(){owner.busy_.store(false,std::memory_order_release);}} reservation{*owner};
  out.error=NativeWriterError::none;out.member_count=devices.size();out.all_members_retired=true;
  const auto remember=[&](const Uuid& filespace,u32 error){if(!owner->close_uncertain_){
    owner->close_uncertain_=true;owner->first_uncertain_filespace_=filespace;owner->first_uncertain_native_error_=error;}};
  for(usize n=0;n<devices.size();++n){auto& receipt=receipts[n];receipt={};const auto& member=devices[n];
    receipt.filespace_uuid=member.filespace_uuid;receipt.page_size_profile_uuid=member.page_size_profile_uuid;
    try{
      if(member.device->is_open()&&!member.device->read_only()){
        disk::FileDevice::BoundedIoBatch batch(*member.device);receipt.synchronization=batch.Sync();
        if(!receipt.synchronization->ok()){
          out.error=NativeWriterError::io_failure;out.all_members_retired=false;continue;}
      }
      const auto closed=member.device->CloseNativeWriterWithoutSync();
      const auto copy=[](const disk::FileDevice::NativeWriterHandleClose& native){NativeWriterHandleCloseReceipt r;
        using D=disk::FileDevice::NativeWriterCloseDisposition;using W=NativeWriterHandleCloseDisposition;
        switch(native.disposition){case D::not_present:r.disposition=W::not_present;break;
          case D::released:r.disposition=W::released;break;case D::retained:r.disposition=W::retained;break;
          case D::released_after_error:r.disposition=W::released_after_error;break;
          case D::indeterminate_quarantined:r.disposition=W::indeterminate_quarantined;break;}
        r.native_error=native.native_error;r.attempted=native.attempted;return r;};
      receipt.data=copy(closed.data);receipt.ownership=copy(closed.ownership);
      for(const auto& handle:{receipt.data,receipt.ownership}){
        if(handle.disposition==NativeWriterHandleCloseDisposition::retained)out.all_members_retired=false;
        if(handle.native_error){
          if(handle.disposition!=NativeWriterHandleCloseDisposition::retained)remember(member.filespace_uuid,handle.native_error);
          out.error=NativeWriterError::close_failure;}
      }
    }catch(const std::system_error& e){out.error=NativeWriterError::synchronization_failure;
      out.synchronization_native_error=e.code().value();out.all_members_retired=false;}
  }
  out.historical_close_uncertainty=owner->close_uncertain_;out.first_uncertain_filespace=owner->first_uncertain_filespace_;
  out.first_uncertain_native_error=owner->first_uncertain_native_error_;
  if(out.historical_close_uncertainty&&out.error==NativeWriterError::none)out.error=NativeWriterError::close_failure;
  return out;
}
inline NativeWriterError NativeOwnedCheckpointWriter::Operation::Check()const noexcept {
  if(!state_)return NativeWriterError::invalid_operation;
  if(!state_->owner->InOwningProcess())return NativeWriterError::wrong_process;
  if(state_->thread!=std::this_thread::get_id())std::terminate();
  return NativeWriterError::none;
}
inline void NativeOwnedCheckpointWriter::Operation::Reset()noexcept {
  if(state_&&state_->owner->InOwningProcess()&&state_->thread!=std::this_thread::get_id())std::terminate();
  state_.reset(); // A live restricted reader retains the same reservation.
}
inline const Uuid& NativeOwnedCheckpointWriter::Operation::identity()const noexcept {
  if(Check()!=NativeWriterError::none)std::terminate();return state_->identity;
}
inline const NativeWriterPhysicalEffects& NativeOwnedCheckpointWriter::Operation::effects()const noexcept {
  if(Check()!=NativeWriterError::none)std::terminate();return state_->effects;
}
inline void NativeOwnedCheckpointWriter::ReadLease::CheckAccess()const noexcept {
  if(state_&&(!state_->operation->owner->InOwningProcess()||state_->operation->thread!=std::this_thread::get_id()))std::terminate();
}
inline const NativeSelectedCheckpointMemoryLease& NativeOwnedCheckpointWriter::ReadLease::source()const noexcept {
  CheckAccess();if(!state_)std::terminate();return *state_->selected;
}
inline void NativeOwnedCheckpointWriter::ReadLease::Reset()noexcept {
  if(!state_)return;
  if(!state_->operation->owner->InOwningProcess()){
    // Same child-only close/abandon rule as the qualified native read owner.
    state_->operation.reset();(void)state_.release();return;
  }
  CheckAccess();state_.reset();
}
inline NativeOwnedCheckpointWriter::ReadResult NativeOwnedCheckpointWriter::Operation::ReadWithMemory(
    NativeSelectedCheckpointMemoryLimits limits,NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding)noexcept {
  ReadResult out;out.writer_error=Check();if(out.writer_error!=NativeWriterError::none)return out;
  // Retain custody before ANY allocation or governor/reporting callback. Such
  // callbacks may reset the caller's external Operation without releasing an
  // in-progress read or invalidating the actual source/device owner.
  const auto retained=state_;
  if(retained->reader_live){out.writer_error=NativeWriterError::read_handle_live;return out;}
  if(retained->io_active){out.writer_error=NativeWriterError::busy;return out;}
  try{
    const auto& cohort=*retained->owner->cohort_;
    if(!limits.maximum_backing_bytes||!limits.maximum_verification_image_bytes){out.writer_error=NativeWriterError::invalid_request;return out;}
    retained->reader_live=true;
    struct ReadReservation {OperationState& state;bool transferred=false;
      ~ReadReservation(){if(!transferred)state.reader_live=false;}} reservation{*retained};
    auto reader=std::make_unique<ReaderState>();reader->operation=retained;
    // From here the ReaderState alone retires its live flag, after selected
    // guards/backing. Do not clear it a second time after control deallocation
    // could have reentered this same operation and established another reader.
    reservation.transferred=true;
    out.memory_error=cohort.memory_state_->memory.CheckBinding(cohort.memory_state_->memory.binding());
    if(out.memory_error!=NativeStorageMemoryError::none){out.error=NativeOwnedSourceError::memory_failure;out.writer_error=NativeWriterError::source_failure;return out;}
    auto selected=AcquireNativeSelectedCheckpointMemoryLease(cohort.database_,cohort.memory_state_->devices,cohort.primary_,limits,memory,binding);
    out.FromMemorySource(selected);
    if(!selected.ok()){out.writer_error=NativeWriterError::source_failure;return out;}
    if(!NativeOwnedCheckpointSource::CompleteMemoryCohort(*selected.lease,cohort.memory_state_->devices.size())){
      out.error=NativeOwnedSourceError::incomplete_cohort;out.writer_error=NativeWriterError::source_failure;return out;}
    reader->selected=std::move(selected.lease);out.lease.state_=std::move(reader);
    out.error=NativeOwnedSourceError::none;out.writer_error=NativeWriterError::none;
  }catch(const std::bad_alloc&){out.error=NativeOwnedSourceError::resource_exhausted;out.writer_error=NativeWriterError::resource_exhausted;}
   catch(const std::system_error&){out.error=NativeOwnedSourceError::lock_failure;out.writer_error=NativeWriterError::synchronization_failure;}
  return out;
}
inline NativeWriterIoResult NativeOwnedCheckpointWriter::Io(Operation& operation,disk::BoundedIoOperation kind,
    const Uuid& filespace,const Uuid& profile,u64 offset,std::span<byte> output,std::span<const byte> input)noexcept {
  using E=NativeWriterError;NativeWriterIoResult out;out.error=operation.Check();out.filespace_uuid=filespace;
  if(out.error!=E::none)return out;
  // Batch destruction publishes optional observations. A reentrant callback
  // may reset the public Operation, but must not retire this call's devices or
  // invalidate IoUse's final io_active reset.
  const auto retained=operation.state_;
  auto& state=*retained;out.operation_uuid=state.identity;out.effects=state.effects;
  const auto fail=[&](E e){out.error=e;out.effects=state.effects;return out;};
  if(state.reader_live)return fail(E::read_handle_live);
  if(state.io_active)return fail(E::busy);
  const auto& devices=state.owner->cohort_->memory_state_->devices;
  const auto found=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.filespace_uuid==filespace;});
  if(found==devices.end())return fail(E::filespace_absent);
  if(found->page_size_profile_uuid!=profile)return fail(E::profile_mismatch);
  // Secondary/archive handles retain their actual immutable per-member OS
  // capability. Owning a writable primary never upgrades any other member.
  if(kind!=disk::BoundedIoOperation::read&&found->device->read_only())return fail(E::read_only_member);
  const auto count=kind==disk::BoundedIoOperation::read?output.size():input.size();
  const auto* pointer=kind==disk::BoundedIoOperation::read?static_cast<const void*>(output.data()):static_cast<const void*>(input.data());
  if(count&&(!pointer||reinterpret_cast<std::uintptr_t>(pointer)>std::numeric_limits<std::uintptr_t>::max()-count))return fail(E::invalid_buffer);
  if(kind!=disk::BoundedIoOperation::sync){out.extent_error=disk::CheckFileDeviceExtentNative(offset,count);
    if(out.extent_error!=disk::BoundedIoError::none)return fail(E::extent_invalid);}
  struct IoUse {OperationState& state;explicit IoUse(OperationState& s):state(s){state.io_active=true;}~IoUse(){state.io_active=false;}} use{state};
  const auto add=[&](u64& target,u64 n){if(n>std::numeric_limits<u64>::max()-target){target=UINT64_MAX;state.effects.counts_saturated=true;}else target+=n;};
  try{
    // Batch owns fixed receipts. Its internal operation guard is released
    // before its destructor publishes any allocating observation/callback.
    disk::FileDevice::BoundedIoBatch batch(*found->device);
    if(kind==disk::BoundedIoOperation::write){
      out.size_observation=batch.Size();if(out.size_observation->native_attempted)add(state.effects.size_attempts,1);
      if(!out.size_observation->ok())return fail(E::io_failure);
      if(offset>out.size_observation->size_bytes||count>out.size_observation->size_bytes-offset)return fail(E::extent_outside_file);
      out.io=batch.WriteAt(offset,input.data(),count);
      if(out.io->native_attempted){add(state.effects.write_attempts,1);add(state.effects.write_bytes,out.io->bytes_transferred);
        if(!out.io->ok())state.effects.uncertain_write=true;}
    }else if(kind==disk::BoundedIoOperation::read){out.io=batch.ReadAt(offset,output.data(),count);
      if(out.io->native_attempted){add(state.effects.read_attempts,1);add(state.effects.read_bytes,out.io->bytes_transferred);}
    }else {out.io=batch.Sync();if(out.io->native_attempted)add(state.effects.sync_attempts,1);
      if(out.io->synchronized)add(state.effects.successful_syncs,1);}
    return fail(out.io->ok()?E::none:E::io_failure);
  }catch(const std::system_error& e){out.synchronization_native_error=e.code().value();return fail(E::synchronization_failure);}
}
inline NativeWriterIoResult NativeOwnedCheckpointWriter::Operation::ReadAtBounded(const Uuid& f,const Uuid& p,u64 at,std::span<byte> output)noexcept {
  return Io(*this,disk::BoundedIoOperation::read,f,p,at,output,{});
}
inline NativeWriterIoResult NativeOwnedCheckpointWriter::Operation::WriteAtBounded(const Uuid& f,const Uuid& p,u64 at,std::span<const byte> input)noexcept {
  return Io(*this,disk::BoundedIoOperation::write,f,p,at,{},input);
}
inline NativeWriterIoResult NativeOwnedCheckpointWriter::Operation::Sync(const Uuid& f,const Uuid& p)noexcept {
  return Io(*this,disk::BoundedIoOperation::sync,f,p,0,{},{});
}
} // namespace scratchbird::storage::database
