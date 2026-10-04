// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_selected_checkpoint_read_lease.hpp"
#include "native_selected_checkpoint_memory_lease.hpp"
#include "route_ownership_lease.hpp"
#include <atomic>

namespace scratchbird::storage::database {
class NativeOwnedCheckpointSource;
enum class NativeOwnedSourceError {
  none, invalid_request, device_ownership, bootstrap_failure, source_failure,
  resource_exhausted, lock_failure, withdrawn, wrong_process, io_failure, route_admission,
  incomplete_cohort, memory_failure, memory_required
};
class NativeOwnedCheckpointReadLease {
 public:
  NativeOwnedCheckpointReadLease(const NativeOwnedCheckpointReadLease&)=delete;
  NativeOwnedCheckpointReadLease& operator=(const NativeOwnedCheckpointReadLease&)=delete;
  const NativeSelectedCheckpointReadLease& source() const noexcept { return *source_; }
  const std::vector<disk::NativeFilespaceDevice>& devices() const noexcept;
 private:
  friend class NativeOwnedCheckpointSource;
  NativeOwnedCheckpointReadLease()=default;
  // Unlock source guards BEFORE the last owner can destroy device mutexes.
  std::shared_ptr<const NativeOwnedCheckpointSource> owner_;
  std::unique_ptr<NativeSelectedCheckpointReadLease> source_;
};
struct NativeOwnedSourceDiagnostics {
  NativeOwnedSourceError error=NativeOwnedSourceError::invalid_request;
  disk::FilespaceBootstrapError bootstrap_error=disk::FilespaceBootstrapError::none;
  NativeSelectedCheckpointReadError source_error=NativeSelectedCheckpointReadError::none;
  NativeCheckpointSelectionError selection_error=NativeCheckpointSelectionError::none;
  NativeCheckpointError checkpoint_error=NativeCheckpointError::none;
  page::NativeAllocationError allocation_error=page::NativeAllocationError::none;
  page::NativeDirectoryError directory_error=page::NativeDirectoryError::none;
  page::NativeInventoryError inventory_error=page::NativeInventoryError::none;
  disk::RouteSourceTransitionError route_error=disk::RouteSourceTransitionError::none;
  NativeSelectedCheckpointMemoryError memory_source_error=NativeSelectedCheckpointMemoryError::none;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  core::platform::Status allocation_status,io_status;
  core::platform::DiagnosticRecord allocation_diagnostic,io_diagnostic;
  u64 physical_bytes_read=0;
  void FromMemorySource(NativeSelectedCheckpointMemoryResult& r) noexcept {
    error=NativeOwnedSourceError::source_failure;memory_source_error=r.error;memory_error=r.memory_error;
    selection_error=r.selection_error;checkpoint_error=r.checkpoint_error;allocation_error=r.allocation_error;
    directory_error=r.directory_error;inventory_error=r.inventory_error;
    allocation_status=r.allocation_status;allocation_diagnostic=std::move(r.allocation_diagnostic);
    io_status=r.io_status;io_diagnostic=std::move(r.io_diagnostic);physical_bytes_read+=r.physical_bytes_read;
  }
  void FromSource(const NativeSelectedCheckpointReadResult& r) noexcept {
    error=NativeOwnedSourceError::source_failure;source_error=r.error;
    selection_error=r.selection_error;checkpoint_error=r.checkpoint_error;
    allocation_error=r.allocation_error;directory_error=r.directory_error;
    inventory_error=r.inventory_error;
  }
};
struct NativeOwnedSourceResult : NativeOwnedSourceDiagnostics {
  std::shared_ptr<NativeOwnedCheckpointSource> owner;
  bool ok() const noexcept { return error==NativeOwnedSourceError::none&&owner!=nullptr; }
};
struct NativeOwnedSourceReadResult : NativeOwnedSourceDiagnostics {
  std::unique_ptr<NativeOwnedCheckpointReadLease> lease;
  bool ok() const noexcept { return error==NativeOwnedSourceError::none&&lease!=nullptr; }
};


struct NativeOwnedCheckpointMemoryLimits {
  std::size_t maximum_owner_backing_bytes=0;
  NativeSelectedCheckpointMemoryLimits source;
};
struct NativeOwnedCheckpointMemoryState {
  // Array elements die before backing; backing dies before its reusable grant.
  NativeStorageMemory memory;
  NativeStorageArena arena;
  std::span<std::unique_ptr<disk::FileDevice>> owned;
  std::span<disk::NativeFilespaceDevice> devices;
  std::size_t backing_bytes_used=0;
  ~NativeOwnedCheckpointMemoryState(){for(auto& file:owned)std::destroy_at(&file);}
};
class NativeOwnedCheckpointMemoryHandle;
struct NativeOwnedSourceMemoryReadResult;
// Defined below the owning source, so cleanup can reject inherited callbacks.
struct NativeOwnedCheckpointMemoryReadState {
  std::shared_ptr<const NativeOwnedCheckpointSource> owner;
  NativeSelectedCheckpointMemoryHandle source;
};
class NativeOwnedCheckpointMemoryHandle {
 public:
  NativeOwnedCheckpointMemoryHandle() noexcept=default;
  NativeOwnedCheckpointMemoryHandle(const NativeOwnedCheckpointMemoryHandle&)=delete;
  NativeOwnedCheckpointMemoryHandle& operator=(const NativeOwnedCheckpointMemoryHandle&)=delete;
  NativeOwnedCheckpointMemoryHandle(NativeOwnedCheckpointMemoryHandle&& other) noexcept;
  NativeOwnedCheckpointMemoryHandle& operator=(NativeOwnedCheckpointMemoryHandle&& other) noexcept;
  ~NativeOwnedCheckpointMemoryHandle(){Reset();}
  explicit operator bool() const noexcept;
  const NativeSelectedCheckpointMemoryLease& source() const noexcept;
  std::span<const disk::NativeFilespaceDevice> devices() const noexcept;
  void Reset() noexcept;
 private:
  void CheckAccess() const noexcept;
  std::unique_ptr<NativeOwnedCheckpointMemoryReadState> state_;
  friend class NativeOwnedCheckpointSource;
};
struct NativeOwnedSourceMemoryReadResult : NativeOwnedSourceDiagnostics {
  NativeOwnedCheckpointMemoryHandle lease;
  bool ok() const noexcept{return error==NativeOwnedSourceError::none&&bool(lease);}
};

// Owns a native source, not serving readiness or configuration activation.
// Admission consumes exclusively owned open devices only on success. No paths
// are reopened, no supplied UUID/profile replaces the actual bootstrap bytes.
// Route-borrowed devices require their owning runtime integration; this API
// must never detach that route's thread-affine serialization guard.
class NativeOwnedCheckpointSource {
 public:
  NativeOwnedCheckpointSource(const NativeOwnedCheckpointSource&)=delete;
  NativeOwnedCheckpointSource& operator=(const NativeOwnedCheckpointSource&)=delete;

  ~NativeOwnedCheckpointSource(){
    if(memory_state_&&!InOwningProcess()){
      // No inherited operation/provider mutex may be destroyed or entered.
      // Only close duplicate native descriptors; the child exits/execs without
      // touching the parent's locks, callbacks or accounting.
      for(auto& file:memory_state_->owned)if(file){
        if(!file->AbandonInheritedSource())std::terminate();
        (void)file.release();
      }
      (void)memory_state_.release();
    }
  }
  bool memory_backed() const noexcept {return bool(memory_state_);}
  std::size_t owner_backing_bytes_used() const noexcept {
    return memory_state_?memory_state_->backing_bytes_used:0;
  }
  static NativeOwnedSourceResult AdoptWithMemory(const Uuid&,const Uuid&,
      std::vector<std::unique_ptr<disk::FileDevice>>&,
      NativeOwnedCheckpointMemoryLimits,NativeStorageMemory&,const NativeStorageMemoryBinding&,
      NativeStorageMemory&,const NativeStorageMemoryBinding&) noexcept;
  static NativeOwnedSourceResult AdoptRouteWithMemory(
      const std::shared_ptr<disk::RouteSourceTransition>&,const Uuid&,const Uuid&,
      std::vector<std::unique_ptr<disk::FileDevice>>&,
      NativeOwnedCheckpointMemoryLimits,NativeStorageMemory&,const NativeStorageMemoryBinding&,
      NativeStorageMemory&,const NativeStorageMemoryBinding&) noexcept;
  static NativeOwnedSourceMemoryReadResult ReadWithMemory(
      const std::shared_ptr<const NativeOwnedCheckpointSource>&,
      NativeSelectedCheckpointMemoryLimits,NativeStorageMemory&,
      const NativeStorageMemoryBinding&) noexcept;

  const Uuid& database_uuid() const noexcept { return database_; }
  const Uuid& primary_uuid() const noexcept { return primary_; }
  // Does not wait or close in-flight sources. Callers admitted before this
  // linearization point retain ownership until their read lease is destroyed.
  void Withdraw() noexcept { accepting_.exchange(false); }
  static NativeOwnedSourceResult Adopt(const Uuid& database,const Uuid& primary,
      std::vector<std::unique_ptr<disk::FileDevice>>& supplied,u64 budget) noexcept {
    using E=NativeOwnedSourceError;
    NativeOwnedSourceResult out;
    try {
      if(!core::uuid::IsEngineIdentityUuid(database)||
          !core::uuid::IsEngineIdentityUuid(primary)||supplied.empty()||!budget)return out;
      // Exclusive unique ownership is a caller precondition. Check process and
      // opening thread BEFORE touching mutexes possibly inherited across fork.
      for(const auto& f:supplied)if(!f||!f->CanAdoptIndependentSource()){
        out.error=E::device_ownership;return out;
      }
      auto owner=std::shared_ptr<NativeOwnedCheckpointSource>(new NativeOwnedCheckpointSource);
      owner->database_=database;owner->primary_=primary;
      owner->process_=disk::FileDevice::SourceOwnershipProcessId();
      std::vector<std::unique_lock<std::recursive_mutex>> guards;
      guards.reserve(supplied.size());owner->devices_.reserve(supplied.size());
      // No other consumer may possess supplied devices during adoption. Later
      // source reads always use binary filespace ordering through the read API.
      for(const auto& f:supplied){
        guards.push_back(f->AcquireOperationGuard());
        const auto bootstrap=disk::ReadFilespaceBootstrapFromOpenDevice(*f);
        if(!bootstrap.ok()){
          out.error=E::bootstrap_failure;out.bootstrap_error=bootstrap.error;return out;
        }
        const auto& b=*bootstrap.preamble;
        if(b.database_uuid!=database){out.error=E::bootstrap_failure;
          out.bootstrap_error=disk::FilespaceBootstrapError::binding_mismatch;return out;}
        owner->devices_.push_back({b.filespace_uuid,b.page_size_profile_uuid,f.get()});
      }
      auto selected=AcquireNativeSelectedCheckpointReadLease(database,owner->devices_,primary,budget);
      if(!selected.ok()){out.FromSource(selected);return out;}
      if(!CompleteCohort(*selected.lease,owner->devices_.size())){
        out.error=E::incomplete_cohort;return out;
      }
      // Everything which allocates or can refuse precedes this ownership move.
      // Each file keeps its actual exclusive OS locks. The legacy per-path
      // recursive guard is unnecessary for independently owned devices, and
      // must be released ON ITS OPENING THREAD before cross-thread lifetime.
      for(const auto& f:supplied)f->route_owner_storage_guard_.unlock();
      owner->owned_.swap(supplied);
      out.owner=std::move(owner);out.error=E::none;return out;
    }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
     catch(const std::length_error&){out.error=E::resource_exhausted;}
     catch(const std::system_error&){out.error=E::lock_failure;}
     catch(...){out.error=E::io_failure;}
    return out;
  }
  static NativeOwnedSourceReadResult Read(
      const std::shared_ptr<const NativeOwnedCheckpointSource>& owner,u64 budget) noexcept {
    using E=NativeOwnedSourceError;
    NativeOwnedSourceReadResult out;
    try {
      if(!owner||!budget)return out;
      if(owner->process_!=disk::FileDevice::SourceOwnershipProcessId()){
        out.error=E::wrong_process;return out;
      }
      // Admission is linearized here; no scheduler or admission mutex spans I/O.
      if(!owner->accepting_.load()){out.error=E::withdrawn;return out;}
      if(owner->memory_state_){out.error=E::memory_required;return out;}
      std::unique_ptr<NativeOwnedCheckpointReadLease> lease(new NativeOwnedCheckpointReadLease);
      lease->owner_=owner;
      auto selected=AcquireNativeSelectedCheckpointReadLease(
          owner->database_,owner->devices_,owner->primary_,budget);
      if(!selected.ok()){out.FromSource(selected);return out;}
      if(!CompleteCohort(*selected.lease,owner->devices_.size())){
        out.error=E::incomplete_cohort;return out;
      }
      lease->source_=std::move(selected.lease);
      out.lease=std::move(lease);out.error=E::none;return out;
    }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
     catch(const std::length_error&){out.error=E::resource_exhausted;}
     catch(const std::system_error&){out.error=E::lock_failure;}
     catch(...){out.error=E::io_failure;}
    return out;
  }
  // Read-source admission, not a writable mount or a security/configuration
  // grant. The primary comes from the owner's retained physical object, never
  // a path. Secondary handles must be independently owned on this thread and
  // are consumed ONLY after complete native validation and live-owner commit.
  static NativeOwnedSourceResult AdoptRoute(
      const std::shared_ptr<disk::RouteSourceTransition>& transition,
      const Uuid& database,const Uuid& primary,
      std::vector<std::unique_ptr<disk::FileDevice>>& secondary,u64 budget) noexcept {
    using E=NativeOwnedSourceError;
    NativeOwnedSourceResult out;
    try {
      if(!transition||!core::uuid::IsEngineIdentityUuid(database)||
          !core::uuid::IsEngineIdentityUuid(primary)||!budget)return out;
      for(const auto& f:secondary)if(!f||!f->CanAdoptIndependentSource()){
        out.error=E::device_ownership;return out;
      }
      auto retained=transition;
      std::unique_ptr<disk::FileDevice> primary_device;
      out.route_error=retained->BeginSourceAdmission(primary_device);
      if(out.route_error!=disk::RouteSourceTransitionError::none){out.error=E::route_admission;return out;}
      // No caller may obtain a reservation token or half-validated primary.
      // Rollback runs after all local guards/devices, and never reopens legacy
      // admission. Only this successful reservation may clear its busy state.
      const auto abort=[](disk::RouteSourceTransition* t){t->AbortSourceAdmission();};
      std::unique_ptr<disk::RouteSourceTransition,decltype(abort)> rollback(retained.get(),abort);
      auto owner=std::shared_ptr<NativeOwnedCheckpointSource>(new NativeOwnedCheckpointSource);
      owner->database_=database;owner->primary_=primary;
      owner->process_=disk::FileDevice::SourceOwnershipProcessId();
      if(secondary.size()==secondary.max_size()){out.error=E::resource_exhausted;return out;}
      const auto count=secondary.size()+1;
      owner->owned_.resize(count);owner->devices_.reserve(count);
      owner->owned_[0]=std::move(primary_device);
      std::vector<std::unique_lock<std::recursive_mutex>> guards;guards.reserve(count);
      for(std::size_t i=0;i<count;++i){
        auto* f=i?secondary[i-1].get():owner->owned_[0].get();
        guards.push_back(f->AcquireOperationGuard());
        const auto bootstrap=disk::ReadFilespaceBootstrapFromOpenDevice(*f);
        if(!bootstrap.ok()){
          out.error=E::bootstrap_failure;out.bootstrap_error=bootstrap.error;return out;
        }
        const auto& b=*bootstrap.preamble;
        if(b.database_uuid!=database||(!i&&b.filespace_uuid!=primary)){
          out.error=E::bootstrap_failure;out.bootstrap_error=disk::FilespaceBootstrapError::binding_mismatch;return out;
        }
        owner->devices_.push_back({b.filespace_uuid,b.page_size_profile_uuid,f});
      }
      auto selected=AcquireNativeSelectedCheckpointReadLease(database,owner->devices_,primary,budget);
      if(!selected.ok()){out.FromSource(selected);return out;}
      if(!CompleteCohort(*selected.lease,owner->devices_.size())){
        out.error=E::incomplete_cohort;return out;
      }
      // No registry/scheduler mutex spans native I/O. Release may have revoked
      // the issuer during validation; commit checks that actual state again.
      out.route_error=retained->CommitSourceAdmission();
      if(out.route_error!=disk::RouteSourceTransitionError::none){out.error=E::route_admission;return out;}
      (void)rollback.release();
      // All fallible work precedes the single-consumption point. Slots already
      // exist; unique ownership moves and guard release are nonallocating.
      for(std::size_t i=0;i<secondary.size();++i){
        secondary[i]->route_owner_storage_guard_=std::unique_lock<std::recursive_mutex>{};
        owner->owned_[i+1]=std::move(secondary[i]);
      }
      secondary.clear();out.owner=std::move(owner);out.error=E::none;return out;
    }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
     catch(const std::length_error&){out.error=E::resource_exhausted;}
     catch(const std::system_error&){out.error=E::lock_failure;}
     catch(...){out.error=E::io_failure;}
    return out;
  }
 private:
  friend class NativeOwnedCheckpointReadLease;

  friend class NativeOwnedCheckpointMemoryHandle;
  bool InOwningProcess() const noexcept {return process_==disk::FileDevice::SourceOwnershipProcessId();}
  static bool CheckMemory(const Uuid& database,NativeStorageMemory& memory,
      const NativeStorageMemoryBinding& binding,NativeOwnedSourceDiagnostics& out){
    out.memory_error=memory.CheckBinding(binding);
    if(out.memory_error==NativeStorageMemoryError::none&&binding.database_uuid!=database)
      out.memory_error=NativeStorageMemoryError::invalid_binding;
    if(out.memory_error!=NativeStorageMemoryError::none){
      out.error=NativeOwnedSourceError::memory_failure;return false;
    }
    return true;
  }
  static std::shared_ptr<NativeOwnedCheckpointSource> PrepareMemoryOwner(
      const Uuid& database,const Uuid& primary,std::size_t count,std::size_t bytes,
      NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding,NativeOwnedSourceDiagnostics& out){
    using P=std::unique_ptr<disk::FileDevice>;
    if(count>std::numeric_limits<std::size_t>::max()/sizeof(P))throw std::bad_alloc();
    auto owner=std::shared_ptr<NativeOwnedCheckpointSource>(new NativeOwnedCheckpointSource);
    owner->database_=database;owner->primary_=primary;owner->process_=disk::FileDevice::SourceOwnershipProcessId();
    owner->memory_state_=std::make_unique<NativeOwnedCheckpointMemoryState>();
    auto allocated=memory.CreateArena(binding,bytes,alignof(std::max_align_t));
    out.memory_error=allocated.error;out.allocation_status=allocated.backing.status;
    out.allocation_diagnostic=std::move(allocated.backing.diagnostic);
    if(!allocated.ok()){out.error=NativeOwnedSourceError::memory_failure;return {};}
    auto& state=*owner->memory_state_;state.arena=std::move(allocated.arena);
    const auto block=state.arena.Allocate(bytes,alignof(std::max_align_t));
    if(!block.ok()){out.error=NativeOwnedSourceError::resource_exhausted;return {};}
    detail::NativeMetadataMemory resource({static_cast<byte*>(block.pointer),block.bytes});
    auto* pointers=static_cast<P*>(resource.allocate(count*sizeof(P),alignof(P)));
    std::uninitialized_value_construct_n(pointers,count);state.owned={pointers,count};
    detail::NativeMetadataScratch scratch{&resource};
    state.devices=scratch.Array<disk::NativeFilespaceDevice>(count);
    state.backing_bytes_used=resource.used();return owner;
  }
  static bool InspectMemoryBootstrap(disk::FileDevice& file,const Uuid& database,
      disk::NativeFilespaceDevice& member,NativeOwnedSourceDiagnostics& out){
    // Exclusive ownership was checked before this inspection. Scope each
    // batch outside its guard; full cohort selection revalidates afterwards.
    disk::FileDevice::ReadLatencyBatch batch(file);
    const auto guard=file.AcquireOperationGuard();
    if(!file.is_open()){out.error=NativeOwnedSourceError::bootstrap_failure;
      out.bootstrap_error=disk::FilespaceBootstrapError::device_not_open;return false;}
    disk::SerializedFilespaceBootstrap bytes{};
    auto read=batch.ReadAt(0,bytes.data(),bytes.size());
    out.physical_bytes_read+=read.bytes_transferred;
    out.io_status=read.status;out.io_diagnostic=std::move(read.diagnostic);
    if(!read.ok()||read.bytes_transferred!=bytes.size()){
      out.error=NativeOwnedSourceError::bootstrap_failure;
      out.bootstrap_error=disk::FilespaceBootstrapError::io_failure;return false;
    }
    const auto decoded=disk::DecodeFilespaceBootstrap(bytes.data(),bytes.size());
    if(!decoded.ok()){out.error=NativeOwnedSourceError::bootstrap_failure;
      out.bootstrap_error=decoded.error;return false;}
    const auto& b=*decoded.preamble;
    if(b.database_uuid!=database){out.error=NativeOwnedSourceError::bootstrap_failure;
      out.bootstrap_error=disk::FilespaceBootstrapError::binding_mismatch;return false;}
    member={b.filespace_uuid,b.page_size_profile_uuid,&file};return true;
  }
  static bool CompleteMemoryCohort(const NativeSelectedCheckpointMemoryLease& source,std::size_t count) noexcept {
    return source.directory().directory.pages.front().directory.total_records==count;
  }
  std::unique_ptr<NativeOwnedCheckpointMemoryState> memory_state_;

  NativeOwnedCheckpointSource()=default;
  static bool CompleteCohort(const NativeSelectedCheckpointReadLease& selected,std::size_t count) noexcept {
    // The underlying reader proves unique supplied members and their exact
    // directory bindings, but deliberately permits partial readers. An owning
    // source additionally requires every committed member to be retained.
    return selected.directory().directory.pages.front().directory->total_records==count;
  }
  std::vector<std::unique_ptr<disk::FileDevice>> owned_;
  std::vector<disk::NativeFilespaceDevice> devices_;
  Uuid database_,primary_;
  u64 process_=0;
  std::atomic<bool> accepting_{true};
};
inline const std::vector<disk::NativeFilespaceDevice>&
NativeOwnedCheckpointReadLease::devices() const noexcept { return owner_->devices_; }

inline void NativeOwnedCheckpointMemoryHandle::CheckAccess() const noexcept {
  if(!state_)return;
  if(!state_->owner->InOwningProcess())std::terminate();
  (void)bool(state_->source); // Enforce the selected handle's acquiring thread.
}
inline NativeOwnedCheckpointMemoryHandle::NativeOwnedCheckpointMemoryHandle(
    NativeOwnedCheckpointMemoryHandle&& other) noexcept {
  other.CheckAccess();state_=std::move(other.state_);
}
inline NativeOwnedCheckpointMemoryHandle& NativeOwnedCheckpointMemoryHandle::operator=(
    NativeOwnedCheckpointMemoryHandle&& other) noexcept {
  if(this!=&other){other.CheckAccess();Reset();state_=std::move(other.state_);}return *this;
}
inline NativeOwnedCheckpointMemoryHandle::operator bool() const noexcept {
  CheckAccess();return state_&&bool(state_->source);
}
inline const NativeSelectedCheckpointMemoryLease& NativeOwnedCheckpointMemoryHandle::source() const noexcept {
  CheckAccess();return *state_->source;
}
inline std::span<const disk::NativeFilespaceDevice> NativeOwnedCheckpointMemoryHandle::devices() const noexcept {
  CheckAccess();return state_->owner->memory_state_->devices;
}
inline void NativeOwnedCheckpointMemoryHandle::Reset() noexcept {
  if(!state_)return;
  if(!state_->owner->InOwningProcess()){
    // Do not destroy inherited selected guards, batches, arena or providers.
    // Retire the owner pin so its child-only descriptor cleanup can run.
    state_->owner.reset();(void)state_.release();return;
  }
  CheckAccess();state_->source.Reset();state_->owner.reset();state_.reset();
}
inline NativeOwnedSourceResult NativeOwnedCheckpointSource::AdoptWithMemory(
    const Uuid& database,const Uuid& primary,std::vector<std::unique_ptr<disk::FileDevice>>& supplied,
    NativeOwnedCheckpointMemoryLimits limits,NativeStorageMemory& persistent,const NativeStorageMemoryBinding& persistent_binding,
    NativeStorageMemory& transient,const NativeStorageMemoryBinding& transient_binding) noexcept {
  using E=NativeOwnedSourceError;NativeOwnedSourceResult out;
  try {
    if(!core::uuid::IsEngineIdentityUuid(database)||!core::uuid::IsEngineIdentityUuid(primary)||
        supplied.empty()||&persistent==&transient||!limits.maximum_owner_backing_bytes||
        !limits.source.maximum_verification_image_bytes||!limits.source.maximum_backing_bytes)return out;
    // Reject inherited/foreign-thread devices before touching either governor.
    for(const auto& file:supplied)if(!file||!file->CanAdoptIndependentSource()){
      out.error=E::device_ownership;return out;
    }
    if(!CheckMemory(database,persistent,persistent_binding,out)||
        !CheckMemory(database,transient,transient_binding,out))return out;
    auto owner=PrepareMemoryOwner(database,primary,supplied.size(),limits.maximum_owner_backing_bytes,
        persistent,persistent_binding,out);
    if(!owner)return out;
    auto& state=*owner->memory_state_;
    for(std::size_t i=0;i<supplied.size();++i)
      if(!InspectMemoryBootstrap(*supplied[i],database,state.devices[i],out))return out;
    auto selected=AcquireNativeSelectedCheckpointMemoryLease(database,state.devices,primary,
        limits.source,transient,transient_binding);
    out.FromMemorySource(selected);
    if(!selected.ok())return out;
    if(!CompleteMemoryCohort(*selected.lease,state.devices.size())){out.error=E::incomplete_cohort;return out;}
    // No fallible work after this point. Keep every actual independent OS lock.
    for(std::size_t i=0;i<supplied.size();++i){
      supplied[i]->route_owner_storage_guard_.unlock();state.owned[i]=std::move(supplied[i]);
    }
    supplied.clear();state.memory=std::move(persistent);
    out.owner=std::move(owner);out.error=E::none;return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
   catch(...){out.error=E::io_failure;}
  return out;
}
inline NativeOwnedSourceMemoryReadResult NativeOwnedCheckpointSource::ReadWithMemory(
    const std::shared_ptr<const NativeOwnedCheckpointSource>& owner,NativeSelectedCheckpointMemoryLimits limits,
    NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding) noexcept {
  using E=NativeOwnedSourceError;NativeOwnedSourceMemoryReadResult out;
  try{
    if(!owner||!limits.maximum_verification_image_bytes||!limits.maximum_backing_bytes)return out;
    if(!owner->InOwningProcess()){out.error=E::wrong_process;return out;}
    if(!owner->accepting_.load()){out.error=E::withdrawn;return out;}
    if(!owner->memory_state_){out.error=E::memory_required;return out;}
    const auto& state=*owner->memory_state_;
    out.memory_error=state.memory.CheckBinding(state.memory.binding());
    if(out.memory_error!=NativeStorageMemoryError::none){out.error=E::memory_failure;return out;}
    auto retained=std::make_unique<NativeOwnedCheckpointMemoryReadState>();retained->owner=owner;
    auto selected=AcquireNativeSelectedCheckpointMemoryLease(owner->database_,state.devices,owner->primary_,
        limits,memory,binding);
    out.FromMemorySource(selected);
    if(!selected.ok())return out;
    if(!CompleteMemoryCohort(*selected.lease,state.devices.size())){out.error=E::incomplete_cohort;return out;}
    retained->source=std::move(selected.lease);
    out.lease.state_=std::move(retained);out.error=E::none;return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
   catch(...){out.error=E::io_failure;}
  return out;
}
inline NativeOwnedSourceResult NativeOwnedCheckpointSource::AdoptRouteWithMemory(
    const std::shared_ptr<disk::RouteSourceTransition>& transition,const Uuid& database,const Uuid& primary,
    std::vector<std::unique_ptr<disk::FileDevice>>& secondary,NativeOwnedCheckpointMemoryLimits limits,
    NativeStorageMemory& persistent,const NativeStorageMemoryBinding& persistent_binding,
    NativeStorageMemory& transient,const NativeStorageMemoryBinding& transient_binding) noexcept {
  using E=NativeOwnedSourceError;NativeOwnedSourceResult out;
  try{
    if(!transition||&persistent==&transient||!core::uuid::IsEngineIdentityUuid(database)||!core::uuid::IsEngineIdentityUuid(primary)||
        !limits.maximum_owner_backing_bytes||!limits.source.maximum_verification_image_bytes||
        !limits.source.maximum_backing_bytes)return out;
    for(const auto& file:secondary)if(!file||!file->CanAdoptIndependentSource()){
      out.error=E::device_ownership;return out;
    }
    // The private transition itself checks its actual process before locks.
    auto retained=transition;std::unique_ptr<disk::FileDevice> primary_device;
    out.route_error=retained->BeginSourceAdmission(primary_device);
    if(out.route_error!=disk::RouteSourceTransitionError::none){out.error=E::route_admission;return out;}
    const auto abort=[&primary_device](disk::RouteSourceTransition* t){
      primary_device.reset();t->AbortSourceAdmission();
    };
    std::unique_ptr<disk::RouteSourceTransition,decltype(abort)> rollback(retained.get(),abort);
    if(!CheckMemory(database,persistent,persistent_binding,out)||
        !CheckMemory(database,transient,transient_binding,out))return out;
    if(secondary.size()==secondary.max_size()){out.error=E::resource_exhausted;return out;}
    auto owner=PrepareMemoryOwner(database,primary,secondary.size()+1,limits.maximum_owner_backing_bytes,
        persistent,persistent_binding,out);
    if(!owner)return out;
    auto& state=*owner->memory_state_;state.owned[0]=std::move(primary_device);
    for(std::size_t i=0;i<state.devices.size();++i)
      if(!InspectMemoryBootstrap(i?*secondary[i-1]:*state.owned[0],database,state.devices[i],out))return out;
    if(state.devices[0].filespace_uuid!=primary){out.error=E::bootstrap_failure;
      out.bootstrap_error=disk::FilespaceBootstrapError::binding_mismatch;return out;}
    auto selected=AcquireNativeSelectedCheckpointMemoryLease(database,state.devices,primary,
        limits.source,transient,transient_binding);
    out.FromMemorySource(selected);
    if(!selected.ok())return out;
    if(!CompleteMemoryCohort(*selected.lease,state.devices.size())){out.error=E::incomplete_cohort;return out;}
    out.route_error=retained->CommitSourceAdmission();
    if(out.route_error!=disk::RouteSourceTransitionError::none){out.error=E::route_admission;return out;}
    (void)rollback.release();
    for(std::size_t i=0;i<secondary.size();++i){
      secondary[i]->route_owner_storage_guard_.unlock();state.owned[i+1]=std::move(secondary[i]);
    }
    secondary.clear();state.memory=std::move(persistent);
    out.owner=std::move(owner);out.error=E::none;return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
   catch(...){out.error=E::io_failure;}
  return out;
}

} // namespace scratchbird::storage::database
