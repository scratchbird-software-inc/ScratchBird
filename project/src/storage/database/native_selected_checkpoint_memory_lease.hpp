// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_selected_checkpoint_readonly.hpp"
#include "native_bound_checkpoint_selection_backing.hpp"
#include "native_current_checkpoint_source_backing.hpp"
#include "native_storage_memory.hpp"
#include "uuid.hpp"
#include <thread>
#include <mutex>
#include <system_error>

namespace scratchbird::storage::database {
struct NativeSelectedCheckpointMemoryLimits {
  u64 maximum_verification_image_bytes=0;
  std::size_t maximum_backing_bytes=0;
};
enum class NativeSelectedCheckpointMemoryError {
  none,invalid_request,invalid_device,memory_binding_failure,memory_allocation_failure,
  selection_failure,directory_failure,resource_exhausted,lock_failure,io_failure
};
struct NativeSelectedCheckpointMemoryResult;
class NativeSelectedCheckpointMemoryHandle;
// Thread-affine, borrowed-device inspection fence. No mutable nested spans and
// no operation/serving/security authority. The issuing manager and devices must
// outlive the handle. Do not recursively publish, close or rebind those devices.
class NativeSelectedCheckpointMemoryLease {
 public:
  NativeSelectedCheckpointMemoryLease(const NativeSelectedCheckpointMemoryLease&)=delete;
  NativeSelectedCheckpointMemoryLease& operator=(const NativeSelectedCheckpointMemoryLease&)=delete;
  const disk::FilespaceRootReference& checkpoint() const noexcept {CheckThread();return checkpoint_;}
  const NativeReadBoundCheckpointSelection& selection() const noexcept {CheckThread();return selection_;}
  const NativeReadCheckpointDirectory& directory() const noexcept {CheckThread();return directory_;}
  u64 retained_image_bytes() const noexcept {CheckThread();return retained_image_bytes_;}
  std::size_t backing_bytes_used() const noexcept {CheckThread();return backing_bytes_used_;}
 private:
  using Batch=disk::FileDevice::ReadLatencyBatch;
  using Guard=std::unique_lock<std::recursive_mutex>;
  NativeSelectedCheckpointMemoryLease() noexcept:thread_(std::this_thread::get_id()){}
  ~NativeSelectedCheckpointMemoryLease(){
    CheckThread();
    // Every source fence is released before the first observation callback.
    while(guard_count_)std::destroy_at(guards_+--guard_count_);
    while(batch_count_)std::destroy_at(batches_+--batch_count_);
  }
  void CheckThread() const noexcept {if(thread_!=std::this_thread::get_id())std::terminate();}
  std::thread::id thread_;
  Guard* guards_=nullptr;std::size_t guard_count_=0;
  Batch* batches_=nullptr;std::size_t batch_count_=0;
  std::span<const disk::NativeFilespaceDevice> devices_;
  disk::FilespaceRootReference checkpoint_;
  NativeReadBoundCheckpointSelection selection_;
  NativeReadCheckpointDirectory directory_;
  u64 retained_image_bytes_=0;
  std::size_t backing_bytes_used_=0;
  friend class NativeSelectedCheckpointMemoryHandle;
  friend class NativeCatalogLeafLeaseReader;
  friend class NativeCatalogRootsLeaseReader;
  friend class NativeBtreeTreeLeaseReader;
  friend class NativeAllocationLeaseReader;
  friend NativeSelectedCheckpointMemoryResult AcquireNativeSelectedCheckpointMemoryLease(
      const Uuid&,std::span<const disk::NativeFilespaceDevice>,const Uuid&,
      NativeSelectedCheckpointMemoryLimits,NativeStorageMemory&,const NativeStorageMemoryBinding&) noexcept;
};
// Backing cannot be separated from the privately constructed lease. A moved
// handle remains on its acquiring thread; cross-thread use is a contract error.
class NativeSelectedCheckpointMemoryHandle {
 public:
  NativeSelectedCheckpointMemoryHandle() noexcept=default;
  NativeSelectedCheckpointMemoryHandle(const NativeSelectedCheckpointMemoryHandle&)=delete;
  NativeSelectedCheckpointMemoryHandle& operator=(const NativeSelectedCheckpointMemoryHandle&)=delete;
  NativeSelectedCheckpointMemoryHandle(NativeSelectedCheckpointMemoryHandle&& other) noexcept {
    other.CheckThread();arena_=std::move(other.arena_);lease_=std::exchange(other.lease_,nullptr);
  }
  NativeSelectedCheckpointMemoryHandle& operator=(NativeSelectedCheckpointMemoryHandle&& other) noexcept {
    if(this!=&other){other.CheckThread();Reset();arena_=std::move(other.arena_);lease_=std::exchange(other.lease_,nullptr);}
    return *this;
  }
  ~NativeSelectedCheckpointMemoryHandle(){Reset();}
  explicit operator bool() const noexcept {CheckThread();return lease_!=nullptr;}
  const NativeSelectedCheckpointMemoryLease* operator->() const noexcept {CheckThread();return lease_;}
  const NativeSelectedCheckpointMemoryLease& operator*() const noexcept {CheckThread();return *lease_;}
  void Reset() noexcept {
    CheckThread();
    if(lease_){lease_->~NativeSelectedCheckpointMemoryLease();lease_=nullptr;}
    arena_=NativeStorageArena{};
  }
 private:
  void CheckThread() const noexcept {if(lease_)lease_->CheckThread();}
  NativeStorageArena arena_;
  NativeSelectedCheckpointMemoryLease* lease_=nullptr;
  friend NativeSelectedCheckpointMemoryResult AcquireNativeSelectedCheckpointMemoryLease(
      const Uuid&,std::span<const disk::NativeFilespaceDevice>,const Uuid&,
      NativeSelectedCheckpointMemoryLimits,NativeStorageMemory&,const NativeStorageMemoryBinding&) noexcept;
};
struct NativeSelectedCheckpointMemoryResult {
  NativeSelectedCheckpointMemoryError error=NativeSelectedCheckpointMemoryError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  NativeCheckpointSelectionError selection_error=NativeCheckpointSelectionError::none;
  NativeCheckpointError checkpoint_error=NativeCheckpointError::none;
  page::NativeInventoryError inventory_error=page::NativeInventoryError::none;
  page::NativeAllocationError allocation_error=page::NativeAllocationError::none;
  page::NativeDirectoryError directory_error=page::NativeDirectoryError::none;
  core::platform::Status allocation_status,io_status;
  core::platform::DiagnosticRecord allocation_diagnostic,io_diagnostic;
  u64 physical_bytes_read=0;
  NativeSelectedCheckpointMemoryHandle lease;
  bool ok() const noexcept{return error==NativeSelectedCheckpointMemoryError::none&&bool(lease);}
};
inline NativeSelectedCheckpointMemoryResult AcquireNativeSelectedCheckpointMemoryLease(
    const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,const Uuid& primary,
    NativeSelectedCheckpointMemoryLimits limits,NativeStorageMemory& memory,
    const NativeStorageMemoryBinding& binding) noexcept {
  using E=NativeSelectedCheckpointMemoryError;
  using L=NativeSelectedCheckpointMemoryLease;
  NativeSelectedCheckpointMemoryResult out;
  try{
    out.memory_error=memory.CheckBinding(binding);
    if(out.memory_error==NativeStorageMemoryError::none&&binding.database_uuid!=database)
      out.memory_error=NativeStorageMemoryError::invalid_binding;
    if(out.memory_error!=NativeStorageMemoryError::none){out.error=E::memory_binding_failure;return out;}
    if(!core::uuid::IsEngineIdentityUuid(database)||!core::uuid::IsEngineIdentityUuid(primary)||
        files.empty()||!limits.maximum_verification_image_bytes||!limits.maximum_backing_bytes)return out;
    bool found=false;
    for(std::size_t i=0;i<files.size();++i){
      const auto& f=files[i];
      if(!f.device||!core::uuid::IsEngineIdentityUuid(f.filespace_uuid)||
          !disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid)){
        out.error=E::invalid_device;return out;
      }
      found|=f.filespace_uuid==primary;
      for(std::size_t j=0;j<i;++j)if(files[j].device==f.device||files[j].filespace_uuid==f.filespace_uuid){
        out.error=E::invalid_device;return out;
      }
    }
    if(!found){out.error=E::invalid_device;return out;}
    auto payload=memory.CreateArena(binding,limits.maximum_backing_bytes,alignof(std::max_align_t));
    out.memory_error=payload.error;out.allocation_status=payload.backing.status;
    out.allocation_diagnostic=std::move(payload.backing.diagnostic);
    if(!payload.ok()){out.error=E::memory_allocation_failure;return out;}
    auto block=payload.arena.Allocate(limits.maximum_backing_bytes,alignof(std::max_align_t));
    if(!block.ok()){out.error=E::resource_exhausted;return out;}
    detail::NativeMetadataMemory resource({static_cast<byte*>(block.pointer),block.bytes});
    detail::NativeMetadataScratch scratch{&resource};
    NativeSelectedCheckpointMemoryHandle handle;handle.arena_=std::move(payload.arena);
    handle.lease_=new(resource.allocate(sizeof(L),alignof(L))) L;
    auto& lease=*handle.lease_;
    auto devices=scratch.Copy<disk::NativeFilespaceDevice>(files);
    std::sort(devices.begin(),devices.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    lease.devices_=devices;
    auto pointers=scratch.Array<L::Batch*>(devices.size());
    if(devices.size()>std::numeric_limits<std::size_t>::max()/sizeof(L::Batch)||
        devices.size()>std::numeric_limits<std::size_t>::max()/sizeof(L::Guard))throw std::bad_alloc();
    lease.batches_=static_cast<L::Batch*>(resource.allocate(devices.size()*sizeof(L::Batch),alignof(L::Batch)));
    lease.guards_=static_cast<L::Guard*>(resource.allocate(devices.size()*sizeof(L::Guard),alignof(L::Guard)));
    for(std::size_t i=0;i<devices.size();++i){
      pointers[i]=std::construct_at(lease.batches_+i,*devices[i].device);++lease.batch_count_;
    }
    // All backing, descriptors and observation objects now exist. No governor
    // operation is permitted until the final retained guard has been released.
    for(const auto& device:devices){
      std::construct_at(lease.guards_+lease.guard_count_,device.device->AcquireOperationGuard());
      ++lease.guard_count_;
    }
    auto selected=detail::ReadNativeBoundCheckpointSelectionBacked(database,devices,primary,
        limits.maximum_verification_image_bytes,pointers,resource);
    out.io_status=selected.io_status;out.io_diagnostic=std::move(selected.io_diagnostic);
    out.physical_bytes_read=selected.physical_bytes_read;
    out.selection_error=selected.selection.error;out.checkpoint_error=selected.selection.checkpoint_error;
    out.inventory_error=selected.selection.checkpoint_inventory.inventory_error;
    out.allocation_error=selected.selection.allocation_error;
    if(!selected.ok()){out.error=E::selection_failure;return out;}
    const auto consumed=selected.selection.retained_image_bytes;
    if(consumed>=limits.maximum_verification_image_bytes){out.error=E::resource_exhausted;return out;}
    const auto& s=*selected.selection.selection;const auto& r=s.checkpoint;
    lease.checkpoint_={9,0x300,r.filespace_uuid,r.page_number,r.page_generation,r.page_size_profile_uuid,s.checkpoint_object_uuid};
    auto directory=detail::VerifyCurrentNativeCheckpointDirectoryBacked(database,devices,lease.checkpoint_,
        limits.maximum_verification_image_bytes-consumed,pointers,resource);
    out.io_status=directory.io_status;out.io_diagnostic=std::move(directory.io_diagnostic);
    out.physical_bytes_read+=directory.physical_bytes_read;
    out.checkpoint_error=directory.directory.error;out.directory_error=directory.directory.directory_error;
    out.inventory_error=directory.directory.checkpoint_inventory.inventory_error;
    if(!directory.ok()){out.error=E::directory_failure;return out;}
    if(directory.directory.retained_image_bytes>limits.maximum_verification_image_bytes-consumed){
      out.error=E::resource_exhausted;return out;
    }
    lease.selection_=detail::ReadOnly(selected.selection,scratch);
    lease.directory_=detail::ReadOnly(directory.directory,scratch);
    lease.retained_image_bytes_=consumed+directory.directory.retained_image_bytes;
    lease.backing_bytes_used_=resource.used();
    out.lease=std::move(handle);out.error=E::none;return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
   catch(...){out.error=E::io_failure;}
  return out;
}
} // namespace scratchbird::storage::database
