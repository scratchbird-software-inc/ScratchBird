// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_checkpoint_selection.hpp"
#include "disk_device.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>

namespace scratchbird::storage::database {
enum class NativeSelectedCheckpointReadError {
  none, invalid_request, invalid_device, selection_failure, directory_failure,
  resource_exhausted, lock_failure, io_failure
};
struct NativeSelectedCheckpointReadResult;
// A thread-affine read fence, not a mounted owner, snapshot or serving grant.
// The owning caller retains all borrowed devices. Never recursively publish,
// close or rebind a device while using this lease, or transfer it to a thread.
class NativeSelectedCheckpointReadLease {
 public:
  NativeSelectedCheckpointReadLease(const NativeSelectedCheckpointReadLease&)=delete;
  NativeSelectedCheckpointReadLease& operator=(const NativeSelectedCheckpointReadLease&)=delete;
  NativeSelectedCheckpointReadLease(NativeSelectedCheckpointReadLease&&)=delete;
  NativeSelectedCheckpointReadLease& operator=(NativeSelectedCheckpointReadLease&&)=delete;
  const disk::FilespaceRootReference& checkpoint() const noexcept {return checkpoint_;}
  const NativeBoundCheckpointSelection& selection() const noexcept {return selection_;}
  const NativeCheckpointDirectoryResult& directory() const noexcept {return directory_;}
  u64 retained_image_bytes() const noexcept {return retained_image_bytes_;}
 private:
  NativeSelectedCheckpointReadLease()=default;
  // Destroy source images before releasing the source's guards.
  std::vector<std::unique_lock<std::recursive_mutex>> guards_;
  disk::FilespaceRootReference checkpoint_;
  NativeBoundCheckpointSelection selection_;
  NativeCheckpointDirectoryResult directory_;
  u64 retained_image_bytes_=0;
  friend NativeSelectedCheckpointReadResult AcquireNativeSelectedCheckpointReadLease(
      const Uuid&,const std::vector<disk::NativeFilespaceDevice>&,
      const Uuid&,u64) noexcept;
};
struct NativeSelectedCheckpointReadResult {
  NativeSelectedCheckpointReadError error=NativeSelectedCheckpointReadError::invalid_request;
  NativeCheckpointSelectionError selection_error=NativeCheckpointSelectionError::none;
  NativeCheckpointError checkpoint_error=NativeCheckpointError::none;
  page::NativeAllocationError allocation_error=page::NativeAllocationError::none;
  page::NativeDirectoryError directory_error=page::NativeDirectoryError::none;
  page::NativeInventoryError inventory_error=page::NativeInventoryError::none;
  std::unique_ptr<NativeSelectedCheckpointReadLease> lease;
  bool ok() const noexcept {
    return error==NativeSelectedCheckpointReadError::none&&lease!=nullptr;
  }
};
// STORAGE-NATIVE-SELECTED-CHECKPOINT-READ-FENCE-001. No caller checkpoint:
// paired selectors and the actual committed directory select the source.
// The ceiling bounds retained images, not a memory-governor grant.
inline NativeSelectedCheckpointReadResult AcquireNativeSelectedCheckpointReadLease(
    const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& supplied,
    const Uuid& primary,u64 budget) noexcept {
  using E=NativeSelectedCheckpointReadError;
  NativeSelectedCheckpointReadResult out;
  try {
    if(!core::uuid::IsEngineIdentityUuid(database)||
       !core::uuid::IsEngineIdentityUuid(primary)||supplied.empty()||!budget)return out;
    auto devices=supplied;
    std::sort(devices.begin(),devices.end(),[](const auto& a,const auto& b){
      return a.filespace_uuid<b.filespace_uuid;
    });
    for(std::size_t i=0;i<devices.size();++i){
      const auto& f=devices[i];
      if(!f.device||!core::uuid::IsEngineIdentityUuid(f.filespace_uuid)||
          !disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid)||
          (i&&devices[i-1].filespace_uuid==f.filespace_uuid)){
        out.error=E::invalid_device;return out;
      }
      for(std::size_t j=0;j<i;++j)if(devices[j].device==f.device){
        out.error=E::invalid_device;return out;
      }
    }
    const auto owner=std::lower_bound(devices.begin(),devices.end(),primary,
        [](const auto& f,const auto& id){return f.filespace_uuid<id;});
    if(owner==devices.end()||owner->filespace_uuid!=primary){
      out.error=E::invalid_device;return out;
    }
    std::unique_ptr<NativeSelectedCheckpointReadLease> lease(new NativeSelectedCheckpointReadLease);
    lease->guards_.reserve(devices.size());
    for(const auto& f:devices)lease->guards_.push_back(f.device->AcquireOperationGuard());
    lease->selection_=ReadNativeBoundCheckpointSelectionFromOpenDevices(database,devices,primary,budget);
    if(!lease->selection_.ok()){
      out.error=E::selection_failure;out.selection_error=lease->selection_.error;
      out.checkpoint_error=lease->selection_.checkpoint_error;
      out.allocation_error=lease->selection_.allocation_error;return out;
    }
    const auto consumed=lease->selection_.retained_image_bytes;
    if(consumed>=budget){out.error=E::resource_exhausted;return out;}
    const auto& s=*lease->selection_.selection;
    const auto& r=s.checkpoint;
    lease->checkpoint_={9,0x300,r.filespace_uuid,r.page_number,r.page_generation,
        r.page_size_profile_uuid,s.checkpoint_object_uuid};
    lease->directory_=VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(
        database,devices,lease->checkpoint_,budget-consumed);
    if(!lease->directory_.ok()){
      out.error=E::directory_failure;out.checkpoint_error=lease->directory_.error;
      out.directory_error=lease->directory_.directory_error;
      out.inventory_error=lease->directory_.checkpoint_inventory.inventory_error;return out;
    }
    if(lease->directory_.retained_image_bytes>budget-consumed){
      out.error=E::resource_exhausted;return out;
    }
    lease->retained_image_bytes_=consumed+lease->directory_.retained_image_bytes;
    out.lease=std::move(lease);out.error=E::none;return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
   catch(...){out.error=E::io_failure;}
  return out;
}
} // namespace scratchbird::storage::database

