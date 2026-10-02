// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_selected_checkpoint_read_lease.hpp"
#include <atomic>

namespace scratchbird::storage::database {
class NativeOwnedCheckpointSource;
enum class NativeOwnedSourceError {
  none, invalid_request, device_ownership, bootstrap_failure, source_failure,
  resource_exhausted, lock_failure, withdrawn, wrong_process, io_failure
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

// Owns a native source, not serving readiness or configuration activation.
// Admission consumes exclusively owned open devices only on success. No paths
// are reopened, no supplied UUID/profile replaces the actual bootstrap bytes.
// Route-borrowed devices require their owning runtime integration; this API
// must never detach that route's thread-affine serialization guard.
class NativeOwnedCheckpointSource {
 public:
  NativeOwnedCheckpointSource(const NativeOwnedCheckpointSource&)=delete;
  NativeOwnedCheckpointSource& operator=(const NativeOwnedCheckpointSource&)=delete;
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
      std::unique_ptr<NativeOwnedCheckpointReadLease> lease(new NativeOwnedCheckpointReadLease);
      lease->owner_=owner;
      auto selected=AcquireNativeSelectedCheckpointReadLease(
          owner->database_,owner->devices_,owner->primary_,budget);
      if(!selected.ok()){out.FromSource(selected);return out;}
      lease->source_=std::move(selected.lease);
      out.lease=std::move(lease);out.error=E::none;return out;
    }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
     catch(const std::length_error&){out.error=E::resource_exhausted;}
     catch(const std::system_error&){out.error=E::lock_failure;}
     catch(...){out.error=E::io_failure;}
    return out;
  }
 private:
  friend class NativeOwnedCheckpointReadLease;
  NativeOwnedCheckpointSource()=default;
  std::vector<std::unique_ptr<disk::FileDevice>> owned_;
  std::vector<disk::NativeFilespaceDevice> devices_;
  Uuid database_,primary_;
  u64 process_=0;
  std::atomic<bool> accepting_{true};
};
inline const std::vector<disk::NativeFilespaceDevice>&
NativeOwnedCheckpointReadLease::devices() const noexcept { return owner_->devices_; }
} // namespace scratchbird::storage::database
