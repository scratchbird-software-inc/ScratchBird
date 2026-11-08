// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_fixed_root_memory_lease.hpp"
#include "route_ownership_lease.hpp"
#include "database_format.hpp"
#include "startup_state.hpp"
#include <atomic>

namespace scratchbird::storage::database {
class NativeOwnedCheckpointSource;
class NativeOwnedCheckpointWriter;
enum class NativeOwnedSourceError {
  none, invalid_request, device_ownership, bootstrap_failure, source_failure,
  resource_exhausted, lock_failure, withdrawn, wrong_process, io_failure, route_admission,
  incomplete_cohort, memory_failure, memory_required
};
struct NativeOwnedSourceDiagnostics {
  NativeOwnedSourceError error=NativeOwnedSourceError::invalid_request;
  disk::FilespaceBootstrapError bootstrap_error=disk::FilespaceBootstrapError::none;
  page::NativeAllocationError allocation_error=page::NativeAllocationError::none;
  page::NativeDirectoryError directory_error=page::NativeDirectoryError::none;
  page::NativeInventoryError inventory_error=page::NativeInventoryError::none;
  disk::RouteSourceTransitionError route_error=disk::RouteSourceTransitionError::none;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  // SB_NATIVE_FIXED_ROOT_READ: findings of a read through page zero's roots.
  NativeFixedRootMemoryError fixed_root_error=NativeFixedRootMemoryError::none;
  disk::FilespacePageZeroError page_zero_error=disk::FilespacePageZeroError::none;
  std::optional<u16> fixed_root_kind;
  core::platform::Status allocation_status,io_status;
  core::platform::DiagnosticRecord allocation_diagnostic,io_diagnostic;
  u64 physical_bytes_read=0;
  std::optional<disk::BoundedIoResult> io_failure;
  void FromFixedRootSource(NativeFixedRootMemoryResult& r) noexcept {
    error=NativeOwnedSourceError::source_failure;fixed_root_error=r.error;memory_error=r.memory_error;
    page_zero_error=r.page_zero_error;inventory_error=r.inventory_error;allocation_error=r.allocation_error;
    directory_error=r.directory_error;fixed_root_kind=r.root_kind;
    allocation_status=r.allocation_status;allocation_diagnostic=std::move(r.allocation_diagnostic);
    io_status=r.io_status;io_diagnostic=std::move(r.io_diagnostic);physical_bytes_read+=r.physical_bytes_read;
  }
};
struct NativeOwnedSourceResult : NativeOwnedSourceDiagnostics {
  std::shared_ptr<NativeOwnedCheckpointSource> owner;
  bool ok() const noexcept { return error==NativeOwnedSourceError::none&&owner!=nullptr; }
};
struct NativeOwnedSourceBootstrapResult : NativeOwnedSourceDiagnostics {
  disk::BoundedIoResult io;
  std::optional<disk::FilespaceBootstrap> bootstrap;
  bool ok() const noexcept {
    return error==NativeOwnedSourceError::none&&bootstrap.has_value();
  }
};


struct NativeOwnedCheckpointMemoryLimits {
  std::size_t maximum_owner_backing_bytes=0;
  NativeFixedRootMemoryLimits source;
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
  // Discover actual binary grant bindings from the already-owned primary inode.
  // Aborts the temporary source-admission reservation on every exit; neither
  // consumes the transition nor permits legacy borrowing. No pathname reopen,
  // supplied identity/profile, mounted source, page-zero or readiness receipt.
  // Full adoption MUST freshly revalidate this observation and the entire graph.
  // SEARCH_KEY: NATIVE_PROBE_ACCEPTS_PRODUCTION_LINEAGE
  // `allow_production_lineage` opts into reading a database created by the
  // production path (CreateDatabaseFile), which writes a DatabaseHeader at
  // offset 0 rather than a FilespaceBootstrap preamble. Default false keeps
  // native-lineage callers on exactly their previous behaviour: the native
  // decode is always attempted first, and this only runs where it already
  // failed.
  static NativeOwnedSourceBootstrapResult ProbeRouteBootstrap(
      const std::shared_ptr<disk::RouteSourceTransition>&,
      bool allow_production_lineage = false) noexcept;
  static NativeOwnedSourceResult AdoptWithMemory(const Uuid&,const Uuid&,
      std::vector<std::unique_ptr<disk::FileDevice>>&,
      NativeOwnedCheckpointMemoryLimits,NativeStorageMemory&,const NativeStorageMemoryBinding&,
      NativeStorageMemory&,const NativeStorageMemoryBinding&) noexcept;
  static NativeOwnedSourceResult AdoptRouteWithMemory(
      const std::shared_ptr<disk::RouteSourceTransition>&,const Uuid&,const Uuid&,
      std::vector<std::unique_ptr<disk::FileDevice>>&,
      NativeOwnedCheckpointMemoryLimits,NativeStorageMemory&,const NativeStorageMemoryBinding&,
      NativeStorageMemory&,const NativeStorageMemoryBinding&) noexcept;

  const Uuid& database_uuid() const noexcept { return database_; }
  const Uuid& primary_uuid() const noexcept { return primary_; }
  // Does not wait or close in-flight sources. Callers admitted before this
  // linearization point retain ownership until their read lease is destroyed.
  void Withdraw() noexcept { accepting_.exchange(false); }
 private:
  friend class NativeOwnedCheckpointWriter;
  static NativeOwnedSourceResult AdoptRouteWithMemoryImpl(
      const std::shared_ptr<disk::RouteSourceTransition>&,const Uuid&,const Uuid&,
      std::vector<std::unique_ptr<disk::FileDevice>>&,NativeOwnedCheckpointMemoryLimits,
      NativeStorageMemory&,const NativeStorageMemoryBinding&,
      NativeStorageMemory&,const NativeStorageMemoryBinding&,disk::RouteSourceAdmissionKind) noexcept;

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
    auto read=batch.ReadAtBounded(0,bytes.data(),bytes.size());
    out.physical_bytes_read+=read.bytes_transferred;
    out.io_status=disk::BoundedIoStatus(read);
    if(!read.ok())out.io_failure=read;
    if(!read.ok()||read.bytes_transferred!=bytes.size()){
      // Retain the typed physical receipt, not only the status projection. The
      // status says a read failed; the rendered diagnostic says which failure it
      // was, and a caller that loses it cannot report the actual cause.
      out.io_diagnostic=disk::RenderBoundedIoResult(read,file.path()).diagnostic;
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
  // Every filespace the directory records is one this source owns.
  static bool CompleteFixedRootCohort(const NativeFixedRootMemoryLease& source,std::size_t count) noexcept {
    return source.directory().pages.front().directory.total_records==count;
  }
  std::unique_ptr<NativeOwnedCheckpointMemoryState> memory_state_;

  NativeOwnedCheckpointSource()=default;
  Uuid database_,primary_;
  u64 process_=0;
  std::atomic<bool> accepting_{true};
};

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
    // SB_NATIVE_FIXED_ROOT_READ (drift removal C3d): the owned files are
    // validated through page zero's fixed roots, not a selected checkpoint.
    auto fixed=AcquireNativeFixedRootMemoryLease(database,state.devices,primary,
        {limits.source.maximum_verification_image_bytes,limits.source.maximum_backing_bytes},transient,transient_binding);
    out.FromFixedRootSource(fixed);
    if(!fixed.ok())return out;
    if(!CompleteFixedRootCohort(*fixed.lease,state.devices.size())){out.error=E::incomplete_cohort;return out;}
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
inline NativeOwnedSourceBootstrapResult NativeOwnedCheckpointSource::ProbeRouteBootstrap(
    const std::shared_ptr<disk::RouteSourceTransition>& transition,
    bool allow_production_lineage) noexcept {
  using E=NativeOwnedSourceError;
  NativeOwnedSourceBootstrapResult out;
  try {
    if(!transition)return out;
    auto retained=transition;
    std::unique_ptr<disk::FileDevice> primary;
    out.route_error=retained->BeginSourceAdmission(primary);
    if(out.route_error!=disk::RouteSourceTransitionError::none) {
      out.error=E::route_admission;return out;
    }
    const auto abort=[&primary](disk::RouteSourceTransition* t) {
      primary.reset();t->AbortSourceAdmission();
    };
    std::unique_ptr<disk::RouteSourceTransition,decltype(abort)> rollback(retained.get(),abort);
    const auto observed=disk::ReadFilespaceBootstrapBoundedFromOpenDevice(*primary);
    out.io=observed.io;
    if(!observed.io.ok())out.io_failure=observed.io;
    out.physical_bytes_read=observed.io.bytes_transferred;
    out.io_status=disk::BoundedIoStatus(observed.io);
    out.bootstrap_error=observed.error;
    if(!observed.ok()){
      // SEARCH_KEY: NATIVE_PROBE_ACCEPTS_PRODUCTION_LINEAGE
      // The native preamble is absent. A production-lineage database carries a
      // DatabaseHeader at offset 0 instead, with its first filespace identity
      // in the fixed startup map. Assemble the equivalent preamble from those
      // two, rather than requiring the file to have been created natively.
      // Only the identities this probe actually supplies downstream are
      // synthesized -- database_uuid and filespace_uuid -- plus the page-size
      // profile they are validated against. Nothing here grants adoption: the
      // caller still revalidates the whole graph.
      if(!allow_production_lineage){out.error=E::bootstrap_failure;return out;}
      disk::SerializedDatabaseHeader header_bytes{};
      const auto header_io=primary->ReadAt(0,header_bytes.data(),header_bytes.size());
      if(!header_io.ok()||header_io.bytes_transferred!=header_bytes.size()){
        out.error=E::bootstrap_failure;return out;
      }
      const auto parsed=disk::ParseDatabaseHeader(header_bytes);
      if(!parsed.ok()){out.error=E::bootstrap_failure;return out;}
      const auto page_size=parsed.header.page_size;
      const auto* profile=disk::FindCanonicalFilespacePageProfileForSize(page_size);
      if(profile==nullptr){out.error=E::bootstrap_failure;return out;}
      auto startup=ReadStartupStatePageBody(primary.get(),page_size);
      if(!startup.status.ok()||startup.state.first_filespace_uuid.value.is_nil()){
        out.error=E::bootstrap_failure;return out;
      }
      disk::FilespaceBootstrap synthesized;
      synthesized.database_uuid=parsed.header.database_uuid;
      synthesized.filespace_uuid=startup.state.first_filespace_uuid.value;
      synthesized.page_size_profile_uuid=profile->uuid;
      synthesized.page_size_bytes=page_size;
      // role/lifecycle_state/checksum+encryption profiles are deliberately left
      // at their defaults: this probe's consumers read only the two identities
      // and the page-size profile, and inventing registry codes the production
      // header does not carry would be fabricating durable facts.
      out.bootstrap=synthesized;
      out.error=E::none;
      return out;
    }
    out.bootstrap=observed.preamble;
    out.error=E::none;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
   catch(...){out.error=E::io_failure;}
  if(out.error!=E::none)out.bootstrap.reset();
  return out;
}

inline NativeOwnedSourceResult NativeOwnedCheckpointSource::AdoptRouteWithMemory(
    const std::shared_ptr<disk::RouteSourceTransition>& transition,const Uuid& database,const Uuid& primary,
    std::vector<std::unique_ptr<disk::FileDevice>>& secondary,NativeOwnedCheckpointMemoryLimits limits,
    NativeStorageMemory& persistent,const NativeStorageMemoryBinding& persistent_binding,
    NativeStorageMemory& transient,const NativeStorageMemoryBinding& transient_binding) noexcept {
  return AdoptRouteWithMemoryImpl(transition,database,primary,secondary,limits,persistent,persistent_binding,
      transient,transient_binding,disk::RouteSourceAdmissionKind::read_only);
}
inline NativeOwnedSourceResult NativeOwnedCheckpointSource::AdoptRouteWithMemoryImpl(
    const std::shared_ptr<disk::RouteSourceTransition>& transition,const Uuid& database,const Uuid& primary,
    std::vector<std::unique_ptr<disk::FileDevice>>& secondary,NativeOwnedCheckpointMemoryLimits limits,
    NativeStorageMemory& persistent,const NativeStorageMemoryBinding& persistent_binding,
    NativeStorageMemory& transient,const NativeStorageMemoryBinding& transient_binding,
    disk::RouteSourceAdmissionKind kind) noexcept {
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
    out.route_error=retained->BeginSourceAdmission(primary_device,kind);
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
    // SB_NATIVE_FIXED_ROOT_READ (drift removal C3d): the owned files are
    // validated through page zero's fixed roots, not a selected checkpoint.
    auto fixed=AcquireNativeFixedRootMemoryLease(database,state.devices,primary,
        {limits.source.maximum_verification_image_bytes,limits.source.maximum_backing_bytes},transient,transient_binding);
    out.FromFixedRootSource(fixed);
    if(!fixed.ok())return out;
    if(!CompleteFixedRootCohort(*fixed.lease,state.devices.size())){out.error=E::incomplete_cohort;return out;}
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
