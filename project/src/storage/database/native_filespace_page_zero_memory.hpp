// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "filespace_page_zero.hpp"
#include "native_storage_memory.hpp"
#include "disk_device.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <system_error>
#include <type_traits>

namespace scratchbird::storage::database {
enum class NativeFilespacePageZeroMemoryError {
  none, invalid_request, memory_binding_failure, memory_allocation_failure,
  bootstrap_failure, page_zero_failure, io_failure, resource_exhausted, lock_failure
};
inline core::platform::usize NativeFilespacePageZeroWorkspaceBytes(const core::platform::Uuid& profile) noexcept {
  const auto* p=disk::FindCanonicalFilespacePageProfile(profile);
  return p?p->page_size_bytes+32*sizeof(disk::FilespaceRootReference)+(alignof(std::max_align_t)-1):0;
}
struct NativeFilespacePageZeroMemoryResult {
  NativeFilespacePageZeroMemoryError error=NativeFilespacePageZeroMemoryError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  disk::FilespaceBootstrapError bootstrap_error=disk::FilespaceBootstrapError::none;
  disk::FilespacePageZeroError page_zero_error=disk::FilespacePageZeroError::none;
  core::platform::Status allocation_status,io_status;
  core::platform::DiagnosticRecord allocation_diagnostic;
  // Fixed backend cause/effects; render only after all source fences release.
  disk::BoundedIoResult io_receipt;
  core::platform::u64 bootstrap_bytes_read=0,page_bytes_read=0;
  std::optional<core::platform::u64> size_before,size_after;
  NativeStorageArena arena;
  std::optional<disk::FilespacePageZeroView> record;
  std::span<const core::platform::byte> image;
  bool ok() const noexcept {return error==NativeFilespacePageZeroMemoryError::none&&bool(arena)&&record.has_value()&&!image.empty();}
};
// Complete clear structural image and stable exact extent only. No target-root,
// current-capacity, encryption, cluster, MGA or serving authority.
// Enter without an outer device/source guard. Admission, cleanup and captured
// I/O observations surround the complete owned fence; compound owners use the
// lower-level codecs and BoundedIoBatch with their own enclosing lifetime.
inline NativeFilespacePageZeroMemoryResult ReadNativeFilespacePageZeroWithMemoryFromOpenDevice(
    disk::FileDevice& device,const disk::FilespaceBootstrapBinding& expected,
    NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding) noexcept {
  using E=NativeFilespacePageZeroMemoryError;
  using P=disk::FilespacePageZeroError;
  NativeFilespacePageZeroMemoryResult out;
  try {
    out.memory_error=memory.CheckBinding(binding);
    if(out.memory_error==NativeStorageMemoryError::none&&binding.database_uuid!=expected.database_uuid)
      out.memory_error=NativeStorageMemoryError::invalid_binding;
    if(out.memory_error!=NativeStorageMemoryError::none){out.error=E::memory_binding_failure;return out;}
    const auto* profile=disk::FindCanonicalFilespacePageProfile(expected.page_size_profile_uuid);
    if(!profile||!core::uuid::IsEngineIdentityUuid(expected.filespace_uuid))return out;
    // Backing precedes guard acquisition and outlives guard destruction on failure.
    auto payload=memory.CreateArena(binding,NativeFilespacePageZeroWorkspaceBytes(profile->uuid),profile->page_size_bytes);
    out.memory_error=payload.error;out.allocation_status=payload.backing.status;
    out.allocation_diagnostic=std::move(payload.backing.diagnostic);
    if(!payload.ok()){out.error=E::memory_allocation_failure;return out;}
    auto image=payload.arena.Allocate(profile->page_size_bytes,profile->page_size_bytes);
    auto roots=payload.arena.Allocate(32*sizeof(disk::FilespaceRootReference),alignof(disk::FilespaceRootReference));
    if(!image.ok()||!roots.ok()){out.error=E::resource_exhausted;return out;}
    static_assert(std::is_trivially_destructible_v<disk::FilespaceRootReference>);
    auto* root_data=static_cast<disk::FilespaceRootReference*>(roots.pointer);
    std::uninitialized_value_construct_n(root_data,32);
    disk::FileDevice::BoundedIoBatch observations(device);
    const auto guard=device.AcquireOperationGuard();
    const auto record_status=[&]{
      out.io_status={out.io_receipt.ok()?core::platform::StatusCode::ok:
        core::platform::StatusCode::platform_required_feature_missing,
        out.io_receipt.ok()?core::platform::Severity::info:core::platform::Severity::error,
        core::platform::Subsystem::storage_disk};
    };
    disk::SerializedFilespaceBootstrap prefix{};
    out.io_receipt=observations.ReadAt(0,prefix.data(),prefix.size());record_status();
    out.bootstrap_bytes_read=out.io_receipt.bytes_transferred;
    if(!out.io_receipt.ok()){
      if(out.io_receipt.error==disk::BoundedIoError::not_open){
        out.error=E::bootstrap_failure;out.bootstrap_error=disk::FilespaceBootstrapError::device_not_open;
      }else out.error=E::io_failure;
      return out;
    }
    const auto bootstrap=disk::DecodeFilespaceBootstrap(prefix.data(),prefix.size(),&expected);
    if(!bootstrap.ok()){out.error=E::bootstrap_failure;out.bootstrap_error=bootstrap.error;return out;}
    out.io_receipt=observations.Size();record_status();
    if(!out.io_receipt.ok()){out.error=E::io_failure;return out;}
    const auto before=out.io_receipt.size_bytes;out.size_before=before;
    const auto size=profile->page_size_bytes;
    if(before<size||before%size){out.error=E::page_zero_failure;out.page_zero_error=P::invalid_capacity;return out;}
    out.io_receipt=observations.ReadAt(0,image.pointer,image.bytes);record_status();
    out.page_bytes_read=out.io_receipt.bytes_transferred;
    if(!out.io_receipt.ok()){out.error=E::io_failure;return out;}
    const std::span<const core::platform::byte> bytes{static_cast<const core::platform::byte*>(image.pointer),image.bytes};
    if(!std::equal(prefix.begin(),prefix.end(),bytes.begin())){
      out.error=E::page_zero_failure;out.page_zero_error=P::probe_changed;return out;
    }
    auto decoded=disk::DecodeFilespacePageZeroInto(bytes,{root_data,32},&expected);
    out.page_zero_error=decoded.error;
    if(!decoded.ok()){out.error=E::page_zero_failure;return out;}
    out.io_receipt=observations.Size();record_status();
    if(!out.io_receipt.ok()){out.error=E::io_failure;return out;}
    out.size_after=out.io_receipt.size_bytes;
    if(before!=*out.size_after||*out.size_after!=decoded.record->total_pages*size){
      out.error=E::page_zero_failure;out.page_zero_error=P::invalid_capacity;return out;
    }
    out.record=std::move(decoded.record);out.image=bytes;out.arena=std::move(payload.arena);
    out.error=E::none;return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
   catch(...){out.error=E::io_failure;}
  return out;
}
} // namespace scratchbird::storage::database
